#!/system/bin/sh

MODDIR=${0%/*}
if [ "$ZYGISK_ENABLED" = "1" ]; then
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

export TMP_PATH="$MODDIR"
[ "$DEBUG" = true ] && export RUST_BACKTRACE=1

BRIDGE_PROP="ro.dalvik.vm.native.bridge"
BRIDGE_LOADER="libzn_loader.so"

set_native_bridge() {
  if command -v resetprop >/dev/null 2>&1; then
    resetprop "$BRIDGE_PROP" "$BRIDGE_LOADER"
  fi
}

hide_native_bridge() {
  if command -v resetprop >/dev/null 2>&1; then
    resetprop "$BRIDGE_PROP" 0
  fi
}

set_native_bridge

./bin/r0zd daemon </dev/null >/dev/null 2>&1 &
