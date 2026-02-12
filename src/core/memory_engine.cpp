#include "pgmem/core/memory_engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pgmem/util/json.h"
#include "pgmem/util/text.h"
#include "pgmem/util/time.h"

namespace pgmem::core {
namespace {

constexpr const char* kMemNs            = "mem_items";
constexpr const char* kEmbNs            = "mem_embeddings";
constexpr const char* kLexNs            = "mem_lexicon";
constexpr const char* kSummaryNs        = "session_summaries";
constexpr const char* kEventsNs         = "ds_events";
constexpr const char* kProjectionCkptNs = "ds_projection_ckpt";

std::string BuildMemKey(const std::string& workspace_id, const std::string& memory_id) {
    return workspace_id + ":" + memory_id;
}

uint64_t BytesFromMb(uint64_t mb) { return mb * 1024ull * 1024ull; }

uint64_t BytesFromGb(uint64_t gb) { return gb * 1024ull * 1024ull * 1024ull; }

std::string ZeroPadUint64(uint64_t v) {
    std::ostringstream oss;
    oss << std::setw(20) << std::setfill('0') << v;
    return oss.str();
}

std::string OpTypeToString(OpType type) {
    switch (type) {
        case OpType::Pin:
            return "pin";
        case OpType::Delete:
            return "delete";
        case OpType::Upsert:
        default:
            return "upsert";
    }
}

std::string WorkspaceFromEventKey(const std::string& event_key) {
    const size_t pos = event_key.find(':');
    if (pos == std::string::npos) {
        return {};
    }
    return event_key.substr(0, pos);
}

bool IsMissingScanNamespaceError(const std::string& error) {
    if (error.empty()) {
        return false;
    }
    std::string lowered = error;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered.find("resource not found") != std::string::npos;
}

double GetDoubleOr(const util::Json& json, const std::string& path, double fallback) {
    try {
        return json.get<double>(path);
    } catch (const std::exception&) {
        return fallback;
    }
}

uint64_t ComputeRecordSizeBytes(const MemoryRecord& record) {
    size_t total = 0;
    total += record.id.size();
    total += record.workspace_id.size();
    total += record.session_id.size();
    total += record.source.size();
    total += record.content.size();
    total += record.node_id.size();
    total += record.tier.size();
    for (const auto& tag : record.tags) {
        total += tag.size();
    }
    total += 256;
    return static_cast<uint64_t>(total);
}

double EvictionRank(const MemoryRecord& rec, uint64_t now_ms) {
    if (rec.pinned) {
        return 1e12;
    }
    const uint64_t reference_ms = std::max(rec.updated_at_ms, rec.last_access_ms);
    const double age_hours      = static_cast<double>(now_ms > reference_ms ? (now_ms - reference_ms) : 0) / 3600000.0;
    const double hit_signal     = std::log1p(static_cast<double>(rec.hit_count));
    const double source_weight  = (rec.source == "commit_turn") ? 2.0 : 0.5;
    return rec.importance_score * 10.0 + hit_signal * 4.0 + source_weight - age_hours * 1.5;
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

WriteAckMode ParseWriteAckMode(const std::string& value) {
    std::string lowered = value;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowered == "accepted") {
        return WriteAckMode::Accepted;
    }
    return WriteAckMode::Durable;
}

const char* WriteAckModeToString(WriteAckMode mode) {
    switch (mode) {
        case WriteAckMode::Accepted:
            return "accepted";
        case WriteAckMode::Durable:
        default:
            return "durable";
    }
}

MemoryEngine::MemoryEngine(std::unique_ptr<store::IStoreAdapter> store, std::unique_ptr<IRetriever> retriever,
                           std::string node_id, MemoryEngineOptions options)
    : store_(std::move(store)),
      retriever_(std::move(retriever)),
      options_(std::move(options)),
      node_id_(std::move(node_id)) {
    if (options_.effective_backend.empty()) {
        options_.effective_backend = "unknown";
    }
    if (options_.volatile_flush_interval_ms == 0) {
        options_.volatile_flush_interval_ms = 1;
    }
    if (options_.volatile_max_pending_ops == 0) {
        options_.volatile_max_pending_ops = 1;
    }
    if (options_.shutdown_drain_timeout_ms == 0) {
        options_.shutdown_drain_timeout_ms = 1;
    }

    if (options_.write_ack_mode == WriteAckMode::Accepted) {
        volatile_writer_stop_.store(false, std::memory_order_release);
        volatile_writer_running_.store(true, std::memory_order_release);
        volatile_writer_thread_ = std::thread(&MemoryEngine::RunVolatileWriter, this);
    }

    if (options_.enable_tombstone_gc) {
        gc_running_.store(true, std::memory_order_release);
        gc_thread_ = std::thread(&MemoryEngine::RunGcLoop, this);
    }
}

MemoryEngine::~MemoryEngine() { Shutdown(); }

void MemoryEngine::Shutdown() {
    std::call_once(shutdown_once_, [&]() {
        gc_stop_.store(true, std::memory_order_release);
        gc_cv_.notify_all();
        if (gc_thread_.joinable()) {
            gc_thread_.join();
        }

        StopVolatileWriter();
    });
}

void MemoryEngine::StopVolatileWriter() {
    if (!volatile_writer_running_.load(std::memory_order_acquire)) {
        return;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.shutdown_drain_timeout_ms);
    while (pending_write_ops_.load(std::memory_order_acquire) > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    volatile_writer_stop_.store(true, std::memory_order_release);
    volatile_cv_.notify_all();
    if (volatile_writer_thread_.joinable()) {
        volatile_writer_thread_.join();
    }
    volatile_writer_running_.store(false, std::memory_order_release);

    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(volatile_mu_);
        for (const auto& batch : volatile_queue_) {
            dropped += static_cast<uint64_t>(batch.writes.size());
        }
        volatile_queue_.clear();
    }
    if (dropped > 0) {
        volatile_dropped_on_shutdown_.fetch_add(dropped, std::memory_order_acq_rel);
        pending_write_ops_.fetch_sub(dropped, std::memory_order_acq_rel);
    }
}

bool MemoryEngine::PersistBatchNow(const std::vector<WriteEntry>& writes, std::string* error) {
    const auto res = store_->BatchWrite(writes);
    if (!res.ok) {
        if (error != nullptr) {
            *error = res.error;
        }
        return false;
    }
    return true;
}

bool MemoryEngine::EnqueueVolatileBatch(std::vector<WriteEntry> writes) {
    if (writes.empty()) {
        return true;
    }

    const uint64_t write_count = static_cast<uint64_t>(writes.size());
    {
        std::lock_guard<std::mutex> lock(volatile_mu_);
        const uint64_t current_pending = pending_write_ops_.load(std::memory_order_acquire);
        if ((current_pending + write_count) > options_.volatile_max_pending_ops) {
            return false;
        }
        volatile_queue_.push_back(VolatileWriteBatch{std::move(writes)});
        pending_write_ops_.fetch_add(write_count, std::memory_order_acq_rel);
    }
    volatile_cv_.notify_one();
    return true;
}

bool MemoryEngine::EnqueueOrPersistBatch(std::vector<WriteEntry> writes, std::string* error) {
    if (options_.write_ack_mode == WriteAckMode::Durable) {
        return PersistBatchNow(writes, error);
    }
    if (EnqueueVolatileBatch(writes)) {
        return true;
    }
    return PersistBatchNow(writes, error);
}

bool MemoryEngine::TryPopVolatileBatch(VolatileWriteBatch* out_batch) {
    if (out_batch == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(volatile_mu_);
    if (volatile_queue_.empty()) {
        return false;
    }
    *out_batch = std::move(volatile_queue_.front());
    volatile_queue_.pop_front();
    pending_write_ops_.fetch_sub(static_cast<uint64_t>(out_batch->writes.size()), std::memory_order_acq_rel);
    return true;
}

bool MemoryEngine::RequeueVolatileFront(VolatileWriteBatch* batch) {
    if (batch == nullptr || batch->writes.empty()) {
        return false;
    }
    const uint64_t write_count = static_cast<uint64_t>(batch->writes.size());
    {
        std::lock_guard<std::mutex> lock(volatile_mu_);
        volatile_queue_.push_front(std::move(*batch));
    }
    pending_write_ops_.fetch_add(write_count, std::memory_order_acq_rel);
    volatile_cv_.notify_one();
    return true;
}

void MemoryEngine::RunVolatileWriter() {
    int backoff_ms = 25;
    VolatileWriteBatch batch;
    while (!volatile_writer_stop_.load(std::memory_order_acquire)) {
        if (!TryPopVolatileBatch(&batch)) {
            std::unique_lock<std::mutex> lock(volatile_mu_);
            volatile_cv_.wait_for(lock, std::chrono::milliseconds(options_.volatile_flush_interval_ms), [&] {
                return volatile_writer_stop_.load(std::memory_order_acquire) || !volatile_queue_.empty();
            });
            continue;
        }

        std::string error;
        if (!PersistBatchNow(batch.writes, &error)) {
            flush_failures_total_.fetch_add(1, std::memory_order_acq_rel);
            {
                std::lock_guard<std::mutex> lock(volatile_mu_);
                last_flush_error_ = error;
            }
            RequeueVolatileFront(&batch);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, 2000);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(volatile_mu_);
            last_flush_error_.clear();
        }
        batch.writes.clear();
        backoff_ms = 25;
    }
}

bool MemoryEngine::Warmup(std::string* error) {
    std::string scan_error;
    auto entries = store_->Scan(kMemNs, "", "", 0, &scan_error);
    if (!scan_error.empty()) {
        if (!IsMissingScanNamespaceError(scan_error)) {
            if (error != nullptr) {
                *error = scan_error;
            }
            return false;
        }
        entries.clear();
        scan_error.clear();
    }

    {
        std::unique_lock<std::shared_mutex> lock(records_mu_);
        for (const auto& entry : entries) {
            MemoryRecord record;
            std::string decode_error;
            if (!DeserializeRecord(entry.value, &record, &decode_error)) {
                continue;
            }

            if (record.size_bytes == 0) {
                record.size_bytes = ComputeRecordSizeBytes(record);
            }
            if (record.last_access_ms == 0) {
                record.last_access_ms = record.updated_at_ms;
            }
            if (record.tier.empty()) {
                record.tier = record.tombstone ? "cold" : "hot";
            }

            records_[RecordMapKey(record.workspace_id, record.id)] = record;
            if (!record.tombstone) {
                retriever_->Index(record);
            }
        }
    }

    if (!ReplayProjectionEvents(error)) {
        return false;
    }
    return true;
}

BootstrapOutput MemoryEngine::Bootstrap(const BootstrapInput& input) {
    const auto start = std::chrono::steady_clock::now();

    BootstrapOutput out;
    const std::string summary_key = BuildSummaryKey(input.workspace_id, input.session_id);
    const auto summary_res        = store_->Get(kSummaryNs, summary_key);
    if (summary_res.found) {
        out.summary = summary_res.value;
    }

    SearchInput search_input;
    search_input.workspace_id = input.workspace_id;
    search_input.query        = input.task_text + "\n" + util::JoinLines(input.open_files);
    search_input.top_k        = 8;
    search_input.token_budget = input.token_budget;

    bool fallback = false;
    auto hits     = retriever_->Search(search_input, &fallback);

    size_t used_tokens = util::EstimateTokenCount(out.summary);
    for (const auto& hit : hits) {
        const size_t hit_tokens = util::EstimateTokenCount(hit.content);
        if (input.token_budget > 0 && (used_tokens + hit_tokens) > static_cast<size_t>(input.token_budget)) {
            break;
        }
        out.recalled_items.push_back(hit);
        used_tokens += hit_tokens;
    }

    const uint64_t now_ms = util::NowMs();
    {
        std::unique_lock<std::shared_mutex> lock(records_mu_);
        for (const auto& hit : out.recalled_items) {
            const auto it = records_.find(RecordMapKey(input.workspace_id, hit.memory_id));
            if (it == records_.end() || it->second.tombstone) {
                continue;
            }
            it->second.last_access_ms = now_ms;
            ++it->second.hit_count;
        }
    }

    const size_t before = util::EstimateTokenCount(search_input.query) + used_tokens;
    const size_t after  = used_tokens;
    metrics_.RecordTokenReduction(before, after);
    metrics_.RecordFallback(fallback);
    out.estimated_tokens_saved = static_cast<int>(before > after ? before - after : 0);

    const auto end  = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    metrics_.RecordReadLatency(ms);

    return out;
}

CommitTurnOutput MemoryEngine::CommitTurn(const CommitTurnInput& input) {
    const auto start = std::chrono::steady_clock::now();

    CommitTurnOutput out;
    const uint64_t now_ms = util::NowMs();

    const std::string redacted_user      = util::RedactSecrets(input.user_text);
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

    out.summary_updated = summary_engine_.Update(existing_summary, redacted_user, redacted_assistant, redacted_code,
                                                 redacted_commands, 512);

    MemoryRecord record;
    record.id           = NextMemoryId();
    record.workspace_id = input.workspace_id;
    record.session_id   = input.session_id;
    record.source       = "commit_turn";
    record.content      = redacted_user + "\n" + redacted_assistant;
    if (!redacted_code.empty()) {
        record.content += "\n```\n" + redacted_code.back() + "\n```";
    }
    if (!redacted_commands.empty()) {
        record.content += "\n$ " + redacted_commands.back();
    }
    if (options_.max_record_bytes > 0 && record.content.size() > static_cast<size_t>(options_.max_record_bytes)) {
        record.content.resize(static_cast<size_t>(options_.max_record_bytes));
    }
    record.tags             = {"turn", "auto"};
    record.pinned           = false;
    record.tombstone        = false;
    record.created_at_ms    = now_ms;
    record.updated_at_ms    = now_ms;
    record.version          = 1;
    record.size_bytes       = ComputeRecordSizeBytes(record);
    record.last_access_ms   = now_ms;
    record.hit_count        = 0;
    record.importance_score = 1.0;
    record.tier             = "hot";
    record.node_id          = node_id_;

    std::vector<WriteEntry> writes;
    writes.push_back(WriteEntry{
        WriteOp::Upsert,
        KeyValueEntry{kSummaryNs, summary_key, out.summary_updated, now_ms},
    });

    std::string persist_error;
    if (!PersistRecord(record, &writes, &persist_error)) {
        return out;
    }
    std::string event_key;
    if (!AppendProjectionEvent(record, OpType::Upsert, "commit_turn", &writes, &event_key)) {
        return out;
    }
    AppendProjectionCheckpoint(record.workspace_id, event_key, now_ms, &writes);

    const bool accepted_mode = (options_.write_ack_mode == WriteAckMode::Accepted);
    if (accepted_mode) {
        UpsertRecordInMemory(record);
    }

    std::string write_error;
    if (!EnqueueOrPersistBatch(std::move(writes), &write_error)) {
        if (accepted_mode) {
            RemoveRecordInMemory(record);
        }
        return out;
    }

    if (!accepted_mode) {
        UpsertRecordInMemory(record);
    }
    out.stored_ids.push_back(record.id);

    const size_t before_tokens = util::EstimateTokenCount(input.user_text) +
                                 util::EstimateTokenCount(input.assistant_text) +
                                 util::EstimateTokenCount(util::JoinLines(input.code_snippets)) +
                                 util::EstimateTokenCount(util::JoinLines(input.commands));
    const size_t after_tokens = util::EstimateTokenCount(record.content);
    metrics_.RecordTokenReduction(before_tokens, after_tokens);

    const auto end  = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    metrics_.RecordWriteLatency(ms);

    MaybeScheduleGc();
    return out;
}

SearchOutput MemoryEngine::Search(const SearchInput& input) {
    const auto start = std::chrono::steady_clock::now();

    SearchOutput out;
    bool fallback = false;
    auto hits     = retriever_->Search(input, &fallback);

    size_t used_tokens = 0;
    for (const auto& hit : hits) {
        const size_t hit_tokens = util::EstimateTokenCount(hit.content);
        if (input.token_budget > 0 && (used_tokens + hit_tokens) > static_cast<size_t>(input.token_budget)) {
            break;
        }
        out.hits.push_back(hit);
        used_tokens += hit_tokens;
    }

    const uint64_t now_ms = util::NowMs();
    {
        std::unique_lock<std::shared_mutex> lock(records_mu_);
        for (const auto& hit : out.hits) {
            const auto it = records_.find(RecordMapKey(input.workspace_id, hit.memory_id));
            if (it == records_.end() || it->second.tombstone) {
                continue;
            }
            it->second.last_access_ms = now_ms;
            ++it->second.hit_count;
        }
    }

    metrics_.RecordFallback(fallback);

    const auto end  = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    metrics_.RecordReadLatency(ms);

    return out;
}

CompactOutput MemoryEngine::Compact(const std::string& workspace_id) { return CompactInternal(workspace_id, true); }

bool MemoryEngine::Pin(const std::string& workspace_id, const std::string& memory_id, bool pin, std::string* error) {
    MemoryRecord record;
    if (!LoadRecord(workspace_id, memory_id, &record, error)) {
        return false;
    }
    const MemoryRecord previous = record;

    record.pinned        = pin;
    record.updated_at_ms = util::NowMs();
    ++record.version;
    record.importance_score = pin ? 100.0 : std::min(record.importance_score, 5.0);
    record.size_bytes       = ComputeRecordSizeBytes(record);
    record.node_id          = node_id_;

    std::vector<WriteEntry> writes;
    if (!PersistRecord(record, &writes, error)) {
        return false;
    }
    std::string event_key;
    if (!AppendProjectionEvent(record, OpType::Pin, "pin", &writes, &event_key)) {
        if (error != nullptr) {
            *error = "failed to append projection event";
        }
        return false;
    }
    AppendProjectionCheckpoint(record.workspace_id, event_key, std::max(record.updated_at_ms, util::NowMs()), &writes);

    const bool accepted_mode = (options_.write_ack_mode == WriteAckMode::Accepted);
    if (accepted_mode) {
        UpsertRecordInMemory(record);
    }
    if (!EnqueueOrPersistBatch(std::move(writes), error)) {
        if (accepted_mode) {
            UpsertRecordInMemory(previous);
        }
        return false;
    }

    if (!accepted_mode) {
        UpsertRecordInMemory(record);
    }
    MaybeScheduleGc();
    return true;
}

StatsSnapshot MemoryEngine::Stats(const std::string& workspace_id, const std::string& window) const {
    (void)window;
    StatsSnapshot out = metrics_.Snapshot();

    std::string usage_error;
    const StoreUsage usage = store_->ApproximateUsage(&usage_error);
    (void)usage_error;
    out.mem_used_bytes  = usage.mem_used_bytes;
    out.disk_used_bytes = usage.disk_used_bytes;

    uint64_t item_count      = 0;
    uint64_t tombstone_count = 0;
    {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        for (const auto& kv : records_) {
            const MemoryRecord& rec = kv.second;
            if (!workspace_id.empty() && rec.workspace_id != workspace_id) {
                continue;
            }
            ++item_count;
            if (rec.tombstone) {
                ++tombstone_count;
            }
        }
    }
    out.item_count                   = item_count;
    out.tombstone_count              = tombstone_count;
    out.gc_last_run_ms               = gc_last_run_ms_.load(std::memory_order_acquire);
    out.gc_evicted_count             = gc_evicted_count_.load(std::memory_order_acquire);
    out.capacity_blocked             = capacity_blocked_.load(std::memory_order_acquire);
    out.write_ack_mode               = WriteAckModeToString(options_.write_ack_mode);
    out.pending_write_ops            = pending_write_ops_.load(std::memory_order_acquire);
    out.flush_failures_total         = flush_failures_total_.load(std::memory_order_acquire);
    out.volatile_dropped_on_shutdown = volatile_dropped_on_shutdown_.load(std::memory_order_acquire);
    out.effective_backend            = options_.effective_backend;
    {
        std::lock_guard<std::mutex> lock(volatile_mu_);
        out.last_flush_error = last_flush_error_;
    }
    return out;
}

bool MemoryEngine::PersistRecord(const MemoryRecord& record, std::vector<WriteEntry>* out_writes,
                                 std::string* error) const {
    if (out_writes == nullptr) {
        if (error != nullptr) {
            *error = "null write list";
        }
        return false;
    }

    const std::string mem_key = BuildMemKey(record.workspace_id, record.id);
    out_writes->push_back(WriteEntry{
        WriteOp::Upsert,
        KeyValueEntry{kMemNs, mem_key, SerializeRecord(record), record.updated_at_ms},
    });

    MemoryRecord old_record;
    bool has_old_record = false;
    {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        const auto it = records_.find(RecordMapKey(record.workspace_id, record.id));
        if (it != records_.end()) {
            old_record     = it->second;
            has_old_record = true;
        }
    }

    if (has_old_record) {
        std::unordered_set<std::string> old_terms;
        const auto old_tokens = util::Tokenize(old_record.content);
        for (const auto& tok : old_tokens) {
            old_terms.insert(tok);
        }
        for (const auto& term : old_terms) {
            const std::string lex_key = record.workspace_id + ":" + term + ":" + record.id;
            out_writes->push_back(WriteEntry{
                WriteOp::Delete,
                KeyValueEntry{kLexNs, lex_key, {}, record.updated_at_ms},
            });
        }
    }

    if (record.tombstone) {
        out_writes->push_back(WriteEntry{
            WriteOp::Delete,
            KeyValueEntry{kEmbNs, mem_key, {}, record.updated_at_ms},
        });

        std::unordered_set<std::string> terms;
        const auto tokens = util::Tokenize(record.content);
        for (const auto& tok : tokens) {
            terms.insert(tok);
        }
        for (const auto& term : terms) {
            const std::string lex_key = record.workspace_id + ":" + term + ":" + record.id;
            out_writes->push_back(WriteEntry{
                WriteOp::Delete,
                KeyValueEntry{kLexNs, lex_key, {}, record.updated_at_ms},
            });
        }
        return true;
    }

    util::Json emb;
    emb.put("model_id", "hash-embed-v1");
    emb.put("dim", 64);
    emb.put("updated_at_ms", record.updated_at_ms);
    out_writes->push_back(WriteEntry{
        WriteOp::Upsert,
        KeyValueEntry{kEmbNs, mem_key, util::ToJsonString(emb, false), record.updated_at_ms},
    });

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
        out_writes->push_back(WriteEntry{
            WriteOp::Upsert,
            KeyValueEntry{kLexNs, lex_key, util::ToJsonString(lex, false), record.updated_at_ms},
        });
    }

