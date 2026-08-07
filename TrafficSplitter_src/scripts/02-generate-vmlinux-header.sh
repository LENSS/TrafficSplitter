#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

SCHEDULER_DIR="${ROOT_DIR}/Saflo_scheduler"
HEADER_DIR="${SCHEDULER_DIR}/headers"
VMLINUX_HEADER="${HEADER_DIR}/vmlinux.h"

echo "============================================================"
echo " TrafficSplitter: Generate vmlinux.h"
echo "============================================================"

echo "[INFO] Running kernel: $(uname -r)"

if ! command -v bpftool >/dev/null 2>&1; then
    echo "[ERROR] bpftool is not installed."
    echo "        Run scripts/01-install-bpf-tools.sh first."
    exit 1
fi

if [[ ! -e /sys/kernel/btf/vmlinux ]]; then
    echo "[ERROR] /sys/kernel/btf/vmlinux does not exist."
    echo "        The running kernel does not expose the required BTF."
    exit 1
fi

mkdir -p "${HEADER_DIR}"

echo "[INFO] Generating:"
echo "       ${VMLINUX_HEADER}"

sudo bpftool btf dump \
    file /sys/kernel/btf/vmlinux \
    format c > "${VMLINUX_HEADER}"

echo
echo "[PASS] Generated vmlinux.h:"
ls -lh "${VMLINUX_HEADER}"
