#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

SCHEDULER_DIR="${ROOT_DIR}/Saflo_scheduler"
HEADER_DIR="${SCHEDULER_DIR}/headers"
BUILD_DIR="${SCHEDULER_DIR}/build"

SAFLO_SOURCE="${SCHEDULER_DIR}/mptcp_bpf_saflo.c"
BWR_SOURCE="${SCHEDULER_DIR}/mptcp_bpf_bwr.c"
MANAGER_SOURCE="${SCHEDULER_DIR}/subflow_manager.c"

SAFLO_OBJECT="${BUILD_DIR}/mptcp_bpf_saflo.o"
BWR_OBJECT="${BUILD_DIR}/mptcp_bpf_bwr.o"
MANAGER_BINARY="${BUILD_DIR}/subflow_manager"

echo "============================================================"
echo " TrafficSplitter: Build eBPF schedulers"
echo "============================================================"

echo "[INFO] Source directory:"
echo "       ${SCHEDULER_DIR}"

# ------------------------------------------------------------
# Check dependencies
# ------------------------------------------------------------

for cmd in clang gcc; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "[ERROR] '${cmd}' is not installed."
        echo "        Run scripts/01-install-bpf-tools.sh first."
        exit 1
    fi
done

for file in \
    "${SAFLO_SOURCE}" \
    "${BWR_SOURCE}" \
    "${MANAGER_SOURCE}"
do
    if [[ ! -f "${file}" ]]; then
        echo "[ERROR] Required source file not found:"
        echo "        ${file}"
        exit 1
    fi
done

mkdir -p "${BUILD_DIR}"

# ------------------------------------------------------------
# Architecture-specific include directory
# ------------------------------------------------------------

ARCH_INCLUDE="/usr/include/$(gcc -dumpmachine)"

if [[ ! -d "${ARCH_INCLUDE}" ]]; then
    echo "[WARN] Architecture include directory not found:"
    echo "       ${ARCH_INCLUDE}"
fi

# ------------------------------------------------------------
# Build Saflo
# ------------------------------------------------------------

echo
echo "[1/3] Compiling Saflo eBPF scheduler..."

clang \
    -O2 \
    -target bpf \
    -g \
    -c "${SAFLO_SOURCE}" \
    -o "${SAFLO_OBJECT}" \
    -I"${ARCH_INCLUDE}" \
    -I/usr/include/i386-linux-gnu/ \
    -I"${HEADER_DIR}"

echo "[PASS] ${SAFLO_OBJECT}"

# ------------------------------------------------------------
# Build BWR
# ------------------------------------------------------------

echo
echo "[2/3] Compiling BWR eBPF scheduler..."

clang \
    -O2 \
    -target bpf \
    -g \
    -c "${BWR_SOURCE}" \
    -o "${BWR_OBJECT}" \
    -I"${ARCH_INCLUDE}" \
    -I/usr/include/i386-linux-gnu/ \
    -I"${HEADER_DIR}"

echo "[PASS] ${BWR_OBJECT}"

# ------------------------------------------------------------
# Build user-space manager
# ------------------------------------------------------------

echo
echo "[3/3] Compiling Saflo subflow manager..."

gcc \
    -O2 \
    -o "${MANAGER_BINARY}" \
    "${MANAGER_SOURCE}" \
    -lbpf \
    -lelf \
    -lz

echo "[PASS] ${MANAGER_BINARY}"

echo
echo "============================================================"
echo " Build completed"
echo "============================================================"

ls -lh \
    "${SAFLO_OBJECT}" \
    "${BWR_OBJECT}" \
    "${MANAGER_BINARY}"
