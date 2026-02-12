#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PGMEMD_BIN="${ROOT}/build/pgmemd"
BACKEND="eloqstore"
HOST="127.0.0.1"
PORT="8765"
CORE_NUMBER="4"
ENABLE_IO_URING_NETWORK_ENGINE="false"
STORE_ROOT="${ROOT}/.pgmem/store"
RUN_SMOKE=0

usage() {
  cat <<USAGE
Usage:
  scripts/start.sh [options]

Options:
  --backend <eloqstore|inmemory>          Backend request (default: eloqstore)
  --host <ip>                             Bind host (default: 127.0.0.1)
  --port <N>                              Bind port (default: 8765)
  --store-root <path>                     Store root directory (default: ./.pgmem/store)
  --core-number <N>                       Runtime core-number (default: 4)
  --enable-io-uring-network-engine <bool> Enable brpc network io_uring (default: false)
  --smoke                                 Run smoke test then exit
  --help, -h                              Show this help
USAGE
}

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
    --enable-io-uring-network-engine)
      ENABLE_IO_URING_NETWORK_ENGINE="$2"
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

mkdir -p "${ROOT}/.pgmem"
mkdir -p "${STORE_ROOT}"

"${PGMEMD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --store-backend "${BACKEND}" \
  --store-root "${STORE_ROOT}" \
  --core-number "${CORE_NUMBER}" \
  --enable-io-uring-network-engine "${ENABLE_IO_URING_NETWORK_ENGINE}" &
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
    if curl -fsS "http://${HOST}:${PORT}/health" >/dev/null 2>&1; then
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
    echo "[start] health check failed on http://${HOST}:${PORT}/health" >&2
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

STATS_JSON="$(curl -fsS "http://${HOST}:${PORT}/mcp" -X POST -H 'Content-Type: application/json' -d '{
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
  "${ROOT}/scripts/verify.sh" smoke --url "http://${HOST}:${PORT}"
  echo "[start] smoke finished"
  kill -TERM "${PID}" >/dev/null 2>&1 || true
  wait "${PID}" || true
  trap - EXIT
  exit 0
fi

echo "[start] service ready at http://${HOST}:${PORT}/mcp"
echo "[start] press Ctrl+C to stop"
wait "${PID}"
