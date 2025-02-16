#!/bin/bash

# Path to your virtual environment
VENV_PATH="/home/sangwoo/workspace/ranflow_scheduler/.venv"

# Path to your Python script
SCRIPT_PATH="./detector.py"

# Log files for each process
DETECTOR_LOG="detector.log"
SUBFLOW_MANAGER_LOG="subflow_manager.log"

# Function to clean up background processes
cleanup() {
    echo "Terminating processes..."
    kill $P1 $P2 2>/dev/null
    wait $P1 $P2 2>/dev/null
    echo "Processes terminated."
}

# Trap signals (e.g., Ctrl+C) and call cleanup
trap cleanup SIGINT SIGTERM

# Activate the virtual environment
source "$VENV_PATH/bin/activate"

# Run the Python script in the background
python3 "$SCRIPT_PATH" > "$DETECTOR_LOG" 2>&1 &
P1=$!

# Run the subflow manager in the background
sudo ./subflow_manager -i 2000000 -x 0.8 -n 0.2 > "$SUBFLOW_MANAGER_LOG" 2>&1 &
P2=$!

# Wait for both processes to complete
wait $P1 $P2
EXIT_CODE=$?

# Check if any process failed
if [ $EXIT_CODE -ne 0 ]; then
    echo "One or more processes failed. Check the logs for details:"
    echo "  Detector Log: $DETECTOR_LOG"
    echo "  Subflow Manager Log: $SUBFLOW_MANAGER_LOG"
    exit 1
else
    echo "Both processes completed successfully."
fi
