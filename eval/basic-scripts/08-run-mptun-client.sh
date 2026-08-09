#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

DEFAULT_SERVER_PORT="1010"
DEFAULT_TUN_NAME="tun0"
GATEWAY_IP="192.168.10.1"

echo "============================================================"
echo " TrafficSplitter: Start MPTun Client"
echo "============================================================"

# ------------------------------------------------------------
# Check arguments
# ------------------------------------------------------------

if [[ $# -ne 2 ]]; then
    echo "Usage:"
    echo "  $0 <client_type> <server_ip>"
    echo
    echo "Client types:"
    echo "  trafficsplitter"
    echo "  bwr"
    echo "  no-batching"
    echo
    echo "Examples:"
    echo "  $0 trafficsplitter 192.168.10.12"
    echo "  $0 bwr             192.168.10.12"
    echo "  $0 no-batching     192.168.10.12"
    exit 1
fi

CLIENT_TYPE="$1"
SERVER_IP="$2"

# ------------------------------------------------------------
# Select client binary
# ------------------------------------------------------------

case "${CLIENT_TYPE}" in
    trafficsplitter)
        CLIENT="${ROOT_DIR}/MPTun_proxy/build/TrafficSplitter_MPTun_client"
        ;;
    bwr)
        CLIENT="${ROOT_DIR}/MPTun_proxy/build/BWR_MPTun_client"
        ;;
    no-batching)
        CLIENT="${ROOT_DIR}/MPTun_proxy/build/No-Batching_MPTun_client"
        ;;
    *)
        echo "[ERROR] Unknown client type: ${CLIENT_TYPE}"
        echo
        echo "Supported client types:"
        echo "  trafficsplitter"
        echo "  bwr"
        echo "  no-batching"
        exit 1
        ;;
esac

# ------------------------------------------------------------
# Check binary
# ------------------------------------------------------------

if [[ ! -x "${CLIENT}" ]]; then
    echo "[ERROR] MPTun client binary not found:"
    echo "        ${CLIENT}"
    echo
    echo "Run the MPTun build script first."
    exit 1
fi

# ------------------------------------------------------------
# Configuration
# ------------------------------------------------------------

SERVER_PORT="${SERVER_PORT:-${DEFAULT_SERVER_PORT}}"
TUN_NAME="${TUN_NAME:-${DEFAULT_TUN_NAME}}"
PHYSICAL_IFACE="${PHYSICAL_IFACE:-}"

# ------------------------------------------------------------
# Detect physical interface
# ------------------------------------------------------------

if [[ -z "${PHYSICAL_IFACE}" ]]; then
    PHYSICAL_IFACE="$(
        ip route get "${SERVER_IP}" 2>/dev/null |
        awk '
        {
            for (i = 1; i <= NF; i++) {
                if ($i == "dev") {
                    print $(i+1)
                    exit
                }
            }
        }
        '
    )"
fi

if [[ -z "${PHYSICAL_IFACE}" ]]; then
    echo "[ERROR] Could not determine the interface used to reach ${SERVER_IP}."
    echo
    echo "You can specify it manually:"
    echo "  PHYSICAL_IFACE=enp0s3 $0 ${CLIENT_TYPE} ${SERVER_IP}"
    exit 1
fi

if ! ip link show "${PHYSICAL_IFACE}" >/dev/null 2>&1; then
    echo "[ERROR] Interface '${PHYSICAL_IFACE}' does not exist."
    exit 1
fi

# ------------------------------------------------------------
# Show configuration
# ------------------------------------------------------------

echo "[INFO] Client type        : ${CLIENT_TYPE}"
echo "[INFO] Client binary      : ${CLIENT}"
echo "[INFO] Server IP          : ${SERVER_IP}"
echo "[INFO] Server port        : ${SERVER_PORT}"
echo "[INFO] TUN interface      : ${TUN_NAME}"
echo "[INFO] Gateway IP         : ${GATEWAY_IP}"
echo "[INFO] Physical interface : ${PHYSICAL_IFACE}"
echo

# ------------------------------------------------------------
# Start client
# ------------------------------------------------------------

echo "[INFO] Starting MPTun client..."

sudo "${CLIENT}" \
    "${SERVER_IP}" \
    "${SERVER_PORT}" \
    "${TUN_NAME}" \
    "${GATEWAY_IP}" \
    "${PHYSICAL_IFACE}" &

CLIENT_PID=$!

# VirtualBox-specific adjustment:
# MPTun adds a host route to the server via the NAT gateway.
# Since the server is directly reachable on the same subnet,
# remove the route as soon as MPTun creates it.

wait "${CLIENT_PID}"
