# poorguy-mem

Local C++17 MCP memory server for coding assistants.

## Scope

- Backends: `eloqstore`, `inmemory`
- Write mode: `accepted`
- Endpoints: `GET /health`, `POST /mcp`, `GET /mcp/describe`
- Methods:
  - `memory.describe`
  - `memory.bootstrap`
  - `memory.commit_turn`
  - `memory.search`
  - `memory.pin`
  - `memory.stats`
  - `memory.compact`

## Quick Start

```bash
scripts/install.sh
scripts/start.sh
scripts/deploy_mcp.sh --workspace .
```

## Daily Commands

```bash
# dependencies only
scripts/install_deps.sh

# dependency probe only
scripts/install_deps.sh --verify-only

# format (uses .clang-format, skips third_party)
scripts/format.sh --check
scripts/format.sh --write

# integrated verification
scripts/verify.sh all --pgmemd-bin ./build/pgmemd

# cleanup local artifacts
scripts/clean.sh
scripts/clean.sh --all
```

Notes:
- `scripts/install_deps.sh` installs `clang-format-18` by default and requires `clang-format 18.1.3`.
- `scripts/deploy_mcp.sh` writes:
  - `.cursor/mcp.json`
  - `.cursor/rules/poorguy-mem.mdc`
  - `.mcp.json`

## Parameter Reference

`scripts/install_deps.sh`
- `--verify-only`: probe dependencies only; do not install.

`scripts/install.sh`
- `--backend <eloqstore|inmemory>`: build backend preset.
- `--build-type <type>`: CMake build type (usually `Release`).
- `--workspace <path>`: target workspace for MCP config files.
- `--url <mcp-url>`: MCP endpoint used for deployment and checks.
- `--name <mcp-name>`: MCP server registration name.
- `--core-number <N>`: runtime core count written into install config.
- `--auto-install-cli <bool>`: auto-install missing `node/codex/claude`.
- `--force-login <bool>`: force interactive login for Codex/Claude if needed.
- `--max-retries <N>`: retry count for install/login/register.
- `--skip-deps`: skip dependency install stage.
- `--skip-tests`: skip `ctest` stage.
- `--skip-deploy`: skip MCP deploy stage.
- `--allow-codex-miss`: do not fail when Codex MCP registration/check fails.
- `--allow-claude-miss`: do not fail when Claude MCP registration/check fails.
- `--allow-runtime-miss`: do not fail when MCP runtime handshake fails.
- `--allow-mcp-miss`: equivalent to enabling both codex/claude miss flags.

`scripts/start.sh`
- `--backend <eloqstore|inmemory>`: runtime backend request.
- `--host <ip>`: bind host.
- `--port <N>`: bind port.
- `--store-root <path>`: local data root.
- `--core-number <N>`: runtime core count.
- `--enable-io-uring-network-engine <bool>`: brpc network io_uring toggle.
- `--smoke`: run smoke test and exit.

`scripts/deploy_mcp.sh`
- `--workspace <path>`: workspace root where Cursor/Claude config is written.
- `--url <mcp-url>`: MCP server URL.
- `--name <mcp-name>`: MCP server name in client configs.
- `--strict-codex <bool>`: fail when Codex registration/check fails.
- `--strict-claude <bool>`: fail when Claude registration/check fails.
- `--strict-runtime <bool>`: fail when runtime MCP handshake fails.
- `--auto-install-cli <bool>`: install missing Codex/Claude CLIs automatically.
- `--force-login <bool>`: trigger interactive login when session missing.
- `--max-retries <N>`: retry count for install/register/login flows.
- `--claude-scope <project|user|local>`: scope passed to `claude mcp add`.

`scripts/format.sh`
- `--check`: check-only mode; non-zero if formatting is needed.
- `--write`: in-place format mode (default).
- `--list-files`: print matched files only.
- `--clang-format-bin <path>`: clang-format binary path (must be `18.1.3`).
- `[paths...]`: optional target paths; default is `include src tests proto`.

`scripts/clean.sh`
- `--runtime-only`: clean runtime artifacts only (default).
- `--build`: include build artifacts.
- `--mcp-config`: include local MCP client config files.
- `--all`: clean runtime + build.
- `--dry-run`: show deletion targets only.

## Minimal Manual MCP Test

Start service:

```bash
scripts/start.sh --backend eloqstore
```

Write one memory:

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -d '{
    "id":"1",
    "method":"memory.commit_turn",
    "params":{
      "workspace_id":"demo",
      "session_id":"s1",
      "user_text":"remember: release window is 20:00",
      "assistant_text":"recorded"
    }
  }'
```

Search:

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -d '{
    "id":"2",
    "method":"memory.search",
    "params":{
      "workspace_id":"demo",
      "query":"release window",
      "top_k":3
    }
  }'
```

Stats:

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -d '{
    "id":"3",
    "method":"memory.stats",
    "params":{"workspace_id":"demo","window":"5m"}
  }'
```

## Shared Deployment (Multi-user)

One `pgmemd` instance can be shared by multiple users.

- Use one endpoint, e.g. `http://<server-ip>:8765/mcp`
- Isolate data by stable `workspace_id` per user/team
- Use `session_id` per conversation/task
- Put TLS/auth at gateway layer (nginx/caddy/api-gateway)

Current boundary:
- No built-in auth in `pgmemd`
- Isolation is logical (`workspace_id`), not OS-level tenant isolation