    return true;
}

void MemoryEngine::UpsertRecordInMemory(const MemoryRecord& record) {
    {
        std::unique_lock<std::shared_mutex> lock(records_mu_);
        records_[RecordMapKey(record.workspace_id, record.id)] = record;
    }
    if (record.tombstone) {
        retriever_->Remove(record.workspace_id, record.id);
    } else {
        retriever_->Index(record);
    }
}

void MemoryEngine::RemoveRecordInMemory(const MemoryRecord& record) {
    {
        std::unique_lock<std::shared_mutex> lock(records_mu_);
        records_.erase(RecordMapKey(record.workspace_id, record.id));
    }
    retriever_->Remove(record.workspace_id, record.id);
}

bool MemoryEngine::AppendProjectionEvent(const MemoryRecord& record, OpType op_type, const std::string& reason,
                                         std::vector<WriteEntry>* out_writes, std::string* out_event_key) {
    if (out_writes == nullptr) {
        return false;
    }
    const uint64_t now_ms       = std::max(record.updated_at_ms, util::NowMs());
    const uint64_t seq          = local_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string workspace = record.workspace_id.empty() ? "default" : record.workspace_id;
    const std::string memory_id = record.id.empty() ? "unknown" : record.id;
    const std::string event_key = workspace + ":" + ZeroPadUint64(now_ms) + ":" + ZeroPadUint64(seq) + ":" + memory_id;

    util::Json event;
    event.put("event_key", event_key);
    event.put("workspace_id", workspace);
    event.put("memory_id", memory_id);
    event.put("event_type", reason);
    event.put("op_type", OpTypeToString(op_type));
    event.put("updated_at_ms", record.updated_at_ms);
    event.put("version", record.version);
    event.put("node_id", record.node_id);
    event.put("record_payload", SerializeRecord(record));

    out_writes->push_back(WriteEntry{
        WriteOp::Upsert,
        KeyValueEntry{kEventsNs, event_key, util::ToJsonString(event, false), now_ms},
    });
    if (out_event_key != nullptr) {
        *out_event_key = event_key;
    }
    return true;
}

void MemoryEngine::AppendProjectionCheckpoint(const std::string& workspace_id, const std::string& event_key,
                                              uint64_t ts, std::vector<WriteEntry>* out_writes) const {
    if (out_writes == nullptr || workspace_id.empty() || event_key.empty()) {
        return;
    }
    util::Json ckpt;
    ckpt.put("workspace_id", workspace_id);
    ckpt.put("event_key", event_key);
    ckpt.put("updated_at_ms", ts);
    out_writes->push_back(WriteEntry{
        WriteOp::Upsert,
        KeyValueEntry{kProjectionCkptNs, workspace_id, util::ToJsonString(ckpt, false), ts},
    });
}

bool MemoryEngine::ReplayProjectionEvents(std::string* error) {
    std::string ckpt_scan_error;
    auto ckpt_entries = store_->Scan(kProjectionCkptNs, "", "", 0, &ckpt_scan_error);
    if (!ckpt_scan_error.empty()) {
        if (!IsMissingScanNamespaceError(ckpt_scan_error)) {
            if (error != nullptr) {
                *error = "scan ds_projection_ckpt failed: " + ckpt_scan_error;
            }
            return false;
        }
        ckpt_entries.clear();
        ckpt_scan_error.clear();
    }

    std::unordered_map<std::string, std::string> checkpoints;
    for (const auto& entry : ckpt_entries) {
        std::string workspace = entry.key;
        std::string event_key = entry.value;

        util::Json json;
        std::string parse_error;
        if (util::ParseJson(entry.value, &json, &parse_error)) {
            const std::string payload_workspace = util::GetStringOr(json, "workspace_id", workspace);
            const std::string payload_key       = util::GetStringOr(json, "event_key", "");
            if (!payload_workspace.empty()) {
                workspace = payload_workspace;
            }
            if (!payload_key.empty()) {
                event_key = payload_key;
            }
        }
        if (!workspace.empty() && !event_key.empty()) {
            checkpoints[workspace] = event_key;
        }
    }

    std::string events_scan_error;
    auto events = store_->Scan(kEventsNs, "", "", 0, &events_scan_error);
    if (!events_scan_error.empty()) {
        if (!IsMissingScanNamespaceError(events_scan_error)) {
            if (error != nullptr) {
                *error = "scan ds_events failed: " + events_scan_error;
            }
            return false;
        }
        events.clear();
        events_scan_error.clear();
    }
    std::sort(events.begin(), events.end(), [](const KeyValueEntry& lhs, const KeyValueEntry& rhs) {
        if (lhs.key == rhs.key) {
            return lhs.ts < rhs.ts;
        }
        return lhs.key < rhs.key;
    });

    std::unordered_map<std::string, std::string> latest_checkpoints = checkpoints;
    for (const auto& event : events) {
        std::string workspace = WorkspaceFromEventKey(event.key);
        if (workspace.empty()) {
            continue;
        }
        const auto ckpt_it = checkpoints.find(workspace);
        if (ckpt_it != checkpoints.end() && !ckpt_it->second.empty() && event.key <= ckpt_it->second) {
            continue;
        }

        util::Json event_json;
        std::string parse_error;
        if (!util::ParseJson(event.value, &event_json, &parse_error)) {
            continue;
        }

        workspace = util::GetStringOr(event_json, "workspace_id", workspace);
        const std::string payload =
            util::GetStringOr(event_json, "record_payload", util::GetStringOr(event_json, "payload_json", ""));
        if (payload.empty()) {
            continue;
        }

        MemoryRecord incoming;
        std::string decode_error;
        if (!DeserializeRecord(payload, &incoming, &decode_error)) {
            continue;
        }
        if (incoming.workspace_id.empty()) {
            incoming.workspace_id = workspace;
        }
        if (incoming.id.empty()) {
            incoming.id = util::GetStringOr(event_json, "memory_id", "");
        }
        if (incoming.id.empty()) {
            continue;
        }
        if (incoming.node_id.empty()) {
            incoming.node_id = util::GetStringOr(event_json, "node_id", "");
        }
        if (incoming.updated_at_ms == 0) {
            incoming.updated_at_ms = util::GetUint64Or(event_json, "updated_at_ms", event.ts);
        }
        if (incoming.version == 0) {
            incoming.version = util::GetUint64Or(event_json, "version", 1);
        }

        bool should_apply = true;
        {
            std::shared_lock<std::shared_mutex> lock(records_mu_);
            const auto it = records_.find(RecordMapKey(incoming.workspace_id, incoming.id));
            if (it != records_.end()) {
                should_apply = IsIncomingNewer(it->second, incoming);
            }
        }

        if (should_apply) {
            std::vector<WriteEntry> projection_writes;
            std::string persist_error;
            if (!PersistRecord(incoming, &projection_writes, &persist_error)) {
                if (error != nullptr) {
                    *error = "replay persist failed: " + persist_error;
                }
                return false;
            }
            const auto replay_res = store_->BatchWrite(projection_writes);
            if (!replay_res.ok) {
                if (error != nullptr) {
                    *error = "replay batch write failed: " + replay_res.error;
                }
                return false;
            }

            {
                std::unique_lock<std::shared_mutex> lock(records_mu_);
                records_[RecordMapKey(incoming.workspace_id, incoming.id)] = incoming;
            }
            if (incoming.tombstone) {
                retriever_->Remove(incoming.workspace_id, incoming.id);
            } else {
                retriever_->Index(incoming);
            }
        }

        latest_checkpoints[workspace] = event.key;
    }

    std::vector<WriteEntry> ckpt_writes;
    const uint64_t now_ms = util::NowMs();
    for (const auto& kv : latest_checkpoints) {
        const auto old = checkpoints.find(kv.first);
        if (old != checkpoints.end() && old->second == kv.second) {
            continue;
        }
        AppendProjectionCheckpoint(kv.first, kv.second, now_ms, &ckpt_writes);
    }
    if (!ckpt_writes.empty()) {
        const auto ckpt_res = store_->BatchWrite(ckpt_writes);
        if (!ckpt_res.ok) {
            if (error != nullptr) {
                *error = "checkpoint update failed: " + ckpt_res.error;
            }
            return false;
        }
    }

    return true;
}

bool MemoryEngine::DeleteRecordStorage(const MemoryRecord& record, std::vector<WriteEntry>* out_writes) const {
    if (out_writes == nullptr) {
        return false;
    }

    const std::string mem_key = BuildMemKey(record.workspace_id, record.id);
    out_writes->push_back(WriteEntry{
        WriteOp::Delete,
        KeyValueEntry{kMemNs, mem_key, {}, util::NowMs()},
    });
    out_writes->push_back(WriteEntry{
        WriteOp::Delete,
        KeyValueEntry{kEmbNs, mem_key, {}, util::NowMs()},
    });

    std::unordered_set<std::string> terms;
    const auto tokens = util::Tokenize(record.content);
    for (const auto& tok : tokens) {
        terms.insert(tok);
    }
    for (const auto& term : terms) {
        const std::string lex_key = record.workspace_id + ":" + term + ":" + record.id;
        out_writes->push_back(WriteEntry{
            WriteOp::Delete,
            KeyValueEntry{kLexNs, lex_key, {}, util::NowMs()},
        });
    }
    return true;
}

bool MemoryEngine::LoadRecord(const std::string& workspace_id, const std::string& memory_id, MemoryRecord* out,
                              std::string* error) const {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "null output";
        }
        return false;
    }

    {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        const auto it = records_.find(RecordMapKey(workspace_id, memory_id));
        if (it != records_.end()) {
            *out = it->second;
            return true;
        }
    }

    const std::string key = BuildMemKey(workspace_id, memory_id);
    const auto get_res    = store_->Get(kMemNs, key);
    if (!get_res.found) {
        if (error != nullptr && !get_res.error.empty()) {
            *error = get_res.error;
        }
        return false;
    }

    return DeserializeRecord(get_res.value, out, error);
}

