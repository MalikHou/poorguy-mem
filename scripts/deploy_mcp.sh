#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE="${ROOT}"
MCP_URL="http://127.0.0.1:8765/mcp"
MCP_NAME="poorguy-mem"
STRICT_CODEX="true"
STRICT_CLAUDE="true"
STRICT_RUNTIME="true"
AUTO_INSTALL_CLI="true"
FORCE_LOGIN="true"
MAX_RETRIES="3"
CLAUDE_SCOPE="project"

usage() {
  cat <<USAGE
Usage:
  scripts/deploy_mcp.sh [options]

Options:
  --workspace <path>   Workspace path (default: current repo)
  --url <mcp-url>      MCP URL (default: http://127.0.0.1:8765/mcp)
  --name <mcp-name>    MCP server name (default: poorguy-mem)
  --strict-codex <bool>
                      Whether codex registration/verification must succeed (default: true)
  --strict-claude <bool>
                      Whether claude registration/verification must succeed (default: true)
  --strict-runtime <bool>
                      Whether MCP runtime handshake must pass (default: true)
  --auto-install-cli <bool>
                      Whether to auto-install missing codex/claude CLIs (default: true)
  --force-login <bool>
                      Whether to force interactive login when needed (default: true)
  --max-retries <N>    Max retries for install/register operations (default: 3)
  --claude-scope <project|user|local>
                      Scope passed to claude mcp add (default: project)
  --help, -h           Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --workspace)
      WORKSPACE="$2"
      shift 2
      ;;
    --url)
      MCP_URL="$2"
      shift 2
      ;;
    --name)
      MCP_NAME="$2"
      shift 2
      ;;
    --strict-codex)
      STRICT_CODEX="$2"
      shift 2
      ;;
    --strict-claude)
      STRICT_CLAUDE="$2"
      shift 2
      ;;
    --strict-runtime)
      STRICT_RUNTIME="$2"
      shift 2
      ;;
    --auto-install-cli)
      AUTO_INSTALL_CLI="$2"
      shift 2
      ;;
    --force-login)
      FORCE_LOGIN="$2"
      shift 2
      ;;
    --max-retries)
      MAX_RETRIES="$2"
      shift 2
      ;;
    --claude-scope)
      CLAUDE_SCOPE="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

normalize_bool() {
  echo "$1" | tr '[:upper:]' '[:lower:]'
}

is_true() {
  [[ "$1" == "true" ]]
}

to_int() {
  local v="$1"
  if [[ ! "$v" =~ ^[0-9]+$ ]]; then
    return 1
  fi
  echo "$v"
}

case "$(normalize_bool "${STRICT_CODEX}")" in
  1|true|yes|on) STRICT_CODEX="true" ;;
  0|false|no|off) STRICT_CODEX="false" ;;
  *) echo "Invalid --strict-codex: ${STRICT_CODEX}" >&2; exit 1 ;;
esac

case "$(normalize_bool "${STRICT_CLAUDE}")" in
  1|true|yes|on) STRICT_CLAUDE="true" ;;
  0|false|no|off) STRICT_CLAUDE="false" ;;
  *) echo "Invalid --strict-claude: ${STRICT_CLAUDE}" >&2; exit 1 ;;
esac

case "$(normalize_bool "${STRICT_RUNTIME}")" in
  1|true|yes|on) STRICT_RUNTIME="true" ;;
  0|false|no|off) STRICT_RUNTIME="false" ;;
  *) echo "Invalid --strict-runtime: ${STRICT_RUNTIME}" >&2; exit 1 ;;
esac

case "$(normalize_bool "${AUTO_INSTALL_CLI}")" in
  1|true|yes|on) AUTO_INSTALL_CLI="true" ;;
  0|false|no|off) AUTO_INSTALL_CLI="false" ;;
  *) echo "Invalid --auto-install-cli: ${AUTO_INSTALL_CLI}" >&2; exit 1 ;;
esac

