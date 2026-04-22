#!/system/bin/sh

MODDIR=${0%/*}
if [ "$ZYGISK_ENABLED" ]; then
  exit 0
fi

cd "$MODDIR"

if [ "$(which magisk)" ]; then
  for file in ../*; do
    if [ -d "$file" ] && [ -d "$file/zygisk" ] && ! [ -f "$file/disable" ]; then
      if [ -f "$file/post-fs-data.sh" ]; then
        cd "$file"
        log -p i -t "r0z-sh" "Manually trigger post-fs-data.sh for $file"
        sh "$(realpath ./post-fs-data.sh)"
        cd "$MODDIR"
      fi
    fi
  done
fi

create_sys_perm() {
  mkdir -p $1
  chmod 555 $1
  chcon u:object_r:system_file:s0 $1
}

export TMP_PATH=/sbin
[ -d /sbin ] || export TMP_PATH=/debug_ramdisk

create_sys_perm $TMP_PATH

if [ -f $MODDIR/lib64/libr0z.so ];then
  create_sys_perm $TMP_PATH/lib64
  cp $MODDIR/lib64/libr0z.so $TMP_PATH/lib64/libr0z.so
  chcon u:object_r:system_file:s0 $TMP_PATH/lib64/libr0z.so
fi

if [ -f $MODDIR/lib/libr0z.so ];then
  create_sys_perm $TMP_PATH/lib
  cp $MODDIR/lib/libr0z.so $TMP_PATH/lib/libr0z.so
  chcon u:object_r:system_file:s0 $TMP_PATH/lib/libr0z.so
fi

[ "$DEBUG" = true ] && export RUST_BACKTRACE=1
MODE_FILE="$MODDIR/r0z_mode"
if [ ! -f "$MODE_FILE" ]; then
  printf '%s\n' compat > "$MODE_FILE"
fi
./bin/r0z-trace64 monitor &
