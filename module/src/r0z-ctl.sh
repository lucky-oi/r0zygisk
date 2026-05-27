MODDIR=${0%/*}
MODDIR=${MODDIR%/bin}

export TMP_PATH="$MODDIR"

case "$1" in
  denylist-policy)
    POLICY_FILE="$MODDIR/denylist_policy"
    case "$2" in
      force|"")
        printf 'force\n' > "$POLICY_FILE"
        echo "denylist policy: force"
        ;;
      just-umount|just_umount|mount-only|mount_only)
        printf 'just_umount\n' > "$POLICY_FILE"
        echo "denylist policy: just_umount"
        ;;
      status)
        if [ -f "$POLICY_FILE" ]; then
          echo "denylist policy: $(cat "$POLICY_FILE")"
        else
          echo "denylist policy: force (default)"
        fi
        ;;
      *)
        echo "usage: $0 denylist-policy [force|just-umount|status]" >&2
        exit 1
        ;;
    esac
    exit 0
    ;;
esac

exec "$MODDIR/bin/r0zd" "$@"
