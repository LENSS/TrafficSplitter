#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="${SCRIPT_DIR}/mptcp_net-next"

echo "============================================================"
echo " TRAFFICSPLITTER Kernel Build and Installation"
echo "============================================================"

if [[ ! -d "${KERNEL_DIR}" ]]; then
    echo "ERROR: Kernel source directory does not exist:"
    echo "       ${KERNEL_DIR}"
    echo
    echo "Run 01-prepare-kernel.sh first."
    exit 1
fi

cd "${KERNEL_DIR}"

# ------------------------------------------------------------
# Check disk space
# ------------------------------------------------------------

AVAILABLE_KB="$(df --output=avail . | tail -1 | tr -d ' ')"
AVAILABLE_GB=$(( AVAILABLE_KB / 1024 / 1024 ))

echo "[INFO] Available disk space: approximately ${AVAILABLE_GB} GB"

if (( AVAILABLE_GB < 10 )); then
    echo
    echo "WARNING: Less than 10 GB of free disk space is available."
    echo "A Linux kernel build can require substantial temporary space."
    echo
    read -r -p "Continue anyway? [y/N] " response

    if [[ ! "${response}" =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# ------------------------------------------------------------
# Build kernel and modules
# ------------------------------------------------------------

JOBS="${JOBS:-$(nproc)}"

echo "[INFO] Building kernel using ${JOBS} parallel jobs..."
echo "[INFO] This step may take considerable time."

make -j"${JOBS}"

# A normal 'make' already builds configured kernel modules.
# Therefore a separate 'make modules' step is not required.

# ------------------------------------------------------------
# Install modules
# ------------------------------------------------------------

echo "[INFO] Installing kernel modules..."
sudo make modules_install

# ------------------------------------------------------------
# Install kernel
# ------------------------------------------------------------

echo "[INFO] Installing kernel..."
sudo make install

# ------------------------------------------------------------
# Verification
# ------------------------------------------------------------

KERNEL_RELEASE="$(make -s kernelrelease)"

echo
echo "[INFO] Installed kernel release:"
echo "       ${KERNEL_RELEASE}"

if [[ -f "/boot/vmlinuz-${KERNEL_RELEASE}" ]]; then
    echo "[PASS] /boot/vmlinuz-${KERNEL_RELEASE}"
else
    echo "[WARN] Kernel image was not found at the expected location."
fi

if [[ -d "/lib/modules/${KERNEL_RELEASE}" ]]; then
    echo "[PASS] /lib/modules/${KERNEL_RELEASE}"
else
    echo "[WARN] Installed module directory was not found."
fi

if [[ -f "/boot/initrd.img-${KERNEL_RELEASE}" ]]; then
    echo "[PASS] /boot/initrd.img-${KERNEL_RELEASE}"
else
    echo "[WARN] initramfs was not found at the expected location."
fi

echo
echo "============================================================"
echo " Kernel installation completed"
echo "============================================================"
echo
echo "Next step:"
echo "  ./03-configure-grub.sh"
