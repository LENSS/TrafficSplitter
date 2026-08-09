#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASIC_SCRIPTS="${SCRIPT_DIR}/../basic-scripts"

CLIENT_LOG="${SCRIPT_DIR}/mptun-client.log"
SUBFLOW_LOG="${SCRIPT_DIR}/subflow-manager.log"

DEFAULT_SERVER_IP="192.168.10.12"
SERVER_IP="${SERVER_IP:-${DEFAULT_SERVER_IP}}"

CLIENT_PID=""
SUBFLOW_PID=""
CLEANED_UP=0

echo "============================================================"
echo " TrafficSplitter: Basic Functional Evaluation - Client"
echo "============================================================"

# ------------------------------------------------------------
# Check scripts
# ------------------------------------------------------------

REQUIRED_SCRIPTS=(
    "02-configure-client-mptcp.sh"
    "03-register-scheduler.sh"
    "06-select-mptcp-scheduler.sh"
    "08-run-mptun-client.sh"
    "09-run-saflo-subflowmanager.sh"
)

for script in "${REQUIRED_SCRIPTS[@]}"; do
    if [[ ! -x "${BASIC_SCRIPTS}/${script}" ]]; then
        echo "[ERROR] Script not found or not executable:"
        echo "        ${BASIC_SCRIPTS}/${script}"
        exit 1
    fi
done

# Cache sudo credentials before starting background processes.
sudo -v

# ------------------------------------------------------------
# Cleanup
# ------------------------------------------------------------

cleanup() {
    if [[ "${CLEANED_UP}" -eq 1 ]]; then
        return
    fi

    CLEANED_UP=1

    echo
    echo "[INFO] Stopping client-side evaluation..."

    # Kill the MPTun client process group.
    if [[ -n "${CLIENT_PID}" ]] && kill -0 "${CLIENT_PID}" 2>/dev/null; then
        sudo kill -TERM -- "-${CLIENT_PID}" 2>/dev/null || true
    fi

    # Kill the Saflo subflow manager process group.
    if [[ -n "${SUBFLOW_PID}" ]] && kill -0 "${SUBFLOW_PID}" 2>/dev/null; then
        sudo kill -TERM -- "-${SUBFLOW_PID}" 2>/dev/null || true
    fi

    wait "${CLIENT_PID}" 2>/dev/null || true
    wait "${SUBFLOW_PID}" 2>/dev/null || true

    echo "[INFO] MPTun client stopped."
    echo "[INFO] Saflo subflow manager stopped."
}

trap cleanup EXIT
trap 'exit 130' INT TERM

# ------------------------------------------------------------
# 1. Configure client MPTCP
# ------------------------------------------------------------

echo "[INFO] Configuring MPTCP..."
"${BASIC_SCRIPTS}/02-configure-client-mptcp.sh"

# ------------------------------------------------------------
# 2. Register Saflo scheduler
# ------------------------------------------------------------

echo "[INFO] Registering Saflo scheduler..."
"${BASIC_SCRIPTS}/03-register-scheduler.sh" saflo

# ------------------------------------------------------------
# 3. Select Saflo as MPTCP scheduler
# ------------------------------------------------------------

echo "[INFO] Selecting bpf_saflo..."
"${BASIC_SCRIPTS}/06-select-mptcp-scheduler.sh" bpf_saflo

# ------------------------------------------------------------
# 4. Start MPTun client
# ------------------------------------------------------------

: > "${CLIENT_LOG}"

setsid "${BASIC_SCRIPTS}/08-run-mptun-client.sh" \
    trafficsplitter \
    "${SERVER_IP}" \
    > "${CLIENT_LOG}" 2>&1 &

CLIENT_PID=$!

echo "[RUNNING] MPTun client"
echo "          Server: ${SERVER_IP}"
echo "          Log   : ${CLIENT_LOG}"

# ------------------------------------------------------------
# 5. Start Saflo subflow manager
# ------------------------------------------------------------

: > "${SUBFLOW_LOG}"

setsid "${BASIC_SCRIPTS}/09-run-saflo-subflowmanager.sh" \
    > "${SUBFLOW_LOG}" 2>&1 &

SUBFLOW_PID=$!

echo "[RUNNING] Saflo subflow manager"
echo "          Log: ${SUBFLOW_LOG}"

# ------------------------------------------------------------
# Keep evaluation running
# ------------------------------------------------------------

echo
echo "============================================================"
echo " Client-side evaluation is running."
echo
echo " Server IP: ${SERVER_IP}"
echo
echo " Logs:"
echo "   MPTun client   : ${CLIENT_LOG}"
echo "   Subflow manager: ${SUBFLOW_LOG}"
echo
echo " Press Ctrl+C to stop."
echo "============================================================"

wait
