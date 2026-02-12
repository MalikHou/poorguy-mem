#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pgmem/store/store_adapter.h"

namespace pgmem::store {
namespace {

struct StoredValue {
    std::string value;
    uint64_t ts{0};
};

constexpr size_t kShardCount = 32;

class InMemoryStoreAdapter final : public IStoreAdapter {
public:
    StoreResult Put(const std::string& name_space, const std::string& key, const std::string& value,
                    uint64_t ts) override {
        const size_t shard_idx = ShardIndex(name_space, key);
        Shard& shard           = shards_[shard_idx];
        std::unique_lock<std::shared_mutex> lock(shard.mu);
        UpsertLocked(shard, name_space, key, value, ts);
        return StoreResult{true, {}};
    }

    GetResult Get(const std::string& name_space, const std::string& key) override {
        const size_t shard_idx = ShardIndex(name_space, key);
        Shard& shard           = shards_[shard_idx];
        std::shared_lock<std::shared_mutex> lock(shard.mu);
        auto ns_it = shard.db.find(name_space);
        if (ns_it == shard.db.end()) {
            return GetResult{false, {}, {}};
        }
        auto key_it = ns_it->second.find(key);
        if (key_it == ns_it->second.end()) {
            return GetResult{false, {}, {}};
        }
        return GetResult{true, key_it->second.value, {}};
    }

    StoreResult Delete(const std::string& name_space, const std::string& key, uint64_t ts) override {
        (void)ts;
        const size_t shard_idx = ShardIndex(name_space, key);
        Shard& shard           = shards_[shard_idx];
        std::unique_lock<std::shared_mutex> lock(shard.mu);
        DeleteLocked(shard, name_space, key);
        return StoreResult{true, {}};
    }

    std::vector<KeyValueEntry> Scan(const std::string& name_space, const std::string& begin, const std::string& end,
                                    size_t limit, std::string* error) override {
        (void)error;
        std::vector<KeyValueEntry> out;
        out.reserve(limit == 0 ? 256 : limit);

        for (Shard& shard : shards_) {
            std::shared_lock<std::shared_mutex> lock(shard.mu);
            auto ns_it = shard.db.find(name_space);
            if (ns_it == shard.db.end()) {
                continue;
            }

            auto it = ns_it->second.lower_bound(begin);
            while (it != ns_it->second.end()) {
                if (!end.empty() && it->first >= end) {
                    break;
                }
                out.push_back(KeyValueEntry{name_space, it->first, it->second.value, it->second.ts});
                ++it;
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
        for (const WriteEntry& entry : entries) {
            if (entry.op == WriteOp::Delete) {
                const auto res = Delete(entry.entry.name_space, entry.entry.key, entry.entry.ts);
                if (!res.ok) {
                    return res;
                }
                continue;
            }

            const auto res = Put(entry.entry.name_space, entry.entry.key, entry.entry.value, entry.entry.ts);
            if (!res.ok) {
                return res;
            }
        }
        return StoreResult{true, {}};
    }

    StoreUsage ApproximateUsage(std::string* error) override {
        (void)error;
        StoreUsage usage;
        usage.mem_used_bytes  = approx_mem_used_bytes_.load(std::memory_order_acquire);
        usage.disk_used_bytes = 0;
        usage.item_count      = approx_item_count_.load(std::memory_order_acquire);
        return usage;
    }

private:
    struct Shard {
        std::shared_mutex mu;
        std::unordered_map<std::string, std::map<std::string, StoredValue>> db;
    };

    static size_t EntryFootprintBytes(const std::string& ns, const std::string& key, const std::string& value) {
        return ns.size() + key.size() + value.size() + sizeof(StoredValue);
    }

    static size_t ShardIndex(const std::string& name_space, const std::string& key) {
        return std::hash<std::string>{}(name_space + ":" + key) % kShardCount;
    }

    void UpsertLocked(Shard& shard, const std::string& name_space, const std::string& key, const std::string& value,
                      uint64_t ts) {
        auto& ns_map           = shard.db[name_space];
        const auto existing    = ns_map.find(key);
        const size_t new_bytes = EntryFootprintBytes(name_space, key, value);
        if (existing == ns_map.end()) {
            ns_map[key] = StoredValue{value, ts};
            approx_item_count_.fetch_add(1, std::memory_order_acq_rel);
            approx_mem_used_bytes_.fetch_add(new_bytes, std::memory_order_acq_rel);
            return;
        }

        const size_t old_bytes = EntryFootprintBytes(name_space, key, existing->second.value);
        existing->second       = StoredValue{value, ts};
        if (new_bytes >= old_bytes) {
            approx_mem_used_bytes_.fetch_add(new_bytes - old_bytes, std::memory_order_acq_rel);
        } else {
            approx_mem_used_bytes_.fetch_sub(old_bytes - new_bytes, std::memory_order_acq_rel);
        }
    }

    void DeleteLocked(Shard& shard, const std::string& name_space, const std::string& key) {
        auto ns_it = shard.db.find(name_space);
        if (ns_it == shard.db.end()) {
            return;
        }
        auto key_it = ns_it->second.find(key);
        if (key_it == ns_it->second.end()) {
            return;
        }

        const size_t old_bytes = EntryFootprintBytes(name_space, key, key_it->second.value);
        ns_it->second.erase(key_it);
        if (ns_it->second.empty()) {
            shard.db.erase(ns_it);
        }
        approx_item_count_.fetch_sub(1, std::memory_order_acq_rel);
        approx_mem_used_bytes_.fetch_sub(old_bytes, std::memory_order_acq_rel);
    }

    std::array<Shard, kShardCount> shards_;
    std::atomic<uint64_t> approx_mem_used_bytes_{0};
    std::atomic<uint64_t> approx_item_count_{0};
};

}  // namespace

std::unique_ptr<IStoreAdapter> CreateInMemoryStoreAdapter() { return std::make_unique<InMemoryStoreAdapter>(); }

}  // namespace pgmem::store
