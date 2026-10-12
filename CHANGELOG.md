# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.2]

### Added

- Over-the-air update through a web page on the device (`ota_update`): one-time PIN, image validation, two OTA slots with rollback.
- "Firmware update" button and "Firmware update server" sensor for Home Assistant.
- Custom partition table for 4 MB flash with `ota_0`, `ota_1` and `otadata`.
- README (English and Russian), license, community files and issue templates.

### Changed

- Flash size is set to 4 MB in `sdkconfig.defaults`.
- Removed the unused `PROGMEM` macro from `resources.h`.
- Moved `resources.h` into the `ha_discovery` component.

## [0.1]

### Added

- Mains measurements with PZEM-004T (voltage, current, power, frequency, power factor, energy).
- Two pulse-counting channels on the hardware PCNT with restore from MQTT.
- Home Assistant integration through MQTT Discovery.
- Captive portal for Wi-Fi, MQTT, NTP and meter settings.
- OLED status display, SNTP time sync, optional statistics push over HTTP(S).

[Unreleased]: ../../compare/v0.2...HEAD
[0.2]: ../../releases/tag/v0.2
[0.1]: ../../releases/tag/v0.1
