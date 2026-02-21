#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PGMEMD_BIN="${ROOT}/build/pgmemd"
BACKEND="eloqstore"
HOST="0.0.0.0"
PORT="8765"
CORE_NUMBER="4"
STORE_ROOT="${ROOT}/.pgmem/store"
RUN_SMOKE=0

VERIFY_KEEP_ARTIFACTS=0
VERIFY_COVERAGE_THRESHOLD="85"

usage() {
  cat <<USAGE
Usage:
  scripts/start.sh [options]
  scripts/start.sh verify

Options (start mode):
  --backend <eloqstore|inmemory>  Backend request (default: eloqstore)
  --host <ip>                     Bind host (default: 0.0.0.0)
  --port <N>                      Bind port (default: 8765)
  --store-root <path>             Store root directory (default: ./.pgmem/store)
  --core-number <N>               Runtime core-number (default: 4)
  --smoke                         Run smoke test then exit
  --help, -h                      Show this help

verify:
  Run single-entry verification pipeline (fixed full profile).
  Supported options:
    --keep-artifacts               Keep runtime artifacts on failure/success
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
    if curl -fsS -m 2 "http://${host}:${port}/health" >/dev/null 2>&1; then
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

verify_require_prebuilt() {
  if [[ ! -x "${PGMEMD_BIN}" ]]; then
    echo "[verify] pgmemd not executable: ${PGMEMD_BIN}" >&2
    echo "[verify] build first: cmake -S . -B build && cmake --build build -j" >&2
    return 1
  fi

  if [[ ! -f "${ROOT}/build/CTestTestfile.cmake" ]]; then
    echo "[verify] missing CTest metadata in build/. verify does not auto-build." >&2
    echo "[verify] configure/build with BUILD_TESTING=ON first." >&2
    return 1
  fi

  if ! command -v ctest >/dev/null 2>&1; then
    echo "[verify] missing ctest in PATH" >&2
    return 1
  fi
}

verify_reset_runtime() {
  rm -rf "${ROOT}/.pgmem" "${ROOT}/.mock-bin" "${ROOT}/.mock-state"
  mkdir -p "${ROOT}/.pgmem"
}

run_verify_stage() {
  local stage_name="$1"
  shift
  echo "[verify] ===== stage: ${stage_name} ====="
  verify_reset_runtime
  "$@"
}

run_ctest_stage() {
  echo "[verify] running ctest"
  ctest --test-dir "${ROOT}/build" --output-on-failure
}

run_smoke_scenario() {
  local base_url="$1"
  local workspace_id="$2"
  local session_id="$3"

  local health_json="$(mktemp /tmp/pgmem-health.XXXXXX.json)"
  local write_json="$(mktemp /tmp/pgmem-write.XXXXXX.json)"
  local query_json="$(mktemp /tmp/pgmem-query.XXXXXX.json)"
  local stats_json="$(mktemp /tmp/pgmem-stats.XXXXXX.json)"
  local describe_json="$(mktemp /tmp/pgmem-describe.XXXXXX.json)"
  local describe_examples_json="$(mktemp /tmp/pgmem-describe-examples.XXXXXX.json)"
  local describe_http_json="$(mktemp /tmp/pgmem-describe-http.XXXXXX.json)"
  local rpc_init_json="$(mktemp /tmp/pgmem-rpc-init.XXXXXX.json)"
  local rpc_tools_json="$(mktemp /tmp/pgmem-rpc-tools.XXXXXX.json)"
  local rpc_call_json="$(mktemp /tmp/pgmem-rpc-call.XXXXXX.json)"
  local bad_json="$(mktemp /tmp/pgmem-bad.XXXXXX.json)"
  local sync_push_json="$(mktemp /tmp/pgmem-sync-push.XXXXXX.json)"
  local sync_pull_json="$(mktemp /tmp/pgmem-sync-pull.XXXXXX.json)"

  curl -fsS -m 5 "${base_url}/health" > "${health_json}"
  json_assert "${health_json}" "data.get('status') == 'ok'" "health check failed: status != ok"

  curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
    \"id\": 1,
    \"method\": \"memory.write\",
    \"params\": {
      \"workspace_id\": \"${workspace_id}\",
      \"session_id\": \"${session_id}\",
      \"records\": [{
        \"source\": \"turn\",
        \"content\": \"please remember retry strategy using exponential backoff\",
        \"tags\": [\"smoke\", \"retry\"]
      }]
    }
  }" > "${write_json}"
  json_assert "${write_json}" "len(data.get('result', {}).get('stored_ids', [])) > 0" "write failed: stored_ids is empty"
  json_assert "${write_json}" "isinstance(data.get('result', {}).get('stored_ids', [None])[0], str) and len(data.get('result', {}).get('stored_ids', [''])[0]) > 0" "write failed: first stored_id is invalid"

  curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
    \"id\": 2,
    \"method\": \"memory.query\",
    \"params\": {
      \"workspace_id\": \"${workspace_id}\",
      \"query\": \"retry backoff\",
      \"top_k\": 3,
      \"token_budget\": 1200
    }
  }" > "${query_json}"
  json_assert "${query_json}" "len(data.get('result', {}).get('hits', [])) > 0" "query failed: hits is empty"
  json_assert "${query_json}" "isinstance(data.get('result', {}).get('hits', [])[0].get('memory_id', ''), str) and len(data.get('result', {}).get('hits', [])[0].get('memory_id', '')) > 0" "query failed: first hit has invalid memory_id"

  curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
    \"id\": 3,
    \"method\": \"memory.stats\",
    \"params\": {
      \"workspace_id\": \"${workspace_id}\",
      \"window\": \"5m\"
    }
  }" > "${stats_json}"
  json_assert "${stats_json}" "'write_ack_mode' in data.get('result', {})" "stats missing write_ack_mode"
  json_assert "${stats_json}" "'resident_used_bytes' in data.get('result', {})" "stats missing resident_used_bytes"
  json_assert "${stats_json}" "'resident_limit_bytes' in data.get('result', {})" "stats missing resident_limit_bytes"
  json_assert "${stats_json}" "'resident_evicted_count' in data.get('result', {})" "stats missing resident_evicted_count"
  json_assert "${stats_json}" "'disk_fallback_search_count' in data.get('result', {})" "stats missing disk_fallback_search_count"
  json_assert "${stats_json}" "'effective_backend' in data.get('result', {})" "stats missing effective_backend"
  json_assert "${stats_json}" "data.get('result', {}).get('write_ack_mode') == 'durable'" "stats write_ack_mode must be durable"
  json_assert "${stats_json}" "'pending_write_ops' not in data.get('result', {})" "stats must not expose pending_write_ops"
  json_assert "${stats_json}" "'flush_failures_total' not in data.get('result', {})" "stats must not expose flush_failures_total"
  json_assert "${stats_json}" "'volatile_dropped_on_shutdown' not in data.get('result', {})" "stats must not expose volatile_dropped_on_shutdown"
  json_assert "${stats_json}" "'last_flush_error' not in data.get('result', {})" "stats must not expose last_flush_error"

  curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 4,
    "method": "memory.describe",
    "params": {}
  }' > "${describe_json}"
  json_assert "${describe_json}" "data.get('result', {}).get('describe_version') == '2.0.0'" "describe failed: describe_version mismatch"
  json_assert "${describe_json}" "data.get('result', {}).get('schema_revision') == '2026-02-19.1'" "describe failed: schema_revision mismatch"
  json_assert "${describe_json}" "isinstance(data.get('result', {}).get('generated_at_ms'), int)" "describe failed: generated_at_ms must be integer"
  json_assert "${describe_json}" "data.get('result', {}).get('server', {}).get('kind') == 'memory-server'" "describe failed: server.kind mismatch"
  json_assert "${describe_json}" "data.get('result', {}).get('server', {}).get('write_ack_mode') == 'durable'" "describe failed: write_ack_mode mismatch"
  json_assert "${describe_json}" "data.get('result', {}).get('server', {}).get('sync_routes_available') in (True, False)" "describe failed: sync_routes_available must be boolean"
  json_assert "${describe_json}" "'memory.write' in data.get('result', {}).get('method_names', [])" "describe failed: method_names missing memory.write"
  json_assert "${describe_json}" "data.get('result', {}).get('methods', {}).get('memory.stats', {}).get('output_schema', {}).get('properties', {}).get('resident_evicted_count', {}).get('type') == 'integer'" "describe failed: resident_evicted_count contract missing"
  json_assert "${describe_json}" "'pending_write_ops' not in data.get('result', {}).get('methods', {}).get('memory.stats', {}).get('output_schema', {}).get('properties', {})" "describe failed: deprecated pending_write_ops still exposed"
  json_assert "${describe_json}" "'examples' not in data.get('result', {}).get('methods', {}).get('memory.query', {})" "describe failed: examples should be omitted by default"
  json_assert "${describe_json}" "data.get('result', {}).get('methods', {}).get('memory.describe', {}).get('input_schema', {}).get('properties', {}).get('include_examples', {}).get('type') == 'boolean'" "describe failed: include_examples schema missing"
  json_assert "${describe_json}" "data.get('result', {}).get('methods', {}).get('memory.describe', {}).get('input_schema', {}).get('properties', {}).get('include_examples', {}).get('default') in (True, False)" "describe failed: include_examples default must be boolean"

  curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 41,
    "method": "memory.describe",
    "params": {"include_examples": true}
  }' > "${describe_examples_json}"
  json_assert "${describe_examples_json}" "'examples' in data.get('result', {}).get('methods', {}).get('memory.query', {})" "describe(include_examples=true) failed: examples missing"
  json_assert "${describe_examples_json}" "len(data.get('result', {}).get('methods', {}).get('memory.write', {}).get('examples', [])) > 0" "describe(include_examples=true) failed: write examples empty"

  curl -fsS -m 5 "${base_url}/mcp/describe" > "${describe_http_json}"
  json_assert "${describe_http_json}" "data.get('describe_version') == '2.0.0'" "GET /mcp/describe failed: describe_version mismatch"
  json_assert "${describe_http_json}" "data.get('schema_revision') == '2026-02-19.1'" "GET /mcp/describe failed: schema_revision mismatch"
  json_assert "${describe_http_json}" "data.get('server', {}).get('endpoint') == '/mcp'" "GET /mcp/describe failed: endpoint contract mismatch"
  json_assert "${describe_http_json}" "data.get('server', {}).get('describe_endpoint') == '/mcp/describe'" "GET /mcp/describe failed: describe endpoint mismatch"
  json_assert "${describe_http_json}" "'examples' not in data.get('methods', {}).get('memory.query', {})" "GET /mcp/describe failed: default payload should omit examples"

  curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "jsonrpc": "2.0",
    "id": 101,
    "method": "initialize",
    "params": {"clientInfo": {"name": "verify", "version": "1"}}
  }' > "${rpc_init_json}"
  json_assert "${rpc_init_json}" "data.get('jsonrpc') == '2.0'" "jsonrpc initialize failed: missing jsonrpc=2.0"
  json_assert "${rpc_init_json}" "data.get('result', {}).get('capabilities', {}).get('tools') is not None" "jsonrpc initialize failed: tools capability missing"

  curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "jsonrpc": "2.0",
    "id": 102,
    "method": "tools/list",
    "params": {}
  }' > "${rpc_tools_json}"
  json_assert "${rpc_tools_json}" "data.get('jsonrpc') == '2.0'" "jsonrpc tools/list failed: missing jsonrpc=2.0"
  json_assert "${rpc_tools_json}" "any(t.get('name') == 'memory.query' for t in data.get('result', {}).get('tools', []))" "jsonrpc tools/list failed: memory.query not found"
  json_assert "${rpc_tools_json}" "any(t.get('name') == 'memory.describe' for t in data.get('result', {}).get('tools', []))" "jsonrpc tools/list failed: memory.describe not found"
  json_assert "${rpc_tools_json}" "any(t.get('name') == 'memory.describe' and t.get('inputSchema', {}).get('properties', {}).get('include_examples', {}).get('type') == 'boolean' for t in data.get('result', {}).get('tools', []))" "jsonrpc tools/list failed: memory.describe include_examples schema mismatch"
  json_assert "${rpc_tools_json}" "not any(t.get('name') == 'store.compact' for t in data.get('result', {}).get('tools', []))" "jsonrpc tools/list failed: hidden store.compact must not be exposed"

  curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
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
  bad_status="$(curl -s -m 5 -o "${bad_json}" -w "%{http_code}" \
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
  sync_push_status="$(curl -s -m 5 -o "${sync_push_json}" -w "%{http_code}" \
    "${base_url}/sync/push" -X POST -H 'Content-Type: application/json' -d '{}')"
  if [[ "${sync_push_status}" != "404" ]]; then
    echo "/sync/push expected 404, got ${sync_push_status}" >&2
    cat "${sync_push_json}" >&2
    return 1
  fi

  local sync_pull_status
  sync_pull_status="$(curl -s -m 5 -o "${sync_pull_json}" -w "%{http_code}" \
    "${base_url}/sync/pull" -X POST -H 'Content-Type: application/json' -d '{}')"
  if [[ "${sync_pull_status}" != "404" ]]; then
    echo "/sync/pull expected 404, got ${sync_pull_status}" >&2
    cat "${sync_pull_json}" >&2
    return 1
  fi

  echo "[verify] smoke completed"
}

