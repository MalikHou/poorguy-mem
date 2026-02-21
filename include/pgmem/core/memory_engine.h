#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pgmem/config/defaults.h"
#include "pgmem/core/metrics.h"
#include "pgmem/core/retriever.h"
#include "pgmem/store/store_adapter.h"
#include "pgmem/types.h"

namespace pgmem::core {

struct CompactOutput {
    bool triggered{false};
    bool capacity_blocked{false};
    uint64_t mem_before_bytes{0};
    uint64_t disk_before_bytes{0};
    uint64_t mem_after_bytes{0};
    uint64_t disk_after_bytes{0};
    uint64_t summarized_count{0};
    uint64_t tombstoned_count{0};
    uint64_t deleted_count{0};

    uint64_t segments_before{0};
    uint64_t segments_after{0};
    uint64_t postings_reclaimed{0};
    uint64_t vectors_reclaimed{0};
};

struct MemoryEngineOptions {
    uint64_t mem_budget_mb{config::kDefaultMemBudgetMb};
    uint64_t disk_budget_gb{config::kDefaultDiskBudgetGb};

    double gc_high_watermark{config::kDefaultGcHighWatermark};
    double gc_low_watermark{config::kDefaultGcLowWatermark};
    uint32_t gc_batch_size{config::kDefaultGcBatchSize};

    uint64_t max_record_bytes{config::kDefaultMaxRecordBytes};
    bool enable_tombstone_gc{config::kDefaultEnableTombstoneGc};
    uint64_t tombstone_retention_ms{24ull * 60ull * 60ull * 1000ull};

    double max_pinned_ratio{0.50};
    uint64_t pin_ttl_override_s{0};
    uint32_t pin_quota_per_workspace{10000};

    std::string effective_backend;
};

class MemoryEngine {
public:
    MemoryEngine(std::unique_ptr<store::IStoreAdapter> store, std::unique_ptr<IRetriever> retriever,
                 std::string node_id, MemoryEngineOptions options = {});
    ~MemoryEngine();

    void Shutdown();
    bool Warmup(std::string* error);

    WriteOutput Write(const WriteInput& input);
    QueryOutput Query(const QueryInput& input);
    bool Pin(const std::string& workspace_id, const std::string& memory_id, bool pin, std::string* error);
    StatsSnapshot Stats(const std::string& workspace_id, const std::string& window) const;

    CompactOutput Compact(const std::string& workspace_id);
    StoreCompactTriggerResult StoreCompact();

private:
    bool PersistBatchNow(const std::vector<WriteEntry>& writes, std::string* error);
    bool PersistRecord(const MemoryRecord& record, std::vector<WriteEntry>* out_writes, std::string* error) const;
    bool DeleteRecordStorage(const MemoryRecord& record, std::vector<WriteEntry>* out_writes) const;

    bool AppendProjectionEvent(const MemoryRecord& record, OpType op_type, const std::string& reason,
                               std::vector<WriteEntry>* out_writes, std::string* out_event_key);
    void AppendProjectionCheckpoint(const std::string& workspace_id, const std::string& event_key, uint64_t ts,
                                    std::vector<WriteEntry>* out_writes) const;
    bool ReplayProjectionEvents(std::string* error);

    bool LoadRecord(const std::string& workspace_id, const std::string& memory_id, MemoryRecord* out,
                    std::string* error) const;

    static std::string RecordMapKey(const std::string& workspace_id, const std::string& memory_id);

    static std::string SerializeRecord(const MemoryRecord& record);
    static bool DeserializeRecord(const std::string& text, MemoryRecord* record, std::string* error);

    std::string NextMemoryId();

    bool IsOverHighWatermark(const StoreUsage& usage) const;
    bool IsBelowLowWatermark(const StoreUsage& usage) const;

    uint64_t ResidentLimitBytes() const;
    uint64_t EstimateResidentCharge(const MemoryRecord& record) const;

    void UpsertRecordInMemory(const MemoryRecord& record);
    void RemoveRecordInMemory(const MemoryRecord& record);

    bool SelectEvictionCandidateLocked(uint64_t now_ms, const std::unordered_set<std::string>* protected_keys,
                                       MemoryRecord* out) const;
    void EnforceResidentHardLimit(const std::unordered_set<std::string>* protected_keys = nullptr);

    void MaybeScheduleGc();
    void RunGcLoop();
    CompactOutput CompactInternal(const std::string& workspace_id, bool force);

    bool IsExpired(const MemoryRecord& record, uint64_t now_ms) const;

    std::unique_ptr<store::IStoreAdapter> store_;
    std::unique_ptr<IRetriever> retriever_;
    Metrics metrics_;
    MemoryEngineOptions options_;
    std::string node_id_;

    std::atomic<uint64_t> local_seq_{0};
    std::atomic<uint64_t> index_generation_{0};

    mutable std::shared_mutex records_mu_;
    std::unordered_map<std::string, MemoryRecord> records_;
    std::unordered_map<std::string, std::string> dedup_index_;

    std::unordered_map<std::string, uint64_t> resident_charge_bytes_;
    std::atomic<uint64_t> resident_used_bytes_{0};
    std::atomic<uint64_t> resident_evicted_count_{0};
    std::atomic<uint64_t> disk_fallback_search_count_{0};
    std::atomic<uint64_t> cold_rehydrate_count_{0};
    std::atomic<uint64_t> dense_probe_count_total_{0};
    std::atomic<uint64_t> dense_probe_query_count_{0};
    std::atomic<uint64_t> query_cache_hit_{0};
    std::atomic<uint64_t> query_cache_total_{0};

    std::atomic<bool> gc_running_{false};
    std::atomic<bool> gc_stop_{false};
    std::atomic<bool> gc_requested_{false};
    mutable std::mutex gc_mu_;
    mutable std::condition_variable gc_cv_;
    std::thread gc_thread_;

    std::atomic<uint64_t> gc_last_run_ms_{0};
    std::atomic<uint64_t> gc_evicted_count_{0};
    std::atomic<bool> capacity_blocked_{false};

    std::once_flag shutdown_once_;
};

}  // namespace pgmem::core
