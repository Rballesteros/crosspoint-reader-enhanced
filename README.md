# CrossPoint Reader

Firmware for the **Xteink X4** e-paper display reader (unaffiliated with Xteink).
Built using **PlatformIO** and targeting the **ESP32-C3** microcontroller.

CrossPoint Reader is a purpose-built firmware designed to be a drop-in, fully open-source replacement for the official 
Xteink firmware. It aims to match or improve upon the standard EPUB reading experience.

![](./docs/images/cover.jpg)

## Motivation

E-paper devices are fantastic for reading, but most commercially available readers are closed systems with limited 
customisation. The **Xteink X4** is an affordable, e-paper device, however the official firmware remains closed.
CrossPoint exists partly as a fun side-project and partly to open up the ecosystem and truly unlock the device's
potential.

CrossPoint Reader aims to:
* Provide a **fully open-source alternative** to the official firmware.
* Offer a **document reader** capable of handling EPUB content on constrained hardware.
* Support **customisable font, layout, and display** options.
* Run purely on the **Xteink X4 hardware**.

This project is **not affiliated with Xteink**; it's built as a community project.

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

Multi-language support: Read EPUBs in various languages, including English, Spanish, French, German, Italian, Portuguese, Russian, Ukrainian, Polish, Swedish, Norwegian, [and more](./USER_GUIDE.md#supported-languages).

See [the user guide](./USER_GUIDE.md) for instructions on operating CrossPoint, including the
[KOReader Sync quick setup](./USER_GUIDE.md#365-koreader-sync-quick-setup).

For a detailed summary of this branch compared to `master`, see [docs/branch-changes.md](./docs/branch-changes.md).

For more details about the scope of the project, see the [SCOPE.md](SCOPE.md) document.

### Bluetooth page turners

Bluetooth LE HID remotes can be configured from **Settings > System > Bluetooth**. EPUB readers also include a
Bluetooth entry in the reader menu so a remote can be connected without leaving the book flow.

The Bluetooth setup screen supports scanning, HID-device filtering, connecting, key learning, key testing, and a debug
monitor for inspecting incoming HID reports. Bonded remote details are saved in settings and reused for reconnects.

Bluetooth uses additional RAM on the ESP32-C3. When memory is tight, the reader now prefers safe fallbacks, such as
using a slower anti-aliased cleanup path instead of failing the page render. Bluetooth is disabled before deep sleep,
scans are stopped when leaving the Bluetooth screen, and the BLE transmit power is kept at the ESP32-C3 default instead
of maximum power to reduce battery drain. If the radio is enabled but no remote is connected or scanning, it suspends
after one idle minute and wakes again from reader-local input to look for the bonded remote.

### Improvements in this branch

- Bluetooth LE HID page-turner support using NimBLE.
- Bluetooth settings screen for enabling Bluetooth, scanning, filtering HID devices, connecting, learning keys, testing mappings, and debugging reports.
- Persistent bonded remote settings for reconnecting to the last paired remote.
- Known-device handling for common page-turn remotes, plus custom learned mappings for non-standard devices.
- Virtual button injection so Bluetooth remotes work through the same input path as the physical buttons.
- Bluetooth-aware reader page turns that avoid accidental chapter skips from delayed remote release events.
- Bluetooth activity tracking to reduce unwanted autosleep while a remote is in use.
- Bluetooth shutdown before deep sleep to keep the power path predictable.
- Lower BLE transmit power, stop active scans when leaving Bluetooth settings, and auto-suspend disconnected idle Bluetooth to reduce battery drain.
- Reader status bar Bluetooth indicator next to the battery: icon shown when Bluetooth is on, filled dot when connected.
- Reader menu shortcut for opening Bluetooth settings directly from an EPUB.
- Serial Bluetooth debug commands for scan, connect, and HID report troubleshooting.
- EPUB first-open behavior now preserves spine order instead of jumping directly to the OPF text reference.
- EPUB section builds clear font cache before retrying memory-heavy cache work.
- EPUB image extraction retries with smaller chunks and dimension probing retries for more reliable image handling.
- Low-heap CSS fallback to avoid spending memory on embedded styles when heap is already constrained.
- Anti-aliased text rendering safely falls back to a slower cleanup path if the BW scratch buffer cannot be allocated.
- Status bar counter partial refresh for faster Bluetooth-driven page turns.
- TXT reader chunk buffer handling uses checked `std::unique_ptr<uint8_t[]>` ownership.
- XTC 1-bit page rendering streams from storage instead of allocating a full page buffer.
- XTCH 2-bit grayscale rendering streams plane data in passes to reduce peak heap usage.
- XTC cover and thumbnail generation use lower-memory paths and low-heap thumbnail caps.
- XTC progress cache now stores total page count alongside current page.
- Rendering improvements include faster horizontal/vertical lines, partial display-window refresh, vector-managed BW buffer chunks, and improved gray dithering.
- Font decompression uses safer buffer ownership and a small hot-group cache.
- ZIP, EPUB, UTF-8, font, and rendering APIs use more `std::string_view` and RAII containers to reduce unnecessary allocation and manual memory management.
- Development version strings now use `base-version-dev-shortsha` without embedding the branch name.
- Branch documentation now summarizes the full diff against `master` in [docs/branch-changes.md](./docs/branch-changes.md).

## Installing

### Web (latest firmware)

1. Connect your Xteink X4 to your computer via USB-C and wake/unlock the device
2. Go to https://xteink.dve.al/ and click "Flash CrossPoint firmware"

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Web (specific firmware version)

1. Connect your Xteink X4 to your computer via USB-C
2. Download the `firmware.bin` file from the release of your choice via the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases)
3. Go to https://xteink.dve.al/ and flash the firmware file using the "OTA fast flash controls" section

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Command line (specific firmware version)

1. Install [`esptool`](https://github.com/espressif/esptool) :
```bash
pip install esptool
```
2. Download the `firmware.bin` file from the release of your choice via the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases)
3. Connect your Xteink X4 to your computer via USB-C.
4. Note the device location. On Linux, run `dmesg` after connecting. On MacOS, run :
```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```
5. Flash the firmware :
```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```
Change `/dev/ttyACM0` to the device for your system.

### Manual

See [Development](#development) below.

## Development

### Prerequisites

* **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
* Python 3.8+
* USB-C cable for flashing the ESP32-C3
* Xteink X4

### Checking out the code

CrossPoint uses PlatformIO for building and flashing the firmware. To get started, clone the repository:

```
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive
```

### Flashing your device

Connect your Xteink X4 to your computer via USB-C and run the following command.

```sh
pio run --target upload
```
### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

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

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only
has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based
on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the 
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:


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

Due the way it's currently implemented, the cache is not automatically cleared when a book is deleted and moving a book
file will use a new cache directory, resetting the reading progress.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

## Contributing

Contributions are very welcome!

If you are new to the codebase, start with the [contributing docs](./docs/contributing/README.md).

If you're looking for a way to help out, take a look at the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas).
If there's something there you'd like to work on, leave a comment so that we can avoid duplicated effort.

Everyone here is a volunteer, so please be respectful and patient. For more details on our governance and community 
principles, please see [GOVERNANCE.md](GOVERNANCE.md).

### To submit a contribution:

1. Fork the repo
2. Create a branch (`feature/dithering-improvement`)
3. Make changes
4. Submit a PR

---

CrossPoint Reader is **not affiliated with Xteink or any manufacturer of the X4 hardware**.

Huge shoutout to [**diy-esp32-epub-reader** by atomic14](https://github.com/atomic14/diy-esp32-epub-reader), which was a project I took a lot of inspiration from as I
was making CrossPoint.
