#pragma once

#include <array>
#include <cstdint>
#include <string>

// Per-device BLE keycode -> logical action mappings, persisted on SD.
// The user's mapping (what each keycode should do) lives here as data;
// the decoder profile (how to extract a keycode from a HID report) lives
// in code in BleDecoders. There are no hardcoded mappings — an unmapped
// keycode is a deliberate no-op.
namespace BleMappings {

enum class Action : uint8_t {
  None = 0,
  PageBack = 1,     // logical "previous page" — injects BTN_UP
  PageForward = 2,  // logical "next page" — injects BTN_DOWN
  Confirm = 3,      // injects SETTINGS.frontButtonConfirm
  Back = 4,         // injects SETTINGS.frontButtonBack
  Left = 5,         // injects BTN_LEFT
  Right = 6,        // injects BTN_RIGHT
};

constexpr uint8_t MAX_MAPPINGS_PER_DEVICE = 6;

struct Entry {
  uint8_t keycode = 0;  // 0 = empty slot, never a valid HID keycode
  Action action = Action::None;
};

struct Set {
  std::array<Entry, MAX_MAPPINGS_PER_DEVICE> entries{};
  uint8_t count = 0;

  Action lookup(uint8_t keycode) const;
  bool isEmpty() const { return count == 0; }
};

// Return the mapping set for the given MAC, or nullptr if the device has
// no learned mapping. Pointer is valid until the next save/remove/clearAll
// for the same address.
const Set* get(const std::string& macAddress);

// Replace the mapping set for a device. Persists to SD.
void save(const std::string& macAddress, const Set& set);

// Remove a device's mapping. Persists to SD.
void remove(const std::string& macAddress);

// Drop every learned mapping. Persists to SD.
void clearAll();

// Translate an Action into the HalGPIO button index the injector expects.
// Confirm/Back route through SETTINGS.frontButton* so the user's local
// remapping is respected. Returns 0xFF for Action::None.
uint8_t actionToButtonIndex(Action action);

}  // namespace BleMappings
