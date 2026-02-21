#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

std::string WorkspacePrefix(const std::string& key) {
    static constexpr const char* kWsPrefix = "ws/";
    if (key.rfind(kWsPrefix, 0) == 0) {
        const size_t slash = key.find('/', 3);
        if (slash == std::string::npos) {
            return key.substr(3);
        }
        return key.substr(3, slash - 3);
    }

    const size_t pos = key.find(':');
    if (pos == std::string::npos || pos == 0) {
        return key;
    }
    return key.substr(0, pos);
}

std::string SuffixAfterLastColon(const std::string& key) {
    const size_t slash = key.rfind('/');
    if (slash != std::string::npos && slash + 1 < key.size()) {
        return key.substr(slash + 1);
    }

    const size_t pos = key.rfind(':');
    if (pos == std::string::npos || pos + 1 >= key.size()) {
        return {};
    }
    return key.substr(pos + 1);
}

std::string RoutingShardKey(const std::string& name_space, const std::string& key) {
    if (name_space == "mem_route_meta" || name_space == "ds_projection_ckpt") {
        return WorkspacePrefix(key);
    }

    if (name_space == "mem_docs" || name_space == "mem_term_dict" || name_space == "mem_posting_blk" ||
        name_space == "mem_vec_code" || name_space == "mem_vec_fp" || name_space == "ds_events") {
        const std::string workspace = WorkspacePrefix(key);
        const std::string record_id = SuffixAfterLastColon(key);
        if (!workspace.empty() && !record_id.empty()) {
            return workspace + ":" + record_id;
        }
        return key;
    }

    return key;
}

