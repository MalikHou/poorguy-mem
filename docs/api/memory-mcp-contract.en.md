# Memory MCP API Contract (EN)

## 1. Purpose and Scope

This document is the normative API contract for Memory MCP, including request format, response format, error model, and runtime semantics.

Audience:
- MCP client implementers
- gateway/platform integrators
- engineering teams integrating at field-level schema granularity

The public method set is fixed:
- `memory.describe`
- `memory.write`
- `memory.query`
- `memory.pin`
- `memory.stats`
- `memory.compact`

## 2. Protocol and Transport Model

### 2.1 HTTP Endpoints

- health: `GET /health`
- MCP call: `POST /mcp`
- contract describe: `GET /mcp/describe`

### 2.2 JSON-RPC Endpoints

`POST /mcp` supports JSON-RPC 2.0 for:
- `initialize`
- `tools/list`
- `tools/call`
- direct method calls (`memory.*`)

### 2.3 Response Envelope

Direct MCP response:
```json
{"id": 1, "result": {...}}
```
or
```json
{"id": 1, "error": {"code": -32601, "message": "method not found"}}
```

JSON-RPC response:
```json
{"jsonrpc": "2.0", "id": 1, "result": {...}}
```
or
```json
{"jsonrpc": "2.0", "id": 1, "error": {"code": -32602, "message": "invalid params"}}
```

## 3. Common Type System and Conventions

### 3.1 JSON Types

- `string`: UTF-8 text
- `integer`: JSON numeric value with integer semantics (not string)
- `number`: JSON floating-point numeric value (not string)
- `boolean`: `true/false`
- `object`: key-value map
- `array<T>`: homogeneous array

### 3.2 Shared Field Conventions

- `workspace_id`: primary logical isolation key
- `session_id`: session scope key, default `default`
- `updated_at_ms`: Unix epoch milliseconds
- `token_budget`: response content budget (approximate token estimation)

### 3.3 Ordering and Stability

- `memory.query` returns `hits` sorted by descending `scores.final`.
- filters and budget truncation are applied after ranking.

## 4. Public Method Set

| Method | Description |
|---|---|
| `memory.describe` | Returns server capability, methods, and schema |
| `memory.write` | Writes records and updates index |
| `memory.query` | Runs sparse+dense hybrid retrieval |
| `memory.pin` | Sets or clears record pin state |
| `memory.stats` | Returns runtime and index metrics |
| `memory.compact` | Runs compaction and cleanup |

## 5. Method Specifications

### 5.1 `memory.describe`

#### Purpose
Returns a machine-consumable formal contract for client-side validation, call-shape generation, and unified error handling.

#### Request Fields

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `include_examples` | boolean | no | `false` | when `true`, include per-method `examples` |

#### Response Fields (core)
- `describe_version`: contract major version (parser compatibility gate)
- `schema_revision`: schema revision id within the major version
- `generated_at_ms`: server generation timestamp in milliseconds
- `server`: server metadata
- `method_names`: public method name list
- `methods`: method contract map (flat keys, e.g. `methods["memory.write"]`)
- `errors.http[]`: HTTP error model
- `errors.jsonrpc[]`: JSON-RPC error model

Each `methods[<method_name>]` includes:

| Field | Type | Description |
|---|---|---|
| `summary` | string | method summary |
| `input_schema` | object | JSON-Schema-style input contract |
| `output_schema` | object | JSON-Schema-style output contract |
| `semantics` | object | runtime semantics (ordering/budget/idempotency/constraints) |
| `errors` | array<string> | method-level error references |
| `examples` | array<object> | optional, returned only with `include_examples=true` |

#### Success Example (default: no examples)
```json
{
  "id": 1,
  "result": {
    "describe_version": "2.0.0",
    "schema_revision": "2026-02-19.1",
    "generated_at_ms": 1771503385237,
    "server": {
      "name": "poorguy-mem",
      "kind": "memory-server",
      "protocol": "jsonrpc-2.0",
      "transport": "http",
      "endpoint": "/mcp",
      "describe_endpoint": "/mcp/describe",
      "sync_routes_available": false,
      "write_ack_mode": "durable",
      "supported_backends": ["eloqstore", "inmemory"]
    },
    "method_names": [
      "memory.describe",
      "memory.write",
      "memory.query",
      "memory.pin",
      "memory.stats",
      "memory.compact"
    ],
    "methods": {
      "memory.describe": {
        "summary": "Return machine-readable server contract and method schemas.",
        "input_schema": {
          "type": "object",
          "properties": {
            "include_examples": {"type": "boolean", "default": false}
          }
        },
        "output_schema": {"type": "object"},
        "semantics": {
          "payload_size": "Examples are omitted unless include_examples=true."
        },
        "errors": ["jsonrpc.method_not_found", "jsonrpc.invalid_params"]
      }
    },
    "errors": {
      "http": [
        {"name": "invalid_json_http", "status": 400, "message": "invalid JSON", "when": "malformed request body"}
      ],
      "jsonrpc": [
        {"name": "method_not_found", "code": -32601, "message": "method not found", "when": "unknown method on POST /mcp"}
      ]
    }
  }
}
```

