# Branch Changes Compared To master

This document summarizes the local `enhanced` branch compared to upstream `master`.

Comparison basis:

- Merge base: `upstream/master` at `3e7d63d`
- Current upstream/master observed during review: `395e68e`
- Branch head: `enhanced` at `2660563`
- Scope: committed branch changes plus reviewed working-tree fixes
- Code diff size: regenerate with `git diff --stat upstream/master...enhanced` before publishing exact counts

Branch commits:

- `ab1767f` - feat: add Bluetooth HID support
- `4d828dc` - feat: BLE follow-on improvements
- `81ba786` - docs: add FORK.md describing enhanced fork scope and branch model
- `881871e` - perf: zero-copy string handling in I18n and path helpers
- `a42f3ef` - refactor: migrate core memory management to RAII
- `76a0622` - perf: optimized rendering with fast line filling and kerning cache
- `3522634` - perf: RAM-based hashed index for EPUB navigation
- `995ecd3` - refactor: safe image decoding with RAII and smart pointers
- `5aed2aa` - refactor: activity framework cleanup and RTOS tuning
- `15a014d` - perf: flash wear leveling for settings and history
- `1b704c5` - feat: UX stability and CSS enhancements
- `4fc88ba` - chore: project metadata and build configuration updates
- `18d8931` - merge upstream `master` into `enhanced`

Last firmware build observed during review:

- Command: `/Users/mbp/.platformio/penv/bin/pio run`
- Result: success
- Version emitted by the build script: `1.2.0-dev-2660563`
- Platform stack: pioarduino `55.03.38-1`, Arduino-ESP32 `3.3.8`, ESP-IDF libs `5.5.4`, GCC `14.2.0+20260121`, NimBLE fork `0f3d130`
- Reported size: RAM 33.6% (`110116` / `327680` bytes), flash 92.3% (`6045979` / `6553600` bytes)

## High-Level Summary

The branch adds Bluetooth LE HID page-turner support and integrates it into the reader, settings UI, hardware input layer, power management, and versioning. It also includes a set of memory and reliability changes across EPUB parsing, image extraction, XTC page rendering, font decompression, ZIP inflation, bitmap rendering, and anti-aliased page rendering.

The most visible user-facing changes are:

- Bluetooth page-turn remotes can be enabled, scanned, connected, learned, debugged, and used as virtual reader buttons.
- The reader status bar shows a Bluetooth indicator next to the battery when Bluetooth is on, with a filled dot when a device is connected.
- Reader menus include a Bluetooth entry so remotes can be connected from reading context.
- Page turns from Bluetooth remotes use safer semantics to avoid accidental chapter skips caused by delayed or synthetic release events.
- EPUB first-open behavior no longer jumps directly to the OPF text reference. This preserves the beginning of the spine, including front matter or cover pages when present in the spine.
- Anti-aliased rendering now uses a slower cleanup fallback if the temporary BW buffer cannot be allocated, instead of continuing into a memory-pressure failure path.
- XTC/XTCH page and thumbnail rendering are much more streaming-oriented, reducing large heap allocations.
- Development versions no longer include the git branch name, only the base version and short SHA.

## Bluetooth HID Support

New files:

- `lib/hal/BluetoothHIDManager.cpp`
- `lib/hal/BluetoothHIDManager.h`
- `lib/hal/DeviceProfiles.cpp`
- `lib/hal/DeviceProfiles.h`
- `src/activities/settings/BluetoothSettingsActivity.cpp`
- `src/activities/settings/BluetoothSettingsActivity.h`
- `src/idf_component.yml`

Key behavior:

