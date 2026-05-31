#include "BleDecoders.h"

#include <cstring>

namespace BleDecoders {

namespace {

// Standard HID keyboard / consumer codes that the GenericExtract decoder
// prefers when scanning a report. Anything else is still returned (clickers
// emit vendor-specific codes), but these get first pick.
constexpr uint8_t STANDARD_PAGE_UP = 0xE9;
constexpr uint8_t STANDARD_PAGE_DOWN = 0xEA;
constexpr uint8_t KEYBOARD_PAGE_UP = 0x4B;
constexpr uint8_t KEYBOARD_PAGE_DOWN = 0x4E;
constexpr uint8_t KEYBOARD_UP_ARROW = 0x52;
constexpr uint8_t KEYBOARD_DOWN_ARROW = 0x51;
constexpr uint8_t KEYBOARD_LEFT_ARROW = 0x50;
constexpr uint8_t KEYBOARD_RIGHT_ARROW = 0x4F;
constexpr uint8_t KEYBOARD_SPACE = 0x2C;
constexpr uint8_t KEYBOARD_ENTER = 0x28;
constexpr uint8_t KEYBOARD_VOLUME_UP = 0x80;
constexpr uint8_t KEYBOARD_VOLUME_DOWN = 0x81;

bool isLikelyPageNavCode(uint8_t code) {
  switch (code) {
    case STANDARD_PAGE_UP:
    case STANDARD_PAGE_DOWN:
    case KEYBOARD_PAGE_UP:
    case KEYBOARD_PAGE_DOWN:
    case KEYBOARD_UP_ARROW:
    case KEYBOARD_DOWN_ARROW:
    case KEYBOARD_LEFT_ARROW:
    case KEYBOARD_RIGHT_ARROW:
    case KEYBOARD_SPACE:
    case KEYBOARD_ENTER:
    case KEYBOARD_VOLUME_UP:
    case KEYBOARD_VOLUME_DOWN:
      return true;
    default:
      return false;
  }
}

DecodedFrame decodeGenericExtract(const uint8_t* data, size_t length, const ReportHints& hints) {
  DecodedFrame frame;
  if (!data || length == 0) {
    return frame;
  }

  const size_t scanLen = length < 8 ? length : 8;

  // Descriptor-hinted byte first: many remotes advertise the exact byte
  // index, and that's strictly better than blind scanning.
  if (hints.preferredByteIndex != 0xFF && hints.preferredByteIndex < scanLen) {
    const uint8_t b = data[hints.preferredByteIndex];
    if (b != 0x00 && b != 0xFF) {
      frame.keycode = b;
      frame.reportIndex = hints.preferredByteIndex;
      frame.pressed = true;
      return frame;
    }
  }

  // Prefer the well-known page-nav codes anywhere in the report.
  for (size_t i = 0; i < scanLen; i++) {
    if (isLikelyPageNavCode(data[i])) {
      frame.keycode = data[i];
      frame.reportIndex = static_cast<uint8_t>(i);
      frame.pressed = true;
      return frame;
    }
  }

  // Typical keyboard report key slots (bytes 2..7).
  for (size_t i = 2; i < scanLen; i++) {
    if (data[i] != 0x00 && data[i] != 0xFF) {
      frame.keycode = data[i];
      frame.reportIndex = static_cast<uint8_t>(i);
      frame.pressed = true;
      return frame;
    }
  }

  // Last-ditch: first non-zero byte for non-keyboard layouts.
  for (size_t i = 0; i < scanLen; i++) {
    if (data[i] != 0x00 && data[i] != 0xFF) {
      frame.keycode = data[i];
      frame.reportIndex = static_cast<uint8_t>(i);
      frame.pressed = true;
      return frame;
    }
  }

  return frame;  // pressed=false, keycode=0 — clean release frame.
}

// GameBrick V2 report format (confirmed via captures):
//   byte[0]   : 0x13 active, 0x12 release tail (bit0 = pressed)
//   byte[1-2] : 16-bit LE cycling counter (+125 per ~8ms frame)
//   byte[3]   : horizontal X axis (joystick), center = 0x98
//   byte[4]   : 0x07 = physical UP, 0x09 = physical DOWN, 0x08 = idle/center
//
// The counter FREEZES to 0x07D0 while any physical button is held; resumes
// cycling while the joystick is moving. We use that to distinguish UP/DOWN
// presses from joystick travel that happens to pass through 0x07/0x09.
DecodedFrame decodeGameBrickV2(const uint8_t* data, size_t length, State& state) {
  DecodedFrame frame;
  if (!data || length < 5) {
    return frame;
  }

  // Ignore transitional frames (0x2x/0x3x report-type bytes) — they can race
  // a real press and emit a spurious double-tap. Bit-pattern (data[0]&0xF0)==0x10
  // is the stable digital-button report family. Anything else: hold previous
  // logical state, return release frame for safety.
  const bool stableButtonReport = (data[0] & 0xF0) == 0x10;
  if (!stableButtonReport) {
    return frame;
  }

  const bool activeFrame = (data[0] & 0x01) != 0;
  const uint16_t counter = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);
  const bool counterFrozen = (counter == state.gameBrickLastCounter);
  state.gameBrickLastCounter = counter;
  const bool isDirectionalFreezeWindow = (counter == 0x07D0);

