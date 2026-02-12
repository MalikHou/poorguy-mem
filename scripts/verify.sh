#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<USAGE
Usage:
  scripts/verify.sh <scenario> [options]

Scenarios:
  smoke      MCP protocol smoke checks against a running service
  stress     accepted-mode stress checks against a running service
  strict     start pgmemd with eloqstore and verify strict backend behavior
  shutdown   verify SIGTERM shutdown drain behavior
  downgrade  force io_uring unavailable and verify auto downgrade
  all        run strict(smoke) + strict(stress) + shutdown + downgrade

Run scenario help:
  scripts/verify.sh <scenario> --help
USAGE
}

json_assert() {
  local file="$1"
  local expr="$2"
  local message="$3"
  python3 - "$file" "$expr" "$message" <<'PY'
import json
import sys

path, expr, message = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, 'r', encoding='utf-8') as f:
    data = json.load(f)

ns = {
    'data': data,
    'len': len,
    'int': int,
    'str': str,
    'isinstance': isinstance,
    'any': any,
    'all': all,
}
ok = bool(eval(expr, {'__builtins__': {}}, ns))
if not ok:
    print(message, file=sys.stderr)
    print(json.dumps(data, ensure_ascii=False, indent=2), file=sys.stderr)
    sys.exit(1)
PY
}

wait_for_health() {
  local host="$1"
  local port="$2"
  local wait_seconds="$3"
  local i
  for ((i=0; i<wait_seconds*10; ++i)); do
    if curl -fsS "http://${host}:${port}/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

wait_for_exit() {
  local pid="$1"
  local wait_seconds="$2"
  local i
  for ((i=0; i<wait_seconds*10; ++i)); do
    if ! kill -0 "$pid" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

run_smoke() {
  local base_url="http://127.0.0.1:8765"
  local workspace_id="demo"
  local session_id="s1"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --url)
        base_url="$2"
        shift 2
        ;;
      --workspace)
        workspace_id="$2"
        shift 2
        ;;
      --session)
        session_id="$2"
        shift 2
        ;;
      --help|-h)
        cat <<USAGE
Usage:
  scripts/verify.sh smoke [options]

