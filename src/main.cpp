#include <Arduino.h>
#include <BluetoothHIDManager.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static volatile bool gBluetoothReaderContext = false;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

EpdFont opendyslexic8RegularFont(&opendyslexic_8_regular);
EpdFont opendyslexic8BoldFont(&opendyslexic_8_bold);
EpdFont opendyslexic8ItalicFont(&opendyslexic_8_italic);
EpdFont opendyslexic8BoldItalicFont(&opendyslexic_8_bolditalic);
EpdFontFamily opendyslexic8FontFamily(&opendyslexic8RegularFont, &opendyslexic8BoldFont, &opendyslexic8ItalicFont,
                                      &opendyslexic8BoldItalicFont);
EpdFont opendyslexic10RegularFont(&opendyslexic_10_regular);
EpdFont opendyslexic10BoldFont(&opendyslexic_10_bold);
EpdFont opendyslexic10ItalicFont(&opendyslexic_10_italic);
EpdFont opendyslexic10BoldItalicFont(&opendyslexic_10_bolditalic);
EpdFontFamily opendyslexic10FontFamily(&opendyslexic10RegularFont, &opendyslexic10BoldFont, &opendyslexic10ItalicFont,
                                       &opendyslexic10BoldItalicFont);
EpdFont opendyslexic12RegularFont(&opendyslexic_12_regular);
EpdFont opendyslexic12BoldFont(&opendyslexic_12_bold);
EpdFont opendyslexic12ItalicFont(&opendyslexic_12_italic);
EpdFont opendyslexic12BoldItalicFont(&opendyslexic_12_bolditalic);
EpdFontFamily opendyslexic12FontFamily(&opendyslexic12RegularFont, &opendyslexic12BoldFont, &opendyslexic12ItalicFont,
                                       &opendyslexic12BoldItalicFont);
EpdFont opendyslexic14RegularFont(&opendyslexic_14_regular);
EpdFont opendyslexic14BoldFont(&opendyslexic_14_bold);
EpdFont opendyslexic14ItalicFont(&opendyslexic_14_italic);
EpdFont opendyslexic14BoldItalicFont(&opendyslexic_14_bolditalic);
EpdFontFamily opendyslexic14FontFamily(&opendyslexic14RegularFont, &opendyslexic14BoldFont, &opendyslexic14ItalicFont,
                                       &opendyslexic14BoldItalicFont);
#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  APP_STATE.saveToFile();

  auto& btMgr = BluetoothHIDManager::getInstance();
  if (btMgr.isEnabled()) {
    LOG_INF("SLP", "Disabling Bluetooth before deep sleep");
    btMgr.disable();
  }

  activityManager.goToSleep();

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void ensureSdFontLoaded() { sdFontSystem.ensureLoaded(renderer); }

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
  renderer.insertFont(OPENDYSLEXIC_8_FONT_ID, opendyslexic8FontFamily);
  renderer.insertFont(OPENDYSLEXIC_10_FONT_ID, opendyslexic10FontFamily);
  renderer.insertFont(OPENDYSLEXIC_12_FONT_ID, opendyslexic12FontFamily);
  renderer.insertFont(OPENDYSLEXIC_14_FONT_ID, opendyslexic14FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  HalSystem::begin();
  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();

#ifdef ENABLE_SERIAL_LOG
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    const unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
  }