run_stress_scenario() {
  local base_url="$1"
  local workspace_id="$2"
  local session_id="$3"
  local iterations="$4"
  local progress_step="$5"
  local require_effective_backend="$6"

  curl -fsS -m 5 "${base_url}/health" >/dev/null

  echo "[stress] start write load: iterations=${iterations}"
  local i
  local last_token=""
  for ((i=1; i<=iterations; ++i)); do
    local token="stress_token_${i}"
    last_token="${token}"
    curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
      \"id\": ${i},
      \"method\": \"memory.write\",
      \"params\": {
        \"workspace_id\": \"${workspace_id}\",
        \"session_id\": \"${session_id}\",
        \"records\": [{
          \"source\": \"turn\",
          \"content\": \"stress turn ${i} durable stress payload ${token} ${token} ${token}\",
          \"tags\": [\"stress\"]
        }]
      }
    }" >/dev/null

    if (( i % progress_step == 0 )); then
      echo "[stress] progress: ${i}/${iterations}"
    fi
  done

  if [[ -z "${last_token}" ]]; then
    echo "[stress] no writes submitted" >&2
    return 1
  fi

  local found=0
  local attempt
  for attempt in $(seq 1 20); do
    local search_json
    search_json="$(curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
      \"id\": 999100,
      \"method\": \"memory.query\",
      \"params\": {
        \"workspace_id\": \"${workspace_id}\",
        \"query\": \"${last_token}\",
        \"top_k\": 5,
        \"token_budget\": 0
      }
    }")"

    if python3 - <<'PY' "$search_json" "$last_token"
