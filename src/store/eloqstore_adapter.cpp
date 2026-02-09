#include "pgmem/store/store_adapter.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef PGMEM_WITH_ELOQSTORE
#include "eloq_store.h"
#include "error.h"
#include "kv_options.h"
#endif

namespace pgmem::store {
namespace {

std::string ComposeKey(const std::string& name_space, const std::string& key) {
    return name_space + ":" + key;
}

#ifdef PGMEM_WITH_ELOQSTORE

uint16_t ResolveThreadCount(uint16_t configured) {
    if (configured > 0) {
        return configured;
    }

    const unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) {
        return 1;
    }

    const auto max_u16 = static_cast<unsigned>(std::numeric_limits<uint16_t>::max());
    return static_cast<uint16_t>(std::min(hc, max_u16));
}

class EloqStoreAdapter final : public IStoreAdapter {
public:
    explicit EloqStoreAdapter(const StoreAdapterConfig& config)
        : table_("pgmem", 0) {
        opts_.store_path = {config.root_path};
        opts_.num_threads = ResolveThreadCount(config.num_threads);
        opts_.data_append_mode = config.append_mode;

        if (config.enable_compression) {
            opts_.enable_compression = true;
            if (config.zstd_compression_level != 0) {
                opts_.zstd_compression_level = config.zstd_compression_level;
            }
        }

        if (config.s3.enabled && !config.s3.bucket_path.empty()) {
            opts_.cloud_store_path = config.s3.bucket_path;
            opts_.cloud_provider = config.s3.provider.empty() ? "aws" : config.s3.provider;
            opts_.cloud_region = config.s3.region.empty() ? "us-east-1" : config.s3.region;
            opts_.cloud_verify_ssl = config.s3.verify_ssl;

            if (!config.s3.endpoint.empty()) {
                opts_.cloud_endpoint = config.s3.endpoint;
            }
            if (!config.s3.access_key.empty()) {
                opts_.cloud_access_key = config.s3.access_key;
            }
            if (!config.s3.secret_key.empty()) {
                opts_.cloud_secret_key = config.s3.secret_key;
            }

            // S3 mode keeps hot cache locally while cloud is the durable tier.
            opts_.allow_reuse_local_caches = true;
        }

        store_ = std::make_unique<eloqstore::EloqStore>(opts_);
    }

    bool Start(std::string* error) {
        const eloqstore::KvError err = store_->Start();
        if (err != eloqstore::KvError::NoError) {
            if (error != nullptr) {
                *error = eloqstore::ErrorString(err);
            }
            return false;
        }
        started_.store(true, std::memory_order_release);
        return true;
    }

    ~EloqStoreAdapter() override {
        if (started_.exchange(false, std::memory_order_acq_rel)) {
            store_->Stop();
        }
    }

    StoreResult Put(const std::string& name_space,
                    const std::string& key,
                    const std::string& value,
                    uint64_t ts) override {
        if (!started_.load(std::memory_order_acquire)) {
            return StoreResult{false, "eloqstore is not started"};
        }

        eloqstore::BatchWriteRequest req;
        std::vector<eloqstore::WriteDataEntry> batch;
        batch.emplace_back(ComposeKey(name_space, key), value, ts, eloqstore::WriteOp::Upsert);
        req.SetArgs(table_, std::move(batch));
        store_->ExecSync(&req);
        if (req.Error() != eloqstore::KvError::NoError) {
            return StoreResult{false, eloqstore::ErrorString(req.Error())};
        }
        return StoreResult{true, {}};
    }

    GetResult Get(const std::string& name_space, const std::string& key) override {
        if (!started_.load(std::memory_order_acquire)) {
            return GetResult{false, {}, "eloqstore is not started"};
        }

        eloqstore::ReadRequest req;
        req.SetArgs(table_, ComposeKey(name_space, key));
        store_->ExecSync(&req);
        if (req.Error() == eloqstore::KvError::NotFound) {
            return GetResult{false, {}, {}};
        }
        if (req.Error() != eloqstore::KvError::NoError) {
            return GetResult{false, {}, eloqstore::ErrorString(req.Error())};
        }
        return GetResult{true, req.value_, {}};
    }

    std::vector<KeyValueEntry> Scan(const std::string& name_space,
                                    const std::string& begin,
                                    const std::string& end,
                                    size_t limit,
                                    std::string* error) override {
        std::vector<KeyValueEntry> out;
        if (!started_.load(std::memory_order_acquire)) {
            if (error != nullptr) {
                *error = "eloqstore is not started";
            }
            return out;
        }

        const std::string begin_key = ComposeKey(name_space, begin);
        std::string end_key;
        if (!end.empty()) {
            end_key = ComposeKey(name_space, end);
        } else {
            // Namespace upper-bound sentinel.
            end_key = name_space + ";";
        }

        eloqstore::ScanRequest req;
        req.SetArgs(table_, begin_key, end_key);
        if (limit > 0) {
            req.SetPagination(limit, static_cast<size_t>(1) << 20);
        }
        store_->ExecSync(&req);
        if (req.Error() != eloqstore::KvError::NoError) {
            if (error != nullptr) {
                *error = eloqstore::ErrorString(req.Error());
            }
            return out;
        }

        for (const auto& entry : req.Entries()) {
            const std::string full_key = entry.key_;
            const std::string prefix = name_space + ":";
            if (full_key.rfind(prefix, 0) != 0) {
                continue;
            }
            out.push_back(KeyValueEntry{name_space,
                                        full_key.substr(prefix.size()),
                                        entry.value_,
                                        entry.timestamp_});
        }
        return out;
    }

    StoreResult BatchWrite(const std::vector<KeyValueEntry>& entries) override {
        if (!started_.load(std::memory_order_acquire)) {
            return StoreResult{false, "eloqstore is not started"};
        }

        eloqstore::BatchWriteRequest req;
        std::vector<eloqstore::WriteDataEntry> batch;
        batch.reserve(entries.size());
        for (const auto& entry : entries) {
            batch.emplace_back(ComposeKey(entry.name_space, entry.key),
                               entry.value,
                               entry.ts,
                               eloqstore::WriteOp::Upsert);
        }
        req.SetArgs(table_, std::move(batch));
        store_->ExecSync(&req);
        if (req.Error() != eloqstore::KvError::NoError) {
            return StoreResult{false, eloqstore::ErrorString(req.Error())};
        }
        return StoreResult{true, {}};
    }

private:
    std::atomic<bool> started_{false};
    eloqstore::KvOptions opts_;
    eloqstore::TableIdent table_;
    std::unique_ptr<eloqstore::EloqStore> store_;
};

#endif

}  // namespace

std::unique_ptr<IStoreAdapter> CreateEloqStoreAdapter(const StoreAdapterConfig& config,
                                                      std::string* error) {
#ifdef PGMEM_WITH_ELOQSTORE
    auto adapter = std::make_unique<EloqStoreAdapter>(config);
    if (!adapter->Start(error)) {
        return nullptr;
    }
    return adapter;
#else
    if (error != nullptr) {
        *error = "built without PGMEM_WITH_ELOQSTORE";
    }
    (void)config;
    return nullptr;
#endif
}

}  // namespace pgmem::store
