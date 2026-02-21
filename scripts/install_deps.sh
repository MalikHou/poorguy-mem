#!/usr/bin/env bash
set -euo pipefail

NPROC="$(nproc || echo 4)"
PYTHON_BIN="python3"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERIFY_ONLY=0
CLANG_FORMAT_PINNED="18.1.3"

if command -v python3.11 >/dev/null 2>&1; then
  PYTHON_BIN="python3.11"
elif command -v python3 >/dev/null 2>&1; then
  PYTHON_BIN="python3"
fi

usage() {
  cat <<USAGE
Usage:
  scripts/install_deps.sh [options]

Options:
  --verify-only      Only verify dependency probes, do not install packages
  --help, -h         Show this help

Notes:
  This script normally runs:
    third_party/eloqstore/scripts/install_dependency_ubuntu2404.sh
  and then applies additional required installs/checks for poorguy-mem.
  Python selection order: python3.11 first, fallback to python3.
  Installs clang-format-18 by default and requires clang-format ${CLANG_FORMAT_PINNED} exactly.
USAGE
}

run_root() {
  if [[ "$(id -u)" -eq 0 ]]; then
    "$@"
  else
    sudo "$@"
  fi
}

has_deadsnakes_ppa() {
  grep -Rhs "ppa.launchpadcontent.net/deadsnakes/ppa" \
    /etc/apt/sources.list /etc/apt/sources.list.d/*.list 2>/dev/null | grep -q deadsnakes
}

ensure_stable_python() {
  if command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    return
  fi

  run_root apt-get install -y software-properties-common
  if ! has_deadsnakes_ppa; then
    run_root add-apt-repository -y ppa:deadsnakes/ppa
    run_root apt-get update
  fi

  run_root apt-get install -y \
    python3.11 \
    python3.11-dev \
    python3.11-venv \
    python3.11-distutils

  if command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    return
  fi

  if command -v python3.11 >/dev/null 2>&1; then
    PYTHON_BIN="python3.11"
    return
  fi

  echo "Required Python runtime is unavailable after installation (python3.11/python3)." >&2
  exit 1
}

awssdk_config_path() {
  local candidates=(
    /usr/lib/x86_64-linux-gnu/cmake/AWSSDK/AWSSDKConfig.cmake
    /usr/lib/cmake/AWSSDK/AWSSDKConfig.cmake
    /usr/local/lib/cmake/AWSSDK/AWSSDKConfig.cmake
    /usr/local/lib64/cmake/AWSSDK/AWSSDKConfig.cmake
  )
  local p
  for p in "${candidates[@]}"; do
    if [[ -f "$p" ]]; then
      echo "$p"
      return 0
    fi
  done
  return 1
}

has_awssdk_config() {
  awssdk_config_path >/dev/null 2>&1
}

has_butil_time_h() {
  [[ -f /usr/include/butil/time.h ]] || [[ -f /usr/local/include/butil/time.h ]]
}

has_brpc_server_h() {
  [[ -f /usr/include/brpc/server.h ]] || [[ -f /usr/local/include/brpc/server.h ]]
}

has_ldconfig_lib() {
  local lib="$1"
  ldconfig -p 2>/dev/null | grep -q "$lib"
}

extract_first_version_triplet() {
  local text="$1"
  echo "$text" | grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n 1
}

clang_format_version_text() {
  local bin="${1:-clang-format-18}"
  if ! command -v "$bin" >/dev/null 2>&1; then
    return 1
  fi
  "$bin" --version 2>/dev/null || return 1
}

clang_format_is_pinned() {
  local version="$1"
  local major minor patch
  major="${version%%.*}"
  minor="${version#*.}"
  patch="${minor#*.}"
  minor="${minor%%.*}"
  patch="${patch%%.*}"

  if [[ -z "$major" || -z "$minor" || -z "$patch" ]]; then
    return 1
  fi

  [[ "${major}.${minor}.${patch}" == "${CLANG_FORMAT_PINNED}" ]]
}

ensure_clang_format() {
  run_root apt-get install -y clang-format-18

  if ! command -v clang-format-18 >/dev/null 2>&1; then
    echo "clang-format-18 installation failed" >&2
    exit 1
  fi

  if ! command -v clang-format >/dev/null 2>&1; then
    run_root ln -sf "$(command -v clang-format-18)" /usr/local/bin/clang-format
  fi

  local cf_text=""
  local cf_triplet=""
  cf_text="$(clang_format_version_text "clang-format-18" || true)"
  cf_triplet="$(extract_first_version_triplet "$cf_text" || true)"
  if [[ -z "$cf_triplet" ]] || ! clang_format_is_pinned "$cf_triplet"; then
    echo "clang-format pin check failed: expected ${CLANG_FORMAT_PINNED}, got: ${cf_text:-unknown}" >&2
    exit 1
  fi
}

awssdk_find_package_probe() {
  local probe_root="/tmp/pgmem-awssdk-probe"
  rm -rf "$probe_root"
  mkdir -p "$probe_root"

  cat > "$probe_root/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(pgmem_awssdk_probe LANGUAGES CXX)
find_package(AWSSDK REQUIRED COMPONENTS s3)
CMAKE

  if cmake -S "$probe_root" -B "$probe_root/build" >/tmp/pgmem-awssdk-probe.log 2>&1; then
    rm -rf "$probe_root"
    return 0
  fi

  rm -rf "$probe_root"
  return 1
}

brpc_runtime_capability_probe() {
  local probe_root="/tmp/pgmem-brpc-probe"
  local cxx="${CXX:-c++}"

  rm -rf "$probe_root"
  mkdir -p "$probe_root"

  cat > "$probe_root/main.cpp" <<'CPP'
#include <cstdint>

#include <gflags/gflags_declare.h>
#include <brpc/server.h>

namespace brpc {
DECLARE_int32(event_dispatcher_num);
}

DECLARE_bool(use_io_uring);

int main() {
  brpc::FLAGS_event_dispatcher_num = 2;
  FLAGS_use_io_uring = false;
  return brpc::FLAGS_event_dispatcher_num == 2 ? 0 : 1;
}
CPP

  if "$cxx" -std=c++17 "$probe_root/main.cpp" -o "$probe_root/probe" \
      -lbrpc -lgflags -lpthread >/tmp/pgmem-brpc-probe.log 2>&1; then
    if "$probe_root/probe" >>/tmp/pgmem-brpc-probe.log 2>&1; then
      rm -rf "$probe_root"
      return 0
    fi
  fi

  rm -rf "$probe_root"
  return 1
}

print_probe_row() {
  local name="$1"
  local status="$2"
  local details="$3"
  printf '%-40s %-8s %s\n' "$name" "$status" "$details"
}

run_dependency_probes() {
  local failures=0

  echo
  echo "Dependency probe results:"

  local awssdk_path=""
  if awssdk_path="$(awssdk_config_path 2>/dev/null)"; then
    print_probe_row "AWSSDKConfig.cmake" "OK" "$awssdk_path"
  else
    print_probe_row "AWSSDKConfig.cmake" "FAIL" "missing"
    failures=$((failures + 1))
  fi

  if has_ldconfig_lib "libaws-cpp-sdk-s3.so"; then
    print_probe_row "ldconfig: libaws-cpp-sdk-s3.so" "OK" "discoverable"
  else
    print_probe_row "ldconfig: libaws-cpp-sdk-s3.so" "FAIL" "not found"
    failures=$((failures + 1))
  fi

  if has_ldconfig_lib "libaws-cpp-sdk-core.so"; then
    print_probe_row "ldconfig: libaws-cpp-sdk-core.so" "OK" "discoverable"
  else
    print_probe_row "ldconfig: libaws-cpp-sdk-core.so" "FAIL" "not found"
    failures=$((failures + 1))
  fi

  if awssdk_find_package_probe; then
    print_probe_row "cmake find_package(AWSSDK s3)" "OK" "probe passed"
  else
    print_probe_row "cmake find_package(AWSSDK s3)" "FAIL" "probe failed"
    echo "  probe log: /tmp/pgmem-awssdk-probe.log"
    failures=$((failures + 1))
  fi

  if has_butil_time_h; then
    print_probe_row "butil/time.h" "OK" "discoverable"
  else
    print_probe_row "butil/time.h" "FAIL" "missing"
    failures=$((failures + 1))
  fi

  if has_brpc_server_h; then
    print_probe_row "brpc/server.h" "OK" "discoverable"
  else
    print_probe_row "brpc/server.h" "FAIL" "missing"
    failures=$((failures + 1))
  fi

  if has_ldconfig_lib "libbrpc.so"; then
    print_probe_row "ldconfig: libbrpc.so" "OK" "discoverable"
  else
    print_probe_row "ldconfig: libbrpc.so" "FAIL" "not found"
    failures=$((failures + 1))
  fi

  if brpc_runtime_capability_probe; then
    print_probe_row "brpc flags probe (event_dispatcher/use_io_uring)" "OK" "probe passed"
  else
    print_probe_row "brpc flags probe (event_dispatcher/use_io_uring)" "FAIL" "probe failed"
    echo "  probe log: /tmp/pgmem-brpc-probe.log"
    failures=$((failures + 1))
  fi

  local cf_text=""
  if cf_text="$(clang_format_version_text "clang-format-18" 2>/dev/null)"; then
    local cf_triplet=""
    cf_triplet="$(extract_first_version_triplet "$cf_text" || true)"
    if [[ -n "$cf_triplet" ]] && clang_format_is_pinned "$cf_triplet"; then
      print_probe_row "clang-format-18 (pinned ${CLANG_FORMAT_PINNED})" "OK" "$cf_text"
    else
      print_probe_row "clang-format-18 (pinned ${CLANG_FORMAT_PINNED})" "FAIL" "$cf_text"
      failures=$((failures + 1))
    fi
  else
    print_probe_row "clang-format-18 (pinned ${CLANG_FORMAT_PINNED})" "FAIL" "missing"
    failures=$((failures + 1))
  fi

  if [[ "$failures" -gt 0 ]]; then
    echo
    echo "Dependency probes failed (${failures})." >&2
    echo "Fix suggestions:" >&2
    echo "  1) scripts/install_deps.sh" >&2
    echo "  2) Ensure brpc/gflags/AWS SDK shared libs are in ldconfig search path and run: sudo ldconfig" >&2
    echo "  3) Re-run verify-only check: scripts/install_deps.sh --verify-only" >&2
    return 1
  fi

  return 0
}

install_eloqdata_brpc() {
  local workdir="/tmp/eloq-brpc-build"
  rm -rf "$workdir"
  git clone --depth 1 https://github.com/eloqdata/brpc.git "$workdir"
  cmake -S "$workdir" -B "$workdir/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_GLOG=ON \
    -DIO_URING_ENABLED=ON \
    -DBUILD_SHARED_LIBS=ON
  cmake --build "$workdir/build" -j"$NPROC"

  if [[ -d "$workdir/build/output/include" ]]; then
    run_root mkdir -p /usr/local/include
    run_root cp -r "$workdir/build/output/include/"* /usr/local/include/
  fi
  if [[ -d "$workdir/build/output/lib" ]]; then
    run_root mkdir -p /usr/local/lib
    run_root cp -r "$workdir/build/output/lib/"* /usr/local/lib/
  fi

  run_root ldconfig
  rm -rf "$workdir"
}

install_awssdk_from_source() {
  local workdir="/tmp/aws-sdk-cpp-build"
  rm -rf "$workdir"
  git clone --depth 1 --branch 1.11.446 --recurse-submodules --shallow-submodules \
    https://github.com/aws/aws-sdk-cpp.git "$workdir"
  git -C "$workdir" submodule sync --recursive
  git -C "$workdir" submodule update --init --recursive
  cmake -S "$workdir" -B "$workdir/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTING=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_ONLY="s3"
  cmake --build "$workdir/build" -j"$NPROC"
  run_root cmake --install "$workdir/build"
  run_root ldconfig
  rm -rf "$workdir"
}

run_eloqstore_official_dependency_script() {
  local official_script="$REPO_ROOT/third_party/eloqstore/scripts/install_dependency_ubuntu2404.sh"
  local shim_dir

  if [[ ! -f "$official_script" ]]; then
    echo "EloqStore dependency script not found: $official_script" >&2
    return 1
  fi

  if [[ ! -x "$official_script" ]]; then
    chmod +x "$official_script"
  fi

  shim_dir="$(mktemp -d /tmp/pgmem-python-shim.XXXXXX)"
  ln -s "$(command -v "$PYTHON_BIN")" "$shim_dir/python3"
  cat > "$shim_dir/pip3" <<EOF2
#!/usr/bin/env bash
exec "$(command -v "$PYTHON_BIN")" -m pip "\$@"
EOF2
  chmod +x "$shim_dir/pip3"

  "$PYTHON_BIN" -m ensurepip --upgrade >/dev/null 2>&1 || true
  "$PYTHON_BIN" -m pip install --upgrade --no-cache-dir pip setuptools wheel >/dev/null 2>&1 || true

  echo "Running EloqStore official dependency script with python shim: $PYTHON_BIN"
  if PATH="$shim_dir:$PATH" bash "$official_script"; then
    rm -rf "$shim_dir"
    return 0
  fi

  rm -rf "$shim_dir"
  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --verify-only)
      VERIFY_ONLY=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ "$VERIFY_ONLY" -eq 1 ]]; then
  echo "Running dependency verification only (no installs)..."
  run_dependency_probes
  echo "Dependency verification completed"
  exit 0
fi

run_root apt-get update
run_root apt-get install -y \
  build-essential \
  cmake \
  make \
  sudo \
  git \
  pkg-config \
  curl \
  ca-certificates \
  software-properties-common \
  libboost-dev \
  libboost-context-dev

ensure_stable_python
ensure_clang_format
echo "Using stable Python: $("$PYTHON_BIN" --version)"

if ! run_eloqstore_official_dependency_script; then
  echo "Warning: EloqStore official dependency script failed; applying fallback installs..." >&2
fi

run_root apt-get install -y \
  libgoogle-glog-dev \
  libgflags-dev \
  libjsoncpp-dev \
  libprotobuf-dev \
  protobuf-compiler \
  libprotoc-dev \
  libcurl4-openssl-dev \
  liburing-dev \
  libzstd-dev \
  libleveldb-dev \
  libsnappy-dev \
  zlib1g-dev \
  libssl-dev

if ! has_brpc_server_h || ! has_ldconfig_lib "libbrpc.so" || ! brpc_runtime_capability_probe; then
  echo "brpc runtime capability missing/incompatible; installing eloqdata/brpc..."
  install_eloqdata_brpc
fi

if ! has_awssdk_config; then
  if apt-cache show libaws-sdk-cpp-dev >/dev/null 2>&1; then
    run_root apt-get install -y libaws-sdk-cpp-dev
  fi
fi

if ! has_awssdk_config; then
  echo "AWSSDK CMake package not found from apt; building aws-sdk-cpp (S3 only) from source..."
  install_awssdk_from_source
fi

run_dependency_probes

echo "Dependency installation completed"
