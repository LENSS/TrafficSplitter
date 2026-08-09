#!/usr/bin/env bash
set -euo pipefail

echo "============================================================"
echo " TrafficSplitter: Install eBPF build dependencies and bpftool"
echo "============================================================"

sudo apt update

sudo apt install -y \
    clang \
    llvm \
    gcc \
    make \
    git \
    libbpf-dev \
    libelf-dev \
    zlib1g-dev \
    libc6-dev-i386

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TOOLS_DIR="${ROOT_DIR}/tools"
BPFTOOL_DIR="${TOOLS_DIR}/bpftool"

mkdir -p "${TOOLS_DIR}"

if [[ ! -d "${BPFTOOL_DIR}/.git" ]]; then
    echo "[INFO] Cloning bpftool..."
    git clone --recurse-submodules \
        https://github.com/libbpf/bpftool.git \
        "${BPFTOOL_DIR}"
else
    echo "[INFO] bpftool repository already exists."
fi

cd "${BPFTOOL_DIR}/src"

echo "[INFO] Building bpftool..."
make -j"$(nproc)"

echo "[INFO] Installing bpftool..."
sudo make install

echo
echo "[INFO] Installed bpftool:"
command -v bpftool
bpftool version

echo
echo "[PASS] bpftool installation completed."
