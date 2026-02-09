#include "pgmem/mcp/mcp_dispatcher.h"

#include "pgmem/util/json.h"

namespace pgmem::mcp {

McpDispatcher::McpDispatcher(core::MemoryEngine* engine) : engine_(engine) {}

util::Json McpDispatcher::Handle(const util::Json& request) {
    const util::Json empty_id;
    const auto id_opt = request.get_child_optional("id");
    const util::Json id = id_opt ? *id_opt : empty_id;

    const std::string method = util::GetStringOr(request, "method", "");
    const auto params_opt = request.get_child_optional("params");
    const util::Json params = params_opt ? *params_opt : util::Json{};

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

    response.add_child("error", Error(-32601, "method not found", id).get_child("error"));
    return response;
}

util::Json McpDispatcher::HandleBootstrap(const util::Json& params) {
    BootstrapInput in;
    in.workspace_id = util::GetStringOr(params, "workspace_id", "default");
    in.session_id = util::GetStringOr(params, "session_id", "default");
    in.task_text = util::GetStringOr(params, "task_text", "");
    in.open_files = util::ReadStringArray(params, "open_files");
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
    in.workspace_id = util::GetStringOr(params, "workspace_id", "default");
    in.session_id = util::GetStringOr(params, "session_id", "default");
    in.user_text = util::GetStringOr(params, "user_text", "");
    in.assistant_text = util::GetStringOr(params, "assistant_text", "");
    in.code_snippets = util::ReadStringArray(params, "code_snippets");
    in.commands = util::ReadStringArray(params, "commands");

    const auto out = engine_->CommitTurn(in);

    util::Json result;
    result.put("summary_updated", out.summary_updated);
    result.add_child("stored_ids", util::MakeArray(out.stored_ids));
    return result;
}

util::Json McpDispatcher::HandleSearch(const util::Json& params) {
    SearchInput in;
    in.workspace_id = util::GetStringOr(params, "workspace_id", "default");
    in.query = util::GetStringOr(params, "query", "");
    in.top_k = static_cast<size_t>(util::GetIntOr(params, "top_k", 8));
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
    const std::string memory_id = util::GetStringOr(params, "memory_id", "");
    const bool pin = util::GetBoolOr(params, "pin", true);

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
    const std::string window = util::GetStringOr(params, "window", "5m");
    const auto stats = engine_->Stats(workspace_id, window);

    util::Json result;
    result.put("p95_read_ms", stats.p95_read_ms);
    result.put("p95_write_ms", stats.p95_write_ms);
    result.put("token_reduction_ratio", stats.token_reduction_ratio);
    result.put("sync_lag_ops", stats.sync_lag_ops);
    result.put("fallback_rate", stats.fallback_rate);
    return result;
}

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