#### Success Example (`include_examples=true`)
```json
{
  "id": 1,
  "result": {
    "methods": {
      "memory.write": {
        "examples": [
          {
            "name": "write_success",
            "request": {"method": "memory.write", "params": {"workspace_id": "demo"}},
            "response": {"result": {"ok": true}}
          }
        ]
      }
    }
  }
}
```

#### Failure Example (invalid param type)
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {"code": -32602, "message": "invalid params"}
}
```
Note: for example, passing non-boolean `include_examples` should be rejected by client/gateway validation and may be rejected by server parameter checks.

#### `/mcp/describe` Alignment
- `GET /mcp/describe` returns the same object shape as `memory.describe.result`.
- Prefer `memory.describe` for unified JSON-RPC flows; use `/mcp/describe` for read-only contract fetch.

#### Client Consumption Guidance
1. Call `memory.describe` at startup and validate `describe_version` + `schema_revision`.
2. Validate request payloads using `methods[method_name].input_schema`.
3. Decode and type-check responses using `methods[method_name].output_schema`.
4. Build a unified error map from `errors.http/jsonrpc`.
5. Keep `include_examples=false` for normal traffic; use `true` for debugging or codegen.

### 5.2 `memory.write`

#### Purpose
Persists records and updates retrieval indexes.

#### Request Fields

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `workspace_id` | string | yes | - | workspace identifier |
| `session_id` | string | no | `default` | session identifier |
| `records` | array<object> | yes | - | record list |
| `write_mode` | string | no | `upsert` | `upsert` or `append` |

`records[i]` fields:

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `record_id` | string | no | auto-generated | record id |
| `source` | string | no | `turn` | source type |
| `content` | string | yes | - | main content |
| `tags` | array<string> | no | `[]` | tags |
| `importance` | number | no | `1.0` | importance score |
| `ttl_s` | integer | no | `0` | TTL seconds |
| `pin` | boolean | no | `false` | pin flag |
| `dedup_key` | string | no | `""` | dedup key |
| `metadata` | object<string,string> | no | `{}` | extension metadata |

#### Response Fields

| Field | Type | Description |
|---|---|---|
| `ok` | boolean | write success flag |
| `stored_ids` | array<string> | actually stored ids |
| `deduped_ids` | array<string> | dedup-hit ids |
| `index_generation` | integer | index generation |
| `warnings` | array<string> | non-fatal warnings |

#### Success Example
```json
{
  "id": 2,
  "result": {
    "ok": true,
    "stored_ids": ["m-00000001771503385237-00000000000000000000"],
    "deduped_ids": [],
    "index_generation": 1,
    "warnings": []
  }
}
```

#### Failure Example
```json
{
  "id": 2,
  "result": {
    "ok": false,
    "stored_ids": [],
    "deduped_ids": [],
    "index_generation": 0,
    "warnings": ["workspace_id is required"]
  }
}
```

### 5.3 `memory.query`

#### Purpose
Runs hybrid recall and rerank, then returns ranked hits.

#### Request Fields

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `workspace_id` | string | yes | - | workspace identifier |
| `query` | string | yes | - | query text |
| `top_k` | integer | no | `8` | max hit count |
| `token_budget` | integer | no | `2048` | content budget |
| `filters` | object | no | `{}` | filter object |
| `recall` | object | no | `{}` | recall options |
| `rerank` | object | no | `{}` | rerank weights |
| `debug` | boolean | no | `false` | include debug stats |

`filters`: `session_id/sources/tags_any/updated_after_ms/updated_before_ms/pinned_only`.

`recall`: `sparse_k/dense_k/oversample`.

`rerank`: `w_sparse/w_dense/w_freshness/w_pin`.

#### Response Fields

| Field | Type | Description |
|---|---|---|
| `hits` | array<object> | ranked hits |
| `hits[].memory_id` | string | record id |
| `hits[].content` | string | content |
| `hits[].scores` | object | score components |
| `hits[].scores.final` | number | final score |
| `debug_stats` | object | debug counters |
| `debug_stats.latency_ms` | object | stage latency |

#### Success Example
```json
{
  "id": 3,
  "result": {
    "hits": [
      {
        "memory_id": "m-00000001771503385237-00000000000000000000",
        "content": "remember retry with exponential backoff",
        "scores": {"sparse": 1.0, "dense": 1.0, "freshness": 0.99, "pin": 0.0, "final": 0.94},
        "updated_at_ms": 1771503385237,
        "pinned": false
      }
    ],
    "debug_stats": {
      "sparse_candidates": 1,
      "dense_candidates": 1,
      "merged_candidates": 1,
      "latency_ms": {"sparse": 0.01, "dense": 0.04, "rerank": 0.01, "total": 0.09}
    }
  }
}
```

#### Failure Example
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "error": {"code": -32602, "message": "invalid params: params must be object"}
}
```
Note: this is returned when JSON-RPC calls `memory.query` with non-object `params`.

### 5.4 `memory.pin`

