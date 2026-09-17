# Changelog

## 0.1.0

Initial release preparation.

- Visible pointer and button mapping for Magicsee R1 on PocketBook Era Color.
- Pointer clicks, reader menu access and short/long Return actions.
- Temporary root SSH startup, status and shutdown commands, with log rotation.
- Controller reconnection and application sessions without a fixed time limit.
- Fast host-side regression tests with ASan/UBSan.

Known limitations: residual pointer images on covers, portrait-only support,
no persistent installation or autostart, and device validation still pending
for extended application sessions, reconnection and M+B navigation.