import json
import sys
obj=json.loads(sys.argv[1])
token=sys.argv[2]
for hit in obj.get("result", {}).get("hits", []):
    if token in hit.get("content", ""):
        raise SystemExit(0)
raise SystemExit(1)
PY
    then
      found=1
      break
    fi
    sleep 0.1
  done

  if [[ "${found}" != "1" ]]; then
    echo "[stress] could not find last token in search results: ${last_token}" >&2
    return 1
  fi

  local last_stats
  last_stats="$(curl -fsS -m 10 "${base_url}/mcp" -X POST -H 'Content-Type: application/json' -d "{
    \"id\": 999001,
    \"method\": \"memory.stats\",
    \"params\": {\"workspace_id\": \"${workspace_id}\", \"window\": \"5m\"}
  }")"

  python3 - <<'PY' "$last_stats" "$require_effective_backend"
import json
import sys
obj=json.loads(sys.argv[1])
required_backend=sys.argv[2]
result=obj.get("result",{})
mode=result.get("write_ack_mode")
backend=result.get("effective_backend", "")
if mode != "durable":
    raise SystemExit(f"write_ack_mode mismatch: expected durable, got {mode!r}")
if required_backend != "any" and backend != required_backend:
    raise SystemExit(f"effective_backend mismatch: expected {required_backend}, got {backend!r}")
