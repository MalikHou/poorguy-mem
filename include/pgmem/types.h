#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pgmem {

enum class OpType { Upsert, Pin, Delete };

using StringMap = std::map<std::string, std::string>;

struct MemoryRecord {
    std::string id;
    std::string workspace_id;
    std::string session_id;
    std::string source;
    std::string content;
    std::vector<std::string> tags;
    StringMap metadata;
    std::string dedup_key;

    bool pinned{false};
    bool tombstone{false};

    uint64_t created_at_ms{0};
    uint64_t updated_at_ms{0};
    uint64_t version{0};
    uint64_t size_bytes{0};
    uint64_t last_access_ms{0};
    uint64_t hit_count{0};

    double importance_score{1.0};

    std::string tier{"hot"};
    std::string node_id;

    uint64_t ttl_s{0};

    // Distributed-routing reserved fields (single-node for now).
    std::string shard_id;
    std::string replica_role;
    uint64_t routing_epoch{0};
    std::string shard_hint;
};

struct IndexStats {
    uint64_t segment_count{0};
    uint64_t posting_terms{0};
    uint64_t vector_count{0};
    double query_cache_hit_rate{0.0};
    double dense_probe_count_p95{0.0};
    uint64_t cold_rehydrate_count{0};
};

struct ScoreBreakdown {
    double sparse{0.0};
    double dense{0.0};
    double freshness{0.0};
    double pin{0.0};
    double final{0.0};
};

struct QueryHit {
    std::string memory_id;
    std::string source;
    std::string content;
    std::vector<std::string> tags;
    StringMap metadata;
    ScoreBreakdown scores;
    uint64_t updated_at_ms{0};
    bool pinned{false};
};

struct QueryDebugStats {
    uint64_t sparse_candidates{0};
    uint64_t dense_candidates{0};
    uint64_t merged_candidates{0};
    double sparse_ms{0.0};
    double dense_ms{0.0};
    double rerank_ms{0.0};
    double total_ms{0.0};
};

struct StatsSnapshot {
    double p95_read_ms{0.0};
    double p95_write_ms{0.0};
    double token_reduction_ratio{0.0};
    double fallback_rate{0.0};

    uint64_t mem_used_bytes{0};
    uint64_t disk_used_bytes{0};
    uint64_t resident_used_bytes{0};
    uint64_t resident_limit_bytes{0};
    uint64_t resident_evicted_count{0};
    uint64_t disk_fallback_search_count{0};

    uint64_t item_count{0};
    uint64_t tombstone_count{0};

    uint64_t gc_last_run_ms{0};
    uint64_t gc_evicted_count{0};
    bool capacity_blocked{false};

    std::string write_ack_mode{"durable"};
    std::string effective_backend;

    IndexStats index_stats;
};

struct WriteRecordInput {
    std::string record_id;
    std::string source{"turn"};
    std::string content;
    std::vector<std::string> tags;
    double importance{1.0};
    uint64_t ttl_s{0};
    bool pin{false};
    std::string dedup_key;
    StringMap metadata;
};

struct WriteInput {
    std::string workspace_id;
    std::string session_id{"default"};
    std::vector<WriteRecordInput> records;
    std::string write_mode{"upsert"};
};

struct WriteOutput {
    bool ok{false};
    std::vector<std::string> stored_ids;
    std::vector<std::string> deduped_ids;
    uint64_t index_generation{0};
    std::vector<std::string> warnings;
};

struct QueryFilter {
    std::string session_id;
    std::vector<std::string> sources;
    std::vector<std::string> tags_any;
    uint64_t updated_after_ms{0};
    uint64_t updated_before_ms{0};
    bool pinned_only{false};
};

struct RecallOptions {
    size_t sparse_k{200};
    size_t dense_k{200};
    size_t oversample{4};
};

struct RerankWeights {
    double w_sparse{0.55};
    double w_dense{0.30};
    double w_freshness{0.10};
    double w_pin{0.05};
};

struct QueryInput {
    std::string workspace_id;
    std::string query;
    size_t top_k{8};
    int token_budget{2048};
    QueryFilter filters;
    RecallOptions recall;
    RerankWeights rerank;
    bool debug{false};
};

struct QueryOutput {
    std::vector<QueryHit> hits;
    QueryDebugStats debug_stats;
};

struct StoreResult {
    bool ok{false};
    std::string error;
};

struct GetResult {
    bool found{false};
    std::string value;
    std::string error;
};

struct KeyValueEntry {
    std::string name_space;
    std::string key;
    std::string value;
    uint64_t ts{0};
};

enum class WriteOp { Upsert, Delete };

struct WriteEntry {
    WriteOp op{WriteOp::Upsert};
    KeyValueEntry entry;
};

struct StoreUsage {
    uint64_t mem_used_bytes{0};
    uint64_t disk_used_bytes{0};
    uint64_t item_count{0};
};

struct StoreCompactTriggerResult {
    bool triggered{false};
    bool noop{false};
    bool busy{false};
    bool async{true};
    uint32_t partition_count{0};
    std::string message;
};

}  // namespace pgmem
