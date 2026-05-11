# CrossPoint Reader Enhanced — Rballesteros fork

```
    ____                    ____       _       _
   / ___|_ __ ___  ___ ___ |  _ \ ___ (_)_ __ | |_
  | |   | '__/ _ \/ __/ __|| |_) / _ \| | '_ \| __|
  | |___| | | (_) \__ \__ \|  __/ (_) | | | | | |_
   \____|_|  \___/|___/___/|_|   \___/|_|_| |_|\__|

         R E A D E R   ·   E N H A N C E D
                  Rballesteros fork

       BLE HID  ·  NimBLE-Arduino-enhanced  ·  ESP32-C3
```

> A focused fork of the open-source **CrossPoint Reader** firmware for the
> Xteink X4 e-paper reader (ESP32-C3). It adds production-grade Bluetooth LE
> HID page-turner support, ships a vendored NimBLE-Arduino fork with
> stack-init and reconnect fixes, and carries a chain of heap-hygiene and
> power-management work to keep the 380 KB RAM ceiling honest on a
> single-core RISC-V part.

**Fork repo:** <https://github.com/Rballesteros/crosspoint-reader-enhanced>
**Upstream:** <https://github.com/crosspoint-reader/crosspoint-reader>
**Vendored NimBLE:** <https://github.com/Rballesteros/NimBLE-Arduino-enhanced>

---

## Why this fork

I wanted Bluetooth page-turn remotes to *just work* on the X4 — pair, stay
paired, survive sleep cycles, and never strand the device with a wedged
radio stack. Getting NimBLE stable inside the ESP32-C3's 380 KB heap took
more than a configuration sweep; it took a small fork of NimBLE-Arduino
itself, plus a chain of memory- and power-management changes in the reader
firmware that fell out of that work. This branch carries all of that.

Everything below the **Original CrossPoint Reader** divider is the upstream
project, preserved so this fork remains usable as a drop-in replacement.

---

## Bluetooth LE HID — what this fork adds

End-user capability:

- **Pair, scan, and auto-reconnect** to bonded BLE HID remotes from
  `Settings → System → Bluetooth`. A shortcut entry is also available
  directly from the in-reader menu so a remote can be brought up without
  leaving the book flow.
- **Known-device profiles** for common page-turn remotes — e.g. the IINE
  Game Brick (MAC prefix `60:4d:ec`) — plus on-device **key learning** for
  remotes whose HID reports don't match a known profile.
- **Confirm / Back key learning** so multi-button remotes can drive menu
  navigation, not just page turns.
- **Virtual button injection**: BLE input flows through the same path as
  the physical buttons, so every activity that handles input handles BLE
  too — no per-screen plumbing.
- **Reader-aware behaviour**: long-press chapter-skip is suppressed
  during recent BLE input to avoid stray jumps, and the status-bar partial
  refresh is windowed to reduce ghosting under fast remote-driven page
  turns.
- **Status-bar BLE indicator** next to the battery: icon when the radio is
  on, filled dot when a remote is connected.
- **Power discipline**: lowered TX power compared to the ESP32 default,
  active scans stopped when leaving the Bluetooth screen, the radio
  auto-suspends after one idle minute when nothing is connected, and BT is
  fully torn down before deep sleep so the power path stays predictable.
- **Heap-aware fallbacks** for the times when BT is up and the heap is
  tight: XTC thumbnail generation uses packed darkness scores so Home
  thumbnail builds still fit, CSS parsing falls back when heap is tight,
  and anti-aliased text falls back to a slower cleanup path if the BW
  scratch buffer can't be allocated.

Debugging hooks:

- Serial commands available from the monitor:
  `BTSCAN`, `BTSCAN:<ms>`, `BTCONNECT:<addr>[,<addrType>]`,
  `BTDEBUG:ON`, `BTDEBUG:OFF`.
- An in-app HID report monitor in the Bluetooth settings screen for
  inspecting incoming reports during key learning.

