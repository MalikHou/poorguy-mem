#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

CLANG_FORMAT_BIN="${CLANG_FORMAT_BIN:-clang-format-18}"
CLANG_FORMAT_PINNED="18.1.3"
MODE="write"
LIST_ONLY=0

TARGETS=()
DEFAULT_TARGETS=("include" "src" "tests" "proto")

usage() {
  cat <<USAGE
Usage:
  scripts/format.sh [options] [paths...]

Options:
  --check                 Check only; exit non-zero when formatting is needed
  --write                 Apply formatting in-place (default)
  --list-files            List candidate files only; do not format
  --clang-format-bin <p>  clang-format binary path (default: clang-format-18)
  --help, -h              Show this help

Notes:
  - Uses repo .clang-format via "-style=file"
  - Requires clang-format version ${CLANG_FORMAT_PINNED}
  - Always skips third-party paths (third_party/)
  - Default target paths: include src tests proto
USAGE
}

resolve_clang_format_bin() {
  command -v "${CLANG_FORMAT_BIN}" >/dev/null 2>&1
}

extract_first_version_triplet() {
  local text="$1"
  echo "$text" | grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n 1
}

clang_format_is_pinned() {
  local version="$1"
  local major minor patch
  major="${version%%.*}"
  minor="${version#*.}"
  patch="${minor#*.}"
  minor="${minor%%.*}"
  patch="${patch%%.*}"
  [[ -n "${major}" && -n "${minor}" && -n "${patch}" ]] || return 1
  [[ "${major}.${minor}.${patch}" == "${CLANG_FORMAT_PINNED}" ]]
}

verify_clang_format_version() {
  local version_text=""
  local triplet=""
  version_text="$("${CLANG_FORMAT_BIN}" --version 2>/dev/null || true)"
  triplet="$(extract_first_version_triplet "${version_text}" || true)"
  if [[ -z "${triplet}" ]] || ! clang_format_is_pinned "${triplet}"; then
    echo "[format] clang-format pin check failed: require ${CLANG_FORMAT_PINNED}, got: ${version_text:-unknown}" >&2
    return 1
  fi
  return 0
}

is_supported_file() {
  local p="$1"
  case "${p}" in
    *.h|*.hh|*.hpp|*.hxx|*.c|*.cc|*.cpp|*.cxx|*.proto) return 0 ;;
    *) return 1 ;;
  esac
}

is_third_party_path() {
  local p="$1"
  case "${p}" in
    third_party/*|*/third_party/*) return 0 ;;
    *) return 1 ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check)
      MODE="check"
      shift
      ;;
    --write)
      MODE="write"
      shift
      ;;
    --list-files)
      LIST_ONLY=1
      shift
      ;;
    --clang-format-bin)
      CLANG_FORMAT_BIN="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      TARGETS+=("$1")
      shift
      ;;
  esac
done

if ! resolve_clang_format_bin; then
  echo "[format] clang-format not found: ${CLANG_FORMAT_BIN}" >&2
  exit 1
fi

if ! verify_clang_format_version; then
  exit 1
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=("${DEFAULT_TARGETS[@]}")
fi

FILES=()

add_file_if_valid() {
  local file="$1"
  if [[ ! -f "${file}" ]]; then
    return 0
  fi
  if is_third_party_path "${file}"; then
    return 0
  fi
  if is_supported_file "${file}"; then
    FILES+=("${file}")
  fi
}

for target in "${TARGETS[@]}"; do
  if [[ ! -e "${target}" ]]; then
    continue
  fi

  if [[ -f "${target}" ]]; then
    add_file_if_valid "${target}"
    continue
  fi

  while IFS= read -r -d '' file; do
    add_file_if_valid "${file}"
  done < <(
    find "${target}" \
      -type f \
      \( -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o -name '*.proto' \) \
      ! -path '*/third_party/*' \
      -print0 | sort -z
  )
done

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "[format] no files matched"
  exit 0
fi

if [[ "${LIST_ONLY}" -eq 1 ]]; then
  printf '%s\n' "${FILES[@]}"
  exit 0
fi

changed=()

for file in "${FILES[@]}"; do
  tmp_file="$(mktemp)"
  "${CLANG_FORMAT_BIN}" -style=file "${file}" > "${tmp_file}"
  if ! cmp -s "${file}" "${tmp_file}"; then
    changed+=("${file}")
    if [[ "${MODE}" == "write" ]]; then
      cp "${tmp_file}" "${file}"
    fi
  fi
  rm -f "${tmp_file}"
done

if [[ "${MODE}" == "check" ]]; then
  if [[ ${#changed[@]} -gt 0 ]]; then
    echo "[format] formatting needed (${#changed[@]} files):"
    printf '  - %s\n' "${changed[@]}"
    exit 1
  fi
  echo "[format] check passed (${#FILES[@]} files)"
  exit 0
fi

echo "[format] formatted ${#changed[@]} files (scanned ${#FILES[@]})"
