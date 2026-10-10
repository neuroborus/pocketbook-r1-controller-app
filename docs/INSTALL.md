# Deployment

Temporary and persistent deployment are independent options. Both use the same
mapper, controls, motion curve, log and exclusive lock. Do not run both at once.
These instructions are specific to PocketBook700k3 firmware 6.10.2359.

## Temporary deployment

The manual root SSH commands in the README remain supported. Alternatively:

```bash
make deploy-temporary
```

This uploads the binary to `/tmp/r1-controller`, verifies a downloaded copy,
and starts it waiting for USB disconnection. It refuses to replace a running
or unrecognized existing binary. It does not modify any persistent files.
Stop a previous temporary session using the README's `--stop` command first.
The binary is lost on reboot. An existing replaced binary is kept in the
private remote staging directory until reboot.

## Persistent deployment

The host needs Bash, OpenSSH (`ssh` and `scp`), GNU coreutils and util-linux
(`findmnt`). Build with `make all`. Connect USB and mount the reader's exported
PB700K3 storage on the computer using the desktop file manager. Do not mount it
on the reader while it is exported to the computer.

Use the actual mount path on your system:

```bash
make install BOOK_MOUNT="/media/$USER/PB700K3"
```

Override `DEVICE=root@host` if the reader's SSH address differs. From an
extracted release archive, use the bundled script without rebuilding:

```bash
bash scripts/deploy.sh install root@192.168.205.1 "/media/$USER/PB700K3"
```

Installation creates only:

- `/ebrmain/r1-controller/`: a root-owned directory containing the mapper,
  a restricted setuid launcher, `VERSION` and an installation marker.
- `applications/R1 Controller.app` and `applications/R1 Stop.app` on the
  exported storage: two launchers for the stock Applications screen.

The system partition `/ebrmain` is temporarily remounted writable and restored
to read-only on normal completion or a handled error. The root component is
staged before publishing; a failed component installation removes only its own
new files. Existing installations or launcher names are never overwritten.

The small setuid launcher accepts only `--start`, `--stop`, `--status` and
`--version`, from root or the stock reader account (UID 101). It verifies a
root-owned, non-writable installation, drops supplementary groups, establishes
root credentials, unblocks signals, closes extra descriptors and executes one
fixed binary with a minimal environment. It does not accept arbitrary commands or paths. The
mapper itself is **not** setuid. No sudoers entries, input-device permissions,
system libraries, startup scripts or reader settings are changed.
The two application scripts execute this helper directly; there is no extra
InkView frontend or persistent supervisor.

After installation, safely eject the storage and disconnect USB. Stop any
temporary session before starting the installed application. Use **R1 Controller**
in Applications to start and **R1 Stop** (or long Return) to stop. Power on the
paired R1 and select M+D. No automatic startup is installed: launch the application
again after a reboot or a USB session. The files survive reboot; a firmware
upgrade may replace them.

Helper start/stop and permissions are checked on the device. Repeated icon
launching must be checked outside USB storage mode: its task registry can retain
dead entries, so USB-side `OpenTask` probes do not establish normal GUI behavior.

Installation does not start, stop or delete an existing temporary session.
The temporary command, binary and configuration remain available.

## Removal and recovery

Connect USB, mount PB700K3 on the computer, then run:

```bash
make uninstall BOOK_MOUNT="/media/$USER/PB700K3"
```

Or use `bash scripts/deploy.sh uninstall root@192.168.205.1 /path/to/PB700K3`
from an extracted archive. Removal stops only the persistent mapper when it
owns the lock. It refuses to remove modified application launchers. The exact
root component is backed up before removal; the temporary binary, log and
settings are not removed.

Each deployment prints its host recovery directory under
`LOCAL_ARTIFACTS/install-backups/`, including readback binaries, preflight state
and a remote staging path. Removal also saves the component archive and the two
application launchers. Remote staging is retained until reboot. Keep the host
backup before disconnecting. No general-purpose recursive deletion is used.

USB loss, power failure or failure while copying the two storage launchers can
leave a partial installation. Keep USB connected, inspect the printed recovery
files and restore missing launchers or remove the installed component. Do not
blindly overwrite files or repeat a failed system remount. If restoration of
read-only mode fails, the installer reports it explicitly. Avoid rebooting or
disconnecting power during installation/removal.
