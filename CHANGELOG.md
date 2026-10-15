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
- Standalone MIT-licensed manager source, with the required upstream and third-party notices.

## Project Origins

### Upstream Foundation and USB Prototype

- Built on sd88me's community catalog, plugin package format and installation tooling, preserving the upstream license notices.
- Adapted the installation workflow from a computer using root SSH to confirmed USB launch directly on the MPC.
- Investigated the stock USB script reader and implemented autorun metadata and checksum generation without bundling Akai files.
- Created a single-package USB installation proof of concept before building the reusable plugin manager.
- Checked stock-reader acceptance, installation through the stock shell and refusal of a target mounted without executable support in a disposable offline environment.
- Selected the SD card's executable Expansions folder as the plugin destination.

### 05-10-2026 — Touchscreen Manager and Reopening

- Added the on-device manager for catalog browsing, local ZIP installation and removal using community package scripts.
- Kept catalog data and downloads in RAM, fetched the catalog once per opening and removed temporary data when the opening ended.
- Corrected the rotated display and mapped touchscreen coordinates to the full-screen interface.
- Moved OS and firmware-library compatibility checks ahead of catalog downloads, with separate download, checking and installation messages.
- Captured the hardware panel events and implemented the two-second Shift + Menu hold while preserving the normal short-press Browser shortcut.
- Added temporary session supervision and shortcut hooks, with cleanup when the stick is removed and no persistent startup installation.
- Tested manager exit and crash recovery, restoring the MPC application after the manager closes.

### 06-10-2026 — Catalog Design and Installation Records

- Replaced the initial interface with a six-card catalog layout, the community palette, Titillium Web and matching tab/action glyphs.
- Added light/dark themes and saved the selected theme on the USB stick.
- Added persistent installation history and comparison with the current device, including installations already present before the manager was used.
- Expanded device detection to inspect plugin folders, retained manager records and MPC registrations, with duplicate paths resolved.
- Separated On MPC from History and distinguished installed versions from versions offered by the catalog or local ZIPs.
- Added installed-card indicators, removal actions and updates for known newer releases, retaining the original-installer requirement for content outside the managed target.
- Remembered each tab's page while the manager remains open and retained browsing after installation.
- Added the themed welcome screen, persistent Hide message preference and the project-save reminder in the stock launch prompt.
- Extended idle return to five minutes, suspended it during active operations and retained immediate recovery after a manager crash.
- Added download and installation progress, then increased the bar to eight pixels with a graceful completion fade.
- Cleared obsolete catalog status text from operation confirmations and removed temporary input/debug logging.

### 10-10-2026 — Independent Build

- Separated the manager into a standalone build and verified ARM compilation, shortcut fixtures, script syntax and interface rendering independently of the plugin projects.

The initial prototype predates these dated refinements; its saved records do not establish an exact implementation date.
These development milestones were consolidated into the first public release, 1.0.0.