std::string MemoryEngine::RecordMapKey(const std::string& workspace_id, const std::string& memory_id) {
    return workspace_id + ":" + memory_id;
}

bool MemoryEngine::IsOverHighWatermark(const StoreUsage& usage) const {
    const uint64_t mem_budget_bytes  = BytesFromMb(options_.mem_budget_mb);
    const uint64_t disk_budget_bytes = BytesFromGb(options_.disk_budget_gb);

    bool over = false;
    if (mem_budget_bytes > 0) {
        const double threshold = static_cast<double>(mem_budget_bytes) * options_.gc_high_watermark;
        over                   = over || (static_cast<double>(usage.mem_used_bytes) > threshold);
    }
    if (disk_budget_bytes > 0) {
        const double threshold = static_cast<double>(disk_budget_bytes) * options_.gc_high_watermark;
        over                   = over || (static_cast<double>(usage.disk_used_bytes) > threshold);
    }
    return over;
}

bool MemoryEngine::IsBelowLowWatermark(const StoreUsage& usage) const {
    const uint64_t mem_budget_bytes  = BytesFromMb(options_.mem_budget_mb);
    const uint64_t disk_budget_bytes = BytesFromGb(options_.disk_budget_gb);

    bool below = true;
    if (mem_budget_bytes > 0) {
        const double threshold = static_cast<double>(mem_budget_bytes) * options_.gc_low_watermark;
        below                  = below && (static_cast<double>(usage.mem_used_bytes) <= threshold);
    }
    if (disk_budget_bytes > 0) {
        const double threshold = static_cast<double>(disk_budget_bytes) * options_.gc_low_watermark;
        below                  = below && (static_cast<double>(usage.disk_used_bytes) <= threshold);
    }
    return below;
}