std::string PartitionRoutingSeed(const std::string& name_space, const std::string& key) {
    return name_space + "|" + RoutingShardKey(name_space, key);
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

constexpr uint64_t kDefaultBufferPoolMb    = 64;
constexpr uint64_t kDefaultRootMetaCacheMb = 2048;

class EloqStoreAdapter final : public IStoreAdapter {
public:
    explicit EloqStoreAdapter(const StoreAdapterConfig& config)
        : num_partitions_(std::max<uint32_t>(1, config.num_partitions)), root_path_(config.root_path) {
        opts_.store_path           = {config.root_path};
        opts_.num_threads          = ResolveThreadCount(config.num_threads);
        opts_.data_append_mode     = config.append_mode;
        opts_.buffer_pool_size     = kDefaultBufferPoolMb * static_cast<uint64_t>(eloqstore::MB);
        opts_.root_meta_cache_size = kDefaultRootMetaCacheMb * static_cast<uint64_t>(eloqstore::MB);

        if (config.enable_compression) {
            opts_.enable_compression = true;
            if (config.zstd_compression_level != 0) {
                opts_.zstd_compression_level = config.zstd_compression_level;
            }
        }
        const uint64_t root_meta_per_shard =
            (opts_.num_threads == 0) ? opts_.root_meta_cache_size : (opts_.root_meta_cache_size / opts_.num_threads);
        std::cerr << "[eloqstore] cache config: index_buffer_pool_per_shard_bytes=" << opts_.buffer_pool_size
                  << " root_meta_cache_total_bytes=" << opts_.root_meta_cache_size
                  << " root_meta_cache_per_shard_bytes=" << root_meta_per_shard << "\n";

        store_ = std::make_unique<eloqstore::EloqStore>(opts_);
    }

    bool Start(std::string* error) {
        CleanupLegacyLayoutMarkerInStoreRoot();

        const eloqstore::KvError err = store_->Start();
        if (err != eloqstore::KvError::NoError) {
            if (error != nullptr) {
                *error = eloqstore::ErrorString(err);
            }
            return false;
        }
        started_.store(true, std::memory_order_release);
        if (!RefreshLayoutOnStartup(error)) {
            started_.store(false, std::memory_order_release);
            store_->Stop();
            return false;
        }
        return true;
    }

    ~EloqStoreAdapter() override {
        if (compact_worker_.joinable()) {
            compact_worker_.join();
        }
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
        if (!out.empty()) {
            std::vector<KeyValueEntry> deduped;
            deduped.reserve(out.size());
            for (auto& entry : out) {
                if (deduped.empty() || deduped.back().key != entry.key) {
                    deduped.push_back(std::move(entry));
                }
            }
            out.swap(deduped);
        }
        if (limit > 0 && out.size() > limit) {
            out.resize(limit);
        }
        return out;
    }

    StoreResult BatchWrite(const std::vector<WriteEntry>& entries) override {
        if (!started_.load(std::memory_order_acquire)) {
            return StoreResult{false, "eloqstore is not started"};
        }

        std::map<uint32_t, std::map<std::string, eloqstore::WriteDataEntry>> batches;
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
            // eloqstore batch write task requires keys to be strictly ordered and unique.
            // Keep the last write for duplicated keys in the same partition.
            batches[pid].insert_or_assign(full_key, eloqstore::WriteDataEntry(full_key, value, entry.entry.ts, op));
        }

        for (auto& kv : batches) {
            std::vector<eloqstore::WriteDataEntry> ordered_batch;
            ordered_batch.reserve(kv.second.size());
            for (auto& item : kv.second) {
                committed.push_back(CommittedEntry{
                    item.second.op_ == eloqstore::WriteOp::Delete ? WriteOp::Delete : WriteOp::Upsert,
                    item.second.key_,
                    item.second.val_.size(),
                });
                ordered_batch.push_back(std::move(item.second));
            }

            eloqstore::BatchWriteRequest req;
            req.SetArgs(TableForPartition(kv.first), std::move(ordered_batch));
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

    StoreCompactTriggerResult TriggerStoreCompactAsync() override {
        StoreCompactTriggerResult out;
        out.async = true;

        if (!started_.load(std::memory_order_acquire)) {
            out.noop    = true;
            out.message = "eloqstore is not started";
            return out;
        }
        if (!opts_.data_append_mode || opts_.file_amplify_factor == 0) {
            out.noop    = true;
            out.message = "eloqstore compact noop: append mode disabled or file amplify factor is zero";
            return out;
        }

        std::lock_guard<std::mutex> lock(compact_mu_);
        if (compact_running_) {
            out.busy    = true;
            out.message = "eloqstore compact already running";
            return out;
        }
        if (compact_worker_.joinable()) {
            compact_worker_.join();
        }

        compact_running_             = true;
        const uint64_t job_id        = ++compact_job_id_;
        const uint32_t partition_cnt = num_partitions_;
        compact_worker_ = std::thread([this, job_id, partition_cnt]() { RunCompactWorker(job_id, partition_cnt); });

        out.triggered       = true;
        out.partition_count = partition_cnt;
        out.message         = "eloqstore compact scheduled";
        return out;
    }

private:
    struct LayoutMeta {
        uint32_t partition_count{0};
        std::string routing_schema;
    };

    static constexpr const char* kLayoutMetaFile       = ".pgmem_store_layout";
    static constexpr const char* kLegacyLayoutMetaFile = ".pgmem_store_layout";
    static constexpr const char* kRoutingSchema        = "balanced_workspace_record";

    bool RefreshLayoutOnStartup(std::string* error) {
        const std::vector<uint32_t> existing_partitions = DetectExistingPartitions();

        LayoutMeta meta;
        const bool has_meta = LoadLayoutMeta(&meta);

        bool needs_rehash = false;
        if (has_meta) {
            if (meta.partition_count != num_partitions_) {
                needs_rehash = true;
            }
            if (meta.routing_schema != kRoutingSchema) {
                needs_rehash = true;
            }
        } else if (!existing_partitions.empty()) {
            // Existing data without layout metadata is treated as legacy and will be rebalanced.
            needs_rehash = true;
        }

        if (!existing_partitions.empty() && existing_partitions.back() >= num_partitions_) {
            needs_rehash = true;
        }

        if (needs_rehash) {
            std::cerr << "[eloqstore] layout refresh start: target_partitions=" << num_partitions_
                      << " existing_partitions=" << existing_partitions.size() << "\n";
            if (!RehashStoredEntries(existing_partitions, error)) {
                return false;
            }
            std::cerr << "[eloqstore] layout refresh done\n";
        }

        return PersistLayoutMeta(num_partitions_, error);
    }

    std::filesystem::path LayoutMetaPath() const {
        const std::filesystem::path root(root_path_);
        const std::string leaf = root.filename().empty() ? std::string("store") : root.filename().string();
        return root.parent_path() / ("." + leaf + "_" + kLayoutMetaFile);
    }

    bool LoadLayoutMeta(LayoutMeta* out) const {
        if (out == nullptr) {
            return false;
        }

        std::ifstream input(LayoutMetaPath());
        if (!input.is_open()) {
            return false;
        }

        LayoutMeta parsed;
        std::string line;
        while (std::getline(input, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key   = line.substr(0, eq);
            const std::string value = line.substr(eq + 1);
            if (key == "partition_count") {
                try {
                    const unsigned long raw = std::stoul(value);
                    if (raw > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
                        continue;
                    }
                    parsed.partition_count = static_cast<uint32_t>(raw);
                } catch (...) {
                    continue;
                }
            } else if (key == "routing_schema") {
                parsed.routing_schema = value;
            }
        }

        if (parsed.partition_count == 0 || parsed.routing_schema.empty()) {
            return false;
        }

        *out = std::move(parsed);
        return true;
    }

    bool PersistLayoutMeta(uint32_t partition_count, std::string* error) const {
        const std::filesystem::path meta_path = LayoutMetaPath();
        std::error_code ec;
        if (!meta_path.parent_path().empty()) {
            std::filesystem::create_directories(meta_path.parent_path(), ec);
        }

        std::ofstream output(meta_path, std::ios::trunc);
        if (!output.is_open()) {
            if (error != nullptr) {
                *error = "failed to persist store layout metadata";
            }
            return false;
        }

        output << "partition_count=" << partition_count << "\n";
        output << "routing_schema=" << kRoutingSchema << "\n";
        output.flush();
        if (!output.good()) {
            if (error != nullptr) {
                *error = "failed to flush store layout metadata";
            }
            return false;
        }
        return true;
    }

    void CleanupLegacyLayoutMarkerInStoreRoot() const {
        std::error_code ec;
        const std::filesystem::path legacy_path = std::filesystem::path(root_path_) / kLegacyLayoutMetaFile;
        if (!std::filesystem::exists(legacy_path, ec)) {
            return;
        }
        if (std::filesystem::is_regular_file(legacy_path, ec)) {
            std::filesystem::remove(legacy_path, ec);
        }
    }

    std::vector<uint32_t> DetectExistingPartitions() const {
        std::vector<uint32_t> out;

        std::error_code ec;
        if (root_path_.empty() || !std::filesystem::exists(root_path_, ec)) {
            return out;
        }

        std::filesystem::directory_iterator it(root_path_, ec);
        std::filesystem::directory_iterator end;
        for (; it != end && !ec; it.increment(ec)) {
            if (!it->is_directory(ec)) {
                continue;
            }
            const std::string name               = it->path().filename().string();
            static constexpr const char* kPrefix = "pgmem.";
            static constexpr size_t kPrefixLen   = 6;
            if (name.rfind(kPrefix, 0) != 0 || name.size() <= kPrefixLen) {
                continue;
            }
            try {
                const unsigned long raw = std::stoul(name.substr(kPrefixLen));
                if (raw > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
                    continue;
                }
                out.push_back(static_cast<uint32_t>(raw));
            } catch (...) {
                continue;
            }
        }

        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    bool RehashStoredEntries(const std::vector<uint32_t>& existing_partitions, std::string* error) {
        if (existing_partitions.empty()) {
            return true;
        }

        static const std::array<const char*, 8> kNamespaces = {"mem_docs",     "mem_term_dict",     "mem_posting_blk",
                                                               "mem_vec_code", "mem_vec_fp",        "mem_route_meta",
                                                               "ds_events",    "ds_projection_ckpt"};

        uint64_t moved_entries = 0;

        for (uint32_t source_pid : existing_partitions) {
            for (const char* ns : kNamespaces) {
                const std::string name_space = ns;
                const std::string prefix     = name_space + ":";
                const std::string begin_key  = prefix;
                const std::string end_key    = name_space + ";";

                eloqstore::ScanRequest scan_req;
                scan_req.SetArgs(TableForPartition(source_pid), begin_key, end_key);
                store_->ExecSync(&scan_req);

                if (scan_req.Error() == eloqstore::KvError::NotFound) {
                    continue;
                }
                if (scan_req.Error() != eloqstore::KvError::NoError) {
                    if (error != nullptr) {
                        *error = "layout refresh scan failed on partition " + std::to_string(source_pid) +
                                 " namespace " + name_space + ": " + eloqstore::ErrorString(scan_req.Error());
                    }
                    return false;
                }

                std::map<uint32_t, std::vector<eloqstore::WriteDataEntry>> upserts;
                std::vector<eloqstore::WriteDataEntry> deletes;

                for (const auto& entry : scan_req.Entries()) {
                    if (entry.key_.rfind(prefix, 0) != 0) {
                        continue;
                    }

                    const std::string key     = entry.key_.substr(prefix.size());
                    const uint32_t target_pid = ResolvePartitionId(name_space, key);
                    if (target_pid == source_pid) {
                        continue;
                    }

                    upserts[target_pid].emplace_back(entry.key_, entry.value_, entry.timestamp_,
                                                     eloqstore::WriteOp::Upsert);
                    deletes.emplace_back(entry.key_, std::string(), entry.timestamp_, eloqstore::WriteOp::Delete);
                }

                for (auto& target_batch : upserts) {
                    eloqstore::BatchWriteRequest write_req;
                    write_req.SetArgs(TableForPartition(target_batch.first), std::move(target_batch.second));
                    store_->ExecSync(&write_req);
                    if (write_req.Error() != eloqstore::KvError::NoError) {
                        if (error != nullptr) {
                            *error = "layout refresh write failed on partition " + std::to_string(target_batch.first) +
                                     " namespace " + name_space + ": " + eloqstore::ErrorString(write_req.Error());
                        }
                        return false;
                    }
                }

                if (!deletes.empty()) {
                    moved_entries += static_cast<uint64_t>(deletes.size());
                    eloqstore::BatchWriteRequest delete_req;
                    delete_req.SetArgs(TableForPartition(source_pid), std::move(deletes));
                    store_->ExecSync(&delete_req);
                    if (delete_req.Error() != eloqstore::KvError::NoError) {
                        if (error != nullptr) {
                            *error = "layout refresh delete failed on partition " + std::to_string(source_pid) +
                                     " namespace " + name_space + ": " + eloqstore::ErrorString(delete_req.Error());
                        }
                        return false;
                    }
                }
            }
        }

        CleanupOutdatedPartitionDirs(existing_partitions);
        CleanupEmptyPartitionDirs(existing_partitions);
        std::cerr << "[eloqstore] layout refresh moved_entries=" << moved_entries << "\n";
        return true;
    }

    void CleanupOutdatedPartitionDirs(const std::vector<uint32_t>& partitions) const {
        for (uint32_t pid : partitions) {
            if (pid < num_partitions_) {
                continue;
            }
            std::error_code ec;
            const std::filesystem::path dir = std::filesystem::path(root_path_) / ("pgmem." + std::to_string(pid));
            if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
                continue;
            }
            std::filesystem::remove_all(dir, ec);
        }
    }

    void CleanupEmptyPartitionDirs(const std::vector<uint32_t>& partitions) const {
        for (uint32_t pid : partitions) {
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::path(root_path_) / ("pgmem." + std::to_string(pid));
            if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
                continue;
            }
            if (std::filesystem::is_empty(dir, ec)) {
                std::filesystem::remove(dir, ec);
            }
        }
    }

    void RunCompactWorker(uint64_t job_id, uint32_t partition_count) {
        std::cerr << "[eloqstore] compact start: job_id=" << job_id << " partitions=" << partition_count << "\n";

        uint32_t succeeded = 0;
        uint32_t failed    = 0;
        for (uint32_t pid = 0; pid < partition_count; ++pid) {
            eloqstore::CompactRequest req;
            req.SetTableId(TableForPartition(pid));
            store_->ExecSync(&req);
            if (req.Error() != eloqstore::KvError::NoError) {
                ++failed;
                std::cerr << "[eloqstore] compact partition failed: job_id=" << job_id << " partition=" << pid
                          << " error=" << eloqstore::ErrorString(req.Error()) << "\n";
                continue;
            }
            ++succeeded;
        }

        std::cerr << "[eloqstore] compact done: job_id=" << job_id << " succeeded_partitions=" << succeeded
                  << " failed_partitions=" << failed << "\n";
        std::lock_guard<std::mutex> lock(compact_mu_);
        compact_running_ = false;
    }

    uint32_t ResolvePartitionId(const std::string& name_space, const std::string& key) const {
        const std::string seed = PartitionRoutingSeed(name_space, key);
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
    std::mutex compact_mu_;
    bool compact_running_{false};
    std::thread compact_worker_;
    uint64_t compact_job_id_{0};
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
