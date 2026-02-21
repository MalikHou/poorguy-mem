#include "pgmem/core/memory_engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_set>

#include "pgmem/util/json.h"
#include "pgmem/util/text.h"
#include "pgmem/util/time.h"

namespace pgmem::core {
namespace {

constexpr const char* kDocNs            = "mem_docs";
constexpr const char* kTermDictNs       = "mem_term_dict";
constexpr const char* kPostingBlkNs     = "mem_posting_blk";
constexpr const char* kVecCodeNs        = "mem_vec_code";
constexpr const char* kVecFpNs          = "mem_vec_fp";
constexpr const char* kRouteMetaNs      = "mem_route_meta";
constexpr const char* kEventsNs         = "ds_events";
constexpr const char* kProjectionCkptNs = "ds_projection_ckpt";

uint64_t BytesFromMb(uint64_t mb) { return mb * 1024ull * 1024ull; }
uint64_t BytesFromGb(uint64_t gb) { return gb * 1024ull * 1024ull * 1024ull; }

std::string ZeroPadUint64(uint64_t v) {
    std::ostringstream oss;
    oss << std::setw(20) << std::setfill('0') << v;
    return oss.str();
}

std::string BuildDocKey(const std::string& workspace_id, const std::string& memory_id) {
    return "ws/" + workspace_id + "/doc/" + memory_id;
}

std::string BuildTermDictKey(const std::string& workspace_id, const std::string& term) {
    return "ws/" + workspace_id + "/term/" + term;
}

std::string BuildPostingKey(const std::string& workspace_id, const std::string& term, uint64_t bucket,
                            const std::string& memory_id) {
    return "ws/" + workspace_id + "/term/" + term + "/b/" + std::to_string(bucket) + "/blk/" + memory_id;
}

std::string BuildVecCodeKey(const std::string& workspace_id, uint64_t bucket, const std::string& memory_id) {
    return "ws/" + workspace_id + "/vec/" + std::to_string(bucket) + "/" + memory_id;
}

std::string BuildVecFpKey(const std::string& workspace_id, const std::string& memory_id) {
    return "ws/" + workspace_id + "/vecfp/" + memory_id;
}

std::string BuildRouteMetaKey(const std::string& workspace_id) { return "ws/" + workspace_id; }

std::string BuildEventKey(const std::string& workspace_id, uint64_t ts, uint64_t seq) {
    return "ws/" + workspace_id + "/ts/" + ZeroPadUint64(ts) + "/seq/" + ZeroPadUint64(seq);
}

std::string BuildCkptKey(const std::string& workspace_id) { return "ws/" + workspace_id; }

std::string WorkspaceFromDocKey(const std::string& doc_key) {
    const std::string prefix = "ws/";
    const std::string marker = "/doc/";
    if (doc_key.rfind(prefix, 0) != 0) {
        return {};
    }
    const size_t marker_pos = doc_key.find(marker);
    if (marker_pos == std::string::npos || marker_pos <= prefix.size()) {
        return {};
    }
    return doc_key.substr(prefix.size(), marker_pos - prefix.size());
}

std::string MemoryIdFromDocKey(const std::string& doc_key) {
    const std::string marker = "/doc/";
    const size_t marker_pos  = doc_key.find(marker);
    if (marker_pos == std::string::npos || marker_pos + marker.size() >= doc_key.size()) {
        return {};
    }
    return doc_key.substr(marker_pos + marker.size());
}

std::string WorkspaceFromEventKey(const std::string& event_key) {
    const std::string prefix = "ws/";
    const std::string marker = "/ts/";
    if (event_key.rfind(prefix, 0) != 0) {
        return {};
    }
    const size_t marker_pos = event_key.find(marker);
    if (marker_pos == std::string::npos || marker_pos <= prefix.size()) {
        return {};
    }
    return event_key.substr(prefix.size(), marker_pos - prefix.size());
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

uint64_t GetUint64OrSafe(const util::Json& json, const std::string& path, uint64_t fallback) {
    try {
        return json.get<uint64_t>(path);
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
    total += record.dedup_key.size();
    total += record.shard_id.size();
    total += record.replica_role.size();
    total += record.shard_hint.size();
    for (const auto& tag : record.tags) {
        total += tag.size();
    }
    for (const auto& kv : record.metadata) {
        total += kv.first.size() + kv.second.size();
    }
    total += 384;
    return static_cast<uint64_t>(total);
}

double EvictionRank(const MemoryRecord& rec, uint64_t now_ms) {
    const uint64_t reference_ms = std::max(rec.updated_at_ms, rec.last_access_ms);
    const double age_hours      = static_cast<double>(now_ms > reference_ms ? (now_ms - reference_ms) : 0) / 3600000.0;
    const double hit_signal     = std::log1p(static_cast<double>(rec.hit_count));
    const double source_weight  = (rec.source == "turn") ? 2.0 : 0.8;
    const double pin_bias       = rec.pinned ? 1500.0 : 0.0;
    const double tombstone_bias = rec.tombstone ? -1500.0 : 0.0;
    return rec.importance_score * 10.0 + hit_signal * 4.0 + source_weight + pin_bias + tombstone_bias - age_hours * 1.5;
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

uint64_t BucketForPosting(const std::string& workspace_id, const std::string& term) {
    return static_cast<uint64_t>(std::hash<std::string>{}(workspace_id + ":" + term) % 16ull);
}

std::vector<float> BuildEmbedding(const std::string& text, size_t dim) {
    std::vector<float> emb(dim, 0.0f);
    const auto tokens = util::Tokenize(text);
    if (tokens.empty() || dim == 0) {
        return emb;
    }

    for (const auto& token : tokens) {
        const size_t index = std::hash<std::string>{}(token) % dim;
        emb[index] += 1.0f;
    }

    float norm = 0.0f;
    for (float v : emb) {
        norm += v * v;
    }
    norm = std::sqrt(norm);
    if (norm > std::numeric_limits<float>::epsilon()) {
        for (float& v : emb) {
            v /= norm;
        }
    }
    return emb;
}

std::string EncodeVectorCsv(const std::vector<float>& vec) {
    std::ostringstream oss;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << vec[i];
    }
    return oss.str();
}

uint64_t EncodeVectorBucketSignature(const std::vector<float>& vec) {
    uint64_t bucket       = 0;
    const size_t max_bits = std::min<size_t>(64, vec.size());
    for (size_t i = 0; i < max_bits; ++i) {
        if (vec[i] > 0.0f) {
            bucket |= (1ull << i);
        }
    }
    return bucket;
}

struct WorkspacePinStats {
    size_t active{0};
    size_t pinned{0};
    bool has_evictable{false};
};

WorkspacePinStats ComputeWorkspacePinStats(const std::unordered_map<std::string, MemoryRecord>& records,
                                           const std::string& workspace_id) {
    WorkspacePinStats stats;
    for (const auto& kv : records) {
        const auto& rec = kv.second;
        if (rec.workspace_id != workspace_id || rec.tombstone) {
            continue;
        }
        ++stats.active;
        if (rec.pinned) {
            ++stats.pinned;
        } else {
            stats.has_evictable = true;
        }
    }
    return stats;
}

double PinnedRatio(size_t pinned, size_t active) {
    if (active == 0) {
        return 0.0;
    }
    return static_cast<double>(pinned) / static_cast<double>(active);
}

bool IsPinBlockedByRatio(size_t pinned, size_t active, double max_ratio) {
    if (active == 0) {
        return false;
    }
    if (max_ratio <= 0.0) {
        return true;
    }
    if (max_ratio >= 1.0) {
        return false;
    }
    return PinnedRatio(pinned, active) > max_ratio;
}

}  // namespace

MemoryEngine::MemoryEngine(std::unique_ptr<store::IStoreAdapter> store, std::unique_ptr<IRetriever> retriever,
                           std::string node_id, MemoryEngineOptions options)
    : store_(std::move(store)),
      retriever_(std::move(retriever)),
      options_(std::move(options)),
      node_id_(std::move(node_id)) {
    if (options_.effective_backend.empty()) {
        options_.effective_backend = "unknown";
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
    });
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

bool MemoryEngine::Warmup(std::string* error) {
    std::string scan_error;
    auto entries = store_->Scan(kDocNs, "", "", 0, &scan_error);
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

    for (const auto& entry : entries) {
        MemoryRecord record;
        std::string decode_error;
        if (!DeserializeRecord(entry.value, &record, &decode_error)) {
            continue;
        }

        if (record.workspace_id.empty()) {
            record.workspace_id = WorkspaceFromDocKey(entry.key);
        }
        if (record.id.empty()) {
            record.id = MemoryIdFromDocKey(entry.key);
        }
        if (record.id.empty() || record.workspace_id.empty()) {
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

        UpsertRecordInMemory(record);
    }

    if (!ReplayProjectionEvents(error)) {
        return false;
    }

    EnforceResidentHardLimit();
    return true;
}

WriteOutput MemoryEngine::Write(const WriteInput& input) {
    const auto start = std::chrono::steady_clock::now();

    WriteOutput out;
    if (input.workspace_id.empty()) {
        out.ok = false;
        out.warnings.push_back("workspace_id is required");
        return out;
    }

    if (input.records.empty()) {
        out.ok = true;
        out.warnings.push_back("records is empty");
        return out;
    }

    std::vector<WriteEntry> writes;
    std::vector<MemoryRecord> upserts;
    writes.reserve(input.records.size() * 16);
    upserts.reserve(input.records.size());

    const uint64_t now_ms   = util::NowMs();
    size_t projected_active = 0;
    size_t projected_pinned = 0;
    {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        const auto pin_stats = ComputeWorkspacePinStats(records_, input.workspace_id);
        projected_active     = pin_stats.active;
        projected_pinned     = pin_stats.pinned;
    }

    for (const auto& rec_in : input.records) {
        if (rec_in.content.empty()) {
            out.warnings.push_back("skip empty content record");
            continue;
        }

        const std::string dedup_key = rec_in.dedup_key;
        if (!dedup_key.empty() && input.write_mode != "append") {
            std::shared_lock<std::shared_mutex> lock(records_mu_);
            const auto it = dedup_index_.find(input.workspace_id + ":" + dedup_key);
            if (it != dedup_index_.end()) {
                out.deduped_ids.push_back(it->second);
                continue;
            }
        }

        MemoryRecord record;
        record.id           = rec_in.record_id.empty() ? NextMemoryId() : rec_in.record_id;
        record.workspace_id = input.workspace_id;
        record.session_id   = input.session_id.empty() ? "default" : input.session_id;
        record.source       = rec_in.source.empty() ? "turn" : rec_in.source;
        record.content      = rec_in.content;
        record.tags         = rec_in.tags;
        record.metadata     = rec_in.metadata;
        record.dedup_key    = rec_in.dedup_key;
        record.pinned       = rec_in.pin;
        if (rec_in.pin) {
            const size_t next_active = projected_active + 1;
            const size_t next_pinned = projected_pinned + 1;
            if (next_pinned > options_.pin_quota_per_workspace) {
                record.pinned = false;
                out.warnings.push_back("pin governance: quota exceeded for record " + record.id);
            } else if (IsPinBlockedByRatio(next_pinned, next_active, options_.max_pinned_ratio)) {
                record.pinned = false;
                out.warnings.push_back("pin governance: pinned ratio exceeded for record " + record.id);
            }
        }
        record.tombstone        = false;
        record.created_at_ms    = now_ms;
        record.updated_at_ms    = now_ms;
        record.version          = 1;
        record.last_access_ms   = now_ms;
        record.hit_count        = 0;
        record.importance_score = std::max(0.0, rec_in.importance);
        record.tier             = "hot";
        record.node_id          = node_id_;
        record.ttl_s            = rec_in.ttl_s;
        if (record.pinned && options_.pin_ttl_override_s > 0) {
            record.ttl_s = options_.pin_ttl_override_s;
        }

        auto meta_it = record.metadata.find("shard_id");
        if (meta_it != record.metadata.end()) {
            record.shard_id = meta_it->second;
        }
        meta_it = record.metadata.find("replica_role");
        if (meta_it != record.metadata.end()) {
            record.replica_role = meta_it->second;
        }
        meta_it = record.metadata.find("shard_hint");
        if (meta_it != record.metadata.end()) {
            record.shard_hint = meta_it->second;
        }

        record.size_bytes = ComputeRecordSizeBytes(record);
        if (record.size_bytes > options_.max_record_bytes) {
            out.warnings.push_back("skip oversized record: " + record.id);
            continue;
        }

        std::string persist_error;
        if (!PersistRecord(record, &writes, &persist_error)) {
            out.warnings.push_back("persist build failed for " + record.id + ": " + persist_error);
            continue;
        }

        std::string event_key;
        if (!AppendProjectionEvent(record, OpType::Upsert, "write", &writes, &event_key)) {
            out.warnings.push_back("failed to append event for " + record.id);
            continue;
        }
        AppendProjectionCheckpoint(record.workspace_id, event_key, now_ms, &writes);

        upserts.push_back(record);
        out.stored_ids.push_back(record.id);
        ++projected_active;
        if (record.pinned) {
            ++projected_pinned;
        }
    }

    if (writes.empty()) {
        out.ok = !out.stored_ids.empty() || !out.deduped_ids.empty() || !out.warnings.empty();
        return out;
    }

    std::string write_error;
    if (!PersistBatchNow(writes, &write_error)) {
        out.ok = false;
        out.warnings.push_back(write_error);
        return out;
    }

    for (const auto& record : upserts) {
        UpsertRecordInMemory(record);
    }

    if (!input.workspace_id.empty()) {
        util::Json route;
        route.put("bucket_count", 1);
        route.put("hot_level", "normal");
        route.put("shard_hint", "workspace");
        std::vector<WriteEntry> route_write{
            WriteEntry{WriteOp::Upsert, KeyValueEntry{kRouteMetaNs, BuildRouteMetaKey(input.workspace_id),
                                                      util::ToJsonString(route, false), now_ms}},
        };
        std::string route_error;
        (void)PersistBatchNow(route_write, &route_error);
    }

    EnforceResidentHardLimit();
    MaybeScheduleGc();

    size_t before_tokens = 0;
    size_t after_tokens  = 0;
    for (const auto& rec_in : input.records) {
        before_tokens += util::EstimateTokenCount(rec_in.content);
    }
    for (const auto& rec : upserts) {
        after_tokens += util::EstimateTokenCount(rec.content);
    }
    metrics_.RecordTokenReduction(before_tokens, after_tokens);

    const auto end  = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    metrics_.RecordWriteLatency(ms);

    out.ok               = true;
    out.index_generation = index_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    return out;
}

QueryOutput MemoryEngine::Query(const QueryInput& input) {
    const auto start = std::chrono::steady_clock::now();

    QueryOutput out;
    if (input.workspace_id.empty()) {
        return out;
    }

    QueryOutput retrieved = retriever_->Query(input);

    size_t used_tokens    = 0;
    const uint64_t now_ms = util::NowMs();
    for (const auto& hit : retrieved.hits) {
        const size_t hit_tokens = util::EstimateTokenCount(hit.content);
        if (input.token_budget > 0 && (used_tokens + hit_tokens) > static_cast<size_t>(input.token_budget)) {
            break;
        }

        bool expired = false;
        {
            std::shared_lock<std::shared_mutex> lock(records_mu_);
            const auto it = records_.find(RecordMapKey(input.workspace_id, hit.memory_id));
            if (it != records_.end()) {
                expired = IsExpired(it->second, now_ms);
            }
        }
        if (expired) {
            continue;
        }

        out.hits.push_back(hit);
        used_tokens += hit_tokens;
    }

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

    out.debug_stats = retrieved.debug_stats;

    query_cache_total_.fetch_add(1, std::memory_order_acq_rel);
    dense_probe_query_count_.fetch_add(1, std::memory_order_acq_rel);
    dense_probe_count_total_.fetch_add(out.debug_stats.dense_candidates, std::memory_order_acq_rel);

    metrics_.RecordFallback(false);

    const auto end  = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    metrics_.RecordReadLatency(ms);

    return out;
}

bool MemoryEngine::Pin(const std::string& workspace_id, const std::string& memory_id, bool pin, std::string* error) {
    MemoryRecord record;
    if (!LoadRecord(workspace_id, memory_id, &record, error)) {
        return false;
    }

    if (pin && !record.pinned) {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        const auto pin_stats = ComputeWorkspacePinStats(records_, workspace_id);
        if (pin_stats.pinned + 1 > options_.pin_quota_per_workspace) {
            if (error != nullptr) {
                *error = "pin quota exceeded";
            }
            return false;
        }
        if (IsPinBlockedByRatio(pin_stats.pinned + 1, pin_stats.active + 1, options_.max_pinned_ratio)) {
            if (error != nullptr) {
                *error = "pin ratio exceeded";
            }
            return false;
        }
    }

    record.pinned        = pin;
    record.updated_at_ms = util::NowMs();
    ++record.version;
    record.importance_score = pin ? 100.0 : std::min(record.importance_score, 5.0);
    if (pin && options_.pin_ttl_override_s > 0) {
        record.ttl_s = options_.pin_ttl_override_s;
    }
    record.size_bytes = ComputeRecordSizeBytes(record);
    record.node_id    = node_id_;

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

    if (!PersistBatchNow(writes, error)) {
        return false;
    }

    UpsertRecordInMemory(record);
    EnforceResidentHardLimit();
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
    uint64_t vector_count    = 0;
    uint64_t active_count    = 0;
    uint64_t pinned_count    = 0;
    bool has_evictable       = false;
    std::unordered_set<std::string> term_set;
    std::unordered_set<std::string> workspace_set;

    {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        for (const auto& kv : records_) {
            const MemoryRecord& rec = kv.second;
            if (!workspace_id.empty() && rec.workspace_id != workspace_id) {
                continue;
            }
            workspace_set.insert(rec.workspace_id);
            ++item_count;
            if (rec.tombstone) {
                ++tombstone_count;
            } else {
                ++vector_count;
                ++active_count;
                if (rec.pinned) {
                    ++pinned_count;
                } else {
                    has_evictable = true;
                }
                for (const auto& tok : util::Tokenize(rec.content)) {
                    term_set.insert(tok);
                }
            }
        }
    }

    out.item_count                 = item_count;
    out.tombstone_count            = tombstone_count;
    out.gc_last_run_ms             = gc_last_run_ms_.load(std::memory_order_acquire);
    out.gc_evicted_count           = gc_evicted_count_.load(std::memory_order_acquire);
    const bool pin_blocked         = (active_count > 0 && !has_evictable &&
                              PinnedRatio(static_cast<size_t>(pinned_count), static_cast<size_t>(active_count)) >=
                                  options_.max_pinned_ratio);
    out.capacity_blocked           = capacity_blocked_.load(std::memory_order_acquire) || pin_blocked;
    out.write_ack_mode             = "durable";
    out.effective_backend          = options_.effective_backend;
    out.resident_used_bytes        = resident_used_bytes_.load(std::memory_order_acquire);
    out.resident_limit_bytes       = ResidentLimitBytes();
    out.resident_evicted_count     = resident_evicted_count_.load(std::memory_order_acquire);
    out.disk_fallback_search_count = disk_fallback_search_count_.load(std::memory_order_acquire);

    out.index_stats.segment_count = workspace_set.empty() ? 0 : workspace_set.size();
    out.index_stats.posting_terms = term_set.size();
    out.index_stats.vector_count  = vector_count;

    const uint64_t cache_total = query_cache_total_.load(std::memory_order_acquire);
    const uint64_t cache_hit   = query_cache_hit_.load(std::memory_order_acquire);
    if (cache_total > 0) {
        out.index_stats.query_cache_hit_rate = static_cast<double>(cache_hit) / static_cast<double>(cache_total);
    }

    const uint64_t dense_queries = dense_probe_query_count_.load(std::memory_order_acquire);
    const uint64_t dense_total   = dense_probe_count_total_.load(std::memory_order_acquire);
    if (dense_queries > 0) {
        out.index_stats.dense_probe_count_p95 = static_cast<double>(dense_total) / static_cast<double>(dense_queries);
    }

    out.index_stats.cold_rehydrate_count = cold_rehydrate_count_.load(std::memory_order_acquire);
    return out;
}

CompactOutput MemoryEngine::Compact(const std::string& workspace_id) { return CompactInternal(workspace_id, true); }

StoreCompactTriggerResult MemoryEngine::StoreCompact() { return store_->TriggerStoreCompactAsync(); }

bool MemoryEngine::PersistRecord(const MemoryRecord& record, std::vector<WriteEntry>* out_writes,
                                 std::string* error) const {
    if (out_writes == nullptr) {
        if (error != nullptr) {
            *error = "null write list";
        }
        return false;
    }

    const std::string doc_key = BuildDocKey(record.workspace_id, record.id);
    out_writes->push_back(
        WriteEntry{WriteOp::Upsert, KeyValueEntry{kDocNs, doc_key, SerializeRecord(record), record.updated_at_ms}});

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

    auto delete_terms_for = [&](const MemoryRecord& rec) {
        std::unordered_set<std::string> old_terms;
        for (const auto& tok : util::Tokenize(rec.content)) {
            old_terms.insert(tok);
        }
        for (const auto& term : old_terms) {
            const uint64_t bucket = BucketForPosting(rec.workspace_id, term);
            out_writes->push_back(WriteEntry{
                WriteOp::Delete,
                KeyValueEntry{
                    kPostingBlkNs, BuildPostingKey(rec.workspace_id, term, bucket, rec.id), {}, record.updated_at_ms}});
        }
    };

    if (has_old_record) {
        delete_terms_for(old_record);
        out_writes->push_back(WriteEntry{
            WriteOp::Delete,
            KeyValueEntry{kVecCodeNs,
                          BuildVecCodeKey(old_record.workspace_id,
                                          BucketForPosting(old_record.workspace_id, old_record.id), old_record.id),
                          {},
                          record.updated_at_ms}});
        out_writes->push_back(WriteEntry{
            WriteOp::Delete,
            KeyValueEntry{kVecFpNs, BuildVecFpKey(old_record.workspace_id, old_record.id), {}, record.updated_at_ms}});
    }

    if (record.tombstone) {
        delete_terms_for(record);
        out_writes->push_back(WriteEntry{
            WriteOp::Delete, KeyValueEntry{kVecCodeNs,
                                           BuildVecCodeKey(record.workspace_id,
                                                           BucketForPosting(record.workspace_id, record.id), record.id),
                                           {},
                                           record.updated_at_ms}});
        out_writes->push_back(WriteEntry{
            WriteOp::Delete,
            KeyValueEntry{kVecFpNs, BuildVecFpKey(record.workspace_id, record.id), {}, record.updated_at_ms}});
        return true;
    }

    const auto embedding      = BuildEmbedding(record.content, 64);
    const uint64_t vec_bucket = EncodeVectorBucketSignature(embedding) % 256ull;

    util::Json vec_code;
    vec_code.put("model_id", "hash-embed-v1");
    vec_code.put("bucket", vec_bucket);
    vec_code.put("dim", 64);
    vec_code.put("updated_at_ms", record.updated_at_ms);

    out_writes->push_back(WriteEntry{
        WriteOp::Upsert, KeyValueEntry{kVecCodeNs, BuildVecCodeKey(record.workspace_id, vec_bucket, record.id),
                                       util::ToJsonString(vec_code, false), record.updated_at_ms}});

    util::Json vec_fp;
    vec_fp.put("model_id", "hash-embed-v1");
    vec_fp.put("dim", 64);
    vec_fp.put("updated_at_ms", record.updated_at_ms);
    vec_fp.put("vector_csv", EncodeVectorCsv(embedding));
    out_writes->push_back(
        WriteEntry{WriteOp::Upsert, KeyValueEntry{kVecFpNs, BuildVecFpKey(record.workspace_id, record.id),
                                                  util::ToJsonString(vec_fp, false), record.updated_at_ms}});

    const auto tokens = util::Tokenize(record.content);
    std::unordered_map<std::string, int> tf;
    for (const auto& token : tokens) {
        ++tf[token];
    }

    for (const auto& kv : tf) {
        util::Json term_dict;
        term_dict.put("df", 1);
        term_dict.put("cf", kv.second);
        term_dict.put("max_tf", kv.second);
        term_dict.put("updated_at", record.updated_at_ms);
        term_dict.add_child(
            "block_refs",
            util::MakeArray({BuildPostingKey(record.workspace_id, kv.first,
                                             BucketForPosting(record.workspace_id, kv.first), record.id)}));

        out_writes->push_back(
            WriteEntry{WriteOp::Upsert, KeyValueEntry{kTermDictNs, BuildTermDictKey(record.workspace_id, kv.first),
                                                      util::ToJsonString(term_dict, false), record.updated_at_ms}});

        util::Json posting;
        posting.put("doc_id", record.id);
        posting.put("tf", kv.second);
        posting.put("doc_len", tokens.size());
        posting.put("flags", record.pinned ? "pinned" : "normal");
        posting.put("updated_at", record.updated_at_ms);

        const uint64_t bucket = BucketForPosting(record.workspace_id, kv.first);
        out_writes->push_back(
            WriteEntry{WriteOp::Upsert,
                       KeyValueEntry{kPostingBlkNs, BuildPostingKey(record.workspace_id, kv.first, bucket, record.id),
                                     util::ToJsonString(posting, false), record.updated_at_ms}});
    }

    return true;
}

void MemoryEngine::UpsertRecordInMemory(const MemoryRecord& record) {
    const std::string map_key = RecordMapKey(record.workspace_id, record.id);
    const uint64_t new_charge = EstimateResidentCharge(record);

    uint64_t old_charge = 0;
    {
        std::unique_lock<std::shared_mutex> lock(records_mu_);
        const auto charge_it = resident_charge_bytes_.find(map_key);
        if (charge_it != resident_charge_bytes_.end()) {
            old_charge = charge_it->second;
        }

        records_[map_key] = record;

        if (!record.dedup_key.empty()) {
            dedup_index_[record.workspace_id + ":" + record.dedup_key] = record.id;
        }

        if (new_charge == 0) {
            resident_charge_bytes_.erase(map_key);
        } else {
            resident_charge_bytes_[map_key] = new_charge;
        }
    }

    if (record.tombstone) {
        retriever_->Remove(record.workspace_id, record.id);
    } else {
        retriever_->Index(record);
    }

    if (new_charge >= old_charge) {
        resident_used_bytes_.fetch_add(new_charge - old_charge, std::memory_order_acq_rel);
    } else {
        resident_used_bytes_.fetch_sub(old_charge - new_charge, std::memory_order_acq_rel);
    }
}

void MemoryEngine::RemoveRecordInMemory(const MemoryRecord& record) {
    const std::string map_key = RecordMapKey(record.workspace_id, record.id);

    uint64_t removed_charge = 0;
    bool erased             = false;
    {
        std::unique_lock<std::shared_mutex> lock(records_mu_);
        const auto it = records_.find(map_key);
        if (it != records_.end()) {
            if (!it->second.dedup_key.empty()) {
                dedup_index_.erase(record.workspace_id + ":" + it->second.dedup_key);
            }
            records_.erase(it);
            erased = true;
        }

        const auto charge_it = resident_charge_bytes_.find(map_key);
        if (charge_it != resident_charge_bytes_.end()) {
            removed_charge = charge_it->second;
            resident_charge_bytes_.erase(charge_it);
        }
    }

    if (!erased) {
        return;
    }

    if (removed_charge > 0) {
        resident_used_bytes_.fetch_sub(removed_charge, std::memory_order_acq_rel);
    }
    retriever_->Remove(record.workspace_id, record.id);
}

uint64_t MemoryEngine::EstimateResidentCharge(const MemoryRecord& record) const {
    const uint64_t record_bytes = (record.size_bytes > 0) ? record.size_bytes : ComputeRecordSizeBytes(record);
    return record_bytes + retriever_->EstimateDocBytes(record);
}

uint64_t MemoryEngine::ResidentLimitBytes() const { return BytesFromMb(options_.mem_budget_mb); }

bool MemoryEngine::SelectEvictionCandidateLocked(uint64_t now_ms, const std::unordered_set<std::string>* protected_keys,
                                                 MemoryRecord* out) const {
    if (out == nullptr || records_.empty()) {
        return false;
    }

    auto pick_candidate = [&](bool allow_protected) {
        bool has_candidate = false;
        MemoryRecord candidate;
        double best_rank = 0.0;
        for (const auto& kv : records_) {
            const MemoryRecord& rec = kv.second;
            if (rec.pinned) {
                continue;
            }
            if (!allow_protected && protected_keys != nullptr &&
                protected_keys->count(RecordMapKey(rec.workspace_id, rec.id)) > 0) {
                continue;
            }
            const double rank = EvictionRank(rec, now_ms);
            if (!has_candidate || rank < best_rank ||
                (rank == best_rank && rec.updated_at_ms < candidate.updated_at_ms)) {
                has_candidate = true;
                best_rank     = rank;
                candidate     = rec;
            }
        }
        if (!has_candidate) {
            return false;
        }
        *out = std::move(candidate);
        return true;
    };

    if (pick_candidate(false)) {
        return true;
    }
    if (protected_keys != nullptr && !protected_keys->empty()) {
        return pick_candidate(true);
    }
    return false;
}

void MemoryEngine::EnforceResidentHardLimit(const std::unordered_set<std::string>* protected_keys) {
    const uint64_t limit = ResidentLimitBytes();
    if (limit == 0) {
        return;
    }

    while (resident_used_bytes_.load(std::memory_order_acquire) > limit) {
        MemoryRecord victim;
        uint64_t victim_charge = 0;
        bool found             = false;

        {
            std::unique_lock<std::shared_mutex> lock(records_mu_);
            if (resident_used_bytes_.load(std::memory_order_acquire) <= limit || records_.empty()) {
                break;
            }
            if (!SelectEvictionCandidateLocked(util::NowMs(), protected_keys, &victim)) {
                break;
            }

            const std::string map_key = RecordMapKey(victim.workspace_id, victim.id);
            const auto it             = records_.find(map_key);
            if (it == records_.end()) {
                continue;
            }
            records_.erase(it);

            const auto charge_it = resident_charge_bytes_.find(map_key);
            if (charge_it != resident_charge_bytes_.end()) {
                victim_charge = charge_it->second;
                resident_charge_bytes_.erase(charge_it);
            }
            found = true;
        }

        if (!found) {
            break;
        }

        if (victim_charge > 0) {
            resident_used_bytes_.fetch_sub(victim_charge, std::memory_order_acq_rel);
        }
        retriever_->Remove(victim.workspace_id, victim.id);
        resident_evicted_count_.fetch_add(1, std::memory_order_acq_rel);
    }
}

bool MemoryEngine::AppendProjectionEvent(const MemoryRecord& record, OpType op_type, const std::string& reason,
                                         std::vector<WriteEntry>* out_writes, std::string* out_event_key) {
    if (out_writes == nullptr) {
        return false;
    }

    const uint64_t now_ms       = std::max(record.updated_at_ms, util::NowMs());
    const uint64_t seq          = local_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string workspace = record.workspace_id.empty() ? "default" : record.workspace_id;
    const std::string event_key = BuildEventKey(workspace, now_ms, seq);

    util::Json event;
    event.put("event_key", event_key);
    event.put("workspace_id", workspace);
    event.put("memory_id", record.id);
    event.put("reason", reason);
    switch (op_type) {
        case OpType::Pin:
            event.put("op_type", "pin");
            break;
        case OpType::Delete:
            event.put("op_type", "delete");
            break;
        case OpType::Upsert:
        default:
            event.put("op_type", "upsert");
            break;
    }
    event.put("updated_at_ms", now_ms);
    event.put("version", record.version);
    event.put("node_id", node_id_);
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
        KeyValueEntry{kProjectionCkptNs, BuildCkptKey(workspace_id), util::ToJsonString(ckpt, false), ts},
    });
}

bool MemoryEngine::ReplayProjectionEvents(std::string* error) {
    std::unordered_map<std::string, std::string> checkpoints;

    {
        std::string ckpt_scan_error;
        auto ckpt_entries = store_->Scan(kProjectionCkptNs, "", "", 0, &ckpt_scan_error);
        if (!ckpt_scan_error.empty()) {
            if (!IsMissingScanNamespaceError(ckpt_scan_error)) {
                if (error != nullptr) {
                    *error = ckpt_scan_error;
                }
                return false;
            }
            ckpt_entries.clear();
        }

        for (const auto& entry : ckpt_entries) {
            util::Json json;
            std::string parse_error;
            if (!util::ParseJson(entry.value, &json, &parse_error)) {
                continue;
            }
            std::string workspace = util::GetStringOr(json, "workspace_id", "");
            if (workspace.empty()) {
                const std::string prefix = "ws/";
                if (entry.key.rfind(prefix, 0) == 0) {
                    workspace = entry.key.substr(prefix.size());
                }
            }
            const std::string event_key = util::GetStringOr(json, "event_key", "");
            if (!workspace.empty() && !event_key.empty()) {
                checkpoints[workspace] = event_key;
            }
        }
    }

    std::string events_scan_error;
    auto events = store_->Scan(kEventsNs, "", "", 0, &events_scan_error);
    if (!events_scan_error.empty()) {
        if (!IsMissingScanNamespaceError(events_scan_error)) {
            if (error != nullptr) {
                *error = events_scan_error;
            }
            return false;
        }
        return true;
    }

    for (const auto& event : events) {
        util::Json event_json;
        std::string parse_error;
        if (!util::ParseJson(event.value, &event_json, &parse_error)) {
            continue;
        }

        std::string workspace = util::GetStringOr(event_json, "workspace_id", "");
        if (workspace.empty()) {
            workspace = WorkspaceFromEventKey(event.key);
        }
        if (workspace.empty()) {
            continue;
        }

        const auto ckpt_it = checkpoints.find(workspace);
        if (ckpt_it != checkpoints.end() && event.key <= ckpt_it->second) {
            continue;
        }

        MemoryRecord incoming;
        std::string decode_error;
        const std::string payload = util::GetStringOr(event_json, "record_payload", "");
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

        bool should_apply = true;
        {
            std::shared_lock<std::shared_mutex> lock(records_mu_);
            const auto it = records_.find(RecordMapKey(incoming.workspace_id, incoming.id));
            if (it != records_.end()) {
                should_apply = IsIncomingNewer(it->second, incoming);
            }
        }

        if (!should_apply) {
            continue;
        }

        if (incoming.size_bytes == 0) {
            incoming.size_bytes = ComputeRecordSizeBytes(incoming);
        }
        if (incoming.last_access_ms == 0) {
            incoming.last_access_ms = incoming.updated_at_ms;
        }
        if (incoming.tier.empty()) {
            incoming.tier = incoming.tombstone ? "cold" : "hot";
        }

        UpsertRecordInMemory(incoming);
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

    const auto get_res = store_->Get(kDocNs, BuildDocKey(workspace_id, memory_id));
    if (!get_res.found) {
        if (error != nullptr) {
            *error = get_res.error.empty() ? "record not found" : get_res.error;
        }
        return false;
    }

    return DeserializeRecord(get_res.value, out, error);
}

bool MemoryEngine::DeleteRecordStorage(const MemoryRecord& record, std::vector<WriteEntry>* out_writes) const {
    if (out_writes == nullptr) {
        return false;
    }

    const auto tokens = util::Tokenize(record.content);
    std::unordered_set<std::string> terms(tokens.begin(), tokens.end());

    out_writes->push_back(WriteEntry{
        WriteOp::Delete, KeyValueEntry{kDocNs, BuildDocKey(record.workspace_id, record.id), {}, util::NowMs()}});

    out_writes->push_back(WriteEntry{
        WriteOp::Delete,
        KeyValueEntry{kVecCodeNs,
                      BuildVecCodeKey(record.workspace_id, BucketForPosting(record.workspace_id, record.id), record.id),
                      {},
                      util::NowMs()},
    });

    out_writes->push_back(WriteEntry{
        WriteOp::Delete,
        KeyValueEntry{kVecFpNs, BuildVecFpKey(record.workspace_id, record.id), {}, util::NowMs()},
    });

    for (const auto& term : terms) {
        out_writes->push_back(WriteEntry{
            WriteOp::Delete, KeyValueEntry{kPostingBlkNs,
                                           BuildPostingKey(record.workspace_id, term,
                                                           BucketForPosting(record.workspace_id, term), record.id),
                                           {},
                                           util::NowMs()}});
    }

    return true;
}

std::string MemoryEngine::RecordMapKey(const std::string& workspace_id, const std::string& memory_id) {
    return workspace_id + ":" + memory_id;
}

bool MemoryEngine::IsOverHighWatermark(const StoreUsage& usage) const {
    const uint64_t max_disk = BytesFromGb(options_.disk_budget_gb);
    if (max_disk == 0) {
        return false;
    }
    return static_cast<double>(usage.disk_used_bytes) / static_cast<double>(max_disk) >= options_.gc_high_watermark;
}

bool MemoryEngine::IsBelowLowWatermark(const StoreUsage& usage) const {
    const uint64_t max_disk = BytesFromGb(options_.disk_budget_gb);
    if (max_disk == 0) {
        return true;
    }
    return static_cast<double>(usage.disk_used_bytes) / static_cast<double>(max_disk) <= options_.gc_low_watermark;
}

void MemoryEngine::MaybeScheduleGc() {
    if (!gc_running_.load(std::memory_order_acquire)) {
        return;
    }

    std::string usage_error;
    const StoreUsage usage = store_->ApproximateUsage(&usage_error);
    (void)usage_error;
    if (IsOverHighWatermark(usage)) {
        gc_requested_.store(true, std::memory_order_release);
        gc_cv_.notify_one();
    }
}

void MemoryEngine::RunGcLoop() {
    while (!gc_stop_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(gc_mu_);
        gc_cv_.wait_for(lock, std::chrono::seconds(30), [&]() {
            return gc_stop_.load(std::memory_order_acquire) || gc_requested_.load(std::memory_order_acquire);
        });
        if (gc_stop_.load(std::memory_order_acquire)) {
            return;
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
    out.segments_before   = 1;

    const bool over_high = IsOverHighWatermark(usage);
    if (!force && !over_high) {
        out.mem_after_bytes  = usage.mem_used_bytes;
        out.disk_after_bytes = usage.disk_used_bytes;
        out.segments_after   = out.segments_before;
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

    std::sort(active_records.begin(), active_records.end(), [&](const MemoryRecord& lhs, const MemoryRecord& rhs) {
        const double lhs_rank = EvictionRank(lhs, now_ms);
        const double rhs_rank = EvictionRank(rhs, now_ms);
        if (lhs_rank == rhs_rank) {
            return lhs.updated_at_ms < rhs.updated_at_ms;
        }
        return lhs_rank < rhs_rank;
    });

    uint32_t actions = 0;
    for (const auto& candidate : active_records) {
        if (actions >= max_batch) {
            break;
        }
        if (candidate.pinned || candidate.tombstone) {
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

        std::string persist_error;
        if (!PersistRecord(tomb, &writes, &persist_error)) {
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

    if (!writes.empty()) {
        const auto batch_res = store_->BatchWrite(writes);
        if (batch_res.ok) {
            for (const auto& rec : upserted_records) {
                UpsertRecordInMemory(rec);
            }
            for (const auto& rec : physically_deleted_records) {
                RemoveRecordInMemory(rec);
                ++out.postings_reclaimed;
                ++out.vectors_reclaimed;
            }
            if (out.deleted_count > 0) {
                gc_evicted_count_.fetch_add(out.deleted_count, std::memory_order_acq_rel);
            }
        }
    }

    std::string post_usage_error;
    usage = store_->ApproximateUsage(&post_usage_error);
    (void)post_usage_error;

    out.mem_after_bytes  = usage.mem_used_bytes;
    out.disk_after_bytes = usage.disk_used_bytes;
    out.segments_after   = 1;
    gc_last_run_ms_.store(now_ms, std::memory_order_release);

    bool has_evictable    = false;
    bool has_active       = false;
    uint64_t pinned_count = 0;
    uint64_t active_count = 0;
    {
        std::shared_lock<std::shared_mutex> lock(records_mu_);
        for (const auto& kv : records_) {
            const auto& rec = kv.second;
            if (!workspace_id.empty() && rec.workspace_id != workspace_id) {
                continue;
            }
            if (rec.tombstone) {
                continue;
            }
            has_active = true;
            ++active_count;
            if (!rec.pinned) {
                has_evictable = true;
            } else {
                ++pinned_count;
            }
        }
    }

    const bool pin_blocked =
        has_active && !has_evictable &&
        PinnedRatio(static_cast<size_t>(pinned_count), static_cast<size_t>(active_count)) >= options_.max_pinned_ratio;
    const bool blocked = (IsOverHighWatermark(usage) && has_active && !has_evictable) || pin_blocked;
    capacity_blocked_.store(blocked, std::memory_order_release);
    out.capacity_blocked = blocked;

    return out;
}

bool MemoryEngine::IsExpired(const MemoryRecord& record, uint64_t now_ms) const {
    if (record.ttl_s == 0 || record.updated_at_ms == 0) {
        return false;
    }
    const uint64_t ttl_ms = record.ttl_s * 1000ull;
    return now_ms > record.updated_at_ms && (now_ms - record.updated_at_ms) >= ttl_ms;
}

std::string MemoryEngine::NextMemoryId() {
    const uint64_t now_ms = util::NowMs();
    const uint64_t seq    = local_seq_.fetch_add(1, std::memory_order_acq_rel);
    return "m-" + ZeroPadUint64(now_ms) + "-" + ZeroPadUint64(seq);
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
    json.put("ttl_s", record.ttl_s);
    json.put("dedup_key", record.dedup_key);
    json.put("shard_id", record.shard_id);
    json.put("replica_role", record.replica_role);
    json.put("routing_epoch", record.routing_epoch);
    json.put("shard_hint", record.shard_hint);

    json.add_child("tags", util::MakeArray(record.tags));

    util::Json metadata;
    for (const auto& kv : record.metadata) {
        metadata.put(kv.first, kv.second);
    }
    json.add_child("metadata", metadata);

    return util::ToJsonString(json, false);
}

bool MemoryEngine::DeserializeRecord(const std::string& text, MemoryRecord* record, std::string* error) {
    if (record == nullptr) {
        if (error != nullptr) {
            *error = "null record";
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
    record->created_at_ms    = GetUint64OrSafe(json, "created_at_ms", 0);
    record->updated_at_ms    = GetUint64OrSafe(json, "updated_at_ms", 0);
    record->version          = GetUint64OrSafe(json, "version", 0);
    record->size_bytes       = GetUint64OrSafe(json, "size_bytes", 0);
    record->last_access_ms   = GetUint64OrSafe(json, "last_access_ms", 0);
    record->hit_count        = GetUint64OrSafe(json, "hit_count", 0);
    record->importance_score = GetDoubleOr(json, "importance_score", 1.0);
    record->tier             = util::GetStringOr(json, "tier", "hot");
    record->node_id          = util::GetStringOr(json, "node_id", "");
    record->ttl_s            = GetUint64OrSafe(json, "ttl_s", 0);
    record->dedup_key        = util::GetStringOr(json, "dedup_key", "");
    record->shard_id         = util::GetStringOr(json, "shard_id", "");
    record->replica_role     = util::GetStringOr(json, "replica_role", "");
    record->routing_epoch    = GetUint64OrSafe(json, "routing_epoch", 0);
    record->shard_hint       = util::GetStringOr(json, "shard_hint", "");

    record->tags = util::ReadStringArray(json, "tags");
    record->metadata.clear();
    if (const auto metadata_opt = json.get_child_optional("metadata")) {
        for (const auto& kv : *metadata_opt) {
            record->metadata[kv.first] = kv.second.get_value<std::string>();
        }
    }

    if (record->size_bytes == 0) {
        record->size_bytes = ComputeRecordSizeBytes(*record);
    }

    return true;
}

}  // namespace pgmem::core
