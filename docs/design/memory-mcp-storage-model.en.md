# Memory MCP Storage Model Design (EN)

## 1. Design Goals (What / Why)

The Memory MCP storage model is built to satisfy three constraints at once:
1. newly written records must become queryable in the interaction path with low delay,
2. sparse index, dense index, and document body must stay semantically aligned,
3. service behavior must remain stable under memory pressure via reclamation and compaction.

The core principle is: **document state, index state, and projection event state represent one logical record state**.

## 2. System Boundaries and Component Responsibilities (How)

The end-to-end path has four layers:
1. MCP layer (`McpDispatcher`): protocol decode, method dispatch, response encode.
2. Engine layer (`MemoryEngine`): lifecycle control, persistence orchestration, capacity governance.
3. Retrieval layer (`HybridRetriever`): in-memory index view, recall, and rerank.
4. Store layer (`IStoreAdapter`): key-value persistence, batch write, scan, usage estimation.

Primary source entry points:
- `src/mcp/mcp_dispatcher.cpp`
- `src/core/memory_engine.cpp`
- `src/core/retriever.cpp`
- `src/store/eloqstore_adapter.cpp`
- `src/store/in_memory_store_adapter.cpp`

## 3. EloqStore Logical Model (namespace / key / value / routing)

| Namespace | Key Pattern | Core Value Fields | Routing Key |
|---|---|---|---|
| `mem_docs` | `ws/{workspace}/doc/{doc_id}` | body, metadata, pin, ttl, timestamps | `workspace + doc_id` |
| `mem_term_dict` | `ws/{workspace}/term/{term}` | df/cf/max_tf/block_refs/updated_at | `workspace + term` |
| `mem_posting_blk` | `ws/{workspace}/term/{term}/b/{bucket}/blk/{blk_id}` | posting block (`doc/tf/flags`) | `workspace + term + bucket` |
| `mem_vec_code` | `ws/{workspace}/vec/{bucket}/{doc_id}` | quantized vector code, norm, model_id | `workspace + bucket` |
| `mem_vec_fp` | `ws/{workspace}/vecfp/{doc_id}` | full precision vector | `workspace + doc_id` |
| `mem_route_meta` | `ws/{workspace}` | bucket_count/hot_level/shard_hint | `workspace` |
| `ds_events` | `ws/{workspace}/ts/{ts}/seq/{seq}` | write/pin/compact event log | `workspace + seq` |
| `ds_projection_ckpt` | `ws/{workspace}` | replay checkpoint | `workspace` |

Routing intent:
- document/vector paths are co-located by `workspace + doc_id`,
- term/posting paths are co-located by `workspace + term`,
- event and checkpoint progression is workspace-local.

## 4. Write Transaction Unit (document, index, event, checkpoint)

`memory.write` builds a batch in engine layer and commits once via `BatchWrite`:
1. `mem_docs`: document rows
2. `mem_term_dict`: term-stat updates
3. `mem_posting_blk`: posting block append/update
4. `mem_vec_code` + `mem_vec_fp`: dense vector persistence
5. `ds_events`: event append
6. `ds_projection_ckpt`: replay position advance
7. `mem_route_meta`: workspace routing metadata update

Transaction objective:
- read path should observe either previous stable state or next complete state,
- avoid version skew between body and index.

## 5. Read Path and Index Visibility

The read path has two planes:
1. store visibility: namespace-level reads through `Scan/Get`,
2. retrieval visibility: `HybridRetriever` in-memory snapshot for recall and rerank.

Visibility rules:
- tombstoned or expired records are excluded from final hits,
- every hit must be resolvable to document payload fields,
- dense and sparse candidates are merged by the same `memory_id`.

## 6. Low-Level Semantics of pin / ttl / tombstone / compact

### 6.1 pin
- pin is a retention priority signal for ranking and eviction.
- pin is governed by `pin_quota_per_workspace` and `max_pinned_ratio`.

### 6.2 ttl
- when `ttl_s > 0`, expiry is evaluated against update time.
- expired records are filtered before final query output.

### 6.3 tombstone
- logical delete is represented by tombstone first to preserve replay consistency.
- tombstoned records are excluded from recall.

### 6.4 compact
- compaction further consolidates tombstones and cold segments.
- output includes reclamation counters and before/after capacity metrics.

## 7. Memory Budget and Reclamation Mechanism

Budget is driven by `MemoryEngineOptions`:
- `mem_budget_mb`
- `disk_budget_gb`
- resident-related thresholds

Core reclamation loop:
1. estimate resident charge per record,
2. pick evictable candidates when `resident_used_bytes` exceeds limit,
3. preserve pinned and hot records preferentially,
4. update `resident_evicted_count` and `capacity_blocked`.

Key capacity fields exposed by `memory.stats`:
- `resident_used_bytes`
- `resident_limit_bytes`
- `resident_evicted_count`
- `capacity_blocked`

## 8. Failure and Recovery (BatchWrite / checkpoint / replay)

### 8.1 BatchWrite failure
- a failed batch must not be treated as committed,
- write response returns `ok=false` or warning diagnostics.

### 8.2 Missing checkpoint
- replay can degrade to full event log scan recovery.

### 8.3 Event replay
- `Warmup` reads `ds_projection_ckpt` to locate start position,
- `ds_events` are applied sequentially to in-memory projection and retrieval index.

Recovery objective:
- converge to queryable consistent state after restart,
- avoid persistent divergence where events exist but index state is absent.

## 9. Performance Trade-offs (write amplification / fan-out / hot bucket split)

### 9.1 Write amplification
- a single write updates body, term dict, posting, vector, event, and checkpoint.
- cost: higher write amplification; gain: stronger consistency and replayability.

### 9.2 Query fan-out
- routing keys reduce probability of cluster-wide scans,
- sparse and dense candidate pools are bounded before merge.

### 9.3 Hot workspace bucket split
- hot workspace traffic can scale via bucketized write distribution,
- query fan-out is limited to target workspace buckets instead of global broadcast.