void MemoryEngine::MaybeScheduleGc() {
    if (!gc_running_.load(std::memory_order_acquire)) {
        return;
    }

    static std::atomic<uint64_t> probe_counter{0};
    const uint64_t probe = probe_counter.fetch_add(1, std::memory_order_acq_rel) + 1;
    if ((probe % 8) != 0) {
        return;
    }

    std::string usage_error;
    const StoreUsage usage = store_->ApproximateUsage(&usage_error);
    (void)usage_error;
    if (!IsOverHighWatermark(usage)) {
        return;
    }

    gc_requested_.store(true, std::memory_order_release);
    gc_cv_.notify_one();
}

void MemoryEngine::RunGcLoop() {
    while (!gc_stop_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(gc_mu_);
        gc_cv_.wait_for(lock, std::chrono::seconds(5), [&] {
            return gc_stop_.load(std::memory_order_acquire) || gc_requested_.load(std::memory_order_acquire);
        });
        if (gc_stop_.load(std::memory_order_acquire)) {
            break;
        }
        gc_requested_.store(false, std::memory_order_release);
        lock.unlock();
        CompactInternal("", false);
    }
}

CompactOutput MemoryEngine::CompactInternal(const std::string& workspace_id, bool force) {
    CompactOutput out;

    std::string usage_error;
    StoreUsage usage = store_->ApproximateUsage(&usage_error);
    (void)usage_error;
    out.mem_before_bytes  = usage.mem_used_bytes;
    out.disk_before_bytes = usage.disk_used_bytes;

    const bool over_high = IsOverHighWatermark(usage);
    if (!force && !over_high) {
        out.mem_after_bytes  = usage.mem_used_bytes;
        out.disk_after_bytes = usage.disk_used_bytes;
        return out;
    }

    if (workspace_id.empty()) {
        const uint64_t now_ms = util::NowMs();
        std::unordered_set<std::string> workspace_set;
        {
            std::shared_lock<std::shared_mutex> lock(records_mu_);
            workspace_set.reserve(records_.size());
            for (const auto& kv : records_) {
                if (!kv.second.workspace_id.empty()) {
                    workspace_set.insert(kv.second.workspace_id);
                }
            }
        }

        std::vector<std::string> workspaces(workspace_set.begin(), workspace_set.end());
        std::sort(workspaces.begin(), workspaces.end());

        for (const auto& ws : workspaces) {
            const auto partial   = CompactInternal(ws, force);
            out.triggered        = out.triggered || partial.triggered;
            out.capacity_blocked = out.capacity_blocked || partial.capacity_blocked;
            out.summarized_count += partial.summarized_count;
            out.tombstoned_count += partial.tombstoned_count;
            out.deleted_count += partial.deleted_count;
        }

        if (out.triggered) {
            gc_last_run_ms_.store(now_ms, std::memory_order_release);
        }

        std::string post_usage_error;
        usage = store_->ApproximateUsage(&post_usage_error);
        (void)post_usage_error;
        out.mem_after_bytes  = usage.mem_used_bytes;
        out.disk_after_bytes = usage.disk_used_bytes;

        const bool still_over = IsOverHighWatermark(usage);
        bool has_evictable    = false;
        {
            std::shared_lock<std::shared_mutex> lock(records_mu_);
            for (const auto& kv : records_) {
                const auto& rec = kv.second;
                if (!rec.tombstone && !rec.pinned) {
                    has_evictable = true;
                    break;
                }
            }
        }
        const bool blocked = still_over && !has_evictable;
        capacity_blocked_.store(blocked, std::memory_order_release);
        out.capacity_blocked = out.capacity_blocked || blocked;
        if (still_over && has_evictable && gc_running_.load(std::memory_order_acquire)) {
            gc_requested_.store(true, std::memory_order_release);
            gc_cv_.notify_one();
        }
        return out;
    }
    out.triggered = true;

    const uint64_t now_ms    = util::NowMs();
    const uint32_t max_batch = (options_.gc_batch_size == 0) ? 256 : options_.gc_batch_size;

    std::vector<MemoryRecord> active_records;
    std::vector<MemoryRecord> tombstones;
    {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        active_records.reserve(records_.size());
        tombstones.reserve(records_.size());
        for (const auto& kv : records_) {
            const MemoryRecord& rec = kv.second;
            if (!workspace_id.empty() && rec.workspace_id != workspace_id) {
                continue;
            }
            if (rec.tombstone) {
                tombstones.push_back(rec);
            } else {
                active_records.push_back(rec);
            }
        }
    }

    std::vector<WriteEntry> writes;
    std::vector<MemoryRecord> upserted_records;
    std::vector<MemoryRecord> physically_deleted_records;

    std::unordered_set<std::string> tombstoned_ids;

    std::vector<MemoryRecord> summary_candidates = active_records;
    std::sort(summary_candidates.begin(), summary_candidates.end(),
              [&](const MemoryRecord& lhs, const MemoryRecord& rhs) {
                  if (lhs.content.size() == rhs.content.size()) {
                      return lhs.updated_at_ms < rhs.updated_at_ms;
                  }
                  return lhs.content.size() > rhs.content.size();
              });

    uint32_t actions              = 0;
    const uint32_t summary_budget = std::max<uint32_t>(1, max_batch / 4);
    for (const auto& candidate : summary_candidates) {
        if (actions >= summary_budget || actions >= max_batch) {
            break;
        }
        if (candidate.pinned || candidate.tombstone || candidate.tier == "summary") {
            continue;
        }
        if (candidate.content.size() < 1024) {
            continue;
        }

        MemoryRecord summary = BuildSummaryRecord(candidate, now_ms);
        MemoryRecord tomb    = candidate;
        tomb.tombstone       = true;
        tomb.tier            = "cold";
        tomb.updated_at_ms   = now_ms;
        tomb.last_access_ms  = now_ms;
        tomb.version         = std::max<uint64_t>(candidate.version, 1) + 1;
        tomb.node_id         = node_id_;
        tomb.size_bytes      = ComputeRecordSizeBytes(tomb);

        std::string error;
        if (!PersistRecord(summary, &writes, &error)) {
            continue;
        }
        if (!PersistRecord(tomb, &writes, &error)) {
            continue;
        }
        std::string event_key;
        if (!AppendProjectionEvent(summary, OpType::Upsert, "compact_summary", &writes, &event_key)) {
            continue;
        }
        AppendProjectionCheckpoint(summary.workspace_id, event_key, now_ms, &writes);
        if (!AppendProjectionEvent(tomb, OpType::Delete, "compact_tombstone", &writes, &event_key)) {
            continue;
        }
        AppendProjectionCheckpoint(tomb.workspace_id, event_key, now_ms, &writes);

        upserted_records.push_back(summary);
        upserted_records.push_back(tomb);
        tombstoned_ids.insert(tomb.id);
        ++out.summarized_count;
        ++out.tombstoned_count;
        ++actions;
    }

    std::vector<MemoryRecord> eviction_candidates = active_records;
    std::sort(eviction_candidates.begin(), eviction_candidates.end(),
              [&](const MemoryRecord& lhs, const MemoryRecord& rhs) {
                  const double lhs_rank = EvictionRank(lhs, now_ms);
                  const double rhs_rank = EvictionRank(rhs, now_ms);
                  if (lhs_rank == rhs_rank) {
                      return lhs.updated_at_ms < rhs.updated_at_ms;
                  }
                  return lhs_rank < rhs_rank;
              });

    for (const auto& candidate : eviction_candidates) {
        if (actions >= max_batch) {
            break;
        }
        if (candidate.pinned || candidate.tombstone) {
            continue;
        }
        if (tombstoned_ids.find(candidate.id) != tombstoned_ids.end()) {
            continue;
        }

        MemoryRecord tomb   = candidate;
        tomb.tombstone      = true;
        tomb.tier           = "cold";
        tomb.updated_at_ms  = now_ms;
        tomb.last_access_ms = now_ms;
        tomb.version        = std::max<uint64_t>(candidate.version, 1) + 1;
        tomb.node_id        = node_id_;
        tomb.size_bytes     = ComputeRecordSizeBytes(tomb);

        std::string error;
        if (!PersistRecord(tomb, &writes, &error)) {
            continue;
        }
        std::string event_key;
        if (!AppendProjectionEvent(tomb, OpType::Delete, "compact_tombstone", &writes, &event_key)) {
            continue;
        }
        AppendProjectionCheckpoint(tomb.workspace_id, event_key, now_ms, &writes);
        upserted_records.push_back(tomb);
        ++out.tombstoned_count;
        ++actions;
    }

    if (options_.enable_tombstone_gc) {
        for (const auto& candidate : tombstones) {
            if (actions >= max_batch) {
                break;
            }
            const uint64_t age_ms = now_ms > candidate.updated_at_ms ? (now_ms - candidate.updated_at_ms) : 0;
            if (age_ms < options_.tombstone_retention_ms) {
                continue;
            }
            if (!DeleteRecordStorage(candidate, &writes)) {
                continue;
            }
            std::string event_key;
            if (!AppendProjectionEvent(candidate, OpType::Delete, "compact_delete", &writes, &event_key)) {
                continue;
            }
            AppendProjectionCheckpoint(candidate.workspace_id, event_key, now_ms, &writes);
            physically_deleted_records.push_back(candidate);
            ++out.deleted_count;
            ++actions;
        }
    }

    if (writes.empty()) {
        bool all_pinned = !active_records.empty();
        for (const auto& rec : active_records) {
            if (!rec.pinned) {
                all_pinned = false;
                break;
            }
        }
        const bool blocked = over_high && all_pinned;
        capacity_blocked_.store(blocked, std::memory_order_release);
        out.capacity_blocked = blocked;
        gc_last_run_ms_.store(now_ms, std::memory_order_release);
        out.mem_after_bytes  = usage.mem_used_bytes;
        out.disk_after_bytes = usage.disk_used_bytes;
        return out;
    }

    const auto batch_res = store_->BatchWrite(writes);
    if (!batch_res.ok) {
        out.mem_after_bytes  = usage.mem_used_bytes;
        out.disk_after_bytes = usage.disk_used_bytes;
        return out;
    }

    {
        std::unique_lock<std::shared_mutex> lock(records_mu_);
        for (const auto& rec : upserted_records) {
            records_[RecordMapKey(rec.workspace_id, rec.id)] = rec;
        }
        for (const auto& rec : physically_deleted_records) {
            records_.erase(RecordMapKey(rec.workspace_id, rec.id));
        }
    }

    for (const auto& rec : upserted_records) {
        if (rec.tombstone) {
            retriever_->Remove(rec.workspace_id, rec.id);
        } else {
            retriever_->Index(rec);
        }
    }
    for (const auto& rec : physically_deleted_records) {
        retriever_->Remove(rec.workspace_id, rec.id);
    }

    gc_last_run_ms_.store(now_ms, std::memory_order_release);
    gc_evicted_count_.fetch_add(out.tombstoned_count + out.deleted_count, std::memory_order_acq_rel);

    std::string post_usage_error;
    usage = store_->ApproximateUsage(&post_usage_error);
    (void)post_usage_error;
    out.mem_after_bytes  = usage.mem_used_bytes;
    out.disk_after_bytes = usage.disk_used_bytes;

    const bool still_over = IsOverHighWatermark(usage);
    bool has_evictable    = false;
    {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        for (const auto& kv : records_) {
            const auto& rec = kv.second;
            if (!workspace_id.empty() && rec.workspace_id != workspace_id) {
                continue;
            }
            if (!rec.tombstone && !rec.pinned) {
                has_evictable = true;
                break;
            }
        }
    }

    const bool blocked = still_over && !has_evictable;
    capacity_blocked_.store(blocked, std::memory_order_release);
    out.capacity_blocked = blocked;
    if (still_over && has_evictable && gc_running_.load(std::memory_order_acquire)) {
        gc_requested_.store(true, std::memory_order_release);
        gc_cv_.notify_one();
    }

    return out;
}

