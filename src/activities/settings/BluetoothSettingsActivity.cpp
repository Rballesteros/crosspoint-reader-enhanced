#include "BluetoothSettingsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

#include "BleMappings.h"
#include "CrossPointSettings.h"
#include "HalGPIO.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr uint32_t kSettingsScanDurationMs = 30000;

// Compact label for the BLE Appearance characteristic. Falls back to "" if
// the value isn't one we recognize.
const char* appearanceShortLabel(uint16_t appearance) {
  switch (appearance) {
    case 0x03C1:
      return "Kbd";
    case 0x03C2:
      return "Mouse";
    case 0x03C3:
      return "Joystick";
    case 0x03C4:
      return "Gamepad";
    case 0x03C5:
      return "Tablet";
    case 0x03C6:
      return "Card";
    case 0x03C7:
      return "Pen";
    case 0x03C8:
      return "Scanner";
    default:
      if (appearance >= 0x03C0 && appearance <= 0x03FF) return "HID";
      if (appearance >= 0x0040 && appearance <= 0x007F) return "Phone";
      if (appearance >= 0x00C0 && appearance <= 0x00FF) return "Watch";
      if (appearance >= 0x0140 && appearance <= 0x017F) return "Tag";
      if (appearance >= 0x0080 && appearance <= 0x00BF) return "Computer";
      return "";
  }
}

// Bluetooth SIG company ID → short name. Only the most common consumer ones.
const char* companyShortLabel(uint16_t companyId) {
  switch (companyId) {
    case 0x004C:
      return "Apple";
    case 0x0006:
      return "MS";
    case 0x0075:
      return "Samsung";
    case 0x00E0:
      return "Google";
    case 0x000B:
      return "Logitech";
    case 0x038F:
      return "Xiaomi";
    case 0x0157:
      return "Mi";
    case 0x0499:
      return "Ruuvi";
    default:
      return "";
  }
}

bool labelLooksUnknown(const std::string& name) {
  if (name.empty()) return true;
  if (name == "Unknown") return true;
  if (name.size() >= 2 && name[0] == '?' && name[1] == '-') return true;
  return false;
}

std::string localizedBluetoothError(const std::string& error) {
  if (error == BluetoothHIDManager::ERROR_REMOTE_SLEEP_RETRY) {
    return tr(STR_BLE_REMOTE_SLEEP_RETRY);
  }
  if (error == BluetoothHIDManager::ERROR_CONNECTION_TIMEOUT) {
    return tr(STR_ERROR_CONNECTION_TIMEOUT);
  }
  if (error == BluetoothHIDManager::ERROR_CONNECTION_FAILED || error.empty()) {
    return tr(STR_CONNECTION_FAILED);
  }
  return error;
}

const char* mappedButtonDebugLabel(uint8_t buttonIndex) {
  if (buttonIndex == SETTINGS.frontButtonConfirm) {
    return "Confirm";
  }
  if (buttonIndex == SETTINGS.frontButtonBack) {
    return "Back";
  }
  if (buttonIndex == SETTINGS.frontButtonLeft) {
    return "Left";
  }
  if (buttonIndex == SETTINGS.frontButtonRight) {
    return "Right";
  }
  switch (buttonIndex) {
    case HalGPIO::BTN_UP:
      return "Up/PageBack";
    case HalGPIO::BTN_DOWN:
      return "Down/PageForward";
    case HalGPIO::BTN_LEFT:
      return "Left";
    case HalGPIO::BTN_RIGHT:
      return "Right";
    case HalGPIO::BTN_CONFIRM:
      return "Confirm";
    case HalGPIO::BTN_BACK:
      return "Back";
    case HalGPIO::BTN_POWER:
      return "Power";
    default:
      return "Unmapped";
  }
}

}  // namespace

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  viewMode = ViewMode::MAIN_MENU;
  lastError = "";
  lastScanTime = 0;
  pendingLearnKey = 0;
  pendingLearnIndex = 0xFF;
  learnedPrevKey = 0;
  learnedNextKey = 0;
  learnedConfirmKey = 0;
  learnedCancelKey = 0;
  learnedLeftKey = 0;
  learnedRightKey = 0;
  learnTestDeadlineMs = 0;
  learnTestForwardSeen = false;
  learnTestBackSeen = false;
  learnTestForwardCount = 0;
  learnTestBackCount = 0;
  debugLastKeycode = 0;
  debugLastReportIndex = 0xFF;
  debugLastMappedButton = 0xFF;
  debugLastPressed = false;
  debugLastRawLength = 0;
  debugEventCount = 0;
  debugRenderedEventCount = 0;
  debugLastEventMs = 0;
  debugUniqueCount = 0;
  memset(debugLastRaw, 0, sizeof(debugLastRaw));
  memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
  memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));
  learnStep = LearnStep::WAIT_PREV;

  // Get BLE manager instance
  btMgr = &BluetoothHIDManager::getInstance();
  LOG_INF("BT", "BluetoothHIDManager ready");

  // Restore Bluetooth persistent state on entry. Skip the restore (without
  // clobbering bluetoothEnabled) when BT memory was released at boot — that
  // pref controls auto-restore after a future reboot, not this session.
  if (SETTINGS.bluetoothEnabled && !btMgr->isEnabled() && BluetoothHIDManager::isAvailableAtBoot()) {
    LOG_INF("BT", "Restoring Bluetooth from settings (enabled)");
    if (btMgr->enable()) {
      lastError = "Bluetooth restored";
    } else {
      lastError = "Failed to restore BT";
      SETTINGS.bluetoothEnabled = 0;
    }
  } else if (!SETTINGS.bluetoothEnabled && btMgr->isEnabled()) {
    LOG_INF("BT", "Disabling Bluetooth per settings (disabled)");
    btMgr->disable();
    lastError = "Bluetooth disabled per settings";
  }

  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  if (btMgr) {
    if (btMgr->isScanning()) {
      btMgr->stopScan();
    }
    btMgr->setLearnInputCallback(nullptr, nullptr);
    btMgr->setInputCallback(nullptr, nullptr);
    btMgr->setDebugInputCallback(nullptr, nullptr);
  }
  Activity::onExit();
}

void BluetoothSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (viewMode == ViewMode::DEVICE_LIST) {
      // Return to main menu
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      if (btMgr && btMgr->isScanning()) {
        btMgr->stopScan();
      }
      requestUpdate();
      return;
    } else if (viewMode == ViewMode::LEARN_KEYS) {
      if (btMgr) {
        btMgr->setLearnInputCallback(nullptr, nullptr);
      }
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      if (learnStep != LearnStep::DONE) {
        lastError = "Learn mode canceled";
      }
      requestUpdate();
      return;
    } else if (viewMode == ViewMode::DEBUG_MONITOR) {
      if (btMgr) {
        btMgr->setInputCallback(nullptr, nullptr);
        btMgr->setDebugInputCallback(nullptr, nullptr);
      }
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      requestUpdate();
      return;
    } else {
      if (onComplete) onComplete();
      return;
    }
  }

  // Check if scan completed or needs live update
  if (btMgr && viewMode == ViewMode::DEVICE_LIST) {
    if (!btMgr->isScanning() && lastScanTime > 0) {
      if (millis() - lastScanTime > 500) {  // Small delay to see final results
        lastScanTime = 0;
        requestUpdate();
      }
    } else if (btMgr->isScanning()) {
      unsigned long now = millis();
      // Throttle UI refreshes to ~5Hz during scan
      if (now - lastDeviceListRefresh > 200) {
        lastDeviceListRefresh = now;
        requestUpdate();
      }
    }
  }

  if (viewMode == ViewMode::MAIN_MENU) {
    handleMainMenuInput();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    handleDeviceListInput();
  } else if (viewMode == ViewMode::DEBUG_MONITOR) {
    handleDebugInput();
  } else {
    handleLearnInput();
  }
}

void BluetoothSettingsActivity::handleMainMenuInput() {
  // Note: kAvailableAtBootIndex is appended after the existing items so the
  // existing index constants don't shift. Increment kMainMenuItemCount by 1
  // and place the toggle at index (kMainMenuItemCount - 1).
  constexpr int kMainMenuItemCount =
#ifdef ENABLE_BT_DEBUG_MONITOR
      9;
#else
      8;
#endif

  constexpr int kToggleBluetoothIndex = 0;
  constexpr int kReconnectBondedIndex = 1;
  constexpr int kDisconnectDevicesIndex = 2;
  constexpr int kScanForDevicesIndex = 3;
  constexpr int kRemoteSetupWizardIndex = 4;
#ifdef ENABLE_BT_DEBUG_MONITOR
  constexpr int kDebugMonitorIndex = 5;
  constexpr int kClearLearnedKeysIndex = 6;
  constexpr int kForgetBondedRemoteIndex = 7;
  constexpr int kAvailableAtBootIndex = 8;
#else
  constexpr int kClearLearnedKeysIndex = 5;
  constexpr int kForgetBondedRemoteIndex = 6;
  constexpr int kAvailableAtBootIndex = 7;
#endif

  buttonNavigator.onPrevious([this, kMainMenuItemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, kMainMenuItemCount);
    requestUpdate();
  });

  buttonNavigator.onNext([this, kMainMenuItemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, kMainMenuItemCount);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!btMgr) {
      lastError = "BLE not available";
      LOG_ERR("BT", "BLE manager not available");
      requestUpdate();
      return;
    }

    if (selectedIndex == kToggleBluetoothIndex) {
      // Toggle Bluetooth
      if (btMgr->isEnabled()) {
        LOG_INF("BT", "Disabling Bluetooth...");
        if (btMgr->disable()) {
          lastError = "Bluetooth disabled";
          SETTINGS.bluetoothEnabled = 0;
          SETTINGS.saveToFile();
        } else {
          lastError = "Failed to disable";
        }
      } else {
        LOG_INF("BT", "Enabling Bluetooth...");
        if (btMgr->enable()) {
          lastError = "Bluetooth enabled";
          SETTINGS.bluetoothEnabled = 1;
          SETTINGS.saveToFile();
        } else {
          lastError = btMgr->lastError.empty() ? "Failed to enable" : btMgr->lastError;
        }
      }
      requestUpdate();
    } else if (selectedIndex == kReconnectBondedIndex) {
      if (!btMgr->isEnabled()) {
        lastError = "Enable BT first";
      } else if (SETTINGS.bleBondedDeviceAddr[0] == '\0') {
        lastError = "No bonded remote saved";
      } else if (btMgr->isConnected(SETTINGS.bleBondedDeviceAddr)) {
        lastError = "Bonded remote already connected";
      } else {
        LOG_INF("BT", "Reconnecting to bonded remote %s (%s)", SETTINGS.bleBondedDeviceName,
                SETTINGS.bleBondedDeviceAddr);
        lastError = "Reconnecting...";
        requestUpdate();

        if (btMgr->connectToDevice(SETTINGS.bleBondedDeviceAddr)) {
          lastError = std::string("Reconnected to ") +
                      (SETTINGS.bleBondedDeviceName[0] ? SETTINGS.bleBondedDeviceName : "bonded remote");
        } else {
          lastError = localizedBluetoothError(btMgr->lastError);
        }
      }
      requestUpdate();
    } else if (selectedIndex == kDisconnectDevicesIndex) {
      if (!btMgr->isEnabled()) {
        lastError = "Enable BT first";
      } else {
        const auto connectedDevices = btMgr->getConnectedDevicesCopy();
        if (connectedDevices.empty()) {
          lastError = "No devices connected";
        } else {
          for (const auto& dev : connectedDevices) {
            btMgr->disconnectFromDevice(dev.address);
          }
          lastError = "Disconnected";
        }
      }
      requestUpdate();
    } else if (selectedIndex == kScanForDevicesIndex) {
      // Start scan and switch to device list
      if (btMgr->isEnabled()) {
        showOnlyHID = false;
        btMgr->startScan(kSettingsScanDurationMs);
        lastScanTime = millis();
        viewMode = ViewMode::DEVICE_LIST;
        selectedIndex = 0;
        lastError = "";
      } else {
        lastError = "Enable BT first";
      }
      requestUpdate();
    } else if (selectedIndex == kRemoteSetupWizardIndex) {
      if (!btMgr->isEnabled()) {
        lastError = "Enable BT first";
      } else if (btMgr->getConnectedDevicesCopy().empty()) {
        lastError = "Connect a remote first";
      } else {
        viewMode = ViewMode::LEARN_KEYS;
        learnStep = LearnStep::WAIT_PREV;
        pendingLearnKey = 0;
        pendingLearnIndex = 0xFF;
        learnedPrevKey = 0;
        learnedNextKey = 0;
        learnedConfirmKey = 0;
        learnedCancelKey = 0;
        learnedLeftKey = 0;
        learnedRightKey = 0;
        learnTestDeadlineMs = 0;
        learnTestForwardSeen = false;
        learnTestBackSeen = false;
        learnTestForwardCount = 0;
        learnTestBackCount = 0;
        btMgr->setLearnInputCallback(&BluetoothSettingsActivity::onLearnInputTrampoline, this);
        lastError = "Wizard: press FORWARD button";
      }
      requestUpdate();
    }
