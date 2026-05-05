# CrossPoint Reader

CrossPoint Reader with **Bluetooth LE HID** support so a paired Bluetooth
keyboard or remote can drive page turns and reader navigation.

For general project info, build instructions, and user guide, see
[`README.md`](./README.md) and [`USER_GUIDE.md`](./USER_GUIDE.md).

## Bluetooth Additions

- **BLE HID input** — pair a Bluetooth keyboard or remote via Settings → System →
  Bluetooth and use it for page turns, chapter skip, and menu navigation.
- **BLE-aware reader integration** — long-press chapter skip is suppressed during
  recent BLE input to prevent accidental jumps; status-bar refresh is windowed to
  reduce ghosting under BLE-driven page turns.
- **BLE-aware memory tuning** — XTC thumbnail generation uses a packed darkness
  score so Home thumbnail builds still fit under tight heap when the BLE stack is
  active.

Build target and toolchain are unchanged (PlatformIO, ESP32-C3, Xteink X4).

## Building

Standard upstream flow:

```sh
pio run -e default
```

`default` env builds for the Xteink X4 (ESP32-C3). Other envs from upstream are
unchanged. Compile-tested green on macOS with PlatformIO 6.1.19 and the
ESP32-C3 toolchain.

## Branch model

- `master` — mirrors `upstream/master` verbatim. Never committed to directly.
- `enhanced` — long-lived integration branch with BLE features layered on top of
  `master`. This is the branch you build and run.

To sync with upstream:

```sh
git fetch upstream
git checkout master && git merge --ff-only upstream/master && git push
git checkout enhanced && git rebase master   # or merge, depending on preference
```

Conflicts on rebase are expected when upstream touches files we patch (notably
`main.cpp`, `EpubReaderActivity.cpp`, `SettingsList.h`, `CrossPointSettings.*`,
`MappedInputManager.*`). Resolve hunk by hunk and re-run `pio run -e default` to
verify.

## Pairing a BLE remote

1. Power on your Bluetooth keyboard / remote in pairing mode.
2. On the reader: Settings → System → Bluetooth → Scan.
3. Select your device when it appears. The reader bonds and remembers the
   address; on next boot it auto-reconnects.
4. From the reader: keys map to Next/Previous page, Menu, etc. Use the
   "Learn keys" flow in `BluetoothSettingsActivity` to remap.

Bonding state persists across reboots in NVS.

## Known caveats

- **NimBLE-Arduino 2.5.0** is required (bumped from upstream 2.3.6 by the BLE
  patches). `platformio.ini` pins this version.
- **Memory headroom is tight.** Build reports RAM 32.1%, Flash 91.9% on the
  default env. Adding more upstream features may push flash over the partition
  limit.
- **Hardware-test before relying on BLE in the field.** Compile-test only
  verifies the linker is happy, not that pairing or input injection actually
  works on device.
- The `chapterSkipConsumedForHold` field on `EpubReaderActivity` is declared but
  not fully wired into upstream's current page-turn flow (upstream restructured
  the long-press behavior). Long-press chapter skip on BLE remotes still works
  via the `allowLongPressChapterSkip()` gate.
