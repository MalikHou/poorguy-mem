#include <butil/third_party/rapidjson/document.h>
#include <butil/third_party/rapidjson/stringbuffer.h>
#include <butil/third_party/rapidjson/writer.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
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
    return R"({"protocolVersion":"2024-11-05","capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"poorguy-mem","version":"1.0.0"},"instructions":"Use tools memory.bootstrap/search/commit_turn/pin/stats/compact. Call memory.describe for full schema."})";
}

std::string ToolsListResultJson() {
    return R"({"tools":[{"name":"memory.describe","description":"Return full memory API contract including semantics, input schema, output schema, and error contract.","inputSchema":{"type":"object","properties":{"include_examples":{"type":"boolean"}}}},{"name":"memory.bootstrap","description":"Bootstrap session context by recalling relevant memory and summary.","inputSchema":{"type":"object","properties":{"workspace_id":{"type":"string"},"session_id":{"type":"string"},"task_text":{"type":"string"},"open_files":{"type":"array","items":{"type":"string"}},"token_budget":{"type":"integer"}}}},{"name":"memory.commit_turn","description":"Persist one conversation turn into memory.","inputSchema":{"type":"object","properties":{"workspace_id":{"type":"string"},"session_id":{"type":"string"},"user_text":{"type":"string"},"assistant_text":{"type":"string"},"code_snippets":{"type":"array","items":{"type":"string"}},"commands":{"type":"array","items":{"type":"string"}}}}},{"name":"memory.search","description":"Search memory using lexical and semantic ranking.","inputSchema":{"type":"object","properties":{"workspace_id":{"type":"string"},"query":{"type":"string"},"top_k":{"type":"integer"},"token_budget":{"type":"integer"}}}},{"name":"memory.pin","description":"Pin or unpin a memory record.","inputSchema":{"type":"object","properties":{"workspace_id":{"type":"string"},"memory_id":{"type":"string"},"pin":{"type":"boolean"}},"required":["memory_id"]}},{"name":"memory.stats","description":"Return runtime stats including effective backend and pending accepted writes.","inputSchema":{"type":"object","properties":{"workspace_id":{"type":"string"},"window":{"type":"string"}}}},{"name":"memory.compact","description":"Trigger compaction and return compaction counters.","inputSchema":{"type":"object","properties":{"workspace_id":{"type":"string"}}}}]})";
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
        resp.body        = JsonRpcResult(id_literal, ToolsListResultJson());
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
        if (tool_name.rfind("memory.", 0) != 0 && tool_name != "mcp.describe") {
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

    if (method.rfind("memory.", 0) == 0 || method == "mcp.describe") {
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

        resp.status_code = 200;
        resp.body        = JsonRpcResult(id_literal, PtreeChildToJson(out, "result"));
        return resp;
    }

    resp.status_code = 200;
    resp.body        = JsonRpcError(id_literal, -32601, "method not found");
    return resp;
}

bool ParseBoolFlag(const std::string& value, bool fallback) {
    const std::string v = ToLower(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        return false;
    }
    return fallback;
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
    bool enable_io_uring_network_engine{pgmem::config::kDefaultEnableIoUringNetworkEngine};
    bool append_mode{pgmem::config::kDefaultAppendMode};
    bool enable_compression{pgmem::config::kDefaultEnableCompression};
    uint64_t mem_budget_mb{pgmem::config::kDefaultMemBudgetMb};
    uint64_t disk_budget_gb{pgmem::config::kDefaultDiskBudgetGb};
    double gc_high_watermark{pgmem::config::kDefaultGcHighWatermark};
    double gc_low_watermark{pgmem::config::kDefaultGcLowWatermark};
    uint32_t gc_batch_size{pgmem::config::kDefaultGcBatchSize};
    uint64_t max_record_bytes{pgmem::config::kDefaultMaxRecordBytes};
    bool enable_tombstone_gc{pgmem::config::kDefaultEnableTombstoneGc};
    uint32_t shutdown_drain_timeout_ms{pgmem::config::kDefaultShutdownDrainTimeoutMs};
    bool invalid{false};
    std::string invalid_message;
};

void MarkRemovedFlag(Args* args, const std::string& key, const std::string& guidance) {
    args->invalid         = true;
    args->invalid_message = "flag " + key + " is removed; " + guidance;
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
        } else if (key == "--enable-io-uring-network-engine" && i + 1 < argc) {
            args.enable_io_uring_network_engine = ParseBoolFlag(argv[++i], args.enable_io_uring_network_engine);
        } else if (key == "--append-mode" && i + 1 < argc) {
            args.append_mode = ParseBoolFlag(argv[++i], args.append_mode);
        } else if (key == "--enable-compression" && i + 1 < argc) {
            args.enable_compression = ParseBoolFlag(argv[++i], args.enable_compression);
        } else if (key == "--mem-budget-mb" && i + 1 < argc) {
            args.mem_budget_mb = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (key == "--disk-budget-gb" && i + 1 < argc) {
            args.disk_budget_gb = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (key == "--gc-high-watermark" && i + 1 < argc) {
            args.gc_high_watermark = std::strtod(argv[++i], nullptr);
        } else if (key == "--gc-low-watermark" && i + 1 < argc) {
            args.gc_low_watermark = std::strtod(argv[++i], nullptr);
        } else if (key == "--gc-batch-size" && i + 1 < argc) {
            args.gc_batch_size = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (key == "--max-record-bytes" && i + 1 < argc) {
            args.max_record_bytes = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (key == "--enable-tombstone-gc" && i + 1 < argc) {
            args.enable_tombstone_gc = ParseBoolFlag(argv[++i], args.enable_tombstone_gc);
        } else if (key == "--shutdown-drain-timeout-ms" && i + 1 < argc) {
            args.shutdown_drain_timeout_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (key == "--event-dispatcher-num") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "event_dispatcher_num now follows core-number automatically");
        } else if (key == "--store-partitions") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "partitions are derived automatically as partitions=core-number");
        } else if (key == "--allow-inmemory-fallback") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "eloqstore init failure now always downgrades to inmemory");
        } else if (key == "--write-ack-mode") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "write_ack_mode is fixed to accepted in local mode");
        } else if (key == "--volatile-flush-interval-ms" || key == "--volatile-max-pending-ops") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "volatile queue tuning is internal-only in local mode");
        } else if (key == "--node-id") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "node-id is fixed to local for single-node mode");
        } else if (key == "--sync-host" || key == "--sync-port") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "sync flags are removed in single-node local mode");
        } else if (key.rfind("--s3-", 0) == 0) {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "S3 backend options are removed from local mode");
        } else {
            args.invalid         = true;
            args.invalid_message = "unknown argument: " + key;
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
    runtime_options.core_number                    = args.core_number;
    runtime_options.event_dispatcher_num           = 0;
    runtime_options.enable_io_uring_network_engine = args.enable_io_uring_network_engine;

    pgmem::net::BrpcRuntimeState runtime_state;
    std::string error;
    if (!pgmem::net::ApplyBrpcRuntimeOptions(runtime_options, &runtime_state, &error)) {
        std::cerr << "[pgmemd] failed to apply brpc runtime options: " << error << "\n";
        return 2;
    }
    if (runtime_state.resolved_core_number > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
        std::cerr << "[pgmemd] core_number is too large for eloqstore num_threads\n";
        return 2;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    pgmem::store::StoreAdapterConfig store_config;
    store_config.backend             = args.store_backend;
    store_config.root_path           = args.store_root;
    store_config.num_threads         = static_cast<uint16_t>(runtime_state.resolved_core_number);
    store_config.num_partitions      = static_cast<uint32_t>(runtime_state.resolved_core_number);
    store_config.append_mode         = args.append_mode;
    store_config.enable_compression  = args.enable_compression;
    store_config.mem_budget_mb       = args.mem_budget_mb;
    store_config.disk_budget_gb      = args.disk_budget_gb;
    store_config.gc_high_watermark   = args.gc_high_watermark;
    store_config.gc_low_watermark    = args.gc_low_watermark;
    store_config.gc_batch_size       = args.gc_batch_size;
    store_config.max_record_bytes    = args.max_record_bytes;
    store_config.enable_tombstone_gc = args.enable_tombstone_gc;

    const std::string requested_backend = ToLower(store_config.backend);
    if (requested_backend == "eloqstore") {
        std::string probe_error;
        if (!pgmem::store::IsIoUringAvailable(&probe_error)) {
            std::cerr << "[pgmemd] warning: eloqstore storage io_uring probe failed: " << probe_error << "\n";
            std::cerr << "[pgmemd] warning: auto-downgrading backend to in-memory\n";
            store_config.backend = "inmemory";
        }
    }

    std::unique_ptr<pgmem::store::IStoreAdapter> store = pgmem::store::CreateStoreAdapter(store_config, &error);
    if (!store) {
        const std::string backend = ToLower(store_config.backend);
        if (backend == "eloqstore") {
            std::cerr << "[pgmemd] failed to initialize eloqstore backend: " << error
                      << ", auto-downgrading to in-memory\n";
            store_config.backend = "inmemory";
            store                = pgmem::store::CreateInMemoryStoreAdapter();
        } else {
            std::cerr << "[pgmemd] failed to initialize backend '" << store_config.backend << "': " << error << "\n";
            return 1;
        }
    }

    const std::string effective_backend = ToLower(store_config.backend);
    std::cerr << "[pgmemd] runtime: core_number=" << runtime_state.resolved_core_number
              << " event_dispatcher_num=" << runtime_state.resolved_event_dispatcher_num
              << " network_io_uring=" << (runtime_state.network_io_uring_enabled ? "true" : "false")
              << " backend=" << effective_backend << " write_ack_mode=accepted\n";

    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions engine_options;
    engine_options.mem_budget_mb              = args.mem_budget_mb;
    engine_options.disk_budget_gb             = args.disk_budget_gb;
    engine_options.gc_high_watermark          = args.gc_high_watermark;
    engine_options.gc_low_watermark           = args.gc_low_watermark;
    engine_options.gc_batch_size              = args.gc_batch_size;
    engine_options.max_record_bytes           = args.max_record_bytes;
    engine_options.enable_tombstone_gc        = args.enable_tombstone_gc;
    engine_options.write_ack_mode             = pgmem::core::WriteAckMode::Accepted;
    engine_options.volatile_flush_interval_ms = pgmem::config::kDefaultVolatileFlushIntervalMs;
    engine_options.volatile_max_pending_ops   = pgmem::config::kDefaultVolatileMaxPendingOps;
    engine_options.shutdown_drain_timeout_ms  = args.shutdown_drain_timeout_ms;
    engine_options.effective_backend          = effective_backend;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), pgmem::config::kDefaultNodeIdLocal,
                                     engine_options);
    if (!engine.Warmup(&error)) {
        std::cerr << "[pgmemd] warmup failed: " << error << "\n";
        return 1;
    }

    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::net::HttpServerOptions server_options;
    server_options.num_threads = runtime_state.resolved_core_number;
    pgmem::net::HttpServer server(args.host, args.port, server_options);
    server.RegisterRoute("GET", "/health", [](const pgmem::net::HttpRequest&) {
        pgmem::net::HttpResponse resp;
        resp.status_code = 200;
        resp.body        = "{\"status\":\"ok\"}";
        return resp;
    });

    server.RegisterRoute("POST", "/mcp", [&](const pgmem::net::HttpRequest& req) {
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

        const auto out   = dispatcher.Handle(json);
        resp.status_code = 200;
        resp.body        = pgmem::util::ToJsonString(out, false);
        return resp;
    });

    server.RegisterRoute("GET", "/mcp/describe", [&](const pgmem::net::HttpRequest&) {
        pgmem::net::HttpResponse resp;
        resp.status_code = 200;
        resp.body        = pgmem::util::ToJsonString(dispatcher.Describe(), false);
        return resp;
    });

    if (!server.Start(&error)) {
        std::cerr << "[pgmemd] start failed: " << error << "\n";
        return 1;
    }

    std::cerr << "[pgmemd] listening on http://" << args.host << ":" << args.port << "/mcp\n";

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    server.Stop();
    return 0;
}
