MODDIR=${0%/*}/..

export TMP_PATH=/sbin
[ -d /sbin ] || export TMP_PATH=/debug_ramdisk

exec $MODDIR/bin/r0z-trace64 ctl $*