#ifdef ENABLE_BT_DEBUG_MONITOR
    else if (selectedIndex == kDebugMonitorIndex) {
      if (!btMgr->isDebugCaptureEnabled()) {
        btMgr->setDebugCaptureEnabled(true);
      }
      debugLastKeycode = 0;
      debugLastReportIndex = 0xFF;
      debugLastMappedButton = 0xFF;
      debugLastPressed = false;
      debugLastRawLength = 0;
      debugEventCount = 0;
      debugRenderedEventCount = 0;
      debugLastEventMs = 0;
      debugUniqueCount = 0;
      memset(debugLastRaw, 0, sizeof(debugLastRaw));
      memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
      memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));
      btMgr->setInputCallback(nullptr, nullptr);
      btMgr->setDebugInputCallback(&BluetoothSettingsActivity::onDebugDecodedInputTrampoline, this);
      viewMode = ViewMode::DEBUG_MONITOR;
      lastError = "BT debug monitor";
      requestUpdate();
    }
#endif
    else if (selectedIndex == kClearLearnedKeysIndex) {
      BleMappings::clearAll();
      if (btMgr) {
        btMgr->refreshDeviceMappings();
      }
      lastError = "Learned mapping cleared";
      requestUpdate();
    } else if (selectedIndex == kForgetBondedRemoteIndex) {
      SETTINGS.bleBondedDeviceAddr[0] = '\0';
      SETTINGS.bleBondedDeviceName[0] = '\0';
      SETTINGS.bleBondedDeviceAddrType = 0;
      SETTINGS.saveToFile();
      btMgr->setBondedDevice("", "");

      // Also wipe the NimBLE-side bond entries from NVS. Stale IRK/encryption
      // keys here cause "failed to configure restored IRK" at boot and lead
      // to ATT-queue wakeup floods when a previously-bonded peer reconnects.
      // deleteAllBonds() requires NimBLE to be initialized.
      int bondsCleared = 0;
      if (btMgr->isEnabled()) {
        bondsCleared = NimBLEDevice::getNumBonds();
        NimBLEDevice::deleteAllBonds();
      }
      char buf[64];
      snprintf(buf, sizeof(buf), "Bonded remote cleared (%d NVS bond%s)", bondsCleared, bondsCleared == 1 ? "" : "s");
      lastError = buf;
      requestUpdate();
    } else if (selectedIndex == kAvailableAtBootIndex) {
      const bool currentlyReserved = BluetoothHIDManager::isAvailableAtBoot();
      const bool nextValue = !currentlyReserved;
      const std::string heading = nextValue ? "Enable BT at next boot?" : "Disable BT at next boot?";
      const std::string body = nextValue
                                   ? "Reserves ~30-40KB DRAM for Bluetooth. Reboot now to take effect."
                                   : "Releases ~30-40KB DRAM. Reboot now; BT will be unavailable until re-enabled.";
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                             [this, nextValue](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 requestUpdate();
                                 return;
                               }
                               if (!BluetoothHIDManager::setAvailableAtBoot(nextValue)) {
                                 lastError = "Failed to save BT boot pref";
                                 requestUpdate();
                                 return;
                               }
                               LOG_INF("BT", "BT boot pref updated to %d, restarting", nextValue ? 1 : 0);
                               delay(500);
                               ESP.restart();
                             });
    }
  }
}