- Adds a NimBLE-based singleton Bluetooth HID manager.
- Supports enabling and disabling Bluetooth at runtime.
- Supports scanning, filtering, identifying, connecting, disconnecting, and auto-reconnecting to a bonded device.
- Stores discovered devices with address, name, RSSI, HID flag, address type, appearance, and manufacturer company ID.
- Tracks connected devices, subscribed HID report characteristics, last activity time, active injected virtual button, last HID keycode, report framing, and device-specific profile data.
- Parses HID reports from common keyboard and consumer-page remotes.
- Adds known page-turner profiles for devices such as IINE Game Brick variants, MINI_KEYBOARD, Kobo Remote, Free2-style devices, Free2-M, and Free3-M.
- Supports user-learned key mappings per device, including previous, next, optional confirm, and optional cancel actions.
- Handles several remote-specific behaviors, including rolling Free2 codes, Game Brick counter/freeze behavior, startup noise suppression, delayed release tails, and generic page-turn code fallback.
- Exposes debug capture state and last error strings for UI and serial troubleshooting.
- Adds remote inactivity management and recent-activity checks so active Bluetooth use can suppress autosleep.
- Uses lower BLE transmit power than the previous max-power setting and exposes a cheap connected-device check for UI status.

## Settings And Persistence

Files changed:

- `src/CrossPointSettings.cpp`
- `src/CrossPointSettings.h`
- `src/JsonSettingsIO.cpp`
- `src/SettingsList.h`
- `src/activities/settings/SettingsActivity.cpp`
- `src/activities/settings/SettingsActivity.h`
- `lib/I18n/translations/english.yaml`

Key behavior:

- Adds persistent `bluetoothEnabled`.
- Adds persistent bonded remote fields:
  - `bleBondedDeviceAddr`
  - `bleBondedDeviceName`
  - `bleBondedDeviceAddrType`
- Allows `CrossPointSettings` to be copied and compared.
- Adds equality and inequality operators so unchanged settings can avoid unnecessary writes.
- Persists Bluetooth bonded-device metadata through JSON settings.
- Adds Bluetooth as a System setting.
- Adds settings UI routing into the Bluetooth settings activity.
- Adds translation strings for Bluetooth UI labels and statuses.

## Bluetooth Settings UI

New activity:

- `src/activities/settings/BluetoothSettingsActivity.cpp`
- `src/activities/settings/BluetoothSettingsActivity.h`

Key behavior:

- Adds a Bluetooth settings screen with four internal views:
  - Main menu
  - Device list
  - Learn keys
  - Debug monitor
- Lets the user enable and disable Bluetooth.
- Stops active scans when leaving Bluetooth settings so scanning cannot continue in the background after exit.
- Starts scans and displays nearby devices.
- Can filter the device list to HID-advertising devices.
- Keeps cursor selection stable while scan results reorder.
- Supports identifying a device by probing its GATT Device Name.
- Connects to a selected device and can optionally exit back to the reader after successful connection.
- Provides a key-learning flow for previous, next, optional confirm, and optional cancel.
- Provides a test step to verify learned forward and back actions.
- Provides a debug monitor with event counts, last key, and unique observed keycodes.

## Main Loop, Power, And Serial Commands

Files changed:

- `src/main.cpp`
- `lib/hal/HalGPIO.cpp`
- `lib/hal/HalGPIO.h`

Key behavior:

- Initializes `BluetoothHIDManager` during setup.
- Wires Bluetooth HID events into `HalGPIO` as virtual button state.
- Adds callbacks for button injection, button activity, reader-context detection, and bonded-device loading.
- Updates Bluetooth maintenance and auto-reconnect logic in the main loop.
- Includes recent Bluetooth activity in autosleep prevention.
- Disables Bluetooth before deep sleep.
- Auto-suspends the Bluetooth radio after one disconnected idle minute, then wakes it from local reader input to search for the bonded remote.
- Adds serial debug commands:
  - `BTDEBUG:ON`
  - `BTDEBUG:OFF`
  - `BTSCAN`
  - `BTSCAN:<milliseconds>`
  - `BTCONNECT:<address>`
  - `BTCONNECT:<address>,<addrType>`