Options:
  --url <base-url>       MCP base URL (default: http://127.0.0.1:8765)
  --workspace <id>       Workspace id used in smoke requests (default: demo)
  --session <id>         Session id used in smoke requests (default: s1)
USAGE
        return 0
        ;;
      *)
        echo "Unknown option for smoke: $1" >&2
        return 1
        ;;
    esac
  done

  local health_json="$(mktemp /tmp/pgmem-health.XXXXXX.json)"
  local commit_json="$(mktemp /tmp/pgmem-commit.XXXXXX.json)"
  local search_json="$(mktemp /tmp/pgmem-search.XXXXXX.json)"
  local stats_json="$(mktemp /tmp/pgmem-stats.XXXXXX.json)"
  local describe_json="$(mktemp /tmp/pgmem-describe.XXXXXX.json)"
  local describe_http_json="$(mktemp /tmp/pgmem-describe-http.XXXXXX.json)"
  local rpc_init_json="$(mktemp /tmp/pgmem-rpc-init.XXXXXX.json)"
  local rpc_tools_json="$(mktemp /tmp/pgmem-rpc-tools.XXXXXX.json)"
  local rpc_call_json="$(mktemp /tmp/pgmem-rpc-call.XXXXXX.json)"
  local bad_json="$(mktemp /tmp/pgmem-bad.XXXXXX.json)"
  local sync_push_json="$(mktemp /tmp/pgmem-sync-push.XXXXXX.json)"
  local sync_pull_json="$(mktemp /tmp/pgmem-sync-pull.XXXXXX.json)"

  curl -fsS "${base_url}/health" > "${health_json}"
  json_assert "${health_json}" "data.get('status') == 'ok'" "health check failed: status != ok"

  curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
    \"id\": 1,
    \"method\": \"memory.commit_turn\",
    \"params\": {
      \"workspace_id\": \"${workspace_id}\",
      \"session_id\": \"${session_id}\",
      \"user_text\": \"please remember retry strategy\",
      \"assistant_text\": \"using exponential backoff\",
      \"code_snippets\": [\"int retry = 5;\"],
      \"commands\": [\"ctest --output-on-failure\"]
    }
  }" > "${commit_json}"
  json_assert "${commit_json}" "len(data.get('result', {}).get('stored_ids', [])) > 0" "commit_turn failed: stored_ids is empty"
  json_assert "${commit_json}" "isinstance(data.get('result', {}).get('stored_ids', [None])[0], str) and len(data.get('result', {}).get('stored_ids', [''])[0]) > 0" "commit_turn failed: first stored_id is invalid"

  curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
    \"id\": 2,
    \"method\": \"memory.search\",
    \"params\": {
      \"workspace_id\": \"${workspace_id}\",
      \"query\": \"retry backoff\",
      \"top_k\": 3,
      \"token_budget\": 1200
    }
  }" > "${search_json}"
  json_assert "${search_json}" "len(data.get('result', {}).get('hits', [])) > 0" "search failed: hits is empty"
  json_assert "${search_json}" "isinstance(data.get('result', {}).get('hits', [])[0].get('memory_id', ''), str) and len(data.get('result', {}).get('hits', [])[0].get('memory_id', '')) > 0" "search failed: first hit has invalid memory_id"

  curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
    \"id\": 3,
    \"method\": \"memory.stats\",
    \"params\": {
      \"workspace_id\": \"${workspace_id}\",
      \"window\": \"5m\"
    }
  }" > "${stats_json}"
  json_assert "${stats_json}" "'write_ack_mode' in data.get('result', {})" "stats missing write_ack_mode"
  json_assert "${stats_json}" "'pending_write_ops' in data.get('result', {})" "stats missing pending_write_ops"
  json_assert "${stats_json}" "'flush_failures_total' in data.get('result', {})" "stats missing flush_failures_total"
  json_assert "${stats_json}" "'volatile_dropped_on_shutdown' in data.get('result', {})" "stats missing volatile_dropped_on_shutdown"
  json_assert "${stats_json}" "'effective_backend' in data.get('result', {})" "stats missing effective_backend"
  json_assert "${stats_json}" "'last_flush_error' in data.get('result', {})" "stats missing last_flush_error"
  json_assert "${stats_json}" "data.get('result', {}).get('write_ack_mode') == 'accepted'" "stats write_ack_mode must be accepted"
  json_assert "${stats_json}" "data.get('result', {}).get('effective_backend') in ('eloqstore', 'inmemory')" "stats effective_backend invalid"
  json_assert "${stats_json}" "int(str(data.get('result', {}).get('pending_write_ops', '0'))) >= 0" "stats pending_write_ops is not numeric"
  json_assert "${stats_json}" "int(str(data.get('result', {}).get('flush_failures_total', '0'))) >= 0" "stats flush_failures_total is not numeric"
  json_assert "${stats_json}" "int(str(data.get('result', {}).get('volatile_dropped_on_shutdown', '0'))) >= 0" "stats volatile_dropped_on_shutdown is not numeric"
  json_assert "${stats_json}" "isinstance(data.get('result', {}).get('last_flush_error', ''), str)" "stats last_flush_error should be string"

  curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 4,
    "method": "memory.describe",
    "params": {}
  }' > "${describe_json}"
  json_assert "${describe_json}" "data.get('result', {}).get('server', {}).get('kind') == 'memory-server'" "describe failed: server.kind mismatch"
  json_assert "${describe_json}" "'memory.commit_turn' in data.get('result', {}).get('method_names', [])" "describe failed: method_names missing memory.commit_turn"
  json_assert "${describe_json}" "data.get('result', {}).get('methods', {}).get('memory', {}).get('commit_turn', {}).get('input', {}).get('properties', {}).get('assistant_text', {}).get('type') == 'string'" "describe failed: commit_turn assistant_text type contract missing"
  json_assert "${describe_json}" "data.get('result', {}).get('methods', {}).get('memory', {}).get('stats', {}).get('output', {}).get('properties', {}).get('effective_backend', {}).get('type') == 'string'" "describe failed: stats effective_backend type contract missing"
  json_assert "${describe_json}" "int(str(data.get('result', {}).get('errors', {}).get('invalid_json_http', {}).get('status', '0'))) == 400" "describe failed: invalid_json_http status contract mismatch"

  curl -fsS "${base_url}/mcp/describe" > "${describe_http_json}"
  json_assert "${describe_http_json}" "data.get('server', {}).get('endpoint') == '/mcp'" "GET /mcp/describe failed: endpoint contract mismatch"
  json_assert "${describe_http_json}" "data.get('server', {}).get('describe_endpoint') == '/mcp/describe'" "GET /mcp/describe failed: describe endpoint mismatch"

  curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "jsonrpc": "2.0",
    "id": 101,
    "method": "initialize",
    "params": {"clientInfo": {"name": "verify", "version": "1"}}
  }' > "${rpc_init_json}"
  json_assert "${rpc_init_json}" "data.get('jsonrpc') == '2.0'" "jsonrpc initialize failed: missing jsonrpc=2.0"
  json_assert "${rpc_init_json}" "data.get('result', {}).get('capabilities', {}).get('tools') is not None" "jsonrpc initialize failed: tools capability missing"

  curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "jsonrpc": "2.0",
    "id": 102,
    "method": "tools/list",
    "params": {}
  }' > "${rpc_tools_json}"
  json_assert "${rpc_tools_json}" "data.get('jsonrpc') == '2.0'" "jsonrpc tools/list failed: missing jsonrpc=2.0"
  json_assert "${rpc_tools_json}" "any(t.get('name') == 'memory.search' for t in data.get('result', {}).get('tools', []))" "jsonrpc tools/list failed: memory.search not found"
  json_assert "${rpc_tools_json}" "any(t.get('name') == 'memory.describe' for t in data.get('result', {}).get('tools', []))" "jsonrpc tools/list failed: memory.describe not found"

  curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
    \"jsonrpc\": \"2.0\",
    \"id\": 103,
    \"method\": \"tools/call\",
    \"params\": {
      \"name\": \"memory.stats\",
      \"arguments\": {
        \"workspace_id\": \"${workspace_id}\",
        \"window\": \"5m\"
      }
    }
  }" > "${rpc_call_json}"
  json_assert "${rpc_call_json}" "data.get('jsonrpc') == '2.0'" "jsonrpc tools/call failed: missing jsonrpc=2.0"
  json_assert "${rpc_call_json}" "data.get('result', {}).get('isError') in (False, 'false')" "jsonrpc tools/call failed: isError should be false"
  json_assert "${rpc_call_json}" "len(data.get('result', {}).get('content', [])) > 0" "jsonrpc tools/call failed: empty content"

  local bad_status
  bad_status="$(curl -s -o "${bad_json}" -w "%{http_code}" \
    "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d '{bad-json')"
  if [[ "${bad_status}" != "400" ]]; then
    echo "invalid JSON request expected status 400, got ${bad_status}" >&2
    cat "${bad_json}" >&2
    return 1
  fi
  if ! grep -qi "invalid JSON" "${bad_json}"; then
    echo "invalid JSON response missing error message" >&2
    cat "${bad_json}" >&2
    return 1
  fi

  local sync_push_status
  sync_push_status="$(curl -s -o "${sync_push_json}" -w "%{http_code}" \
    "${base_url}/sync/push" -X POST -H 'Content-Type: application/json' -d '{}')"
  if [[ "${sync_push_status}" != "404" ]]; then
    echo "/sync/push expected 404, got ${sync_push_status}" >&2
    cat "${sync_push_json}" >&2
    return 1
  fi

  local sync_pull_status
  sync_pull_status="$(curl -s -o "${sync_pull_json}" -w "%{http_code}" \
    "${base_url}/sync/pull" -X POST -H 'Content-Type: application/json' -d '{}')"
  if [[ "${sync_pull_status}" != "404" ]]; then
    echo "/sync/pull expected 404, got ${sync_pull_status}" >&2
    cat "${sync_pull_json}" >&2
    return 1
  fi

  echo "Smoke test completed"
}

