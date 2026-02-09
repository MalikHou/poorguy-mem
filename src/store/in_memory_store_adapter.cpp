#include "pgmem/store/store_adapter.h"

#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace pgmem::store {
namespace {

struct StoredValue {
    std::string value;
    uint64_t ts{0};
};

class InMemoryStoreAdapter final : public IStoreAdapter {
public:
    StoreResult Put(const std::string& name_space,
                    const std::string& key,
                    const std::string& value,
                    uint64_t ts) override {
        std::unique_lock<std::shared_mutex> lock(mu_);
        db_[name_space][key] = StoredValue{value, ts};
        return StoreResult{true, {}};
    }

    GetResult Get(const std::string& name_space, const std::string& key) override {
        std::shared_lock<std::shared_mutex> lock(mu_);
        auto ns_it = db_.find(name_space);
        if (ns_it == db_.end()) {
            return GetResult{false, {}, {}};
        }
        auto key_it = ns_it->second.find(key);
        if (key_it == ns_it->second.end()) {
            return GetResult{false, {}, {}};
        }
        return GetResult{true, key_it->second.value, {}};
    }

    std::vector<KeyValueEntry> Scan(const std::string& name_space,
                                    const std::string& begin,
                                    const std::string& end,
                                    size_t limit,
                                    std::string* error) override {
        (void)error;
        std::vector<KeyValueEntry> out;
        std::shared_lock<std::shared_mutex> lock(mu_);
        auto ns_it = db_.find(name_space);
        if (ns_it == db_.end()) {
            return out;
        }

        auto it = ns_it->second.lower_bound(begin);
        while (it != ns_it->second.end()) {
            if (!end.empty() && it->first >= end) {
                break;
            }
            out.push_back(KeyValueEntry{name_space, it->first, it->second.value, it->second.ts});
            if (limit != 0 && out.size() >= limit) {
                break;
            }
            ++it;
        }
        return out;
    }

    StoreResult BatchWrite(const std::vector<KeyValueEntry>& entries) override {
        std::unique_lock<std::shared_mutex> lock(mu_);
        for (const KeyValueEntry& entry : entries) {
            db_[entry.name_space][entry.key] = StoredValue{entry.value, entry.ts};
        }
        return StoreResult{true, {}};
    }

private:
    std::shared_mutex mu_;
    std::unordered_map<std::string, std::map<std::string, StoredValue>> db_;
};

}  // namespace

std::unique_ptr<IStoreAdapter> CreateInMemoryStoreAdapter() {
    return std::make_unique<InMemoryStoreAdapter>();
}

}  // namespace pgmem::store
