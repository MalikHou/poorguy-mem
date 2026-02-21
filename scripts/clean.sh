#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<USAGE
Usage:
  scripts/clean.sh

Options:
  --help, -h            Show this help

Behavior:
  - Clean runtime/build artifacts under the repository
  - Reset and clean third_party (including recursive submodules)
USAGE
}

if [[ $# -gt 0 ]]; then
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "[clean] unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
fi

if ! git -C "${ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "[clean] repository is not a valid git worktree: ${ROOT}" >&2
  exit 1
fi

if [[ ! -d "${ROOT}/third_party" ]]; then
  echo "[clean] missing required directory: ${ROOT}/third_party" >&2
  exit 1
fi

if ! git -C "${ROOT}" submodule status --recursive >/dev/null 2>&1; then
  echo "[clean] failed to inspect git submodules; third_party reset would be unsafe" >&2
  exit 1
fi

cleanup_paths=(
  "${ROOT}/.pgmem"
  "${ROOT}/.mock-bin"
  "${ROOT}/.mock-state"
  "${ROOT}/build"
  "${ROOT}/build_durable"
  "${ROOT}/compile_commands.json"
)

echo "[clean] removing local artifacts:"
for p in "${cleanup_paths[@]}"; do
  echo "  - ${p}"
  rm -rf "${p}"
done

echo "[clean] resetting third_party in superproject"
git -C "${ROOT}" checkout -- third_party
git -C "${ROOT}" clean -fd -- third_party

echo "[clean] cleaning third_party submodules recursively"
git -C "${ROOT}" submodule foreach --recursive '
  case "$displaypath" in
    third_party|third_party/*)
      git reset --hard >/dev/null
      git clean -fd >/dev/null
      ;;
  esac
'

echo "[clean] syncing and updating submodules recursively"
git -C "${ROOT}" submodule sync --recursive
git -C "${ROOT}" submodule update --init --recursive

echo "[clean] done"
