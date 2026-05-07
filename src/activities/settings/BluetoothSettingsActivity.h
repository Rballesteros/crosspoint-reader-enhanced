#pragma once

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <string>

#include "activities/Activity.h"
#include "MappedInputManager.h"
#include "util/ButtonNavigator.h"

class BluetoothSettingsActivity : public Activity {
 private:
  enum class ViewMode {
    MAIN_MENU,
    DEVICE_LIST,
    LEARN_KEYS,
    DEBUG_MONITOR
  };

  enum class LearnStep {
    WAIT_PREV,
    WAIT_NEXT,
    WAIT_CONFIRM,  // Optional 3rd button: BTN_CONFIRM action
    WAIT_CANCEL,   // Optional 4th button: BTN_BACK action
    WAIT_TEST,
    DONE
  };

  ViewMode viewMode = ViewMode::MAIN_MENU;
  int selectedIndex = 0;
  BluetoothHIDManager* btMgr = nullptr;
  std::string lastError = "";
  unsigned long lastScanTime = 0;
  LearnStep learnStep = LearnStep::WAIT_PREV;
  uint8_t pendingLearnKey = 0;
  uint8_t pendingLearnIndex = 0xFF;
  uint8_t learnedPrevKey = 0;
  uint8_t learnedNextKey = 0;
  uint8_t learnedConfirmKey = 0;  // 0 = not learned (skipped or never set)
  uint8_t learnedCancelKey = 0;   // 0 = not learned
  uint8_t learnedReportIndex = 2;
  unsigned long learnTestDeadlineMs = 0;
  bool learnTestForwardSeen = false;
  bool learnTestBackSeen = false;
  uint16_t learnTestForwardCount = 0;
  uint16_t learnTestBackCount = 0;
  uint16_t debugLastKeycode = 0;
  uint8_t debugLastReportIndex = 0xFF;
  uint8_t debugLastMappedButton = 0xFF;
  bool debugLastPressed = false;
  uint8_t debugLastRaw[8] = {0};
  uint8_t debugLastRawLength = 0;
  uint32_t debugEventCount = 0;
  uint32_t debugRenderedEventCount = 0;
  unsigned long debugLastEventMs = 0;
  static constexpr uint8_t kDebugUniqueKeyMax = 8;
  uint8_t debugUniqueKeys[kDebugUniqueKeyMax] = {0};
  uint16_t debugUniqueCounts[kDebugUniqueKeyMax] = {0};
  uint8_t debugUniqueCount = 0;
  bool exitOnSuccessfulConnect = false;

  // Device-list view state
  std::string highlightedAddress;            // BLE address the cursor is "on" (so sort doesn't lose it)
  bool showOnlyHID = false;                  // Filter list to HID-advertising devices only
  unsigned long lastDeviceListRefresh = 0;   // millis() of last live re-render during scan
  ButtonNavigator buttonNavigator;

 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& onComplete,
                                     const bool exitOnSuccessfulConnect = false)
      : Activity("BluetoothSettings", renderer, mappedInput),
        exitOnSuccessfulConnect(exitOnSuccessfulConnect),
        onComplete(onComplete) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleMainMenuInput();
  void handleDeviceListInput();
  void handleLearnInput();
  void handleDebugInput();
  void renderMainMenu();
  void renderDeviceList();
  void renderLearnKeys();
  void renderDebugMonitor();
  std::string getSignalStrengthIndicator(const int32_t rssi) const;

  // Trampolines for BluetoothHIDManager raw fn-ptr callbacks (avoids
  // std::function heap/binary cost). Cast ctx back to instance pointer.
  static void onLearnInputTrampoline(void* ctx, uint8_t keycode, uint8_t reportIndex);
  static void onDebugDecodedInputTrampoline(void* ctx, uint8_t keycode, uint8_t reportIndex, uint8_t mappedButton,
                                            bool pressed, const uint8_t* raw, uint8_t rawLength);

  const std::function<void()> onComplete;
};