void BluetoothSettingsActivity::handleLearnInput() {
  // Helper used by several branches to jump into the test step.
  auto enterTestStep = [&]() {
    learnStep = LearnStep::WAIT_TEST;
    learnTestDeadlineMs = millis() + 15000;
    learnTestForwardSeen = false;
    learnTestBackSeen = false;
    learnTestForwardCount = 0;
    learnTestBackCount = 0;
  };

  if (pendingLearnKey != 0) {
    const uint8_t capturedKey = pendingLearnKey;
    const uint8_t capturedIndex = pendingLearnIndex;
    pendingLearnKey = 0;
    pendingLearnIndex = 0xFF;
    (void)capturedIndex;

    // Any already-captured key re-pressed at an optional step means "skip this
    // step". Two-button clickers use this to skip Confirm/Cancel/Left/Right.
    auto isAlreadyLearnedKey = [&](uint8_t k) {
      return k == learnedNextKey || k == learnedPrevKey || (learnedConfirmKey != 0 && k == learnedConfirmKey) ||
             (learnedCancelKey != 0 && k == learnedCancelKey) || (learnedLeftKey != 0 && k == learnedLeftKey) ||
             (learnedRightKey != 0 && k == learnedRightKey);
    };

    if (learnStep == LearnStep::WAIT_PREV) {
      learnedNextKey = capturedKey;
      learnStep = LearnStep::WAIT_NEXT;
      char buf[96];
      snprintf(buf, sizeof(buf), "Forward=0x%02X captured", learnedNextKey);
      lastError = buf;
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_NEXT) {
      if (capturedKey == learnedNextKey) {
        lastError = "Back must be a different button";
        requestUpdate();
        return;
      }
      learnedPrevKey = capturedKey;
      learnStep = LearnStep::WAIT_CONFIRM;
      char buf[96];
      snprintf(buf, sizeof(buf), "Back=0x%02X captured", learnedPrevKey);
      lastError = buf;
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_CONFIRM) {
      if (isAlreadyLearnedKey(capturedKey)) {
        learnedConfirmKey = 0;
        lastError = "Confirm skipped";
      } else {
        learnedConfirmKey = capturedKey;
        char buf[96];
        snprintf(buf, sizeof(buf), "Confirm=0x%02X captured", learnedConfirmKey);
        lastError = buf;
      }
      learnStep = LearnStep::WAIT_CANCEL;
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_CANCEL) {
      if (isAlreadyLearnedKey(capturedKey)) {
        learnedCancelKey = 0;
        lastError = "Cancel skipped — now test your buttons";
      } else {
        learnedCancelKey = capturedKey;
        char buf[96];
        snprintf(buf, sizeof(buf), "Cancel=0x%02X — now test your buttons", learnedCancelKey);
        lastError = buf;
      }
      // After Cancel, jump directly to test. Left/Right are advanced and
      // only entered when the user explicitly presses physical Left at the
      // test step.
      enterTestStep();
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_LEFT) {
      if (isAlreadyLearnedKey(capturedKey)) {
        learnedLeftKey = 0;
        lastError = "Left skipped";
      } else {
        learnedLeftKey = capturedKey;
        char buf[96];
        snprintf(buf, sizeof(buf), "Left=0x%02X captured", learnedLeftKey);
        lastError = buf;
      }
      learnStep = LearnStep::WAIT_RIGHT;
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_RIGHT) {
      if (isAlreadyLearnedKey(capturedKey)) {
        learnedRightKey = 0;
        lastError = "Right skipped — back to test";
      } else {
        learnedRightKey = capturedKey;
        char buf[96];
        snprintf(buf, sizeof(buf), "Right=0x%02X — back to test", learnedRightKey);
        lastError = buf;
      }
      enterTestStep();
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_TEST) {
      if (capturedKey == learnedNextKey) {
        learnTestForwardSeen = true;
        if (learnTestForwardCount < 65535) learnTestForwardCount++;
      } else if (capturedKey == learnedPrevKey) {
        learnTestBackSeen = true;
        if (learnTestBackCount < 65535) learnTestBackCount++;
      }
      learnTestDeadlineMs = millis() + 15000;  // refresh idle timeout
      // Counts are shown in a dedicated zone by the renderer; don't duplicate
      // them into the status line.
      if (learnTestForwardSeen && learnTestBackSeen) {
        lastError = "Both keys verified — press Confirm to save";
      }
      requestUpdate();
      return;
    }
  }

  // Physical LEFT at the test step opens the advanced flow for mapping the
  // remote's Left/Right keys (e.g. GameBrick joystick). Most 2-button
  // clickers never need this — it's deliberately hidden from the main flow.
  if (learnStep == LearnStep::WAIT_TEST && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    learnStep = LearnStep::WAIT_LEFT;
    learnTestDeadlineMs = 0;  // disable timeout while in advanced
    lastError = "Advanced: press LEFT on remote (or Confirm to skip)";
    requestUpdate();
    return;
  }

  // Physical CONFIRM:
  //   - on optional steps: skip this step
  //   - on advanced steps: skip and continue
  //   - at the test step: save (handled separately below)
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (learnStep == LearnStep::WAIT_CONFIRM) {
      learnedConfirmKey = 0;
      learnStep = LearnStep::WAIT_CANCEL;
      lastError = "Confirm skipped";
      requestUpdate();
      return;
    }
    if (learnStep == LearnStep::WAIT_CANCEL) {
      learnedCancelKey = 0;
      enterTestStep();
      lastError = "Cancel skipped — now test your buttons";
      requestUpdate();
      return;
    }
    if (learnStep == LearnStep::WAIT_LEFT) {
      learnedLeftKey = 0;
      learnStep = LearnStep::WAIT_RIGHT;
      lastError = "Left skipped";
      requestUpdate();
      return;
    }
    if (learnStep == LearnStep::WAIT_RIGHT) {
      learnedRightKey = 0;
      enterTestStep();
      lastError = "Right skipped — back to test";
      requestUpdate();
      return;
    }
  }

  if (learnStep == LearnStep::WAIT_TEST && millis() > learnTestDeadlineMs) {
    if (btMgr) {
      btMgr->setLearnInputCallback(nullptr, nullptr);
    }
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    lastError = "Wizard timed out (not saved)";
    requestUpdate();
    return;
  }

  if (learnStep == LearnStep::WAIT_TEST && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Build the user's mapping from whatever they captured. Skipped steps
    // simply don't produce an entry — unmapped keycodes are no-ops at runtime.
    BleMappings::Set wizardMappings;
    auto appendMapping = [&wizardMappings](uint8_t keycode, BleMappings::Action action) {
      if (keycode == 0 || action == BleMappings::Action::None) return;
      if (wizardMappings.count >= BleMappings::MAX_MAPPINGS_PER_DEVICE) return;
      wizardMappings.entries[wizardMappings.count++] = {keycode, action};
    };
    appendMapping(learnedNextKey, BleMappings::Action::PageForward);
    appendMapping(learnedPrevKey, BleMappings::Action::PageBack);
    appendMapping(learnedConfirmKey, BleMappings::Action::Confirm);
    appendMapping(learnedCancelKey, BleMappings::Action::Back);
    appendMapping(learnedLeftKey, BleMappings::Action::Left);
    appendMapping(learnedRightKey, BleMappings::Action::Right);

    if (btMgr) {
      const auto connected = btMgr->getConnectedDevicesCopy();
      for (const auto& dev : connected) {
        BleMappings::save(dev.address, wizardMappings);
      }
      btMgr->refreshDeviceMappings();
      btMgr->setLearnInputCallback(nullptr, nullptr);
    }
    learnStep = LearnStep::DONE;
    char buf[96];
    snprintf(buf, sizeof(buf), "Saved! Fwd=0x%02X Back=0x%02X Conf=0x%02X Cncl=0x%02X", learnedNextKey, learnedPrevKey,
             learnedConfirmKey, learnedCancelKey);
    lastError = buf;

    // On successful wizard completion, return immediately to menu (or back to book).
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    if (exitOnSuccessfulConnect) {
      if (onComplete) onComplete();
      return;
    }

    requestUpdate();
    return;
  }

  if (learnStep == LearnStep::DONE && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (btMgr) {
      btMgr->setLearnInputCallback(nullptr, nullptr);
    }
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    requestUpdate();
  }
}