  // Reset the directional latch once the counter resumes cycling or the
  // release-tail (bit0=0) arrives, so the next press is decoded fresh.
  if (!counterFrozen || !activeFrame) {
    state.gameBrickLatchedKey = 0x00;
  }

  const uint8_t b4 = data[4];

  if (b4 == 0x07 || b4 == 0x09) {
    const bool directionalFreezeWindow =
        isDirectionalFreezeWindow || (counterFrozen && state.gameBrickLatchedKey != 0x00);

    if (directionalFreezeWindow) {
      // D-pad UP/DOWN: latch the first directional code so the release ramp
      // crossing the opposite code doesn't fire a phantom second press.
      if (state.gameBrickLatchedKey == 0x00) {
        state.gameBrickLatchedKey = b4;
      }
      if (b4 == state.gameBrickLatchedKey) {
        frame.keycode = b4;
        frame.reportIndex = 4;
        frame.pressed = activeFrame;
      }
    } else {
      // Non-frozen window: byte[4] 0x07/0x09 means physical A/B action button.
      // Emit synthetic codes so the mapping layer can address them stably.
      frame.keycode = (b4 == 0x07) ? GAMEBRICK_ACTION_A : GAMEBRICK_ACTION_B;
      frame.reportIndex = 4;
      frame.pressed = activeFrame;
    }
    state.gameBrickCenterFrames = 0;
    return frame;
  }

  if (b4 == 0x08) {
    // Joystick horizontal. Vertical d-pad latch blocks horizontal reads
    // during release ramps.
    const bool allowHorizontal = !counterFrozen || state.gameBrickLatchedKey == 0x00;
    if (!allowHorizontal) {
      state.gameBrickCenterFrames = 0;
      return frame;
    }

    const int dx = static_cast<int>(data[3]) - 0x98;
    constexpr int kDeadzoneRight = 2;
    constexpr int kDeadzoneLeft = 0;

    if (dx < -kDeadzoneRight) {
      frame.keycode = KEYBOARD_RIGHT_ARROW;
      frame.reportIndex = 3;
      frame.pressed = activeFrame;
      state.gameBrickCenterFrames = 0;
      return frame;
    }
    if (dx > kDeadzoneLeft) {
      frame.keycode = KEYBOARD_LEFT_ARROW;
      frame.reportIndex = 3;
      frame.pressed = activeFrame;
      state.gameBrickCenterFrames = 0;
      return frame;
    }
    if (activeFrame && !counterFrozen && state.gameBrickLatchedKey == 0x00) {
      // Centered burst with cycling counter: some units emit weak-LEFT this way.
      // Require several frames before committing so noise can't trigger a fake LEFT.
      if (state.gameBrickCenterFrames < 255) {
        state.gameBrickCenterFrames++;
      }
      if (state.gameBrickCenterFrames >= 6) {
        frame.keycode = KEYBOARD_LEFT_ARROW;
        frame.reportIndex = 3;
        frame.pressed = activeFrame;
      }
      return frame;
    }
    state.gameBrickCenterFrames = 0;
    return frame;
  }