case "$(normalize_bool "${FORCE_LOGIN}")" in
  1|true|yes|on) FORCE_LOGIN="true" ;;
  0|false|no|off) FORCE_LOGIN="false" ;;
  *) echo "Invalid --force-login: ${FORCE_LOGIN}" >&2; exit 1 ;;
esac

MAX_RETRIES="$(to_int "${MAX_RETRIES}")" || {
  echo "Invalid --max-retries: ${MAX_RETRIES}" >&2
  exit 1
}
if [[ "${MAX_RETRIES}" -lt 1 ]]; then
  echo "--max-retries must be >= 1" >&2
  exit 1
fi

case "${CLAUDE_SCOPE}" in
  project|user|local) ;;
  *) echo "Invalid --claude-scope: ${CLAUDE_SCOPE}" >&2; exit 1 ;;
esac

runtime_base_url() {
  local url="$1"
  if [[ "$url" == */mcp ]]; then
    echo "${url%/mcp}"
  else
    echo "$url"
  fi
}

MCP_BASE_URL="$(runtime_base_url "${MCP_URL}")"
MCP_ENDPOINT="${MCP_BASE_URL}/mcp"
HEALTH_ENDPOINT="${MCP_BASE_URL}/health"

fail_or_warn() {
  local strict="$1"
  local message="$2"
  if is_true "$strict"; then
    echo "[deploy_mcp] error: ${message}" >&2
    exit 1
  fi
  echo "[deploy_mcp] warning: ${message}" >&2
}

run_with_retries() {
  local label="$1"
  shift
  local attempt=1
  local rc=0
  while (( attempt <= MAX_RETRIES )); do
    if "$@"; then
      return 0
    else
      rc=$?
    fi
    echo "[deploy_mcp] ${label} failed (attempt ${attempt}/${MAX_RETRIES}, rc=${rc})" >&2
    if (( attempt == MAX_RETRIES )); then
      break
    fi
    sleep 1
    attempt=$((attempt + 1))
  done
  return "$rc"
}

as_root() {
  if [[ "$(id -u)" -eq 0 ]]; then
    "$@"
    return
  fi
  if command -v sudo >/dev/null 2>&1; then
    sudo "$@"
    return
  fi
  echo "[deploy_mcp] sudo is required for this operation but is not available" >&2
  return 1
}

npm_global_install() {
  local package="$1"
  if npm install -g "$package"; then
    return 0
  fi
  if command -v sudo >/dev/null 2>&1; then
    sudo npm install -g "$package"
    return $?
  fi

  if [[ -n "${HOME:-}" ]]; then
    mkdir -p "${HOME}/.local"
    if npm install -g --prefix "${HOME}/.local" "$package"; then
      export PATH="${HOME}/.local/bin:${PATH}"
      return 0
    fi
  fi
  return 1
}

ensure_node_npm() {
  if command -v node >/dev/null 2>&1 && command -v npm >/dev/null 2>&1; then
    return 0
  fi

  if ! is_true "${AUTO_INSTALL_CLI}"; then
    echo "[deploy_mcp] node/npm missing and --auto-install-cli=false" >&2
    return 1
  fi

  echo "[deploy_mcp] node/npm not found, installing via apt"
  run_with_retries "apt-get update" as_root apt-get update
  run_with_retries "apt-get install nodejs npm" as_root apt-get install -y nodejs npm

  command -v node >/dev/null 2>&1 && command -v npm >/dev/null 2>&1
}

refresh_path_from_npm() {
  local npm_bin=""
  npm_bin="$(npm prefix -g 2>/dev/null || true)"
  if [[ -n "${npm_bin}" && -d "${npm_bin}/bin" ]]; then
    export PATH="${npm_bin}/bin:${PATH}"
  fi
  hash -r
}

ensure_codex_cli() {
  if command -v codex >/dev/null 2>&1; then
    return 0
  fi

  if ! is_true "${AUTO_INSTALL_CLI}"; then
    echo "[deploy_mcp] codex CLI missing and --auto-install-cli=false" >&2
    return 1
  fi

  ensure_node_npm || return 1
  echo "[deploy_mcp] installing codex CLI (@openai/codex)"
  run_with_retries "install codex CLI" npm_global_install @openai/codex || return 1
  refresh_path_from_npm
  command -v codex >/dev/null 2>&1
}

