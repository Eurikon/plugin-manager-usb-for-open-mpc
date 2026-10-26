# Developer FAQ

This guide describes the current source and the validation recorded on
**26-10-2026**.
Start with [README.md](README.md) for USB use, build prerequisites,
tested platforms and setup. Firmware, plugin packages
and private device data are external inputs; none belongs in this repository.

## Building and finding the code

### What is upstream, and what did we add?

- The upstream installer runs on a computer using existing root SSH access.
- This USB manager opens on the MPC itself and adds USB launch packaging, a
  reopening shortcut, saved installation inventory and an extracted-firmware
  offline test workflow.

See the [Guide credits](README.md#credits-and-acknowledgments) for project origins.
Preserve the bundled license notices when redistributing. This repository
contains neither the upstream plugin collection nor Akai firmware.

### What has actually been tested?

Hardware testing is limited to **MPC Live 2 — Gen 1 / MPC Live II Retro, running
AKAI MPC OS 3.9.1 and 3.10**. Development has been validated from an Apple Silicon Mac;
the exact checked macOS version is recorded in
[Building the Manager](README.md#building-the-manager). ARM builds and broader
offline tests run in Linux Docker containers. Windows and native Linux
development hosts remain unverified. Offline passes do not establish support
for another MPC model or firmware.

The Gen 2 testing build adds ARM64 compilation, sectioned-firmware extraction, architecture-specific catalog/registry checks, SD/update fixtures and native OS 3.10 dialog/queue tests against an extracted filesystem.
Gen 2 hardware testing is pending; the stable Gen 1 hardware record above does not cover it.

### How do I build the manager?

Install Python 3.10 or newer, GNU Make and Docker with `linux/arm/v7` execution
support. From this checkout, run:

```sh
make syntax
make arm
```

`syntax` checks Python and shell syntax.
`arm` builds the manager, its supporting ARM executables and the 3.10 controller helper.
`make arm-helper` rebuilds only `build/startup/manager-entry.so` using the existing image.

For Gen 2, also run `make arm64`; Docker must support `linux/arm64`.
The same Dockerfile uses GCC 12/Bookworm for this variant and writes to `build/aarch64/`.
The manager's static SDL build, source and dependency set are shared; the native binaries remain architecture-specific.
Run `make test-arm64` for architecture, SD install/removal, manager-update rollback and shortcut/addin-chain checks in that container.
These use temporary manager fixtures and do not require or install a plugin package.

For a universal bundle, supply the extracted official OS 3.9.1 and Gen1 OS 3.10 executables and a blank standalone project saved on your MPC.
Use `package.py` with both `--mpc-310` and `--setup-project`; the complete image-extraction recipe is in [the README](README.md#building-a-usb-bundle-without-redistributing-firmware).
The project must contain no samples or saved sequences.
It is an external build input, not a bundled example project in this repository.
Only its unchanged setup copy is eligible for automatic cleanup.

`tools/extract_firmware.py --controller-helper` validates the 3.10 controller profile rather than the older USB autorun scheme.
`tools/helper_package.py` reads the profile checksum initialization from the exact supported executable without executing firmware code.
Unknown executable hashes or decoded values are rejected.
This strict 3.10 build match is necessary for the native UI and panel bindings; it is separate from the automatic legacy autorun salt scanner.

For both generations on one stick, add `--arm64-build build/aarch64 --mpc-310-arm64 /path/to/Gen2-MPC310` to the packaging command.
Both options are required together for a universal helper.
The helper verifies the Gen 2 executable independently before enabling its native bindings.
The legacy Gen 1 and Gen 2 OS 3.9.1 executables decode the same autorun salt; differing candidates still stop packaging.

On Gen 2, the helper resolves its own module inside the approved internal MPC Documents locations rather than assuming `/sdcard` exists.
An exFAT permission exception applies only to that internal helper source; RAM locks and approval files retain their stricter mode checks.
The manager uses `pluginList-arm-64bit`, ARM64 catalog assets and registered executable plugin storage.
No 32-bit compatibility libraries are required on the device.

The Gen 2 testing release requires copying a complete bundle and reimporting its helper.
The older stable updater refuses the changed helper/layout rather than partially applying it.
Testing releases are excluded from the normal stable-update lookup.


[Dockerfile](Dockerfile) uses GCC 11/Bullseye and SDL2 2.30.12, plus FreeType,
libarchive, nlohmann JSON and Linux display dependencies. [build.sh](build.sh)
uses C++17 and treats compiler warnings as errors. SDL2 is linked statically;
FreeType and libarchive remain dynamic dependencies. Outputs and copied notices
go to ignored `build/`.

The base image uses a tag, apt packages are not fully version-pinned, and the
SDL archive download has no configured digest check. Rebuilding is supported;
a byte-identical result from a fresh dependency download is not guaranteed.
No separate formatter, linter or type-checker is configured.

### Which files should I edit?

| File | Responsibility |
| --- | --- |
| [src/main.cpp](src/main.cpp) | SDL interface, rendering, touch interaction, navigation and job progress. |
| [src/backend.hpp](src/backend.hpp), [src/backend.cpp](src/backend.cpp) | Catalog, package validation, installation/removal, device inventory, history and preferences. |
| [open.sh](open.sh) | USB entry point; reuses an active supervisor or copies runtime into RAM. |
| [setup.sh](setup.sh) | Detects the MPC service/private MIDI port and starts the temporary supervisor. |
| [src/shortcut.cpp](src/shortcut.cpp) | Session ownership, reopen requests, temporary service configuration and cleanup. |
| [src/shortcut_hook.cpp](src/shortcut_hook.cpp), [src/shortcut_state.hpp](src/shortcut_state.hpp) | Observe panel MIDI and detect the Shift + Menu hold. |
| [launch.sh](launch.sh) | Stops MPC, opens the manager, removes the opening's RAM data and restores MPC. |
| [src/startup/entry.c](src/startup/entry.c), [src/startup/hotplug.cpp](src/startup/hotplug.cpp) | Controller entry, USB/SD observation, native dialogs/loading, panel input and RAM bootstrap for OS 3.10. |
| [build_helper.sh](build_helper.sh) | Compiles the controller helper with the shared ARM build image. |
| [tools/helper_package.py](tools/helper_package.py) | Validates the 3.10 profile/setup project and builds the helper XPA with its runtime binding. |
| [package.py](package.py) | Constructs legacy or universal USB bundles and their checksums. |
| [tools/extract_firmware.py](tools/extract_firmware.py) | Bounded extraction of an external official update image for packaging/tests. |
| [tools/saltscan.py](tools/saltscan.py) | Bounded automatic search for a consistent autorun salt. |
| [tools/deploy.cpp](tools/deploy.cpp) | Separate development deployment transaction; absent from the default USB bundle. |
| [tests/run_offline.py](tests/run_offline.py) | Stages and runs the broader offline suites. |

Keep source changes within the owning component. Preserve on-device names such
as `mpc-usb-manager`, `.mpc-usb-manager` and marker contents
`mpc-usb-manager-v1`; these are runtime contracts, not old checkout names.

## Firmware, launch and session lifetime

### Why do I need an official firmware image?

You can compile the manager without one.
USB packaging reads `usr/bin/MPC` to generate the verification file accepted
by its USB script reader.
Broader offline tests also need the extracted filesystem's ARM libraries and
BusyBox.

[package.py](package.py) validates an ELF32 little-endian ARM executable with
EABI5 hard-float, valid loaded segments and an executable entry point.
[tools/saltscan.py](tools/saltscan.py) then finds the autorun salt automatically
across the XOR alignments instead of reading a fixed offset.
At least one complete salt must lie in readable loaded executable content.
Every candidate must agree and match the known salt fingerprint.
Missing, conflicting or unexpected salts stop packaging.
File size, decompression memory, scan volume and candidate counts are bounded;
damaged compressed streams are rejected.

The Gen1 OS 3.9.1 executable SHA-256 remains a recorded reference, not an
acceptance gate.
Passing the structure and salt checks establishes packaging compatibility,
not proof that a file is unmodified stock firmware or that another device
works correctly.
Use an official update image; hardware support still requires separate tests.

The executable and extracted filesystem are **not copied into the USB bundle**.
Extraction reads a locally supplied official update; it neither executes that
image nor changes the MPC.
Use the
[README packaging recipe](README.md#building-a-usb-bundle-without-redistributing-firmware).
The extractor requires a nonexisting disposable directory under `/tmp`, rejects
unsupported/truncated images, and limits filesystem size and decoder memory.

### What triggers open.sh, and why does it run as root?

On MPC OS 3.9.1, the stock MPC USB script reader recognises
`autorun/autorun.json` and `autorun/autorun.check`. After the check and your
confirmation in the stock prompt, it invokes the configured `open.sh` through
its root-privileged launch process.

Our scripts inherit that privilege and check for it. They do not enable SSH,
install different firmware or perform a separate privilege-escalation step.
The stock reader itself is Akai code and is not included here.

On OS 3.10, the imported controller profile calls the original helper source in `src/startup/` instead.
The helper detects the supported USB/SD installation, presents native consent for a new USB insertion and stages execution in RAM.
It uses the stock controller-script mechanism; it is not an instrument plugin.
Its persistent files live in the user's MPC Documents content area, with no root-filesystem startup service.

The universal bundle carries both routes; OS 3.9.1 does not need the controller profile.
On 3.10, import `Plugin Manager helper.xpa` into internal MPC Documents once and restart the MPC application.
On Gen 1 the directory is `/sdcard/MPC Documents/Devices/Plugin Manager helper`.
Gen 2 resolves the corresponding internal Documents location, including `/nvme/MPC Documents` on NVMe-equipped devices.
Extraction beside the XPA on removable media does not activate it.
The unchanged blank `PluginManager.xpj` used for import is removed only after successful setup, with size, hash, ownership and file-identity checks.
Modified projects and failed setup files are preserved.

An Akai update can change or remove either launch mechanism.
The scanner refuses an unknown scheme; successful packaging alone does not
establish support for another release.

### What happens between Open and Return to MPC?

On the legacy USB route:

1. `open.sh` copies our runtime to `/tmp/mpc-usb-session.*`, or sends an open
   request to an existing session's socket.
2. `setup.sh` detects `acvs` or, if absent, `inmusic-mpc`; it finds the
   single input/bidirectional MIDI port whose name ends in `Private`, then
   starts a transient `mpc-usb-manager` systemd unit.
   Missing, ambiguous or invalid hardware addresses are rejected.
3. The supervisor reads the active MPC service's `MainPID` and obtains its
   effective `LD_PRELOAD` from `/proc/<MainPID>/environ`, including values from
   `Environment=` and `EnvironmentFile=` settings. It preserves existing preload
   libraries in order, removes stale manager-hook entries and appends the current
   hook last. A root-only RAM environment file is read after existing environment
   files through `/run/systemd/system/<service>.service.d/99-mpc-usb-shortcut.conf`.
4. The supervisor creates a separate `/tmp/mpc-usb-manager.*` opening.
   `launch.sh` stops the MPC application, waits for its exit and runs the SDL
   manager using KMSDRM with the SD target and settings path.
5. Return to MPC, five idle minutes, or an unexpected manager exit closes that
   opening, removes its RAM data and starts the MPC application again.

The 3.10 path uses `src/startup/plugin_manager.lua`, `entry.c`, `hotplug.cpp` and `bootstrap.sh` to validate and prepare the runtime before handing it to the same supervisor and `launch.sh`.
The helper build checks its required exported entry points and rejects load-time constructors/destructors.

These are **application restarts**. The MPC service must be running before
opening a new USB session so its effective preload configuration can be read.
Existing service and addin configuration files are not edited.
No persistent startup service or root filesystem change is installed.
Preload chaining passed offline checks and an isolated native MPC Live II test
using generic addin fixtures, without restarting the MPC application.
Force hardware testing with addins remains pending external review.

### How does the shortcut work, and what remains after closing?

The hook forwards normal ALSA MIDI reads unchanged and sends relevant button
events to the supervisor. Holding **Shift + Menu for two seconds** opens the
manager; a shorter press retains the MPC Browser shortcut.

The supervisor waits on socket, timer, signal and source-removal events using
`poll`, `timerfd`, `signalfd` and `inotify`. It does not repeatedly scan the
system. It remains active while its source is present so the manager can reopen.

Closing an opening discards its catalog, downloads and extracted packages.
Source removal or supervisor termination removes its RAM drop-in, temporary
environment file, session-state file and runtime, then reloads systemd.
The lock is released, although `/run/mpc-usb-manager.lock` may remain. An already-running MPC process
can retain the loaded hook until its next application restart.
Reboot loses the transient session.
On 3.9.1, a new USB opening prepares it again.
On 3.10, the imported controller helper is loaded with MPC and prepares the RAM session again when supported storage is available.
This does not launch the manager's interface at boot.

### How do USB insertion and SD startup differ on 3.10?

`src/startup/hotplug.cpp` distinguishes storage present at startup from a fresh USB insertion.
A cold boot with USB already connected stays on MAIN.
A fresh supported USB insertion presents native consent; Cancel preserves the existing SD selection, while Open selects the USB source.
SD reinsertion is silent; Shift + Menu opens the SD manager when available.
The internal helper is independent of the removable SD application and remains installed when the card is removed.

The helper prepares runtime in RAM before handing off to `setup.sh` and the shortcut supervisor.
Its native loading display is optional feedback: failure to create that display must not cancel an otherwise valid Open action.
Returning from the GUI restarts the MPC application, not the hardware.
The user measured a 30-second return from an SD session on the Gen 1 test unit; allow longer rather than treating a short MAIN delay as a failed restart.

### What do the different checksums prove?

| Check | Purpose |
| --- | --- |
| Executable SHA-256 | Records the supplied executable and whether it matches the tested Gen1 reference. |
| Autorun salt fingerprint | Rejects a different salt after bounded scanning and executable validation. |
| `autorun.check` | Matches the configured launch files to the stock reader's integrity calculation. |
| Controller profile checksum | Matches the helper's generated profile to the supported 3.10 controller reader. It is separate from legacy autorun validation. |
| `USB-SHA256.json` | Generated inventory for external bundle verification; the current manager does not read it at runtime. |
| Plugin ZIP hash | Catalog downloads must match the catalog's hash; local ZIPs must match the file selected in that session. |
| Package `SHA256SUMS` | Checks extracted package files before installation scripts run. |

Regenerate the bundle after changing any packaged binary, font, script or
notice. Checksums establish consistency, **not publisher identity or script
safety**. Package installers and uninstallers run as root without a sandbox.

## Packages, settings and saved data

### What must a plugin package contain?

A ZIP needs one top-level package folder containing `mpc-plugin.json`,
`install.sh`, `uninstall.sh`, `plugin_list.awk` and `SHA256SUMS`, alongside its
payload. The checks in `Manager::manifest()` include:

| Field or content | Requirement |
| --- | --- |
| `schema`, `arch` | `1` and `"armv7"`. |
| `id` | Lowercase identity matching `[a-z0-9][a-z0-9_-]*`. |
| `kind` | System `addin` packages are unsupported. |
| `os_compat` | Array containing `"3.x"`; an omitted value can be supplied by a compatible matching catalog item. Local packages need their own declaration. |
| `max_glibc` | Numeric dotted version no newer than the device's libc. |
| `folder`, `skin` | Safe relative folder; `skin` must be a single directory name. |
| Scripts | Both contain the deferred-session `DEFER=` marker. |
| `SHA256SUMS` | Valid hashes covering extracted regular files other than `SHA256SUMS`. |

The interface also consumes identity/display fields such as `name`, `version`
and `kind`; installed-payload detection consumes `so`. This table describes
current manager checks, not a substitute for the upstream release specification.
See [src/backend.cpp](src/backend.cpp), particularly `manifest()`, `unpack()`
and `installedPlugins()`.

Archive extraction rejects unsafe/duplicate paths, hard links and unsupported
entry types. Relative symlinks must remain inside the package. The archive must
have fewer than 100,000 entries and at most **512 MiB** of declared extracted
regular-file data. The target filesystem must also support the package's links.

### How are catalog compatibility and metadata checked?

The manager fetches the community's `catalog.tsv` once per opening and optionally
enriches it from `catalog.json`. Retrieval failure still permits local packages.

The TSV header must be `#mpc-catalog-tsv 1`. Accepted plugin rows supply identity,
version, kind, skin, UID, download size/hash/HTTPS URL, deferred-install support,
OS compatibility and glibc requirement. System addins are excluded. Missing OS3
confirmation, unknown/newer glibc or non-deferred installers are blocked
**before download**.

JSON enrichment is accepted only when schema, plugin ID, version and SHA-256
agree. It supplies author, summary and license text. Downloaded contents are
then checked independently; catalog metadata alone never authorises running
an unchecked package.

### How are MPC.settings changes and failures handled?

`launch.sh` stops MPC before the GUI permits installation. The backend requires
an existing settings file and a registered writable/executable plugin content location.
Gen 1 uses `/sdcard/Expansions`; Gen 2 selects its registered location and prefers `/nvme/Synths` when available.
It exports `MPC_SETTINGS` and invokes the package's installer or uninstaller with
`-y -n -t` followed by that validated location.

Those external scripts perform the payload and settings edits. Standard
community scripts back up settings, but the GUI does not provide a universal
rollback transaction around every package. Script failure can leave partial
changes; checksum success only means the validated files were intact.

The separate [tools/deploy.cpp](tools/deploy.cpp) helper validates current and
previous identities, snapshots payload/registry/settings/history, verifies
installed files and unrelated settings, and attempts restoration after failure.
It is used by development consumers and **is absent from the default USB
bundle**. Do not attribute its guarantees to ordinary GUI installations.

### Where are plugins installed, and what does uninstall remove?

The managed target is `/sdcard/Expansions` on Gen 1 and the selected registered executable content folder on Gen 2.
Registry folders under
`.mpc-usb-manager/<id>/` retain the manifest, uninstaller and settings-edit helper.
Removal runs the retained uninstaller and updates the USB inventory. User-data
preservation and payload cleanup depend on that script. Content outside the
managed target needs its original installer.

On the tested Live II, `/sdcard` is internal content storage.
The removable card used for the manager application is mounted separately under `/media/`.
Do not infer a plugin's physical location from the manager's selected USB/SD source.
Changing the plugin target means changing and testing the target/registration contract; it is not just a UI preference.

### What is saved on the source drive?

`plugin-history.json` has `schema: 1` and a `plugins` object keyed by plugin ID.
Entries remember identity/version, an `installed` flag and an `updated`
timestamp in `DD-MM-YYYY HH:MM:SS UTC`. It is the latest remembered inventory,
not an event-by-event journal. Uninstall retains the entry.

On opening, detected installations are recorded. **On MPC** reflects actual
files/registrations. **History** compares saved inventory with the current
device and available packages; a missing plugin does not erase its record.

`manager-preferences.json` has `schema: 1`, `dark_theme` and `hide_welcome`
booleans, defaulting to false. Saves use a temporary file, `fsync` and rename.
Invalid existing data is rejected rather than silently overwritten. Preserve
the original files before repairing them. Tab page positions live only in
memory for the current opening.

The source is the USB root or the SD card's `Plugin Manager USB/` directory.
Each source has its own history, preferences and `local-plugins/` folder.
Opening the manager records detected installations in that source's history.
Catalog data, downloads and extracted packages remain per-opening RAM data and are discarded on exit.

### How is SD installation different from installing a plugin?

`src/sd_install.cpp` installs this manager's runtime under `Plugin Manager USB/` at the selected card's root.
The 3.10 helper capability, supported firmware, removable SD identity, mount state and available space are checked before installation.
Internal disks and SSDs are excluded.
The internal helper records the SD UUID; this selection does not change the plugin package target described above.

SETUP's **Update manager** copies the current runtime to SD.
Removal validates the installation receipt and owned file identities before deleting recognized application files and clearing the SD selection.
History, preferences, local packages and unrelated files are retained, so a directory can remain after uninstall.
Modified or unrecognized application files require inspection rather than blind deletion.
The controller helper remains for USB operation.

Removal can end the active SD supervisor.
`src/shortcut.cpp` checks MPC service state after reaping the opening and requests a normal application start if needed, so retiring that supervisor does not strand MPC stopped.

### How do manager updates work?

SETUP's **Check for updates** queries GitHub Releases; it does not install anything.
An eligible newer release enables **Install update**, followed by a separate download/install confirmation.
`src/manager_update.cpp` checks the release asset, bundle inventory, supported runtime/helper contract and existing managed copies before publication.
An accepted update covers connected managed media while preserving user data.
Changed helper bindings, launcher/supervisor contracts or modified owned files can require a manual update instead.
The running process uses its existing code until the user returns to MPC and reopens the manager.

The real GitHub release check and isolated update/rollback fixtures have passed.
Applying a genuinely newer public release on hardware remains pending because there was no newer published release during these checks.
Do not describe that application step as a completed hardware test.

### How does the welcome-screen Plugin Manager offer work?

`Manager::installationOffer()` compares the catalog release with actual
installations matched by ID, skin or UID.
It offers **Install** when absent and **Update** only when all matching releases
are known, older and inside the managed SD target.
Equal, newer, unknown-version or external installations show disabled **Installed**.
Choosing **Install** or **Update** authorizes the operation without another prompt.
`Manager::installLatestPlugin()` rechecks this guard before the live release
lookup, before downloading and immediately before installer execution.
`Manager::verifyLatestPlugin()` checks the pinned repository's latest stable release.
Its asset for the current architecture must match the session catalog's version, download URL, size
and SHA-256 digest; unavailable, stale or conflicting metadata blocks the offer.
The package then passes the existing ZIP checksum, compatibility, identity
and payload checks before its installer runs.
Normal catalog installation retains deliberate reinstallation support.
Opening the welcome screen does not download or install the plugin package.
The catalog remains a once-per-opening RAM snapshot; no plugin is bundled.

## Testing and troubleshooting

### How do I prepare the offline suite?

Use your own supplied plugin packages and an official firmware image kept
outside the checkout. The repository provides no plugin payloads or Akai
firmware files. Keep extracted firmware, installed test content and results
in a disposable Linux environment, separate from your real device and user data.

Choose packages that follow the
[manager package contract](#what-must-a-plugin-package-contain). Package
scripts execute as root inside that environment; use only scripts you trust.

Run fast local checks with a trusted package you supply:

```sh
make test-local TEST_PACKAGE_ZIP="/path/to/your-plugin.zip"
```

This checks the runner, packager, firmware extraction/scanning and setup guards.
`make test-local` also runs without a package, but package-dependent cases
report a skip.
`make test` builds and runs the ARM shortcut checks inside Docker.

For the broader suite, first run `make arm`.
Set `UPDATE_IMAGE` and `TEST_PACKAGE_ZIP` to absolute paths outside the checkout,
then run:

```sh
docker run --rm --pull never --platform linux/arm/v7 --network none \
  --mount "type=bind,src=$PWD,dst=/work" \
  --mount "type=bind,src=$UPDATE_IMAGE,dst=/input/update.img,readonly" \
  --mount "type=bind,src=$TEST_PACKAGE_ZIP,dst=/input/plugin.zip,readonly" \
  -w /work mpc-usb-plugin-installer-build-tools sh -ec '
    python3 tools/extract_firmware.py /input/update.img /tmp/manager-test-firmware
    python3 tests/run_offline.py /tmp/manager-test-firmware /input/plugin.zip --direct --skip-build
  '
```

The extracted filesystem and installed test content disappear with the container.
Logs and input identities remain in ignored `build/offline-results/`.
An optional legacy package without `os_compat` needs `--legacy-package` and
explicit `--legacy-os3-confirmed`; the suite never assumes OS 3 compatibility.
No supplied plugin package is copied into public source or a manager release.

### What does an offline pass establish?

Offline evaluation checks package rejection, installation/removal, inventory,
navigation and session recovery with mocked downloads and device services.
Record executed checks, input hashes, skipped cases and retained results.
An offline pass does not extend the recorded hardware compatibility or prove
physical display/touch behavior, actual USB prompt behavior or hardware CPU
performance. Record those separately against the installed artifact. Never run
test-mode environment settings against a real device as ordinary installation.

### Where should I look when startup or installation fails?

| Evidence or error | Check |
| --- | --- |
| `manager-result.txt` on the source drive | Latest opening's log; launch failure, application stop/start and manager exit. |
| `shortcut-result.txt` on the source drive | Latest supervisor checkpoint; panel port, session ownership and reopen events. |
| `/run/mpc-usb-manager-session` | Current runtime directory and selected USB or SD source path. |
| `systemctl status mpc-usb-manager` | Transient supervisor, when a device shell is already available. |
| `systemctl status acvs` / `inmusic-mpc` | MPC application state; inspect the service present on that device. |
| Rejected firmware executable | Check the official update, ARM executable structure and scanner error; do not bypass a failed validation. |
| Private MIDI port missing/ambiguous | Setup needs exactly one input/bidirectional hardware MIDI port ending in `Private`. |
| Missing settings or SD content | Check the existing settings path, `/sdcard/Expansions`, registration and mount options. |
| Effective service environment unavailable | MPC must be running; the service MainPID and its process environment must be readable before opening a new session. |
| Invalid history/preferences | Preserve the original source-drive JSON before repairing its schema or contents. |
| No first-use USB prompt on 3.10 | Import the helper into internal MPC Documents and restart the MPC application; legacy autorun alone does not provide this route. |
| No interface after boot or SD reinsertion | Intentional quiet startup; use the two-second Shift + Menu shortcut. |
| Manager update requires manual installation | Preserve existing media; inspect the reported helper/runtime compatibility failure instead of bypassing it. |

Package subprocesses have a 180-second deadline and a 512 MiB output-file size
limit. Failure messages distinguish rejection before scripts run, potentially
partial script changes, and successful installation followed by a failed USB
record save. Preserve logs before another opening replaces the latest report.

## Contributing and preparing releases

### What should I check before submitting a change?

Match surrounding formatting, use descriptive names and comment only
non-obvious decisions. Build the changed code and validate the affected backend,
interface or session/shortcut behavior. Include the behavior change, executed
checks and remaining hardware checks in the PR description; include screenshots
for visible interface changes.

Use concise imperative commit subjects without automatic AI attribution.
Preserve licenses and runtime identities. Keep firmware, plugin packages,
credentials, generated outputs and device/user files outside commits. These are
documented contribution conventions, not an automated PR gate. No automatic
full-firmware test pipeline or PR template is configured.

### How do I use this checkout from another development project?

External development consumers can accept `MPC_MANAGER`.
Use an absolute checkout path, or a path relative to the consuming project;
empty/omitted values retain their `../../plugin-manager-usb` workspace default.
Those consumers are separate projects, not contents required to build this one.

### How do I create a downloadable installer ZIP?

Generate a **fresh manager-only bundle** with the README packaging command,
omitting the optional plugin argument. Do not archive a used USB drive containing
plugins, history, preferences or logs. Set `OUTPUT_DIR` as in that recipe, then:

```sh
python3 - "$OUTPUT_DIR/usb" "$OUTPUT_DIR/plugin-manager-usb.zip" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
import zipfile

root = Path(sys.argv[1]).resolve()
archive = Path(sys.argv[2]).resolve()
manifest = json.loads((root / "USB-SHA256.json").read_text())
paths = list(root.rglob("*"))
if any(path.is_symlink() for path in paths):
    raise SystemExit("Bundle contains a symlink")
files = {str(path.relative_to(root)) for path in paths if path.is_file()}
if files != set(manifest) | {"USB-SHA256.json"}:
    raise SystemExit("Bundle contains missing or unlisted files")
if any((root / "local-plugins").iterdir()):
    raise SystemExit("Public bundle must not include plugins")
for name, expected in manifest.items():
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
        raise SystemExit("Checksum mismatch: " + name)
with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED) as output:
    for name in sorted(files):
        output.write(root / name, name)
    output.writestr("local-plugins/", b"")
print(hashlib.sha256(archive.read_bytes()).hexdigest(), archive.name)
PY
```

This verifies the generated inventory and preserves the hidden marker and
empty `local-plugins/` folder. Extracting the ZIP puts the launch files at the
USB root, without an extra enclosing folder. Use it only on fresh packager
output: an inventory is not a secret scan or proof of publisher identity.

Record the source commit, tested target and final ZIP checksum in release notes.
The commands do not upload or publish anything. Installer release automation
is not configured; creating a website does not create an installer release.

### What must stay out of a public release?

Keep Akai images/extracted files, plugin sources and ZIPs, pairing credentials,
API tokens, device settings/history and development logs out of the manager
distribution. Include our MIT license and the bundled third-party notices.
Public source, user guidance and manager-only downloads are published separately:
the [repository](https://github.com/Eurikon/plugin-manager-usb-for-open-mpc),
[website](https://eurikon.github.io/plugin-manager-usb-for-open-mpc/) and
[releases](https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases).
Release notes identify the source commit, tested target and installer checksum.
