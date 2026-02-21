# Memory MCP Query Pipeline Design (EN)

## 1. Objectives and Lifecycle (What / Why)

`memory.query` is designed to guarantee:
1. controllable keyword recall (sparse) and semantic recall (dense),
2. explainable ranking through multi-signal fusion,
3. stable output under a fixed `token_budget`.

Standard lifecycle:
`input parse -> sparse recall -> dense recall -> merge -> rerank -> filtering -> budget truncation -> output`

## 2. Execution Model (How)

Execution owner: `HybridRetriever` (`src/core/retriever.cpp`)

Input model: `QueryInput` (`include/pgmem/types.h`)
- `workspace_id`
- `query`
- `top_k`
- `token_budget`
- `filters`
- `recall`
- `rerank`
- `debug`

Output model: `QueryOutput`
- `hits`
- `debug_stats`

## 3. Sparse Recall (tokenization / term stats / BM25-like)

Flow:
1. `IAnalyzer` tokenizes the query.
2. term statistics and document term frequencies are evaluated in workspace scope.
3. lexical score (BM25-like) is computed.
4. sparse candidate pool is selected (`recall.sparse_k` and `oversample`).

Purpose:
- preserve strong keyword and phrase recall,
- provide a stable floor when dense quality is weak.

## 4. Dense Recall (embedding / LSH index / candidate generation)

Flow:
1. `IEmbeddingProvider` generates query vectors (default `HashEmbeddingProvider`).
2. `IVectorIndex` performs nearest-neighbor retrieval (default `LshVectorIndex`).
3. candidates are filtered by workspace scope.
4. dense candidate pool is generated (`recall.dense_k` and `oversample`).

Purpose:
- capture semantic similarity and paraphrase matches,
- complement sparse retrieval.

## 5. Score Fusion (`sparse/dense/freshness/pin/final`)

Candidates are merged by `memory_id`, then score components are computed:
- `sparse`: lexical relevance
- `dense`: vector similarity
- `freshness`: time-decay signal
- `pin`: retention-priority signal

Final score:

```text
final = w_sparse*sparse + w_dense*dense + w_freshness*freshness + w_pin*pin
```

Weights come from `QueryInput.rerank`; defaults are used when absent.

## 6. Filters and `token_budget` Truncation

Filter fields:
- `session_id`
- `sources`
- `tags_any`
- `updated_after_ms`
- `updated_before_ms`
- `pinned_only`

Execution order:
1. filtering is applied after ranking,
2. TTL-expired hits are removed,
3. token estimates are accumulated per hit; output stops when `token_budget` is exceeded.

Semantic rule:
- `token_budget = 0` means budget truncation is disabled.

## 7. Tuning Guide (recall / rerank)

Recall knobs:
- `recall.sparse_k`: higher value improves keyword coverage
- `recall.dense_k`: higher value improves semantic coverage
- `recall.oversample`: higher value increases pre-merge candidate redundancy

Rerank knobs:
- `w_sparse`: stronger lexical preference
- `w_dense`: stronger semantic preference
- `w_freshness`: stronger recency preference
- `w_pin`: stronger pin priority

Tuning guidance:
1. terminology-heavy tasks: increase `w_sparse`.
2. semantic expansion tasks: increase `w_dense`.
3. policy/manual-like workloads: increase `w_pin`.

## 8. Observability (`debug_stats` Fields and Diagnostics)

`debug_stats` fields:
- `sparse_candidates`
- `dense_candidates`
- `merged_candidates`
- `latency_ms.sparse`
- `latency_ms.dense`
- `latency_ms.rerank`
- `latency_ms.total`

Diagnostic patterns:
1. low `sparse_candidates` with weak relevance: inspect analyzer and term coverage.
2. low `dense_candidates`: inspect embedding dimension and vector index settings.
3. high `latency_ms.total`: inspect dense and rerank stage share first.
4. `merged_candidates` much smaller than recall totals: inspect filter strictness.