void BluetoothSettingsActivity::handleDeviceListInput() {
  if (!btMgr) return;

  const auto devices = btMgr->getDiscoveredDevicesCopy();
  const auto connectedDevices = btMgr->getConnectedDevicesCopy();

  // Build the user-visible projection (apply HID-only filter if active).
  std::vector<const BluetoothDevice*> visible;
  visible.reserve(devices.size());
  for (const auto& d : devices) {
    if (showOnlyHID && !d.isHID) continue;
    visible.push_back(&d);
  }

  // Action-row indices live after the device rows.
  const int filterIdx = static_cast<int>(visible.size());
  const int rescanIdx = filterIdx + 1;
  const int disconnectIdx = (!connectedDevices.empty()) ? rescanIdx + 1 : -1;
  const int maxIndex = (disconnectIdx >= 0 ? disconnectIdx : rescanIdx);

  // If a previous re-sort moved the highlighted device, follow it. Look up by
  // address rather than by index so the cursor stays "on" the same device.
  if (!highlightedAddress.empty()) {
    bool matched = false;
    for (size_t i = 0; i < visible.size(); i++) {
      if (visible[i]->address == highlightedAddress) {
        selectedIndex = static_cast<int>(i);
        matched = true;
        break;
      }
    }
    if (!matched) {
      // Highlighted device disappeared (filtered out, scan dropped it, etc.)
      highlightedAddress.clear();
    }
  }

  // Clamp in case the list shrank below selectedIndex.
  if (selectedIndex > maxIndex) selectedIndex = maxIndex;
  if (selectedIndex < 0) selectedIndex = 0;

  auto syncHighlightedDevice = [this, &visible] {
    if (selectedIndex < static_cast<int>(visible.size())) {
      highlightedAddress = visible[selectedIndex]->address;
    } else {
      highlightedAddress.clear();
    }
  };

  buttonNavigator.onPrevious([this, maxIndex, &syncHighlightedDevice] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, maxIndex + 1);
    syncHighlightedDevice();
    requestUpdate();
  });

  buttonNavigator.onNext([this, maxIndex, &syncHighlightedDevice] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, maxIndex + 1);
    syncHighlightedDevice();
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == filterIdx) {
      // Toggle HID-only filter
      showOnlyHID = !showOnlyHID;
      lastError = showOnlyHID ? "Filter: HID only" : "Filter: All devices";
      // The visible list will change next tick — clear highlight to avoid jumping.
      highlightedAddress.clear();
      selectedIndex = 0;
      requestUpdate();
      return;
    }

    if (selectedIndex == rescanIdx) {
      LOG_INF("BT", "Refreshing scan...");
      lastError = "Scanning...";
      showOnlyHID = false;
      btMgr->startScan(kSettingsScanDurationMs);
      lastScanTime = millis();
      requestUpdate();
      return;
    }

    if (disconnectIdx >= 0 && selectedIndex == disconnectIdx) {
      LOG_INF("BT", "Disconnecting from all devices...");
      for (const auto& device : connectedDevices) {
        LOG_DBG("BT", "Disconnecting from %s", device.address.c_str());
        btMgr->disconnectFromDevice(device.address);
      }
      lastError = "Disconnected";
      selectedIndex = 0;
      requestUpdate();
      return;
    }

    // Otherwise: connect to highlighted device
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(visible.size())) {
      const auto& device = *visible[selectedIndex];

      LOG_INF("BT", "Connecting to %s (%s)", device.name.c_str(), device.address.c_str());
      lastError = "Connecting...";
      requestUpdate();

      if (btMgr->connectToDevice(device.address)) {
        std::string bondedName = device.name;
        const auto connectedAfterConnect = btMgr->getConnectedDevicesCopy();
        for (const auto& connected : connectedAfterConnect) {
          if (connected.address == device.address && !labelLooksUnknown(connected.name)) {
            bondedName = connected.name;
            break;
          }
        }

        strncpy(SETTINGS.bleBondedDeviceAddr, device.address.c_str(), sizeof(SETTINGS.bleBondedDeviceAddr) - 1);
        SETTINGS.bleBondedDeviceAddr[sizeof(SETTINGS.bleBondedDeviceAddr) - 1] = '\0';
        strncpy(SETTINGS.bleBondedDeviceName, bondedName.c_str(), sizeof(SETTINGS.bleBondedDeviceName) - 1);
        SETTINGS.bleBondedDeviceName[sizeof(SETTINGS.bleBondedDeviceName) - 1] = '\0';
        SETTINGS.bleBondedDeviceAddrType = device.addrType;
        SETTINGS.saveToFile();
        btMgr->setBondedDevice(device.address, bondedName, device.addrType);

        lastError = std::string("Connected: ") + bondedName;
        LOG_INF("BT", "Successfully connected to %s", bondedName.c_str());
        if (exitOnSuccessfulConnect) {
          if (onComplete) onComplete();
          return;
        }
      } else {
        lastError = localizedBluetoothError(btMgr->lastError);
        LOG_ERR("BT", "Failed to connect: %s", lastError.c_str());
      }
      requestUpdate();
    }
  }
}

void BluetoothSettingsActivity::render(RenderLock&&) {
  if (viewMode == ViewMode::MAIN_MENU) {
    renderMainMenu();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    renderDeviceList();
  } else if (viewMode == ViewMode::DEBUG_MONITOR) {
    renderDebugMonitor();
  } else {
    renderLearnKeys();
  }
}

void BluetoothSettingsActivity::handleDebugInput() {
  if (!btMgr) {
    return;
  }

  if (debugRenderedEventCount != debugEventCount) {
    debugRenderedEventCount = debugEventCount;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const bool next = !btMgr->isDebugCaptureEnabled();
    btMgr->setDebugCaptureEnabled(next);
    lastError = next ? "BT debug capture: ON" : "BT debug capture: OFF";
    requestUpdate();
    return;
  }
}

