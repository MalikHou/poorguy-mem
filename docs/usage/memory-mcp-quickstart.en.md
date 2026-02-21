# Memory MCP Quickstart (EN)

## 1. Start

### 1.1 Build and Launch

```bash
scripts/install.sh
scripts/start.sh --backend eloqstore
```

### 1.2 Health Check

```bash
curl -sS http://127.0.0.1:8765/health
```

Expected response:
```json
{"status":"ok"}
```

### 1.3 Fetch Machine Contract (Recommended)

```bash
# default compact contract (no examples)
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 100,
    "method": "memory.describe",
    "params": {}
  }'
```

```bash
# include examples for debugging/code generation
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 101,
    "method": "memory.describe",
    "params": {"include_examples": true}
  }'
```

## 2. Minimal Write + Query

### 2.1 Write

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
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

Expected: `result.ok=true` with non-empty `stored_ids`.

### 2.2 Query

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 2,
    "method": "memory.query",
    "params": {
      "workspace_id": "demo",
      "query": "retry backoff",
      "top_k": 3,
      "token_budget": 1200,
      "debug": true
    }
  }'
```

Expected response:
- non-empty `hits`
- `hits[0].memory_id` is a string
- `debug_stats` contains stage latency

## 3. pin / stats / compact Operations

### 3.1 pin and unpin

```bash
# pin
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 3,
    "method": "memory.pin",
    "params": {
      "workspace_id": "demo",
      "memory_id": "m-REPLACE-WITH-ID",
      "pin": true
    }
  }'

# unpin
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 4,
    "method": "memory.pin",
    "params": {
      "workspace_id": "demo",
      "memory_id": "m-REPLACE-WITH-ID",
      "pin": false
    }
  }'
```

### 3.2 stats

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 5,
    "method": "memory.stats",
    "params": {
      "workspace_id": "demo",
      "window": "5m"
    }
  }'
```

Key fields:
- `write_ack_mode`
- `effective_backend`
- `resident_used_bytes`
- `resident_limit_bytes`
- `index_stats`

### 3.3 compact

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 6,
    "method": "memory.compact",
    "params": {
      "workspace_id": "demo"
    }
  }'
```

Key fields:
- `triggered`
- `capacity_blocked`
- `mem_before_bytes` / `mem_after_bytes`
- `disk_before_bytes` / `disk_after_bytes`

## 4. Common Errors and Diagnostics

### 4.1 `400 invalid JSON`

Symptom: request body is not valid JSON.

Checklist:
1. validate payload with `jq .`.
2. verify quotes and comma placement.

### 4.2 `-32601 method not found`

Symptom: method is outside the public set.

Checklist:
1. use only `memory.describe/write/query/pin/stats/compact`.
2. call `memory.describe` to fetch current method list.

### 4.3 `memory.pin` returns `ok=false`

Symptom: target record is missing or pin governance blocks the operation.

Checklist:
1. run `memory.query` first and confirm `memory_id`.
2. inspect the `error` field in response.

### 4.4 `eloqstore` startup failure (io_uring)

Symptom: process exits during startup and logs `io_uring unavailable for eloqstore backend`.

Checklist:
1. verify host kernel and container privilege setup.
2. ensure runtime environment allows io_uring.

## 5. Production Recommendations (gateway / auth / resource budget)

1. run `pgmemd` behind an API gateway (Nginx/Caddy/Ingress).
2. enforce TLS and authentication at gateway layer (JWT/API key/mTLS).
3. assign dedicated `workspace_id` per tenant for logical isolation.
4. size `--mem-budget-mb` and `--disk-budget-gb` to peak capacity.
5. collect `memory.stats` periodically and set alert thresholds.