#endif

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  auto& btMgr = BluetoothHIDManager::getInstance();
  btMgr.setButtonInjector(
      [](void*, uint8_t buttonIndex, bool pressed) { gpio.setVirtualButtonState(buttonIndex, pressed); }, nullptr);
  btMgr.setButtonActivityNotifier(
      [](void*, uint8_t buttonIndex) { gpio.updateVirtualButtonActivity(buttonIndex); }, nullptr);
  btMgr.setReaderContextCallback([](void*) { return gBluetoothReaderContext; }, nullptr);
  btMgr.setBondedDevice(SETTINGS.bleBondedDeviceAddr, SETTINGS.bleBondedDeviceName,
                        SETTINGS.bleBondedDeviceAddrType);
  LOG_INF("MAIN", "Bluetooth HID initialized with button injection");

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  setupDisplayAndFonts();

  activityManager.goToBoot();

  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;
  static bool lastBluetoothStatusVisible = BaseTheme::showBluetoothStatusIndicator();
  static bool lastBluetoothStatusConnected = BaseTheme::isBluetoothStatusConnected();

  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  const bool userInputDetected = gpio.wasAnyPressed() || gpio.wasAnyReleased();
  bool bleRecentActivity = false;

  gBluetoothReaderContext = activityManager.isReaderActivity();
  auto& btMgr = BluetoothHIDManager::getInstance();
  // Global auto-release: if BT has been enabled but disconnected/idle for several seconds,
  // release the radio to free NimBLE's ~50-80KB heap. This prevents the radio stack
  // from pinning the heap low during navigation or heavy UI work.
  // Wake-on-input or manual settings toggle will bring it back.
  static unsigned long btDisconnectedSinceMs = 0;
  if (btMgr.isEnabled() && !btMgr.hasConnectedDevice() && !btMgr.isScanning()) {
    const unsigned long now = millis();
    if (btDisconnectedSinceMs == 0) {
      btDisconnectedSinceMs = now;
    } else if (now - btDisconnectedSinceMs > 10000) {
      LOG_INF("BT", "BT idle >10s; releasing radio to free heap");
      btMgr.disable();
      btDisconnectedSinceMs = 0;
    }
  } else {
    btDisconnectedSinceMs = 0;
  }
  // Skip auto-wake/reconnect polling when neither settings nor the runtime stack are active.
  // We never auto-disable here — other paths (settings activity, debug serial commands,
  // wizards) may enable BT without flipping SETTINGS, and tearing them down would break them.
  if (SETTINGS.bluetoothEnabled || btMgr.isEnabled()) {
    if (SETTINGS.bluetoothEnabled && gBluetoothReaderContext && userInputDetected && !btMgr.isEnabled() &&
        SETTINGS.bleBondedDeviceAddr[0] != '\0') {
      LOG_INF("BT", "Local input detected; waking Bluetooth for bonded-device reconnect");
      btMgr.enable();
    }
    btMgr.updateActivity();
    btMgr.checkAutoReconnect(userInputDetected);
    bleRecentActivity = btMgr.hasRecentActivity();
  }
  const bool bluetoothStatusVisible = BaseTheme::showBluetoothStatusIndicator();
  const bool bluetoothStatusConnected = BaseTheme::isBluetoothStatusConnected();
  if (bluetoothStatusVisible != lastBluetoothStatusVisible ||
      bluetoothStatusConnected != lastBluetoothStatusConnected) {
    activityManager.requestUpdate();
    lastBluetoothStatusVisible = bluetoothStatusVisible;
    lastBluetoothStatusConnected = bluetoothStatusConnected;
  }

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    const auto uiStack = uxTaskGetStackHighWaterMark(nullptr);
    const auto renderStack = uxTaskGetStackHighWaterMark(activityManager.getRenderTaskHandle());
    const size_t maxAlloc = ESP.getMaxAllocHeap();

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);

    LOG_INF("MEM", "Free: %d, MaxAlloc: %d, Blocks: %d | Stack: UI:%u, Render:%u",
            static_cast<int>(info.total_free_bytes),
            static_cast<int>(info.largest_free_block),
            static_cast<int>(info.free_blocks),
            static_cast<unsigned>(uiStack), static_cast<unsigned>(renderStack));

    lastMemPrint = millis();
    recordHeapSample();

    // Emergency fragmentation mitigation: if the largest free block is dangerously small,
    // flush the font cache to consolidate memory holes.
    if (maxAlloc < 8192) {
      LOG_INF("MEM", "Fragmentation emergency: flushing font cache");
      fontCacheManager.clearCache();
    }
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      } else if (cmd == "BTDEBUG:ON") {
        btMgr.setDebugCaptureEnabled(true);
        logSerial.printf("BTDEBUG:ON\n");
      } else if (cmd == "BTDEBUG:OFF") {
        btMgr.setDebugCaptureEnabled(false);
        logSerial.printf("BTDEBUG:OFF\n");
      } else if (cmd.startsWith("BTSCAN")) {
        uint32_t scanMs = 30000;
        const int sep = cmd.indexOf(':');
        if (sep >= 0) {
          const uint32_t parsedMs = static_cast<uint32_t>(cmd.substring(sep + 1).toInt());
          if (parsedMs > 0) {
            scanMs = parsedMs;
          }
        }
        if (!btMgr.isEnabled()) {
          btMgr.enable();
        }
        btMgr.startScan(scanMs);
        logSerial.printf("BTSCAN:STARTED:%lu\n", static_cast<unsigned long>(scanMs));
      } else if (cmd.startsWith("BTCONNECT:")) {
        String payload = cmd.substring(10);
        payload.trim();

        bool hasAddrType = false;
        uint8_t addrType = 0;
        const int comma = payload.lastIndexOf(',');
        if (comma > 0) {
          const String typeStr = payload.substring(comma + 1);
          payload = payload.substring(0, comma);
          payload.trim();
          addrType = static_cast<uint8_t>(typeStr.toInt());
          hasAddrType = true;
        }

        if (!btMgr.isEnabled()) {
          btMgr.enable();
        }
        btMgr.setDebugCaptureEnabled(true);
        const bool ok = btMgr.connectToDevice(std::string(payload.c_str()), addrType, hasAddrType);
        logSerial.printf("BTCONNECT:%s:%s\n", ok ? "OK" : "FAIL", btMgr.lastError.c_str());
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (userInputDetected || halTiltSensor.hadActivity() || activityManager.preventAutoSleep() || bleRecentActivity) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