ensure_claude_cli() {
  if command -v claude >/dev/null 2>&1; then
    return 0
  fi

  if ! is_true "${AUTO_INSTALL_CLI}"; then
    echo "[deploy_mcp] claude CLI missing and --auto-install-cli=false" >&2
    return 1
  fi

  ensure_node_npm || return 1
  echo "[deploy_mcp] installing claude CLI (@anthropic-ai/claude-code)"
  run_with_retries "install claude CLI" npm_global_install @anthropic-ai/claude-code || return 1
  refresh_path_from_npm
  command -v claude >/dev/null 2>&1
}

check_runtime_handshake() {
  curl -fsS "${HEALTH_ENDPOINT}" >/tmp/pgmem-health-check.json || return 1

  local init_json
  init_json="$(curl -fsS "${MCP_ENDPOINT}" -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"clientInfo":{"name":"deploy_mcp","version":"1"}}}')" || return 1
  local tools_json
  tools_json="$(curl -fsS "${MCP_ENDPOINT}" -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}')" || return 1
  local call_json
  call_json="$(curl -fsS "${MCP_ENDPOINT}" -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"memory.stats","arguments":{"workspace_id":"deploy-check","window":"5m"}}}')" || return 1
  local unknown_json
  unknown_json="$(curl -fsS "${MCP_ENDPOINT}" -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":4,"method":"unknown.method","params":{}}')" || return 1

  python3 - "$init_json" "$tools_json" "$call_json" "$unknown_json" <<'PY'
import json
import sys

init = json.loads(sys.argv[1])
tools = json.loads(sys.argv[2])
call = json.loads(sys.argv[3])
unknown = json.loads(sys.argv[4])

if init.get("jsonrpc") != "2.0" or not isinstance(init.get("result"), dict):
    raise SystemExit("initialize response invalid")

items = tools.get("result", {}).get("tools", [])
if not any(t.get("name") == "memory.search" for t in items):
    raise SystemExit("tools/list missing memory.search")
if not any(t.get("name") == "memory.describe" for t in items):
    raise SystemExit("tools/list missing memory.describe")

if call.get("jsonrpc") != "2.0":
    raise SystemExit("tools/call jsonrpc missing")
result = call.get("result", {})
if result.get("isError") is True:
    raise SystemExit("tools/call returned isError=true")
if not result.get("content"):
    raise SystemExit("tools/call returned empty content")

err = unknown.get("error", {})
if not isinstance(err.get("code"), int):
    raise SystemExit("unknown method error.code must be number")
if err.get("code") != -32601:
    raise SystemExit("unknown method must return -32601")
PY
}

codex_logged_in() {
  codex login status >/dev/null 2>&1
}

ensure_codex_login() {
  if codex_logged_in; then
    return 0
  fi

  if ! is_true "${FORCE_LOGIN}"; then
    echo "[deploy_mcp] codex not logged in and --force-login=false" >&2
    return 1
  fi

  echo "[deploy_mcp] codex login required, launching interactive login"
  local attempt=1
  while (( attempt <= MAX_RETRIES )); do
    if codex login && codex_logged_in; then
      return 0
    fi
    echo "[deploy_mcp] codex login attempt ${attempt}/${MAX_RETRIES} failed" >&2
    if (( attempt == MAX_RETRIES )); then
      break
    fi
    attempt=$((attempt + 1))
  done
  return 1
}

