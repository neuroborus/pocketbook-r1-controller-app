# Changelog

## 0.1.0

Initial release preparation.

- Visible pointer and button mapping for Magicsee R1 on PocketBook Era Color.
- Pointer clicks, reader menu access and short/long Return actions.
- Temporary root SSH startup, status and shutdown commands, with log rotation.
- Optional persistent installation with a restricted root launcher and
  Applications entries, plus removal and recovery scripts; no autostart.
- Controller reconnection and application sessions without a fixed time limit.
- Native-input focus handoff with held-finger priority; physical gesture
  validation is pending.
- Fast host-side regression tests with ASan/UBSan.

Known limitations: residual pointer images on covers, portrait-only support,
no autostart, and device validation still pending
for extended application sessions, reconnection and M+B navigation.