run_stress() {
  local base_url="http://127.0.0.1:8765"
  local workspace_id="stress-ws"
  local session_id="stress-s1"
  local iterations=10000
  local progress_step=1000
  local timeout_seconds=30
  local require_write_ack_mode="accepted"
  local require_effective_backend="any"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --url)
        base_url="$2"
        shift 2
        ;;
      --workspace)
        workspace_id="$2"
        shift 2
        ;;
      --session)
        session_id="$2"
        shift 2
        ;;
      --iterations)
        iterations="$2"
        shift 2
        ;;
      --progress-step)
        progress_step="$2"
        shift 2
        ;;
      --timeout-seconds)
        timeout_seconds="$2"
        shift 2
        ;;
      --require-write-ack-mode)
        require_write_ack_mode="$2"
        shift 2
        ;;
      --require-effective-backend)
        require_effective_backend="$2"
        shift 2
        ;;
      --help|-h)
        cat <<USAGE
Usage:
  scripts/verify.sh stress [options]

Options:
  --url <base-url>         MCP base URL (default: http://127.0.0.1:8765)
  --workspace <id>         Workspace id for stress writes (default: stress-ws)
  --session <id>           Session id for stress writes (default: stress-s1)
  --iterations <N>         Number of commit_turn requests (default: 10000)
  --progress-step <N>      Print progress every N ops (default: 1000)
  --timeout-seconds <N>    Max wait for pending_write_ops to drain (default: 30)
  --require-write-ack-mode <mode>
  --require-effective-backend <name|any>
USAGE
        return 0
        ;;
      *)
        echo "Unknown option for stress: $1" >&2
        return 1
        ;;
    esac
  done

  if [[ "${require_write_ack_mode}" != "accepted" ]]; then
    echo "Invalid --require-write-ack-mode: ${require_write_ack_mode}" >&2
    return 1
  fi

  curl -fsS "${base_url}/health" >/dev/null

  echo "[stress] start commit_turn load: iterations=${iterations}"
  local i
  for ((i=1; i<=iterations; ++i)); do
    curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
      \"id\": ${i},
      \"method\": \"memory.commit_turn\",
      \"params\": {
        \"workspace_id\": \"${workspace_id}\",
        \"session_id\": \"${session_id}\",
        \"user_text\": \"stress turn ${i}\",
        \"assistant_text\": \"accepted mode stress payload ${i}\",
        \"code_snippets\": [\"int n=${i};\"],
        \"commands\": [\"echo ${i}\"]
      }
    }" >/dev/null

    if (( i % progress_step == 0 )); then
      echo "[stress] progress: ${i}/${iterations}"
    fi
  done

  echo "[stress] writes submitted, waiting pending_write_ops to drain"
  local deadline=$((SECONDS + timeout_seconds))
  local last_stats=""
  while (( SECONDS < deadline )); do
    last_stats="$(curl -fsS "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
      \"id\": 999001,
      \"method\": \"memory.stats\",
      \"params\": {\"workspace_id\": \"${workspace_id}\", \"window\": \"5m\"}
    }")"

    local pending
    pending="$(python3 - <<'PY' "$last_stats"
