# crosspoint-reader-enhanced

Personal fork of [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
adding **Bluetooth LE HID** support so a paired Bluetooth keyboard or remote can drive
page turns and reader navigation.

This fork tracks upstream and applies a focused set of additional features.
For general project info, build instructions, and user guide, see the upstream
[`README.md`](./README.md) and [`USER_GUIDE.md`](./USER_GUIDE.md).

## What this fork adds

- **BLE HID input** — pair a Bluetooth keyboard or remote via Settings → System →
  Bluetooth and use it for page turns, chapter skip, and menu navigation.
- **BLE-aware reader integration** — long-press chapter skip is suppressed during
  recent BLE input to prevent accidental jumps; status-bar refresh is windowed to
  reduce ghosting under BLE-driven page turns.
- **BLE-aware memory tuning** — XTC thumbnail generation uses a packed darkness
  score so Home thumbnail builds still fit under tight heap when the BLE stack is
  active.

Build target and toolchain are unchanged from upstream (PlatformIO, ESP32-C3,
Xteink X4).

## What this fork does not add (intentionally)

The upstream fork this work derives from also bundled a dungeon-crawler/RPG game,
a Solitaire game, a Subreddit reader, a CARDS UI theme, generic page-turn-speed
expansion, and other features. Those are out of scope here — this fork is BLE only.

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
  via the `allowLongPressChapterSkip()` gate, but the per-button hold semantics
  from the source fork are not preserved 1:1.

## Origin / credit

The BLE feature set was originally developed by
[thedrunkpenguin](https://github.com/thedrunkpenguin) in the
[`crosspoint-reader-ble`](https://github.com/thedrunkpenguin/crosspoint-reader-ble)
fork (branch `crosspoint-ble-1.2`). This fork extracts the BLE-only subset and
rebases it onto current upstream `master`, dropping unrelated features bundled
in that branch.

See `AGENTS.md` for general project conventions inherited from the upstream fork.
