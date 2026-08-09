#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASIC_SCRIPTS="${SCRIPT_DIR}/../basic-scripts"

SERVER_LOG="${SCRIPT_DIR}/mptun-server.log"
SUBFLOW_LOG="${SCRIPT_DIR}/subflow-manager.log"

SERVER_PID=""
SUBFLOW_PID=""
CLEANED_UP=0

echo "============================================================"
echo " TrafficSplitter: Basic Functional Evaluation - Server"
echo "============================================================"

# ------------------------------------------------------------
# Check scripts
# ------------------------------------------------------------

REQUIRED_SCRIPTS=(
    "01-configure-server-mptcp.sh"
    "03-register-scheduler.sh"
    "06-select-mptcp-scheduler.sh"
    "07-run-mptun-server.sh"
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
    echo "[INFO] Stopping server-side evaluation..."

    # Kill the MPTun server process group.
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        sudo kill -TERM -- "-${SERVER_PID}" 2>/dev/null || true
    fi

    # Kill the Saflo subflow manager process group.
    if [[ -n "${SUBFLOW_PID}" ]] && kill -0 "${SUBFLOW_PID}" 2>/dev/null; then
        sudo kill -TERM -- "-${SUBFLOW_PID}" 2>/dev/null || true
    fi

    wait "${SERVER_PID}" 2>/dev/null || true
    wait "${SUBFLOW_PID}" 2>/dev/null || true

    echo "[INFO] MPTun server stopped."
    echo "[INFO] Saflo subflow manager stopped."
}

trap cleanup EXIT
trap 'exit 130' INT TERM

# ------------------------------------------------------------
# 1. Configure server MPTCP
# ------------------------------------------------------------

echo "[INFO] Configuring MPTCP..."
"${BASIC_SCRIPTS}/01-configure-server-mptcp.sh"

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
# 4. Start MPTun server
# ------------------------------------------------------------

: > "${SERVER_LOG}"

setsid "${BASIC_SCRIPTS}/07-run-mptun-server.sh" trafficsplitter \
    > "${SERVER_LOG}" 2>&1 &

SERVER_PID=$!

echo "[RUNNING] MPTun server"
echo "          Log: ${SERVER_LOG}"

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
echo " Server-side evaluation is running."
echo
echo " Logs:"
echo "   MPTun server   : ${SERVER_LOG}"
echo "   Subflow manager: ${SUBFLOW_LOG}"
echo
echo " Press Ctrl+C to stop."
echo "============================================================"

# Keep this wrapper alive until the user presses Ctrl+C
# or one of the background components terminates.
wait
