# Author: Madhav Appanaboyina
# Brief: Script to build and install the kernel

#!/bin/bash
set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_REPO=git://busybox.net/busybox.git
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

# make /tmp/aeld as the default argument and absolute path
if [ $# -ge 1 ]; then
  OUTDIR=$1
fi
OUTDIR=$(realpath "${OUTDIR}")


mkdir -p "${OUTDIR}"

#steps to build the kernel image
if [ ! -d "${OUTDIR}/linux-stable" ]; then
  cd "${OUTDIR}"
  git clone "${KERNEL_REPO}" --depth 1 --single-branch --branch "${KERNEL_VERSION}" linux-stable
fi

if [ ! -e "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" ]; then
  cd "${OUTDIR}/linux-stable"
  git checkout "${KERNEL_VERSION}"
  make mrproper
  make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" defconfig
  make -j"$(nproc)" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" Image
fi

# copy Image to outdir
cp -f "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}/Image"

# build root filesystem in outdir/rootfs
if [ -d "${OUTDIR}/rootfs" ]; then
  sudo rm -rf "${OUTDIR}/rootfs"
fi

mkdir -p "${OUTDIR}/rootfs"
cd "${OUTDIR}/rootfs"
mkdir -p bin sbin etc proc sys dev lib lib64 tmp var usr/bin usr/sbin home home/conf
chmod 1777 tmp

# BusyBox clone/config/build/install
if [ ! -d "${OUTDIR}/busybox" ]; then
  cd "${OUTDIR}"
  git clone "${BUSYBOX_REPO}" busybox
fi

cd "${OUTDIR}/busybox"
git checkout "${BUSYBOX_VERSION}"
make distclean
make defconfig
make -j"$(nproc)" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}"
make CONFIG_PREFIX="${OUTDIR}/rootfs" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" install

# Add required shared libs, busybox
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)

LOADER=$(${CROSS_COMPILE}readelf -a "${OUTDIR}/busybox/busybox" | \
  grep "program interpreter" | awk -F': ' '{print $2}' | tr -d '[]')

if [ -z "${LOADER}" ]; then
  exit 1
fi

mkdir -p "${OUTDIR}/rootfs$(dirname "${LOADER}")"
cp -a "${SYSROOT}${LOADER}" "${OUTDIR}/rootfs${LOADER}"

LIBS=$(${CROSS_COMPILE}readelf -a "${OUTDIR}/busybox/busybox" | \
  grep "Shared library" | awk -F'[][]' '{print $2}')

for lib in ${LIBS}; do
  if [ -e "${SYSROOT}/lib/${lib}" ]; then
    cp -a "${SYSROOT}/lib/${lib}" "${OUTDIR}/rootfs/lib/"
  elif [ -e "${SYSROOT}/lib64/${lib}" ]; then
    cp -a "${SYSROOT}/lib64/${lib}" "${OUTDIR}/rootfs/lib64/"
  elif [ -e "${SYSROOT}/usr/lib/${lib}" ]; then
    mkdir -p "${OUTDIR}/rootfs/usr/lib"
    cp -a "${SYSROOT}/usr/lib/${lib}" "${OUTDIR}/rootfs/usr/lib/"
  elif [ -e "${SYSROOT}/usr/lib64/${lib}" ]; then
    mkdir -p "${OUTDIR}/rootfs/usr/lib64"
    cp -a "${SYSROOT}/usr/lib64/${lib}" "${OUTDIR}/rootfs/usr/lib64/"
  else
    exit 1
  fi
done

# device nodes
sudo mknod -m 666 "${OUTDIR}/rootfs/dev/null" c 1 3
sudo mknod -m 600 "${OUTDIR}/rootfs/dev/console" c 5 1

# cross compiling writer into /home in kernel
cd "${FINDER_APP_DIR}"
${CROSS_COMPILE}gcc -Wall -Werror -o writer writer.c
cp -a writer "${OUTDIR}/rootfs/home/"


# copy finder scripts and conf to /home in kernel
cp -a finder.sh finder-test.sh autorun-qemu.sh "${OUTDIR}/rootfs/home/"
cp -a conf/username.txt conf/assignment.txt "${OUTDIR}/rootfs/home/conf/"
sed -i 's|\.\./conf/assignment\.txt|conf/assignment.txt|g' "${OUTDIR}/rootfs/home/finder-test.sh"


# create initramfs
sudo chown -R root:root "${OUTDIR}/rootfs"
cd "${OUTDIR}/rootfs"
find . | cpio -H newc -ov --owner root:root | gzip > "${OUTDIR}/initramfs.cpio.gz"
