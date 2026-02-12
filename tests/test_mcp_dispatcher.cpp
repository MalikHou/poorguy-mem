#include <string>

#include "pgmem/core/memory_engine.h"
#include "pgmem/core/retriever.h"
#include "pgmem/mcp/mcp_dispatcher.h"
#include "pgmem/store/store_adapter.h"
#include "pgmem/util/json.h"
#include "test_framework.h"

namespace {

bool HasPath(const pgmem::util::Json& json, const std::string& path) {
    return json.get_child_optional(path).has_value();
}

bool ArrayContainsString(const pgmem::util::Json& json, const std::string& path, const std::string& expected) {
    const auto arr_opt = json.get_child_optional(path);
    if (!arr_opt) {
        return false;
    }
    for (const auto& node : *arr_opt) {
        if (node.second.get_value<std::string>() == expected) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE(test_mcp_dispatcher_unknown_method_returns_jsonrpc_not_found) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::util::Json request;
    request.put("id", 42);
    request.put("method", "memory.no_such_method");

    const auto response = dispatcher.Handle(request);
    ASSERT_EQ(pgmem::util::GetIntOr(response, "error.code", 0), -32601);
    ASSERT_EQ(pgmem::util::GetStringOr(response, "error.message", ""), "method not found");
}

TEST_CASE(test_mcp_dispatcher_stats_contract_fields_and_types) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::util::Json commit_req;
    commit_req.put("id", 1);
    commit_req.put("method", "memory.commit_turn");
    pgmem::util::Json commit_params;
    commit_params.put("workspace_id", "ws-dispatcher");
    commit_params.put("session_id", "s1");
    commit_params.put("user_text", "stats contract user");
    commit_params.put("assistant_text", "stats contract assistant");
    commit_req.add_child("params", commit_params);
    const auto commit_resp = dispatcher.Handle(commit_req);
    ASSERT_TRUE(HasPath(commit_resp, "result.stored_ids"));

    pgmem::util::Json stats_req;
    stats_req.put("id", 2);
    stats_req.put("method", "memory.stats");
    pgmem::util::Json stats_params;
    stats_params.put("workspace_id", "ws-dispatcher");
    stats_params.put("window", "5m");
    stats_req.add_child("params", stats_params);

    const auto stats_resp = dispatcher.Handle(stats_req);
    ASSERT_TRUE(HasPath(stats_resp, "result.write_ack_mode"));
    ASSERT_TRUE(HasPath(stats_resp, "result.pending_write_ops"));
    ASSERT_TRUE(HasPath(stats_resp, "result.flush_failures_total"));
    ASSERT_TRUE(HasPath(stats_resp, "result.volatile_dropped_on_shutdown"));
    ASSERT_TRUE(HasPath(stats_resp, "result.effective_backend"));
    ASSERT_TRUE(HasPath(stats_resp, "result.last_flush_error"));

    const std::string mode = pgmem::util::GetStringOr(stats_resp, "result.write_ack_mode", "");
    ASSERT_EQ(mode, "accepted");

    const int pending        = pgmem::util::GetIntOr(stats_resp, "result.pending_write_ops", -1);
    const int flush_failures = pgmem::util::GetIntOr(stats_resp, "result.flush_failures_total", -1);
    const int dropped        = pgmem::util::GetIntOr(stats_resp, "result.volatile_dropped_on_shutdown", -1);
    ASSERT_TRUE(pending >= 0);
    ASSERT_TRUE(flush_failures >= 0);
    ASSERT_TRUE(dropped >= 0);

    const std::string backend = pgmem::util::GetStringOr(stats_resp, "result.effective_backend", "");
    ASSERT_TRUE(!backend.empty());
}

TEST_CASE(test_mcp_dispatcher_compact_response_fields) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::util::Json commit_req;
    commit_req.put("id", 10);
    commit_req.put("method", "memory.commit_turn");
    pgmem::util::Json commit_params;
    commit_params.put("workspace_id", "ws-compact");
    commit_params.put("session_id", "s1");
    commit_params.put("user_text", "compact user");
    commit_params.put("assistant_text", std::string(1200, 'c'));
    commit_req.add_child("params", commit_params);
    dispatcher.Handle(commit_req);

    pgmem::util::Json compact_req;
    compact_req.put("id", 11);
    compact_req.put("method", "memory.compact");
    pgmem::util::Json compact_params;
    compact_params.put("workspace_id", "ws-compact");
    compact_req.add_child("params", compact_params);

    const auto compact_resp = dispatcher.Handle(compact_req);
    ASSERT_TRUE(HasPath(compact_resp, "result.triggered"));
    ASSERT_TRUE(HasPath(compact_resp, "result.capacity_blocked"));
    ASSERT_TRUE(HasPath(compact_resp, "result.mem_before_bytes"));
    ASSERT_TRUE(HasPath(compact_resp, "result.disk_before_bytes"));
    ASSERT_TRUE(HasPath(compact_resp, "result.mem_after_bytes"));
    ASSERT_TRUE(HasPath(compact_resp, "result.disk_after_bytes"));
    ASSERT_TRUE(HasPath(compact_resp, "result.summarized_count"));
    ASSERT_TRUE(HasPath(compact_resp, "result.tombstoned_count"));
    ASSERT_TRUE(HasPath(compact_resp, "result.deleted_count"));
}

TEST_CASE(test_mcp_dispatcher_describe_contract_is_complete) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::util::Json describe_req;
    describe_req.put("id", 100);
    describe_req.put("method", "memory.describe");

    const auto describe_resp = dispatcher.Handle(describe_req);
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.server.kind", ""), "memory-server");
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.server.endpoint", ""), "/mcp");
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.server.describe_endpoint", ""), "/mcp/describe");
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.server.write_ack_mode", ""), "accepted");

    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.bootstrap"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.commit_turn"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.search"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.pin"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.stats"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.compact"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.describe"));

    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp,
                                       "result.methods.memory.commit_turn.input.properties.assistant_text.type", ""),
              "string");
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.methods.memory.search.output.properties.hits.type", ""),
              "array<object>");
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp,
                                       "result.methods.memory.stats.output.properties.pending_write_ops.type", ""),
              "integer");
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp,
                                       "result.methods.memory.compact.output.properties.deleted_count.type", ""),
              "integer");

    ASSERT_EQ(pgmem::util::GetIntOr(describe_resp, "result.errors.jsonrpc_method_not_found.code", 0), -32601);
    ASSERT_EQ(pgmem::util::GetIntOr(describe_resp, "result.errors.invalid_json_http.status", 0), 400);
}
