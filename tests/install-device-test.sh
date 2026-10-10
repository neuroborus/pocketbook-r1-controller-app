#!/usr/bin/env bash
# Run the actual installer against a private filesystem fixture and a mock
# BusyBox. No root, network, device access, real mounts or wall-clock sleeps.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
# Check the literal arithmetic syntax, without expanding it on the host.
# shellcheck disable=SC2016
if grep -F '$((' "$project/scripts/install-device.sh"; then
    echo 'Device BusyBox has shell arithmetic disabled; use its expr applet' >&2
    exit 1
fi
fixture=$(mktemp -d "${TMPDIR:-/tmp}/r1-install-test.XXXXXX")
cleanup() {
    for dir in "$fixture/ebrmain/r1-controller" "$fixture/ebrmain/.r1-controller-install"; do
        if [[ -d $dir ]]; then
            rm -f -- "$dir/r1-controller" "$dir/r1-launcher" "$dir/VERSION" "$dir/INSTALLATION"
            rmdir -- "$dir"
        fi
    done
    rm -f -- "$fixture/stage.test/r1-controller" "$fixture/stage.test/r1-launcher" \
        "$fixture/stage.test/VERSION" "$fixture/stage.test/installed-backup.tar" \
        "$fixture/busybox" "$fixture/install.sh" "$fixture/mounts" "$fixture/passwd" \
        "$fixture/usb" "$fixture/temporary" "$fixture/actions" "$fixture/mapper-state"
    rmdir -- "$fixture/stage.test" "$fixture/ebrmain" "$fixture"
}
trap cleanup EXIT
mkdir "$fixture/ebrmain" "$fixture/stage.test"
printf 'reader:*:101:101:reader:/:/bin/sh\n' > "$fixture/passwd"
printf 'CONFIGURED\n' > "$fixture/usb"
printf 'temporary deployment stays intact\n' > "$fixture/temporary"
printf 'device %s/ebrmain ext2 ro,relatime 0 0\n' "$fixture" > "$fixture/mounts"
cat > "$fixture/busybox" <<'SH'
#!/bin/sh
set -eu
action=$1
shift
case "$action" in
    id) echo 0 ;;
    uname) echo armv7l ;;
    stat) echo 0:700 ;;
    df) printf 'Filesystem 1K-blocks Used Available Use%% Mounted\ndevice 10000 10 9990 1%% system\n' ;;
    chown|usleep) : ;;
    sync) [ "${R1_FAIL_STEP-}" != sync ] ;;
    mount)
        echo "$*" >> "$R1_FIXTURE/actions"
        mode=ro
        case "$2" in
            remount,rw)
                [ "${R1_FAIL_STEP-}" != remount ] || exit 1
                mode=rw ;;
        esac
        printf 'device %s/ebrmain ext2 %s,relatime 0 0\n' "$R1_FIXTURE" "$mode" > "$R1_FIXTURE/mounts"
        ;;
    cp)
        [ "${R1_FAIL_STEP-}" != copy ] || exit 1
        exec /usr/bin/cp "$@" ;;
    awk|cat|grep|mkdir|chmod|mv|rm|rmdir|tar|expr) exec "/usr/bin/$action" "$@" ;;
    *) echo "Unexpected fixture command: $action" >&2; exit 1 ;;
esac
SH
cat > "$fixture/stage.test/r1-controller" <<'SH'
#!/bin/sh
case "$1" in
    --version) echo 'R1 Controller 0.1.0' ;;
    --status)
        state=$(cat "$R1_FIXTURE/mapper-state" 2>/dev/null || echo stopped)
        if [ "$state" = stopped ]; then echo 'R1 controller is stopped'
        else echo 'R1 controller pid=123 running'; fi
        if [ "$state" = stopping ]; then echo stopped > "$R1_FIXTURE/mapper-state"; fi
        ;;
    --stop) echo stopping > "$R1_FIXTURE/mapper-state" ;;
    *) exit 1 ;;
esac
SH
cp "$fixture/stage.test/r1-controller" "$fixture/stage.test/r1-launcher"
printf '0.1.0\n' > "$fixture/stage.test/VERSION"
chmod 700 "$fixture/busybox" "$fixture/stage.test/r1-controller" "$fixture/stage.test/r1-launcher"
sed -e "s|/bin/busybox|$fixture/busybox|g" \
    -e "s|/ebrmain|$fixture/ebrmain|g" \
    -e "s|/tmp/r1-deploy\.|$fixture/stage.|g" \
    -e "s|/proc/mounts|$fixture/mounts|g" \
    -e "s|/etc/passwd|$fixture/passwd|g" \
    -e "s|/sys/class/android_usb/android0/state|$fixture/usb|g" \
    "$project/scripts/install-device.sh" > "$fixture/install.sh"
export R1_FIXTURE=$fixture
run() { sh "$fixture/install.sh" "$1" "$fixture/stage.test"; }
is_ro() { grep -q ' ext2 ro,' "$fixture/mounts"; }

run install
is_ro
[[ $(stat -c '%a' "$fixture/ebrmain/r1-controller/r1-launcher") == 4755 ]]
[[ $(stat -c '%a' "$fixture/ebrmain/r1-controller/r1-controller") == 700 ]]
cmp "$fixture/stage.test/VERSION" "$fixture/ebrmain/r1-controller/VERSION"
if run install; then echo 'Existing installation was overwritten' >&2; exit 1; fi
is_ro
printf 'running\n' > "$fixture/mapper-state"
run uninstall
is_ro
[[ ! -e $fixture/ebrmain/r1-controller ]]
tar -tf "$fixture/stage.test/installed-backup.tar" | grep -q '^r1-controller/r1-launcher$'
for failure in copy remount; do
    if R1_FAIL_STEP=$failure run install; then echo "Expected $failure failure" >&2; exit 1; fi
    is_ro
    [[ ! -e $fixture/ebrmain/r1-controller && ! -e $fixture/ebrmain/.r1-controller-install ]]
done
if R1_FAIL_STEP=sync run install; then echo 'Expected sync failure' >&2; exit 1; fi
is_ro # A failed sync must not skip restoration of the read-only mount.
run uninstall
is_ro
[[ $(cat "$fixture/temporary") == 'temporary deployment stays intact' ]]
printf 'PASS: install/remove, overwrite refusal, backup, copy/remount rollback, read-only restoration even after sync failure and temporary preservation; no root or hardware\n'
