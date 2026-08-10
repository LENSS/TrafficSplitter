#!/usr/bin/env bash

set -euo pipefail


# ------------------------------------------------------------
# Python environment
# ------------------------------------------------------------

# Prefer the local virtual environment used by the AE setup.
if [[ -x ".venv/bin/python" ]]; then
    PYTHON=".venv/bin/python"
    echo "[INFO] Using Python virtual environment: ${PYTHON}"
else
    PYTHON="$(command -v python3 || true)"

    if [[ -z "${PYTHON}" ]]; then
        echo "[ERROR] Python 3 was not found." >&2
        echo "Please create the AE virtual environment before running this script." >&2
        exit 1
    fi

    echo "[WARN] .venv was not found; using: ${PYTHON}"
fi

# Verify required packages before starting the evaluation.
if ! "${PYTHON}" -c "import tensorflow, pandas" >/dev/null 2>&1; then
    echo "[ERROR] Required Python packages are missing." >&2
    echo "Please install TensorFlow and pandas in the AE virtual environment:" >&2
    echo "  .venv/bin/python -m pip install tensorflow pandas" >&2
    exit 1
fi


# ============================================================
# Traffic Analysis Evaluation
#
# Run from:
#   ~/ndss2027/TrafficSplitter/eval/03-traffic-analysis
#
# Usage:
#   ./run-traffic-analysis.sh
#   ./run-traffic-analysis.sh --clear
# ============================================================


# ------------------------------------------------------------
# Clear generated evaluation results
# ------------------------------------------------------------

clear_results() {
    echo "============================================================"
    echo " Clearing Traffic-Analysis Evaluation Results"
    echo "============================================================"
    echo

    rm -rf \
        trafficsplitter-web-tfrecord \
        trafficsplitter-video-tfrecord \
        bwr-web-tfrecord \
        bwr-video-tfrecord \
        trafficsplitter-wf-models \
        trafficsplitter-vf-models \
        bwr-wf-models \
        bwr-vf-models

    rm -f \
        trafficsplitter-wf-training.log \
        trafficsplitter-vf-training.log \
        bwr-wf-training.log \
        bwr-vf-training.log

    echo "[PASS] Evaluation results have been removed."
    echo
}


# ------------------------------------------------------------
# Command-line options
# ------------------------------------------------------------

if [[ "${1:-}" == "--clear" ]]; then
    clear_results
    exit 0
fi


echo "============================================================"
echo " TrafficSplitter: Traffic Analysis Evaluation"
echo "============================================================"
echo

# ------------------------------------------------------------
# 1. Create output directories
# ------------------------------------------------------------

echo "[1/3] Preparing directories..."

mkdir -p \
    trafficsplitter-web-tfrecord \
    trafficsplitter-video-tfrecord \
    bwr-web-tfrecord \
    bwr-video-tfrecord \
    trafficsplitter-wf-models \
    trafficsplitter-vf-models \
    bwr-wf-models \
    bwr-vf-models

echo "[PASS] Output directories are ready."
echo


# ------------------------------------------------------------
# 2. Preprocess traces
# ------------------------------------------------------------

echo "============================================================"
echo "[2/3] Preprocessing Traffic Traces"
echo "============================================================"
echo

echo "[INFO] TrafficSplitter video traces..."
"${PYTHON}" 02-preprocess_VF_bps.py \
    ../02-data-collection/trafficsplitter-video-traces \
    trafficsplitter-video-tfrecord/

echo

echo "[INFO] BWR video traces..."
"${PYTHON}" 02-preprocess_VF_bps.py \
    ../02-data-collection/bwr-video-traces \
    bwr-video-tfrecord/

echo

echo "[INFO] TrafficSplitter website traces..."
"${PYTHON}" 03-preprocess_WF_DF.py \
    ../02-data-collection/trafficsplitter-web-traces \
    trafficsplitter-web-tfrecord/

echo

echo "[INFO] BWR website traces..."
"${PYTHON}" 03-preprocess_WF_DF.py \
    ../02-data-collection/bwr-web-traces \
    bwr-web-tfrecord/

echo
echo "[PASS] Preprocessing completed."
echo


# ------------------------------------------------------------
# Helper function:
# Run training and extract mean validation accuracy
# ------------------------------------------------------------

run_training() {
    local result_variable="$1"
    local log_file="$2"
    shift 2

    "$@" 2>&1 | tee "${log_file}"

    local accuracy

    accuracy=$(
        awk '
            /Mean validation accuracy:/ {
                acc=$4
            }
            /Mean val_accuracy:/ {
                acc=$3
            }
            END {
                if (acc != "")
                    print acc
            }
        ' "${log_file}"
    )

    if [[ -z "${accuracy}" ]]; then
        echo "[ERROR] Could not extract accuracy from ${log_file}" >&2
        exit 1
    fi

    printf -v "${result_variable}" '%s' "${accuracy}"
}


# ------------------------------------------------------------
# 3. Run traffic-analysis attacks
# ------------------------------------------------------------

echo "============================================================"
echo "[3/3] Running Traffic-Analysis Evaluation"
echo "============================================================"
echo


# -------------------------
# Video Fingerprinting
# -------------------------

echo
echo "------------------------------------------------------------"
echo " Video Fingerprinting: TrafficSplitter"
echo "------------------------------------------------------------"

run_training \
    TS_VF_ACC \
    trafficsplitter-vf-training.log \
    "${PYTHON}" 04-video-fingerprinting-CNN.py \
        trafficsplitter-video-tfrecord/ \
        trafficsplitter-vf-models/


echo
echo "------------------------------------------------------------"
echo " Video Fingerprinting: BWR"
echo "------------------------------------------------------------"

run_training \
    BWR_VF_ACC \
    bwr-vf-training.log \
    "${PYTHON}" 04-video-fingerprinting-CNN.py \
        bwr-video-tfrecord/ \
        bwr-vf-models/


# -------------------------
# Website Fingerprinting
# -------------------------

echo
echo "------------------------------------------------------------"
echo " Website Fingerprinting: TrafficSplitter"
echo "------------------------------------------------------------"

run_training \
    TS_WF_ACC \
    trafficsplitter-wf-training.log \
    "${PYTHON}" 05-web-fingerprinting-DF.py \
        trafficsplitter-web-tfrecord/ \
        trafficsplitter-wf-models/


echo
echo "------------------------------------------------------------"
echo " Website Fingerprinting: BWR"
echo "------------------------------------------------------------"

run_training \
    BWR_WF_ACC \
    bwr-wf-training.log \
    "${PYTHON}" 05-web-fingerprinting-DF.py \
        bwr-web-tfrecord/ \
        bwr-wf-models/


# ------------------------------------------------------------
# Final results
# ------------------------------------------------------------

echo
echo
echo "============================================================"
echo " Traffic-Analysis Evaluation Results"
echo "============================================================"
echo

printf "%-25s | %-16s | %-16s\n" \
    "Attack" \
    "TrafficSplitter" \
    "BWR"

printf "%-25s-+-%-16s-+-%-16s\n" \
    "-------------------------" \
    "----------------" \
    "----------------"

printf "%-25s | %-16s | %-16s\n" \
    "Website Fingerprinting" \
    "${TS_WF_ACC}" \
    "${BWR_WF_ACC}"

printf "%-25s | %-16s | %-16s\n" \
    "Video Fingerprinting" \
    "${TS_VF_ACC}" \
    "${BWR_VF_ACC}"

echo
echo "Values represent mean validation accuracy across the 5 folds."
echo
echo "============================================================"
echo " Evaluation completed."
echo "============================================================"
