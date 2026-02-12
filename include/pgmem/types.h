#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pgmem {

enum class OpType { Upsert, Pin, Delete };

struct MemoryRecord {
    std::string id;
    std::string workspace_id;
    std::string session_id;
    std::string source;
    std::string content;
    std::vector<std::string> tags;
    bool pinned{false};
    bool tombstone{false};
    uint64_t created_at_ms{0};
    uint64_t updated_at_ms{0};
    uint64_t version{0};
    uint64_t size_bytes{0};
    uint64_t last_access_ms{0};
    uint64_t hit_count{0};
    double importance_score{0.0};
    std::string tier{"hot"};
    std::string node_id;
};

struct SearchHit {
    std::string memory_id;
    std::string source;
    std::string content;
    double lexical_score{0.0};
    double semantic_score{0.0};
    double final_score{0.0};
    uint64_t updated_at_ms{0};
    bool pinned{false};
};

struct StatsSnapshot {
    double p95_read_ms{0.0};
    double p95_write_ms{0.0};
    double token_reduction_ratio{0.0};
    double fallback_rate{0.0};
    uint64_t mem_used_bytes{0};
    uint64_t disk_used_bytes{0};
    uint64_t item_count{0};
    uint64_t tombstone_count{0};
    uint64_t gc_last_run_ms{0};
    uint64_t gc_evicted_count{0};
    bool capacity_blocked{false};
    std::string write_ack_mode{"accepted"};
    uint64_t pending_write_ops{0};
    uint64_t flush_failures_total{0};
    uint64_t volatile_dropped_on_shutdown{0};
    std::string effective_backend;
    std::string last_flush_error;
};

struct CommitTurnInput {
    std::string workspace_id;
    std::string session_id;
    std::string user_text;
    std::string assistant_text;
    std::vector<std::string> code_snippets;
    std::vector<std::string> commands;
};

struct BootstrapInput {
    std::string workspace_id;
    std::string session_id;
    std::string task_text;
    std::vector<std::string> open_files;
    int token_budget{2048};
};

struct SearchInput {
    std::string workspace_id;
    std::string query;
    size_t top_k{8};
    int token_budget{2048};
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

using StringMap = std::map<std::string, std::string>;

}  // namespace pgmem
