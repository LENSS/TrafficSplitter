#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage:"
    echo "  $0 <scheduler>"
    echo
    echo "Supported schedulers:"
    echo "  default (BLEST)"
    echo "  bpf_saflo (Saflo)"
    echo "  bpf_ran (BWR)"
    exit 1
fi

SCHEDULER="$1"

case "${SCHEDULER}" in
    default|bpf_saflo|bpf_ran)
        ;;
    *)
        echo "[ERROR] Unsupported MPTCP scheduler: ${SCHEDULER}"
        echo
        echo "Supported schedulers:"
        echo "  default (BLEST)"
        echo "  bpf_saflo (Saflo)"
        echo "  bpf_ran (BWR)"
        exit 1
        ;;
esac

echo "[INFO] Setting MPTCP scheduler to: ${SCHEDULER}"

sudo sysctl -w "net.mptcp.scheduler=${SCHEDULER}"

echo
echo "[INFO] Current MPTCP scheduler:"
sysctl net.mptcp.scheduler
