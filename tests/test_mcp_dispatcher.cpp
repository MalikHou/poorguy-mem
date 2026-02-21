#include <memory>
#include <string>
#include <vector>

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

const pgmem::util::Json* FindErrorByName(const pgmem::util::Json& errors, const std::string& expected_name) {
    for (const auto& node : errors) {
        if (pgmem::util::GetStringOr(node.second, "name", "") == expected_name) {
            return &node.second;
        }
    }
    return nullptr;
}

const pgmem::util::Json* FindNamedChild(const pgmem::util::Json& parent, const std::string& child_name) {
    for (const auto& node : parent) {
        if (node.first == child_name) {
            return &node.second;
        }
    }
    return nullptr;
}

class BusyStoreCompactAdapter final : public pgmem::store::IStoreAdapter {
public:
    BusyStoreCompactAdapter() : inner_(pgmem::store::CreateInMemoryStoreAdapter()) {}

    pgmem::StoreResult Put(const std::string& name_space, const std::string& key, const std::string& value,
                           uint64_t ts) override {
        return inner_->Put(name_space, key, value, ts);
    }

    pgmem::GetResult Get(const std::string& name_space, const std::string& key) override {
        return inner_->Get(name_space, key);
    }

    pgmem::StoreResult Delete(const std::string& name_space, const std::string& key, uint64_t ts) override {
        return inner_->Delete(name_space, key, ts);
    }

    std::vector<pgmem::KeyValueEntry> Scan(const std::string& name_space, const std::string& begin,
                                           const std::string& end, size_t limit, std::string* error) override {
        return inner_->Scan(name_space, begin, end, limit, error);
    }

    pgmem::StoreResult BatchWrite(const std::vector<pgmem::WriteEntry>& entries) override {
        return inner_->BatchWrite(entries);
    }

    pgmem::StoreUsage ApproximateUsage(std::string* error) override { return inner_->ApproximateUsage(error); }

    pgmem::StoreCompactTriggerResult TriggerStoreCompactAsync() override {
        pgmem::StoreCompactTriggerResult out;
        out.busy    = true;
        out.async   = true;
        out.message = "busy";
        return out;
    }

private:
    std::unique_ptr<pgmem::store::IStoreAdapter> inner_;
};

pgmem::util::Json BuildWriteReq() {
    pgmem::util::Json req;
    req.put("id", 1);
    req.put("method", "memory.write");

    pgmem::util::Json params;
    params.put("workspace_id", "ws-dispatcher");
    params.put("session_id", "s1");

    pgmem::util::Json records;
    pgmem::util::Json rec;
    rec.put("source", "turn");
    rec.put("content", "dispatcher write contract");
    records.push_back(std::make_pair("", rec));

    params.add_child("records", records);
    req.add_child("params", params);
    return req;
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

TEST_CASE(test_mcp_dispatcher_mcp_describe_alias_is_removed) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::util::Json request;
    request.put("id", 43);
    request.put("method", "mcp.describe");

    const auto response = dispatcher.Handle(request);
    ASSERT_EQ(pgmem::util::GetIntOr(response, "error.code", 0), -32601);
    ASSERT_EQ(pgmem::util::GetStringOr(response, "error.message", ""), "method not found");
}

