#include "pgmem/mcp/mcp_dispatcher.h"

#include <exception>
#include <vector>

#include "pgmem/util/json.h"
#include "pgmem/util/time.h"

namespace pgmem::mcp {
namespace {

util::Json StringArray(const std::vector<std::string>& values) { return util::MakeArray(values); }

double GetDoubleOr(const util::Json& json, const std::string& path, double fallback) {
    try {
        return json.get<double>(path);
    } catch (const std::exception&) {
        return fallback;
    }
}

util::Json JsonSchema(const std::string& type, const std::string& description = "") {
    util::Json schema;
    schema.put("type", type);
    if (!description.empty()) {
        schema.put("description", description);
    }
    return schema;
}

void SetRequired(util::Json* schema, const std::vector<std::string>& fields) {
    if (schema == nullptr || fields.empty()) {
        return;
    }
    schema->add_child("required", StringArray(fields));
}

void SetEnum(util::Json* schema, const std::vector<std::string>& values) {
    if (schema == nullptr || values.empty()) {
        return;
    }
    schema->add_child("enum", StringArray(values));
}

void SetDefault(util::Json* schema, const std::string& value) {
    if (schema == nullptr) {
        return;
    }
    schema->put("default", value);
}

void SetDefault(util::Json* schema, bool value) {
    if (schema == nullptr) {
        return;
    }
    schema->put("default", value);
}

void SetDefault(util::Json* schema, int value) {
    if (schema == nullptr) {
        return;
    }
    schema->put("default", value);
}

void SetDefault(util::Json* schema, double value) {
    if (schema == nullptr) {
        return;
    }
    schema->put("default", value);
}

util::Json HttpError(const std::string& name, int status, const std::string& message, const std::string& when) {
    util::Json err;
    err.put("name", name);
    err.put("status", status);
    err.put("message", message);
    err.put("when", when);
    return err;
}

util::Json JsonRpcErrorEntry(const std::string& name, int code, const std::string& message, const std::string& when) {
    util::Json err;
    err.put("name", name);
    err.put("code", code);
    err.put("message", message);
    err.put("when", when);
    return err;
}

util::Json MakeExample(const std::string& name, const util::Json& request, const util::Json& response) {
    util::Json ex;
    ex.put("name", name);
    ex.add_child("request", request);
    ex.add_child("response", response);
    return ex;
}

util::Json BuildDescribeExamples() {
    util::Json examples;

    util::Json request;
    request.put("method", "memory.describe");
    util::Json params;
    params.put("include_examples", true);
    request.add_child("params", params);

    util::Json response;
    util::Json result;
    result.put("describe_version", "2.0.0");
    result.put("schema_revision", "2026-02-19.1");
    response.add_child("result", result);

    examples.push_back(std::make_pair("", MakeExample("describe_with_examples", request, response)));
    return examples;
}

util::Json BuildWriteExamples() {
    util::Json examples;

    {
        util::Json request;
        request.put("method", "memory.write");
        util::Json params;
        params.put("workspace_id", "demo");
        params.put("session_id", "s1");
        util::Json records;
        util::Json record;
        record.put("source", "turn");
        record.put("content", "remember retry with exponential backoff");
        records.push_back(std::make_pair("", record));
        params.add_child("records", records);
        request.add_child("params", params);

        util::Json response;
        util::Json result;
        result.put("ok", true);
        result.add_child("stored_ids", StringArray({"m-demo-1"}));
        result.add_child("deduped_ids", util::Json{});
        result.put("index_generation", 1);
        result.add_child("warnings", util::Json{});
        response.add_child("result", result);

        examples.push_back(std::make_pair("", MakeExample("write_success", request, response)));
    }

    {
        util::Json request;
        request.put("method", "memory.write");
        util::Json params;
        util::Json records;
        util::Json record;
        record.put("content", "invalid without workspace");
        records.push_back(std::make_pair("", record));
        params.add_child("records", records);
        request.add_child("params", params);

        util::Json response;
        util::Json result;
        result.put("ok", false);
        result.add_child("stored_ids", util::Json{});
        result.add_child("deduped_ids", util::Json{});
        result.put("index_generation", 0);
        result.add_child("warnings", StringArray({"workspace_id is required"}));
        response.add_child("result", result);

        examples.push_back(std::make_pair("", MakeExample("write_validation_error", request, response)));
    }

    return examples;
}

util::Json BuildQueryExamples() {
    util::Json examples;

    {
        util::Json request;
        request.put("method", "memory.query");
        util::Json params;
        params.put("workspace_id", "demo");
        params.put("query", "retry backoff");
        params.put("top_k", 3);
        params.put("token_budget", 1200);
        request.add_child("params", params);

        util::Json response;
        util::Json result;
        util::Json hits;
        util::Json hit;
        hit.put("memory_id", "m-demo-1");
        hit.put("content", "remember retry with exponential backoff");
        hits.push_back(std::make_pair("", hit));
        result.add_child("hits", hits);
        util::Json debug;
        debug.put("merged_candidates", 1);
        result.add_child("debug_stats", debug);
        response.add_child("result", result);

        examples.push_back(std::make_pair("", MakeExample("query_success", request, response)));
    }

    {
        util::Json request;
        request.put("method", "memory.query");
        util::Json params;
        params.put("workspace_id", "demo");
        params.put("query", "retry backoff");
        request.add_child("params", params);

        util::Json response;
        util::Json result;
        result.add_child("hits", util::Json{});
        result.add_child("debug_stats", util::Json{});
        response.add_child("result", result);

        examples.push_back(std::make_pair("", MakeExample("query_empty_result", request, response)));
    }

    return examples;
}

util::Json BuildPinExamples() {
    util::Json examples;

    {
        util::Json request;
        request.put("method", "memory.pin");
        util::Json params;
        params.put("workspace_id", "demo");
        params.put("memory_id", "m-demo-1");
        params.put("pin", true);
        request.add_child("params", params);

        util::Json response;
        util::Json result;
        result.put("ok", true);
        response.add_child("result", result);

        examples.push_back(std::make_pair("", MakeExample("pin_success", request, response)));
    }

    {
        util::Json request;
        request.put("method", "memory.pin");
        util::Json params;
        params.put("workspace_id", "demo");
        params.put("memory_id", "m-not-exist");
        params.put("pin", true);
        request.add_child("params", params);

        util::Json response;
        util::Json result;
        result.put("ok", false);
        result.put("error", "record not found");
        response.add_child("result", result);

        examples.push_back(std::make_pair("", MakeExample("pin_not_found", request, response)));
    }

    return examples;
}

util::Json BuildStatsExamples() {
    util::Json examples;

    util::Json request;
    request.put("method", "memory.stats");
    util::Json params;
    params.put("workspace_id", "demo");
    params.put("window", "5m");
    request.add_child("params", params);

    util::Json response;
    util::Json result;
    result.put("write_ack_mode", "durable");
    result.put("effective_backend", "eloqstore");
    util::Json index_stats;
    index_stats.put("segment_count", 1);
    result.add_child("index_stats", index_stats);
    response.add_child("result", result);

    examples.push_back(std::make_pair("", MakeExample("stats_snapshot", request, response)));
    return examples;
}

util::Json BuildCompactExamples() {
    util::Json examples;

    util::Json request;
    request.put("method", "memory.compact");
    util::Json params;
    params.put("workspace_id", "demo");
    request.add_child("params", params);

    util::Json response;
    util::Json result;
    result.put("triggered", true);
    result.put("capacity_blocked", false);
    result.put("deleted_count", 0);
    response.add_child("result", result);

    examples.push_back(std::make_pair("", MakeExample("compact_success", request, response)));
    return examples;
}

util::Json BuildDescribeMethod(const McpDispatcher::DescribeOptions& options) {
    util::Json method;
    method.put("summary", "Return machine-readable server contract and method schemas.");

    util::Json include_examples = JsonSchema("boolean", "Include request/response examples.");
    SetDefault(&include_examples, false);

    util::Json input_props;
    input_props.add_child("include_examples", include_examples);

    util::Json input_schema = JsonSchema("object", "Describe request.");
    input_schema.put("additionalProperties", false);
    input_schema.add_child("properties", input_props);

    util::Json output_props;
    output_props.add_child("describe_version", JsonSchema("string", "Describe contract major version."));
    output_props.add_child("schema_revision", JsonSchema("string", "Schema revision identifier."));
    output_props.add_child("generated_at_ms", JsonSchema("integer", "Server generation timestamp in ms."));
    output_props.add_child("server", JsonSchema("object", "Server metadata."));
    output_props.add_child("method_names", JsonSchema("array", "Exposed method names."));
    output_props.add_child("methods", JsonSchema("object", "Per-method contract map."));
    output_props.add_child("errors", JsonSchema("object", "Error contract by protocol layer."));

    util::Json output_schema = JsonSchema("object", "Describe response payload.");
    output_schema.put("additionalProperties", false);
    output_schema.add_child("properties", output_props);
    SetRequired(&output_schema,
                {"describe_version", "schema_revision", "generated_at_ms", "server", "method_names", "methods", "errors"});

    util::Json semantics;
    semantics.put("payload_size", "Examples are omitted unless include_examples=true.");
    semantics.put("stability", "describe_version and schema_revision gate client parser compatibility.");

    method.add_child("input_schema", input_schema);
    method.add_child("output_schema", output_schema);
    method.add_child("semantics", semantics);
    method.add_child("errors", StringArray({"jsonrpc.method_not_found", "jsonrpc.invalid_params"}));

    if (options.include_examples) {
        method.add_child("examples", BuildDescribeExamples());
    }
    return method;
}

util::Json BuildWriteMethod(const McpDispatcher::DescribeOptions& options) {
    util::Json method;
    method.put("summary", "Write memory records into durable storage and index.");

    util::Json record_props;
    record_props.add_child("record_id", JsonSchema("string", "Optional caller-provided record id."));
    {
        auto source = JsonSchema("string", "Record source label.");
        SetDefault(&source, "turn");
        record_props.add_child("source", source);
    }
    record_props.add_child("content", JsonSchema("string", "Record content."));
    {
        auto tags = JsonSchema("array", "Tag list.");
        tags.add_child("items", JsonSchema("string"));
        record_props.add_child("tags", tags);
    }
    {
        auto importance = JsonSchema("number", "Importance score.");
        SetDefault(&importance, 1.0);
        record_props.add_child("importance", importance);
    }
    {
        auto ttl = JsonSchema("integer", "TTL in seconds. 0 means no expiry.");
        SetDefault(&ttl, 0);
        record_props.add_child("ttl_s", ttl);
    }
    {
        auto pin = JsonSchema("boolean", "Pin flag.");
        SetDefault(&pin, false);
        record_props.add_child("pin", pin);
    }
    record_props.add_child("dedup_key", JsonSchema("string", "Deduplication key."));
    record_props.add_child("metadata", JsonSchema("object", "String key/value metadata."));

    util::Json record_schema = JsonSchema("object", "Write record item.");
    record_schema.put("additionalProperties", false);
    record_schema.add_child("properties", record_props);
    SetRequired(&record_schema, {"content"});

    util::Json input_props;
    input_props.add_child("workspace_id", JsonSchema("string", "Workspace id."));
    {
        auto session = JsonSchema("string", "Session id.");
        SetDefault(&session, "default");
        input_props.add_child("session_id", session);
    }
    {
        auto records = JsonSchema("array", "Records to write.");
        records.add_child("items", record_schema);
        input_props.add_child("records", records);
    }
    {
        auto mode = JsonSchema("string", "Write mode.");
        SetEnum(&mode, {"upsert", "append"});
        SetDefault(&mode, "upsert");
        input_props.add_child("write_mode", mode);
    }

    util::Json input_schema = JsonSchema("object", "memory.write input.");
    input_schema.put("additionalProperties", false);
    input_schema.add_child("properties", input_props);
    SetRequired(&input_schema, {"workspace_id", "records"});

    util::Json output_props;
    output_props.add_child("ok", JsonSchema("boolean", "Durable write success."));
    {
        auto ids = JsonSchema("array", "Stored memory ids.");
        ids.add_child("items", JsonSchema("string"));
        output_props.add_child("stored_ids", ids);
    }
    {
        auto deduped = JsonSchema("array", "Deduplicated memory ids.");
        deduped.add_child("items", JsonSchema("string"));
        output_props.add_child("deduped_ids", deduped);
    }
    output_props.add_child("index_generation", JsonSchema("integer", "Index generation after write."));
    {
        auto warnings = JsonSchema("array", "Write warnings.");
        warnings.add_child("items", JsonSchema("string"));
        output_props.add_child("warnings", warnings);
    }

    util::Json output_schema = JsonSchema("object", "memory.write output.");
    output_schema.put("additionalProperties", false);
    output_schema.add_child("properties", output_props);
    SetRequired(&output_schema, {"ok", "stored_ids", "deduped_ids", "index_generation", "warnings"});

    util::Json semantics;
    semantics.put("idempotency", "With write_mode=upsert, dedup_key may suppress duplicate writes.");
    semantics.put("ordering", "stored_ids preserves accepted record order in this request.");
    semantics.put("pin_governance", "pin=true may be downgraded with warning when quota/ratio limits are hit.");

    method.add_child("input_schema", input_schema);
    method.add_child("output_schema", output_schema);
    method.add_child("semantics", semantics);
    method.add_child("errors", StringArray({"jsonrpc.invalid_params"}));
    if (options.include_examples) {
        method.add_child("examples", BuildWriteExamples());
    }
    return method;
}

util::Json BuildQueryMethod(const McpDispatcher::DescribeOptions& options) {
    util::Json method;
    method.put("summary", "Query memory via sparse+dense hybrid retrieval.");

    util::Json filter_props;
    filter_props.add_child("session_id", JsonSchema("string", "Filter by session."));
    {
        auto sources = JsonSchema("array", "Allowed source values.");
        sources.add_child("items", JsonSchema("string"));
        filter_props.add_child("sources", sources);
    }
    {
        auto tags_any = JsonSchema("array", "Match any tag.");
        tags_any.add_child("items", JsonSchema("string"));
        filter_props.add_child("tags_any", tags_any);
    }
    filter_props.add_child("updated_after_ms", JsonSchema("integer", "Lower update-time bound."));
    filter_props.add_child("updated_before_ms", JsonSchema("integer", "Upper update-time bound."));
    filter_props.add_child("pinned_only", JsonSchema("boolean", "Return pinned records only."));

    util::Json filter_schema = JsonSchema("object", "Query filters.");
    filter_schema.put("additionalProperties", false);
    filter_schema.add_child("properties", filter_props);

    util::Json recall_props;
    {
        auto sparse_k = JsonSchema("integer", "Sparse candidate count.");
        SetDefault(&sparse_k, 200);
        recall_props.add_child("sparse_k", sparse_k);
    }
    {
        auto dense_k = JsonSchema("integer", "Dense candidate count.");
        SetDefault(&dense_k, 200);
        recall_props.add_child("dense_k", dense_k);
    }
    {
        auto oversample = JsonSchema("integer", "Candidate oversampling factor.");
        SetDefault(&oversample, 4);
        recall_props.add_child("oversample", oversample);
    }
    util::Json recall_schema = JsonSchema("object", "Recall options.");
    recall_schema.put("additionalProperties", false);
    recall_schema.add_child("properties", recall_props);

    util::Json rerank_props;
    {
        auto w_sparse = JsonSchema("number", "Sparse score weight.");
        SetDefault(&w_sparse, 0.55);
        rerank_props.add_child("w_sparse", w_sparse);
    }
    {
        auto w_dense = JsonSchema("number", "Dense score weight.");
        SetDefault(&w_dense, 0.30);
        rerank_props.add_child("w_dense", w_dense);
    }
    {
        auto w_freshness = JsonSchema("number", "Freshness score weight.");
        SetDefault(&w_freshness, 0.10);
        rerank_props.add_child("w_freshness", w_freshness);
    }
    {
        auto w_pin = JsonSchema("number", "Pin score weight.");
        SetDefault(&w_pin, 0.05);
        rerank_props.add_child("w_pin", w_pin);
    }
    util::Json rerank_schema = JsonSchema("object", "Rerank options.");
    rerank_schema.put("additionalProperties", false);
    rerank_schema.add_child("properties", rerank_props);

    util::Json input_props;
    input_props.add_child("workspace_id", JsonSchema("string", "Workspace id."));
    input_props.add_child("query", JsonSchema("string", "Query text."));
    {
        auto top_k = JsonSchema("integer", "Returned hit count.");
        SetDefault(&top_k, 8);
        input_props.add_child("top_k", top_k);
    }
    {
        auto budget = JsonSchema("integer", "Token budget; 0 means unlimited.");
        SetDefault(&budget, 2048);
        input_props.add_child("token_budget", budget);
    }
    input_props.add_child("filters", filter_schema);
    input_props.add_child("recall", recall_schema);
    input_props.add_child("rerank", rerank_schema);
    {
        auto debug = JsonSchema("boolean", "Include debug stats.");
        SetDefault(&debug, false);
        input_props.add_child("debug", debug);
    }

    util::Json input_schema = JsonSchema("object", "memory.query input.");
    input_schema.put("additionalProperties", false);
    input_schema.add_child("properties", input_props);
    SetRequired(&input_schema, {"workspace_id", "query"});

    util::Json score_props;
    score_props.add_child("sparse", JsonSchema("number"));
    score_props.add_child("dense", JsonSchema("number"));
    score_props.add_child("freshness", JsonSchema("number"));
    score_props.add_child("pin", JsonSchema("number"));
    score_props.add_child("final", JsonSchema("number"));

    util::Json score_schema = JsonSchema("object", "Score breakdown.");
    score_schema.put("additionalProperties", false);
    score_schema.add_child("properties", score_props);
    SetRequired(&score_schema, {"sparse", "dense", "freshness", "pin", "final"});

    util::Json hit_props;
    hit_props.add_child("memory_id", JsonSchema("string"));
    hit_props.add_child("source", JsonSchema("string"));
    hit_props.add_child("content", JsonSchema("string"));
    {
        auto tags = JsonSchema("array");
        tags.add_child("items", JsonSchema("string"));
        hit_props.add_child("tags", tags);
    }
    hit_props.add_child("metadata", JsonSchema("object"));
    hit_props.add_child("scores", score_schema);
    hit_props.add_child("updated_at_ms", JsonSchema("integer"));
    hit_props.add_child("pinned", JsonSchema("boolean"));

    util::Json hit_schema = JsonSchema("object", "Query hit.");
    hit_schema.put("additionalProperties", false);
    hit_schema.add_child("properties", hit_props);
    SetRequired(&hit_schema, {"memory_id", "source", "content", "tags", "metadata", "scores", "updated_at_ms", "pinned"});

    util::Json debug_latency_props;
    debug_latency_props.add_child("sparse", JsonSchema("number"));
    debug_latency_props.add_child("dense", JsonSchema("number"));
    debug_latency_props.add_child("rerank", JsonSchema("number"));
    debug_latency_props.add_child("total", JsonSchema("number"));

    util::Json debug_latency = JsonSchema("object");
    debug_latency.put("additionalProperties", false);
    debug_latency.add_child("properties", debug_latency_props);
    SetRequired(&debug_latency, {"sparse", "dense", "rerank", "total"});

    util::Json debug_props;
    debug_props.add_child("sparse_candidates", JsonSchema("integer"));
    debug_props.add_child("dense_candidates", JsonSchema("integer"));
    debug_props.add_child("merged_candidates", JsonSchema("integer"));
    debug_props.add_child("latency_ms", debug_latency);

    util::Json debug_schema = JsonSchema("object", "Debug stats.");
    debug_schema.put("additionalProperties", false);
    debug_schema.add_child("properties", debug_props);
    SetRequired(&debug_schema, {"sparse_candidates", "dense_candidates", "merged_candidates", "latency_ms"});

    util::Json output_props;
    {
        auto hits = JsonSchema("array", "Ranked hits.");
        hits.add_child("items", hit_schema);
        output_props.add_child("hits", hits);
    }
    output_props.add_child("debug_stats", debug_schema);

    util::Json output_schema = JsonSchema("object", "memory.query output.");
    output_schema.put("additionalProperties", false);
    output_schema.add_child("properties", output_props);
    SetRequired(&output_schema, {"hits", "debug_stats"});

    util::Json semantics;
    semantics.put("ranking", "Hits are ordered by final score descending.");
    semantics.put("token_budget", "Results are truncated when cumulative token estimate exceeds token_budget.");
    semantics.put("expiry", "Expired records are filtered before final output.");

    method.add_child("input_schema", input_schema);
    method.add_child("output_schema", output_schema);
    method.add_child("semantics", semantics);
    method.add_child("errors", StringArray({"jsonrpc.invalid_params"}));
    if (options.include_examples) {
        method.add_child("examples", BuildQueryExamples());
    }
    return method;
}

util::Json BuildPinMethod(const McpDispatcher::DescribeOptions& options) {
    util::Json method;
    method.put("summary", "Pin or unpin an existing memory record.");

    util::Json input_props;
    input_props.add_child("workspace_id", JsonSchema("string", "Workspace id."));
    input_props.add_child("memory_id", JsonSchema("string", "Target memory id."));
    {
        auto pin = JsonSchema("boolean", "Pin=true, unpin=false.");
        SetDefault(&pin, true);
        input_props.add_child("pin", pin);
    }

    util::Json input_schema = JsonSchema("object", "memory.pin input.");
    input_schema.put("additionalProperties", false);
    input_schema.add_child("properties", input_props);
    SetRequired(&input_schema, {"workspace_id", "memory_id"});

    util::Json output_props;
    output_props.add_child("ok", JsonSchema("boolean", "Operation status."));
    output_props.add_child("error", JsonSchema("string", "Present when ok=false."));

    util::Json output_schema = JsonSchema("object", "memory.pin output.");
    output_schema.put("additionalProperties", false);
    output_schema.add_child("properties", output_props);
    SetRequired(&output_schema, {"ok"});

    util::Json semantics;
    semantics.put("governance", "Pin operation may be rejected by quota or ratio limits.");

    method.add_child("input_schema", input_schema);
    method.add_child("output_schema", output_schema);
    method.add_child("semantics", semantics);
    method.add_child("errors", StringArray({"jsonrpc.invalid_params"}));
    if (options.include_examples) {
        method.add_child("examples", BuildPinExamples());
    }
    return method;
}

util::Json BuildStatsMethod(const McpDispatcher::DescribeOptions& options) {
    util::Json method;
    method.put("summary", "Return runtime usage and index counters.");

    util::Json input_props;
    input_props.add_child("workspace_id", JsonSchema("string", "Optional workspace scope."));
    {
        auto window = JsonSchema("string", "Window hint.");
        SetDefault(&window, "5m");
        input_props.add_child("window", window);
    }

    util::Json input_schema = JsonSchema("object", "memory.stats input.");
    input_schema.put("additionalProperties", false);
    input_schema.add_child("properties", input_props);

    util::Json index_props;
    index_props.add_child("segment_count", JsonSchema("integer"));
    index_props.add_child("posting_terms", JsonSchema("integer"));
    index_props.add_child("vector_count", JsonSchema("integer"));
    index_props.add_child("query_cache_hit_rate", JsonSchema("number"));
    index_props.add_child("dense_probe_count_p95", JsonSchema("number"));
    index_props.add_child("cold_rehydrate_count", JsonSchema("integer"));

    util::Json index_schema = JsonSchema("object", "Index statistics.");
    index_schema.put("additionalProperties", false);
    index_schema.add_child("properties", index_props);
    SetRequired(&index_schema,
                {"segment_count", "posting_terms", "vector_count", "query_cache_hit_rate", "dense_probe_count_p95",
                 "cold_rehydrate_count"});

    util::Json output_props;
    output_props.add_child("p95_read_ms", JsonSchema("number"));
    output_props.add_child("p95_write_ms", JsonSchema("number"));
    output_props.add_child("token_reduction_ratio", JsonSchema("number"));
    output_props.add_child("fallback_rate", JsonSchema("number"));
    output_props.add_child("mem_used_bytes", JsonSchema("integer"));
    output_props.add_child("disk_used_bytes", JsonSchema("integer"));
    output_props.add_child("resident_used_bytes", JsonSchema("integer"));
    output_props.add_child("resident_limit_bytes", JsonSchema("integer"));
    output_props.add_child("resident_evicted_count", JsonSchema("integer"));
    output_props.add_child("disk_fallback_search_count", JsonSchema("integer"));
    output_props.add_child("item_count", JsonSchema("integer"));
    output_props.add_child("tombstone_count", JsonSchema("integer"));
    output_props.add_child("gc_last_run_ms", JsonSchema("integer"));
    output_props.add_child("gc_evicted_count", JsonSchema("integer"));
    output_props.add_child("capacity_blocked", JsonSchema("boolean"));
    output_props.add_child("write_ack_mode", JsonSchema("string"));
    output_props.add_child("effective_backend", JsonSchema("string"));
    output_props.add_child("index_stats", index_schema);

    util::Json output_schema = JsonSchema("object", "memory.stats output.");
    output_schema.put("additionalProperties", false);
    output_schema.add_child("properties", output_props);
    SetRequired(&output_schema,
                {"p95_read_ms", "p95_write_ms", "token_reduction_ratio", "fallback_rate", "mem_used_bytes",
                 "disk_used_bytes", "resident_used_bytes", "resident_limit_bytes", "resident_evicted_count",
                 "disk_fallback_search_count", "item_count", "tombstone_count", "gc_last_run_ms", "gc_evicted_count",
                 "capacity_blocked", "write_ack_mode", "effective_backend", "index_stats"});

    util::Json semantics;
    semantics.put("scope", "workspace_id omitted means global snapshot.");
    semantics.put("mode", "write_ack_mode is fixed to durable.");

    method.add_child("input_schema", input_schema);
    method.add_child("output_schema", output_schema);
    method.add_child("semantics", semantics);
    method.add_child("errors", StringArray({"jsonrpc.invalid_params"}));
    if (options.include_examples) {
        method.add_child("examples", BuildStatsExamples());
    }
    return method;
}

util::Json BuildCompactMethod(const McpDispatcher::DescribeOptions& options) {
    util::Json method;
    method.put("summary", "Run compaction and return reclamation counters.");

    util::Json input_props;
    input_props.add_child("workspace_id", JsonSchema("string", "Optional workspace scope."));

    util::Json input_schema = JsonSchema("object", "memory.compact input.");
    input_schema.put("additionalProperties", false);
    input_schema.add_child("properties", input_props);

    util::Json output_props;
    output_props.add_child("triggered", JsonSchema("boolean"));
    output_props.add_child("capacity_blocked", JsonSchema("boolean"));
    output_props.add_child("mem_before_bytes", JsonSchema("integer"));
    output_props.add_child("disk_before_bytes", JsonSchema("integer"));
    output_props.add_child("mem_after_bytes", JsonSchema("integer"));
    output_props.add_child("disk_after_bytes", JsonSchema("integer"));
    output_props.add_child("summarized_count", JsonSchema("integer"));
    output_props.add_child("tombstoned_count", JsonSchema("integer"));
    output_props.add_child("deleted_count", JsonSchema("integer"));
    output_props.add_child("segments_before", JsonSchema("integer"));
    output_props.add_child("segments_after", JsonSchema("integer"));
    output_props.add_child("postings_reclaimed", JsonSchema("integer"));
    output_props.add_child("vectors_reclaimed", JsonSchema("integer"));

    util::Json output_schema = JsonSchema("object", "memory.compact output.");
    output_schema.put("additionalProperties", false);
    output_schema.add_child("properties", output_props);
    SetRequired(&output_schema,
                {"triggered", "capacity_blocked", "mem_before_bytes", "disk_before_bytes", "mem_after_bytes",
                 "disk_after_bytes", "summarized_count", "tombstoned_count", "deleted_count", "segments_before",
                 "segments_after", "postings_reclaimed", "vectors_reclaimed"});

    util::Json semantics;
    semantics.put("effect", "Compaction may create tombstones and reclaim storage asynchronously.");

    method.add_child("input_schema", input_schema);
    method.add_child("output_schema", output_schema);
    method.add_child("semantics", semantics);
    method.add_child("errors", StringArray({"jsonrpc.invalid_params"}));
    if (options.include_examples) {
        method.add_child("examples", BuildCompactExamples());
    }
    return method;
}

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

