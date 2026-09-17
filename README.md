# PocketBook R1 Controller

A Magicsee R1 button mapper with a visible pointer for PocketBook Era Color.
The initial release is **0.1.0**. It runs temporarily through root SSH; it does
not install a system service or start automatically after a reboot.

## Compatibility and limitations

- Tested hardware: PocketBook Era Color (`PocketBook700k3`), firmware
  `6.10.2359`, ARMv7, with a Bluetooth-connected Magicsee R1.
- Supported display: normal portrait orientation, 1264 x 1680. Other devices,
  firmware versions and orientations are not supported.
- Residual pointer images can remain on book covers. This is a known limitation.
- Clicking, opening the reader menu with C, and selecting Dictionary have been
  verified on the device. Extended reading, controller reconnection and M+B
  navigation still need device validation of the packaged application.
- Root is required to access input devices. Stock `.app` launchers run as
  `reader` and cannot provide that access. This project does not modify device
  permissions, system libraries or system partitions.

## Build and test

Requirements: GNU make, GCC with ASan/UBSan, GNU coreutils, GNU tar, and
`arm-linux-gnueabi-gcc` for the ARM build. Use the **soft-float** toolchain;
`arm-linux-gnueabihf-gcc` is not compatible with this firmware's InkView ABI.

```bash
make test
make all
make package
```

The outputs are `build/r1-controller` and
`build/r1-controller-0.1.0.tar.gz`. The archive contains the binary and
documentation under `r1-controller-0.1.0/`, without an installer.
[VERSION](VERSION) is the build's version source.

Host tests require neither a connected reader nor root. They use ASan/UBSan
and finish in seconds; long scenarios use virtual time. Each test has a
15-second limit, configurable with `TEST_TIMEOUT`. Initial compilation is
additional. `tests/renderer-baseline.sha256` guards the established rendering
and input headers against unintended changes.

The build exports the process-local EPDC hook with `--export-dynamic` and loads
the device's `/usr/lib/libinkview.so`. It does not require or distribute the
proprietary SDK library.

## Temporary deployment

Run these commands from the project directory on your computer, with the reader
connected over USB. Stop any previous instance and confirm it has stopped before
replacing its binary. See the status and stop commands below.
When using an extracted release archive, replace `build/r1-controller` with
`./r1-controller` in the upload command.

```bash
scp -O -o ConnectTimeout=10 \
  -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  build/r1-controller root@192.168.205.1:/tmp/r1-controller
ssh -o ConnectTimeout=10 \
  -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  root@192.168.205.1 'chmod 700 /tmp/r1-controller && /tmp/r1-controller --start'
```

The legacy SSH options are scoped to these commands for the reader's older SSH
server. Do not enable them globally.

`--start` waits up to five seconds for the background process to open its log,
acquire its lock and record its PID. Success means it is ready to wait for USB
disconnection and the controller, not that the pointer is already visible.
Startup errors and timeouts return a nonzero status and cancel the failed child.

Safely eject the reader's exported storage on your computer, disconnect USB,
turn on the paired R1, and select M+D. The application waits for USB disconnection,
the controller and a live reader application before taking the screen.
Reconnecting USB ends the session; run `--start` again for another session.

```bash
ssh -o ConnectTimeout=10 \
  -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  root@192.168.205.1 '/tmp/r1-controller --status'
ssh -o ConnectTimeout=10 \
  -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  root@192.168.205.1 '/tmp/r1-controller --stop'
```

`--stop` requests shutdown; use `--status` to confirm completion. `--run` runs in
the foreground for diagnostics. `--version` prints the application version.
There is no session time limit in the application modes.

The device log is `/tmp/r1-controller.log`, rotated at about 2 MiB with one
previous copy (`.1`). The temporary binary and logs are lost on reboot.

## Controls

| Control | Action |
| --- | --- |
| M+D joystick | Move the pointer |
| M+D circle | Click at the pointer; the library may require a double click |
| C | Stock top-button action; opens the menu in the reader |
| Return | Back; hold for at least one second and release to stop the mapper |
| A / B | Hardware page keys 104 / 109; direction depends on stock assignments |
| D | Back, using hardware key 158 |
| M+B joystick / circle | Addressed InkView arrows / OK; application-dependent |

The pointer hides after a click so the actual application can respond. Move
the joystick to resume it after the application is ready. Move the joystick
first after switching modes to disambiguate button codes. See
[the input map](docs/KEY_MAP.md) for raw events and mapping details.

## Repository layout

- `src/`: application and device-specific rendering/input helpers.
- `tests/`: host-side regression tests and renderer baseline checksums.
- `docs/`: hardware input map and application mappings.
- `build/`: generated binaries and release archives, ignored by Git.
- `LOCAL_ARTIFACTS/`: private development history, logs, captures and older
  experiments, ignored by Git and excluded from release archives.

Public source, documentation and filenames use English. Local development
notes are not required to build or use the application.
