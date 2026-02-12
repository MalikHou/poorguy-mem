#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE="${ROOT}"
MCP_URL="http://127.0.0.1:8765/mcp"
MCP_NAME="poorguy-mem"
REAL=0
STRICT_RUNTIME="true"
STRICT_CODEX="true"
STRICT_CLAUDE="true"

usage() {
  cat <<USAGE
Usage:
  scripts/verify_clients.sh [options]

Options:
  --workspace <path>      Workspace path (default: repo root)
  --url <mcp-url>         MCP URL (default: http://127.0.0.1:8765/mcp)
  --name <mcp-name>       MCP server name (default: poorguy-mem)
  --real                  Enable real Codex/Claude CLI checks
  --strict-runtime <bool> Require MCP runtime handshake (default: true)
  --strict-codex <bool>   Require Codex checks to pass (default: true)
  --strict-claude <bool>  Require Claude checks to pass (default: true)
  --help, -h              Show this help
USAGE
}

normalize_bool() {
  echo "$1" | tr '[:upper:]' '[:lower:]'
}

parse_bool() {
  case "$(normalize_bool "$1")" in
    1|true|yes|on) echo "true" ;;
    0|false|no|off) echo "false" ;;
    *)
      echo "invalid-bool"
      ;;
  esac
}

runtime_base_url() {
  local url="$1"
  if [[ "$url" == */mcp ]]; then
    echo "${url%/mcp}"
  else
    echo "$url"
  fi
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
    --real)
      REAL=1
      shift
      ;;
    --strict-runtime)
      STRICT_RUNTIME="$(parse_bool "$2")"
      shift 2
      ;;
    --strict-codex)
      STRICT_CODEX="$(parse_bool "$2")"
      shift 2
      ;;
    --strict-claude)
      STRICT_CLAUDE="$(parse_bool "$2")"
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

if [[ "${STRICT_RUNTIME}" == "invalid-bool" ]]; then
  echo "Invalid --strict-runtime value" >&2
  exit 1
fi
if [[ "${STRICT_CODEX}" == "invalid-bool" ]]; then
  echo "Invalid --strict-codex value" >&2
  exit 1
fi
if [[ "${STRICT_CLAUDE}" == "invalid-bool" ]]; then
  echo "Invalid --strict-claude value" >&2
  exit 1
fi

MCP_BASE_URL="$(runtime_base_url "${MCP_URL}")"
MCP_ENDPOINT="${MCP_BASE_URL}/mcp"
HEALTH_ENDPOINT="${MCP_BASE_URL}/health"

declare -a FAILURES=()

strict_or_warn() {
  local strict="$1"
  local message="$2"
  if [[ "${strict}" == "true" ]]; then
    FAILURES+=("${message}")
  else
    echo "[verify_clients] warning: ${message}" >&2
  fi
}

check_cursor_config() {
  local path="${WORKSPACE}/.cursor/mcp.json"
  python3 - "${path}" "${MCP_NAME}" "${MCP_URL}" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
name = sys.argv[2]
url = sys.argv[3]
if not path.exists():
    raise SystemExit(f"missing Cursor MCP config: {path}")
doc = json.loads(path.read_text(encoding="utf-8"))
servers = doc.get("mcpServers", {})
if servers.get(name, {}).get("url") != url:
    raise SystemExit(f"Cursor MCP URL mismatch for {name}")
PY
}

check_claude_config() {
  local path="${WORKSPACE}/.mcp.json"
  python3 - "${path}" "${MCP_NAME}" "${MCP_URL}" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
name = sys.argv[2]
url = sys.argv[3]
if not path.exists():
    raise SystemExit(f"missing Claude MCP config: {path}")
doc = json.loads(path.read_text(encoding="utf-8"))
entry = doc.get("mcpServers", {}).get(name, {})
if entry.get("type") != "http":
    raise SystemExit(f"Claude MCP type mismatch for {name}")
if entry.get("url") != url:
    raise SystemExit(f"Claude MCP URL mismatch for {name}")
PY
}

