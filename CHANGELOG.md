# Changelog

## 1.2.2 — 28-10-2026

- Record OS 3.10 helper launch refusals in a bounded `bootstrap-result.txt` on the confirmed USB stick, preserving the original failure status.
- This helper-package change requires copying the complete bundle; OS 3.10 users must also reimport the matching helper. Existing updater versions refuse changed helper components.

- Fix a Gen2 startup crash caused by large saved settings. The settings reader now scans tags without recursive regular-expression matching.

## 1.2.1 — 28-10-2026

- Follow the Gen2 MPC content-download drive preference for automatic plugin installation, using the mounted drive’s `Synths` folder instead of a guessed `/nvme/Synths` fallback.
- Add a separate **Plugin installation location** selector in SETUP, with automatic selection and browsing for existing custom folders.
- Create missing standard content folders and register chosen locations only after confirmed installation and package validation, preserving an original settings backup.
- Stop clearly for missing, replaced, read-only or non-executable drives, and retain Gen1’s existing internal plugin destination.
- Distinguish plugin storage from the **Plugin Manager installation** on removable SD.

## 1.2.0 — 27-10-2026

- Add automatic Gen1/Gen2 selection in one USB bundle, including ARM64 catalog packages, installed inventory and the Gen2 OS 3.10 helper.
- Correct Gen2 native UI bindings and prevent recursive shortcut/addin lookup chains.
- Preserve current service addin settings and their order across repeated manager openings.
- Retain generation-specific SD installation, update/removal checks and display/touch orientation handling.

Beta 2 hardware approval covers Akai Force on OS 3.9.1 and MPC Live III (Gen2) on OS 3.10 over OS 3.9.1.
Stock Gen2 OS 3.10 remains offline-tested without a hardware-test claim.
Updating from 1.1.0 or a 1.2.0 beta requires copying the complete bundle and reimporting the helper on OS 3.10.

## 1.2.0-beta.2 — 27-10-2026

- Correct two Gen 2 OS 3.10 native UI bindings that prevented the helper from opening the manager.
- Resolve shortcut forwarding through libc's versioned lookup so an addin's custom `dlsym` cannot create a recursive wrapper chain; retain the Gen 1 symbol-version fallback.
- Read current service environment settings and ordered environment files before restarting MPC, preserving addins configured after the current process started.

Gen 2 OS 3.9.1 hardware operation was reported on MPC Live III.
The corrected hook still requires a hardware retest; the reported OS 3.10 helper test used an application overlay rather than a stock OS 3.10 installation.

## 1.2.0-beta.1 — 26-10-2026

- One USB bundle selects the ARMv7 Gen 1 or ARM64 Gen 2 manager, shortcut and helper automatically.
- Gen 2 catalog downloads, local-package checks and installed inventory use ARM64 releases and the 64-bit plugin registry.
- The OS 3.10 helper supports the verified stock Gen 2 build, including native loading dialogs and internal Documents locations.
- SD installation and manager updates retain the selected generation's runtime and preserve user files during removal or rollback.
- Display rendering and touch coordinates follow the detected panel orientation.
- Added bounded extraction and section-checksum verification for Gen 2 update images.

Gen 2 has passed offline checks using official OS 3.9.1 and 3.10 images; device testing remains pending.
Install this testing bundle by extracting it onto a prepared USB stick and importing its updated helper on OS 3.10.
The stable 1.1.0 updater cannot install this changed helper automatically.

## 1.1.0 — 26-10-2026

### Added

- Universal USB bundle with the OS 3.9.1 launch route and a one-time controller-helper import for supported Gen 1 OS 3.10.
- SD-card installation, update/reinstallation and removal of the manager itself, preserving plugins, history, preferences and local packages.
- Shared SETUP page for SD management, welcome-screen visibility and GitHub manager-update checks with explicit installation confirmation.
- USB-stick website favicon, with SVG and ICO versions.
- Public helper source, ARM build target and universal packaging from externally supplied official firmware and a blank setup project.

### Changed

- Keep cold boot and SD reinsertion quiet; use Shift + Menu to open the manager when the drive was already present at startup.
- Offer a USB session on fresh insertion even when an SD copy is configured; Cancel keeps the SD selection.
- Refine the welcome screen, SD setup row, compact header controls and consistent sun/moon theme buttons.
- Explain both firmware setup routes, welcome actions, SD removal and return times in the guide and Developer FAQ, with refreshed hardware screenshots.
- Use the sidebar website/manual layout with compact screen cards, category filters and the existing dark/light colors.

### Fixed

- Restore the native loading display during the 3.10 USB handoff, while allowing launch to continue if optional loading feedback is unavailable.
- Restore the MPC application when uninstalling the active SD copy ends its supervisor.
- Remove only the verified, unchanged setup project after successful helper setup.

## 1.0.3 — 15-10-2026

### Fixed

- Preserve existing addin preload libraries in their original order and append the manager's shortcut hook last.
- Read the running MPC service's effective `LD_PRELOAD`, including values loaded through environment files.
- Remove stale manager-hook entries before adding the current hook.
- Use a root-only temporary RAM environment file, read after existing service environment files, without editing addin configuration.
- Remove the temporary environment file and service drop-in when the USB session ends.

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
