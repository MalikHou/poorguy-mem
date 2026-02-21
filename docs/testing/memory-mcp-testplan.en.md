# Memory MCP Test Plan (EN)

## 1. Objective

This plan validates engineering quality of Memory MCP across functional correctness, API stability, recovery behavior, and performance boundaries.

## 2. Scope

Covered areas:
- MCP APIs: `memory.describe/write/query/pin/stats/compact`
- retrieval pipeline: sparse + dense + rerank
- storage backends: `eloqstore` and `inmemory`
- capacity governance: pin, TTL, tombstone, compact

## 3. Test Environment and Preconditions

1. dependency probes pass:
```bash
scripts/install_deps.sh --verify-only
```
2. build is completed (testing enabled recommended):
```bash
cmake -S . -B build -DBUILD_TESTING=ON -DPGMEM_STORE_BACKEND=eloqstore
cmake --build build -j"$(nproc)"
```
3. verify pipeline performs runtime reset before each stage (`.pgmem/.mock-bin/.mock-state`).

## 4. Unit Test Matrix

### U-01 Analyzer
- tokenization correctness
- case normalization
- empty input behavior

### U-02 Sparse Scoring
- term-match monotonicity
- relevance ranking correctness

### U-03 Dense and Rerank
- dense candidate recall path
- stable multi-signal score fusion

### U-04 Pin Governance
- pin/unpin happy path
- quota and ratio constraints
- pin impact on ranking and reclamation choice

### U-05 TTL and Tombstone
- expired record filtering
- tombstoned record exclusion

## 5. Integration Scenarios

### I-01 Minimal Closed Loop
`write -> query -> pin -> compact -> stats`

### I-02 Workspace Isolation
- records are not cross-queryable across different `workspace_id`.

### I-03 Backend Consistency
- response shape stays consistent between `eloqstore` and `inmemory` for equivalent input.

### I-04 Concurrency Consistency
- after concurrent read/write load, written tokens remain retrievable.

### I-05 Startup/Shutdown Health
- `/health` readiness behavior is correct.
- process exits within timeout on SIGTERM.

## 6. Fault Injection Scenarios

1. batch write failure: validate failure visibility and response semantics.
2. checkpoint anomaly: validate replay continuation behavior.
3. compact interruption: validate continued availability after interruption.
4. io_uring unavailable (eloqstore): validate startup-fail contract.

## 7. Performance Acceptance Targets

- recall target: `Recall@10 >= 0.95`
- latency target: `p95 query <= 180ms`
- write target: `p95 write <= 120ms`

Note: large-scale benchmarks should run in dedicated benchmark environments.

## 8. Execution Entry

```bash
ctest --test-dir build --output-on-failure
scripts/start.sh verify
scripts/start.sh verify --keep-artifacts
```

## 9. Acceptance Criteria

1. all unit tests pass.
2. all verify stages pass through `scripts/start.sh verify`.
3. coverage gate meets threshold (currently 85%).
4. output field types of key APIs match the API contract.