MemoryRecord MemoryEngine::BuildSummaryRecord(const MemoryRecord& src, uint64_t now_ms) {
    MemoryRecord summary;
    summary.id               = NextMemoryId();
    summary.workspace_id     = src.workspace_id;
    summary.session_id       = src.session_id;
    summary.source           = "compact_summary";
    summary.content          = summary_engine_.Update("", src.content, "", {}, {}, 256);
    summary.tags             = {"summary", "compact"};
    summary.pinned           = false;
    summary.tombstone        = false;
    summary.created_at_ms    = now_ms;
    summary.updated_at_ms    = now_ms;
    summary.version          = 1;
    summary.size_bytes       = ComputeRecordSizeBytes(summary);
    summary.last_access_ms   = now_ms;
    summary.hit_count        = 0;
    summary.importance_score = std::max(0.2, src.importance_score * 0.4);
    summary.tier             = "summary";
    summary.node_id          = node_id_;
    return summary;
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
    json.put("tombstone", record.tombstone);
    json.put("created_at_ms", record.created_at_ms);
    json.put("updated_at_ms", record.updated_at_ms);
    json.put("version", record.version);
    json.put("size_bytes", record.size_bytes);
    json.put("last_access_ms", record.last_access_ms);
    json.put("hit_count", record.hit_count);
    json.put("importance_score", record.importance_score);
    json.put("tier", record.tier);
    json.put("node_id", record.node_id);
    json.add_child("tags", util::MakeArray(record.tags));
    return util::ToJsonString(json, false);
}

