#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

# a) Parse outdir argument (default /tmp/aeld) and make absolute
if [ $# -lt 1 ]; then
    echo "Using default directory ${OUTDIR} for output"
else
    OUTDIR=$1
    echo "Using passed directory ${OUTDIR} for output"
fi

OUTDIR=$(realpath "${OUTDIR}")
echo "OUTDIR absolute: ${OUTDIR}"

# b) Create outdir if it doesn’t exist
mkdir -p "${OUTDIR}"
cd "${OUTDIR}"

# c) Clone kernel if needed, checkout tag, build Image
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
    git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION} linux-stable
fi

cd "${OUTDIR}/linux-stable"
echo "Checking out version ${KERNEL_VERSION}"
git checkout ${KERNEL_VERSION}

echo "Building kernel Image"
make mrproper
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
make -j"$(nproc)" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image

# d) Copy Image to outdir
cp -f "arch/${ARCH}/boot/Image" "${OUTDIR}/Image"
echo "Copied kernel Image to ${OUTDIR}/Image"

# Stop here for now (we'll do rootfs later)
exit 0