void BluetoothSettingsActivity::renderMainMenu() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Header with Bluetooth title
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH));

  // Status subheader
  std::string statusLine;
  if (btMgr) {
    if (btMgr->isEnabled()) {
      auto connDevices = btMgr->getConnectedDevicesCopy();
      if (!connDevices.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Enabled, %zu device(s) connected", connDevices.size());
        statusLine = buf;
      } else {
        statusLine = "Enabled, no devices connected";
      }
    } else {
      statusLine = "Disabled";
    }
  } else {
    statusLine = "Error initializing Bluetooth";
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    statusLine.c_str());

  int listOffsetY = 0;
  if (btMgr && btMgr->isEnabled() && SETTINGS.bleBondedDeviceName[0] != '\0') {
    const int nameY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 4;
    std::string deviceLine = std::string("Remote: ") + SETTINGS.bleBondedDeviceName;
    deviceLine = renderer.truncatedText(UI_10_FONT_ID, deviceLine.c_str(), pageWidth - metrics.contentSidePadding * 2);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, nameY, deviceLine.c_str(), true);
    listOffsetY = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  }

  // Use GUI.drawList for consistent formatting with main settings.
  // Keep this list in sync with the index constants in handleMainMenuInput().
  const char* items[] = {
      btMgr && btMgr->isEnabled() ? "Disable Bluetooth" : "Enable Bluetooth",
      "Reconnect Bonded Remote",
      "Disconnect Device(s)",
      "Scan for Devices",
      "Remote Setup Wizard",
#ifdef ENABLE_BT_DEBUG_MONITOR
      btMgr && btMgr->isDebugCaptureEnabled() ? "Disable BT Debug Capture" : "Enable BT Debug Capture",
#endif
      "Clear Learned Keys",
      "Forget Bonded Remote",
      "Bluetooth at Boot"};

  const bool btAvailableAtBoot = BluetoothHIDManager::isAvailableAtBoot();
  const int itemCount = static_cast<int>(sizeof(items) / sizeof(items[0]));
  const int availableAtBootIdx = itemCount - 1;

  std::vector<std::string> itemLabels;
  itemLabels.reserve(itemCount);
  for (int i = 0; i < itemCount; i++) {
    itemLabels.push_back(items[i]);
  }

  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing + listOffsetY,
           pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2 + listOffsetY)},
      static_cast<int>(itemLabels.size()), selectedIndex, [&itemLabels](int index) { return itemLabels[index]; },
      nullptr, nullptr,
      [this, availableAtBootIdx, btAvailableAtBoot](int i) {
        if (i == 0) {
          return std::string(btMgr && btMgr->isEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
        }
        if (i == 1 && SETTINGS.bleBondedDeviceName[0] != '\0') {
          return renderer.truncatedText(
              UI_10_FONT_ID, SETTINGS.bleBondedDeviceName,
              renderer.getScreenWidth() - UITheme::getInstance().getMetrics().contentSidePadding * 4);
        }
        if (i == availableAtBootIdx) {
          return std::string(btAvailableAtBoot ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
        }
        return std::string("");
      },
      true);

  if (!lastError.empty()) {
    std::string statusText =
        renderer.truncatedText(UI_10_FONT_ID, lastError.c_str(), pageWidth - metrics.contentSidePadding * 2);
    const int statusY =
        pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, statusY, statusText.c_str(), true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDeviceList() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (!btMgr) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Bluetooth error");
    return;
  }

  const auto devices = btMgr->getDiscoveredDevicesCopy();
  const auto connectedDevices = btMgr->getConnectedDevicesCopy();

  // Apply HID-only filter (same projection used by handleDeviceListInput).
  std::vector<const BluetoothDevice*> visible;
  visible.reserve(devices.size());
  for (const auto& d : devices) {
    if (showOnlyHID && !d.isHID) continue;
    // We must push the address of the local copy's element. Since `devices` is
    // a local copy inside this function, its memory remains stable for the
    // duration of this function call, making it safe to take pointers.
    visible.push_back(&d);
  }

  // Header — show filtered count vs. total when filter is active.
  char countStr[40];
  if (btMgr->isScanning()) {
    snprintf(countStr, sizeof(countStr), tr(STR_SCANNING));
  } else if (showOnlyHID && visible.size() != devices.size()) {
    snprintf(countStr, sizeof(countStr), "HID %zu/%zu", visible.size(), devices.size());
  } else {
    snprintf(countStr, sizeof(countStr), "Found %zu", devices.size());
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH), countStr);

  // Subheader with scan status
  std::string subheaderText;
  if (btMgr->isScanning()) {
    char sBuf[48];
    snprintf(sBuf, sizeof(sBuf), "Searching... %zu found", visible.size());
    subheaderText = sBuf;
  } else if (visible.empty()) {
    subheaderText = devices.empty() ? "No devices found" : "No matches (filter on)";
  } else {
    char sBuf[64];
    snprintf(sBuf, sizeof(sBuf), "%zu device(s) available", visible.size());
    subheaderText = sBuf;
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    subheaderText.c_str());

  // Build device list labels for the visible projection.
  std::vector<std::string> deviceLabels;
  std::vector<std::string> deviceValues;
  deviceLabels.reserve(visible.size() + 3);
  deviceValues.reserve(visible.size() + 3);
  char buf[128];

  auto isDeviceConnected = [&connectedDevices](const std::string& address) {
    return std::find_if(connectedDevices.begin(), connectedDevices.end(), [&address](const ConnectedDevice& device) {
             return device.address == address;
           }) != connectedDevices.end();
  };

  for (const auto* dp : visible) {
    const auto& device = *dp;
    const bool connected = isDeviceConnected(device.address);

    // Device name with indicators
    const char* connSymbol = connected ? "[*] " : "";
    const char* hidSymbol = device.isHID ? "[HID] " : "";
    snprintf(buf, sizeof(buf), "%s%s%s", connSymbol, hidSymbol, device.name.c_str());
    deviceLabels.push_back(buf);

    // RSSI/signal strength + identification hints (appearance / manufacturer)
    const std::string signalBars = getSignalStrengthIndicator(device.rssi);
    const char* appLabel = appearanceShortLabel(device.appearance);
    const char* mfgLabel = companyShortLabel(device.companyId);
    char hintBuf[32];
    hintBuf[0] = '\0';
    if (appLabel[0] && mfgLabel[0]) {
      snprintf(hintBuf, sizeof(hintBuf), " %s/%s", appLabel, mfgLabel);
    } else if (appLabel[0]) {
      snprintf(hintBuf, sizeof(hintBuf), " %s", appLabel);
    } else if (mfgLabel[0]) {
      snprintf(hintBuf, sizeof(hintBuf), " %s", mfgLabel);
    }
    snprintf(buf, sizeof(buf), "%s (%d dBm)%s", signalBars.c_str(), device.rssi, hintBuf);
    deviceValues.push_back(buf);
  }

  // Action rows — order must match handleDeviceListInput indices.
  deviceLabels.push_back(showOnlyHID ? "< Show all devices >" : "< Show HID only >");
  deviceValues.push_back("");

  deviceLabels.push_back("< Rescan >");
  deviceValues.push_back("");

  if (!connectedDevices.empty()) {
    deviceLabels.push_back("< Disconnect All >");
    deviceValues.push_back("");
  }

  // Render the list using GUI.drawList for consistency
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      deviceLabels.size(), selectedIndex, [&deviceLabels](int index) { return deviceLabels[index]; }, nullptr, nullptr,
      [&deviceValues](int i) { return i < (int)deviceValues.size() ? deviceValues[i] : std::string(""); }, true);

  // Help text keeps navigation consistent with the rest of settings. Rescan,
  // filter, and disconnect are selectable action rows in the list.
  GUI.drawHelpText(renderer,
                   Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
                   "Up/Down: Scroll | Select: Connect");

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string BluetoothSettingsActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // BLE RSSI tends to be lower than WiFi at similar distance.
  // Use BLE-friendly thresholds so nearby remotes are not shown as always weak.
  if (rssi >= -60) {
    return "||||";  // Excellent
  }
  if (rssi >= -70) {
    return " |||";  // Good
  }
  if (rssi >= -80) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void BluetoothSettingsActivity::renderLearnKeys() {
  // TODO(i18n): every user-facing string in this function (and most of this
  // file) is hardcoded English. Match the rest of the codebase by routing
  // through tr() / new STR_* keys once the dedicated i18n pass lands.
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Remote Setup Wizard");

  // Step indicator shown in the subheader. Numbered for required steps, named
  // for optional/advanced/test so the user sees how far along they are.
  const char* stepText = "Step 1 of 2 — required";
  if (learnStep == LearnStep::WAIT_NEXT) {
    stepText = "Step 2 of 2 — required";
  } else if (learnStep == LearnStep::WAIT_CONFIRM) {
    stepText = "Optional — Confirm";
  } else if (learnStep == LearnStep::WAIT_CANCEL) {
    stepText = "Optional — Cancel";
  } else if (learnStep == LearnStep::WAIT_LEFT) {
    stepText = "Advanced — Left";
  } else if (learnStep == LearnStep::WAIT_RIGHT) {
    stepText = "Advanced — Right";
  } else if (learnStep == LearnStep::WAIT_TEST) {
    stepText = "Test your buttons";
  } else if (learnStep == LearnStep::DONE) {
    stepText = "Saved";
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    stepText);

  // Big headline label naming the action being mapped.
  const char* headline = "PAGE FORWARD";
  if (learnStep == LearnStep::WAIT_NEXT)
    headline = "PAGE BACK";
  else if (learnStep == LearnStep::WAIT_CONFIRM)
    headline = "CONFIRM / SELECT";
  else if (learnStep == LearnStep::WAIT_CANCEL)
    headline = "CANCEL / BACK";
  else if (learnStep == LearnStep::WAIT_LEFT)
    headline = "LEFT";
  else if (learnStep == LearnStep::WAIT_RIGHT)
    headline = "RIGHT";
  else if (learnStep == LearnStep::WAIT_TEST)
    headline = "TEST FORWARD / BACK";

  // Single-line hint describing the action. Empty for the test/done screens.
  const char* description = "";
  switch (learnStep) {
    case LearnStep::WAIT_PREV:
      description = "Turns pages forward, scrolls lists down";
      break;
    case LearnStep::WAIT_NEXT:
      description = "Turns pages backward, scrolls lists up";
      break;
    case LearnStep::WAIT_CONFIRM:
      description = "Selects items, answers dialogs Yes";
      break;
    case LearnStep::WAIT_CANCEL:
      description = "Exits screens, answers dialogs No";
      break;
    case LearnStep::WAIT_LEFT:
      description = "Cursor left, fine-grained adjustments";
      break;
    case LearnStep::WAIT_RIGHT:
      description = "Cursor right, fine-grained adjustments";
      break;
    case LearnStep::WAIT_TEST:
      description = "Press Forward/Back on the remote to confirm they work";
      break;
    default:
      break;
  }

  // Layout is split into fixed zones so each one breathes independently of
  // what the others contain. All coordinates are baselines for drawCenteredText.
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight;
  // 24px clearance above the button-hints strip so descenders don't clip in.
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - 24;
  const int sidePad = metrics.contentSidePadding;
  const int rowX = sidePad + 32;  // Left-aligned column for progress rows.

  const bool isCaptureStep =
      (learnStep == LearnStep::WAIT_PREV || learnStep == LearnStep::WAIT_NEXT || learnStep == LearnStep::WAIT_CONFIRM ||
       learnStep == LearnStep::WAIT_CANCEL || learnStep == LearnStep::WAIT_LEFT || learnStep == LearnStep::WAIT_RIGHT);

  // ----- Zone 1: instruction + headline + description -----
  int y = contentTop + 24;
  if (isCaptureStep) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Press the button on your remote for:");
    y += 36;
    // Headline uses NotoSerif 14 (always loaded, larger than UI_12) so the
    // user sees at a glance which action they're mapping.
    renderer.drawCenteredText(NOTOSERIF_14_FONT_ID, y, headline);
    y += 32;
  } else {
    renderer.drawCenteredText(NOTOSERIF_14_FONT_ID, y + 8, headline);
    y += 48;
  }
  if (description[0]) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, description);
    y += 28;
  }

  // ----- Zone 2: progress list -----
  // Each row 24px apart. Required rows are always visible; advanced rows only
  // when the user has opted into them or captured them.
  const bool showLeft = (learnStep == LearnStep::WAIT_LEFT) || (learnStep == LearnStep::WAIT_RIGHT) || learnedLeftKey;
  const bool showRight = (learnStep == LearnStep::WAIT_RIGHT) || learnedRightKey;

  y += 8;  // small gap before list

  auto drawRow = [&](int yy, const char* label, uint8_t key, bool current, bool optional) {
    char left[24];
    char right[40];
    snprintf(left, sizeof(left), "%s", label);
    if (key) {
      snprintf(right, sizeof(right), "captured (0x%02X)", key);
    } else if (current) {
      snprintf(right, sizeof(right), "← press now");
    } else if (optional) {
      snprintf(right, sizeof(right), "skip allowed");
    } else {
      snprintf(right, sizeof(right), "pending");
    }
    renderer.drawText(UI_10_FONT_ID, rowX, yy, left);
    // Right side aligned to a column ~half the screen across.
    renderer.drawText(UI_10_FONT_ID, rowX + 110, yy, right);
  };

  drawRow(y, "Forward", learnedNextKey, learnStep == LearnStep::WAIT_PREV, false);
  y += 24;
  drawRow(y, "Back", learnedPrevKey, learnStep == LearnStep::WAIT_NEXT, false);
  y += 24;
  drawRow(y, "Confirm", learnedConfirmKey, learnStep == LearnStep::WAIT_CONFIRM, true);
  y += 24;
  drawRow(y, "Cancel", learnedCancelKey, learnStep == LearnStep::WAIT_CANCEL, true);
  y += 24;
  if (showLeft) {
    drawRow(y, "Left", learnedLeftKey, learnStep == LearnStep::WAIT_LEFT, true);
    y += 24;
  }
  if (showRight) {
    drawRow(y, "Right", learnedRightKey, learnStep == LearnStep::WAIT_RIGHT, true);
    y += 24;
  }

  // ----- Zone 3 (bottom-anchored): physical-button shortcuts + status -----
  // Anchor these to contentBottom so they don't shift when the progress list
  // grows. Three lines of hint text + one line of status.
  int hintY = contentBottom - 22 * 3;
  renderer.drawCenteredText(UI_10_FONT_ID, hintY, "─── Physical buttons ───");
  hintY += 22;

  if (learnStep == LearnStep::WAIT_PREV || learnStep == LearnStep::WAIT_NEXT) {
    renderer.drawCenteredText(UI_10_FONT_ID, hintY, "Back = cancel wizard");
  } else if (learnStep == LearnStep::WAIT_CONFIRM || learnStep == LearnStep::WAIT_CANCEL) {
    renderer.drawCenteredText(UI_10_FONT_ID, hintY, "Confirm = skip this step   ·   Back = cancel wizard");
  } else if (learnStep == LearnStep::WAIT_LEFT || learnStep == LearnStep::WAIT_RIGHT) {
    renderer.drawCenteredText(UI_10_FONT_ID, hintY, "Confirm = skip   ·   Back = cancel wizard");
  } else if (learnStep == LearnStep::WAIT_TEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, hintY, "Confirm = save and exit   ·   Back = cancel wizard");
    hintY += 22;
    renderer.drawCenteredText(UI_10_FONT_ID, hintY, "Left = add advanced Left/Right keys");
  }

  // Test-step telemetry replaces the description-row content with live press
  // counts and the idle timeout. Use a fixed slot so it doesn't shift.
  if (learnStep == LearnStep::WAIT_TEST && learnTestDeadlineMs != 0) {
    char countLine[64];
    snprintf(countLine, sizeof(countLine), "Forward presses: %u    Back presses: %u",
             static_cast<unsigned>(learnTestForwardCount), static_cast<unsigned>(learnTestBackCount));
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 96, countLine);
    char timerLine[40];
    const unsigned int remaining = (learnTestDeadlineMs > millis()) ? (learnTestDeadlineMs - millis()) / 1000 : 0;
    snprintf(timerLine, sizeof(timerLine), "Idle timeout: %us", remaining);
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 118, timerLine);
  }

  // Status line: latest message, always at the very bottom of the content
  // area with real margin above the button-hints strip.
  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentBottom, lastError.c_str());
  }

  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), (learnStep == LearnStep::DONE || learnStep == LearnStep::WAIT_TEST) ? tr(STR_SELECT) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDebugMonitor() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Bluetooth Debug");

  std::string sub = btMgr && btMgr->isDebugCaptureEnabled() ? "Capture ON" : "Capture OFF";
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    sub.c_str());

  char line1[64];
  char line2[64];
  char line3[64];
  char line4[64];
  char line5[64];
  char line6[64];
  char line7[64];

  unsigned int connectedCount = btMgr ? static_cast<unsigned int>(btMgr->getConnectedDevicesCopy().size()) : 0;
  snprintf(line1, sizeof(line1), "Connected: %u", connectedCount);
  snprintf(line2, sizeof(line2), "Decoded reports: %u", static_cast<unsigned>(debugEventCount));
  snprintf(line3, sizeof(line3), "Unique keys: %u", static_cast<unsigned>(debugUniqueCount));
  snprintf(line4, sizeof(line4), "Last key: 0x%02X", static_cast<unsigned>(debugLastKeycode & 0xFF));
  if (debugLastReportIndex == 0xFF) {
    snprintf(line5, sizeof(line5), "Byte: --  State: %s", debugLastPressed ? "down" : "up");
  } else {
    snprintf(line5, sizeof(line5), "Byte: %u  State: %s", static_cast<unsigned>(debugLastReportIndex),
             debugLastPressed ? "down" : "up");
  }
  snprintf(line6, sizeof(line6), "Mapped: %s", mappedButtonDebugLabel(debugLastMappedButton));
  if (debugLastRawLength == 0) {
    snprintf(line7, sizeof(line7), "Raw: --");
  } else {
    int pos = snprintf(line7, sizeof(line7), "Raw:");
    for (uint8_t i = 0; i < debugLastRawLength && pos > 0 && pos < static_cast<int>(sizeof(line7)) - 4; i++) {
      pos += snprintf(line7 + pos, sizeof(line7) - pos, " %02X", static_cast<unsigned>(debugLastRaw[i]));
    }
  }

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 24,
                            line1);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 48,
                            line2);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 72,
                            line3);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 96,
                            line4);
  renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 116,
                            line5);
  renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 134,
                            line6);
  renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 152,
                            line7);

  if (debugLastEventMs > 0) {
    char eventAgeLine[64];
    snprintf(eventAgeLine, sizeof(eventAgeLine), "Last event: %lus ago", (millis() - debugLastEventMs) / 1000);
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 170,
                              eventAgeLine);
  }

  const int uniqueStartY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 188;
  if (debugUniqueCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY, "No key presses captured yet");
  } else {
    uint8_t sortedIndices[kDebugUniqueKeyMax] = {0};
    for (uint8_t i = 0; i < debugUniqueCount; i++) {
      sortedIndices[i] = i;
    }

    for (uint8_t i = 0; i + 1 < debugUniqueCount; i++) {
      uint8_t best = i;
      for (uint8_t j = i + 1; j < debugUniqueCount; j++) {
        const uint16_t bestCount = debugUniqueCounts[sortedIndices[best]];
        const uint16_t candidateCount = debugUniqueCounts[sortedIndices[j]];
        if (candidateCount > bestCount) {
          best = j;
        }
      }
      if (best != i) {
        const uint8_t tmp = sortedIndices[i];
        sortedIndices[i] = sortedIndices[best];
        sortedIndices[best] = tmp;
      }
    }

    const uint8_t renderCount = (debugUniqueCount < 4) ? debugUniqueCount : 4;
    for (uint8_t i = 0; i < renderCount; i++) {
      const uint8_t idx = sortedIndices[i];
      char keyLine[64];
      snprintf(keyLine, sizeof(keyLine), "Key 0x%02X  x%u", static_cast<unsigned>(debugUniqueKeys[idx]),
               static_cast<unsigned>(debugUniqueCounts[idx]));
      renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY + static_cast<int>(i) * 16, keyLine);
    }

    if (debugUniqueCount > renderCount) {
      char moreLine[48];
      snprintf(moreLine, sizeof(moreLine), "+%u more keys", static_cast<unsigned>(debugUniqueCount - renderCount));
      renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY + static_cast<int>(renderCount) * 16, moreLine);
    }
  }

  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 16, lastError.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BluetoothSettingsActivity::onLearnInputTrampoline(void* ctx, uint8_t keycode, uint8_t reportIndex) {
  auto* self = static_cast<BluetoothSettingsActivity*>(ctx);
  if (self->viewMode == ViewMode::LEARN_KEYS && keycode > 0 && reportIndex != 0xFF) {
    self->pendingLearnKey = keycode;
    self->pendingLearnIndex = reportIndex;
  }
}