bool MemoryEngine::DeserializeRecord(const std::string& text, MemoryRecord* record, std::string* error) {
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

    record->id               = util::GetStringOr(json, "id", "");
    record->workspace_id     = util::GetStringOr(json, "workspace_id", "");
    record->session_id       = util::GetStringOr(json, "session_id", "");
    record->source           = util::GetStringOr(json, "source", "");
    record->content          = util::GetStringOr(json, "content", "");
    record->pinned           = util::GetBoolOr(json, "pinned", false);
    record->tombstone        = util::GetBoolOr(json, "tombstone", false);
    record->created_at_ms    = util::GetUint64Or(json, "created_at_ms", 0);
    record->updated_at_ms    = util::GetUint64Or(json, "updated_at_ms", 0);
    record->version          = util::GetUint64Or(json, "version", 0);
    record->size_bytes       = util::GetUint64Or(json, "size_bytes", 0);
    record->last_access_ms   = util::GetUint64Or(json, "last_access_ms", record->updated_at_ms);
    record->hit_count        = util::GetUint64Or(json, "hit_count", 0);
    record->importance_score = GetDoubleOr(json, "importance_score", 0.0);
    record->tier             = util::GetStringOr(json, "tier", record->tombstone ? "cold" : "hot");
    record->node_id          = util::GetStringOr(json, "node_id", "");
    record->tags             = util::ReadStringArray(json, "tags");

    if (record->size_bytes == 0) {
        record->size_bytes = ComputeRecordSizeBytes(*record);
    }
    if (record->last_access_ms == 0) {
        record->last_access_ms = record->updated_at_ms;
    }
    if (record->tier.empty()) {
        record->tier = record->tombstone ? "cold" : "hot";
    }
    return true;
}

}  // namespace pgmem::core
