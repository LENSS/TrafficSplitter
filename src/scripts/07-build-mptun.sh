#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MPTUN_DIR="${SCRIPT_DIR}/../MPTun_proxy"
BUILD_DIR="${MPTUN_DIR}/build"

echo "============================================================"
echo " TrafficSplitter: Build MPTun client/server programs"
echo "============================================================"

mkdir -p "${BUILD_DIR}"

SOURCES=(
    "TrafficSplitter_MPTun_client.c"
    "TrafficSplitter_MPTun_server.c"
    "BWR_MPTun_client.c"
    "BWR_MPTun_server.c"
    "No-Batching_MPTun_client.c"
    "No-Batching_MPTun_server.c"
)

for src in "${SOURCES[@]}"; do
    src_path="${MPTUN_DIR}/${src}"

    if [[ ! -f "${src_path}" ]]; then
        echo "[ERROR] Missing source file:"
        echo "        ${src_path}"
        exit 1
    fi

    binary_name="${src%.c}"
    output_path="${BUILD_DIR}/${binary_name}"

    echo
    echo "[INFO] Compiling ${src}..."

    gcc \
        -O2 \
        -Wall \
        -Wextra \
        -o "${output_path}" \
        "${src_path}"

    echo "[PASS] ${output_path}"
done

echo
echo "============================================================"
echo " MPTun build completed"
echo "============================================================"

ls -lh "${BUILD_DIR}"
