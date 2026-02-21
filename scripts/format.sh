#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

CLANG_FORMAT_BIN="clang-format-18"
CLANG_FORMAT_PINNED="18.1.3"
MODE="write"

usage() {
  cat <<USAGE
Usage:
  scripts/format.sh [options]

Options:
  --check                 Check only; exit non-zero when formatting is needed
  --write                 Apply formatting in-place (default)
  --help, -h              Show this help

Notes:
  - Uses repo .clang-format via "-style=file"
  - Requires clang-format version ${CLANG_FORMAT_PINNED}
  - Always skips third-party paths (third_party/)
  - Fixed target paths: include src tests proto
USAGE
}

extract_first_version_triplet() {
  local text="$1"
  echo "${text}" | grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n 1
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
  if ! command -v "${CLANG_FORMAT_BIN}" >/dev/null 2>&1; then
    echo "[format] clang-format not found: ${CLANG_FORMAT_BIN}" >&2
    return 1
  fi

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
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "[format] unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if ! verify_clang_format_version; then
  exit 1
fi

FILES=()
while IFS= read -r -d '' file; do
  FILES+=("${file}")
done < <(
  find include src tests proto \
    -type f \
    \( -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o -name '*.proto' \) \
    ! -path '*/third_party/*' \
    -print0 | sort -z
)

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "[format] no files matched"
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
