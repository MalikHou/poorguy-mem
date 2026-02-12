#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pgmem/store/store_adapter.h"

#ifdef PGMEM_WITH_ELOQSTORE
#include "eloq_store.h"
#include "error.h"
#include "kv_options.h"
#endif

namespace pgmem::store {
namespace {

std::string ComposeKey(const std::string& name_space, const std::string& key) { return name_space + ":" + key; }

std::string PartitionAffinitySeed(const std::string& name_space, const std::string& key) {
    const size_t pos = key.find(':');
    if (pos == std::string::npos || pos == 0) {
        return name_space + ":" + key;
    }
    return name_space + ":" + key.substr(0, pos);
}

uint64_t DirectorySizeBytes(const std::string& root_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (root_path.empty() || !fs::exists(root_path, ec)) {
        return 0;
    }

    uint64_t total = 0;
    fs::recursive_directory_iterator it(root_path, ec);
    fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        total += static_cast<uint64_t>(it->file_size(ec));
    }
    return total;
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
        : num_partitions_(std::max<uint32_t>(1, config.num_partitions)), root_path_(config.root_path) {
        opts_.store_path       = {config.root_path};
        opts_.num_threads      = ResolveThreadCount(config.num_threads);
        opts_.data_append_mode = config.append_mode;

        if (config.enable_compression) {
            opts_.enable_compression = true;
            if (config.zstd_compression_level != 0) {
                opts_.zstd_compression_level = config.zstd_compression_level;
            }
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

    StoreResult Put(const std::string& name_space, const std::string& key, const std::string& value,
                    uint64_t ts) override {
        if (!started_.load(std::memory_order_acquire)) {
            return StoreResult{false, "eloqstore is not started"};
        }

        const std::string full_key = ComposeKey(name_space, key);
        const auto table           = ResolveTable(name_space, key);
        eloqstore::BatchWriteRequest req;
        std::vector<eloqstore::WriteDataEntry> batch;
        batch.emplace_back(full_key, value, ts, eloqstore::WriteOp::Upsert);
        req.SetArgs(table, std::move(batch));
        store_->ExecSync(&req);
        if (req.Error() != eloqstore::KvError::NoError) {
            return StoreResult{false, eloqstore::ErrorString(req.Error())};
        }

        OnUpsertCommitted(full_key, value.size());
        return StoreResult{true, {}};
    }

    GetResult Get(const std::string& name_space, const std::string& key) override {
        if (!started_.load(std::memory_order_acquire)) {
            return GetResult{false, {}, "eloqstore is not started"};
        }

        const auto table = ResolveTable(name_space, key);
        eloqstore::ReadRequest req;
        req.SetArgs(table, ComposeKey(name_space, key));
        store_->ExecSync(&req);
        if (req.Error() == eloqstore::KvError::NotFound) {
            return GetResult{false, {}, {}};
        }
        if (req.Error() != eloqstore::KvError::NoError) {
            return GetResult{false, {}, eloqstore::ErrorString(req.Error())};
        }
        return GetResult{true, req.value_, {}};
    }

    StoreResult Delete(const std::string& name_space, const std::string& key, uint64_t ts) override {
        if (!started_.load(std::memory_order_acquire)) {
            return StoreResult{false, "eloqstore is not started"};
        }

        const std::string full_key = ComposeKey(name_space, key);
        const auto table           = ResolveTable(name_space, key);
        eloqstore::BatchWriteRequest req;
        std::vector<eloqstore::WriteDataEntry> batch;
        batch.emplace_back(full_key, std::string(), ts, eloqstore::WriteOp::Delete);
        req.SetArgs(table, std::move(batch));
        store_->ExecSync(&req);
        if (req.Error() != eloqstore::KvError::NoError) {
            return StoreResult{false, eloqstore::ErrorString(req.Error())};
        }

        OnDeleteCommitted(full_key);
        return StoreResult{true, {}};
    }

    std::vector<KeyValueEntry> Scan(const std::string& name_space, const std::string& begin, const std::string& end,
                                    size_t limit, std::string* error) override {
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

        const std::string prefix = name_space + ":";
        for (uint32_t pid = 0; pid < num_partitions_; ++pid) {
            eloqstore::ScanRequest req;
            req.SetArgs(TableForPartition(pid), begin_key, end_key);
            if (limit > 0) {
                req.SetPagination(limit, static_cast<size_t>(1) << 20);
            }
            store_->ExecSync(&req);
            if (req.Error() != eloqstore::KvError::NoError) {
                if (error != nullptr) {
                    *error =
                        "scan partition " + std::to_string(pid) + " failed: " + eloqstore::ErrorString(req.Error());
                }
                return out;
            }

            for (const auto& entry : req.Entries()) {
                const std::string full_key = entry.key_;
                if (full_key.rfind(prefix, 0) != 0) {
                    continue;
                }
                out.push_back(
                    KeyValueEntry{name_space, full_key.substr(prefix.size()), entry.value_, entry.timestamp_});
            }
        }
        std::sort(out.begin(), out.end(), [](const KeyValueEntry& lhs, const KeyValueEntry& rhs) {
            if (lhs.key == rhs.key) {
                return lhs.ts > rhs.ts;
            }
            return lhs.key < rhs.key;
        });
        if (limit > 0 && out.size() > limit) {
            out.resize(limit);
        }
        return out;
    }

    StoreResult BatchWrite(const std::vector<WriteEntry>& entries) override {
        if (!started_.load(std::memory_order_acquire)) {
            return StoreResult{false, "eloqstore is not started"};
        }

        std::map<uint32_t, std::vector<eloqstore::WriteDataEntry>> batches;
        struct CommittedEntry {
            WriteOp op;
            std::string full_key;
            size_t value_size;
        };
        std::vector<CommittedEntry> committed;
        committed.reserve(entries.size());

        for (const auto& entry : entries) {
            const std::string full_key = ComposeKey(entry.entry.name_space, entry.entry.key);
            const uint32_t pid         = ResolvePartitionId(entry.entry.name_space, entry.entry.key);
            const auto op = (entry.op == WriteOp::Delete) ? eloqstore::WriteOp::Delete : eloqstore::WriteOp::Upsert;
            const std::string value = (entry.op == WriteOp::Delete) ? std::string() : entry.entry.value;
            batches[pid].emplace_back(full_key, value, entry.entry.ts, op);
            committed.push_back(CommittedEntry{entry.op, full_key, entry.entry.value.size()});
        }

        for (auto& kv : batches) {
            eloqstore::BatchWriteRequest req;
            req.SetArgs(TableForPartition(kv.first), std::move(kv.second));
            store_->ExecSync(&req);
            if (req.Error() != eloqstore::KvError::NoError) {
                return StoreResult{false, "batch write partition " + std::to_string(kv.first) +
                                              " failed: " + eloqstore::ErrorString(req.Error())};
            }
        }

        for (const auto& entry : committed) {
            if (entry.op == WriteOp::Delete) {
                OnDeleteCommitted(entry.full_key);
            } else {
                OnUpsertCommitted(entry.full_key, entry.value_size);
            }
        }
        return StoreResult{true, {}};
    }

    StoreUsage ApproximateUsage(std::string* error) override {
        (void)error;
        StoreUsage usage;
        usage.mem_used_bytes  = approx_mem_used_bytes_.load(std::memory_order_acquire);
        usage.disk_used_bytes = DirectorySizeBytes(root_path_);
        usage.item_count      = approx_item_count_.load(std::memory_order_acquire);
        return usage;
    }

private:
    uint32_t ResolvePartitionId(const std::string& name_space, const std::string& key) const {
        const std::string seed = PartitionAffinitySeed(name_space, key);
        return static_cast<uint32_t>(std::hash<std::string>{}(seed) % num_partitions_);
    }

    eloqstore::TableIdent TableForPartition(uint32_t pid) const { return eloqstore::TableIdent("pgmem", pid); }

    eloqstore::TableIdent ResolveTable(const std::string& name_space, const std::string& key) const {
        return TableForPartition(ResolvePartitionId(name_space, key));
    }

    void OnUpsertCommitted(const std::string& full_key, size_t value_size) {
        std::unique_lock<std::shared_mutex> lock(usage_mu_);
        const auto existing      = key_size_bytes_.find(full_key);
        const uint64_t next_size = static_cast<uint64_t>(value_size);
        if (existing == key_size_bytes_.end()) {
            key_size_bytes_[full_key] = next_size;
            approx_item_count_.fetch_add(1, std::memory_order_acq_rel);
            approx_mem_used_bytes_.fetch_add(next_size, std::memory_order_acq_rel);
            return;
        }

        const uint64_t prev_size = existing->second;
        existing->second         = next_size;
        if (next_size >= prev_size) {
            approx_mem_used_bytes_.fetch_add(next_size - prev_size, std::memory_order_acq_rel);
        } else {
            approx_mem_used_bytes_.fetch_sub(prev_size - next_size, std::memory_order_acq_rel);
        }
    }

    void OnDeleteCommitted(const std::string& full_key) {
        std::unique_lock<std::shared_mutex> lock(usage_mu_);
        const auto existing = key_size_bytes_.find(full_key);
        if (existing == key_size_bytes_.end()) {
            return;
        }
        const uint64_t prev_size = existing->second;
        key_size_bytes_.erase(existing);
        approx_item_count_.fetch_sub(1, std::memory_order_acq_rel);
        approx_mem_used_bytes_.fetch_sub(prev_size, std::memory_order_acq_rel);
    }

    std::atomic<bool> started_{false};
    eloqstore::KvOptions opts_;
    uint32_t num_partitions_{1};
    std::unique_ptr<eloqstore::EloqStore> store_;
    std::string root_path_;

    std::shared_mutex usage_mu_;
    std::unordered_map<std::string, uint64_t> key_size_bytes_;
    std::atomic<uint64_t> approx_mem_used_bytes_{0};
    std::atomic<uint64_t> approx_item_count_{0};
};

#endif

}  // namespace

std::unique_ptr<IStoreAdapter> CreateEloqStoreAdapter(const StoreAdapterConfig& config, std::string* error) {
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
