#pragma once

#include <cstddef>
#include <cstdint>

// Decoder layer for BLE HID reports.
//
// "Decoding" is the step that turns a raw HID report frame into a stable
// (keycode, pressed) pair. It must stay device-specific because some remotes
// (GameBrick joystick, Free2 rolling-keycode families) do not expose standard
// HID semantics — without per-device parsing the report bytes are noise.
//
// "Mapping" — what each keycode should do — lives in BleMappings (data) and
// is owned entirely by the user, never hardcoded here.
namespace BleDecoders {

enum class Kind : uint8_t {
  GenericExtract,  // Standard HID keyboard/consumer — pick the first non-zero key.
  GameBrickV2,     // IINE Game Brick: byte[4] + joystick + frozen-counter framing.
  Free2Rolling,    // Free2 / Free3 rolling keycode family.
};

struct DecodedFrame {
  uint8_t keycode = 0;         // Canonical keycode for mapping; 0 = no key / release.
  bool pressed = false;        // True while held.
  uint8_t reportIndex = 0xFF;  // Byte offset the keycode came from (for the learn wizard).
};

// Hints derived from the device's HID Report Map descriptor. Used by
// GenericExtract to bias the byte scan toward the layout the device actually
// advertised.
struct ReportHints {
  bool hasKeyboardPage = false;
  bool hasConsumerPage = false;
  uint8_t preferredByteIndex = 0xFF;  // 0xFF = no hint, scan as usual.
};

// Per-device, persistent decoder state. Owned by ConnectedDevice; passed in
// on every decode call. Decoders mutate the fields they own and ignore the
// rest. A union would save ~6 bytes; not worth the destructor/aliasing care.
struct State {
  // GameBrick
  uint16_t gameBrickLastCounter = 0xFFFF;
  uint8_t gameBrickLatchedKey = 0x00;
  uint8_t gameBrickCenterFrames = 0;

  // Free2 / Free3 rolling decoders are stateless — each frame's keycode is
  // collapsed to a canonical direction id, so no per-device latch is needed.
};

// Parse a single HID report. Returns the decoded frame; updates `state` in
// place. The caller compares `frame.pressed` and `state` snapshots across
// calls to detect press/release transitions.
DecodedFrame decode(Kind kind, const uint8_t* data, size_t length, State& state, const ReportHints& hints);

// Synthetic keycodes for the two physical action buttons on GameBrick.
// These don't appear on the wire — the GameBrick decoder fabricates them so
// the mapping layer can address A/B by a stable id.
constexpr uint8_t GAMEBRICK_ACTION_A = 0xF1;
constexpr uint8_t GAMEBRICK_ACTION_B = 0xF2;

// Synthetic keycodes for the Free2/Free3 rolling families. Each physical
// direction cycles through several wire keycodes; the decoder collapses the
// whole family to one of these stable ids so the user only learns one mapping
// per direction (MAX_MAPPINGS_PER_DEVICE can't hold every rolling code).
constexpr uint8_t FREE2_FORWARD = 0xF3;
constexpr uint8_t FREE2_BACK = 0xF4;

// Maximum idle time before the orchestrator should synthesize a release for
// a still-held button. 0 means "trust the device's release frame".
//   GenericExtract: 0   (devices send clean release frames)
//   GameBrickV2:    1200 (fallback for missed releases)
//   Free2Rolling:   250 idle / 500 reader (Free2 frequently drops release frames)
unsigned long staleReleaseTimeoutMs(Kind kind, bool inReaderContext);

// Choose a decoder kind for a known/unknown remote at connect time. Falls
// back to GenericExtract if no profile is matched.
struct ProfileMatch {
  Kind kind = Kind::GenericExtract;
  uint8_t reportByteIndex = 2;  // Default HID keyboard offset.
  const char* name = "Generic";
};

ProfileMatch matchProfile(const char* macAddress, const char* deviceName);

}  // namespace BleDecoders
