#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CLEAN_RUNTIME=1
CLEAN_BUILD=0
CLEAN_MCP_CONFIG=0
DRY_RUN=0

usage() {
  cat <<USAGE
Usage:
  scripts/clean.sh [options]

Options:
  --runtime-only        Clean runtime artifacts only (default behavior)
  --build               Also remove build artifacts (build/, compile_commands.json)
  --mcp-config          Also remove local MCP client config (.cursor/mcp.json, .cursor/rules/poorguy-mem.mdc, .mcp.json)
  --all                 Clean runtime + build
  --dry-run             Show targets without deleting
  --help, -h            Show this help
USAGE
}

cleanup_paths=()

add_path_if_exists() {
  local p="$1"
  if [[ -e "${p}" || -L "${p}" ]]; then
    cleanup_paths+=("${p}")
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runtime-only)
      CLEAN_RUNTIME=1
      CLEAN_BUILD=0
      shift
      ;;
    --build)
      CLEAN_BUILD=1
      shift
      ;;
    --mcp-config)
      CLEAN_MCP_CONFIG=1
      shift
      ;;
    --all)
      CLEAN_RUNTIME=1
      CLEAN_BUILD=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
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

if [[ "${CLEAN_RUNTIME}" -eq 1 ]]; then
  add_path_if_exists "${ROOT}/.pgmem"
  add_path_if_exists "${ROOT}/.mock-bin"
  add_path_if_exists "${ROOT}/.mock-state"
fi

if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
  add_path_if_exists "${ROOT}/build"
  add_path_if_exists "${ROOT}/compile_commands.json"
fi

if [[ "${CLEAN_MCP_CONFIG}" -eq 1 ]]; then
  add_path_if_exists "${ROOT}/.cursor/mcp.json"
  add_path_if_exists "${ROOT}/.cursor/rules/poorguy-mem.mdc"
  add_path_if_exists "${ROOT}/.mcp.json"
fi

if [[ ${#cleanup_paths[@]} -eq 0 ]]; then
  echo "[clean] nothing to clean"
  exit 0
fi

echo "[clean] targets:"
for p in "${cleanup_paths[@]}"; do
  echo "  - ${p}"
done

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "[clean] dry-run enabled, no files removed"
  exit 0
fi

for p in "${cleanup_paths[@]}"; do
  rm -rf "${p}"
done

echo "[clean] done"
