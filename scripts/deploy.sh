#!/usr/bin/env bash
# Host-side deployment. Never mounts reader storage on both systems at once.
set -euo pipefail
action=${1-}
device=${2:-root@192.168.205.1}
book_mount=${3-}
case "$action" in temporary|install|uninstall) ;; *) echo "Usage: $0 temporary|install|uninstall [root@host] [book-mount]" >&2; exit 2 ;; esac
[[ $device =~ ^root@[a-zA-Z0-9_.:-]+$ ]] || { echo "Expected root@host" >&2; exit 2; }
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
binary_dir=${R1_BINARY_DIR:-$project/build}
if [[ -z ${R1_BINARY_DIR-} && ! -f $binary_dir/r1-controller ]]; then
    binary_dir=$project
fi
ssh_options=(-o BatchMode=yes -o ConnectTimeout=5 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa)
# Commands use fixed paths or the validated staging token; expansion is local.
# shellcheck disable=SC2029
remote() { ssh "${ssh_options[@]}" "$device" "$@"; }
upload() { scp -O "${ssh_options[@]}" "$1" "$device:$stage/$2"; }
download() { scp -O "${ssh_options[@]}" "$device:$stage/$1" "$2"; }

if [[ $action != temporary ]]; then
    [[ -n $book_mount && -d $book_mount/applications && ! -L $book_mount/applications ]] || {
        echo "Pass the mounted reader storage (containing applications/)" >&2; exit 1;
    }
    book_mount=$(realpath -- "$book_mount")
    [[ $(findmnt -n -o FSTYPE --target "$book_mount") == vfat &&
       $(findmnt -n -o LABEL --target "$book_mount") == PB700K3 &&
       $(findmnt -n -o TARGET --target "$book_mount") == "$book_mount" ]] || {
        echo "Expected the mounted PB700K3 FAT storage" >&2; exit 1;
    }
    for name in 'R1 Controller.app' 'R1 Stop.app'; do
        file=$book_mount/applications/$name
        if [[ $action == install ]]; then
            [[ ! -e $file && ! -L $file ]] || { echo "Refusing to overwrite $file" >&2; exit 1; }
        else
            cmp -s "$file" "$project/packaging/applications/$name" || {
                echo "Missing or modified launcher: $file; refusing removal" >&2; exit 1;
            }
        fi
    done
fi

# Keep recovery data on the host, outside the public repository.
mkdir -p "$project/LOCAL_ARTIFACTS/install-backups"
backup=$(mktemp -d "$project/LOCAL_ARTIFACTS/install-backups/$action-XXXXXX")
echo "Recovery files: $backup"
remote 'cat /proc/mounts; ls -ld /ebrmain; /bin/busybox df -k /ebrmain' > "$backup/device-before.log"
stage=$(remote 'umask 077; /bin/busybox mktemp -d /tmp/r1-deploy.XXXXXX')
[[ $stage =~ ^/tmp/r1-deploy\.[a-zA-Z0-9]+$ ]] || { echo "Invalid remote staging response" >&2; exit 1; }
echo "$stage" > "$backup/remote-stage.txt"
# Remote staging is deliberately retained for recovery; no broad cleanup.
upload "$project/scripts/install-device.sh" install-device.sh
remote "/bin/busybox sh -n '$stage/install-device.sh'"

if [[ $action != uninstall ]]; then
    upload "$binary_dir/r1-controller" r1-controller
    download r1-controller "$backup/r1-controller"
    cmp "$binary_dir/r1-controller" "$backup/r1-controller"
    remote "/bin/busybox chmod 700 '$stage/r1-controller'"
fi
if [[ $action == temporary ]]; then
    # Do not replace a running or unrecognized binary. Preserve the existing
    # manual temporary workflow, lock, log and startup options.
    remote "
      if [ -e /tmp/r1-controller ] || [ -L /tmp/r1-controller ]; then
        [ ! -L /tmp/r1-controller ] &&
        [ \"\$(/bin/busybox stat -c '%u:%a' /tmp/r1-controller)\" = 0:700 ] &&
        [ \"\$(/tmp/r1-controller --status)\" = 'R1 controller is stopped' ] || exit 1
        /bin/busybox cp /tmp/r1-controller '$stage/previous-temporary' || exit 1
      fi
      /bin/busybox mv '$stage/r1-controller' /tmp/r1-controller &&
      /tmp/r1-controller --start && /tmp/r1-controller --status
    "
    exit
fi

if [[ $action == install ]]; then
    upload "$binary_dir/r1-launcher" r1-launcher
    download r1-launcher "$backup/r1-launcher"
    cmp "$binary_dir/r1-launcher" "$backup/r1-launcher"
    upload "$project/VERSION" VERSION
else
    cp -- "$book_mount/applications/R1 Controller.app" "$book_mount/applications/R1 Stop.app" "$backup/"
fi
remote "/bin/busybox sh '$stage/install-device.sh' '$action' '$stage'" | tee "$backup/result.log"
if [[ $action == install ]]; then
    # Publish only these two launchers; all root-sensitive paths are immutable
    # to reader. A failed copy leaves the component installed for recovery.
    cp -- "$project/packaging/applications/R1 Controller.app" "$book_mount/applications/R1 Controller.app"
    cp -- "$project/packaging/applications/R1 Stop.app" "$book_mount/applications/R1 Stop.app"
    echo "Installed. Safely eject USB, then use R1 Controller / R1 Stop in Applications."
else
    download installed-backup.tar "$backup/installed-backup.tar"
    rm -- "$book_mount/applications/R1 Controller.app" "$book_mount/applications/R1 Stop.app"
    echo "Uninstalled. Temporary deployment and its settings were preserved."
fi
sync -f "$book_mount"
