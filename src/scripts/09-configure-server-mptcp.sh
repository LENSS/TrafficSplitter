#!/usr/bin/env bash
set -e

IFACE="enp0s3"

# Detect the current IPv4 address assigned to the interface
IP=$(ip -4 addr show dev "${IFACE}" \
    | awk '/inet / {print $2}' \
    | cut -d/ -f1 \
    | head -n1)

if [ -z "${IP}" ]; then
    echo "[ERROR] No IPv4 address found on ${IFACE}."
    exit 1
fi

echo "[INFO] Detected ${IFACE}: ${IP}"
echo "[INFO] Configuring MPTCP endpoint on proxy..."

ip mptcp endpoint flush

ip mptcp endpoint add "${IP}" id 1 signal dev "${IFACE}"

ip mptcp limit set add_addr_accepted 4 subflows 4

echo
echo "[INFO] Current MPTCP endpoints:"
ip mptcp endpoint show

echo
echo "[INFO] Current MPTCP limits:"
ip mptcp limit show
