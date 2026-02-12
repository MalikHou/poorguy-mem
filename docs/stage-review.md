# Poorguy-Mem Stage Review

Verification baseline refreshed on **2026-02-11**.

## Current Status

All previously marked local bring-up items are closed in this environment:

- dependency verification
- eloqstore Release build and startup verification
- unit tests (`ctest`)
- MCP smoke path
- shutdown drain verification
- forced io_uring-unavailable downgrade verification
- accepted-mode stress verification

## Local Minimal-Mode Decision (Applied)

Runtime and user-facing operation were simplified to local single-node usage:

- only `eloqstore` / `inmemory` backends are exposed
- `node-id` removed from CLI (internal local identity remains for LWW)
- `store-partitions` removed from CLI; internally `partitions = resolved_core_number`
- `allow-inmemory-fallback` removed from CLI; eloqstore init failure auto-downgrades
- `write_ack_mode` fixed to `accepted`
- `deploy_mcp.sh` defaults to strict runtime handshake + strict Codex + strict Claude checks, with auto-install/login/retry
- `install.sh` defaults to strict client-availability gate and runs `verify_clients.sh --real` before success exit

## Residual Risks

- host kernel/runtime capability drift (especially io_uring and dependency mirrors)
- CI environment package source instability
- performance characteristics still depend on host storage and CPU
- third-party CLI auth/session expiration can require re-login and may block strict install in unattended shells

## Validation Entry

See `docs/test-matrix.md` for PR-required checks and local replay commands.
