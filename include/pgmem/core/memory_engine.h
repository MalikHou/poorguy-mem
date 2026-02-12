#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pgmem/config/defaults.h"
#include "pgmem/core/metrics.h"
#include "pgmem/core/retriever.h"
#include "pgmem/core/summary.h"
#include "pgmem/store/store_adapter.h"
#include "pgmem/types.h"

namespace pgmem::core {

enum class WriteAckMode {
    Durable,
    Accepted,
};

WriteAckMode ParseWriteAckMode(const std::string& value);
const char* WriteAckModeToString(WriteAckMode mode);

struct BootstrapOutput {
    std::string summary;
    std::vector<SearchHit> recalled_items;
    int estimated_tokens_saved{0};
};

struct CommitTurnOutput {
    std::vector<std::string> stored_ids;
    std::string summary_updated;
};

struct SearchOutput {
    std::vector<SearchHit> hits;
};

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
    WriteAckMode write_ack_mode{WriteAckMode::Accepted};
    uint32_t volatile_flush_interval_ms{config::kDefaultVolatileFlushIntervalMs};
    uint32_t volatile_max_pending_ops{config::kDefaultVolatileMaxPendingOps};
    uint32_t shutdown_drain_timeout_ms{config::kDefaultShutdownDrainTimeoutMs};
    std::string effective_backend;
};

class MemoryEngine {
public:
    MemoryEngine(std::unique_ptr<store::IStoreAdapter> store, std::unique_ptr<IRetriever> retriever,
                 std::string node_id, MemoryEngineOptions options = {});
    ~MemoryEngine();

    // Allows explicit shutdown in tests before destructor runs.
    void Shutdown();

    bool Warmup(std::string* error);

    BootstrapOutput Bootstrap(const BootstrapInput& input);
    CommitTurnOutput CommitTurn(const CommitTurnInput& input);
    SearchOutput Search(const SearchInput& input);
    CompactOutput Compact(const std::string& workspace_id);
    bool Pin(const std::string& workspace_id, const std::string& memory_id, bool pin, std::string* error);
    StatsSnapshot Stats(const std::string& workspace_id, const std::string& window) const;

private:
    struct VolatileWriteBatch {
        std::vector<WriteEntry> writes;
    };

    bool PersistBatchNow(const std::vector<WriteEntry>& writes, std::string* error);
    bool EnqueueOrPersistBatch(std::vector<WriteEntry> writes, std::string* error);
    bool EnqueueVolatileBatch(std::vector<WriteEntry> writes);
    bool TryPopVolatileBatch(VolatileWriteBatch* out_batch);
    bool RequeueVolatileFront(VolatileWriteBatch* batch);
    void RunVolatileWriter();
    void StopVolatileWriter();

    void UpsertRecordInMemory(const MemoryRecord& record);
    void RemoveRecordInMemory(const MemoryRecord& record);

    bool PersistRecord(const MemoryRecord& record, std::vector<WriteEntry>* out_writes, std::string* error) const;
    bool LoadRecord(const std::string& workspace_id, const std::string& memory_id, MemoryRecord* out,
                    std::string* error) const;
    bool DeleteRecordStorage(const MemoryRecord& record, std::vector<WriteEntry>* out_writes) const;
    bool AppendProjectionEvent(const MemoryRecord& record, OpType op_type, const std::string& reason,
                               std::vector<WriteEntry>* out_writes, std::string* out_event_key);
    void AppendProjectionCheckpoint(const std::string& workspace_id, const std::string& event_key, uint64_t ts,
                                    std::vector<WriteEntry>* out_writes) const;
    bool ReplayProjectionEvents(std::string* error);
    static std::string RecordMapKey(const std::string& workspace_id, const std::string& memory_id);
    bool IsOverHighWatermark(const StoreUsage& usage) const;
    bool IsBelowLowWatermark(const StoreUsage& usage) const;
    void MaybeScheduleGc();
    void RunGcLoop();
    CompactOutput CompactInternal(const std::string& workspace_id, bool force);
    MemoryRecord BuildSummaryRecord(const MemoryRecord& src, uint64_t now_ms);

    std::string NextMemoryId();
    std::string BuildSummaryKey(const std::string& workspace_id, const std::string& session_id) const;

    static std::string SerializeRecord(const MemoryRecord& record);
    static bool DeserializeRecord(const std::string& text, MemoryRecord* record, std::string* error);

    std::unique_ptr<store::IStoreAdapter> store_;
    std::unique_ptr<IRetriever> retriever_;
    SummaryEngine summary_engine_;
    Metrics metrics_;
    MemoryEngineOptions options_;
    std::string node_id_;
    std::atomic<uint64_t> local_seq_{0};
    mutable std::shared_mutex records_mu_;
    std::unordered_map<std::string, MemoryRecord> records_;

    std::atomic<bool> gc_running_{false};
    std::atomic<bool> gc_stop_{false};
    std::atomic<bool> gc_requested_{false};
    mutable std::mutex gc_mu_;
    mutable std::condition_variable gc_cv_;
    std::thread gc_thread_;

    std::atomic<uint64_t> gc_last_run_ms_{0};
    std::atomic<uint64_t> gc_evicted_count_{0};
    std::atomic<bool> capacity_blocked_{false};

    mutable std::mutex volatile_mu_;
    std::condition_variable volatile_cv_;
    std::deque<VolatileWriteBatch> volatile_queue_;
    std::thread volatile_writer_thread_;
    std::atomic<bool> volatile_writer_running_{false};
    std::atomic<bool> volatile_writer_stop_{false};
    std::atomic<uint64_t> pending_write_ops_{0};
    std::atomic<uint64_t> flush_failures_total_{0};
    std::atomic<uint64_t> volatile_dropped_on_shutdown_{0};
    std::string last_flush_error_;
    std::once_flag shutdown_once_;
};

}  // namespace pgmem::core
