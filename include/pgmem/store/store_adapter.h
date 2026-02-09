#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pgmem/types.h"

namespace pgmem::store {

struct S3Config {
    bool enabled{false};
    std::string bucket_path;
    std::string provider{"aws"};
    std::string endpoint;
    std::string region{"us-east-1"};
    std::string access_key;
    std::string secret_key;
    bool verify_ssl{false};
};

struct StoreAdapterConfig {
    std::string backend;
    std::string root_path;
    uint16_t num_threads{0};
    bool append_mode{true};
    bool enable_compression{false};
    int zstd_compression_level{0};
    S3Config s3;
};

class IStoreAdapter {
public:
    virtual ~IStoreAdapter() = default;

    virtual StoreResult Put(const std::string& name_space,
                            const std::string& key,
                            const std::string& value,
                            uint64_t ts) = 0;

    virtual GetResult Get(const std::string& name_space, const std::string& key) = 0;

    virtual std::vector<KeyValueEntry> Scan(const std::string& name_space,
                                            const std::string& begin,
                                            const std::string& end,
                                            size_t limit,
                                            std::string* error) = 0;

    virtual StoreResult BatchWrite(const std::vector<KeyValueEntry>& entries) = 0;
};

std::unique_ptr<IStoreAdapter> CreateInMemoryStoreAdapter();
std::unique_ptr<IStoreAdapter> CreateEloqStoreAdapter(const StoreAdapterConfig& config,
                                                       std::string* error);
std::unique_ptr<IStoreAdapter> CreateStoreAdapter(const StoreAdapterConfig& config,
                                                  std::string* error);

#ifdef PGMEM_HAS_CUSTOM_STORE
std::unique_ptr<IStoreAdapter> CreateCustomStoreAdapter(const StoreAdapterConfig& config,
                                                        std::string* error);
#endif

}  // namespace pgmem::store
