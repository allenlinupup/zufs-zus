#!/bin/bash -e

export LC_ALL=C
unset CDPATH

SELF="${BASH_SOURCE[0]}"
SELFDIR=$(dirname ${SELF})
ZUFS_HOME=$(realpath ${SELFDIR}/../)
PMEM_DEV=/dev/pmem0
ZUF_MOUNT=/sys/fs/zuf/
TOYFS_MOUNT=/mnt/toyfs/
UUID=$(uuid)

debug_on() {
	echo -n "module zuf +p" > "/sys/kernel/debug/dynamic_debug/control"
}

echo "Prepare zufs"
stat ${PMEM_DEV}
mkdir -p ${TOYFS_MOUNT}
sysctl -w kernel.sched_rt_runtime_us=-1
modprobe pmem
ln -s ${PMEM_DEV} /dev/disk/by-uuid/${UUID}
sleep 3

echo 3 > /proc/sys/vm/drop_caches
insmod ${ZUFS_HOME}/zuf/fs/zuf/zuf.ko
mount -t zuf nodev ${ZUF_MOUNT}
sleep 3

echo "mkfs toyfs"
${ZUFS_HOME}/zus/mkfs.toyfs ${PMEM_DEV} ${UUID}
sleep 3

echo "Running zus"
#debug_on
${ZUFS_HOME}/zus/zus --numcpu=$(nproc) ${ZUF_MOUNT} &
sleep 3

echo "Mount toyfs"
mount -t toyfs ${PMEM_DEV} ${TOYFS_MOUNT}
sleep 3

# Wait for child zus
wait




