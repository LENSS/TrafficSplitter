#!/usr/bin/env bash

set -euo pipefail

GRUB_CONFIG="/etc/default/grub"
GRUB_BACKUP="/etc/default/grub.artifact-backup"

echo "============================================================"
echo " TRAFFICSPLITTER GRUB Configuration"
echo "============================================================"

if [[ ! -f "${GRUB_CONFIG}" ]]; then
    echo "ERROR: ${GRUB_CONFIG} does not exist."
    exit 1
fi

# ------------------------------------------------------------
# Backup existing GRUB configuration
# ------------------------------------------------------------

if [[ ! -f "${GRUB_BACKUP}" ]]; then
    echo "[INFO] Backing up existing GRUB configuration..."
    sudo cp "${GRUB_CONFIG}" "${GRUB_BACKUP}"
fi

# Helper: replace an existing setting or append it if absent.
set_grub_option()
{
    local key="$1"
    local value="$2"

    if sudo grep -qE "^${key}=" "${GRUB_CONFIG}"; then
        sudo sed -i "s|^${key}=.*|${key}=${value}|" "${GRUB_CONFIG}"
    else
        echo "${key}=${value}" | sudo tee -a "${GRUB_CONFIG}" >/dev/null
    fi
}

echo "[INFO] Configuring GRUB..."

set_grub_option "GRUB_DEFAULT" "saved"
set_grub_option "GRUB_SAVEDEFAULT" "true"
set_grub_option "GRUB_TIMEOUT_STYLE" "menu"
set_grub_option "GRUB_TIMEOUT" "10"

echo "[INFO] Updating GRUB..."
sudo update-grub

echo
echo "============================================================"
echo " GRUB configuration completed"
echo "============================================================"
echo
echo "On the next reboot:"
echo
echo "  1. Open the GRUB menu."
echo "  2. Select 'Advanced options for Ubuntu'."
echo "  3. Select the custom kernel."
echo
echo "GRUB is configured to remember the selected entry."
echo
echo "After booting, verify with:"
echo
echo "  uname -r"
