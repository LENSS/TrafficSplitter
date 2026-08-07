#!/usr/bin/env bash

set -euo pipefail

KERNEL_REPO="https://github.com/multipath-tcp/mptcp_net-next.git"
KERNEL_COMMIT="4d907d0e9f974e706ad6f916b8bf2391d82573bf"
KERNEL_DIR="mptcp_net-next"

echo "============================================================"
echo " TRAFFICSPLITTER Kernel Preparation"
echo "============================================================"

# ------------------------------------------------------------
# 1. Check basic environment
# ------------------------------------------------------------

if [[ "$(id -u)" -eq 0 ]]; then
    echo "ERROR: Run this script as a normal user, not as root."
    echo "       The script will use sudo when necessary."
    exit 1
fi

echo "[INFO] Current kernel: $(uname -r)"
echo "[INFO] Architecture:   $(uname -m)"

# ------------------------------------------------------------
# 2. Configure Ubuntu repositories
# ------------------------------------------------------------

echo "[INFO] Configuring Ubuntu APT repositories..."

APT_SOURCE="/etc/apt/sources.list.d/ubuntu.sources"
APT_BACKUP="${APT_SOURCE}.artifact-backup"

if [[ -f "${APT_SOURCE}" && ! -f "${APT_BACKUP}" ]]; then
    echo "[INFO] Backing up ${APT_SOURCE}"
    sudo cp "${APT_SOURCE}" "${APT_BACKUP}"
fi

sudo tee "${APT_SOURCE}" >/dev/null <<'EOF'
Types: deb deb-src
URIs: http://us.archive.ubuntu.com/ubuntu/
Suites: noble noble-updates noble-backports noble-proposed
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb deb-src
URIs: http://security.ubuntu.com/ubuntu/
Suites: noble-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

# ------------------------------------------------------------
# 3. Update APT
# ------------------------------------------------------------

echo "[INFO] Updating package information..."
sudo apt update

# NOTE:
# We intentionally do not run "apt upgrade" automatically because
# upgrading the complete VM can change the evaluation environment.
#
# If desired, run manually:
#   sudo apt upgrade

# ------------------------------------------------------------
# 4. Install kernel build dependencies
# ------------------------------------------------------------

echo "[INFO] Installing kernel build dependencies..."

# Build dependencies for Ubuntu's kernel package.
sudo apt-get build-dep -y linux || {
    echo "[WARN] 'apt-get build-dep linux' failed."
    echo "[WARN] Continuing with explicitly listed dependencies."
}

sudo apt-get install -y \
    build-essential \
    bc \
    libncurses-dev \
    gawk \
    flex \
    bison \
    openssl \
    libssl-dev \
    dkms \
    libelf-dev \
    libudev-dev \
    libpci-dev \
    libiberty-dev \
    autoconf \
    dwarves \
    pahole \
    cpio \
    rsync \
    git

# ------------------------------------------------------------
# 5. Clone and check out exact kernel revision
# ------------------------------------------------------------

if [[ ! -d "${KERNEL_DIR}/.git" ]]; then
    echo "[INFO] Cloning MPTCP kernel repository..."
    git clone "${KERNEL_REPO}" "${KERNEL_DIR}"
else
    echo "[INFO] Kernel repository already exists."
fi

cd "${KERNEL_DIR}"

echo "[INFO] Fetching repository updates..."
git fetch origin

echo "[INFO] Checking out artifact kernel commit:"
echo "       ${KERNEL_COMMIT}"

git checkout "${KERNEL_COMMIT}"

ACTUAL_COMMIT="$(git rev-parse HEAD)"

if [[ "${ACTUAL_COMMIT}" != "${KERNEL_COMMIT}" ]]; then
    echo "ERROR: Unexpected kernel commit:"
    echo "       ${ACTUAL_COMMIT}"
    exit 1
fi

# ------------------------------------------------------------
# 6. Copy current Ubuntu kernel configuration
# ------------------------------------------------------------

CURRENT_CONFIG="/boot/config-$(uname -r)"

if [[ ! -f "${CURRENT_CONFIG}" ]]; then
    echo "ERROR: Cannot find ${CURRENT_CONFIG}"
    exit 1
fi

echo "[INFO] Copying configuration from:"
echo "       ${CURRENT_CONFIG}"

cp "${CURRENT_CONFIG}" .config

echo "[INFO] Updating configuration for this kernel..."
make olddefconfig

# ------------------------------------------------------------
# 7. Generate module-signing certificate
# ------------------------------------------------------------

echo "[INFO] Generating kernel signing certificate..."

mkdir -p certs

if [[ ! -f certs/mycert.pem ]]; then
    openssl req \
        -x509 \
        -newkey rsa:4096 \
        -keyout certs/mycert.pem \
        -out certs/mycert.pem \
        -nodes \
        -days 3650 \
        -subj "/CN=TRAFFICSPLITTER Artifact Kernel/"
else
    echo "[INFO] certs/mycert.pem already exists; keeping existing certificate."
fi

# ------------------------------------------------------------
# 8. Configure signing keys
# ------------------------------------------------------------

echo "[INFO] Configuring kernel signing options..."

./scripts/config \
    --set-str SYSTEM_TRUSTED_KEYS "certs/mycert.pem"

./scripts/config \
    --set-str MODULE_SIG_KEY "certs/mycert.pem"

# Resolve any dependencies introduced by the changes.
make olddefconfig

# ------------------------------------------------------------
# 9. Save provenance
# ------------------------------------------------------------

git rev-parse HEAD > ../kernel-commit.txt
cp .config ../kernel.config

echo
echo "============================================================"
echo " Kernel preparation completed"
echo "============================================================"
echo
echo "Kernel commit:"
cat ../kernel-commit.txt
echo
echo "Next step:"
echo "  ./02-build-install-kernel.sh"