---

## NimBLE-Arduino-enhanced — vendored library

This fork pins a small NimBLE-Arduino fork rather than the upstream
release. The pin lives in [`platformio.ini:76`](./platformio.ini) at:

```
https://github.com/Rballesteros/NimBLE-Arduino-enhanced.git#d7568c2dfe81e8af877cd8ad6db37b5fa08fd8d8
```

**Why the fork exists.** Two issues showed up consistently on the
ESP32-C3 single-core RISC-V build path:

1. **Stack init/teardown ordering** caused intermittent hangs on the first
   enable after boot, particularly when WiFi had been started earlier in
   the same session.
2. **Reconnect to a bonded device** after a short disconnect could miss a
   whitelist refresh and silently fail until the radio was fully cycled.

**Configuration.** NimBLE is configured central-only (this device only
acts as an HID *client*), with the minimum BLE ATT MTU since one HID
remote needs nothing more. The MYNEWT knobs are used directly because
`CONFIG_BT_NIMBLE_ROLE_*_DISABLED` is *not* consumed by NimBLE-Arduino —
see the comment in [`platformio.ini:41-43`](./platformio.ini).

**Boot-time controller-heap quirk.** Arduino-ESP32 3.x reclaims the BT
controller heap during startup unless a Bluetooth library marks it as
in-use before `app_main()`. The build flag `CROSSPOINT_BT_RESERVE_MEM=1`
(default for builds that ship with BT enabled) opts in to the reserve.
Setting `CROSSPOINT_BT_RESERVE_MEM=0` releases the BT controller heap to
the rest of the firmware — the trade-off is that toggling Bluetooth on
from the UI then requires a reboot. See the header comment in
`lib/hal/BluetoothHIDManager.cpp` for the full details.

