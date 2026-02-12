#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
BACKEND="eloqstore"
BUILD_TYPE="Release"
WORKSPACE="${ROOT}"
MCP_URL="http://127.0.0.1:8765/mcp"
MCP_NAME="poorguy-mem"
RUN_TESTS=1
RUN_DEPLOY=1
RUN_DEPS=1
CORE_NUMBER="4"
ALLOW_CODEX_MISS=0
ALLOW_CLAUDE_MISS=0
ALLOW_RUNTIME_MISS=0
AUTO_INSTALL_CLI="true"
FORCE_LOGIN="true"
MAX_RETRIES="3"
TEMP_PGMEMD_PID=""
TEMP_PGMEMD_LOG=""

usage() {
  cat <<USAGE
Usage:
  scripts/install.sh [options]

Options:
  --backend <eloqstore|inmemory>   Build backend (default: eloqstore)
  --build-type <type>              CMake build type (default: Release)
  --workspace <path>               Workspace for Cursor MCP deployment (default: repo root)
  --url <mcp-url>                  MCP URL for deployment (default: http://127.0.0.1:8765/mcp)
  --name <mcp-name>                MCP server name (default: poorguy-mem)
  --core-number <N>                Runtime core-number used in generated systemd unit (default: 4)
  --auto-install-cli <bool>        Auto-install Node/Codex/Claude CLIs when missing (default: true)
  --force-login <bool>             Force interactive Codex/Claude login when needed (default: true)
  --max-retries <N>                Retry count for deploy/install/login steps (default: 3)
  --skip-deps                      Skip dependency install stage
  --skip-tests                     Skip ctest stage
  --skip-deploy                    Skip MCP deployment stage
  --allow-codex-miss               Allow Codex MCP registration/verification to fail
  --allow-claude-miss              Allow Claude MCP registration/verification to fail
  --allow-runtime-miss             Allow runtime MCP handshake verification to fail
  --allow-mcp-miss                 Allow both Codex and Claude MCP registration misses
  --help, -h                       Show this help
USAGE
}

normalize_bool() {
  echo "$1" | tr '[:upper:]' '[:lower:]'
}

parse_bool() {
  case "$(normalize_bool "$1")" in
    1|true|yes|on) echo "true" ;;
    0|false|no|off) echo "false" ;;
    *) echo "invalid" ;;
  esac
}

runtime_base_url() {
  local url="$1"
  if [[ "$url" == */mcp ]]; then
    echo "${url%/mcp}"
  else
    echo "$url"
  fi
}

cleanup_temp_pgmemd() {
  if [[ -n "${TEMP_PGMEMD_PID}" ]]; then
    echo "[install] stopping temporary pgmemd (pid=${TEMP_PGMEMD_PID})"
    kill "${TEMP_PGMEMD_PID}" >/dev/null 2>&1 || true
    wait "${TEMP_PGMEMD_PID}" >/dev/null 2>&1 || true
    TEMP_PGMEMD_PID=""
  fi
}

wait_for_health() {
  local url="$1"
  local wait_seconds="$2"
  local i
  for ((i=0; i<wait_seconds*10; ++i)); do
    if curl -fsS "${url}" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
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
    --workspace)
      WORKSPACE="$2"
      shift 2
      ;;
    --url)
      MCP_URL="$2"
      shift 2
      ;;
    --name)
      MCP_NAME="$2"
      shift 2
      ;;
    --core-number)
      CORE_NUMBER="$2"
      shift 2
      ;;
    --auto-install-cli)
      AUTO_INSTALL_CLI="$(parse_bool "$2")"
      shift 2
      ;;
    --force-login)
      FORCE_LOGIN="$(parse_bool "$2")"
      shift 2
      ;;
    --max-retries)
      MAX_RETRIES="$2"
      shift 2
      ;;
    --skip-deps)
      RUN_DEPS=0
      shift
      ;;
    --skip-tests)
      RUN_TESTS=0
      shift
      ;;
    --skip-deploy)
      RUN_DEPLOY=0
      shift
      ;;
    --allow-codex-miss)
      ALLOW_CODEX_MISS=1
      shift
      ;;
    --allow-claude-miss)
      ALLOW_CLAUDE_MISS=1
      shift
      ;;
    --allow-runtime-miss)
      ALLOW_RUNTIME_MISS=1
      shift
      ;;
    --allow-mcp-miss)
      ALLOW_CODEX_MISS=1
      ALLOW_CLAUDE_MISS=1
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

if [[ "${AUTO_INSTALL_CLI}" == "invalid" ]]; then
  echo "Invalid --auto-install-cli value" >&2
  exit 1
fi
if [[ "${FORCE_LOGIN}" == "invalid" ]]; then
  echo "Invalid --force-login value" >&2
  exit 1
