#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD_DIR="${ROOT_DIR}/Saflo_scheduler/build"

if [[ $# -ne 1 ]]; then
    echo "Usage:"
    echo "  $0 saflo"
    echo "  $0 bwr"
    exit 1
fi

SCHEDULER="$1"

case "${SCHEDULER}" in
    saflo)
        BPF_OBJECT="${BUILD_DIR}/mptcp_bpf_saflo.o"
        ;;
    bwr)
        BPF_OBJECT="${BUILD_DIR}/mptcp_bpf_bwr.o"
        ;;
    *)
        echo "[ERROR] Unknown scheduler: ${SCHEDULER}"
        echo "        Supported schedulers: saflo, bwr"
        exit 1
        ;;
esac

echo "============================================================"
echo " Register MPTCP eBPF Scheduler: ${SCHEDULER}"
echo "============================================================"

if [[ ! -f "${BPF_OBJECT}" ]]; then
    echo "[ERROR] BPF object not found:"
    echo "        ${BPF_OBJECT}"
    echo
    echo "Run scripts/03-build-schedulers.sh first."
    exit 1
fi

echo "[INFO] Running kernel: $(uname -r)"
echo "[INFO] Registering:"
echo "       ${BPF_OBJECT}"

sudo bpftool struct_ops register "${BPF_OBJECT}"

echo
echo "[PASS] Registration completed."

echo
echo "[INFO] Registered struct_ops:"
sudo bpftool struct_ops show
