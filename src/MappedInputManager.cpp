#include "MappedInputManager.h"

#include "CrossPointSettings.h"

namespace {
using ButtonIndex = uint8_t;

// Sentinel: the logical button has no physical mapping (e.g. side-button paging
// disabled). mapButton()/getHeldTime() treat it as "not pressed".
constexpr ButtonIndex NO_BUTTON = 0xFF;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches the first entries of CrossPointSettings::SIDE_BUTTON_LAYOUT
// (PREV_NEXT, NEXT_PREV). SIDE_BUTTONS_DISABLED has no entry and maps to NO_BUTTON.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};
constexpr size_t kSideLayoutCount = sizeof(kSideLayouts) / sizeof(kSideLayouts[0]);
}  // namespace

uint8_t MappedInputManager::getPhysicalButtonIndex(const Button button) const {
  const uint8_t sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      return SETTINGS.frontButtonBack;
    case Button::Confirm:
      return SETTINGS.frontButtonConfirm;
    case Button::Left:
      return SETTINGS.frontButtonLeft;
    case Button::Right:
      return SETTINGS.frontButtonRight;
    case Button::Up:
      return HalGPIO::BTN_UP;
    case Button::Down:
      return HalGPIO::BTN_DOWN;
    case Button::Power:
      return HalGPIO::BTN_POWER;
    case Button::PageBack:
      // Side-button paging can be swapped or disabled via settings; an out-of-range
      // (e.g. SIDE_BUTTONS_DISABLED) layout maps to NO_BUTTON so paging does nothing.
      return sideLayout < kSideLayoutCount ? kSideLayouts[sideLayout].pageBack : NO_BUTTON;
    case Button::PageForward:
      return sideLayout < kSideLayoutCount ? kSideLayouts[sideLayout].pageForward : NO_BUTTON;
  }

  return HalGPIO::BTN_BACK;
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const uint8_t idx = getPhysicalButtonIndex(button);
  return idx == NO_BUTTON ? false : (gpio.*fn)(idx);
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

unsigned long MappedInputManager::getHeldTime(const Button button) const {
  const uint8_t idx = getPhysicalButtonIndex(button);
  return idx == NO_BUTTON ? 0UL : gpio.getHeldTime(idx);
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return leftLabel;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return rightLabel;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
