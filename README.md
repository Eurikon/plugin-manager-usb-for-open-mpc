# Plugin Manager USB for OPEN MPC

[![Download installer](https://img.shields.io/badge/Download-Installer-brightgreen?logo=github)](https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/download/v1.0.2/plugin-manager-usb-v1.0.2.zip)
[![Latest release](https://img.shields.io/github/v/release/Eurikon/plugin-manager-usb-for-open-mpc?logo=github&label=Release)](https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/latest)
[![Changelog](https://img.shields.io/badge/Changelog-History-blue?logo=github)](CHANGELOG.md)
[![User guide](https://img.shields.io/badge/Guide-Website-blue?logo=githubpages)](https://eurikon.github.io/plugin-manager-usb-for-open-mpc/guide.html)
[![MIT license](https://img.shields.io/github/license/Eurikon/plugin-manager-usb-for-open-mpc)](LICENSE)

Browse, install, update and remove community instruments and effects on your MPC touchscreen.<br>
Run the manager from a USB stick and install plugins onto your SD card.

The bundle includes the manager, without plugins or Akai firmware files.

**MPC Live 2 - Gen1 / AKAI MPC OS 3.9.1** (MPC Live II Retro test unit).<br>
This describes recorded local hardware testing; other firmware versions and devices are unverified.

![Plugin Manager catalog on the MPC touchscreen](site/assets/manager-catalog.png)

On your computer, extract the ZIP first, then copy its contents rather than the enclosing extraction folder.<br>
Show hidden files to include `.mpc-manager-id`.<br>
Windows File Explorer and macOS Finder can extract ZIPs.<br>
Development tools and firmware extraction are only needed when building from source.

## Using a Prepared USB Bundle

You need an MPC-formatted USB stick and an SD card with an `Expansions` folder where MPC can install plugins and run them.<br>
Use your computer to copy the bundle onto the stick; no SSH, development tools or ongoing computer connection is needed.<br>
The online catalog needs internet on the MPC; local plugin ZIPs can be installed offline.

**Save your project first.**<br>
Opening the manager stops the MPC application; returning starts it again.

1. Copy the bundle contents directly onto the USB stick, without an extra
   enclosing folder. Keep `autorun/`, `.mpc-manager-id`, `USB-SHA256.json` and
   `local-plugins/` together at the root.
2. Insert the stick into the MPC and choose **Open** in the script prompt.
3. Choose a compatible plugin from **Catalog**, or place your own ZIP in
   `local-plugins/` and select it from **Local**. Follow the installation
   confirmation to install onto the SD card.
4. Install more plugins or choose **Return to MPC**. You can then select the
   installed instrument or effect through MPC's normal plugin selection.

While the stick remains inserted, hold **Shift + Menu for two seconds** to reopen the manager.<br>
A short press retains the MPC Browser shortcut.

The welcome screen also offers **poloq Plugin Manager**, which runs on an MPC track.<br>
Its button offers **Install** when absent, or **Update** when an older, known release is installed on the managed SD card.<br>
**Installed** is disabled for equal, newer or unknown versions and installations outside that location.<br>
The regular catalog still allows deliberate reinstallation.<br>
Choosing **Install** or **Update** authorizes the operation directly.<br>
The latest stable release must match the current catalog before the package is downloaded and checked; failed verification stops installation.<br>
No plugin package is downloaded before you choose an enabled action.

## Find, Update and Remove Plugins

| Tab | Use it for |
| --- | --- |
| <picture><img src="site/assets/tab-catalog.svg" width="16" height="16" alt=""></picture> **Catalog** | Browse online community releases and check compatibility before downloading. |
| <picture><img src="site/assets/tab-local.svg" width="16" height="16" alt=""></picture> **Local** | Install plugin ZIPs you copied into `local-plugins/`. |
| <picture><img src="site/assets/tab-installed.svg" width="16" height="16" alt=""></picture> **On MPC** | See plugins actually present on the device and select available update/removal actions. |
| <picture><img src="site/assets/tab-history.svg" width="16" height="16" alt=""></picture> **History** | Compare this stick's saved installation record with the current device and available packages. |

Uninstall removes the managed plugin and its registration, while keeping its history entry.<br>
History helps identify previous installations after wiping or changing a device; it does not back up projects or plugin files.

Theme and welcome-message preferences stay on the stick.<br>
Page positions last only while the manager is open.<br>
The catalog and downloaded packages are kept temporarily in RAM and discarded when the manager closes.

## What Changes on the MPC?

Plugin files go to `/sdcard/Expansions`; retained uninstall scripts go into `.mpc-usb-manager/` there.<br>
Package scripts run as root and update `MPC.settings`.<br>
The installer does not replace your firmware.<br>
Its USB launch mechanism is firmware-specific and may change after an Akai update.

Checksums detect damaged or changed files; they do not certify who wrote a plugin or whether its scripts are safe.<br>
Use packages from publishers you trust.<br>
Recovery and user-data preservation depend on each package's scripts.<br>
Content outside the managed target needs its original installer.

After five idle minutes, the manager returns to MPC.<br>
An unexpected exit triggers an immediate application restart.<br>
USB removal during installation can interrupt the operation.<br>
Follow the MPC's own drive-removal instructions.

## Building the Manager

The [Developer FAQ](DEVELOPER_FAQ.md) covers the source, USB launch mechanism, package format, recovery and release preparation.

Development tools were tested on an **Apple Silicon Mac** with **macOS 26.6.2**.<br>
Builds and offline tests use Docker.<br>
**Windows and Linux development setups haven’t been tested.**

Requirements: Python 3.9 or later, GNU Make and Docker with `linux/arm/v7` support.<br>
Run from this checkout:

```sh
make syntax
make arm
```

`make syntax` checks Python and shell scripts.<br>
`make arm` builds with warnings as errors in the `mpc-usb-plugin-installer-build-tools` Docker image.<br>
Creating the image downloads dependencies; compilation runs with networking disabled.<br>
Outputs stay in ignored `build/`.<br>
No sibling project is needed.

## Building a USB Bundle Without Redistributing Firmware

To create a USB bundle from source, obtain the official **Gen1 OS 3.9.1** update image and keep it outside this checkout.<br>
The extractor reads it locally without executing or altering the firmware.

After `make arm`, set `UPDATE_IMAGE` to the absolute path of that image and `OUTPUT_DIR` to an empty local directory you create outside the checkout:

```sh
docker run --rm --pull never --platform linux/arm/v7 --network none \
  --mount "type=bind,src=$PWD,dst=/work,readonly" \
  --mount "type=bind,src=$UPDATE_IMAGE,dst=/input/update.img,readonly" \
  --mount "type=bind,src=$OUTPUT_DIR,dst=/output" \
  -w /work mpc-usb-plugin-installer-build-tools sh -ec '
    python3 tools/extract_firmware.py /input/update.img /tmp/manager-firmware
    python3 package.py /tmp/manager-firmware/usr/bin/MPC /output/usb
  '
```

The extracted `usr/bin/MPC` must have the supported ARM executable structure and the expected autorun salt.<br>
The packager locates that salt automatically, using bounded scanning instead of a fixed offset.<br>
It rejects missing, conflicting or unexpected values.<br>
These checks establish packaging compatibility, not proof of an unmodified stock executable or support for another device.<br>
The executable is **not copied into the bundle**.<br>
The temporary extraction disappears with the container.<br>
`/output/usb` must not already exist; the packager refuses to overwrite it.

The default bundle has an empty `local-plugins/` directory for users' own ZIPs.<br>
The packager's optional third argument adds a ZIP for local development only; do not include plugins in public manager bundles or source control.

## Broader Offline Tests

For offline evaluation, developers supply their own plugin packages and official firmware outside the checkout.<br>
Run package scripts only in a disposable Linux environment, using packages you trust.<br>
The repository provides no plugin payloads or Akai firmware files.

Offline checks cover installation/removal, inventory, package rejection, interface navigation, recovery and shortcut behavior.<br>
Run `make test-local TEST_PACKAGE_ZIP="/path/to/your-plugin.zip"` for local runner, packaging, firmware scanner and setup checks.<br>
Checks needing an optional package report a skip when that input is absent.<br>
An offline pass does not extend the recorded hardware compatibility: display/touch behavior, USB notifications and device CPU performance still need hardware testing.<br>
See the [Developer FAQ](DEVELOPER_FAQ.md#testing-and-troubleshooting) for the disposable firmware test recipe and troubleshooting.

## Source Layout and Integration

`src/` owns the SDL interface, installation backend and shortcut supervisor; `tools/` owns deployment/extraction helpers.<br>
Top-level shell scripts control the transient RAM session.<br>
`assets/` and `licenses/` hold the font and notices; `tests/` holds reusable checks.

Other development projects can set `MPC_MANAGER` to this checkout's path.<br>
See the [Developer FAQ](DEVELOPER_FAQ.md#how-do-i-use-this-checkout-from-another-development-project) for path resolution and integration details.

## Credits and Acknowledgments

This project builds on **sd88me**'s [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) catalog, package format and installation tooling.<br>
The [upstream copyright notice](licenses/community-tools.txt) is preserved in the repository and USB bundles.<br>
See the [upstream credits](https://github.com/sd88me/mpc-vst-plugins#credits) for community contributors.

**Twisted Cable** develops this on-device USB manager, its interface, reopening shortcut, installation inventory and firmware-based offline testing workflow.<br>
Plugin authors retain ownership and licenses for their own packages; plugins are downloaded separately and are not included in this repository.

## Licensing

Original manager code is [MIT licensed](LICENSE).<br>
Third-party components retain their own licenses: Titillium Web uses SIL OFL 1.1, SDL2 uses zlib, and nlohmann JSON uses MIT.<br>
Bundles include these notices and the manager's license.<br>
Preserve them when redistributing.

## Disclaimer

Use this installer on your hardware at your own discretion and risk.<br>
It is provided **as is, without warranty**.<br>
This independent community project is not affiliated with or endorsed by Akai.