- Moves wake power-button duration handling into the GPIO/input path and removes the older local helper from `main.cpp`.

## Virtual Button Input

Files changed:

- `lib/hal/HalGPIO.cpp`
- `lib/hal/HalGPIO.h`
- `src/MappedInputManager.cpp`
- `src/MappedInputManager.h`

Key behavior:

- Adds virtual button state for Bluetooth-injected input.
- Integrates virtual buttons into press, release, held-time, and any-pressed checks.
- Adds virtual input latching/debounce behavior to reduce duplicate events from noisy HID reports.
- Tracks recent virtual-button activity separately from physical GPIO input.
- Extends mapped input so Bluetooth-injected button state behaves like the existing reader controls.

## Activity Routing And Reader Menu Integration

Files changed:

- `src/activities/Activity.cpp`
- `src/activities/Activity.h`
- `src/activities/ActivityManager.cpp`
- `src/activities/ActivityManager.h`
- `src/activities/reader/EpubReaderMenuActivity.cpp`
- `src/activities/reader/EpubReaderMenuActivity.h`
- `src/activities/reader/EpubReaderActivity.cpp`
- `src/activities/reader/EpubReaderActivity.h`

Key behavior:

- Adds `ActivityManager::goToBluetoothSettings(...)`.
- Adds Bluetooth as an EPUB reader menu action.
- Allows opening Bluetooth settings from the reader menu.
- Supports returning directly to the reader after successful Bluetooth connection.
- Forces a stronger refresh when returning from menu/settings overlays so the book page re-establishes a clean display baseline.
- Suppresses stale confirm/back input immediately after returning from modal activities.
- Raises render task priority in the activity manager path.
- Modernizes some route/helper APIs around `std::string_view`.

## EPUB Reader Behavior

Files changed:

- `src/activities/reader/EpubReaderActivity.cpp`
- `src/activities/reader/EpubReaderActivity.h`
- `src/activities/reader/ReaderActivity.cpp`
- `src/activities/reader/ReaderActivity.h`
- `src/activities/reader/ReaderUtils.h`

Key behavior:

- Removes first-open automatic navigation to `textReference`. New books now start at the current spine index instead of skipping directly to the OPF text reference.
- Adds Bluetooth-aware page-turn handling:
  - Recent Bluetooth page-turn input disables long-press chapter skipping.
  - Local hardware buttons keep the configured long-press chapter-skip behavior.
- Adds Bluetooth-aware status counter refresh:
  - Uses partial window refresh for the status bar counter area when recent Bluetooth input is present.
  - Helps keep page counters/progress visible during fast remote page turns.
- Clears the font cache before building a missing EPUB section cache to free memory for ZIP inflation.
- Retries failed page loads up to three times before rendering an error message.
- Saves progress using the spine index passed to `saveProgress(...)`, not always the current in-memory spine index.
- Re-renders the status bar during image anti-aliasing cleanup so the final BW frame stays synchronized.
- If BW scratch buffer allocation fails before anti-aliased rendering, uses the slower re-render cleanup path and keeps the BW frame synchronized.

## TXT Reader Changes

Files changed:

- `src/activities/reader/TxtReaderActivity.cpp`

Key behavior:

- Uses the centralized activity manager home routing.
- Replaces a manual `malloc`/`free` text read buffer with checked `std::unique_ptr<uint8_t[]>` ownership.
- Adds Bluetooth-aware status counter partial refresh for TXT page renders.

## XTC Reader And XTC Format Changes

Files changed:

- `src/activities/reader/XtcReaderActivity.cpp`
- `lib/Xtc/Xtc.cpp`
- `lib/Xtc/Xtc.h`
- `lib/I18n/translations/english.yaml`

Key behavior:

