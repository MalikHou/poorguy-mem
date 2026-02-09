#include "pgmem/core/memory_engine.h"

#include <chrono>
#include <sstream>
#include <unordered_map>

#include "pgmem/util/json.h"
#include "pgmem/util/text.h"
#include "pgmem/util/time.h"

namespace pgmem::core {
namespace {

constexpr const char* kMemNs = "mem_items";
constexpr const char* kEmbNs = "mem_embeddings";
constexpr const char* kLexNs = "mem_lexicon";
constexpr const char* kSummaryNs = "session_summaries";
constexpr const char* kOutNs = "sync_outbox";
constexpr const char* kAuditNs = "audit_log";

std::string BuildMemKey(const std::string& workspace_id, const std::string& memory_id) {
    return workspace_id + ":" + memory_id;
}

bool IsIncomingNewer(const MemoryRecord& current, const MemoryRecord& incoming) {
    if (incoming.updated_at_ms != current.updated_at_ms) {
        return incoming.updated_at_ms > current.updated_at_ms;
    }
    if (incoming.version != current.version) {
        return incoming.version > current.version;
    }
    return incoming.node_id > current.node_id;
}

}  // namespace

MemoryEngine::MemoryEngine(std::unique_ptr<store::IStoreAdapter> store,
                           std::unique_ptr<IRetriever> retriever,
                           std::unique_ptr<SyncWorker> sync_worker,
                           std::string node_id)
    : store_(std::move(store)),
      retriever_(std::move(retriever)),
      sync_worker_(std::move(sync_worker)),
      node_id_(std::move(node_id)) {
    if (sync_worker_) {
        sync_worker_->Start();
    }
}

bool MemoryEngine::Warmup(std::string* error) {
    auto entries = store_->Scan(kMemNs, "", "", 0, error);
    for (const auto& entry : entries) {
        MemoryRecord record;
        std::string decode_error;
        if (!DeserializeRecord(entry.value, &record, &decode_error)) {
            continue;
        }
        retriever_->Index(record);
    }
    return true;
}

BootstrapOutput MemoryEngine::Bootstrap(const BootstrapInput& input) {
    const auto start = std::chrono::steady_clock::now();

    BootstrapOutput out;
    const std::string summary_key = BuildSummaryKey(input.workspace_id, input.session_id);
    const auto summary_res = store_->Get(kSummaryNs, summary_key);
    if (summary_res.found) {
        out.summary = summary_res.value;
    }

    SearchInput search_input;
    search_input.workspace_id = input.workspace_id;
    search_input.query = input.task_text + "\n" + util::JoinLines(input.open_files);
    search_input.top_k = 8;
    search_input.token_budget = input.token_budget;

    bool fallback = false;
    auto hits = retriever_->Search(search_input, &fallback);

    size_t used_tokens = util::EstimateTokenCount(out.summary);
    for (const auto& hit : hits) {
        const size_t hit_tokens = util::EstimateTokenCount(hit.content);
        if (input.token_budget > 0 && (used_tokens + hit_tokens) > static_cast<size_t>(input.token_budget)) {
            break;
        }
        out.recalled_items.push_back(hit);
        used_tokens += hit_tokens;
    }

    const size_t before = util::EstimateTokenCount(search_input.query) + used_tokens;
    const size_t after = used_tokens;
    metrics_.RecordTokenReduction(before, after);
    metrics_.RecordFallback(fallback);
    out.estimated_tokens_saved = static_cast<int>(before > after ? before - after : 0);

    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    metrics_.RecordReadLatency(ms);

    return out;
}

CommitTurnOutput MemoryEngine::CommitTurn(const CommitTurnInput& input) {
    const auto start = std::chrono::steady_clock::now();

    CommitTurnOutput out;
    const uint64_t now_ms = util::NowMs();

    const std::string redacted_user = util::RedactSecrets(input.user_text);
    const std::string redacted_assistant = util::RedactSecrets(input.assistant_text);

    std::vector<std::string> redacted_code = input.code_snippets;
    for (std::string& code : redacted_code) {
        code = util::RedactSecrets(code);
    }

    std::vector<std::string> redacted_commands = input.commands;
    for (std::string& cmd : redacted_commands) {
        cmd = util::RedactSecrets(cmd);
    }

    const std::string summary_key = BuildSummaryKey(input.workspace_id, input.session_id);
    std::string existing_summary;
    const auto summary_res = store_->Get(kSummaryNs, summary_key);
    if (summary_res.found) {
        existing_summary = summary_res.value;
    }

    out.summary_updated = summary_engine_.Update(existing_summary,
                                                 redacted_user,
                                                 redacted_assistant,
                                                 redacted_code,
                                                 redacted_commands,
                                                 512);

    store_->Put(kSummaryNs, summary_key, out.summary_updated, now_ms);

    MemoryRecord record;
    record.id = NextMemoryId();
    record.workspace_id = input.workspace_id;
    record.session_id = input.session_id;
    record.source = "commit_turn";
    record.content = redacted_user + "\n" + redacted_assistant;
    if (!redacted_code.empty()) {
        record.content += "\n```\n" + redacted_code.back() + "\n```";
    }
    if (!redacted_commands.empty()) {
        record.content += "\n$ " + redacted_commands.back();
    }
    record.tags = {"turn", "auto"};
    record.created_at_ms = now_ms;
    record.updated_at_ms = now_ms;
    record.version = 1;
    record.node_id = node_id_;

    std::string persist_error;
    if (PersistRecord(record, &persist_error)) {
        retriever_->Index(record);
        out.stored_ids.push_back(record.id);

        // Keep outbox for durability and enqueue best-effort async sync.
        const uint64_t seq = local_seq_.fetch_add(1) + 1;
        SyncOp op;
        op.op_seq = seq;
        op.op_type = OpType::Upsert;
        op.workspace_id = record.workspace_id;
        op.memory_id = record.id;
        op.updated_at_ms = record.updated_at_ms;
        op.node_id = node_id_;
        op.payload_json = SerializeSyncOpPayload(record);

        const std::string outbox_key = node_id_ + ":" + std::to_string(seq);
        util::Json outbox_json;
        outbox_json.put("op_seq", op.op_seq);
        outbox_json.put("op_type", "upsert");
        outbox_json.put("workspace_id", op.workspace_id);
        outbox_json.put("memory_id", op.memory_id);
        outbox_json.put("updated_at_ms", op.updated_at_ms);
        outbox_json.put("node_id", op.node_id);
        outbox_json.put("payload_json", op.payload_json);
        store_->Put(kOutNs, outbox_key, util::ToJsonString(outbox_json, false), now_ms);

        if (sync_worker_) {
            sync_worker_->Enqueue(std::move(op));
            metrics_.SetSyncLag(sync_worker_->Lag());
        }
    }

    const size_t before_tokens = util::EstimateTokenCount(input.user_text) +
                                 util::EstimateTokenCount(input.assistant_text) +
                                 util::EstimateTokenCount(util::JoinLines(input.code_snippets)) +
                                 util::EstimateTokenCount(util::JoinLines(input.commands));
    const size_t after_tokens = util::EstimateTokenCount(record.content);
    metrics_.RecordTokenReduction(before_tokens, after_tokens);

    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    metrics_.RecordWriteLatency(ms);

    return out;
}

SearchOutput MemoryEngine::Search(const SearchInput& input) {
    const auto start = std::chrono::steady_clock::now();

    SearchOutput out;
    bool fallback = false;
    auto hits = retriever_->Search(input, &fallback);

    size_t used_tokens = 0;
    for (const auto& hit : hits) {
        const size_t hit_tokens = util::EstimateTokenCount(hit.content);
        if (input.token_budget > 0 && (used_tokens + hit_tokens) > static_cast<size_t>(input.token_budget)) {
            break;
        }
        out.hits.push_back(hit);
        used_tokens += hit_tokens;
    }

    metrics_.RecordFallback(fallback);

    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    metrics_.RecordReadLatency(ms);

    return out;
}

bool MemoryEngine::Pin(const std::string& workspace_id,
                       const std::string& memory_id,
                       bool pin,
                       std::string* error) {
    MemoryRecord record;
    if (!LoadRecord(workspace_id, memory_id, &record, error)) {
        return false;
    }

    record.pinned = pin;
    record.updated_at_ms = util::NowMs();
    ++record.version;

    if (!PersistRecord(record, error)) {
        return false;
    }

    retriever_->Index(record);

    const uint64_t seq = local_seq_.fetch_add(1) + 1;
    SyncOp op;
    op.op_seq = seq;
    op.op_type = OpType::Pin;
    op.workspace_id = workspace_id;
    op.memory_id = memory_id;
    op.updated_at_ms = record.updated_at_ms;
    op.node_id = node_id_;
    op.payload_json = SerializeSyncOpPayload(record);

    if (sync_worker_) {
        sync_worker_->Enqueue(std::move(op));
        metrics_.SetSyncLag(sync_worker_->Lag());
    }

    return true;
}

StatsSnapshot MemoryEngine::Stats(const std::string& workspace_id, const std::string& window) const {
    (void)workspace_id;
    (void)window;

    if (sync_worker_) {
        const_cast<Metrics&>(metrics_).SetSyncLag(sync_worker_->Lag());
    }
    return metrics_.Snapshot();
}

bool MemoryEngine::ApplySyncOp(const SyncOp& op, std::string* error) {
    if (op.op_type == OpType::Delete) {
        return true;
    }

    MemoryRecord incoming;
    if (!DeserializeRecord(op.payload_json, &incoming, error)) {
        return false;
    }

    MemoryRecord current;
    std::string load_error;
    const bool exists = LoadRecord(op.workspace_id, op.memory_id, &current, &load_error);
    if (!exists || IsIncomingNewer(current, incoming)) {
        if (!PersistRecord(incoming, error)) {
            return false;
        }
        retriever_->Index(incoming);
    }

    const std::string audit_key = incoming.workspace_id + ":" + incoming.id + ":" + std::to_string(incoming.version);
    util::Json audit;
    audit.put("workspace_id", incoming.workspace_id);
    audit.put("memory_id", incoming.id);
    audit.put("version", incoming.version);
    audit.put("updated_at_ms", incoming.updated_at_ms);
    audit.put("node_id", incoming.node_id);
    audit.put("payload", op.payload_json);
    store_->Put(kAuditNs, audit_key, util::ToJsonString(audit, false), util::NowMs());

    return true;
}

bool MemoryEngine::PersistRecord(const MemoryRecord& record, std::string* error) {
    const std::string mem_key = BuildMemKey(record.workspace_id, record.id);
    const auto put_mem = store_->Put(kMemNs, mem_key, SerializeRecord(record), record.updated_at_ms);
    if (!put_mem.ok) {
        if (error != nullptr) {
            *error = put_mem.error;
        }
        return false;
    }

    util::Json emb;
    emb.put("model_id", "hash-embed-v1");
    emb.put("dim", 64);
    emb.put("updated_at_ms", record.updated_at_ms);
    const auto put_emb = store_->Put(kEmbNs,
                                     mem_key,
                                     util::ToJsonString(emb, false),
                                     record.updated_at_ms);
    if (!put_emb.ok) {
        if (error != nullptr) {
            *error = put_emb.error;
        }
        return false;
    }

    const auto tokens = util::Tokenize(record.content);
    std::unordered_map<std::string, int> tf;
    for (const auto& token : tokens) {
        ++tf[token];
    }
    for (const auto& kv : tf) {
        util::Json lex;
        lex.put("tf", kv.second);
        lex.put("doc_len", tokens.size());
        lex.put("updated_at_ms", record.updated_at_ms);
        const std::string lex_key = record.workspace_id + ":" + kv.first + ":" + record.id;
        const auto put_lex = store_->Put(kLexNs, lex_key, util::ToJsonString(lex, false), record.updated_at_ms);
        if (!put_lex.ok) {
            if (error != nullptr) {
                *error = put_lex.error;
            }
            return false;
        }
    }

    return true;
}

bool MemoryEngine::LoadRecord(const std::string& workspace_id,
                              const std::string& memory_id,
                              MemoryRecord* out,
                              std::string* error) const {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "null output";
        }
        return false;
    }

    const std::string key = BuildMemKey(workspace_id, memory_id);
    const auto get_res = store_->Get(kMemNs, key);
    if (!get_res.found) {
        if (error != nullptr && !get_res.error.empty()) {
            *error = get_res.error;
        }
        return false;
    }

    return DeserializeRecord(get_res.value, out, error);
}

