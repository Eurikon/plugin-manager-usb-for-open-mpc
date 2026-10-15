# Changelog

## 1.0.2 — 15-10-2026

### Added

- Optional installation of poloq Plugin Manager directly from the welcome screen.
- Latest-release verification against GitHub and the session catalog before downloading poloq's package.
- Install, Update and Installed button states based on the actual device installation.
- Changelog document, README badge and website header navigation.

### Changed

- Locate the autorun salt automatically instead of requiring one exact firmware hash and a fixed binary offset.
- Validate the MPC executable structure and refuse missing, conflicting or unexpected salt values.
- Recognize a uniquely matching MIDI input port whose name ends in Private, including device-specific prefixes.
- Make offline tests accept developer-supplied plugin ZIPs instead of requiring particular plugins.
- Remove obsolete legacy USB test helpers from the public source.
- Development deployments now back up verified older plugin ZIPs locally, then remove their USB copies and checksum entries after successful installation.
- Update the README and website heading to Plugin Manager USB for OPEN MPC.

### Fixed

- Prevent the welcome action from reinstalling an equal version, downgrading a newer version or creating another copy alongside an installation outside the managed SD folder.
- Block the welcome action when the installed release version cannot be established safely.
- Recheck installation state before release verification, download and installer execution.
- Handle printable bytes before the autorun salt, including longer prefixes and scan-buffer boundaries.
- Reject firmware inputs that change during validation or extraction, and enforce input size limits.
- Protect current, equal/newer, unrelated and unrecognized packages during development cleanup, with cleanup failures reported separately from installation success.

## 1.0.1 — 14-10-2026

### Fixed

- Fix sluggish touch response and repeated taps during package checks and preference saves.
- Move blocking checks and preference writes away from the interface thread.
- Consolidate package validation to avoid repeated payload checks.

### Changed

- Reduce idle CPU use.
- Refine the documentation website, navigation, light theme and README badges.

## 1.0.0 — 13-10-2026

### Added

- USB launch into a touchscreen community plugin catalog on the MPC.
- Browse, install, update and remove compatible catalog plugins, with local ZIP installation from the stick.
- Install plugins onto the SD card and compare actual installations with saved USB history.
- Package compatibility and checksum checks before installer execution.
- Persistent light/dark theme and welcome-message preferences.
- Themed download and installation progress.
- Reopen the manager by holding Shift + Menu for two seconds while the stick remains inserted.
- Return to MPC on request or after five idle minutes, with immediate recovery following an unexpected manager exit.
- Manager-only USB packaging, ARM build tools and firmware-based offline tests.
- Developer FAQ, setup guide and documentation website.
