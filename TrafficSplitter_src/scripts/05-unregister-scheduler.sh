#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage:"
    echo "  $0 saflo"
    echo "  $0 bwr"
    exit 1
fi

SCHEDULER="$1"

case "${SCHEDULER}" in
    saflo|bwr)
        ;;
    *)
        echo "[ERROR] Unknown scheduler: ${SCHEDULER}"
        echo "        Supported schedulers: saflo, bwr"
        exit 1
        ;;
esac

echo "============================================================"
echo " Unregister MPTCP eBPF Scheduler: ${SCHEDULER}"
echo "============================================================"

sudo bpftool struct_ops unregister name "${SCHEDULER}"

echo
echo "[PASS] ${SCHEDULER} unregistered."

echo
echo "[INFO] Remaining struct_ops:"
sudo bpftool struct_ops show || true
