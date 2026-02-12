# Poorguy-Mem PR Test Matrix

This document defines the required pull-request validation matrix for the local minimal mode.

## Scope

- Baseline refresh date: **2026-02-11**
- Supported backends: `eloqstore`, `inmemory`
- Policy: all listed jobs are required on every PR

## PR Gate Sequence (Fixed)

1. `scripts/install_deps.sh --verify-only`
2. Build (`PGMEM_STORE_BACKEND=eloqstore`, `Release`)
3. `ctest --test-dir build --output-on-failure`
4. Integration matrix (all required)
5. Client mock gate (`deploy_mcp.sh` + `verify_clients.sh`)

## Integration Matrix

### IT-01: eloqstore strict startup + MCP smoke

Command:

```bash
scripts/verify.sh strict --pgmemd-bin ./build/pgmemd --with-smoke
```

Assertions:
- `/health` is reachable
- `memory.stats.result.effective_backend == "eloqstore"`
- `memory.stats.result.write_ack_mode == "accepted"`
- `memory.describe` and `GET /mcp/describe` expose complete API contract
- daemon log does not contain `auto-downgrading backend to in-memory`
- smoke checks pass (`commit/search/stats`, invalid JSON `400`, `/sync/*` `404`)

### IT-02: eloqstore accepted stress drain

Command:

```bash
scripts/verify.sh strict --pgmemd-bin ./build/pgmemd --with-stress
```

Assertions:
- strict backend remains `eloqstore`
- stress writes complete
- `pending_write_ops` drains to `0`

### IT-03: inmemory accepted shutdown drain

Command:

```bash
scripts/verify.sh shutdown --pgmemd-bin ./build/pgmemd --store-backend inmemory --count 500
```

Assertions:
- write load accepted
- SIGTERM exits within timeout
- process does not hang

### IT-04: forced io_uring unavailable downgrade

Command:

```bash
scripts/verify.sh downgrade --pgmemd-bin ./build/pgmemd
```

Assertions:
- startup log contains downgrade warning
- `memory.stats.result.effective_backend == "inmemory"`
- service remains healthy

## Client Availability Gates

### CI-CL-01: mock Codex/Claude strict deploy flow

Command:

```bash
scripts/deploy_mcp.sh \
  --workspace . \
  --url http://127.0.0.1:8765/mcp \
  --strict-runtime false \
  --strict-codex true \
  --strict-claude true \
  --auto-install-cli false \
  --force-login true
```

Assertions:
- deploy script executes login/register/retry branches using mock CLIs
- strict Codex and Claude checks pass
- generated `.cursor/mcp.json` and `.mcp.json` are readable and valid

### CI-CL-02: verify_clients mock gate

Command:

```bash
scripts/verify_clients.sh --workspace . --url http://127.0.0.1:8765/mcp --real --strict-runtime false
```

Assertions:
- Cursor config contract passes
- Claude config contract passes
- Codex/Claude registered server `poorguy-mem` is discoverable (`mcp get/list`)

## Failure Triage

- Build or dependency issues:
  - rerun `scripts/install_deps.sh --verify-only`
  - inspect CMake and linker output
- Unit failures:
  - rerun `ctest --test-dir build --output-on-failure`
- Integration failures:
  - inspect `.pgmem/*.log`
  - inspect `build/Testing/**`
  - rerun failed script locally with same args
