#include <butil/third_party/rapidjson/document.h>
#include <butil/third_party/rapidjson/stringbuffer.h>
#include <butil/third_party/rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "pgmem/config/defaults.h"
#include "pgmem/core/memory_engine.h"
#include "pgmem/core/retriever.h"
#include "pgmem/mcp/mcp_dispatcher.h"
#include "pgmem/net/brpc_runtime.h"
#include "pgmem/net/http_server.h"
#include "pgmem/store/io_uring_probe.h"
#include "pgmem/store/store_adapter.h"
#include "pgmem/util/json.h"

namespace {

namespace rj = BUTIL_RAPIDJSON_NAMESPACE;

std::atomic<bool> g_running{true};
constexpr const char* kIoUringUnavailablePrefix = "[pgmemd] io_uring unavailable for eloqstore backend: ";

void OnSignal(int) { g_running.store(false); }

std::string ToLower(std::string v) {
    for (char& c : v) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v;
}

std::string JsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
            case '\"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0x0f]);
                    out.push_back(hex[c & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string JsonQuote(const std::string& input) { return std::string("\"") + JsonEscape(input) + "\""; }

std::string JsonStringifyRapid(const rj::Value& value) {
    rj::StringBuffer buffer;
    rj::Writer<rj::StringBuffer> writer(buffer);
    value.Accept(writer);
    return buffer.GetString();
}

std::string JsonRpcIdLiteral(const rj::Document& doc) {
    if (!doc.HasMember("id")) {
        return "null";
    }
    const auto& id = doc["id"];
    if (id.IsNull()) {
        return "null";
    }
    if (id.IsBool()) {
        return id.GetBool() ? "true" : "false";
    }
    if (id.IsInt64()) {
        return std::to_string(id.GetInt64());
    }
    if (id.IsUint64()) {
        return std::to_string(id.GetUint64());
    }
    if (id.IsDouble()) {
        std::ostringstream oss;
        oss << id.GetDouble();
        return oss.str();
    }
    if (id.IsString()) {
        return JsonQuote(id.GetString());
    }
    return "null";
}

std::string JsonRpcError(const std::string& id_literal, int code, const std::string& message) {
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id_literal + ",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":" + JsonQuote(message) + "}}";
}

std::string JsonRpcResult(const std::string& id_literal, const std::string& result_json) {
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id_literal + ",\"result\":" + result_json + "}";
}

std::string InitializeResultJson() {
    return R"({"protocolVersion":"2024-11-05","capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"poorguy-mem","version":"2.0.0"},"instructions":"Use tools memory.write/query/pin/stats/compact. Call memory.describe for full schema."})";
}

const pgmem::util::Json* FindMethodSpec(const pgmem::util::Json& methods, const std::string& method_name) {
    for (const auto& node : methods) {
        if (node.first == method_name) {
            return &node.second;
        }
    }
    return nullptr;
}

std::string ToolsListResultJson(pgmem::mcp::McpDispatcher* dispatcher) {
    if (dispatcher == nullptr) {
        return R"({"tools":[]})";
    }

    const auto describe = dispatcher->Describe();
    const auto method_names_opt = describe.get_child_optional("method_names");
    const auto methods_opt = describe.get_child_optional("methods");

    pgmem::util::Json tools;
    if (method_names_opt && methods_opt) {
        for (const auto& method_name_node : *method_names_opt) {
            const std::string method_name = method_name_node.second.get_value<std::string>();
            if (method_name.rfind("memory.", 0) != 0) {
                continue;
            }

            const pgmem::util::Json* method_spec = FindMethodSpec(*methods_opt, method_name);
            if (method_spec == nullptr) {
                continue;
            }

            pgmem::util::Json tool;
            tool.put("name", method_name);
            tool.put("description", pgmem::util::GetStringOr(*method_spec, "summary", ""));

            if (const auto input_schema_opt = method_spec->get_child_optional("input_schema")) {
                tool.add_child("inputSchema", *input_schema_opt);
            } else {
                pgmem::util::Json empty_schema;
                empty_schema.put("type", "object");
                tool.add_child("inputSchema", empty_schema);
            }

            tools.push_back(std::make_pair("", tool));
        }
    }

    pgmem::util::Json out;
    out.add_child("tools", tools);
    return pgmem::util::ToJsonString(out, false);
}