claude_session_ready() {
  # Try lightweight checks in order; if any succeeds, treat as ready.
  if claude login status >/dev/null 2>&1; then
    return 0
  fi
  if claude auth status >/dev/null 2>&1; then
    return 0
  fi
  if claude mcp list >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

ensure_claude_login() {
  if claude_session_ready; then
    return 0
  fi

  if ! is_true "${FORCE_LOGIN}"; then
    echo "[deploy_mcp] claude not logged in and --force-login=false" >&2
    return 1
  fi

  echo "[deploy_mcp] claude login required, launching interactive login"
  local attempt=1
  while (( attempt <= MAX_RETRIES )); do
    if claude login >/dev/null 2>&1; then
      :
    else
      echo "[deploy_mcp] falling back to interactive claude shell login (/login then exit)"
      claude || true
    fi
    if claude_session_ready; then
      return 0
    fi
    echo "[deploy_mcp] claude login attempt ${attempt}/${MAX_RETRIES} failed" >&2
    if (( attempt == MAX_RETRIES )); then
      break
    fi
    attempt=$((attempt + 1))
  done
  return 1
}

register_codex() {
  if codex mcp get "${MCP_NAME}" >/dev/null 2>&1; then
    echo "[deploy_mcp] codex mcp already exists: ${MCP_NAME}"
    return 0
  fi

  run_with_retries "codex mcp add" codex mcp add "${MCP_NAME}" --url "${MCP_URL}"
}

verify_codex_registration() {
  codex mcp get "${MCP_NAME}" >/dev/null 2>&1 || return 1
  codex mcp list | grep -q "${MCP_NAME}" || return 1
}

register_claude() {
  if claude mcp get "${MCP_NAME}" >/dev/null 2>&1; then
    echo "[deploy_mcp] claude mcp already exists: ${MCP_NAME}"
    return 0
  fi

  run_with_retries "claude mcp add" \
    bash -lc "claude mcp add --scope '${CLAUDE_SCOPE}' --transport http '${MCP_NAME}' '${MCP_URL}' >/dev/null 2>&1 || claude mcp add '${MCP_NAME}' '${MCP_URL}' --transport http --scope '${CLAUDE_SCOPE}' >/dev/null 2>&1"
}

verify_claude_registration() {
  claude mcp get "${MCP_NAME}" >/dev/null 2>&1 || return 1
  claude mcp list | grep -q "${MCP_NAME}" || return 1
}

validate_workspace_files() {
  local cursor_mcp_path="${WORKSPACE}/.cursor/mcp.json"
  local cursor_rule_path="${WORKSPACE}/.cursor/rules/poorguy-mem.mdc"
  local claude_mcp_path="${WORKSPACE}/.mcp.json"

  mkdir -p "$(dirname "${cursor_mcp_path}")" "$(dirname "${cursor_rule_path}")"

  python3 - <<'PY' "${cursor_mcp_path}" "${MCP_NAME}" "${MCP_URL}"
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
name = sys.argv[2]
url = sys.argv[3]

root = {}
if path.exists():
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        root = {}

servers = root.get("mcpServers")
if not isinstance(servers, dict):
    servers = {}
entry = servers.get(name)
if not isinstance(entry, dict):
    entry = {}
entry["url"] = url
entry["description"] = (
    "Local memory server for this workspace. "
    "Use memory.describe/bootstrap/search/commit_turn/pin/stats/compact."
)
servers[name] = entry
root["mcpServers"] = servers
path.write_text(json.dumps(root, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
PY

  python3 - <<'PY' "${claude_mcp_path}" "${MCP_NAME}" "${MCP_URL}"
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
name = sys.argv[2]
url = sys.argv[3]

root = {}
if path.exists():
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        root = {}

servers = root.get("mcpServers")
if not isinstance(servers, dict):
    servers = {}

entry = servers.get(name)
if not isinstance(entry, dict):
    entry = {}
entry["type"] = "http"
entry["url"] = url
entry["description"] = (
    "Local memory server for this workspace. "
    "Use memory.describe/bootstrap/search/commit_turn/pin/stats/compact."
)
servers[name] = entry
root["mcpServers"] = servers
path.write_text(json.dumps(root, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
PY

  cat > "${cursor_rule_path}" <<'RULE'
---
description: poorguy-mem memory server contract
alwaysApply: true
---
`poorguy-mem` is the workspace memory MCP server.

Execution contract:
1. If schema is unknown, call `memory.describe` first and follow its contract.
2. At task/session start, call `memory.bootstrap` with current `workspace_id` and `session_id`.
3. Before asking for repeated context, call `memory.search` to recall prior facts.
4. After each meaningful assistant turn, call `memory.commit_turn` with:
   `workspace_id`, `session_id`, `user_text`, `assistant_text`, plus useful `code_snippets` and `commands`.
5. If user marks information as important, call `memory.pin`.
6. For diagnostics only, call `memory.stats` or `memory.compact`.

Protocol constraints:
- Use only `POST /mcp` with `memory.*` methods.
- `/sync/push` and `/sync/pull` are removed and must be treated as unavailable.
RULE

  python3 - <<'PY' "${cursor_mcp_path}" "${claude_mcp_path}" "${MCP_NAME}" "${MCP_URL}"
import json
import pathlib
import sys

cursor_path = pathlib.Path(sys.argv[1])
claude_path = pathlib.Path(sys.argv[2])
name = sys.argv[3]
url = sys.argv[4]

cursor = json.loads(cursor_path.read_text(encoding="utf-8"))
claude = json.loads(claude_path.read_text(encoding="utf-8"))

if cursor.get("mcpServers", {}).get(name, {}).get("url") != url:
    raise SystemExit("cursor mcp url mismatch")

entry = claude.get("mcpServers", {}).get(name, {})
if entry.get("type") != "http" or entry.get("url") != url:
    raise SystemExit("claude mcp config mismatch")
PY

  echo "[deploy_mcp] updated ${cursor_mcp_path}"
  echo "[deploy_mcp] updated ${cursor_rule_path}"
  echo "[deploy_mcp] updated ${claude_mcp_path}"
}

main() {
  validate_workspace_files

  if is_true "${STRICT_RUNTIME}"; then
    run_with_retries "MCP runtime handshake" check_runtime_handshake || {
      fail_or_warn "${STRICT_RUNTIME}" "MCP runtime handshake failed on ${MCP_ENDPOINT}. Start pgmemd and retry."
      return
    }
  fi

  if ! ensure_codex_cli; then
    fail_or_warn "${STRICT_CODEX}" "codex CLI is unavailable"
  fi
  if command -v codex >/dev/null 2>&1; then
    if ! codex --version >/dev/null 2>&1; then
      fail_or_warn "${STRICT_CODEX}" "codex --version failed"
    fi
    if ! ensure_codex_login; then
      fail_or_warn "${STRICT_CODEX}" "codex login failed"
    elif ! register_codex; then
      fail_or_warn "${STRICT_CODEX}" "failed to register codex MCP server ${MCP_NAME}"
    elif ! verify_codex_registration; then
      fail_or_warn "${STRICT_CODEX}" "codex MCP verification failed for ${MCP_NAME}"
    else
      echo "[deploy_mcp] codex mcp verify: ok (${MCP_NAME})"
    fi
  fi

  if ! ensure_claude_cli; then
    fail_or_warn "${STRICT_CLAUDE}" "claude CLI is unavailable"
  fi
  if command -v claude >/dev/null 2>&1; then
    if ! claude --version >/dev/null 2>&1; then
      fail_or_warn "${STRICT_CLAUDE}" "claude --version failed"
    fi
    if ! ensure_claude_login; then
      fail_or_warn "${STRICT_CLAUDE}" "claude login failed"
    elif ! register_claude; then
      fail_or_warn "${STRICT_CLAUDE}" "failed to register claude MCP server ${MCP_NAME}"
    elif ! verify_claude_registration; then
      fail_or_warn "${STRICT_CLAUDE}" "claude MCP verification failed for ${MCP_NAME}"
    else
      echo "[deploy_mcp] claude mcp verify: ok (${MCP_NAME})"
    fi
  fi

  if is_true "${STRICT_RUNTIME}"; then
    run_with_retries "final MCP runtime handshake" check_runtime_handshake || {
      fail_or_warn "${STRICT_RUNTIME}" "final MCP runtime handshake failed on ${MCP_ENDPOINT}"
      return
    }
  fi

  echo "[deploy_mcp] done"
}

main
