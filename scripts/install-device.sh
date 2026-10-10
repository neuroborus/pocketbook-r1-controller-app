#!/bin/sh
# Run only through root SSH, with a private upload directory in /tmp.
# The bb wrapper runs awk, and guard chains deliberately reject either failure.
# shellcheck disable=SC2016,SC2015
set -eu
bb() { /bin/busybox "$@"; }
die() { echo "$*" >&2; exit 1; }

action=${1-}
stage=${2-}
case "$action" in install|uninstall) ;; *) die "Expected install or uninstall" ;; esac
case "$stage" in /tmp/r1-deploy.*) ;; *) die "Invalid staging directory" ;; esac
[ "$(bb id -u)" = 0 ] || die "Root SSH is required"
[ "$(bb uname -m)" = armv7l ] || die "Unsupported architecture"
bb grep -q '^reader:[^:]*:101:101:' /etc/passwd || die "Unsupported reader account"
[ "$(bb cat /sys/class/android_usb/android0/state)" = CONFIGURED ] || die "Connect USB first"
[ "$(bb stat -c '%u:%a' "$stage")" = 0:700 ] || die "Unsafe staging permissions"
[ ! -L "$stage" ] || die "Staging directory must not be a symlink"
bb awk '$2 == "/mnt/ext1" { found = 1 } END { exit !found }' /proc/mounts &&
    die "Reader storage must not be mounted on the device"
mount_mode=$(bb awk '$2 == "/ebrmain" && $3 == "ext2" { print $4 }' /proc/mounts)
case ",$mount_mode," in *,ro,*) ;; *) die "Expected read-only /ebrmain ext2 mount" ;; esac
case ",$mount_mode," in *,nosuid,*) die "The system mount does not allow the launcher" ;; esac

target=/ebrmain/r1-controller
staging=/ebrmain/.r1-controller-install
created=0
published=0
complete=0
remounted=0
cleanup() {
    result=$?
    trap - 0 1 2 15
    if [ "$action" = install ] && [ "$complete" = 0 ] && [ "$created" = 1 ]; then
        rollback=$staging
        [ "$published" = 0 ] || rollback=$target
        # Only files created in this transaction, never recursive removal.
        bb rm -f "$rollback/r1-launcher" "$rollback/r1-controller" \
            "$rollback/VERSION" "$rollback/INSTALLATION" || result=1
        bb rmdir "$rollback" || result=1
    fi
    if [ "$remounted" = 1 ]; then
        bb sync || result=1
        if ! bb mount -o remount,ro /ebrmain; then
            echo "ERROR: could not restore /ebrmain to read-only" >&2
            result=1
        fi
    fi
    exit "$result"
}
trap cleanup 0
trap 'exit 1' 1 2 15

if [ "$action" = install ]; then
    [ ! -e "$target" ] && [ ! -L "$target" ] || die "Installation exists; uninstall before replacing it"
    [ ! -e "$staging" ] && [ ! -L "$staging" ] || die "Stale staging directory; inspect it manually"
    [ -f "$stage/r1-controller" ] && [ ! -L "$stage/r1-controller" ] || die "Missing mapper"
    [ -f "$stage/r1-launcher" ] && [ ! -L "$stage/r1-launcher" ] || die "Missing launcher"
    [ -f "$stage/VERSION" ] && [ ! -L "$stage/VERSION" ] || die "Missing version"
    version=$(bb cat "$stage/VERSION")
    [ "$("$stage/r1-controller" --version)" = "R1 Controller $version" ] || die "Binary version mismatch"
    [ "$(bb df -k /ebrmain | bb awk 'NR == 2 { print $4 }')" -ge 4096 ] || die "Insufficient system space"
else
    [ -d "$target" ] && [ ! -L "$target" ] || die "No installation found"
    [ "$(bb cat "$target/INSTALLATION")" = pocketbook-r1-controller:1 ] || die "Unknown installation"
    # Preserve a complete copy, including original modes, before removal.
    bb tar -cf "$stage/installed-backup.tar" -C /ebrmain r1-controller
    state=$("$target/r1-controller" --status) || {
        [ "$state" = 'Another controller/test owns the lock; not signalling it' ] || die "Cannot inspect mapper state"
    }
    case "$state" in
        *' running')
            "$target/r1-controller" --stop
            attempt=0
            while [ "$("$target/r1-controller" --status)" != 'R1 controller is stopped' ]; do
                attempt=$(bb expr "$attempt" + 1)
                [ "$attempt" -lt 50 ] || die "Mapper has not stopped; refusing removal"
                bb usleep 100000
            done
            ;;
    esac
fi

remounted=1
bb mount -o remount,rw /ebrmain
if [ "$action" = install ]; then
    bb mkdir -m 700 "$staging"
    created=1
    bb cp "$stage/r1-controller" "$stage/r1-launcher" "$stage/VERSION" "$staging/"
    echo pocketbook-r1-controller:1 > "$staging/INSTALLATION"
    bb chown 0:0 "$staging" "$staging/r1-controller" "$staging/r1-launcher" \
        "$staging/VERSION" "$staging/INSTALLATION"
    bb chmod 700 "$staging/r1-controller"
    bb chmod 4755 "$staging/r1-launcher"
    bb chmod 644 "$staging/VERSION" "$staging/INSTALLATION"
    bb chmod 755 "$staging"
    bb mv "$staging" "$target"
    published=1
    "$target/r1-launcher" --version
else
    bb rm "$target/r1-launcher" "$target/r1-controller" "$target/VERSION" "$target/INSTALLATION"
    bb rmdir "$target"
fi
complete=1
echo "Persistent component $action complete; temporary files were not changed"