std::string ToolCallResultJson(const std::string& text, bool is_error) {
    return std::string("{\"content\":[{\"type\":\"text\",\"text\":") + JsonQuote(text) +
           "}],\"isError\":" + (is_error ? "true" : "false") + "}";
}

bool ParseJsonToPtree(const std::string& text, pgmem::util::Json* out, std::string* error) {
    return pgmem::util::ParseJson(text, out, error);
}

std::string PtreeChildToJson(const pgmem::util::Json& json, const std::string& path) {
    if (const auto child_opt = json.get_child_optional(path)) {
        return pgmem::util::ToJsonString(*child_opt, false);
    }
    return "{}";
}

bool ParseBoolLiteral(const char* text, bool* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    std::string lowered(text);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowered == "true" || lowered == "1") {
        *out = true;
        return true;
    }
    if (lowered == "false" || lowered == "0") {
        *out = false;
        return true;
    }
    return false;
}

bool ParseInt64Literal(const char* text, int64_t* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    errno       = 0;
    char* end   = nullptr;
    long long v = std::strtoll(text, &end, 10);
    if (errno != 0 || end == text || (end != nullptr && *end != '\0')) {
        return false;
    }
    *out = static_cast<int64_t>(v);
    return true;
}

bool ParseDoubleLiteral(const char* text, double* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    errno     = 0;
    char* end = nullptr;
    double v  = std::strtod(text, &end);
    if (errno != 0 || end == text || (end != nullptr && *end != '\0')) {
        return false;
    }
    *out = v;
    return true;
}

void ConvertValueToBoolIfString(rj::Value* value) {
    if (value == nullptr || !value->IsString()) {
        return;
    }
    bool parsed = false;
    if (ParseBoolLiteral(value->GetString(), &parsed)) {
        value->SetBool(parsed);
    }
}

void ConvertValueToIntIfString(rj::Value* value) {
    if (value == nullptr || !value->IsString()) {
        return;
    }
    int64_t parsed = 0;
    if (ParseInt64Literal(value->GetString(), &parsed)) {
        value->SetInt64(parsed);
    }
}

void ConvertValueToDoubleIfString(rj::Value* value) {
    if (value == nullptr || !value->IsString()) {
        return;
    }
    double parsed = 0.0;
    if (ParseDoubleLiteral(value->GetString(), &parsed)) {
        value->SetDouble(parsed);
    }
}

rj::Value* FindMemberObject(rj::Value* value, const char* key) {
    if (value == nullptr || key == nullptr || !value->IsObject() || !value->HasMember(key)) {
        return nullptr;
    }
    return &(*value)[key];
}

void EnsureArrayMember(rj::Value* object, const char* key, rj::Document::AllocatorType& alloc) {
    if (object == nullptr || key == nullptr || !object->IsObject() || !object->HasMember(key)) {
        return;
    }

    rj::Value& field = (*object)[key];
    if (field.IsArray()) {
        return;
    }

    if (field.IsString() && field.GetStringLength() == 0) {
        field.SetArray();
        return;
    }

    if (field.IsString()) {
        std::string copy = field.GetString();
        field.SetArray();
        rj::Value str;
        str.SetString(copy.c_str(), static_cast<rj::SizeType>(copy.size()), alloc);
        field.PushBack(str, alloc);
    }
}

void EnsureObjectMember(rj::Value* object, const char* key) {
    if (object == nullptr || key == nullptr || !object->IsObject() || !object->HasMember(key)) {
        return;
    }

    rj::Value& field = (*object)[key];
    if (field.IsObject()) {
        return;
    }
    if (field.IsString() && field.GetStringLength() == 0) {
        field.SetObject();
    }
}

void NormalizeSchemaDefaultByType(rj::Value* node) {
    if (node == nullptr || !node->IsObject() || !node->HasMember("type") || !node->HasMember("default")) {
        return;
    }
    const rj::Value& type = (*node)["type"];
    if (!type.IsString()) {
        return;
    }

    rj::Value& default_value = (*node)["default"];
    const std::string type_name = type.GetString();
    if (type_name == "boolean") {
        ConvertValueToBoolIfString(&default_value);
    } else if (type_name == "integer") {
        ConvertValueToIntIfString(&default_value);
    } else if (type_name == "number") {
        ConvertValueToDoubleIfString(&default_value);
    }
}

