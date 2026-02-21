#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
BACKEND="eloqstore"
BUILD_TYPE="Release"

usage() {
  cat <<USAGE
Usage:
  scripts/install.sh [options]

Options:
  --backend <eloqstore|inmemory>  Build backend (default: eloqstore)
  --build-type <type>             CMake build type (default: Release)
  --help, -h                      Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend)
      BACKEND="$2"
      shift 2
      ;;
    --build-type)
      BUILD_TYPE="$2"
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

if [[ "${BACKEND}" != "eloqstore" && "${BACKEND}" != "inmemory" ]]; then
  echo "Invalid backend: ${BACKEND}" >&2
  exit 1
fi

echo "[install] stage 1/3: install dependencies"
"${ROOT}/scripts/install_deps.sh"

echo "[install] stage 2/3: configure + build"
cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_TESTING=ON \
  -DPGMEM_STORE_BACKEND="${BACKEND}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"
ln -snf "${BUILD_DIR}/compile_commands.json" "${ROOT}/compile_commands.json"

echo "[install] stage 3/3: run tests"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "[install] done"
