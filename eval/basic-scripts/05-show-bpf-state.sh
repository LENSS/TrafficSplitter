#!/usr/bin/env bash
set -euo pipefail

MAP_NAME="${1:-mprd_map}"

echo "============================================================"
echo " TrafficSplitter: eBPF State"
echo "============================================================"

echo
echo "---------------- struct_ops ----------------"
sudo bpftool struct_ops show || true

echo
echo "---------------- BPF maps ------------------"
sudo bpftool map show || true

echo
echo "---------------- map: ${MAP_NAME} ----------------"

MAP_ID="$(
    sudo bpftool map show 2>/dev/null |
    awk -v target="${MAP_NAME}" '
        $0 ~ "name " target {
            gsub(":", "", $1)
            print $1
            exit
        }
    '
)"

if [[ -n "${MAP_ID}" ]]; then
    echo "[INFO] Found ${MAP_NAME}, ID=${MAP_ID}"
    sudo bpftool map dump id "${MAP_ID}"
else
    echo "[WARN] BPF map '${MAP_NAME}' is not currently loaded."
fi

echo
echo "---------------- trace log -----------------"

TRACE_FILE=""

if [[ -e /sys/kernel/tracing/trace ]]; then
    TRACE_FILE="/sys/kernel/tracing/trace"
elif [[ -e /sys/kernel/debug/tracing/trace ]]; then
    TRACE_FILE="/sys/kernel/debug/tracing/trace"
fi

if [[ -n "${TRACE_FILE}" ]]; then
    echo "[INFO] ${TRACE_FILE}"
    sudo cat "${TRACE_FILE}"
else
    echo "[WARN] Kernel tracing interface was not found."
fi
