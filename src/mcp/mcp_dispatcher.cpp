#include "pgmem/mcp/mcp_dispatcher.h"

#include <vector>

#include "pgmem/util/json.h"

namespace pgmem::mcp {
namespace {

util::Json SchemaField(const std::string& type, const std::string& description, bool required,
                       const std::string& default_value = "") {
    util::Json field;
    field.put("type", type);
    field.put("description", description);
    field.put("required", required);
    if (!default_value.empty()) {
        field.put("default", default_value);
    }
    return field;
}

util::Json StringArray(const std::vector<std::string>& values) { return util::MakeArray(values); }

}  // namespace

McpDispatcher::McpDispatcher(core::MemoryEngine* engine) : engine_(engine) {}

util::Json McpDispatcher::Handle(const util::Json& request) {
    const util::Json empty_id;
    const auto id_opt   = request.get_child_optional("id");
    const util::Json id = id_opt ? *id_opt : empty_id;

    const std::string method = util::GetStringOr(request, "method", "");
    const auto params_opt    = request.get_child_optional("params");
    const util::Json params  = params_opt ? *params_opt : util::Json{};

    util::Json response;
    response.add_child("id", id);

    if (method == "memory.bootstrap") {
        response.add_child("result", HandleBootstrap(params));
        return response;
    }
    if (method == "memory.commit_turn") {
        response.add_child("result", HandleCommitTurn(params));
        return response;
    }
    if (method == "memory.search") {
        response.add_child("result", HandleSearch(params));
        return response;
    }
    if (method == "memory.pin") {
        response.add_child("result", HandlePin(params));
        return response;
    }
    if (method == "memory.stats") {
        response.add_child("result", HandleStats(params));
        return response;
    }
    if (method == "memory.compact") {
        response.add_child("result", HandleCompact(params));
        return response;
    }
    if (method == "memory.describe" || method == "mcp.describe") {
        response.add_child("result", HandleDescribe(params));
        return response;
    }

    response.add_child("error", Error(-32601, "method not found", id).get_child("error"));
    return response;
}

util::Json McpDispatcher::HandleBootstrap(const util::Json& params) {
    BootstrapInput in;
    in.workspace_id = util::GetStringOr(params, "workspace_id", "default");
    in.session_id   = util::GetStringOr(params, "session_id", "default");
    in.task_text    = util::GetStringOr(params, "task_text", "");
    in.open_files   = util::ReadStringArray(params, "open_files");
    in.token_budget = util::GetIntOr(params, "token_budget", 2048);

    const auto out = engine_->Bootstrap(in);

    util::Json result;
    result.put("summary", out.summary);
    result.put("estimated_tokens_saved", out.estimated_tokens_saved);

    util::Json recalled;
    for (const auto& hit : out.recalled_items) {
        util::Json node;
        node.put("memory_id", hit.memory_id);
        node.put("source", hit.source);
        node.put("content", hit.content);
        node.put("score", hit.final_score);
        node.put("updated_at_ms", hit.updated_at_ms);
        recalled.push_back(std::make_pair("", node));
    }
    result.add_child("recalled_items", recalled);
    return result;
}

util::Json McpDispatcher::HandleCommitTurn(const util::Json& params) {
    CommitTurnInput in;
    in.workspace_id   = util::GetStringOr(params, "workspace_id", "default");
    in.session_id     = util::GetStringOr(params, "session_id", "default");
    in.user_text      = util::GetStringOr(params, "user_text", "");
    in.assistant_text = util::GetStringOr(params, "assistant_text", "");
    in.code_snippets  = util::ReadStringArray(params, "code_snippets");
    in.commands       = util::ReadStringArray(params, "commands");

    const auto out = engine_->CommitTurn(in);

    util::Json result;
    result.put("summary_updated", out.summary_updated);
    result.add_child("stored_ids", util::MakeArray(out.stored_ids));
    return result;
}

util::Json McpDispatcher::HandleSearch(const util::Json& params) {
    SearchInput in;
    in.workspace_id = util::GetStringOr(params, "workspace_id", "default");
    in.query        = util::GetStringOr(params, "query", "");
    in.top_k        = static_cast<size_t>(util::GetIntOr(params, "top_k", 8));
    in.token_budget = util::GetIntOr(params, "token_budget", 2048);

    const auto out = engine_->Search(in);

    util::Json result;
    util::Json hits;
    for (const auto& hit : out.hits) {
        util::Json node;
        node.put("memory_id", hit.memory_id);
        node.put("source", hit.source);
        node.put("content", hit.content);
        node.put("lexical_score", hit.lexical_score);
        node.put("semantic_score", hit.semantic_score);
        node.put("final_score", hit.final_score);
        node.put("updated_at_ms", hit.updated_at_ms);
        node.put("pinned", hit.pinned);
        hits.push_back(std::make_pair("", node));
    }
    result.add_child("hits", hits);
    return result;
}

util::Json McpDispatcher::HandlePin(const util::Json& params) {
    const std::string workspace_id = util::GetStringOr(params, "workspace_id", "default");
    const std::string memory_id    = util::GetStringOr(params, "memory_id", "");
    const bool pin                 = util::GetBoolOr(params, "pin", true);

    std::string error;
    const bool ok = engine_->Pin(workspace_id, memory_id, pin, &error);

    util::Json result;
    result.put("ok", ok);
    if (!ok) {
        result.put("error", error);
    }
    return result;
}

util::Json McpDispatcher::HandleStats(const util::Json& params) {
    const std::string workspace_id = util::GetStringOr(params, "workspace_id", "default");
    const std::string window       = util::GetStringOr(params, "window", "5m");
    const auto stats               = engine_->Stats(workspace_id, window);

    util::Json result;
    result.put("p95_read_ms", stats.p95_read_ms);
    result.put("p95_write_ms", stats.p95_write_ms);
    result.put("token_reduction_ratio", stats.token_reduction_ratio);
    result.put("fallback_rate", stats.fallback_rate);
    result.put("mem_used_bytes", stats.mem_used_bytes);
    result.put("disk_used_bytes", stats.disk_used_bytes);
    result.put("item_count", stats.item_count);
    result.put("tombstone_count", stats.tombstone_count);
    result.put("gc_last_run_ms", stats.gc_last_run_ms);
    result.put("gc_evicted_count", stats.gc_evicted_count);
    result.put("capacity_blocked", stats.capacity_blocked);
    result.put("write_ack_mode", stats.write_ack_mode);
    result.put("pending_write_ops", stats.pending_write_ops);
    result.put("flush_failures_total", stats.flush_failures_total);
    result.put("volatile_dropped_on_shutdown", stats.volatile_dropped_on_shutdown);
    result.put("effective_backend", stats.effective_backend);
    result.put("last_flush_error", stats.last_flush_error);
    return result;
}

util::Json McpDispatcher::HandleCompact(const util::Json& params) {
    const std::string workspace_id = util::GetStringOr(params, "workspace_id", "default");
    const auto out                 = engine_->Compact(workspace_id);

    util::Json result;
    result.put("triggered", out.triggered);
    result.put("capacity_blocked", out.capacity_blocked);
    result.put("mem_before_bytes", out.mem_before_bytes);
    result.put("disk_before_bytes", out.disk_before_bytes);
    result.put("mem_after_bytes", out.mem_after_bytes);
    result.put("disk_after_bytes", out.disk_after_bytes);
    result.put("summarized_count", out.summarized_count);
    result.put("tombstoned_count", out.tombstoned_count);
    result.put("deleted_count", out.deleted_count);
    return result;
}

util::Json McpDispatcher::Describe() const {
    util::Json result;
    result.put("server.name", "poorguy-mem");
    result.put("server.kind", "memory-server");
    result.put("server.protocol", "jsonrpc-2.0");
    result.put("server.transport", "http");
    result.put("server.endpoint", "/mcp");
    result.put("server.describe_endpoint", "/mcp/describe");
    result.put("server.sync_routes_available", false);
    result.put("server.write_ack_mode", "accepted");
    result.add_child("server.supported_backends", StringArray({"eloqstore", "inmemory"}));

    result.add_child("method_names", StringArray({"memory.describe", "memory.bootstrap", "memory.commit_turn",
                                                  "memory.search", "memory.pin", "memory.stats", "memory.compact"}));

    util::Json methods;
    util::Json memory;

    {
        util::Json method;
        method.put("summary", "Return full API contract, semantics, and field schemas.");

        util::Json input_props;
        input_props.add_child(
            "include_examples",
            SchemaField("boolean", "Whether to include request/response examples in future versions.", false, "true"));
        util::Json input;
        input.put("type", "object");
        input.add_child("properties", input_props);
        method.add_child("input", input);

        util::Json output_props;
        output_props.add_child("server", SchemaField("object", "Server metadata and transport contract.", true));
        output_props.add_child("method_names", SchemaField("array<string>", "List of callable memory methods.", true));
        output_props.add_child("methods", SchemaField("object", "Method-level semantic/input/output schemas.", true));
        output_props.add_child("errors", SchemaField("object", "Transport and JSON-RPC error contract.", true));
        util::Json output;
        output.put("type", "object");
        output.add_child("properties", output_props);
        method.add_child("output", output);

        memory.add_child("describe", method);
    }

    {
        util::Json method;
        method.put("summary", "Bootstrap current session by recalling relevant memory and summary.");

        util::Json input_props;
        input_props.add_child(
            "workspace_id",
            SchemaField("string", "Workspace identity. Defaults to 'default' if absent.", false, "default"));
        input_props.add_child("session_id", SchemaField("string", "Session identity. Defaults to 'default' if absent.",
                                                        false, "default"));
        input_props.add_child("task_text", SchemaField("string", "Current task text to guide recall.", false, ""));
        input_props.add_child("open_files",
                              SchemaField("array<string>", "Open file paths used as recall context.", false));
        input_props.add_child("token_budget",
                              SchemaField("integer", "Budget used to truncate recalled context.", false, "2048"));
        util::Json input;
        input.put("type", "object");
        input.add_child("properties", input_props);
        method.add_child("input", input);

        util::Json recalled_item_props;
        recalled_item_props.add_child("memory_id", SchemaField("string", "Unique memory id.", true));
        recalled_item_props.add_child("source", SchemaField("string", "Origin type of memory.", true));
        recalled_item_props.add_child("content", SchemaField("string", "Recalled memory content.", true));
        recalled_item_props.add_child("score", SchemaField("number", "Final rank score.", true));
        recalled_item_props.add_child("updated_at_ms",
                                      SchemaField("integer", "Last update timestamp in milliseconds.", true));

        util::Json recalled_items = SchemaField("array<object>", "Top recalled memory hits.", true);
        recalled_items.add_child("item_properties", recalled_item_props);

        util::Json output_props;
        output_props.add_child("summary", SchemaField("string", "Session summary for prompt reduction.", true));
        output_props.add_child("estimated_tokens_saved",
                               SchemaField("integer", "Estimated tokens reduced by summary+recall.", true));
        output_props.add_child("recalled_items", recalled_items);
        util::Json output;
        output.put("type", "object");
        output.add_child("properties", output_props);
        method.add_child("output", output);

        memory.add_child("bootstrap", method);
    }

    {
        util::Json method;
        method.put("summary", "Persist one conversation turn into memory store.");

        util::Json input_props;
        input_props.add_child(
            "workspace_id",
            SchemaField("string", "Workspace identity. Defaults to 'default' if absent.", false, "default"));
        input_props.add_child("session_id", SchemaField("string", "Session identity. Defaults to 'default' if absent.",
                                                        false, "default"));
        input_props.add_child("user_text", SchemaField("string", "Current user utterance text.", false, ""));
        input_props.add_child("assistant_text", SchemaField("string", "Current assistant response text.", false, ""));
        input_props.add_child("code_snippets", SchemaField("array<string>", "Code snippets from current turn.", false));
        input_props.add_child("commands", SchemaField("array<string>", "Shell commands from current turn.", false));
        util::Json input;
        input.put("type", "object");
        input.add_child("properties", input_props);
        method.add_child("input", input);

        util::Json output_props;
        output_props.add_child("summary_updated",
                               SchemaField("string", "Latest session summary after this write.", true));
        output_props.add_child("stored_ids", SchemaField("array<string>", "Inserted/updated memory ids.", true));
        util::Json output;
        output.put("type", "object");
        output.add_child("properties", output_props);
        method.add_child("output", output);

        memory.add_child("commit_turn", method);
    }

    {
        util::Json method;
        method.put("summary", "Search memory using lexical+semantic ranking.");

        util::Json input_props;
        input_props.add_child(
            "workspace_id",
            SchemaField("string", "Workspace identity. Defaults to 'default' if absent.", false, "default"));
        input_props.add_child("query", SchemaField("string", "Search query text.", false, ""));
        input_props.add_child("top_k", SchemaField("integer", "Maximum number of returned hits.", false, "8"));
        input_props.add_child("token_budget",
                              SchemaField("integer", "Budget used to trim response payload.", false, "2048"));
        util::Json input;
        input.put("type", "object");
        input.add_child("properties", input_props);
        method.add_child("input", input);

        util::Json hit_props;
        hit_props.add_child("memory_id", SchemaField("string", "Unique memory id.", true));
        hit_props.add_child("source", SchemaField("string", "Memory source.", true));
        hit_props.add_child("content", SchemaField("string", "Stored content.", true));
        hit_props.add_child("lexical_score", SchemaField("number", "Lexical ranking score.", true));
        hit_props.add_child("semantic_score", SchemaField("number", "Semantic ranking score.", true));
        hit_props.add_child("final_score", SchemaField("number", "Combined ranking score.", true));
        hit_props.add_child("updated_at_ms", SchemaField("integer", "Last update timestamp in milliseconds.", true));
        hit_props.add_child("pinned", SchemaField("boolean", "Pin status.", true));

        util::Json hits = SchemaField("array<object>", "Ranked memory hits.", true);
        hits.add_child("item_properties", hit_props);

        util::Json output_props;
        output_props.add_child("hits", hits);
        util::Json output;
        output.put("type", "object");
        output.add_child("properties", output_props);
        method.add_child("output", output);

        memory.add_child("search", method);
    }

    {
        util::Json method;
        method.put("summary", "Pin or unpin one memory record.");

        util::Json input_props;
        input_props.add_child(
            "workspace_id",
            SchemaField("string", "Workspace identity. Defaults to 'default' if absent.", false, "default"));
        input_props.add_child("memory_id", SchemaField("string", "Target memory id to pin/unpin.", true));
        input_props.add_child("pin", SchemaField("boolean", "true=pin, false=unpin.", false, "true"));
        util::Json input;
        input.put("type", "object");
        input.add_child("properties", input_props);
        method.add_child("input", input);

        util::Json output_props;
        output_props.add_child("ok", SchemaField("boolean", "Operation success flag.", true));
        output_props.add_child("error", SchemaField("string", "Error message when ok=false.", false));
        util::Json output;
        output.put("type", "object");
        output.add_child("properties", output_props);
        method.add_child("output", output);

        memory.add_child("pin", method);
    }

    {
        util::Json method;
        method.put("summary", "Return runtime stats and backend/write-path state.");

        util::Json input_props;
        input_props.add_child(
            "workspace_id",
            SchemaField("string", "Workspace identity. Defaults to 'default' if absent.", false, "default"));
        input_props.add_child("window", SchemaField("string", "Window hint for latency stats.", false, "5m"));
        util::Json input;
        input.put("type", "object");
        input.add_child("properties", input_props);
        method.add_child("input", input);

        util::Json output_props;
        output_props.add_child("p95_read_ms", SchemaField("number", "Read p95 latency in ms.", true));
        output_props.add_child("p95_write_ms", SchemaField("number", "Write p95 latency in ms.", true));
        output_props.add_child("token_reduction_ratio", SchemaField("number", "Prompt token reduction ratio.", true));
        output_props.add_child("fallback_rate", SchemaField("number", "Retriever fallback ratio.", true));
        output_props.add_child("mem_used_bytes", SchemaField("integer", "Estimated memory usage.", true));
        output_props.add_child("disk_used_bytes", SchemaField("integer", "Estimated disk usage.", true));
        output_props.add_child("item_count", SchemaField("integer", "Total items in workspace.", true));
        output_props.add_child("tombstone_count", SchemaField("integer", "Total tombstones in workspace.", true));
        output_props.add_child("gc_last_run_ms", SchemaField("integer", "Last GC timestamp in milliseconds.", true));
        output_props.add_child("gc_evicted_count", SchemaField("integer", "Accumulated GC evictions.", true));
        output_props.add_child("capacity_blocked", SchemaField("boolean", "Whether capacity guard is active.", true));
        output_props.add_child("write_ack_mode", SchemaField("string", "Current write ack mode.", true, "accepted"));
        output_props.add_child("pending_write_ops",
                               SchemaField("integer", "Accepted-mode pending async writes.", true));
        output_props.add_child("flush_failures_total", SchemaField("integer", "Total async flush failures.", true));
        output_props.add_child("volatile_dropped_on_shutdown",
                               SchemaField("integer", "Dropped accepted writes on forced shutdown.", true));
        output_props.add_child("effective_backend", SchemaField("string", "Runtime effective backend.", true));
        output_props.add_child("last_flush_error", SchemaField("string", "Last async flush error text.", true));
        util::Json effective_backend = output_props.get_child("effective_backend");
        effective_backend.add_child("enum", StringArray({"eloqstore", "inmemory"}));
        output_props.put_child("effective_backend", effective_backend);

        util::Json output;
        output.put("type", "object");
        output.add_child("properties", output_props);
        method.add_child("output", output);

        memory.add_child("stats", method);
    }

    {
        util::Json method;
        method.put("summary", "Trigger memory compaction/GC projection cleanup.");

        util::Json input_props;
        input_props.add_child(
            "workspace_id",
            SchemaField("string", "Workspace identity. Defaults to 'default' if absent.", false, "default"));
        util::Json input;
        input.put("type", "object");
        input.add_child("properties", input_props);
        method.add_child("input", input);

        util::Json output_props;
        output_props.add_child("triggered", SchemaField("boolean", "Whether compaction was triggered.", true));
        output_props.add_child("capacity_blocked",
                               SchemaField("boolean", "Whether writes are blocked by capacity.", true));
        output_props.add_child("mem_before_bytes", SchemaField("integer", "Memory usage before compaction.", true));
        output_props.add_child("disk_before_bytes", SchemaField("integer", "Disk usage before compaction.", true));
        output_props.add_child("mem_after_bytes", SchemaField("integer", "Memory usage after compaction.", true));
        output_props.add_child("disk_after_bytes", SchemaField("integer", "Disk usage after compaction.", true));
        output_props.add_child("summarized_count", SchemaField("integer", "Count of summary projections.", true));
        output_props.add_child("tombstoned_count", SchemaField("integer", "Count of tombstoned records.", true));
        output_props.add_child("deleted_count", SchemaField("integer", "Count of physically deleted records.", true));
        util::Json output;
        output.put("type", "object");
        output.add_child("properties", output_props);
        method.add_child("output", output);

        memory.add_child("compact", method);
    }

    methods.add_child("memory", memory);
    result.add_child("methods", methods);

    util::Json errors;
    errors.put("jsonrpc_method_not_found.code", -32601);
    errors.put("jsonrpc_method_not_found.message", "method not found");
    errors.put("jsonrpc_method_not_found.when", "unknown method on POST /mcp");
    errors.put("invalid_json_http.status", 400);
    errors.put("invalid_json_http.message", "invalid JSON");
    errors.put("invalid_json_http.when", "malformed request body");
    errors.put("route_not_found_http.status", 404);
    errors.put("route_not_found_http.message", "route not found");
    errors.put("route_not_found_http.when", "unsupported path such as /sync/push or /sync/pull");
    result.add_child("errors", errors);

    return result;
}

util::Json McpDispatcher::HandleDescribe(const util::Json&) { return Describe(); }

util::Json McpDispatcher::Error(int code, const std::string& message, const util::Json& id) const {
    util::Json response;
    response.add_child("id", id);

    util::Json err;
    err.put("code", code);
    err.put("message", message);
    response.add_child("error", err);
    return response;
}

}  // namespace pgmem::mcp