void NormalizeDescribeNode(rj::Value* node) {
    if (node == nullptr) {
        return;
    }
    if (node->IsObject()) {
        if (node->HasMember("additionalProperties")) {
            ConvertValueToBoolIfString(&(*node)["additionalProperties"]);
        }
        NormalizeSchemaDefaultByType(node);
        for (auto it = node->MemberBegin(); it != node->MemberEnd(); ++it) {
            const std::string key = it->name.GetString();
            if (key == "generated_at_ms" || key == "code" || key == "status") {
                ConvertValueToIntIfString(&it->value);
            } else if (key == "sync_routes_available") {
                ConvertValueToBoolIfString(&it->value);
            }
            NormalizeDescribeNode(&it->value);
        }
        return;
    }
    if (node->IsArray()) {
        for (auto it = node->Begin(); it != node->End(); ++it) {
            NormalizeDescribeNode(&(*it));
        }
    }
}

void NormalizeQueryResult(rj::Value* result, rj::Document::AllocatorType& alloc) {
    if (result == nullptr || !result->IsObject()) {
        return;
    }

    EnsureArrayMember(result, "hits", alloc);
    rj::Value* hits = FindMemberObject(result, "hits");
    if (hits != nullptr && hits->IsArray()) {
        for (auto it = hits->Begin(); it != hits->End(); ++it) {
            rj::Value& hit = *it;
            if (!hit.IsObject()) {
                continue;
            }
            EnsureArrayMember(&hit, "tags", alloc);
            EnsureObjectMember(&hit, "metadata");
            ConvertValueToIntIfString(FindMemberObject(&hit, "updated_at_ms"));
            ConvertValueToBoolIfString(FindMemberObject(&hit, "pinned"));

            rj::Value* scores = FindMemberObject(&hit, "scores");
            if (scores != nullptr && scores->IsObject()) {
                ConvertValueToDoubleIfString(FindMemberObject(scores, "sparse"));
                ConvertValueToDoubleIfString(FindMemberObject(scores, "dense"));
                ConvertValueToDoubleIfString(FindMemberObject(scores, "freshness"));
                ConvertValueToDoubleIfString(FindMemberObject(scores, "pin"));
                ConvertValueToDoubleIfString(FindMemberObject(scores, "final"));
            }
        }
    }

    rj::Value* debug = FindMemberObject(result, "debug_stats");
    if (debug != nullptr && debug->IsObject()) {
        ConvertValueToIntIfString(FindMemberObject(debug, "sparse_candidates"));
        ConvertValueToIntIfString(FindMemberObject(debug, "dense_candidates"));
        ConvertValueToIntIfString(FindMemberObject(debug, "merged_candidates"));

        rj::Value* latency = FindMemberObject(debug, "latency_ms");
        if (latency != nullptr && latency->IsObject()) {
            ConvertValueToDoubleIfString(FindMemberObject(latency, "sparse"));
            ConvertValueToDoubleIfString(FindMemberObject(latency, "dense"));
            ConvertValueToDoubleIfString(FindMemberObject(latency, "rerank"));
            ConvertValueToDoubleIfString(FindMemberObject(latency, "total"));
        }
    }
}

