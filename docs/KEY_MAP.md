# PocketBook Era Color + Magicsee R1 Input Map

## PocketBook Era Color

Device: `PocketBook700k3`

Firmware: `6.10.2359`

Observed raw physical button events:

| Physical button (top -> bottom) | Linux event | Device during capture |
|---|---:|---|
| Top / Menu-Power button | `KEY 116` | `/dev/input/event1` |
| 2nd physical button | `KEY 109` | `/dev/input/event4` |
| 3rd physical button | `KEY 104` | `/dev/input/event4` |
| Bottom physical button | `KEY 158` | `/dev/input/event4` |

Device names, which are more stable than event numbers:

- `pmu1736-powerkey` -> top button (`116`)
- `gpio_keys` -> `109`, `104`, `158`
- `fts_ts` -> touchscreen

> Note: event numbers change after reconnect/reboot. Discover devices by name.

## Magicsee R1

Device name: `Magicsee R1`

Transport: Bluetooth UHID (`Bus=0005`)

Captured initially as `/dev/input/event7`, later as `/dev/input/event8`/`event9`.

### Gamepad mode (`M + B`)

| Control | Linux input event |
|---|---|
| Joystick | `ABS_X (code 0)`, `ABS_Y (code 1)`; center = `1`, edges = `0 / 2` |
| Rear circle | `KEY 310` |
| Back / Return | `KEY 311` |
| A | `KEY 308` + companion `KEY 313` |
| B | `KEY 304` |
| C | `KEY 307` + companion `KEY 312` |
| D | `KEY 305` |

Joystick direction values observed during the session:

- X `0` -> left
- X `2` -> right
- X `1` -> center
- Y `0` -> up
- Y `2` -> down
- Y `1` -> center

### Mouse mode (`M + D`)

| Control | Linux input event |
|---|---|
| Joystick Up | `REL_Y (code 1) = -28` |
| Joystick Down | `REL_Y (code 1) = +28` |
| Joystick Left | `REL_X (code 0) = -28` |
| Joystick Right | `REL_X (code 0) = +28` |
| Rear circle | `BTN_LEFT / KEY 272` |
| Back / Return | `BTN_RIGHT / KEY 273` |
| A | `KEY 304` |
| B | `KEY 305` |
| C | `KEY 115` |
| D | `KEY 114` |

### Media mode (initial/default mode observed)

- `KEY 164` -> Play/Pause
- `KEY 165` -> Previous media
- `KEY 163` -> Next media
- `KEY 115` -> Volume Up
- `KEY 114` -> Volume Down

## Touchscreen capture

`fts_ts` emitted a standard multitouch sequence:

- `ABS_MT_TRACKING_ID` = code `57`
- `ABS_MT_TOUCH_MAJOR` = code `48`
- `ABS_MT_POSITION_X` = code `53`
- `ABS_MT_POSITION_Y` = code `54`
- `BTN_TOUCH` = code `330`

Example center-ish touch captured:

```text
ABS 57 = 73
ABS 48 = 64
ABS 53 = 632
ABS 54 = 893
KEY 330 = 1
...
ABS 57 = -1
KEY 330 = 0
```

## Application mapping

Short Return is Back; holding it for at least one second and releasing stops
the mapper.
C opens the stock book menu (Dictionary selection has been physically validated).
The new M+B path sends addressed InkView keypress/release events, not a virtual
UHID keyboard. Its stock-UI behavior still needs physical validation. Joystick
directions are edge-triggered, without hold repeat. Move the joystick first
after switching modes so ambiguous button codes are interpreted correctly.

Button mappings:

| R1 | Desired behavior |
|---|---|
| A | Previous page |
| B | Next page |
| Return | Back |
| C | Top PocketBook hardware action |
| D | Bottom PocketBook hardware action |
| `M+B` joystick | InkView Up/Down/Left/Right (`0x11/0x12/0x13/0x14`) |
| `M+B` rear circle | InkView OK (`0x0a`) |
| `M+D` joystick | Visible pointer movement |
| `M+D` rear circle | Click at pointer |

### A/B caveat

Page direction depends on the stock button assignments. The mapper sends:

- A -> `KEY 104`
- B -> `KEY 109`

This must be re-verified on-device before declaring the page mapping final.
