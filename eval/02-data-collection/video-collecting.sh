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
OUTPUT_DIR="${SCRIPT_DIR}/${TRACE_NAME}-video-traces"

ITERATIONS=10
CAPTURE_TIME=90.0
BETWEEN_TRACES=0.5

VIDEOS=(
    "youtu.be/kJQP7kiw5Fk?si=Dyov981i67Kta04x"
    "youtu.be/fcnDmrtj6Sk?si=ZldoYi0q567V8fiz"
    "youtu.be/yebNIHKAC4A?si=Fp84Ie9ikwS0xbbw"
    "youtu.be/pRpeEdMmmQ0?si=1YAbwMuWzCi0aqwT"
    "youtu.be/27C4pfRsf9g?si=Fukxv35KD0CYDvPo"
)

# ------------------------------------------------------------
# Preparation
# ------------------------------------------------------------

echo "============================================================"
echo " Video Trace Collection"
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

echo "[INFO] Interface     : ${INTERFACE}"
echo "[INFO] Output        : ${OUTPUT_DIR}"
echo "[INFO] Videos        : ${#VIDEOS[@]}"
echo "[INFO] Traces/video  : ${ITERATIONS}"
echo "[INFO] Capture time  : ${CAPTURE_TIME} seconds"
echo "[INFO] Total traces  : $((${#VIDEOS[@]} * ITERATIONS))"
echo

# Cache sudo credentials for tcpdump.
sudo -v

# ------------------------------------------------------------
# Data collection
# ------------------------------------------------------------

for video_idx in "${!VIDEOS[@]}"; do

    VIDEO="${VIDEOS[$video_idx]}"

    echo
    echo "============================================================"
    echo " Video $((video_idx + 1))/${#VIDEOS[@]}: ${VIDEO}"
    echo "============================================================"

    for ((trace_idx = 0; trace_idx < ITERATIONS; trace_idx++)); do

        OUTPUT_FILE="${OUTPUT_DIR}/data${video_idx}_${trace_idx}.log"

        echo
        echo "[TRACE] Video  : ${VIDEO}"
        echo "        Trace  : $((trace_idx + 1))/${ITERATIONS}"
        echo "        Output : ${OUTPUT_FILE}"

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
        # Open video
        # ----------------------------------------------------

        setsid google-chrome "https://${VIDEO}" \
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
echo " Video trace collection completed."
echo "============================================================"
echo
echo "[INFO] Collected $((${#VIDEOS[@]} * ITERATIONS)) traces."
echo "[INFO] Output directory:"
echo "       ${OUTPUT_DIR}"