import json,sys
obj=json.loads(sys.argv[1])
print(int(obj.get("result",{}).get("pending_write_ops",0)))
PY
)"

    if [[ "$pending" == "0" ]]; then
      break
    fi
    sleep 0.5
  done

  if [[ -z "${last_stats}" ]]; then
    echo "[stress] failed to query memory.stats" >&2
    return 1
  fi

  python3 - <<'PY' "$last_stats" "$require_write_ack_mode" "$require_effective_backend"
import json,sys
obj=json.loads(sys.argv[1])
required_mode=sys.argv[2]
required_backend=sys.argv[3]
result=obj.get("result",{})
pending=int(result.get("pending_write_ops",-1))
mode=result.get("write_ack_mode")
backend=result.get("effective_backend", "")
if mode != required_mode:
    raise SystemExit(f"write_ack_mode mismatch: expected {required_mode}, got {mode!r}")
if pending != 0:
    raise SystemExit(f"pending_write_ops not drained: {pending}")
if required_backend != "any" and backend != required_backend:
    raise SystemExit(f"effective_backend mismatch: expected {required_backend}, got {backend!r}")
print("[stress] final stats:", json.dumps({
    "write_ack_mode": mode,
    "pending_write_ops": pending,
    "flush_failures_total": result.get("flush_failures_total", 0),
    "volatile_dropped_on_shutdown": result.get("volatile_dropped_on_shutdown", 0),
    "effective_backend": backend
}, ensure_ascii=False))
PY

  echo "[stress] completed"
}

