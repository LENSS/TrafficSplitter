#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -ne 1 ]]; then
    echo "Usage:"
    echo "  $0 <trace-name>"
    echo
    echo "Example:"
    echo "  $0 trafficsplitter"
    exit 1
fi

TRACE_NAME="$1"

# ------------------------------------------------------------
# Configuration
# ------------------------------------------------------------

INTERFACE="enp0s3"
OUTPUT_DIR="${SCRIPT_DIR}/${TRACE_NAME}-web-traces"

ITERATIONS=25
CAPTURE_TIME=7.0
BETWEEN_TRACES=0.5

SITES=(
    "google.com"
    "youtube.com"
    "reddit.com"
    "amazon.com"
    "facebook.com"
    "bing.com"
    "duckduckgo.com"
    "yahoo.com"
    "wikipedia.org"
    "instagram.com"
)

# ------------------------------------------------------------
# Preparation
# ------------------------------------------------------------

echo "============================================================"
echo " Website Trace Collection"
echo "============================================================"

if ! command -v tcpdump >/dev/null 2>&1; then
    echo "[ERROR] tcpdump is not installed."
    exit 1
fi

if ! command -v google-chrome >/dev/null 2>&1; then
    echo "[ERROR] google-chrome is not installed."
    exit 1
fi

if ! ip link show "${INTERFACE}" >/dev/null 2>&1; then
    echo "[ERROR] Interface '${INTERFACE}' does not exist."
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

echo "[INFO] Interface      : ${INTERFACE}"
echo "[INFO] Output         : ${OUTPUT_DIR}"
echo "[INFO] Websites       : ${#SITES[@]}"
echo "[INFO] Traces/site    : ${ITERATIONS}"
echo "[INFO] Capture time   : ${CAPTURE_TIME} seconds"
echo "[INFO] Total traces   : $((${#SITES[@]} * ITERATIONS))"
echo

# Cache sudo credentials for tcpdump.
sudo -v

# ------------------------------------------------------------
# Data collection
# ------------------------------------------------------------

for site_idx in "${!SITES[@]}"; do

    SITE="${SITES[$site_idx]}"

    echo
    echo "============================================================"
    echo " Website $((site_idx + 1))/${#SITES[@]}: ${SITE}"
    echo "============================================================"

    for ((trace_idx = 0; trace_idx < ITERATIONS; trace_idx++)); do

        OUTPUT_FILE="${OUTPUT_DIR}/data${site_idx}_${trace_idx}.log"

        echo
        echo "[TRACE] Website : ${SITE}"
        echo "        Trace   : $((trace_idx + 1))/${ITERATIONS}"
        echo "        Output  : ${OUTPUT_FILE}"

        # ----------------------------------------------------
        # Start tcpdump
        # ----------------------------------------------------

        sudo tcpdump \
            -i "${INTERFACE}" \
            -n \
            > "${OUTPUT_FILE}" &

        TCPDUMP_PID=$!

        # Give tcpdump a short moment to initialize.
        sleep 0.2

        # ----------------------------------------------------
        # Open website
        # ----------------------------------------------------

        setsid google-chrome "https://${SITE}" \
            >/dev/null 2>&1 &

        BROWSER_PID=$!

        # ----------------------------------------------------
        # Collect traffic
        # ----------------------------------------------------

        sleep "${CAPTURE_TIME}"

        # ----------------------------------------------------
        # Stop tcpdump
        # ----------------------------------------------------

        sudo kill -SIGTERM "${TCPDUMP_PID}" 2>/dev/null || true
        wait "${TCPDUMP_PID}" 2>/dev/null || true

        # ----------------------------------------------------
        # Stop browser
        # ----------------------------------------------------

        if kill -0 "${BROWSER_PID}" 2>/dev/null; then
            kill -TERM -- "-${BROWSER_PID}" 2>/dev/null || true
        fi

        wait "${BROWSER_PID}" 2>/dev/null || true

        echo "[PASS] Trace collected."

        sleep "${BETWEEN_TRACES}"
    done
done

echo
echo "============================================================"
echo " Website trace collection completed."
echo "============================================================"
echo
echo "[INFO] Collected $((${#SITES[@]} * ITERATIONS)) traces."
echo "[INFO] Output directory:"
echo "       ${OUTPUT_DIR}"