    if (method == "memory.write") {
        response.add_child("result", HandleWrite(params));
        return response;
    }
    if (method == "memory.query") {
        response.add_child("result", HandleQuery(params));
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
    if (method == "store.compact") {
        const auto result = HandleStoreCompact(params);
        if (util::GetBoolOr(result, "busy", false)) {
            response.add_child("error", Error(-32001, "store compact is busy", id).get_child("error"));
            return response;
        }
        response.add_child("result", result);
        return response;
    }
    if (method == "memory.describe") {
        response.add_child("result", HandleDescribe(params));
        return response;
    }

    response.add_child("error", Error(-32601, "method not found", id).get_child("error"));
    return response;
}

util::Json McpDispatcher::HandleWrite(const util::Json& params) {
    WriteInput in;
    in.workspace_id = util::GetStringOr(params, "workspace_id", "");
    in.session_id   = util::GetStringOr(params, "session_id", "default");
    in.write_mode   = util::GetStringOr(params, "write_mode", "upsert");

    if (const auto records_opt = params.get_child_optional("records")) {
        for (const auto& node : *records_opt) {
            const auto& rec_json = node.second;
            WriteRecordInput rec;
            rec.record_id  = util::GetStringOr(rec_json, "record_id", "");
            rec.source     = util::GetStringOr(rec_json, "source", "turn");
            rec.content    = util::GetStringOr(rec_json, "content", "");
            rec.tags       = util::ReadStringArray(rec_json, "tags");
            rec.importance = GetDoubleOr(rec_json, "importance", 1.0);
            rec.ttl_s      = util::GetUint64Or(rec_json, "ttl_s", 0);
            rec.pin        = util::GetBoolOr(rec_json, "pin", false);
            rec.dedup_key  = util::GetStringOr(rec_json, "dedup_key", "");

            if (const auto metadata_opt = rec_json.get_child_optional("metadata")) {
                for (const auto& kv : *metadata_opt) {
                    rec.metadata[kv.first] = kv.second.get_value<std::string>();
                }
            }

            in.records.push_back(std::move(rec));
        }
    }

    const auto out = engine_->Write(in);

    util::Json result;
    result.put("ok", out.ok);
    result.add_child("stored_ids", util::MakeArray(out.stored_ids));
    result.add_child("deduped_ids", util::MakeArray(out.deduped_ids));
    result.put("index_generation", out.index_generation);
    result.add_child("warnings", util::MakeArray(out.warnings));
    return result;
}

util::Json McpDispatcher::HandleQuery(const util::Json& params) {
    QueryInput in;
    in.workspace_id = util::GetStringOr(params, "workspace_id", "");
    in.query        = util::GetStringOr(params, "query", "");
    in.top_k        = static_cast<size_t>(util::GetIntOr(params, "top_k", 8));
    in.token_budget = util::GetIntOr(params, "token_budget", 2048);
    in.debug        = util::GetBoolOr(params, "debug", false);

    if (const auto filter_opt = params.get_child_optional("filters")) {
        in.filters.session_id        = util::GetStringOr(*filter_opt, "session_id", "");
        in.filters.sources           = util::ReadStringArray(*filter_opt, "sources");
        in.filters.tags_any          = util::ReadStringArray(*filter_opt, "tags_any");
        in.filters.updated_after_ms  = util::GetUint64Or(*filter_opt, "updated_after_ms", 0);
        in.filters.updated_before_ms = util::GetUint64Or(*filter_opt, "updated_before_ms", 0);
        in.filters.pinned_only       = util::GetBoolOr(*filter_opt, "pinned_only", false);
    }

    if (const auto recall_opt = params.get_child_optional("recall")) {
        in.recall.sparse_k   = static_cast<size_t>(util::GetIntOr(*recall_opt, "sparse_k", 200));
        in.recall.dense_k    = static_cast<size_t>(util::GetIntOr(*recall_opt, "dense_k", 200));
        in.recall.oversample = static_cast<size_t>(util::GetIntOr(*recall_opt, "oversample", 4));
    }

    if (const auto rerank_opt = params.get_child_optional("rerank")) {
        in.rerank.w_sparse    = GetDoubleOr(*rerank_opt, "w_sparse", 0.55);
        in.rerank.w_dense     = GetDoubleOr(*rerank_opt, "w_dense", 0.30);
        in.rerank.w_freshness = GetDoubleOr(*rerank_opt, "w_freshness", 0.10);
        in.rerank.w_pin       = GetDoubleOr(*rerank_opt, "w_pin", 0.05);
    }

    const auto out = engine_->Query(in);

    util::Json result;
    util::Json hits;
    for (const auto& hit : out.hits) {
        util::Json node;
        node.put("memory_id", hit.memory_id);
        node.put("source", hit.source);
        node.put("content", hit.content);
        node.add_child("tags", util::MakeArray(hit.tags));

        util::Json metadata;
        for (const auto& kv : hit.metadata) {
            metadata.put(kv.first, kv.second);
        }
        node.add_child("metadata", metadata);

        util::Json scores;
        scores.put("sparse", hit.scores.sparse);
        scores.put("dense", hit.scores.dense);
        scores.put("freshness", hit.scores.freshness);
        scores.put("pin", hit.scores.pin);
        scores.put("final", hit.scores.final);
        node.add_child("scores", scores);

        node.put("updated_at_ms", hit.updated_at_ms);
        node.put("pinned", hit.pinned);
        hits.push_back(std::make_pair("", node));
    }
    result.add_child("hits", hits);

    util::Json debug_stats;
    debug_stats.put("sparse_candidates", out.debug_stats.sparse_candidates);
    debug_stats.put("dense_candidates", out.debug_stats.dense_candidates);
    debug_stats.put("merged_candidates", out.debug_stats.merged_candidates);
    util::Json latency;
    latency.put("sparse", out.debug_stats.sparse_ms);
    latency.put("dense", out.debug_stats.dense_ms);
    latency.put("rerank", out.debug_stats.rerank_ms);
    latency.put("total", out.debug_stats.total_ms);
    debug_stats.add_child("latency_ms", latency);
    result.add_child("debug_stats", debug_stats);

    return result;
}

util::Json McpDispatcher::HandlePin(const util::Json& params) {
    const std::string workspace_id = util::GetStringOr(params, "workspace_id", "");
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
    const std::string workspace_id = util::GetStringOr(params, "workspace_id", "");
    const std::string window       = util::GetStringOr(params, "window", "5m");
    const auto stats               = engine_->Stats(workspace_id, window);

    util::Json result;
    result.put("p95_read_ms", stats.p95_read_ms);
    result.put("p95_write_ms", stats.p95_write_ms);
    result.put("token_reduction_ratio", stats.token_reduction_ratio);
    result.put("fallback_rate", stats.fallback_rate);
    result.put("mem_used_bytes", stats.mem_used_bytes);
    result.put("disk_used_bytes", stats.disk_used_bytes);
    result.put("resident_used_bytes", stats.resident_used_bytes);
    result.put("resident_limit_bytes", stats.resident_limit_bytes);
    result.put("resident_evicted_count", stats.resident_evicted_count);
    result.put("disk_fallback_search_count", stats.disk_fallback_search_count);
    result.put("item_count", stats.item_count);
    result.put("tombstone_count", stats.tombstone_count);
    result.put("gc_last_run_ms", stats.gc_last_run_ms);
    result.put("gc_evicted_count", stats.gc_evicted_count);
    result.put("capacity_blocked", stats.capacity_blocked);
    result.put("write_ack_mode", stats.write_ack_mode);
    result.put("effective_backend", stats.effective_backend);

    util::Json index_stats;
    index_stats.put("segment_count", stats.index_stats.segment_count);
    index_stats.put("posting_terms", stats.index_stats.posting_terms);
    index_stats.put("vector_count", stats.index_stats.vector_count);
    index_stats.put("query_cache_hit_rate", stats.index_stats.query_cache_hit_rate);
    index_stats.put("dense_probe_count_p95", stats.index_stats.dense_probe_count_p95);
    index_stats.put("cold_rehydrate_count", stats.index_stats.cold_rehydrate_count);
    result.add_child("index_stats", index_stats);
    return result;
}

util::Json McpDispatcher::HandleCompact(const util::Json& params) {
    const std::string workspace_id = util::GetStringOr(params, "workspace_id", "");
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
    result.put("segments_before", out.segments_before);
    result.put("segments_after", out.segments_after);
    result.put("postings_reclaimed", out.postings_reclaimed);
    result.put("vectors_reclaimed", out.vectors_reclaimed);
    return result;
}

util::Json McpDispatcher::HandleStoreCompact(const util::Json&) {
    const auto out = engine_->StoreCompact();

    util::Json result;
    result.put("triggered", out.triggered);
    result.put("noop", out.noop);
    result.put("busy", out.busy);
    result.put("async", out.async);
    result.put("partition_count", out.partition_count);
    result.put("message", out.message);
    return result;
}

util::Json McpDispatcher::Describe() const { return Describe(DescribeOptions{}); }

util::Json McpDispatcher::Describe(const DescribeOptions& options) const {
    util::Json result;
    result.put("describe_version", "2.0.0");
    result.put("schema_revision", "2026-02-19.1");
    result.put("generated_at_ms", util::NowMs());

    result.put("server.name", "poorguy-mem");
    result.put("server.kind", "memory-server");
    result.put("server.protocol", "jsonrpc-2.0");
    result.put("server.transport", "http");
    result.put("server.endpoint", "/mcp");
    result.put("server.describe_endpoint", "/mcp/describe");
    result.put("server.sync_routes_available", false);
    result.put("server.write_ack_mode", "durable");
    result.add_child("server.supported_backends", StringArray({"eloqstore", "inmemory"}));

    result.add_child("method_names", StringArray({"memory.describe", "memory.write", "memory.query", "memory.pin",
                                                    "memory.stats", "memory.compact"}));

    util::Json methods;
    methods.push_back(std::make_pair("memory.describe", BuildDescribeMethod(options)));
    methods.push_back(std::make_pair("memory.write", BuildWriteMethod(options)));
    methods.push_back(std::make_pair("memory.query", BuildQueryMethod(options)));
    methods.push_back(std::make_pair("memory.pin", BuildPinMethod(options)));
    methods.push_back(std::make_pair("memory.stats", BuildStatsMethod(options)));
    methods.push_back(std::make_pair("memory.compact", BuildCompactMethod(options)));
    result.add_child("methods", methods);

    util::Json errors;
    util::Json http_errors;
    http_errors.push_back(
        std::make_pair("", HttpError("invalid_json_http", 400, "invalid JSON", "malformed request body")));
    http_errors.push_back(std::make_pair(
        "", HttpError("route_not_found_http", 404, "route not found", "unsupported path such as /sync/push or /sync/pull")));

    util::Json jsonrpc_errors;
    jsonrpc_errors.push_back(std::make_pair(
        "", JsonRpcErrorEntry("invalid_request", -32600, "invalid request", "invalid jsonrpc envelope")));
    jsonrpc_errors.push_back(std::make_pair(
        "", JsonRpcErrorEntry("method_not_found", -32601, "method not found", "unknown method on POST /mcp")));
    jsonrpc_errors.push_back(std::make_pair(
        "", JsonRpcErrorEntry("invalid_params", -32602, "invalid params", "params or arguments shape mismatch")));
    jsonrpc_errors.push_back(std::make_pair(
        "", JsonRpcErrorEntry("store_compact_busy", -32001, "store compact is busy", "store.compact async task already running")));

    errors.add_child("http", http_errors);
    errors.add_child("jsonrpc", jsonrpc_errors);
    result.add_child("errors", errors);

    return result;
}

util::Json McpDispatcher::HandleDescribe(const util::Json& params) {
    DescribeOptions options;
    options.include_examples = util::GetBoolOr(params, "include_examples", false);
    return Describe(options);
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
