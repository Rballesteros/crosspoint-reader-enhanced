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

[![Fund contributors](https://img.shields.io/badge/%F0%9F%91%91_Fund_contributors-royalty.dev-BB953A?style=for-the-badge&labelColor=1a1a1a)](https://app.royalty.dev/crosspoint-reader/crosspoint-reader)

CrossPoint is open-source e-reader firmware - community-built, fully hackable, free forever. It's maintained by a growing community of developers and readers who believe your device should do what you want - not what a manufacturer decided for you.

**Now running on:** ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3).

![CrossPoint Reader running on Xteink device](./docs/images/cover.jpg)

## What can CrossPoint do?

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter navigation, footnotes, go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more. 

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Screenshots.**

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 only)**.

- **Library workflow**: folder browser, hidden-file toggle, long-press delete, recent books, SD-cache management.

- **Wireless workflows**:
  
  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing WiFi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download
  - OTA update checks and installs from GitHub releases

- **Customization**: multiple themes (Classic, Lyra, Lyra Extended, RoundedRaff), sleep screen modes, front/side button remapping, status bar controls, power-button behavior, refresh cadence, and more.

- **Localization**: 22 UI languages and counting.

### Coming soon:

- RTL support — Arabic, Hebrew, and Farsi.

- Bookmarks.

- Dictionary lookup — inline word lookup without leaving the reader.

- More themes.

- Much more! stay tuned.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you will need to use the **Xteink Unlocker** tool available at
https://crosspointreader.com/#unlock-tool before you can flash CrossPoint.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

> ### ⚠️ WARNING: READ THIS BEFORE USING THE UNLOCKER ⚠️
> 
> **The only officially supported firmwares in the unlock tool are CrossPoint and CrossInk.**
> 
> Flashing any other firmware on a USB-locked device may **permanently brick the device** or leave it **permanently
> stuck on that firmware with no recovery path**. Once USB flashing is re-locked, your only way back is via OTA, and if
> the firmware you flashed doesn't support OTA, **there is no way out**.
> 
> **The Papyrix fork has removed OTA update support from its code.** If you flash Papyrix onto a
> USB-locked unit, you will have **zero update or recovery path** and will be stuck on it forever. **Do not flash
> Papyrix (or any other unsupported firmware) on a locked device.**

## Install firmware

### Web installer (recommended)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Go to https://crosspointreader.com/#flash-tools, select device (X3 or X4), and choose an official CrossPoint release.

### Web installer (specific version)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Download a `firmware.bin` from [Releases](https://github.com/crosspoint-reader/crosspoint-reader/releases), local build, or continuous integration artifact.
3. Go to https://crosspointreader.com/#flash-tools, select device (X3 or X4), click "Custom .bin" and upload a `firmware.bin`.

### Revert to Official Firmware

To revert to the official firmware, you can also flash the latest official firmware using https://crosspointreader.com/#flash-tools.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download `firmware.bin` from the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

Adjust `/dev/ttyACM0` to match your system.

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

Convert your own TTF/OTF files into `.cpfont` files that load from the SD card. No firmware reflash is needed.

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)

---

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader

# if cloned without --recursive:
git submodule update --init --recursive
```

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it's recommended to capture detailed
logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the 
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Note: the cache isn't cleared automatically when you delete a book, and moving a file to a new path resets its reading progress.

For more details on the internal file structures, see the
[file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the [contributing docs](./docs/contributing/README.md). For things to work on, check the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas) — leave a comment before starting so we don't duplicate effort.

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

## Community forks

One of the best things about open source is that anyone can take the code in a different direction. If you need something outside CrossPoint's [scope](./SCOPE.md), check out the community forks:

- [CrossInk](https://github.com/uxjulia/CrossInk) — Typography and reading tracking: Bionic Reading (bolds word stems to create fixation points), guide dots between words, improved paragraph indents, and replaces the default fonts with ChareInk/Lexend/Bitter.

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) — Adds FB2 and MD format support. Actively maintained with Arabic script support. Custom themes via SD card.

- [crosspet](https://github.com/trilwu/crosspet) — A Vietnamese fork that adds a Tamagotchi-style virtual chicken that grows based on your reading milestones (pages read, streaks, care). Also: Flashcards, Weather, Pomodoro timer, and mini-games.

- [crosspoint-reader-cjk](https://github.com/aBER0724/crosspoint-reader-cjk) — Purpose-built for Chinese, Japanese, and Korean reading.

- [inx](https://github.com/obijuankenobiii/inx) — Completely reimagines the user interface with tabbed navigation.

- ~~[PlusPoint](https://github.com/ngxson/pluspoint-reader) — custom JS apps support.~~ (Unmaintained)

- [crosspoint-reader-papers3](https://github.com/juicecultus/crosspoint-reader-papers3) — Crosspoint port for M5Stack Paper S3. 

**Note:** Many of these features will make their way into CrossPoint over time. We maintain a slower pace to ensure rock-solid stability and squash bugs before they reach your device.

Want to build your own device? Be sure to check out the [de-link](https://github.com/iandchasse/de-link) project.

---

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.

Huge shoutout to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired this project.
