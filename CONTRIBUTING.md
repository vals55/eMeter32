# Contributing to eMeter32

Thank you for your interest in the project! Bug reports, ideas and pull requests are welcome.

## Reporting bugs and requesting features

- Search the [existing issues](../../issues) first.
- Use the issue templates. For bugs, please include the firmware version (`FIRMWARE_VERSION` in `settings.h`), the board, the ESP-IDF/PlatformIO versions and a serial log (`pio device monitor`).
- **Remove passwords, tokens and private addresses** from logs and screenshots before posting.
- Security problems must not be reported in public issues. See [SECURITY.md](SECURITY.md).

## Development setup

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or PlatformIO Core).
2. Clone the repository and build:

   ```bash
   pio run
   ```

3. Flash over USB and watch the log:

   ```bash
   pio run -t upload
   pio device monitor
   ```

The first flash after a change of the partition table must be done over a cable.

## Project conventions

- **Language:** C (ESP-IDF). Only standard ESP-IDF components are used; please do not add third-party libraries without a discussion.
- **Structure:** one component per feature in `components/<name>/` with its own `CMakeLists.txt` and a public header in `include/`.
- **Pins:** all GPIO assignments live in `board_config.h`.
- **File header:** every source file (`.c`, `.h`) starts with these two lines:

  ```c
  // Copyright 2026 vals. All rights reserved.
  // SPDX-License-Identifier: MIT
  ```

- **Settings:** if you change `settings_t`, increase `SETTINGS_VERSION` in `settings.h` and add the new field **before** `crc`.
- **Versions:** keep `FIRMWARE_VERSION` in `settings.h` and `PROJECT_VER` in the top-level `CMakeLists.txt` in sync.
- **Generated files:** do not commit `sdkconfig.<env>` or the `.pio/` folder. Put configuration changes in `sdkconfig.defaults`.
- **Mains voltage:** changes that touch measurement hardware must be described carefully in the pull request. Never publish designs that are unsafe to build.

## Pull requests

1. Fork the repository and create a branch from `master` (for example `fix/pzem-timeout` or `feature/new-sensor`).
2. Keep the change focused: one topic per pull request.
3. Build the project (`pio run`) without new warnings and, if possible, test on real hardware.
4. Update the documentation (README, comments) and `CHANGELOG.md` when behaviour changes.
5. Fill in the pull request template.

By contributing you agree that your contribution is licensed under the [MIT License](LICENSE).