for k in ("resident_used_bytes", "resident_limit_bytes", "resident_evicted_count", "disk_fallback_search_count"):
    if k not in result:
        raise SystemExit(f"missing stats field: {k}")
for legacy in ("pending_write_ops", "flush_failures_total", "volatile_dropped_on_shutdown", "last_flush_error"):
    if legacy in result:
        raise SystemExit(f"legacy stats field still exposed: {legacy}")
print("[stress] final stats:", json.dumps({
    "write_ack_mode": mode,
    "effective_backend": backend,
    "resident_used_bytes": result.get("resident_used_bytes", 0),
    "resident_limit_bytes": result.get("resident_limit_bytes", 0),
    "resident_evicted_count": result.get("resident_evicted_count", 0),
    "disk_fallback_search_count": result.get("disk_fallback_search_count", 0),
}, ensure_ascii=False))
PY

  echo "[stress] completed"
}

run_strict_scenario() {
  local with_smoke="$1"
  local with_stress="$2"
  local stress_iterations="${3:-0}"

  local host="127.0.0.1"
  local port="18765"
  if [[ "${with_stress}" == "true" ]]; then
    port="18766"
  fi
  local wait_health_seconds=20
  local wait_exit_seconds=8
  local store_root="${ROOT}/.pgmem/eloqstore-strict-${port}"
  local log_file="${ROOT}/.pgmem/eloqstore-strict-${port}.log"
  local core_number="2"

  mkdir -p "${ROOT}/.pgmem"
  rm -f "${log_file}"
  rm -rf "${store_root}"
  mkdir -p "${store_root}"

  local pid=""
  cleanup() {
    trap - RETURN
    if [[ -n "${pid:-}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup RETURN

  "${PGMEMD_BIN}" \
    --host "${host}" \
    --port "${port}" \
    --store-backend eloqstore \
    --store-root "${store_root}" \
    --core-number "${core_number}" \
    >"${log_file}" 2>&1 &
  pid=$!

  if ! wait_for_health "${host}" "${port}" "${wait_health_seconds}"; then
    echo "[strict] health check failed" >&2
    tail -n 120 "${log_file}" >&2 || true
    return 1
  fi

  local stats_json
  stats_json="$(curl -fsS -m 10 "http://${host}:${port}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 8101,
    "method": "memory.stats",
    "params": {"workspace_id": "eloqstore-strict", "window": "5m"}
  }')"

  local detected_backend
  detected_backend="$(python3 - <<'PY' "${stats_json}"
import json
import sys
obj = json.loads(sys.argv[1])
result = obj.get("result", {})
backend = result.get("effective_backend")
mode = result.get("write_ack_mode")
if mode != "durable":
    raise SystemExit(f"expected write_ack_mode='durable', got {mode!r}")
if backend != "eloqstore":
    raise SystemExit(f"expected effective_backend='eloqstore', got {backend!r}")
print(f"[strict] effective_backend={backend} write_ack_mode={mode}")
print(backend)
PY
)"
  detected_backend="$(echo "${detected_backend}" | tail -n1)"

  if [[ "${with_smoke}" == "true" ]]; then
    run_smoke_scenario "http://${host}:${port}" "eloqstore-strict" "s1"
  fi

  if [[ "${with_stress}" == "true" ]]; then
    run_stress_scenario "http://${host}:${port}" "eloqstore-strict" "s1" "${stress_iterations}" 500 "${detected_backend}"
  fi

  kill -TERM "${pid}" >/dev/null 2>&1 || true
  if ! wait_for_exit "${pid}" "${wait_exit_seconds}"; then
    echo "[strict] process did not exit after SIGTERM" >&2
    return 1
  fi

  wait "${pid}" || true
  pid=""
  trap - RETURN

  echo "[strict] verification passed"
}

run_shutdown_scenario() {
  local host="127.0.0.1"
  local port="18767"
  local store_backend="inmemory"
  local count=500
  local core_number="2"
  local wait_health_seconds=12
  local wait_exit_seconds=10
  local store_root="${ROOT}/.pgmem/shutdown-verify"
  local log_file="${ROOT}/.pgmem/shutdown-verify.log"

  mkdir -p "${ROOT}/.pgmem"
  rm -rf "${store_root}"
  mkdir -p "${store_root}"
  rm -f "${log_file}"

  local pid=""
  cleanup() {
    trap - RETURN
    if [[ -n "${pid:-}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup RETURN

  "${PGMEMD_BIN}" \
    --host "${host}" \
    --port "${port}" \
    --store-backend "${store_backend}" \
    --store-root "${store_root}" \
    --core-number "${core_number}" \
    >"${log_file}" 2>&1 &
  pid=$!

  if ! wait_for_health "${host}" "${port}" "${wait_health_seconds}"; then
    echo "[shutdown] health check failed" >&2
    tail -n 80 "${log_file}" >&2 || true
    return 1
  fi

  echo "[shutdown] sending ${count} writes"
  local i
  for ((i=1; i<=count; ++i)); do
    curl -fsS -m 10 "http://${host}:${port}/mcp" -X POST -H 'Content-Type: application/json' -d "{
      \"id\": ${i},
      \"method\": \"memory.write\",
      \"params\": {
        \"workspace_id\": \"shutdown-verify\",
        \"session_id\": \"s1\",
        \"records\": [{
          \"source\": \"turn\",
          \"content\": \"shutdown test ${i} payload ${i}\"
        }]
      }
    }" >/dev/null
  done

  local pre_term_stats
  pre_term_stats="$(curl -fsS -m 10 "http://${host}:${port}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 6601,
    "method": "memory.stats",
    "params": {"workspace_id": "shutdown-verify", "window": "5m"}
  }')"

  python3 - <<'PY' "${pre_term_stats}"