fi
if [[ ! "${MAX_RETRIES}" =~ ^[0-9]+$ ]] || [[ "${MAX_RETRIES}" -lt 1 ]]; then
  echo "Invalid --max-retries value: ${MAX_RETRIES}" >&2
  exit 1
fi

if [[ "${RUN_DEPS}" -eq 1 ]]; then
  echo "[install] stage 1/4: install dependencies"
  "${ROOT}/scripts/install_deps.sh"
else
  echo "[install] stage 1/4: skipped dependencies"
fi

echo "[install] stage 2/4: configure + build"
cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_TESTING=ON \
  -DPGMEM_STORE_BACKEND="${BACKEND}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"
ln -snf "${BUILD_DIR}/compile_commands.json" "${ROOT}/compile_commands.json"

if [[ "${RUN_TESTS}" -eq 1 ]]; then
  echo "[install] stage 3/4: run tests"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
else
  echo "[install] stage 3/4: skipped tests"
fi

if [[ "${RUN_DEPLOY}" -eq 1 ]]; then
  echo "[install] stage 4/4: deploy MCP config"
  STRICT_CODEX="true"
  STRICT_CLAUDE="true"
  STRICT_RUNTIME="true"
  if [[ "${ALLOW_CODEX_MISS}" -eq 1 ]]; then
    STRICT_CODEX="false"
  fi
  if [[ "${ALLOW_CLAUDE_MISS}" -eq 1 ]]; then
    STRICT_CLAUDE="false"
  fi
  if [[ "${ALLOW_RUNTIME_MISS}" -eq 1 ]]; then
    STRICT_RUNTIME="false"
  fi

  MCP_BASE_URL="$(runtime_base_url "${MCP_URL}")"
  HEALTH_URL="${MCP_BASE_URL}/health"
  readarray -t HOST_PORT < <(python3 - "${MCP_BASE_URL}" <<'PY'
from urllib.parse import urlparse
import sys

u = urlparse(sys.argv[1])
host = u.hostname or "127.0.0.1"
if u.port is not None:
    port = u.port
else:
    port = 443 if u.scheme == "https" else 80
print(host)
print(port)
PY
)
  MCP_HOST="${HOST_PORT[0]}"
  MCP_PORT="${HOST_PORT[1]}"

  trap cleanup_temp_pgmemd EXIT
  if ! curl -fsS "${HEALTH_URL}" >/dev/null 2>&1; then
    mkdir -p "${ROOT}/.pgmem"
    TEMP_PGMEMD_LOG="${ROOT}/.pgmem/install-temp-pgmemd.log"
    echo "[install] no service detected at ${HEALTH_URL}, starting temporary pgmemd"
    "${BUILD_DIR}/pgmemd" \
      --host "${MCP_HOST}" \
      --port "${MCP_PORT}" \
      --store-backend "${BACKEND}" \
      >"${TEMP_PGMEMD_LOG}" 2>&1 &
    TEMP_PGMEMD_PID="$!"
    if ! wait_for_health "${HEALTH_URL}" 30; then
      echo "[install] temporary pgmemd failed to become healthy" >&2
      if [[ -f "${TEMP_PGMEMD_LOG}" ]]; then
        tail -n 120 "${TEMP_PGMEMD_LOG}" >&2 || true
      fi
      exit 1
    fi
  fi

  "${ROOT}/scripts/deploy_mcp.sh" \
    --workspace "${WORKSPACE}" \
    --url "${MCP_URL}" \
    --name "${MCP_NAME}" \
    --strict-codex "${STRICT_CODEX}" \
    --strict-claude "${STRICT_CLAUDE}" \
    --strict-runtime "${STRICT_RUNTIME}" \
    --auto-install-cli "${AUTO_INSTALL_CLI}" \
    --force-login "${FORCE_LOGIN}" \
    --max-retries "${MAX_RETRIES}"

  "${ROOT}/scripts/verify_clients.sh" \
    --workspace "${WORKSPACE}" \
    --url "${MCP_URL}" \
    --name "${MCP_NAME}" \
    --real \
    --strict-runtime "${STRICT_RUNTIME}" \
    --strict-codex "${STRICT_CODEX}" \
    --strict-claude "${STRICT_CLAUDE}"

  if [[ -x "${BUILD_DIR}/pgmem-install" ]]; then
    "${BUILD_DIR}/pgmem-install" \
      --workspace "${WORKSPACE}" \
      --url "${MCP_URL}" \
      --pgmemd-bin "${BUILD_DIR}/pgmemd" \
      --core-number "${CORE_NUMBER}" \
      --no-systemd >/dev/null 2>&1 || true
  fi
  cleanup_temp_pgmemd
  trap - EXIT
else
  echo "[install] stage 4/4: skipped deploy"
fi

echo "[install] done"