#### Purpose
Pins or unpins an existing record.

#### Request Fields

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `workspace_id` | string | yes | - | workspace identifier |
| `memory_id` | string | yes | - | record id |
| `pin` | boolean | no | `true` | `true`=pin, `false`=unpin |

#### Response Fields

| Field | Type | Description |
|---|---|---|
| `ok` | boolean | operation result |
| `error` | string | failure reason (only when `ok=false`) |

#### Success Example
```json
{
  "id": 4,
  "result": {"ok": true}
}
```

#### Failure Example
```json
{
  "id": 4,
  "result": {"ok": false, "error": "record not found"}
}
```

### 5.5 `memory.stats`

#### Purpose
Returns runtime capacity, latency, and index counters.

#### Request Fields

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `workspace_id` | string | no | `""` | empty means global stats |
| `window` | string | no | `5m` | window hint |

#### Response Fields (core)

| Field | Type | Description |
|---|---|---|
| `p95_read_ms` | number | read p95 latency |
| `p95_write_ms` | number | write p95 latency |
| `mem_used_bytes` | integer | estimated memory usage |
| `disk_used_bytes` | integer | estimated disk usage |
| `resident_used_bytes` | integer | resident memory usage |
| `resident_limit_bytes` | integer | resident memory limit |
| `resident_evicted_count` | integer | resident eviction count |
| `disk_fallback_search_count` | integer | disk fallback query count |
| `write_ack_mode` | string | write acknowledgment mode |
| `effective_backend` | string | active backend |
| `index_stats` | object | index statistics |

#### Success Example
```json
{
  "id": 5,
  "result": {
    "p95_read_ms": 0.1,
    "p95_write_ms": 40.0,
    "mem_used_bytes": 5199,
    "disk_used_bytes": 4295020544,
    "resident_used_bytes": 1621,
    "resident_limit_bytes": 536870912,
    "resident_evicted_count": 0,
    "disk_fallback_search_count": 0,
    "write_ack_mode": "durable",
    "effective_backend": "eloqstore",
    "index_stats": {"segment_count": 1, "posting_terms": 5, "vector_count": 1}
  }
}
```

#### Failure Example
```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "error": {"code": -32602, "message": "invalid params: params must be object"}
}
```

### 5.6 `memory.compact`

#### Purpose
Triggers compaction/GC and returns reclamation counters.

#### Request Fields

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `workspace_id` | string | no | `""` | empty means global compaction |

#### Response Fields (core)

| Field | Type | Description |
|---|---|---|
| `triggered` | boolean | compaction triggered |
| `capacity_blocked` | boolean | capacity pressure remains after compaction |
| `mem_before_bytes` | integer | memory bytes before |
| `disk_before_bytes` | integer | disk bytes before |
| `mem_after_bytes` | integer | memory bytes after |
| `disk_after_bytes` | integer | disk bytes after |
| `tombstoned_count` | integer | tombstones created |
| `deleted_count` | integer | physically deleted records |

#### Success Example
```json
{
  "id": 6,
  "result": {
    "triggered": true,
    "capacity_blocked": false,
    "mem_before_bytes": 5199,
    "disk_before_bytes": 4295020544,
    "mem_after_bytes": 5246,
    "disk_after_bytes": 4295032832,
    "tombstoned_count": 1,
    "deleted_count": 0
  }
}
```

#### Failure Example
```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "error": {"code": -32602, "message": "invalid params: params must be object"}
}
```

## 6. Error Model

### 6.1 HTTP Layer

| Scenario | Status | Payload |
|---|---|---|
| request body JSON parse error | `400` | `{"error":"invalid JSON"}` |
| unknown route (for example `/sync/push`) | `404` | `{"error":"route not found"}` |

### 6.2 JSON-RPC Layer

| code | Meaning | Typical trigger |
|---|---|---|
| `-32600` | invalid request | bad `jsonrpc`, missing `method` |
| `-32601` | method not found | unknown method |
| `-32602` | invalid params | invalid `params/arguments` shape |
| `-32001` | store compact busy | internal `store.compact` async task busy |

## 7. Semantic Rules

### 7.1 Idempotency and Write Semantics

- `memory.write` with `write_mode=upsert` may return `deduped_ids` when `dedup_key` hits.
- `write_mode=append` skips dedup merge.

### 7.2 Query Semantics

- `memory.query` is ranked by final score.
- `token_budget` is a hard truncation condition; `0` means unlimited.

### 7.3 Pin Constraint Semantics

- `pin` is guarded by quota and ratio governance.
- over-limit `memory.pin` returns `ok=false`; `memory.write(pin=true)` may return warnings.

### 7.4 Type Stability

- external responses use strict JSON types: numeric fields as number/integer, boolean fields as boolean.
- clients must not rely on numeric-string payloads.

## 8. Internal Ops Interface (Outside Public MCP Method Set)

Internal direct-call method: `store.compact`.

Return fields:
- `triggered`
- `noop`
- `busy`
- `async`
- `partition_count`
- `message`

Constraints:
- not listed in `tools/list`
- not part of public `memory.*` method contract