import json
import sys
obj = json.loads(sys.argv[1])
result = obj.get("result", {})
if result.get("write_ack_mode") != "durable":
    raise SystemExit("write_ack_mode must be durable")
for legacy in ("pending_write_ops", "flush_failures_total", "volatile_dropped_on_shutdown", "last_flush_error"):
    if legacy in result:
        raise SystemExit(f"legacy stats field still exposed: {legacy}")
print("[shutdown] pre-term resident_used_bytes=" + str(result.get("resident_used_bytes", "")))
PY

  echo "[shutdown] sending SIGTERM"
  kill -TERM "${pid}"

  if ! wait_for_exit "${pid}" "${wait_exit_seconds}"; then
    echo "[shutdown] process did not exit in time" >&2
    return 1
  fi

  wait "${pid}" || true
  pid=""
  trap - RETURN

  echo "[shutdown] process exited cleanly"
}

run_io_uring_strict_fail_scenario() {
  local host="127.0.0.1"
  local port="18768"
  local wait_exit_seconds=8
  local store_root="${ROOT}/.pgmem/io-uring-strict-fail"
  local log_file="${ROOT}/.pgmem/io-uring-strict-fail.log"
  local core_number="2"

  mkdir -p "${ROOT}/.pgmem"
  rm -rf "${store_root}"
  mkdir -p "${store_root}"
  rm -f "${log_file}"

  local pid=""
  cleanup() {
    trap - RETURN
    if [[ -n "${pid:-}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup RETURN

  "${PGMEMD_BIN}" \
    --host "${host}" \
    --port "${port}" \
    --store-backend eloqstore \
    --store-root "${store_root}" \
    --core-number "${core_number}" \
    --test-force-io-uring-unavailable \
    >"${log_file}" 2>&1 &
  pid=$!

  if ! wait_for_exit "${pid}" "${wait_exit_seconds}"; then
    echo "[io-uring-strict-fail] pgmemd did not fail fast within ${wait_exit_seconds}s" >&2
    tail -n 80 "${log_file}" >&2 || true
    return 1
  fi

  local exit_code=0
  if wait "${pid}"; then
    exit_code=0
  else
    exit_code=$?
  fi
  pid=""
  trap - RETURN

  if [[ "${exit_code}" -ne 1 ]]; then
    echo "[io-uring-strict-fail] expected exit code 1, got ${exit_code}" >&2
    tail -n 80 "${log_file}" >&2 || true
    return 1
  fi

  if ! grep -q "io_uring unavailable for eloqstore backend" "${log_file}"; then
    echo "[io-uring-strict-fail] expected io_uring fail-fast log not found" >&2
    tail -n 80 "${log_file}" >&2 || true
    return 1
  fi

  if curl -fsS -m 2 "http://${host}:${port}/health" >/dev/null 2>&1; then
    echo "[io-uring-strict-fail] unexpected health endpoint reachable" >&2
    return 1
  fi

  echo "[io-uring-strict-fail] verification passed"
}

run_concurrency_consistency_scenario() {
  local host="127.0.0.1"
  local port="18769"
  local wait_health_seconds=15
  local wait_exit_seconds=8
  local store_root="${ROOT}/.pgmem/concurrency-consistency"
  local log_file="${ROOT}/.pgmem/concurrency-consistency.log"
  local core_number="4"

  mkdir -p "${ROOT}/.pgmem"
  rm -f "${log_file}"
  rm -rf "${store_root}"
  mkdir -p "${store_root}"

  local pid=""
  cleanup() {
    trap - RETURN
    if [[ -n "${pid:-}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup RETURN

  "${PGMEMD_BIN}" \
    --host "${host}" \
    --port "${port}" \
    --store-backend eloqstore \
    --store-root "${store_root}" \
    --core-number "${core_number}" \
    >"${log_file}" 2>&1 &
  pid=$!

  if ! wait_for_health "${host}" "${port}" "${wait_health_seconds}"; then
    echo "[consistency] health check failed" >&2
    tail -n 120 "${log_file}" >&2 || true
    return 1
  fi

  python3 - <<'PY' "http://${host}:${port}"
import concurrent.futures
import json
import random
import threading
import time
import urllib.error
import urllib.request
import sys

base_url = sys.argv[1]
writer_threads = 8
writes_per_thread = 80
reader_threads = 8
reader_iterations = 220
verify_retries = 20

workspace = "verify-concurrency-main"
other_workspace = "verify-concurrency-other"

token_to_id = {}
known_tokens = []
lock = threading.Lock()
mismatch_tokens = []
state = {
    "stop": False,
    "health_failed": False,
    "health_error": "",
}


def post_json(path, payload, timeout=6.0):
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url=base_url + path,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def mcp_call(method, params, timeout=6.0):
    payload = {"id": int(time.time() * 1000) % 1000000000, "method": method, "params": params}
    for attempt in range(3):
        try:
            out = post_json("/mcp", payload, timeout=timeout)
            if "error" in out:
                raise RuntimeError(f"mcp error {out['error']}")
            return out.get("result", {})
        except Exception:
            if attempt >= 2:
                raise
            time.sleep(0.05)
    raise RuntimeError("unreachable")


def health_monitor():
    consecutive_failures = 0
    while not state["stop"]:
        try:
            with urllib.request.urlopen(base_url + "/health", timeout=1.0) as resp:
                _ = resp.read()
            consecutive_failures = 0
        except Exception as exc:
            consecutive_failures += 1
            if consecutive_failures >= 5:
                state["health_failed"] = True
                state["health_error"] = str(exc)
                state["stop"] = True
                return
        time.sleep(0.2)


def seed_workspace_data():
    seed_main = "seed_main_token"
    result = mcp_call(
        "memory.write",
        {
            "workspace_id": workspace,
            "session_id": "seed",
            "records": [
                {
                    "source": "turn",
                    "content": f"seed payload {seed_main} {seed_main}",
                }
            ],
        },
    )
    with lock:
        token_to_id[seed_main] = result["stored_ids"][0]
        known_tokens.append(seed_main)

    other_token = "other_workspace_only_seed"
    mcp_call(
        "memory.write",
        {
            "workspace_id": other_workspace,
            "session_id": "seed",
            "records": [
                {
                    "source": "turn",
                    "content": f"other payload {other_token} {other_token}",
                }
            ],
        },
    )
    return other_token


def writer_task(writer_id):
    for seq in range(writes_per_thread):
        if state["stop"]:
            return
        token = f"w{writer_id}_t{seq}_{writer_id * 100000 + seq}"
        result = mcp_call(
            "memory.write",
            {
                "workspace_id": workspace,
                "session_id": f"writer-{writer_id}",
                "records": [
                    {
                        "source": "turn",
                        "content": f"writer {writer_id} seq {seq} payload {token} {token} {token}",
                    }
                ],
            },
        )
        stored = result.get("stored_ids", [])
        if not stored:
            raise RuntimeError(f"empty stored_ids for token={token}")
        with lock:
            token_to_id[token] = stored[0]
            known_tokens.append(token)


def search_token(workspace_id, token, expected_memory_id=None):
    result = mcp_call(
        "memory.query",
        {
            "workspace_id": workspace_id,
            "query": token,
            "top_k": 5,
            "token_budget": 0,
        },
    )
    hits = result.get("hits", [])
    for hit in hits:
        content = hit.get("content", "")
        memory_id = hit.get("memory_id", "")
        if token in content:
            if expected_memory_id is None:
                return True
            if memory_id == expected_memory_id:
                return True
    return False


def reader_task(_reader_id):
    for _ in range(reader_iterations):
        if state["stop"]:
            return
        with lock:
            if not known_tokens:
                token = None
                expected = None
            else:
                token = random.choice(known_tokens)
                expected = token_to_id.get(token)
        if token is None:
            time.sleep(0.01)
            continue

        ok = False
        for _retry in range(3):
            if search_token(workspace, token, expected):
                ok = True
                break
            time.sleep(0.02)
        if not ok:
            mismatch_tokens.append(token)


def verify_all_tokens():
    with lock:
        items = list(token_to_id.items())
    missing = []
    for token, memory_id in items:
        found = False
        for _ in range(verify_retries):
            if search_token(workspace, token, memory_id):
                found = True
                break
            time.sleep(0.05)
        if not found:
            missing.append(token)
    return missing


monitor = threading.Thread(target=health_monitor, daemon=True)
monitor.start()

other_seed = seed_workspace_data()

with concurrent.futures.ThreadPoolExecutor(max_workers=writer_threads + reader_threads) as executor:
    writer_futures = [executor.submit(writer_task, i) for i in range(writer_threads)]
    reader_futures = [executor.submit(reader_task, i) for i in range(reader_threads)]

    for future in writer_futures:
        future.result()
    for future in reader_futures:
        future.result()

missing_tokens = verify_all_tokens()

with lock:
    sample_main = known_tokens[0] if known_tokens else ""

cross_violation = False
if sample_main:
    if search_token(other_workspace, sample_main):
        cross_violation = True
if search_token(workspace, other_seed):
    cross_violation = True
if not search_token(other_workspace, other_seed):
    cross_violation = True

state["stop"] = True
monitor.join(timeout=2.0)

print(
    json.dumps(
        {
            "profile": "full",
            "writer_threads": writer_threads,
            "writes_per_thread": writes_per_thread,
            "reader_threads": reader_threads,
            "reader_iterations": reader_iterations,
            "written_tokens": len(token_to_id),
            "mismatch_count": len(mismatch_tokens),
            "missing_count": len(missing_tokens),
            "health_failed": state["health_failed"],
            "cross_violation": cross_violation,
        },
        ensure_ascii=False,
    )
)

if state["health_failed"]:
    raise SystemExit(f"health monitor failed: {state['health_error']}")
if mismatch_tokens:
    raise SystemExit(f"reader mismatch detected, count={len(mismatch_tokens)}")
if missing_tokens:
    raise SystemExit(f"final consistency missing tokens, count={len(missing_tokens)}")
if cross_violation:
    raise SystemExit("workspace isolation violation detected")
PY

  local stats_json
  stats_json="$(curl -fsS -m 10 "http://${host}:${port}/mcp" -X POST -H 'Content-Type: application/json' -d '{
    "id": 8901,
    "method": "memory.stats",
    "params": {"workspace_id": "verify-concurrency-main", "window": "5m"}
  }')"

  python3 - <<'PY' "${stats_json}"
import json
import sys
obj = json.loads(sys.argv[1])
result = obj.get("result", {})
if result.get("write_ack_mode") != "durable":
    raise SystemExit("consistency stats write_ack_mode must be durable")
print("[consistency] durable stats ok")
PY

  kill -TERM "${pid}" >/dev/null 2>&1 || true
  if ! wait_for_exit "${pid}" "${wait_exit_seconds}"; then
    echo "[consistency] process did not exit after SIGTERM" >&2
    return 1
  fi

  wait "${pid}" || true
  pid=""
  trap - RETURN

  echo "[consistency] verification passed"
}

verify_has_coverage_instrumentation() {
  local cache_file="${ROOT}/build/CMakeCache.txt"
  if [[ ! -f "${cache_file}" ]]; then
    return 1
  fi
  if ! grep -q '^PGMEM_ENABLE_COVERAGE:BOOL=ON$' "${cache_file}"; then
    return 1
  fi
  if ! find "${ROOT}/build" -name '*.gcno' -print -quit | grep -q .; then
    return 1
  fi
  return 0
}

run_coverage_stage() {
  if ! command -v lcov >/dev/null 2>&1 || ! command -v genhtml >/dev/null 2>&1; then
    echo "[verify] full profile requires lcov/genhtml in PATH" >&2
    return 1
  fi

  if ! verify_has_coverage_instrumentation; then
    echo "[verify] full profile requires coverage build artifacts." >&2
    echo "[verify] rebuild with: cmake -S . -B build -DPGMEM_ENABLE_COVERAGE=ON -DBUILD_TESTING=ON && cmake --build build -j" >&2
    return 1
  fi

  local coverage_dir="${ROOT}/build/coverage"
  local raw_info="${coverage_dir}/coverage.raw.info"
  local core_info="${coverage_dir}/coverage.core.info"
  rm -rf "${coverage_dir}"
  mkdir -p "${coverage_dir}"

  echo "[verify] collecting coverage with lcov (core scope)"
  if ! lcov --capture --directory "${ROOT}/build" --output-file "${raw_info}" \
    --rc geninfo_unexecuted_blocks=1 >/dev/null 2>&1; then
    echo "[verify] lcov capture failed" >&2
    return 1
  fi
  if ! lcov --extract "${raw_info}" \
    '*/src/core/*' \
    '*/include/pgmem/core/*' \
    --output-file "${core_info}" >/dev/null 2>&1; then
    echo "[verify] lcov core extraction failed" >&2
    return 1
  fi

  local summary_text
  if ! summary_text="$(lcov --summary "${core_info}" 2>&1)"; then
    echo "${summary_text}" >&2
    return 1
  fi
  echo "${summary_text}"

  local lines_pct
  lines_pct="$(python3 - <<'PY' "${summary_text}"
import re
import sys
m = re.search(r'lines\.+:\s*([0-9]+(?:\.[0-9]+)?)%', sys.argv[1], re.IGNORECASE)
print(m.group(1) if m else "")
PY
)"

  if [[ -z "${lines_pct}" ]]; then
    echo "[verify] failed to parse line coverage from lcov summary" >&2
    return 1
  fi

  if ! python3 - <<'PY' "${lines_pct}" "${VERIFY_COVERAGE_THRESHOLD}"
import sys
actual = float(sys.argv[1])
threshold = float(sys.argv[2])
if actual + 1e-9 < threshold:
    raise SystemExit(f"coverage gate failed: {actual:.2f}% < {threshold:.2f}%")
print(f"[verify] coverage gate passed: {actual:.2f}% >= {threshold:.2f}%")
PY
  then
    return 1
  fi

  if ! genhtml "${core_info}" --output-directory "${coverage_dir}/html" >/dev/null 2>&1; then
    echo "[verify] genhtml failed" >&2
    return 1
  fi
  echo "[verify] coverage html report: ${coverage_dir}/html/index.html"
}

run_verify_all() {
  verify_require_prebuilt

  if command -v lcov >/dev/null 2>&1 && verify_has_coverage_instrumentation; then
    lcov --zerocounters --directory "${ROOT}/build" >/dev/null 2>&1 || true
  fi

  run_ctest_stage || return 1

  run_verify_stage "strict-smoke" run_strict_scenario true false || return 1
  run_verify_stage "strict-stress" run_strict_scenario false true 2000 || return 1
  run_verify_stage "concurrency-consistency" run_concurrency_consistency_scenario || return 1
  run_verify_stage "shutdown" run_shutdown_scenario || return 1
  run_verify_stage "io-uring-strict-fail" run_io_uring_strict_fail_scenario || return 1
  run_coverage_stage || return 1

  echo "[verify] profile=full completed"
}

run_verify_with_cleanup() {
  local status=0
  if (set -e; run_verify_all); then
    status=0
  else
    status=$?
  fi

  if [[ "${VERIFY_KEEP_ARTIFACTS}" == "1" ]]; then
    echo "[verify] keep-artifacts enabled; artifacts kept under ${ROOT}/.pgmem"
  else
    rm -rf "${ROOT}/.pgmem" "${ROOT}/.mock-bin" "${ROOT}/.mock-state"
  fi

  return "${status}"
}

if [[ "${1:-}" == "verify" ]]; then
  shift
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --keep-artifacts)
        VERIFY_KEEP_ARTIFACTS=1
        shift
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "[verify] unknown option: $1" >&2
        usage >&2
        exit 1
        ;;
    esac
  done
  run_verify_with_cleanup
  exit 0
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend)
      BACKEND="$2"
      shift 2
      ;;
    --host)
      HOST="$2"
      shift 2
      ;;
    --port)
      PORT="$2"
      shift 2
      ;;
    --store-root)
      STORE_ROOT="$2"
      shift 2
      ;;
    --core-number)
      CORE_NUMBER="$2"
      shift 2
      ;;
    --smoke)
      RUN_SMOKE=1
      shift
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

