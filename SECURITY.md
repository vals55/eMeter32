# Security Policy

eMeter32 is network-connected firmware: it talks to a Wi-Fi network and an MQTT broker, serves a configuration portal and can receive firmware updates over Wi-Fi. Security reports are taken seriously.

## Supported versions

Only the latest released firmware version receives fixes.

| Version | Supported |
|---|---|
| latest release | yes |
| older versions | no |

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Use GitHub's private reporting: open the **Security** tab of this repository and choose **Report a vulnerability**.

Please include:

- a description of the problem and its impact;
- the firmware version and the hardware you used;
- steps to reproduce, or a proof of concept;
- any suggestions for a fix.

You can expect an acknowledgement within about a week. This is a hobby project, so fixes are made on a best-effort basis. Please give a reasonable time to prepare a fix before any public disclosure.

## Security notes for users

- The setup portal starts an **open** access point only when the device is not configured or when you hold the button. Configure the device and do not leave it in setup mode.
- The firmware update page is protected by a one-time PIN, runs for 10 minutes at most and is meant for a **trusted local network**. Do not expose it to the internet.
- Use a dedicated MQTT account with limited permissions and a strong password.
- Wi-Fi and MQTT passwords are stored in the device flash (NVS). Do not publish dumps of the flash.
- The device works with mains voltage. Wiring must be done by a qualified person.