- Uses common reader page-turn helpers, including Bluetooth-aware chapter-skip suppression.
- Streams 1-bit XTC pages from storage instead of allocating a full page buffer.
- Streams 2-bit XTCH grayscale planes in multiple passes instead of allocating the full page buffer.
- Adds error handling for invalid page geometry and stream read failures.
- Saves total page count in the XTC progress cache alongside the current page.
- Adds `Xtc::getPageInfo(...)`.
- Reworks cover and thumbnail generation around streaming/derived monochrome bitmaps.
- Changes generated cover/thumbnail cache names, including `cover_v2.bmp` and `thumb_v3_*.bmp`.
- Caps thumbnail target size under low heap to avoid failures when returning to Home while Bluetooth is active.
- Writes small empty thumbnail placeholders for cases where thumbnail generation is intentionally skipped.

## EPUB, ZIP, CSS, And Image Reliability

Files changed:

- `lib/Epub/Epub.cpp`
- `lib/Epub/Epub.h`
- `lib/Epub/Epub/BookMetadataCache.cpp`
- `lib/Epub/Epub/BookMetadataCache.h`
- `lib/Epub/Epub/Page.cpp`
- `lib/Epub/Epub/ParsedText.cpp`
- `lib/Epub/Epub/Section.cpp`
- `lib/Epub/Epub/Section.h`
- `lib/Epub/Epub/blocks/ImageBlock.cpp`
- `lib/Epub/Epub/blocks/TextBlock.cpp`
- `lib/Epub/Epub/converters/DirectPixelWriter.h`
- `lib/Epub/Epub/converters/ImageDecoderFactory.cpp`
- `lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp`
- `lib/Epub/Epub/converters/ImageToFramebufferDecoder.h`
- `lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp`
- `lib/Epub/Epub/converters/JpegToFramebufferConverter.h`
- `lib/Epub/Epub/converters/PixelCache.h`
- `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp`
- `lib/Epub/Epub/converters/PngToFramebufferConverter.h`
- `lib/Epub/Epub/css/CssParser.cpp`
- `lib/Epub/Epub/css/CssParser.h`
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`
- `lib/Epub/Epub/parsers/ContentOpfParser.h`
- `lib/InflateReader/InflateReader.cpp`
- `lib/InflateReader/InflateReader.h`
- `lib/ZipFile/ZipFile.cpp`
- `lib/ZipFile/ZipFile.h`

Key behavior:

- Converts several raw heap allocations to RAII containers such as `std::vector` and `std::unique_ptr`.
- Adds `std::string_view` APIs in EPUB, ZIP, CSS, parsing, and rendering paths to reduce unnecessary string handling.
- Changes `Epub::readItemContentsToBytes(...)` to return a `std::vector<uint8_t>`.
- Adds stream-oriented ZIP/EPUB reads for lower peak memory.
- Adds configurable smaller inflate dictionaries for smaller files.
- Adds retry logic for EPUB image extraction with smaller chunk sizes: 1024, 768, and 512 bytes.
- Retries image dimension probing after extraction.
- Tracks supported-image extraction failures during parsing.
- Allows silent next-chapter cache builds to fail if supported image extraction failed transiently, preventing bad cached sections.
- Clears font cache before retrying some section/cache work under memory pressure.
- Adds a low-heap CSS guard that returns an empty style set when heap is below the configured threshold.
- Adds a faster href-to-spine lookup through metadata cache hash indexing.

## Rendering And Font Memory Changes

Files changed:

- `lib/GfxRenderer/GfxRenderer.cpp`
- `lib/GfxRenderer/GfxRenderer.h`
- `lib/GfxRenderer/Bitmap.cpp`
- `lib/GfxRenderer/Bitmap.h`
- `lib/GfxRenderer/BitmapHelpers.h`
- `lib/EpdFont/EpdFont.cpp`
- `lib/EpdFont/EpdFont.h`
- `lib/EpdFont/EpdFontFamily.cpp`
- `lib/EpdFont/EpdFontFamily.h`
- `lib/EpdFont/FontDecompressor.cpp`
- `lib/EpdFont/FontDecompressor.h`
- `lib/Utf8/Utf8.cpp`
- `lib/Utf8/Utf8.h`

Key behavior:

- Adds optimized fast horizontal and vertical line rendering.
- Adds a compact Bluetooth status indicator to the reader status bar near the battery icon.
- Uses fast line rendering inside rectangle and line drawing paths.
- Adds `GfxRenderer::displayWindow(...)` for partial display refresh.
- Changes text measurement/drawing/truncation APIs toward `std::string_view`.
- Adds `utf8NextCodepoint(std::string_view&)`.
- Replaces several bitmap row and polygon temporary buffers with `std::vector`.
- Changes BW buffer storage from raw chunk pointers to vector-managed chunks.
- Improves light-gray and dark-gray dither patterns with 4x4 Bayer matrices.
- Updates font decompression to use vector-managed buffers and a small hot-group cache.
- Adds more string-view based font APIs and small cache improvements.

## Filesystem, Utilities, And Other Library Changes

Files changed:

- `lib/FsHelpers/FsHelpers.cpp`
- `lib/FsHelpers/FsHelpers.h`
- `lib/JpegToBmpConverter/JpegToBmpConverter.cpp`
- `lib/PngToBmpConverter/PngToBmpConverter.cpp`
- `lib/KOReaderSync/KOReaderSyncClient.cpp`
- `lib/KOReaderSync/KOReaderSyncClient.h`
- `src/RecentBooksStore.cpp`
- `src/util/QrUtils.cpp`
- `src/util/QrUtils.h`
- `src/util/ScreenshotUtil.cpp`
- `src/activities/browser/OpdsBookBrowserActivity.cpp`
- `src/activities/home/FileBrowserActivity.cpp`
- `src/activities/home/HomeActivity.cpp`
- `src/activities/home/HomeActivity.h`
- `src/activities/home/RecentBooksActivity.cpp`
- `src/activities/network/CrossPointWebServerActivity.cpp`
- `src/activities/util/BmpViewerActivity.cpp`
- `src/components/themes/BaseTheme.cpp`

Key behavior:

- Applies smaller memory and API cleanup changes across filesystem helpers, QR utilities, screenshots, KOReader Sync, OPDS browsing, Home, Recent Books, and the web server.
- Replaces several manual buffers with safer standard-library containers.
- Updates call sites for the new rendering/string-view/settings APIs.
- Updates recent-book behavior for changed XTC cover/thumbnail cache paths.
- Renders grayscale BMP viewer previews with the full LSB/MSB grayscale pass instead of showing only the dark BW base pass.

## Build And Versioning

Files changed:

- `platformio.ini`
- `scripts/git_branch.py`
- `src/idf_component.yml`

Key behavior:

- Adds `h2zero/NimBLE-Arduino @ 2.5.0`.
- Adds the `ENABLE_BT_DEBUG_MONITOR` build flag.
- Keeps `CROSSPOINT_VERSION` based on base firmware version plus short SHA.
- Removes the git branch name from generated dev versions.
- Adds ESP-IDF component metadata needed by the added Bluetooth/NimBLE integration.

Current dev version format:

```text
1.2.0-dev-<short-sha>
```

Example from the current branch:

```text
1.2.0-dev-2660563
```

## Documentation And Repository Metadata

Files changed:

- `AGENTS.md`
- `FORK.md`
- `README.md`
- `docs/branch-changes.md`

Key behavior:

- Adds agent/development guidance in `AGENTS.md`.
- Adds branch/fork notes in `FORK.md`, now written as CrossPoint Reader Bluetooth notes.
- Updates `README.md` with Bluetooth page-turner support, a branch improvements list, branch-change documentation, and serial Bluetooth debug commands.
- Adds this document to summarize the branch changes against `master`.

## Platform Update Research

The current `platformio.ini` points at pioarduino `55.03.38-1`, which packages Arduino-ESP32 `3.3.8`, ESP-IDF libs `5.5.4`, and the 2026 GCC 14.2.0 toolchain for this build.

Relevant upstream notes:

- Arduino-ESP32 `3.3.8` is based on ESP-IDF `5.5.4` and includes Bluetooth/BLE, WiFi, WebServer, Update, UART, USB, and board-support fixes: https://github.com/espressif/arduino-esp32/releases/tag/3.3.8
- ESP-IDF `5.5.4` is a bugfix release for the ESP-IDF 5.5 line. Its headline bugfix is a potential NimBLE host connection loss on ESP32, ESP32-C3, and ESP32-S3: https://github.com/espressif/esp-idf/releases/tag/v5.5.4
- pioarduino `55.03.38-1` is the latest listed pioarduino release. It fixes an OpenOCD build command issue and states there are no Arduino or IDF changes compared with `55.03.38`: https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.38-1

Conclusion: the update is worthwhile for this branch because the ESP-IDF NimBLE fix directly intersects the Bluetooth HID work. No heap or performance improvement is claimed from the platform bump alone; the verified improvement is compatibility/stability, with build size measured above.

## Operational Notes

- Bluetooth support increases firmware size and runtime memory pressure. The reviewed default build reported around 92.3% flash usage.
- When Bluetooth is connected, the reader may have less free heap for image extraction, grayscale text anti-aliasing, XTC thumbnails, and section cache building.
- Anti-aliased text rendering now intentionally uses a slower cleanup fallback if the BW scratch buffer cannot be stored. This avoids a hard render failure and leaves the readable BW page on screen.
- Existing `progress.bin` files can still resume a book at the previously saved spine/page. Removing the per-book progress file forces a fresh open from the beginning.
- The EPUB first-open fix preserves spine order. If a book's cover image is only declared as metadata and is not part of the spine, the reader will not synthesize a cover page from that metadata.
- XTC cover and thumbnail cache filename changes mean older generated cache files may be ignored and regenerated.

## Complete Changed-File Inventory

Added files:

- `AGENTS.md`
- `FORK.md`
- `lib/hal/BluetoothHIDManager.cpp`
- `lib/hal/BluetoothHIDManager.h`
- `lib/hal/DeviceProfiles.cpp`
- `lib/hal/DeviceProfiles.h`
- `src/activities/settings/BluetoothSettingsActivity.cpp`
- `src/activities/settings/BluetoothSettingsActivity.h`
- `src/idf_component.yml`
- `docs/branch-changes.md`

Modified files:

- `README.md`
- `lib/EpdFont/EpdFont.cpp`
- `lib/EpdFont/EpdFont.h`
- `lib/EpdFont/EpdFontFamily.cpp`
- `lib/EpdFont/EpdFontFamily.h`
- `lib/EpdFont/FontDecompressor.cpp`
- `lib/EpdFont/FontDecompressor.h`
- `lib/Epub/Epub.cpp`
- `lib/Epub/Epub.h`
- `lib/Epub/Epub/BookMetadataCache.cpp`
- `lib/Epub/Epub/BookMetadataCache.h`
- `lib/Epub/Epub/Page.cpp`
- `lib/Epub/Epub/ParsedText.cpp`
- `lib/Epub/Epub/Section.cpp`
- `lib/Epub/Epub/Section.h`
- `lib/Epub/Epub/blocks/ImageBlock.cpp`
- `lib/Epub/Epub/blocks/TextBlock.cpp`
- `lib/Epub/Epub/converters/DirectPixelWriter.h`
- `lib/Epub/Epub/converters/ImageDecoderFactory.cpp`
- `lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp`
- `lib/Epub/Epub/converters/ImageToFramebufferDecoder.h`
- `lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp`
- `lib/Epub/Epub/converters/JpegToFramebufferConverter.h`
- `lib/Epub/Epub/converters/PixelCache.h`
- `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp`
- `lib/Epub/Epub/converters/PngToFramebufferConverter.h`
- `lib/Epub/Epub/css/CssParser.cpp`
- `lib/Epub/Epub/css/CssParser.h`
- `lib/Epub/Epub/hyphenation/HyphenationCommon.cpp`
- `lib/Epub/Epub/hyphenation/HyphenationCommon.h`
- `lib/Epub/Epub/hyphenation/Hyphenator.cpp`
- `lib/Epub/Epub/hyphenation/Hyphenator.h`
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`
- `lib/Epub/Epub/parsers/ContentOpfParser.h`
- `lib/FsHelpers/FsHelpers.cpp`
- `lib/FsHelpers/FsHelpers.h`
- `lib/GfxRenderer/Bitmap.cpp`
- `lib/GfxRenderer/Bitmap.h`
- `lib/GfxRenderer/BitmapHelpers.h`
- `lib/GfxRenderer/GfxRenderer.cpp`
- `lib/GfxRenderer/GfxRenderer.h`
- `lib/I18n/I18n.cpp`
- `lib/I18n/I18n.h`
- `lib/I18n/translations/english.yaml`
- `lib/InflateReader/InflateReader.cpp`
- `lib/InflateReader/InflateReader.h`
- `lib/JpegToBmpConverter/JpegToBmpConverter.cpp`
- `lib/KOReaderSync/KOReaderSyncClient.cpp`
- `lib/KOReaderSync/KOReaderSyncClient.h`
- `lib/PngToBmpConverter/PngToBmpConverter.cpp`
- `lib/Utf8/Utf8.cpp`
- `lib/Utf8/Utf8.h`
- `lib/Xtc/Xtc.cpp`
- `lib/Xtc/Xtc.h`
- `lib/ZipFile/ZipFile.cpp`
- `lib/ZipFile/ZipFile.h`
- `lib/hal/HalDisplay.cpp`
- `lib/hal/HalDisplay.h`
- `lib/hal/HalGPIO.cpp`
- `lib/hal/HalGPIO.h`
- `platformio.ini`
- `scripts/git_branch.py`
- `src/CrossPointSettings.cpp`
- `src/CrossPointSettings.h`
- `src/JsonSettingsIO.cpp`
- `src/MappedInputManager.cpp`
- `src/MappedInputManager.h`
- `src/RecentBooksStore.cpp`
- `src/SettingsList.h`
- `src/activities/Activity.cpp`
- `src/activities/Activity.h`
- `src/activities/ActivityManager.cpp`
- `src/activities/ActivityManager.h`
- `src/activities/browser/OpdsBookBrowserActivity.cpp`
- `src/activities/home/FileBrowserActivity.cpp`
- `src/activities/home/HomeActivity.cpp`
- `src/activities/home/HomeActivity.h`
- `src/activities/home/RecentBooksActivity.cpp`
- `src/activities/network/CrossPointWebServerActivity.cpp`
- `src/activities/reader/EpubReaderActivity.cpp`
- `src/activities/reader/EpubReaderActivity.h`
- `src/activities/reader/EpubReaderMenuActivity.cpp`
- `src/activities/reader/EpubReaderMenuActivity.h`
- `src/activities/reader/ReaderActivity.cpp`
- `src/activities/reader/ReaderActivity.h`
- `src/activities/reader/ReaderUtils.h`
- `src/activities/reader/TxtReaderActivity.cpp`
- `src/activities/reader/XtcReaderActivity.cpp`
- `src/activities/settings/SettingsActivity.cpp`
- `src/activities/settings/SettingsActivity.h`
- `src/activities/util/BmpViewerActivity.cpp`
- `src/components/themes/BaseTheme.cpp`
- `src/main.cpp`
- `src/util/QrUtils.cpp`
- `src/util/QrUtils.h`
- `src/util/ScreenshotUtil.cpp`
