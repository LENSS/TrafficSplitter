#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASIC_SCRIPTS="${SCRIPT_DIR}/../basic-scripts"

CLIENT_LOG="${SCRIPT_DIR}/mptun-client.log"

DEFAULT_SERVER_IP="192.168.10.12"
SERVER_IP="${SERVER_IP:-${DEFAULT_SERVER_IP}}"

CLIENT_PID=""
MONITOR_PID=""
CLEANED_UP=0

echo "============================================================"
echo " BWR: Basic Functional Evaluation - Client"
echo "============================================================"

# ------------------------------------------------------------
# Check scripts
# ------------------------------------------------------------

REQUIRED_SCRIPTS=(
    "02-configure-client-mptcp.sh"
    "03-register-scheduler.sh"
    "06-select-mptcp-scheduler.sh"
    "08-run-mptun-client.sh"
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

    # Kill the MPTCP monitor process group.
    if [[ -n "${MONITOR_PID}" ]] && kill -0 "${MONITOR_PID}" 2>/dev/null; then
        sudo kill -TERM -- "-${MONITOR_PID}" 2>/dev/null || true
    fi

    if [[ -n "${CLIENT_PID}" ]]; then
        wait "${CLIENT_PID}" 2>/dev/null || true
    fi

    if [[ -n "${MONITOR_PID}" ]]; then
        wait "${MONITOR_PID}" 2>/dev/null || true
    fi

    echo "[INFO] MPTun client stopped."
    echo "[INFO] MPTCP monitor stopped."
}

trap cleanup EXIT
trap 'exit 130' INT TERM

# ------------------------------------------------------------
# 1. Configure client MPTCP
# ------------------------------------------------------------

echo
echo "[INFO] Configuring MPTCP..."
"${BASIC_SCRIPTS}/02-configure-client-mptcp.sh"

# ------------------------------------------------------------
# 2. Register BWR scheduler
# ------------------------------------------------------------

echo
echo "[INFO] Registering BWR scheduler..."
"${BASIC_SCRIPTS}/03-register-scheduler.sh" bwr

# ------------------------------------------------------------
# 3. Select BWR as MPTCP scheduler
# ------------------------------------------------------------

echo
echo "[INFO] Selecting bpf_ran..."
"${BASIC_SCRIPTS}/06-select-mptcp-scheduler.sh" bpf_ran

# ------------------------------------------------------------
# 4. Start MPTCP monitor
# ------------------------------------------------------------

echo
echo "============================================================"
echo " MPTCP Monitor"
echo "============================================================"
echo "[INFO] MPTCP events will be displayed below."
echo

setsid sudo stdbuf -oL ip mptcp monitor &
MONITOR_PID=$!

# ------------------------------------------------------------
# 5. Start MPTun client
# ------------------------------------------------------------

: > "${CLIENT_LOG}"

setsid "${BASIC_SCRIPTS}/08-run-mptun-client.sh" \
    bwr \
    "${SERVER_IP}" \
    > "${CLIENT_LOG}" 2>&1 &

CLIENT_PID=$!

echo
echo "[RUNNING] MPTun client"
echo "          Server: ${SERVER_IP}"
echo "          Log   : ${CLIENT_LOG}"

# ------------------------------------------------------------
# Keep evaluation running
# ------------------------------------------------------------

echo
echo "============================================================"
echo " Client-side evaluation is running."
echo
echo " Server IP: ${SERVER_IP}"
echo
echo " MPTCP connection events will appear in this terminal."
echo
echo " Logs:"
echo "   MPTun client : ${CLIENT_LOG}"
echo
echo " Press Ctrl+C to stop."
echo "============================================================"
echo

wait
