#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
BUILD_TYPE="Release"
BACKEND="eloqstore"
CUSTOM_SOURCES=""
BUILD_TESTING="ON"
CLEAN=0

usage() {
  cat <<USAGE
Usage:
  scripts/build.sh [options]

Options:
  --backend <eloqstore|inmemory|custom>  Storage backend (default: eloqstore)
  --build-dir <path>                     Build directory (default: <repo>/build)
  --build-type <Debug|Release|...>       CMake build type (default: Release)
  --custom-sources <a;b;c>               Required when backend=custom
  --without-tests                        Set BUILD_TESTING=OFF
  --clean                                Remove build dir before configure
  --help, -h                             Show this help

Notes:
  1) This script always generates compile_commands.json.
  2) It also links <repo>/compile_commands.json -> <build-dir>/compile_commands.json.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend)
      BACKEND="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-type)
      BUILD_TYPE="$2"
      shift 2
      ;;
    --custom-sources)
      CUSTOM_SOURCES="$2"
      shift 2
      ;;
    --without-tests)
      BUILD_TESTING="OFF"
      shift
      ;;
    --clean)
      CLEAN=1
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

if [[ "$BACKEND" != "eloqstore" && "$BACKEND" != "inmemory" && "$BACKEND" != "custom" ]]; then
  echo "Invalid backend: $BACKEND" >&2
  exit 1
fi

if [[ "$BACKEND" == "custom" && -z "$CUSTOM_SOURCES" ]]; then
  echo "--custom-sources is required when --backend custom" >&2
  exit 1
fi

if [[ "$CLEAN" -eq 1 ]]; then
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

CMAKE_ARGS=(
  -S "$ROOT"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DBUILD_TESTING="$BUILD_TESTING"
  -DPGMEM_STORE_BACKEND="$BACKEND"
)

if [[ "$BACKEND" == "custom" ]]; then
  CMAKE_ARGS+=("-DPGMEM_CUSTOM_STORE_SOURCES=$CUSTOM_SOURCES")
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" -j"$(nproc)"

if [[ "$BUILD_TESTING" == "ON" ]]; then
  ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

ln -snf "$BUILD_DIR/compile_commands.json" "$ROOT/compile_commands.json"

echo "Build done."
echo "Backend: $BACKEND"
echo "Build dir: $BUILD_DIR"
echo "compile_commands.json: $ROOT/compile_commands.json"