std::string MemoryEngine::NextMemoryId() {
    const uint64_t seq = local_seq_.fetch_add(1) + 1;
    return std::to_string(util::NowMs()) + "-" + std::to_string(seq);
}

std::string MemoryEngine::BuildSummaryKey(const std::string& workspace_id, const std::string& session_id) const {
    return workspace_id + ":" + session_id;
}

std::string MemoryEngine::SerializeRecord(const MemoryRecord& record) {
    util::Json json;
    json.put("id", record.id);
    json.put("workspace_id", record.workspace_id);
    json.put("session_id", record.session_id);
    json.put("source", record.source);
    json.put("content", record.content);
    json.put("pinned", record.pinned);
    json.put("created_at_ms", record.created_at_ms);
    json.put("updated_at_ms", record.updated_at_ms);
    json.put("version", record.version);
    json.put("node_id", record.node_id);
    json.add_child("tags", util::MakeArray(record.tags));
    return util::ToJsonString(json, false);
}

bool MemoryEngine::DeserializeRecord(const std::string& text,
                                     MemoryRecord* record,
                                     std::string* error) {
    if (record == nullptr) {
        if (error != nullptr) {
            *error = "null output record";
        }
        return false;
    }

    util::Json json;
    std::string parse_error;
    if (!util::ParseJson(text, &json, &parse_error)) {
        if (error != nullptr) {
            *error = parse_error;
        }
        return false;
    }

    record->id = util::GetStringOr(json, "id", "");
    record->workspace_id = util::GetStringOr(json, "workspace_id", "");
    record->session_id = util::GetStringOr(json, "session_id", "");
    record->source = util::GetStringOr(json, "source", "");
    record->content = util::GetStringOr(json, "content", "");
    record->pinned = util::GetBoolOr(json, "pinned", false);
    record->created_at_ms = util::GetUint64Or(json, "created_at_ms", 0);
    record->updated_at_ms = util::GetUint64Or(json, "updated_at_ms", 0);
    record->version = util::GetUint64Or(json, "version", 0);
    record->node_id = util::GetStringOr(json, "node_id", "");
    record->tags = util::ReadStringArray(json, "tags");

    return true;
}

std::string MemoryEngine::SerializeSyncOpPayload(const MemoryRecord& record) {
    return SerializeRecord(record);
}

}  // namespace pgmem::core