run_strict() {
  local pgmemd_bin="${ROOT}/build/pgmemd"
  local host="127.0.0.1"
  local port="18765"
  local wait_health_seconds=15
  local wait_exit_seconds=8
  local store_root="${ROOT}/.pgmem/eloqstore-strict"
  local log_file="${ROOT}/.pgmem/eloqstore-strict.log"
  local core_number="2"
  local with_smoke=0
  local with_stress=0
  local stress_iterations=2000
  local stress_timeout_seconds=30

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --pgmemd-bin)
        pgmemd_bin="$2"
        shift 2
        ;;
      --host)
        host="$2"
        shift 2
        ;;
      --port)
        port="$2"
        shift 2
        ;;
      --store-root)
        store_root="$2"
        shift 2
        ;;
      --core-number)
        core_number="$2"
        shift 2
        ;;
      --wait-health)
        wait_health_seconds="$2"
        shift 2
        ;;
      --with-smoke)
        with_smoke=1
        shift
        ;;
      --with-stress)
        with_stress=1
        shift
        ;;
      --stress-iterations)
        stress_iterations="$2"
        shift 2
        ;;
      --stress-timeout)
        stress_timeout_seconds="$2"
        shift 2
        ;;
      --help|-h)
        cat <<USAGE
Usage:
  scripts/verify.sh strict [options]

Options:
  --pgmemd-bin <path>         pgmemd binary path (default: ./build/pgmemd)
  --host <ip>                 Bind host (default: 127.0.0.1)
  --port <N>                  Bind port (default: 18765)
  --store-root <path>         Store root for this check
  --core-number <N>           Runtime core-number (default: 2)
  --wait-health <sec>         Max seconds to wait for /health (default: 15)
  --with-smoke                Run smoke checks
  --with-stress               Run stress checks
  --stress-iterations <N>     Stress iterations (default: 2000)
  --stress-timeout <sec>      Stress timeout seconds (default: 30)
