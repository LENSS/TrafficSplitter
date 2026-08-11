#!/usr/bin/env bash

set -euo pipefail

IFACE1="enp0s3"
IFACE2="enp0s8"
INTERVAL="${1:-10}"

read_bytes() {
    local iface="$1"

    local rx
    local tx

    rx=$(cat "/sys/class/net/${iface}/statistics/rx_bytes")
    tx=$(cat "/sys/class/net/${iface}/statistics/tx_bytes")

    echo "$rx $tx"
}

echo "============================================================"
echo " Client Interface Traffic Measurement"
echo "============================================================"
echo
echo "Interfaces:"
echo "  ${IFACE1}"
echo "  ${IFACE2}"
echo
echo "Measurement interval: ${INTERVAL} seconds"
echo
echo "Generate traffic now..."
echo

read RX1_START TX1_START < <(read_bytes "${IFACE1}")
read RX2_START TX2_START < <(read_bytes "${IFACE2}")

sleep "${INTERVAL}"

read RX1_END TX1_END < <(read_bytes "${IFACE1}")
read RX2_END TX2_END < <(read_bytes "${IFACE2}")

RX1=$((RX1_END - RX1_START))
TX1=$((TX1_END - TX1_START))
RX2=$((RX2_END - RX2_START))
TX2=$((TX2_END - TX2_START))

TOTAL1=$((RX1 + TX1))
TOTAL2=$((RX2 + TX2))
TOTAL=$((TOTAL1 + TOTAL2))

echo
echo "============================================================"
echo " Traffic Distribution"
echo "============================================================"

printf "%-10s %12s %12s %12s\n" \
    "Interface" "RX (MiB)" "TX (MiB)" "Total (MiB)"

awk -v i="${IFACE1}" -v rx="${RX1}" -v tx="${TX1}" \
    'BEGIN {
        printf "%-10s %12.2f %12.2f %12.2f\n",
        i, rx/1048576, tx/1048576, (rx+tx)/1048576
    }'

awk -v i="${IFACE2}" -v rx="${RX2}" -v tx="${TX2}" \
    'BEGIN {
        printf "%-10s %12.2f %12.2f %12.2f\n",
        i, rx/1048576, tx/1048576, (rx+tx)/1048576
    }'

echo

if (( TOTAL > 0 )); then
    awk \
        -v i1="${IFACE1}" \
        -v i2="${IFACE2}" \
        -v t1="${TOTAL1}" \
        -v t2="${TOTAL2}" \
        -v total="${TOTAL}" \
        'BEGIN {
            printf "%s share: %.1f%%\n", i1, 100*t1/total
            printf "%s share: %.1f%%\n", i2, 100*t2/total
        }'
else
    echo "[WARN] No traffic was observed during the measurement interval."
fi

echo
echo "============================================================"