void NormalizeResultByMethod(rj::Value* result, const std::string& method, rj::Document::AllocatorType& alloc) {
    if (result == nullptr || !result->IsObject()) {
        return;
    }

    if (method == "memory.write") {
        ConvertValueToBoolIfString(FindMemberObject(result, "ok"));
        ConvertValueToIntIfString(FindMemberObject(result, "index_generation"));
        EnsureArrayMember(result, "stored_ids", alloc);
        EnsureArrayMember(result, "deduped_ids", alloc);
        EnsureArrayMember(result, "warnings", alloc);
        return;
    }

    if (method == "memory.query") {
        NormalizeQueryResult(result, alloc);
        return;
    }

    if (method == "memory.pin") {
        ConvertValueToBoolIfString(FindMemberObject(result, "ok"));
        return;
    }

    if (method == "memory.stats") {
        ConvertValueToDoubleIfString(FindMemberObject(result, "p95_read_ms"));
        ConvertValueToDoubleIfString(FindMemberObject(result, "p95_write_ms"));
        ConvertValueToDoubleIfString(FindMemberObject(result, "token_reduction_ratio"));
        ConvertValueToDoubleIfString(FindMemberObject(result, "fallback_rate"));
        ConvertValueToIntIfString(FindMemberObject(result, "mem_used_bytes"));
        ConvertValueToIntIfString(FindMemberObject(result, "disk_used_bytes"));
        ConvertValueToIntIfString(FindMemberObject(result, "resident_used_bytes"));
        ConvertValueToIntIfString(FindMemberObject(result, "resident_limit_bytes"));
        ConvertValueToIntIfString(FindMemberObject(result, "resident_evicted_count"));
        ConvertValueToIntIfString(FindMemberObject(result, "disk_fallback_search_count"));
        ConvertValueToIntIfString(FindMemberObject(result, "item_count"));
        ConvertValueToIntIfString(FindMemberObject(result, "tombstone_count"));
        ConvertValueToIntIfString(FindMemberObject(result, "gc_last_run_ms"));
        ConvertValueToIntIfString(FindMemberObject(result, "gc_evicted_count"));
        ConvertValueToBoolIfString(FindMemberObject(result, "capacity_blocked"));

        rj::Value* index_stats = FindMemberObject(result, "index_stats");
        if (index_stats != nullptr && index_stats->IsObject()) {
            ConvertValueToIntIfString(FindMemberObject(index_stats, "segment_count"));
            ConvertValueToIntIfString(FindMemberObject(index_stats, "posting_terms"));
            ConvertValueToIntIfString(FindMemberObject(index_stats, "vector_count"));
            ConvertValueToDoubleIfString(FindMemberObject(index_stats, "query_cache_hit_rate"));
            ConvertValueToDoubleIfString(FindMemberObject(index_stats, "dense_probe_count_p95"));
            ConvertValueToIntIfString(FindMemberObject(index_stats, "cold_rehydrate_count"));
        }
        return;
    }

    if (method == "memory.compact") {
        ConvertValueToBoolIfString(FindMemberObject(result, "triggered"));
        ConvertValueToBoolIfString(FindMemberObject(result, "capacity_blocked"));
        ConvertValueToIntIfString(FindMemberObject(result, "mem_before_bytes"));
        ConvertValueToIntIfString(FindMemberObject(result, "disk_before_bytes"));
        ConvertValueToIntIfString(FindMemberObject(result, "mem_after_bytes"));
        ConvertValueToIntIfString(FindMemberObject(result, "disk_after_bytes"));
        ConvertValueToIntIfString(FindMemberObject(result, "summarized_count"));
        ConvertValueToIntIfString(FindMemberObject(result, "tombstoned_count"));
        ConvertValueToIntIfString(FindMemberObject(result, "deleted_count"));
        ConvertValueToIntIfString(FindMemberObject(result, "segments_before"));
        ConvertValueToIntIfString(FindMemberObject(result, "segments_after"));
        ConvertValueToIntIfString(FindMemberObject(result, "postings_reclaimed"));
        ConvertValueToIntIfString(FindMemberObject(result, "vectors_reclaimed"));
        return;
    }

    if (method == "store.compact") {
        ConvertValueToBoolIfString(FindMemberObject(result, "triggered"));
        ConvertValueToBoolIfString(FindMemberObject(result, "noop"));
        ConvertValueToBoolIfString(FindMemberObject(result, "busy"));
        ConvertValueToBoolIfString(FindMemberObject(result, "async"));
        ConvertValueToIntIfString(FindMemberObject(result, "partition_count"));
        return;
    }

    if (method == "memory.describe") {
        NormalizeDescribeNode(result);
        return;
    }

    if (method == "tools.list") {
        NormalizeDescribeNode(result);
    }
}

std::string NormalizeMcpResponseJson(const std::string& raw_json, const std::string& method) {
    rj::Document doc;
    doc.Parse(raw_json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        return raw_json;
    }

    ConvertValueToIntIfString(FindMemberObject(&doc, "id"));
    NormalizeResultByMethod(FindMemberObject(&doc, "result"), method, doc.GetAllocator());

    rj::Value* error = FindMemberObject(&doc, "error");
    if (error != nullptr && error->IsObject()) {
        ConvertValueToIntIfString(FindMemberObject(error, "code"));
    }

    rj::StringBuffer buffer;
    rj::Writer<rj::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return buffer.GetString();
}