USAGE
        return 0
        ;;
      *)
        echo "Unknown option for strict: $1" >&2
        return 1
        ;;
    esac
  done

  if [[ ! -x "${pgmemd_bin}" ]]; then
    echo "pgmemd not executable: ${pgmemd_bin}" >&2
    return 1
  fi

  mkdir -p "${ROOT}/.pgmem"
  rm -f "${log_file}"
  rm -rf "${store_root}"
  mkdir -p "${store_root}"

  "${pgmemd_bin}" \
    --host "${host}" \
    --port "${port}" \
    --store-backend eloqstore \
    --store-root "${store_root}" \
    --core-number "${core_number}" \
    --enable-io-uring-network-engine false \
    >"${log_file}" 2>&1 &
  local pid=$!

  cleanup() {
    if [[ -n "${pid:-}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup EXIT

  if ! wait_for_health "${host}" "${port}" "${wait_health_seconds}"; then
    echo "[strict] health check failed" >&2
    tail -n 120 "${log_file}" >&2 || true
    exit 1
  fi

  local stats_json
  stats_json="$(curl -fsS "http://${host}:${port}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 8101,
    "method": "memory.stats",
    "params": {"workspace_id": "eloqstore-strict", "window": "5m"}
  }')"

  python3 - <<'PY' "${stats_json}"
import json
import sys
obj = json.loads(sys.argv[1])
result = obj.get("result", {})
backend = result.get("effective_backend")
mode = result.get("write_ack_mode")
if backend != "eloqstore":
    raise SystemExit(f"expected effective_backend=eloqstore, got {backend!r}")
if mode != "accepted":
    raise SystemExit(f"expected write_ack_mode='accepted', got {mode!r}")
print(f"[strict] effective_backend={backend} write_ack_mode={mode}")
PY

  if grep -q "auto-downgrading backend to in-memory" "${log_file}"; then
    echo "[strict] unexpected downgrade log found" >&2
    tail -n 120 "${log_file}" >&2 || true
    exit 1
  fi

  if [[ "${with_smoke}" -eq 1 ]]; then
    run_smoke --url "http://${host}:${port}" --workspace "eloqstore-strict" --session "s1"
  fi

  if [[ "${with_stress}" -eq 1 ]]; then
    run_stress \
      --url "http://${host}:${port}" \
      --workspace "eloqstore-strict" \
      --session "s1" \
      --iterations "${stress_iterations}" \
      --timeout-seconds "${stress_timeout_seconds}" \
      --require-write-ack-mode "accepted" \
      --require-effective-backend "eloqstore"
  fi

  kill -TERM "${pid}" >/dev/null 2>&1 || true
  if ! wait_for_exit "${pid}" "${wait_exit_seconds}"; then
    echo "[strict] process did not exit after SIGTERM" >&2
    exit 1
  fi

  wait "${pid}" || true
  trap - EXIT

  echo "[strict] verification passed"
}

run_shutdown() {
  local pgmemd_bin="${ROOT}/build/pgmemd"
  local host="127.0.0.1"
  local port="18767"
  local store_backend="inmemory"
  local count=500
  local core_number="2"
  local shutdown_timeout_ms="100"
  local wait_health_seconds=10
  local wait_exit_seconds=10
  local store_root="${ROOT}/.pgmem/shutdown-verify"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --pgmemd-bin)
        pgmemd_bin="$2"
        shift 2
        ;;
      --host)
        host="$2"
        shift 2
        ;;
      --port)
        port="$2"
        shift 2
        ;;
      --store-backend)
        store_backend="$2"
        shift 2
        ;;
      --core-number)
        core_number="$2"
        shift 2
        ;;
      --shutdown-timeout-ms)
        shutdown_timeout_ms="$2"
        shift 2
        ;;
      --count)
        count="$2"
        shift 2
        ;;
      --help|-h)
        cat <<USAGE
Usage:
  scripts/verify.sh shutdown [options]

Options:
  --pgmemd-bin <path>         pgmemd binary path
  --host <ip>                 Bind host (default: 127.0.0.1)
  --port <N>                  Bind port (default: 18767)
  --store-backend <name>      Backend (default: inmemory)
  --core-number <N>           Runtime core-number (default: 2)
  --shutdown-timeout-ms <N>   Shutdown drain timeout passed to pgmemd (default: 100)
  --count <N>                 Number of commit_turn requests before SIGTERM (default: 500)
