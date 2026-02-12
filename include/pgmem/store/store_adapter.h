#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pgmem/config/defaults.h"
#include "pgmem/types.h"

namespace pgmem::store {

struct StoreAdapterConfig {
    std::string backend;
    std::string root_path;
    uint16_t num_threads{config::kDefaultStoreThreads};
    uint32_t num_partitions{config::kDefaultStorePartitions};
    bool append_mode{config::kDefaultAppendMode};
    bool enable_compression{config::kDefaultEnableCompression};
    int zstd_compression_level{0};
    uint64_t mem_budget_mb{config::kDefaultMemBudgetMb};
    uint64_t disk_budget_gb{config::kDefaultDiskBudgetGb};
    double gc_high_watermark{config::kDefaultGcHighWatermark};
    double gc_low_watermark{config::kDefaultGcLowWatermark};
    uint32_t gc_batch_size{config::kDefaultGcBatchSize};
    uint64_t max_record_bytes{config::kDefaultMaxRecordBytes};
    bool enable_tombstone_gc{config::kDefaultEnableTombstoneGc};
};

class IStoreAdapter {
public:
    virtual ~IStoreAdapter() = default;

    virtual StoreResult Put(const std::string& name_space, const std::string& key, const std::string& value,
                            uint64_t ts) = 0;

    virtual GetResult Get(const std::string& name_space, const std::string& key) = 0;

    virtual StoreResult Delete(const std::string& name_space, const std::string& key, uint64_t ts) = 0;

    virtual std::vector<KeyValueEntry> Scan(const std::string& name_space, const std::string& begin,
                                            const std::string& end, size_t limit, std::string* error) = 0;

    virtual StoreResult BatchWrite(const std::vector<WriteEntry>& entries) = 0;
    virtual StoreUsage ApproximateUsage(std::string* error)                = 0;
};

std::unique_ptr<IStoreAdapter> CreateInMemoryStoreAdapter();
std::unique_ptr<IStoreAdapter> CreateEloqStoreAdapter(const StoreAdapterConfig& config, std::string* error);
std::unique_ptr<IStoreAdapter> CreateStoreAdapter(const StoreAdapterConfig& config, std::string* error);

}  // namespace pgmem::store
