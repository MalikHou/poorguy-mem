#pragma once

#include <memory>
#include <atomic>
#include <string>
#include <vector>

#include "pgmem/core/metrics.h"
#include "pgmem/core/retriever.h"
#include "pgmem/core/summary.h"
#include "pgmem/core/sync.h"
#include "pgmem/store/store_adapter.h"
#include "pgmem/types.h"

namespace pgmem::core {

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

class MemoryEngine {
public:
    MemoryEngine(std::unique_ptr<store::IStoreAdapter> store,
                 std::unique_ptr<IRetriever> retriever,
                 std::unique_ptr<SyncWorker> sync_worker,
                 std::string node_id);

    bool Warmup(std::string* error);

    BootstrapOutput Bootstrap(const BootstrapInput& input);
    CommitTurnOutput CommitTurn(const CommitTurnInput& input);
    SearchOutput Search(const SearchInput& input);
    bool Pin(const std::string& workspace_id, const std::string& memory_id, bool pin, std::string* error);
    StatsSnapshot Stats(const std::string& workspace_id, const std::string& window) const;

    bool ApplySyncOp(const SyncOp& op, std::string* error);

private:
    bool PersistRecord(const MemoryRecord& record, std::string* error);
    bool LoadRecord(const std::string& workspace_id, const std::string& memory_id, MemoryRecord* out, std::string* error) const;

    std::string NextMemoryId();
    std::string BuildSummaryKey(const std::string& workspace_id, const std::string& session_id) const;

    static std::string SerializeRecord(const MemoryRecord& record);
    static bool DeserializeRecord(const std::string& text, MemoryRecord* record, std::string* error);

    static std::string SerializeSyncOpPayload(const MemoryRecord& record);

    std::unique_ptr<store::IStoreAdapter> store_;
    std::unique_ptr<IRetriever> retriever_;
    std::unique_ptr<SyncWorker> sync_worker_;
    SummaryEngine summary_engine_;
    Metrics metrics_;
    std::string node_id_;
    std::atomic<uint64_t> local_seq_{0};
};

}  // namespace pgmem::core