USAGE
        return 0
        ;;
      *)
        echo "Unknown option for shutdown: $1" >&2
        return 1
        ;;
    esac
  done

  if [[ ! -x "$pgmemd_bin" ]]; then
    echo "pgmemd not executable: $pgmemd_bin" >&2
    return 1
  fi

  mkdir -p "${ROOT}/.pgmem"
  rm -rf "${store_root}"
  mkdir -p "${store_root}"

  "$pgmemd_bin" \
    --host "$host" \
    --port "$port" \
    --store-backend "$store_backend" \
    --store-root "${store_root}" \
    --core-number "$core_number" \
    --enable-io-uring-network-engine false \
    --shutdown-drain-timeout-ms "$shutdown_timeout_ms" &
  local pid=$!

  cleanup() {
    if [[ -n "${pid:-}" ]]; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup EXIT

  if ! wait_for_health "${host}" "${port}" "${wait_health_seconds}"; then
    echo "[shutdown] health check failed" >&2
    exit 1
  fi

  echo "[shutdown] sending ${count} writes"
  local i
  for ((i=1; i<=count; ++i)); do
    curl -fsS "http://${host}:${port}/mcp" -X POST -H 'Content-Type: application/json' -d "{
      \"id\": ${i},
      \"method\": \"memory.commit_turn\",
      \"params\": {
        \"workspace_id\": \"shutdown-verify\",
        \"session_id\": \"s1\",
        \"user_text\": \"shutdown test ${i}\",
        \"assistant_text\": \"payload ${i}\"
      }
    }" >/dev/null
  done

  local pre_term_stats
  pre_term_stats="$(curl -fsS "http://${host}:${port}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 6601,
    "method": "memory.stats",
    "params": {"workspace_id": "shutdown-verify", "window": "5m"}
  }')"

  python3 - <<'PY' "${pre_term_stats}"
import json
import sys
obj = json.loads(sys.argv[1])
result = obj.get("result", {})
if result.get("write_ack_mode") != "accepted":
    raise SystemExit("write_ack_mode must be accepted")
print("[shutdown] pre-term pending_write_ops=" + str(result.get("pending_write_ops", "")))
PY

  echo "[shutdown] sending SIGTERM"
  kill -TERM "$pid"

  if ! wait_for_exit "$pid" "${wait_exit_seconds}"; then
    echo "[shutdown] process did not exit in time" >&2
    exit 1
  fi

  wait "$pid"
  trap - EXIT

  echo "[shutdown] process exited cleanly"
}

run_downgrade() {
  local pgmemd_bin="${ROOT}/build/pgmemd"
  local host="127.0.0.1"
  local port="18768"
  local wait_health_seconds=10
  local wait_exit_seconds=5
  local store_root="${ROOT}/.pgmem/io-uring-downgrade"
  local log_file="${ROOT}/.pgmem/io-uring-downgrade.log"
  local core_number="2"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --pgmemd-bin)
        pgmemd_bin="$2"
        shift 2
        ;;
      --host)
        host="$2"
        shift 2
        ;;
      --port)
        port="$2"
        shift 2
        ;;
      --store-root)
        store_root="$2"
        shift 2
        ;;
      --core-number)
        core_number="$2"
        shift 2
        ;;
      --wait-health)
        wait_health_seconds="$2"
        shift 2
        ;;
      --help|-h)
        cat <<USAGE
Usage:
  scripts/verify.sh downgrade [options]

Options:
  --pgmemd-bin <path>      pgmemd binary path
  --host <ip>              Bind host (default: 127.0.0.1)
  --port <N>               Bind port (default: 18768)
  --store-root <path>      Store root for this check
  --core-number <N>        Runtime core-number (default: 2)
  --wait-health <sec>      Max seconds to wait for /health (default: 10)