void BluetoothSettingsActivity::onDebugDecodedInputTrampoline(void* ctx, uint8_t keycode, uint8_t reportIndex,
                                                              uint8_t mappedButton, bool pressed, const uint8_t* raw,
                                                              uint8_t rawLength) {
  auto* self = static_cast<BluetoothSettingsActivity*>(ctx);
  self->debugLastKeycode = keycode;
  self->debugLastReportIndex = reportIndex;
  self->debugLastMappedButton = mappedButton;
  self->debugLastPressed = pressed;
  self->debugLastRawLength = rawLength > sizeof(self->debugLastRaw) ? sizeof(self->debugLastRaw) : rawLength;
  if (raw && self->debugLastRawLength > 0) {
    memcpy(self->debugLastRaw, raw, self->debugLastRawLength);
  }
  if (self->debugLastRawLength < sizeof(self->debugLastRaw)) {
    memset(self->debugLastRaw + self->debugLastRawLength, 0, sizeof(self->debugLastRaw) - self->debugLastRawLength);
  }

  self->debugEventCount++;
  self->debugLastEventMs = millis();

  const uint8_t code = keycode;
  if (!pressed || code == 0x00 || code == 0xFF) {
    return;
  }

  for (uint8_t i = 0; i < self->debugUniqueCount; i++) {
    if (self->debugUniqueKeys[i] == code) {
      if (self->debugUniqueCounts[i] < 65535) self->debugUniqueCounts[i]++;
      return;
    }
  }
  if (self->debugUniqueCount < kDebugUniqueKeyMax) {
    self->debugUniqueKeys[self->debugUniqueCount] = code;
    self->debugUniqueCounts[self->debugUniqueCount] = 1;
    self->debugUniqueCount++;
  }
}