std::string NormalizeMcpResultJson(const std::string& raw_json, const std::string& method) {
    rj::Document doc;
    doc.Parse(raw_json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        return raw_json;
    }

    NormalizeResultByMethod(&doc, method, doc.GetAllocator());

    rj::StringBuffer buffer;
    rj::Writer<rj::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return buffer.GetString();
}

pgmem::net::HttpResponse HandleMcpJsonRpc(const std::string& body, pgmem::mcp::McpDispatcher* dispatcher) {
    pgmem::net::HttpResponse resp;
    rj::Document doc;
    doc.Parse(body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        resp.status_code = 400;
        resp.body        = "{\"error\":\"invalid JSON\"}";
        return resp;
    }

    if (!doc.HasMember("jsonrpc")) {
        resp.status_code = 0;
        return resp;
    }

    const std::string id_literal = JsonRpcIdLiteral(doc);
    if (!doc["jsonrpc"].IsString() || std::string(doc["jsonrpc"].GetString()) != "2.0") {
        resp.status_code = 200;
        resp.body        = JsonRpcError(id_literal, -32600, "invalid request: jsonrpc must be 2.0");
        return resp;
    }

    if (!doc.HasMember("method") || !doc["method"].IsString()) {
        resp.status_code = 200;
        resp.body        = JsonRpcError(id_literal, -32600, "invalid request: method must be string");
        return resp;
    }

    const std::string method = doc["method"].GetString();
    if (method == "notifications/initialized") {
        resp.status_code = 204;
        resp.body.clear();
        return resp;
    }
    if (method == "initialize") {
        resp.status_code = 200;
        resp.body        = JsonRpcResult(id_literal, InitializeResultJson());
        return resp;
    }
    if (method == "ping") {
        resp.status_code = 200;
        resp.body        = JsonRpcResult(id_literal, "{}");
        return resp;
    }
    if (method == "tools/list") {
        resp.status_code = 200;
        const std::string result_json = NormalizeMcpResultJson(ToolsListResultJson(dispatcher), "tools.list");
        resp.body                     = JsonRpcResult(id_literal, result_json);
        return resp;
    }
    if (method == "tools/call") {
        if (!doc.HasMember("params") || !doc["params"].IsObject()) {
            resp.status_code = 200;
            resp.body        = JsonRpcError(id_literal, -32602, "invalid params: params object is required");
            return resp;
        }
        const auto& params = doc["params"];
        if (!params.HasMember("name") || !params["name"].IsString()) {
            resp.status_code = 200;
            resp.body        = JsonRpcError(id_literal, -32602, "invalid params: tool name is required");
            return resp;
        }

        const std::string tool_name = params["name"].GetString();
        if (tool_name.rfind("memory.", 0) != 0) {
            resp.status_code = 200;
            resp.body        = JsonRpcError(id_literal, -32601, "tool not found");
            return resp;
        }

        pgmem::util::Json mem_req;
        mem_req.put("id", "tool-call");
        mem_req.put("method", tool_name);

        if (params.HasMember("arguments")) {
            if (!params["arguments"].IsObject()) {
                resp.status_code = 200;
                resp.body        = JsonRpcError(id_literal, -32602, "invalid params: arguments must be object");
                return resp;
            }
            pgmem::util::Json mem_params;
            std::string parse_error;
            if (!ParseJsonToPtree(JsonStringifyRapid(params["arguments"]), &mem_params, &parse_error)) {
                resp.status_code = 200;
                resp.body        = JsonRpcError(id_literal, -32602, "invalid params: failed to parse arguments");
                return resp;
            }
            mem_req.add_child("params", mem_params);
        }

        const auto out         = dispatcher->Handle(mem_req);
        const bool has_error   = out.get_child_optional("error").has_value();
        const std::string text = has_error ? PtreeChildToJson(out, "error") : PtreeChildToJson(out, "result");
        resp.status_code       = 200;
        resp.body              = JsonRpcResult(id_literal, ToolCallResultJson(text, has_error));
        return resp;
    }

    if (method.rfind("memory.", 0) == 0 || method == "store.compact") {
        pgmem::util::Json mem_req;
        mem_req.put("id", "jsonrpc-call");
        mem_req.put("method", method);

        if (doc.HasMember("params")) {
            if (!doc["params"].IsObject()) {
                resp.status_code = 200;
                resp.body        = JsonRpcError(id_literal, -32602, "invalid params: params must be object");
                return resp;
            }
            pgmem::util::Json mem_params;
            std::string parse_error;
            if (!ParseJsonToPtree(JsonStringifyRapid(doc["params"]), &mem_params, &parse_error)) {
                resp.status_code = 200;
                resp.body        = JsonRpcError(id_literal, -32602, "invalid params: failed to parse params");
                return resp;
            }
            mem_req.add_child("params", mem_params);
        }

        const auto out = dispatcher->Handle(mem_req);
        if (out.get_child_optional("error").has_value()) {
            const int code            = pgmem::util::GetIntOr(out, "error.code", -32603);
            const std::string message = pgmem::util::GetStringOr(out, "error.message", "internal error");
            resp.status_code          = 200;
            resp.body                 = JsonRpcError(id_literal, code, message);
            return resp;
        }

        const std::string normalized_result = NormalizeMcpResultJson(PtreeChildToJson(out, "result"), method);
        resp.status_code                    = 200;
        resp.body                           = JsonRpcResult(id_literal, normalized_result);
        return resp;
    }

    resp.status_code = 200;
    resp.body        = JsonRpcError(id_literal, -32601, "method not found");
    return resp;
}