USAGE
        return 0
        ;;
      *)
        echo "Unknown option for downgrade: $1" >&2
        return 1
        ;;
    esac
  done

  if [[ ! -x "${pgmemd_bin}" ]]; then
    echo "pgmemd not executable: ${pgmemd_bin}" >&2
    return 1
  fi

  mkdir -p "${ROOT}/.pgmem"
  rm -rf "${store_root}"
  mkdir -p "${store_root}"
  rm -f "${log_file}"

  PGMEM_FORCE_IO_URING_UNAVAILABLE=1 \
    "${pgmemd_bin}" \
    --host "${host}" \
    --port "${port}" \
    --store-backend eloqstore \
    --store-root "${store_root}" \
    --core-number "${core_number}" \
    --enable-io-uring-network-engine false \
    >"${log_file}" 2>&1 &
  local pid=$!

  cleanup() {
    if [[ -n "${pid:-}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup EXIT

  if ! wait_for_health "${host}" "${port}" "${wait_health_seconds}"; then
    echo "[downgrade] health check failed" >&2
    tail -n 80 "${log_file}" >&2 || true
    exit 1
  fi

  local stats_json
  stats_json="$(curl -fsS "http://${host}:${port}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 7001,
    "method": "memory.stats",
    "params": {"workspace_id": "io-uring-probe", "window": "5m"}
  }')"

  python3 - <<'PY' "${stats_json}"
import json
import sys
obj = json.loads(sys.argv[1])
result = obj.get("result", {})
backend = result.get("effective_backend")
mode = result.get("write_ack_mode")
if backend != "inmemory":
    raise SystemExit(f"expected effective_backend=inmemory, got {backend!r}")
if mode != "accepted":
    raise SystemExit(f"expected write_ack_mode='accepted', got {mode!r}")
print(f"[downgrade] effective_backend={backend} write_ack_mode={mode}")
PY

  if ! grep -q "auto-downgrading backend to in-memory" "${log_file}"; then
    echo "[downgrade] expected downgrade warning not found in log" >&2
    tail -n 80 "${log_file}" >&2 || true
    exit 1
  fi

  kill -TERM "${pid}" >/dev/null 2>&1 || true
  if ! wait_for_exit "${pid}" "${wait_exit_seconds}"; then
    echo "[downgrade] process did not exit after SIGTERM" >&2
    exit 1
  fi

  wait "${pid}" || true
  trap - EXIT

  echo "[downgrade] verification passed"
}

run_all() {
  local pgmemd_bin="${ROOT}/build/pgmemd"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --pgmemd-bin)
        pgmemd_bin="$2"
        shift 2
        ;;
      --help|-h)
        cat <<USAGE
Usage:
  scripts/verify.sh all [--pgmemd-bin <path>]
USAGE
        return 0
        ;;
      *)
        echo "Unknown option for all: $1" >&2
        return 1
        ;;
    esac
  done

  run_strict --pgmemd-bin "${pgmemd_bin}" --with-smoke
  run_strict --pgmemd-bin "${pgmemd_bin}" --with-stress --stress-iterations 2000 --stress-timeout 30
  run_shutdown --pgmemd-bin "${pgmemd_bin}" --store-backend inmemory --count 500
  run_downgrade --pgmemd-bin "${pgmemd_bin}"
}

scenario="${1:-}"
if [[ -z "${scenario}" ]]; then
  usage
  exit 1
fi
shift || true

case "${scenario}" in
  smoke)
    run_smoke "$@"
    ;;
  stress)
    run_stress "$@"
    ;;
  strict)
    run_strict "$@"
    ;;
  shutdown)
    run_shutdown "$@"
    ;;
  downgrade)
    run_downgrade "$@"
    ;;
  all)
    run_all "$@"
    ;;
  --help|-h|help)
    usage
    ;;
  *)
    echo "Unknown scenario: ${scenario}" >&2
    usage >&2
    exit 1
    ;;
esac
