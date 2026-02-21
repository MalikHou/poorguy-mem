# poorguy-mem

Memory MCP is a local-first memory service for AI applications. It provides durable write, hybrid retrieval, and operational controls through a single MCP endpoint.

## Product Positioning

Memory MCP solves one core problem: keep high-value conversation and task memory queryable with stable latency and predictable semantics.

It is designed for:
- agent memory persistence across sessions
- hybrid recall (keyword + semantic)
- controllable retention via `pin`, TTL, and compaction

## Core Capabilities

- `memory.write`: durable memory ingest with indexing
- `memory.query`: sparse+dense hybrid retrieval with rerank
- `memory.pin`: retention priority control
- `memory.stats`: runtime and index observability
- `memory.compact`: compaction and tombstone cleanup
- `memory.describe`: versioned machine-readable contract (`describe_version` + `schema_revision`, optional `include_examples`)

## Architecture Overview

Request flow:
1. MCP transport (`POST /mcp`) receives JSON/JSON-RPC request.
2. Dispatcher validates method and maps payload to typed inputs.
3. `MemoryEngine` coordinates storage, indexing, and retention logic.
4. `HybridRetriever` executes sparse recall, dense recall, merge, and rerank.
5. `StoreAdapter` persists data to `eloqstore` or `inmemory` backend.

Core modules:
- MCP layer: `src/mcp/mcp_dispatcher.cpp`
- Engine layer: `src/core/memory_engine.cpp`
- Retrieval layer: `src/core/retriever.cpp`
- Store layer: `src/store/eloqstore_adapter.cpp`, `src/store/in_memory_store_adapter.cpp`

## Documentation Index

### Product Docs

API:
- `docs/api/memory-mcp-contract.zh.md`
- `docs/api/memory-mcp-contract.en.md`

Architecture and low-level design:
- `docs/design/memory-mcp-storage-model.zh.md`
- `docs/design/memory-mcp-storage-model.en.md`
- `docs/design/memory-mcp-query-pipeline.zh.md`
- `docs/design/memory-mcp-query-pipeline.en.md`

Usage:
- `docs/usage/memory-mcp-quickstart.zh.md`
- `docs/usage/memory-mcp-quickstart.en.md`

### Engineering QA

- `docs/testing/memory-mcp-testplan.zh.md`
- `docs/testing/memory-mcp-testplan.en.md`

## Minimal Runnable Example

Start service:

```bash
scripts/install.sh
scripts/start.sh --backend eloqstore
```

Write:

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -d '{
    "id": 1,
    "method": "memory.write",
    "params": {
      "workspace_id": "demo",
      "session_id": "s1",
      "records": [
        {
          "source": "turn",
          "content": "remember retry with exponential backoff",
          "tags": ["retry", "ops"],
          "pin": false
        }
      ]
    }
  }'
```

Query:

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -d '{
    "id": 2,
    "method": "memory.query",
    "params": {
      "workspace_id": "demo",
      "query": "retry backoff",
      "top_k": 3,
      "token_budget": 1200
    }
  }'
```

Stats:

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -d '{
    "id": 3,
    "method": "memory.stats",
    "params": {"workspace_id": "demo", "window": "5m"}
  }'
```
