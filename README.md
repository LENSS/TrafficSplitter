# Introduction

This document describes introductions for the NDSS Artifact Evaluation (AE) process. We provide the VMs as OVA files for the reproduction of evaluations and simple test runs. If you use OVA files, you can skip VM setup steps (1)-(3) and proceed from (4).

- The VM setup consists of two main steps: (1) installing Ubuntu 24.04 Desktop on VirtualBox and (2) installing the custom Linux kernel. We use Ubuntu 24.04 Desktop rather than the Server edition to make the overall evaluation process easier and more user-friendly.
- Our system uses a non-mainline Linux kernel. We began developing the system when the MPTCP eBPF scheduler was still an experimental feature and had not yet been included in the mainline Linux kernel. Since then, the MPTCP eBPF scheduler has become an official kernel feature. However, we continue to use the older custom kernel because (1) the eBPF-related structures have changed in newer kernel versions, and (2) our system has already been extensively tested and is stable with the kernel version used during development and evaluation.

# (1) Ubuntu Installation

1. Download the Ubuntu 24.04 Desktop image from:
   - https://releases.ubuntu.com/noble/

2. Create VirtualBox VM.
   - We recommend allocating at least **50 GB of storage, 3 CPU cores, and 8 GB of RAM**. These resources are mainly required for the kernel compilation process. After the kernel has been successfully compiled and installed, you can reduce the allocated resources if needed.

3. Install Ubuntu 24.04 Desktop on the VM.

# (2) Kernel Installation

Please note that we have shell scripts for the following procedure. You may want to simply run the scripts.

1. Using Git, clone the kernel repository and check out the exact version used in our artifact.

   ```bash
   git clone https://github.com/multipath-tcp/mptcp_net-next.git
   cd mptcp_net-next
   git fetch origin
   git checkout 4d907d0e9f974e706ad6f916b8bf2391d82573bf
   ```

2. Update the Ubuntu APT source configuration.

   Open the APT source file:

   ```bash
   sudo vim /etc/apt/sources.list.d/ubuntu.sources
   ```

   Replace or update the contents so that both binary packages (`deb`) and source packages (`deb-src`) are enabled, and make sure the required Ubuntu repositories are included:

   ```text
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
   ```

   This configuration enables access to both binary and source packages from the standard Ubuntu 24.04 repositories. The source repositories are required for installing some kernel build dependencies and related development packages.

3. Run `apt update` and upgrade.

   ```bash
   sudo apt update
   sudo apt upgrade
   ```

4. Install the packages for the build environment.

   ```bash
   sudo apt-get build-dep linux linux-image-unsigned-$(uname -r)
   sudo apt-get install libncurses-dev gawk flex bison openssl        libssl-dev dkms libelf-dev libudev-dev libpci-dev libiberty-dev        autoconf
   ```

5. Copy the existing kernel configuration into the `mptcp_net-next` directory.

   ```bash
   cp /boot/config-(your current kernel version) .config
   make olddefconfig
   ```

6. Generate a self-signed certificate for kernel module signing. From the `mptcp_net-next` directory, run:

   ```bash
   openssl req -x509 -newkey rsa:4096 -keyout certs/mycert.pem -out certs/mycert.pem -nodes -days 3650
   ```

7. Update the kernel configuration to use the generated certificate.

   Open the kernel configuration file:

   ```bash
   sudo vim .config
   ```

   Find the following options:

   ```text
   CONFIG_SYSTEM_TRUSTED_KEYS
   CONFIG_MODULE_SIG_KEY
   ```

   Update them to reference the certificate generated in the previous step:

   ```text
   CONFIG_SYSTEM_TRUSTED_KEYS="certs/mycert.pem"
   CONFIG_MODULE_SIG_KEY="certs/mycert.pem"
   ```
![cert config](img/.png)
8. Compile and install the kernel (It takes some time....)

   ```bash
   make -j$(nproc)
   sudo make modules_install
   sudo make install
   ```

9. Configure GRUB to boot the desired kernel version.

   Open the GRUB configuration file:

   ```bash
   sudo vim /etc/default/grub
   ```

   Make the following changes:

   ```text
   GRUB_DEFAULT=saved
   GRUB_SAVEDEFAULT=true
   GRUB_TIMEOUT_STYLE=menu
   GRUB_TIMEOUT=10
   ```

   After making these changes, update the GRUB configuration:

   ```bash
   sudo update-grub
   ```

   On the next reboot, select the desired kernel from the GRUB menu. GRUB should remember this selection for subsequent boots.

# (3) TrafficSplitter Build

1. Clone our git repository.

   ```bash
   git clone https://github.com/LENSS/TrafficSplitter.git
   ```

2. Move to the `src` directory and run scripts to build BPF tools, MPTun proxy servers/clients, and MPTCP schedulers.

   ```bash
   cd TrafficSplitter/src/
   sudo chmod +x ./scripts/*.sh
   sudo ./scripts/01-install-bpf-tools.sh
   sudo ./scripts/02-generate-vmlinux-header.sh
   sudo ./scripts/03-build-schedulers.sh
   sudo ./scripts/07-build-mptun.sh
   ```

3. You can check the resulting files.

   ```bash
   ls MPTun_proxy/build/ Saflo_scheduler/build/
   ```

4. Optionally, you can remove the `bpftool` files as it is already installed.

   ```bash
   sudo rm tools/ -r
   ```

# (4) Before Start Evaluations

> If you are using the provided OVA files, the VM password is `ndss2027`.

We assume that you have already installed VirtualBox and prepared the two VMs by either following `AE-VMsetup.pdf` or importing the OVA files provided with the artifact. First, create a VirtualBox NAT Network named `aeNet` for communication between the two VMs.

## Linux Host

Open a terminal and run:

```bash
VBoxManage natnetwork add   --netname aeNet   --network "192.168.10.0/24"   --enable   --dhcp on

VBoxManage natnetwork start --netname aeNet

VBoxManage dhcpserver modify   --network aeNet   --server-ip 192.168.10.2   --lower-ip 192.168.10.10   --upper-ip 192.168.10.254   --netmask 255.255.255.0   --enable

VBoxManage dhcpserver restart --network aeNet
```

## Window Host

Open Command Prompt or PowerShell and run:

```text
VBoxManage.exe natnetwork add --netname aeNet --network "192.168.10.0/24" --enable --dhcp on
VBoxManage.exe natnetwork start --netname aeNet
VBoxManage.exe dhcpserver modify --network aeNet --server-ip 192.168.10.2 --lower-ip 192.168.10.10 --upper-ip 192.168.10.254 --netmask 255.255.255.0 --enable
VBoxManage.exe dhcpserver restart --network aeNet
```

If Windows reports that `VBoxManage.exe` is not recognized, run the commands from the VirtualBox installation directory (e.g., `C:\Program Files\Oracle\VirtualBox`).

Then, assign (or check) the network adapters of each VM. Please note that the user (i.e., client) has to have two network adapters, while the proxy server needs one.

- **User (Client):** Two NAT Network adapters, both attached to `aeNet`.
- **Proxy-server:** One NAT Network adapter attached to `aeNet`.

The screenshots on page 5 of the original document show the corresponding VirtualBox Network settings for the two client adapters and the single proxy-server adapter.

# To Do List

- MPTCP Setup on proxy and client
- Data Collection Scripts
  - collected trace visualization
- Scale-down Evaluation Scripts
