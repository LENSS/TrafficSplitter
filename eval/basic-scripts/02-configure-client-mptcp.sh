#!/usr/bin/env bash
set -e

IFACE1="enp0s3"
IFACE2="enp0s8"

IP1=$(ip -4 addr show dev "${IFACE1}" \
    | awk '/inet / {print $2}' \
    | cut -d/ -f1 \
    | head -n1)

IP2=$(ip -4 addr show dev "${IFACE2}" \
    | awk '/inet / {print $2}' \
    | cut -d/ -f1 \
    | head -n1)

if [ -z "${IP1}" ]; then
    echo "[ERROR] No IPv4 address found on ${IFACE1}."
    exit 1
fi

if [ -z "${IP2}" ]; then
    echo "[ERROR] No IPv4 address found on ${IFACE2}."
    exit 1
fi

echo "[INFO] Detected ${IFACE1}: ${IP1}"
echo "[INFO] Detected ${IFACE2}: ${IP2}"
echo "[INFO] Configuring MPTCP endpoints..."

ip mptcp endpoint flush

ip mptcp endpoint add "${IP1}" id 1 subflow dev "${IFACE1}"
ip mptcp endpoint add "${IP2}" id 2 subflow dev "${IFACE2}"

ip mptcp limit set add_addr_accepted 4 subflows 4

echo
echo "[INFO] Current MPTCP endpoints:"
ip mptcp endpoint show

echo
echo "[INFO] Current MPTCP limits:"
ip mptcp limit show
