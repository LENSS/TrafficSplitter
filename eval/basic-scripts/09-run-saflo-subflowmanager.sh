#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

SUBFLOW_MANAGER="${ROOT_DIR}/src/Saflo_scheduler/build/subflow_manager"

DEFAULT_INTERVAL="2000000"
DEFAULT_MAX_PROB="0.8"
DEFAULT_MIN_PROB="0.2"

INTERVAL="${INTERVAL:-${DEFAULT_INTERVAL}}"
MAX_PROB="${MAX_PROB:-${DEFAULT_MAX_PROB}}"
MIN_PROB="${MIN_PROB:-${DEFAULT_MIN_PROB}}"

echo "============================================================"
echo " TrafficSplitter: Start Saflo Subflow Manager"
echo "============================================================"

# ------------------------------------------------------------
# Check binary
# ------------------------------------------------------------

if [[ ! -x "${SUBFLOW_MANAGER}" ]]; then
    echo "[ERROR] Subflow manager binary not found:"
    echo "        ${SUBFLOW_MANAGER}"
    echo
    echo "Run the scheduler build script first."
    exit 1
fi

# ------------------------------------------------------------
# Show configuration
# ------------------------------------------------------------

echo "[INFO] Subflow manager : ${SUBFLOW_MANAGER}"
echo "[INFO] Time interval   : ${INTERVAL} us"
echo "[INFO] Max probability : ${MAX_PROB}"
echo "[INFO] Min probability : ${MIN_PROB}"
echo

# ------------------------------------------------------------
# Start subflow manager
# ------------------------------------------------------------

echo "[INFO] Starting Saflo subflow manager..."

exec sudo stdbuf -oL -eL "${SUBFLOW_MANAGER}" \
    -i "${INTERVAL}" \
    -x "${MAX_PROB}" \
    -n "${MIN_PROB}"
