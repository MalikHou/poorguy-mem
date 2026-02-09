#!/usr/bin/env bash
set -euo pipefail

WITH_ELOQSTORE=0

for arg in "$@"; do
  case "$arg" in
    --with-eloqstore)
      WITH_ELOQSTORE=1
      ;;
    *)
      echo "Unknown arg: $arg" >&2
      exit 1
      ;;
  esac
done

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  make \
  git \
  libboost-dev

if [[ "$WITH_ELOQSTORE" -eq 1 ]]; then
  if [[ ! -x third_party/eloqstore/scripts/install_dependency_ubuntu2404.sh ]]; then
    echo "EloqStore dependency script not found. Did you init submodules?" >&2
    exit 1
  fi
  sudo bash third_party/eloqstore/scripts/install_dependency_ubuntu2404.sh
fi

echo "Dependency installation completed"