  // Any other byte[4] value is ramp overshoot; treat as no-key.
  state.gameBrickCenterFrames = 0;
  return frame;
}

// Free2 rolling keycode families (observed empirically across firmwares).
// While one physical button is held, the device cycles through a small set
// of keycodes that all mean "this direction". We collapse the family to one
// logical keycode (the first one seen) so the mapping layer only needs one
// entry per direction.
bool isFree2Forward(uint8_t code) { return code == 0x1C || code == 0xC4 || code == 0x6C || code == 0xBC; }
bool isFree2Back(uint8_t code) { return code == 0xB4 || code == 0x0E || code == 0x66 || code == 0x16; }

DecodedFrame decodeFree2Rolling(const uint8_t* data, size_t length, State& /*state*/) {
  DecodedFrame frame;
  if (!data || length == 0) {
    return frame;
  }

  // Free2 reports the keycode at byte[2] in keyboard mode. Scan a wider
  // window because some firmwares emit on byte[1] under consumer-page mode.
  const size_t scanLen = length < 8 ? length : 8;
  for (size_t i = 0; i < scanLen; i++) {
    const uint8_t b = data[i];
    if (b == 0 || b == 0xFF) continue;
    const bool fwd = isFree2Forward(b);
    const bool back = isFree2Back(b);
    if (!fwd && !back && b < 0x04) continue;  // skip modifier/zero bytes

    // Collapse each rolling family to a single canonical keycode so one
    // learned mapping covers the whole hold. Any other (non-rolling) button
    // passes through raw so the user can still learn it.
    if (fwd) {
      frame.keycode = FREE2_FORWARD;
    } else if (back) {
      frame.keycode = FREE2_BACK;
    } else {
      frame.keycode = b;
    }
    frame.reportIndex = static_cast<uint8_t>(i);
    frame.pressed = true;
    return frame;
  }

  return frame;  // No active key — clean release frame (pressed=false, keycode=0).
}

}  // namespace

DecodedFrame decode(Kind kind, const uint8_t* data, size_t length, State& state, const ReportHints& hints) {
  switch (kind) {
    case Kind::GameBrickV2:
      return decodeGameBrickV2(data, length, state);
    case Kind::Free2Rolling:
      return decodeFree2Rolling(data, length, state);
    case Kind::GenericExtract:
    default:
      return decodeGenericExtract(data, length, hints);
  }
}

unsigned long staleReleaseTimeoutMs(Kind kind, bool inReaderContext) {
  switch (kind) {
    case Kind::Free2Rolling:
      return inReaderContext ? 500UL : 250UL;
    case Kind::GameBrickV2:
      return 1200UL;
    case Kind::GenericExtract:
    default:
      return 0UL;  // 0 = trust device release frames; no synthetic timeout.
  }
}

namespace {

bool macPrefixMatches(const char* mac, const char* prefix) {
  if (!mac || !prefix) return false;
  for (size_t j = 0; prefix[j] != '\0' && mac[j] != '\0'; j++) {
    char a = mac[j], b = prefix[j];
    if (a >= 'A' && a <= 'F') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'F') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

bool nameContains(const char* name, const char* needle) { return name && strstr(name, needle) != nullptr; }

}  // namespace

ProfileMatch matchProfile(const char* macAddress, const char* deviceName) {
  ProfileMatch match;  // defaults to GenericExtract / byte[2]

  // IINE Game Brick V2 ships from a known MAC prefix.
  if (macPrefixMatches(macAddress, "60:4d:ec")) {
    match.kind = Kind::GameBrickV2;
    match.reportByteIndex = 4;
    match.name = "IINE Game Brick V2";
    return match;
  }

  if (deviceName) {
    const bool isGameBrick =
        (nameContains(deviceName, "Game") || nameContains(deviceName, "game") || nameContains(deviceName, "GAME")) &&
        (nameContains(deviceName, "Brick") || nameContains(deviceName, "brick") || nameContains(deviceName, "BRICK"));
    if (isGameBrick || nameContains(deviceName, "IINE") || nameContains(deviceName, "iine")) {
      match.kind = Kind::GameBrickV2;
      match.reportByteIndex = 4;
      match.name = "IINE Game Brick";
      return match;
    }

    const bool isFree2 = nameContains(deviceName, "Free2") || nameContains(deviceName, "FREE2") ||
                         nameContains(deviceName, "free2") || nameContains(deviceName, "Free 2") ||
                         nameContains(deviceName, "FREE 2") || nameContains(deviceName, "Free-2");
    const bool isFree3 = nameContains(deviceName, "Free3") || nameContains(deviceName, "FREE3") ||
                         nameContains(deviceName, "free3") || nameContains(deviceName, "Free 3") ||
                         nameContains(deviceName, "FREE 3") || nameContains(deviceName, "Free-3");
    if (isFree2 || isFree3) {
      match.kind = Kind::Free2Rolling;
      match.reportByteIndex = 2;
      match.name = isFree3 ? "Free3-M" : "Free2-M";
      return match;
    }
  }

  return match;
}

}  // namespace BleDecoders