TEST_CASE(test_mcp_dispatcher_write_query_stats_contract) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    const auto write_resp = dispatcher.Handle(BuildWriteReq());
    ASSERT_TRUE(HasPath(write_resp, "result.ok"));
    ASSERT_TRUE(HasPath(write_resp, "result.stored_ids"));

    pgmem::util::Json query_req;
    query_req.put("id", 2);
    query_req.put("method", "memory.query");
    pgmem::util::Json query_params;
    query_params.put("workspace_id", "ws-dispatcher");
    query_params.put("query", "dispatcher contract");
    query_params.put("top_k", 3);
    query_req.add_child("params", query_params);

    const auto query_resp = dispatcher.Handle(query_req);
    ASSERT_TRUE(HasPath(query_resp, "result.hits"));
    ASSERT_TRUE(HasPath(query_resp, "result.debug_stats"));

    pgmem::util::Json stats_req;
    stats_req.put("id", 3);
    stats_req.put("method", "memory.stats");
    pgmem::util::Json stats_params;
    stats_params.put("workspace_id", "ws-dispatcher");
    stats_params.put("window", "5m");
    stats_req.add_child("params", stats_params);

    const auto stats_resp = dispatcher.Handle(stats_req);
    ASSERT_TRUE(HasPath(stats_resp, "result.write_ack_mode"));
    ASSERT_TRUE(HasPath(stats_resp, "result.index_stats"));
    ASSERT_TRUE(HasPath(stats_resp, "result.capacity_blocked"));
    ASSERT_EQ(pgmem::util::GetStringOr(stats_resp, "result.write_ack_mode", ""), "durable");
}

TEST_CASE(test_mcp_dispatcher_compact_response_fields) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    (void)dispatcher.Handle(BuildWriteReq());

    pgmem::util::Json compact_req;
    compact_req.put("id", 11);
    compact_req.put("method", "memory.compact");
    pgmem::util::Json compact_params;
    compact_params.put("workspace_id", "ws-dispatcher");
    compact_req.add_child("params", compact_params);

    const auto compact_resp = dispatcher.Handle(compact_req);
    ASSERT_TRUE(HasPath(compact_resp, "result.triggered"));
    ASSERT_TRUE(HasPath(compact_resp, "result.deleted_count"));
    ASSERT_TRUE(HasPath(compact_resp, "result.postings_reclaimed"));
    ASSERT_TRUE(HasPath(compact_resp, "result.vectors_reclaimed"));
}

TEST_CASE(test_mcp_dispatcher_store_compact_noop_inmemory) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::util::Json req;
    req.put("id", 21);
    req.put("method", "store.compact");
    pgmem::util::Json params;
    req.add_child("params", params);

    const auto resp = dispatcher.Handle(req);
    ASSERT_TRUE(!HasPath(resp, "error.code"));
    ASSERT_TRUE(HasPath(resp, "result.triggered"));
    ASSERT_TRUE(HasPath(resp, "result.noop"));
    ASSERT_TRUE(HasPath(resp, "result.busy"));
    ASSERT_TRUE(HasPath(resp, "result.async"));
    ASSERT_TRUE(HasPath(resp, "result.partition_count"));
    ASSERT_TRUE(HasPath(resp, "result.message"));
    ASSERT_TRUE(pgmem::util::GetBoolOr(resp, "result.noop", false));
    ASSERT_TRUE(pgmem::util::GetBoolOr(resp, "result.async", false));
}