if [[ "${BACKEND}" != "eloqstore" && "${BACKEND}" != "inmemory" ]]; then
  echo "Invalid backend: ${BACKEND}" >&2
  exit 1
fi

if [[ ! -x "${PGMEMD_BIN}" ]]; then
  echo "Missing executable: ${PGMEMD_BIN}" >&2
  echo "Run scripts/install.sh first." >&2
  exit 1
fi

CHECK_HOST="${HOST}"
if [[ "${CHECK_HOST}" == "0.0.0.0" ]]; then
  CHECK_HOST="127.0.0.1"
fi

mkdir -p "${ROOT}/.pgmem"
mkdir -p "${STORE_ROOT}"

"${PGMEMD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --store-backend "${BACKEND}" \
  --store-root "${STORE_ROOT}" \
  --core-number "${CORE_NUMBER}" &
PID=$!

cleanup() {
  kill "${PID}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "[start] pgmemd pid=${PID}"

await_health() {
  local tries=50
  local i
  for ((i=0; i<tries; ++i)); do
    if ! kill -0 "${PID}" >/dev/null 2>&1; then
      return 1
    fi
    if curl -fsS -m 2 "http://${CHECK_HOST}:${PORT}/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

port_owned_by_pid() {
  if ! command -v ss >/dev/null 2>&1; then
    return 0
  fi
  ss -ltnp "( sport = :${PORT} )" 2>/dev/null | grep -q "pid=${PID},"
}

if ! await_health; then
  if ! kill -0 "${PID}" >/dev/null 2>&1; then
    echo "[start] pgmemd exited before becoming healthy" >&2
  else
    echo "[start] health check failed on http://${CHECK_HOST}:${PORT}/health" >&2
  fi
  wait "${PID}" || true
  exit 1
fi

if ! kill -0 "${PID}" >/dev/null 2>&1; then
  echo "[start] pgmemd exited unexpectedly after health check" >&2
  wait "${PID}" || true
  exit 1
fi

if ! port_owned_by_pid; then
  echo "[start] port ${PORT} is served by another process, not the newly started pgmemd (pid=${PID})" >&2
  wait "${PID}" || true
  exit 1
fi

STATS_JSON="$(curl -fsS -m 10 "http://${CHECK_HOST}:${PORT}/mcp" -X POST -H 'Content-Type: application/json' -d '{
  "id": 1,
  "method": "memory.stats",
  "params": {"workspace_id": "startup", "window": "5m"}
}')"

python3 - <<'PY' "${STATS_JSON}"
import json
import sys

obj = json.loads(sys.argv[1])
result = obj.get("result", {})
print("[start] effective_backend=" + str(result.get("effective_backend", "")))
print("[start] write_ack_mode=" + str(result.get("write_ack_mode", "")))
PY

if [[ "${RUN_SMOKE}" -eq 1 ]]; then
  run_smoke_scenario "http://${CHECK_HOST}:${PORT}" "startup" "s1"
  echo "[start] smoke finished"
  kill -TERM "${PID}" >/dev/null 2>&1 || true
  wait "${PID}" || true
  trap - EXIT
  exit 0
fi

echo "[start] service ready at http://${HOST}:${PORT}/mcp"
echo "[start] press Ctrl+C to stop"
wait "${PID}"
