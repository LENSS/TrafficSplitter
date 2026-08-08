#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PORT="${PORT:-1010}"
INTERFACE="${INTERFACE:-}"
DNS_IP="${DNS_IP:-}"

echo "============================================================"
echo " TrafficSplitter: Start MPTun Proxy Server"
echo "============================================================"

# ------------------------------------------------------------
# Check arguments
# ------------------------------------------------------------

if [[ $# -ne 1 ]]; then
    echo "Usage:"
    echo "  $0 <server_type>"
    echo
    echo "Server types:"
    echo "  trafficsplitter"
    echo "  bwr"
    echo "  no-batching"
    echo
    echo "Examples:"
    echo "  $0 trafficsplitter"
    echo "  $0 bwr"
    echo "  $0 no-batching"
    exit 1
fi

SERVER_TYPE="$1"

# ------------------------------------------------------------
# Select server binary
# ------------------------------------------------------------

case "${SERVER_TYPE}" in
    trafficsplitter)
        SERVER="${ROOT_DIR}/MPTun_proxy/build/TrafficSplitter_MPTun_server"
        ;;
    bwr)
        SERVER="${ROOT_DIR}/MPTun_proxy/build/BWR_MPTun_server"
        ;;
    no-batching)
        SERVER="${ROOT_DIR}/MPTun_proxy/build/No-Batching_MPTun_server"
        ;;
    *)
        echo "[ERROR] Unknown server type: ${SERVER_TYPE}"
        echo
        echo "Supported server types:"
        echo "  trafficsplitter"
        echo "  bwr"
        echo "  no-batching"
        exit 1
        ;;
esac

# ------------------------------------------------------------
# Check binary
# ------------------------------------------------------------

if [[ ! -x "${SERVER}" ]]; then
    echo "[ERROR] MPTun server binary not found:"
    echo "        ${SERVER}"
    echo
    echo "Run the MPTun build script first."
    exit 1
fi

# ------------------------------------------------------------
# Detect physical interface
# ------------------------------------------------------------

if [[ -z "${INTERFACE}" ]]; then
    INTERFACE="$(
        ip route show default |
        awk '{print $5; exit}'
    )"
fi

if [[ -z "${INTERFACE}" ]]; then
    echo "[ERROR] Could not automatically determine the physical interface."
    echo "        You can specify it manually with:"
    echo
    echo "        INTERFACE=enp0s3 $0 ${SERVER_TYPE}"
    exit 1
fi

if ! ip link show "${INTERFACE}" >/dev/null 2>&1; then
    echo "[ERROR] Interface '${INTERFACE}' does not exist."
    exit 1
fi

# ------------------------------------------------------------
# Detect bind IP
# ------------------------------------------------------------

BIND_IP="$(
    ip -4 addr show dev "${INTERFACE}" |
    awk '/inet / {split($2,a,"/"); print a[1]; exit}'
)"

if [[ -z "${BIND_IP}" ]]; then
    echo "[ERROR] No IPv4 address found on interface '${INTERFACE}'."
    exit 1
fi

# ------------------------------------------------------------
# Determine DNS server
# ------------------------------------------------------------

if [[ -z "${DNS_IP}" ]]; then

    # First try systemd-resolved.
    if command -v resolvectl >/dev/null 2>&1; then
        DNS_IP="$(
            resolvectl dns "${INTERFACE}" 2>/dev/null |
            awk '
                {
                    for (i = 3; i <= NF; i++) {
                        if ($i ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/) {
                            print $i
                            exit
                        }
                    }
                }
            '
        )"
    fi

    # Fall back to /etc/resolv.conf.
    if [[ -z "${DNS_IP}" ]]; then
        DNS_IP="$(
            awk '
                /^nameserver[[:space:]]+/ &&
                $2 ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/ {
                    print $2
                    exit
                }
            ' /etc/resolv.conf
        )"
    fi
fi

if [[ -z "${DNS_IP}" ]]; then
    echo "[ERROR] Could not determine a DNS server."
    echo "        You can specify it manually with:"
    echo
    echo "        DNS_IP=192.168.10.3 $0 ${SERVER_TYPE}"
    exit 1
fi

# ------------------------------------------------------------
# Show configuration
# ------------------------------------------------------------

echo "[INFO] Server type   : ${SERVER_TYPE}"
echo "[INFO] Server binary : ${SERVER}"
echo "[INFO] Interface     : ${INTERFACE}"
echo "[INFO] Bind IP       : ${BIND_IP}"
echo "[INFO] Port          : ${PORT}"
echo "[INFO] DNS IP        : ${DNS_IP}"
echo

# ------------------------------------------------------------
# Start server
# ------------------------------------------------------------

echo "[INFO] Starting MPTun server..."

exec sudo "${SERVER}" \
    "${BIND_IP}" \
    "${PORT}" \
    "${INTERFACE}" \
    "${DNS_IP}"