TEST_CASE(test_mcp_dispatcher_store_compact_busy_returns_error) {
    auto store     = std::make_unique<BusyStoreCompactAdapter>();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::util::Json req;
    req.put("id", 22);
    req.put("method", "store.compact");
    pgmem::util::Json params;
    req.add_child("params", params);

    const auto resp = dispatcher.Handle(req);
    ASSERT_EQ(pgmem::util::GetIntOr(resp, "error.code", 0), -32001);
    ASSERT_EQ(pgmem::util::GetStringOr(resp, "error.message", ""), "store compact is busy");
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
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.describe_version", ""), "2.0.0");
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.schema_revision", ""), "2026-02-19.1");
    ASSERT_TRUE(HasPath(describe_resp, "result.generated_at_ms"));
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.server.kind", ""), "memory-server");
    ASSERT_EQ(pgmem::util::GetStringOr(describe_resp, "result.server.endpoint", ""), "/mcp");

    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.write"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.query"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.pin"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.stats"));
    ASSERT_TRUE(ArrayContainsString(describe_resp, "result.method_names", "memory.compact"));

    const auto methods_opt = describe_resp.get_child_optional("result.methods");
    ASSERT_TRUE(methods_opt.has_value());
    ASSERT_TRUE(FindNamedChild(*methods_opt, "memory") == nullptr);
    const pgmem::util::Json* write_method = FindNamedChild(*methods_opt, "memory.write");
    ASSERT_TRUE(write_method != nullptr);
    const pgmem::util::Json* query_method = FindNamedChild(*methods_opt, "memory.query");
    ASSERT_TRUE(query_method != nullptr);
    const pgmem::util::Json* stats_method = FindNamedChild(*methods_opt, "memory.stats");
    ASSERT_TRUE(stats_method != nullptr);
    ASSERT_TRUE(!write_method->get_child_optional("examples").has_value());

    ASSERT_EQ(pgmem::util::GetStringOr(*write_method, "input_schema.properties.workspace_id.type", ""), "string");
    ASSERT_EQ(pgmem::util::GetStringOr(*query_method, "output_schema.properties.hits.type", ""), "array");
    ASSERT_EQ(pgmem::util::GetStringOr(*stats_method, "output_schema.properties.index_stats.type", ""), "object");

    const auto http_errors_opt = describe_resp.get_child_optional("result.errors.http");
    ASSERT_TRUE(http_errors_opt.has_value());
    const pgmem::util::Json* invalid_json_error = FindErrorByName(*http_errors_opt, "invalid_json_http");
    ASSERT_TRUE(invalid_json_error != nullptr);
    ASSERT_EQ(invalid_json_error->get<int>("status", 0), 400);

    const auto jsonrpc_errors_opt = describe_resp.get_child_optional("result.errors.jsonrpc");
    ASSERT_TRUE(jsonrpc_errors_opt.has_value());
    const pgmem::util::Json* method_not_found_error = FindErrorByName(*jsonrpc_errors_opt, "method_not_found");
    ASSERT_TRUE(method_not_found_error != nullptr);
    ASSERT_EQ(method_not_found_error->get<int>("code", 0), -32601);
}

TEST_CASE(test_mcp_dispatcher_describe_examples_toggle) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::util::Json default_req;
    default_req.put("id", 101);
    default_req.put("method", "memory.describe");
    const auto default_resp = dispatcher.Handle(default_req);
    const auto default_methods_opt = default_resp.get_child_optional("result.methods");
    ASSERT_TRUE(default_methods_opt.has_value());
    const pgmem::util::Json* default_query_method = FindNamedChild(*default_methods_opt, "memory.query");
    ASSERT_TRUE(default_query_method != nullptr);
    ASSERT_TRUE(!default_query_method->get_child_optional("examples").has_value());

    pgmem::util::Json req_with_examples;
    req_with_examples.put("id", 102);
    req_with_examples.put("method", "memory.describe");
    pgmem::util::Json params;
    params.put("include_examples", true);
    req_with_examples.add_child("params", params);

    const auto resp_with_examples = dispatcher.Handle(req_with_examples);
    const auto methods_opt = resp_with_examples.get_child_optional("result.methods");
    ASSERT_TRUE(methods_opt.has_value());
    const pgmem::util::Json* query_method = FindNamedChild(*methods_opt, "memory.query");
    ASSERT_TRUE(query_method != nullptr);
    const pgmem::util::Json* write_method = FindNamedChild(*methods_opt, "memory.write");
    ASSERT_TRUE(write_method != nullptr);
    ASSERT_TRUE(query_method->get_child_optional("examples").has_value());
    ASSERT_TRUE(write_method->get_child_optional("examples").has_value());

    const pgmem::util::Json* describe_method = FindNamedChild(*methods_opt, "memory.describe");
    ASSERT_TRUE(describe_method != nullptr);
    ASSERT_EQ(
        pgmem::util::GetStringOr(*describe_method, "input_schema.properties.include_examples.type", ""),
        "boolean");
    ASSERT_TRUE(
        !pgmem::util::GetBoolOr(*describe_method, "input_schema.properties.include_examples.default", true));
}