check_runtime_handshake() {
  local init_json
  local tools_json
  local call_json
  local unknown_json

  curl -fsS "${HEALTH_ENDPOINT}" >/tmp/pgmem-verify-health.json || return 1

  init_json="$(curl -fsS "${MCP_ENDPOINT}" -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"clientInfo":{"name":"verify_clients","version":"1"}}}')" || return 1
  tools_json="$(curl -fsS "${MCP_ENDPOINT}" -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}')" || return 1
  call_json="$(curl -fsS "${MCP_ENDPOINT}" -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"memory.stats","arguments":{"workspace_id":"verify-clients","window":"5m"}}}')" || return 1
  unknown_json="$(curl -fsS "${MCP_ENDPOINT}" -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":4,"method":"unknown.method","params":{}}')" || return 1

  python3 - "$init_json" "$tools_json" "$call_json" "$unknown_json" <<'PY'
import json
import sys

init = json.loads(sys.argv[1])
tools = json.loads(sys.argv[2])
call = json.loads(sys.argv[3])
unknown = json.loads(sys.argv[4])

if init.get("jsonrpc") != "2.0" or not isinstance(init.get("result"), dict):
    raise SystemExit("initialize invalid")

tool_items = tools.get("result", {}).get("tools", [])
if not any(x.get("name") == "memory.search" for x in tool_items):
    raise SystemExit("tools/list missing memory.search")
if not any(x.get("name") == "memory.describe" for x in tool_items):
    raise SystemExit("tools/list missing memory.describe")

if call.get("jsonrpc") != "2.0":
    raise SystemExit("tools/call missing jsonrpc=2.0")
call_result = call.get("result", {})
if call_result.get("isError") is True:
    raise SystemExit("tools/call isError=true")
if not call_result.get("content"):
    raise SystemExit("tools/call content empty")

err = unknown.get("error", {})
if not isinstance(err.get("code"), int):
    raise SystemExit("unknown method error.code must be number")
if err.get("code") != -32601:
    raise SystemExit("unknown method error.code must be -32601")
PY
}

check_codex() {
  if ! command -v codex >/dev/null 2>&1; then
    strict_or_warn "${STRICT_CODEX}" "codex CLI not found"
    return
  fi

  if [[ "${REAL}" -eq 0 ]]; then
    echo "[verify_clients] codex check skipped (use --real to enforce)" >&2
    return
  fi

  codex login status >/dev/null 2>&1 || strict_or_warn "${STRICT_CODEX}" "codex login status failed"
  codex mcp get "${MCP_NAME}" >/dev/null 2>&1 || strict_or_warn "${STRICT_CODEX}" "codex mcp get failed for ${MCP_NAME}"
  codex mcp list | grep -q "${MCP_NAME}" || strict_or_warn "${STRICT_CODEX}" "codex mcp list missing ${MCP_NAME}"
}

check_claude() {
  if ! command -v claude >/dev/null 2>&1; then
    strict_or_warn "${STRICT_CLAUDE}" "claude CLI not found"
    return
  fi

  if [[ "${REAL}" -eq 0 ]]; then
    echo "[verify_clients] claude check skipped (use --real to enforce)" >&2
    return
  fi

  if ! claude login status >/dev/null 2>&1 && ! claude auth status >/dev/null 2>&1; then
    strict_or_warn "${STRICT_CLAUDE}" "claude login/auth status failed"
  fi
  claude mcp get "${MCP_NAME}" >/dev/null 2>&1 || strict_or_warn "${STRICT_CLAUDE}" "claude mcp get failed for ${MCP_NAME}"
  claude mcp list | grep -q "${MCP_NAME}" || strict_or_warn "${STRICT_CLAUDE}" "claude mcp list missing ${MCP_NAME}"
}

if ! check_cursor_config; then
  FAILURES+=("Cursor config check failed (.cursor/mcp.json)")
fi

if ! check_claude_config; then
  FAILURES+=("Claude config check failed (.mcp.json)")
fi

if [[ "${STRICT_RUNTIME}" == "true" ]]; then
  if ! check_runtime_handshake; then
    FAILURES+=("Runtime handshake failed on ${MCP_ENDPOINT}")
  fi
fi

check_codex
check_claude

if [[ ${#FAILURES[@]} -gt 0 ]]; then
  echo "[verify_clients] failed with ${#FAILURES[@]} issue(s):" >&2
  for i in "${!FAILURES[@]}"; do
    echo "  $((i + 1)). ${FAILURES[$i]}" >&2
  done
  exit 1
fi

echo "[verify_clients] ok"
