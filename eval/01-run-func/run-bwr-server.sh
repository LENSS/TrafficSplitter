#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASIC_SCRIPTS="${SCRIPT_DIR}/../basic-scripts"

SERVER_LOG="${SCRIPT_DIR}/mptun-server.log"

SERVER_PID=""
MONITOR_PID=""
CLEANED_UP=0

echo "============================================================"
echo " BWR: Basic Functional Evaluation - Server"
echo "============================================================"

# ------------------------------------------------------------
# Check scripts
# ------------------------------------------------------------

REQUIRED_SCRIPTS=(
    "01-configure-server-mptcp.sh"
    "03-register-scheduler.sh"
    "06-select-mptcp-scheduler.sh"
    "07-run-mptun-server.sh"
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

    # Kill the MPTCP monitor process group.
    if [[ -n "${MONITOR_PID}" ]] && kill -0 "${MONITOR_PID}" 2>/dev/null; then
        sudo kill -TERM -- "-${MONITOR_PID}" 2>/dev/null || true
    fi

    if [[ -n "${SERVER_PID}" ]]; then
        wait "${SERVER_PID}" 2>/dev/null || true
    fi

    if [[ -n "${MONITOR_PID}" ]]; then
        wait "${MONITOR_PID}" 2>/dev/null || true
    fi

    echo "[INFO] MPTun server stopped."
    echo "[INFO] MPTCP monitor stopped."
}

trap cleanup EXIT
trap 'exit 130' INT TERM

# ------------------------------------------------------------
# 1. Configure server MPTCP
# ------------------------------------------------------------

echo
echo "[INFO] Configuring MPTCP..."
"${BASIC_SCRIPTS}/01-configure-server-mptcp.sh"

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
# 5. Start MPTun server
# ------------------------------------------------------------

: > "${SERVER_LOG}"

setsid "${BASIC_SCRIPTS}/07-run-mptun-server.sh" bwr \
    > "${SERVER_LOG}" 2>&1 &

SERVER_PID=$!

echo
echo "[RUNNING] MPTun server"
echo "          Log: ${SERVER_LOG}"

# ------------------------------------------------------------
# Keep evaluation running
# ------------------------------------------------------------

echo
echo "============================================================"
echo " Server-side evaluation is running."
echo
echo " MPTCP connection events will appear in this terminal."
echo
echo " Logs:"
echo "   MPTun server : ${SERVER_LOG}"
echo
echo " Press Ctrl+C to stop."
echo "============================================================"
echo

wait
