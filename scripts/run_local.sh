#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT}/build"

if [[ ! -x "${BUILD_DIR}/pgmemd" || ! -x "${BUILD_DIR}/pgmem-syncd" ]]; then
  echo "Build binaries first: cmake -S . -B build && cmake --build build -j" >&2
  exit 1
fi

mkdir -p "${ROOT}/.pgmem"

"${BUILD_DIR}/pgmem-syncd" --host 127.0.0.1 --port 9765 --store-root "${ROOT}/.pgmem/sync-store" --node-id syncd &
SYNC_PID=$!

"${BUILD_DIR}/pgmemd" --host 127.0.0.1 --port 8765 --store-root "${ROOT}/.pgmem/store" --node-id local --sync-host 127.0.0.1 --sync-port 9765 &
MEM_PID=$!

cleanup() {
  kill "$MEM_PID" "$SYNC_PID" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "pgmem-syncd pid=${SYNC_PID}"
echo "pgmemd pid=${MEM_PID}"

echo "Press Ctrl+C to stop"
wait