const char* DefaultStoreBackend() {
#ifdef PGMEM_DEFAULT_STORE_BACKEND
    return PGMEM_DEFAULT_STORE_BACKEND;
#else
    return pgmem::config::kDefaultStoreBackend;
#endif
}

struct Args {
    std::string host{pgmem::config::kDefaultHost};
    int port{pgmem::config::kDefaultMcpPort};
    std::string store_backend{DefaultStoreBackend()};
    std::string store_root{pgmem::config::kDefaultStoreRoot};
    int core_number{pgmem::config::kDefaultCoreNumber};
    uint64_t mem_budget_mb{pgmem::config::kDefaultMemBudgetMb};
    uint64_t disk_budget_gb{pgmem::config::kDefaultDiskBudgetGb};
    bool test_force_io_uring_unavailable{false};
    bool invalid{false};
    std::string invalid_message;
};

const char* AllowedPgmemdFlags() {
    return "--host --port --store-backend --store-root --core-number --mem-budget-mb --disk-budget-gb";
}

Args ParseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (key == "--port" && i + 1 < argc) {
            args.port = std::atoi(argv[++i]);
        } else if (key == "--store-backend" && i + 1 < argc) {
            args.store_backend = argv[++i];
        } else if (key == "--store-root" && i + 1 < argc) {
            args.store_root = argv[++i];
        } else if (key == "--core-number" && i + 1 < argc) {
            args.core_number = std::atoi(argv[++i]);
        } else if (key == "--mem-budget-mb" && i + 1 < argc) {
            args.mem_budget_mb = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (key == "--disk-budget-gb" && i + 1 < argc) {
            args.disk_budget_gb = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (key == "--test-force-io-uring-unavailable") {
            args.test_force_io_uring_unavailable = true;
        } else {
            args.invalid         = true;
            args.invalid_message = "unknown argument: " + key + "; allowed flags: " + AllowedPgmemdFlags();
            break;
        }
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);
    if (args.invalid) {
        std::cerr << "[pgmemd] invalid arguments: " << args.invalid_message << "\n";
        return 2;
    }

    pgmem::net::BrpcRuntimeOptions runtime_options;
    runtime_options.core_number          = args.core_number;
    runtime_options.event_dispatcher_num = 0;

    pgmem::net::BrpcRuntimeState runtime_state;
    std::string error;
    if (!pgmem::net::ApplyBrpcRuntimeOptions(runtime_options, &runtime_state, &error)) {
        std::cerr << "[pgmemd] failed to apply brpc runtime options: " << error << "\n";
        return 2;
    }
    if (args.core_number > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
        std::cerr << "[pgmemd] core_number is too large for eloqstore num_threads\n";
        return 2;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    pgmem::store::StoreAdapterConfig store_config;
    store_config.backend        = args.store_backend;
    store_config.root_path      = args.store_root;
    store_config.num_threads    = static_cast<uint16_t>(args.core_number);
    store_config.num_partitions = static_cast<uint32_t>(args.core_number);
    store_config.mem_budget_mb  = args.mem_budget_mb;
    store_config.disk_budget_gb = args.disk_budget_gb;

    const std::string requested_backend = ToLower(store_config.backend);
    if (requested_backend == "eloqstore") {
        std::string probe_error;
        if (!pgmem::store::IsIoUringAvailable(&probe_error, args.test_force_io_uring_unavailable)) {
            std::cerr << kIoUringUnavailablePrefix << probe_error << "\n";
            return 1;
        }
    }

    std::unique_ptr<pgmem::store::IStoreAdapter> store = pgmem::store::CreateStoreAdapter(store_config, &error);
    if (!store) {
        std::cerr << "[pgmemd] failed to initialize backend '" << store_config.backend << "': " << error << "\n";
        return 1;
    }

    const std::string effective_backend = ToLower(store_config.backend);
    std::cerr << "[pgmemd] runtime: core_number=" << runtime_state.resolved_core_number
              << " event_dispatcher_num=" << runtime_state.resolved_event_dispatcher_num
              << " store_threads=" << store_config.num_threads << " store_partitions=" << store_config.num_partitions
              << " network_io_uring=" << (runtime_state.network_io_uring_enabled ? "true" : "false")
              << " backend=" << effective_backend << " write_ack_mode=durable\n";

    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions engine_options;
    engine_options.mem_budget_mb     = args.mem_budget_mb;
    engine_options.disk_budget_gb    = args.disk_budget_gb;
    engine_options.effective_backend = effective_backend;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), pgmem::config::kDefaultNodeIdLocal,
                                     engine_options);
    std::atomic<bool> ready{false};

    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::net::HttpServerOptions server_options;
    server_options.num_threads = runtime_state.resolved_core_number;
    pgmem::net::HttpServer server(args.host, args.port, server_options);
    server.RegisterRoute("GET", "/health", [&](const pgmem::net::HttpRequest&) {
        pgmem::net::HttpResponse resp;
        if (!ready.load(std::memory_order_acquire)) {
            resp.status_code = 503;
            resp.body        = "{\"status\":\"starting\"}";
            return resp;
        }
        resp.status_code = 200;
        resp.body        = "{\"status\":\"ok\"}";
        return resp;
    });

    server.RegisterRoute("POST", "/mcp", [&](const pgmem::net::HttpRequest& req) {
        if (!ready.load(std::memory_order_acquire)) {
            pgmem::net::HttpResponse resp;
            resp.status_code = 503;
            resp.body        = "{\"error\":\"server is warming up\"}";
            return resp;
        }

        auto rpc_resp = HandleMcpJsonRpc(req.body, &dispatcher);
        if (rpc_resp.status_code != 0) {
            return rpc_resp;
        }

        pgmem::net::HttpResponse resp;
        pgmem::util::Json json;
        std::string parse_error;
        if (!pgmem::util::ParseJson(req.body, &json, &parse_error)) {
            resp.status_code = 400;
            resp.body        = std::string("{\"error\":\"invalid JSON: ") + parse_error + "\"}";
            return resp;
        }

        const auto out           = dispatcher.Handle(json);
        resp.status_code         = 200;
        const std::string method = pgmem::util::GetStringOr(json, "method", "");
        resp.body                = NormalizeMcpResponseJson(pgmem::util::ToJsonString(out, false), method);
        return resp;
    });

    server.RegisterRoute("GET", "/mcp/describe", [&](const pgmem::net::HttpRequest&) {
        pgmem::net::HttpResponse resp;
        if (!ready.load(std::memory_order_acquire)) {
            resp.status_code = 503;
            resp.body        = "{\"error\":\"server is warming up\"}";
            return resp;
        }
        resp.status_code = 200;
        resp.body = NormalizeMcpResultJson(pgmem::util::ToJsonString(dispatcher.Describe(), false), "memory.describe");
        return resp;
    });

    if (!server.Start(&error)) {
        std::cerr << "[pgmemd] start failed: " << error << "\n";
        return 1;
    }

    if (!engine.Warmup(&error)) {
        std::cerr << "[pgmemd] warmup failed: " << error << "\n";
        server.Stop();
        return 1;
    }
    ready.store(true, std::memory_order_release);

    std::cerr << "[pgmemd] listening on http://" << args.host << ":" << args.port << "/mcp\n";

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    server.Stop();
    return 0;
}