**Companion firmware-side fix.** Commit `783d9ac`
("fix: bluetooth reconnection logic and NimBLE stack initialization
stability") on this branch adds a 20 ms post-init guard, restores
`deinit()`'s heap reclaim behaviour, and tightens the disconnect
timer lifecycle. It pairs with the NimBLE library fork above.

---

## What else this fork carries

Beyond the BLE work, this branch picks up a chain of memory- and
performance-related changes. Each links into [`docs/branch-changes.md`](./docs/branch-changes.md)
for full per-commit detail; commit hashes below are pointers into the
fork's git log.

- **RAII migration** — malloc/free converted to `std::unique_ptr` /
  `std::vector` in InflateReader, ZipFile, FontDecompressor, KOReaderSync
  (`a42f3ef`).
- **RAM-based hashed index for EPUB navigation** — replaces a linear
  spine-href scan with an FNV-1a hash table for O(1) lookup (`3522634`).
- **Optimised rendering** — kerning cache, fast horizontal/vertical line
  fill, partial-window display refresh, vector-managed BW buffer chunks
  (`76a0622`).
- **Activity framework + RTOS tuning** — render task lifecycle, priority
  audit, deterministic teardown (`5aed2aa`).
- **Flash wear-levelling** — settings and reading-history writes are
  guarded and debounced so SPIFFS sectors aren't thrashed (`15a014d`).
- **Heap and Bluetooth-path hardening** — heap budget pre-checks before
  large allocations, BT idle-radio suspend, WiFi/OTA pre-disable BT, CSS
  fallback under heap pressure (`18eafde`).
- **Zero-copy `string_view`** plumbing in I18n and path helpers
  (`881871e`).
- **Image decoding** — JPEG/PNG decoders converted to safe RAII
  ownership with smart pointers (`995ecd3`).
- **Recent-books store** releases unused capacity once trimmed
  (`531105c`).
- **In-reader menu header redesign** — chapter title gets its own line,
  metadata line shows `Chapter X/Y  Pages A/B  |  Z%` (this session).

For the full list, see [`docs/branch-changes.md`](./docs/branch-changes.md).

---

## Installing this fork

### Web flasher (recommended)

The upstream community-hosted web flasher at <https://xteink.dve.al/>
ships the upstream binary, so to install **this fork** instead:

1. Connect your Xteink X4 to your computer via USB-C and wake/unlock the
   device.
2. Download `firmware.bin` from the
   [fork's releases page](https://github.com/Rballesteros/crosspoint-reader-enhanced/releases).
3. Open <https://xteink.dve.al/> and use the **"OTA fast flash controls"**
   section to flash the downloaded `firmware.bin`.

To revert to the official Xteink firmware, flash from
<https://xteink.dve.al/>, or swap back to the other partition via the
"Swap boot partition" button at <https://xteink.dve.al/debug>.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):
   ```bash
   pip install esptool
   ```
2. Download `firmware.bin` from the
   [fork's releases page](https://github.com/Rballesteros/crosspoint-reader-enhanced/releases).
3. Connect the X4 via USB-C and note the serial device. On Linux run
   `dmesg`; on macOS run:
   ```bash
   log stream --predicate 'subsystem == "com.apple.iokit"' --info
   ```
4. Flash:
   ```bash
   esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
              write_flash 0x10000 /path/to/firmware.bin
   ```

### Build from source

```bash
git clone --recursive https://github.com/Rballesteros/crosspoint-reader-enhanced
cd crosspoint-reader-enhanced
pio run --target upload   # builds and flashes over USB-C
```

If you forgot `--recursive`, run:

```bash
git submodule update --init --recursive
```

PlatformIO will fetch `NimBLE-Arduino-enhanced` automatically based on the
pin in `platformio.ini`.

---

# Original CrossPoint Reader

Everything below this line is preserved from the upstream project for
reference. The fork above builds on this work without changing its scope.

> This fork builds on **CrossPoint Reader** by the
> [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
> project — a fully open-source firmware for the Xteink X4 e-paper reader,
> built with PlatformIO for the ESP32-C3.
>
> CrossPoint Reader is **not affiliated with Xteink or any manufacturer of
> the X4 hardware**.
>
> Huge shoutout to
> [**diy-esp32-epub-reader** by atomic14](https://github.com/atomic14/diy-esp32-epub-reader),
> which inspired the original project.

## Motivation

E-paper devices are fantastic for reading, but most commercially available
readers are closed systems with limited customisation. The **Xteink X4**
is an affordable e-paper device, however the official firmware remains
closed. CrossPoint exists partly as a fun side-project and partly to open
up the ecosystem and truly unlock the device's potential.

CrossPoint Reader aims to:

* Provide a **fully open-source alternative** to the official firmware.
* Offer a **document reader** capable of handling EPUB content on
  constrained hardware.
* Support **customisable font, layout, and display** options.
* Run purely on the **Xteink X4 hardware**.

This project is **not affiliated with Xteink**; it's built as a community
project.

## Features & Usage

- [x] EPUB parsing and rendering (EPUB 2 and EPUB 3)
- [x] Image support within EPUB
- [x] Saved reading position
- [x] File explorer with file picker
  - [x] Basic EPUB picker from root directory
  - [x] Support nested folders
  - [ ] EPUB picker with cover art
- [x] Custom sleep screen
  - [x] Cover sleep screen
- [x] Wifi book upload
- [x] Wifi OTA updates
- [x] KOReader Sync integration for cross-device reading progress
- [x] Bluetooth LE HID page-turn remote support
  - [x] Scan, connect, and auto-reconnect to page-turn remotes
  - [x] Learn previous/next key mappings for remotes with non-standard reports
  - [x] Optional confirm/back key learning for multi-button remotes
- [x] Configurable font, layout, and display options
  - [ ] User provided fonts
  - [ ] Full UTF support
- [x] Screen rotation
- [x] XTC/XTCH reader memory improvements with streaming page rendering
- [x] Low-memory rendering safeguards for EPUB images, XTC thumbnails, and anti-aliased text

Multi-language support: Read EPUBs in various languages, including
English, Spanish, French, German, Italian, Portuguese, Russian, Ukrainian,
Polish, Swedish, Norwegian,
[and more](./USER_GUIDE.md#supported-languages).

See [the user guide](./USER_GUIDE.md) for instructions on operating
CrossPoint, including the
[KOReader Sync quick setup](./USER_GUIDE.md#365-koreader-sync-quick-setup).

For more details about the scope of the project, see the
[SCOPE.md](SCOPE.md) document.

### Bluetooth page turners

Bluetooth LE HID remotes can be configured from
**Settings → System → Bluetooth**. EPUB readers also include a Bluetooth
entry in the reader menu so a remote can be connected without leaving the
book flow.

The Bluetooth setup screen supports scanning, HID-device filtering,
connecting, key learning, key testing, and a debug monitor for inspecting
incoming HID reports. Bonded remote details are saved in settings and
reused for reconnects.

Bluetooth uses additional RAM on the ESP32-C3. When memory is tight, the
reader prefers safe fallbacks, such as using a slower anti-aliased cleanup
path instead of failing the page render. Bluetooth is disabled before
deep sleep, scans are stopped when leaving the Bluetooth screen, and the
BLE transmit power is kept at the ESP32-C3 default instead of maximum
power to reduce battery drain. If the radio is enabled but no remote is
connected or scanning, it suspends after one idle minute and wakes again
from reader-local input to look for the bonded remote.

## Development

### Prerequisites

* **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
* Python 3.8+
* USB-C cable for flashing the ESP32-C3
* Xteink X4

### Checking out the code

CrossPoint uses PlatformIO for building and flashing the firmware. To get
started, clone the repository:

```
git clone --recursive https://github.com/Rballesteros/crosspoint-reader-enhanced

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive
```

### Flashing your device

Connect your Xteink X4 to your computer via USB-C and run the following
command.

```sh
pio run --target upload
```

### Debugging

After flashing the new features, it's recommended to capture detailed
logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

after that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

Bluetooth debug commands are also available from the serial monitor:

```text
BTDEBUG:ON
BTDEBUG:OFF
BTSCAN
BTSCAN:<milliseconds>
BTCONNECT:<address>
BTCONNECT:<address>,<addrType>
```

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD
card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM,
so we have to be careful. A lot of the decisions made in the design of
the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD
card. Subsequent loads are served from the cache. This cache directory
exists at `.crosspoint` on the SD card. The structure is as follows:

```
.crosspoint/
├── epub_12471232/       # Each EPUB is cached to a subdirectory named `epub_<hash>`
│   ├── progress.bin     # Stores reading progress (chapter, page, etc.)
│   ├── cover.bmp        # Book cover image (once generated)
│   ├── book.bin         # Book metadata (title, author, spine, table of contents, etc.)
│   └── sections/        # All chapter data is stored in the sections subdirectory
│       ├── 0.bin        # Chapter data (screen count, all text layout info, etc.)
│       ├── 1.bin        #     files are named by their index in the spine
│       └── ...
│
└── epub_189013891/
```

Deleting the `.crosspoint` directory will clear the entire cache.

Due the way it's currently implemented, the cache is not automatically
cleared when a book is deleted and moving a book file will use a new
cache directory, resetting the reading progress.

For more details on the internal file structures, see the
[file formats document](./docs/file-formats.md).

## Contributing

Contributions are welcome on either the upstream project or this fork.

For upstream contributions, start with the
[upstream contributing docs](./docs/contributing/README.md) and the
[upstream ideas board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas).

For fork-specific issues (BLE, NimBLE-Arduino-enhanced, fork-only
features), open an issue or PR against this repository.

For more details on upstream governance and community principles, see
[GOVERNANCE.md](GOVERNANCE.md).
