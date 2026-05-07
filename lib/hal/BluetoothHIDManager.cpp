#include "BluetoothHIDManager.h"
#include <Logging.h>
#include <NimBLEDevice.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <WiFi.h>
#include <algorithm>
#include <cstring>

#include "../../src/CrossPointSettings.h"

// Arduino-ESP32 3.x releases BT controller memory during startup unless a
// Bluetooth library marks it as in use before app_main(). Pulling in
// <esp32-hal-bt-mem.h> sets the core's `_btLibraryInUse` flag early via a
// constructor and keeps BLE memory reserved — but that locks ~30-40KB DRAM
// permanently, even for users who never enable BT.
//
// CROSSPOINT_BT_RESERVE_MEM=1 (default for builds that ship with BT) opts in
// to the reserve. Define CROSSPOINT_BT_RESERVE_MEM=0 to release the BT
// controller heap; the trade-off is that toggling BT on from the UI then
// requires a reboot.
#ifndef CROSSPOINT_BT_RESERVE_MEM
#define CROSSPOINT_BT_RESERVE_MEM 1
#endif
#if CROSSPOINT_BT_RESERVE_MEM && defined(ARDUINO) && __has_include(<esp32-hal-bt-mem.h>)
#include <esp32-hal-bt-mem.h>
#endif

// HID Service and characteristic UUIDs
static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2A4D";
static const char* HID_INFO_UUID = "2A4A";
static const char* HID_REPORT_MAP_UUID = "2A4B";
static const char* HID_PROTOCOL_MODE_UUID = "2A4E";

static constexpr uint8_t GAMEBRICK_ACTION_A_CODE = 0xF1;
static constexpr uint8_t GAMEBRICK_ACTION_B_CODE = 0xF2;

namespace {
// BLE intervals are in 1.25ms units and timeout is in 10ms units.
// Keep latency at 0 for low input lag while allowing a longer supervision timeout
// to reduce disconnects at marginal range.
constexpr uint16_t BLE_CONN_MIN_INTERVAL = 12;   // 15ms
constexpr uint16_t BLE_CONN_MAX_INTERVAL = 24;   // 30ms
constexpr uint16_t BLE_CONN_LATENCY = 0;
constexpr uint16_t BLE_CONN_TIMEOUT = 600;       // 6s
constexpr uint16_t BLE_CONN_SCAN_INTERVAL = 60;
constexpr uint16_t BLE_CONN_SCAN_WINDOW = 30;
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 4000;
constexpr unsigned long FREE2_STALE_RELEASE_DEFAULT_MS = 250;
constexpr unsigned long FREE2_STALE_RELEASE_READER_MS = 500;
constexpr size_t MAX_DISCOVERED_DEVICES = 24;
constexpr unsigned long RECENT_DISCONNECT_WINDOW_MS = 15000;

class StateLock {
 public:
  explicit StateLock(SemaphoreHandle_t mutex) : mutex(mutex) {
    if (mutex) {
      locked = (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
    }
  }

  ~StateLock() {
    if (locked) {
      xSemaphoreGive(mutex);
    }
  }

  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;

 private:
  SemaphoreHandle_t mutex = nullptr;
  bool locked = false;
};

bool isPlaceholderDeviceName(const std::string& name) {
  return name.empty() || name == "Unknown" || name.rfind("?-", 0) == 0;
}

bool isHIDAppearance(const uint16_t appearance) {
  return appearance >= 0x03C0 && appearance <= 0x03FF;
}

uint8_t discoveryPriority(const BluetoothDevice& device) {
  if (device.isHID) return 4;
  if (isHIDAppearance(device.appearance)) return 3;
  if (isPlaceholderDeviceName(device.name)) return 2;
  return 1;
}

bool compareDiscoveredDevice(const BluetoothDevice& a, const BluetoothDevice& b) {
  const uint8_t aPriority = discoveryPriority(a);
  const uint8_t bPriority = discoveryPriority(b);
  if (aPriority != bPriority) return aPriority > bPriority;
  return a.rssi > b.rssi;
}

const char* describeDisconnectReason(const int reason) {
  switch (reason) {
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO):
      return "connection supervision timeout";
    case BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM):
      return "remote user terminated connection";
    case BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_RESRCS):
      return "remote terminated due to resources";
    case BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_PWROFF):
      return "remote powered off or slept";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL):
      return "local host terminated connection";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT):
      return "connection establishment failed";
    case BLE_HS_ETIMEOUT:
      return "host timeout";
    case BLE_HS_ETIMEOUT_HCI:
      return "controller timeout";
    default:
      return "unknown";
  }
}

const char* connectFailureMessage(const int reason) {
  switch (reason) {
    case BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_PWROFF):
      return BluetoothHIDManager::ERROR_REMOTE_SLEEP_RETRY;
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO):
      return BluetoothHIDManager::ERROR_CONNECTION_TIMEOUT;
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT):
      return BluetoothHIDManager::ERROR_CONNECTION_FAILED;
    case BLE_HS_ETIMEOUT:
    case BLE_HS_ETIMEOUT_HCI:
      return BluetoothHIDManager::ERROR_CONNECTION_TIMEOUT;
    default:
      return BluetoothHIDManager::ERROR_CONNECTION_FAILED;
  }
}

const char* debugButtonName(const uint8_t buttonIndex) {
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

uint8_t logicalConfirmButtonIndex() {
  return SETTINGS.frontButtonConfirm < CrossPointSettings::FRONT_BUTTON_HARDWARE::FRONT_BUTTON_HARDWARE_COUNT
             ? SETTINGS.frontButtonConfirm
             : static_cast<uint8_t>(HalGPIO::BTN_CONFIRM);
}

uint8_t logicalBackButtonIndex() {
  return SETTINGS.frontButtonBack < CrossPointSettings::FRONT_BUTTON_HARDWARE::FRONT_BUTTON_HARDWARE_COUNT
             ? SETTINGS.frontButtonBack
             : static_cast<uint8_t>(HalGPIO::BTN_BACK);
}
}

struct ReportMapHints {
  bool hasConsumerPage = false;
  bool hasKeyboardPage = false;
  uint8_t preferredByteIndex = 0xFF;
};

struct ExtractedHIDKey {
  uint8_t keycode = 0x00;
  uint8_t reportIndex = 0xFF;
};

static ExtractedHIDKey extractGenericPageTurnKeycode(const uint8_t* report, size_t length) {
  ExtractedHIDKey result;

  if (!report || length == 0) {
    return result;
  }

  // First pass: prefer known page-turn keycodes anywhere in short reports.
  const size_t scanLen = length < 8 ? length : 8;
  for (size_t i = 0; i < scanLen; i++) {
    const uint8_t code = report[i];
    if (DeviceProfiles::isCommonPageTurnCode(code)) {
      result.keycode = code;
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  // Second pass: typical keyboard report key slots (bytes 2..7)
  for (size_t i = 2; i < scanLen; i++) {
    if (report[i] != 0x00) {
      result.keycode = report[i];
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  // Final fallback for non-keyboard HID layouts: first non-zero byte.
  for (size_t i = 0; i < scanLen; i++) {
    if (report[i] != 0x00) {
      result.keycode = report[i];
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  return result;
}

static uint8_t classifyFree2Direction(const uint8_t keycode) {
  if (keycode == DeviceProfiles::FREE2_FORWARD_A || keycode == DeviceProfiles::FREE2_FORWARD_B ||
      keycode == DeviceProfiles::FREE2_FORWARD_C || keycode == DeviceProfiles::FREE2_FORWARD_D) {
    return 0x01;
  }

  if (keycode == DeviceProfiles::FREE2_BACK_A || keycode == DeviceProfiles::FREE2_BACK_B ||
      keycode == DeviceProfiles::FREE2_BACK_C || keycode == DeviceProfiles::FREE2_BACK_D) {
    return 0x00;
  }

  return 0xFF;
}

static bool isFree2Profile(const DeviceProfiles::DeviceProfile* profile) {
  if (profile == nullptr || profile->name == nullptr) {
    return false;
  }

  return strcmp(profile->name, "Free2-M") == 0 || strcmp(profile->name, "Free2 Style") == 0;
}

static ReportMapHints parseReportMapHints(const std::string& map) {
  ReportMapHints hints;
  if (map.empty()) {
    return hints;
  }

  for (size_t i = 0; i + 1 < map.size(); i++) {
    const uint8_t b = static_cast<uint8_t>(map[i]);
    const uint8_t next = static_cast<uint8_t>(map[i + 1]);

    // Usage Page (1 byte value)
    if (b == 0x05) {
      if (next == 0x0C) {
        hints.hasConsumerPage = true;
      } else if (next == 0x07) {
        hints.hasKeyboardPage = true;
      }
    }
  }

  // Heuristic preferred byte index:
  // keyboard-like reports commonly place keycode at byte[2], consumer-control
  // reports are often compact and keycode-like values appear at byte[1].
  if (hints.hasKeyboardPage) {
    hints.preferredByteIndex = 2;
  } else if (hints.hasConsumerPage) {
    hints.preferredByteIndex = 1;
  }

  return hints;
}

// Global static for singleton
static BluetoothHIDManager* g_instance = nullptr;

// Scan callbacks for NimBLE 2.x - keep as static to ensure it stays alive
class ScanCallbacks : public NimBLEScanCallbacks {
  void onDiscovered(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (g_instance) {
      g_instance->onScanResult(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
    }
  }

  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (g_instance) {
      // onScanResult expects non-const pointer, need to cast
      g_instance->onScanResult(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
    } else {
      LOG_ERR("BT", "onResult called but g_instance is NULL!");
    }
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results;
    (void)reason;
    if (g_instance) {
      g_instance->onScanEnded();
    }
  }
};

// Static instance to keep callbacks alive during scan
static ScanCallbacks scanCallbacks;

// Client connection callbacks
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    LOG_INF("BT", "Client connected: %s", pClient->getPeerAddress().toString().c_str());
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    const std::string address = pClient->getPeerAddress().toString();
    LOG_ERR("BT", "Client disconnected: %s (reason: %d, %s)", address.c_str(), reason,
            describeDisconnectReason(reason));
    if (g_instance) {
      g_instance->onClientDisconnected(address, reason);
    }
  }
};

BluetoothHIDManager& BluetoothHIDManager::getInstance() {
  if (!g_instance) {
    g_instance = new BluetoothHIDManager();
    LOG_INF("BT", "BluetoothHIDManager instance created");
  }
  return *g_instance;
}

unsigned long BluetoothHIDManager::lastDisconnectTime() const {
  StateLock lock(_stateMutex);
  return _lastDisconnectTime;
}

BluetoothHIDManager::BluetoothHIDManager() {
  LOG_DBG("BT", "BluetoothHIDManager constructor");
  _stateMutex = xSemaphoreCreateMutex();
  if (!_stateMutex) {
    LOG_ERR("BT", "Failed to create Bluetooth state mutex");
  }
}

BluetoothHIDManager::~BluetoothHIDManager() {
  cleanup();
  if (_stateMutex) {
    vSemaphoreDelete(_stateMutex);
    _stateMutex = nullptr;
  }
}

void BluetoothHIDManager::cleanup() {
  if (isEnabled()) {
    disable();
  }
}

bool BluetoothHIDManager::isEnabled() const {
  StateLock lock(_stateMutex);
  return _enabled;
}

bool BluetoothHIDManager::isScanning() const {
  StateLock lock(_stateMutex);
  return _scanning;
}

std::vector<BluetoothDevice> BluetoothHIDManager::getDiscoveredDevicesCopy() const {
  StateLock lock(_stateMutex);
  std::vector<BluetoothDevice> copy = _discoveredDevices;
  return copy;
}

std::vector<ConnectedDevice> BluetoothHIDManager::getConnectedDevicesCopy() const {
  StateLock lock(_stateMutex);
  std::vector<ConnectedDevice> copy;
  copy.reserve(_connectedDevices.size());
  for (const auto& device : _connectedDevices) {
    if (device.client && device.client->isConnected()) {
      copy.push_back(device);
    }
  }
  return copy;
}

bool BluetoothHIDManager::hasConnectedDevice() const {
  StateLock lock(_stateMutex);
  for (const auto& device : _connectedDevices) {
    if (device.client && device.client->isConnected()) {
      return true;
    }
  }
  return false;
}

void BluetoothHIDManager::applyLearnedActionOverrides(ConnectedDevice& device) const {
  DeviceProfiles::DeviceProfile perDeviceProfile;
  const bool hasPerDeviceProfile = DeviceProfiles::getCustomProfileForDevice(device.address, perDeviceProfile);
  if (hasPerDeviceProfile) {
    device.simpleConfirmKeycode = perDeviceProfile.confirmCode;
    device.simpleCancelKeycode = perDeviceProfile.cancelCode;
  }

  const auto* customProfile = DeviceProfiles::getCustomProfile();
  const bool customProfileMatchesPerDevice =
      hasPerDeviceProfile && customProfile &&
      customProfile->pageUpCode == perDeviceProfile.pageUpCode &&
      customProfile->pageDownCode == perDeviceProfile.pageDownCode;
  if (customProfile && (!hasPerDeviceProfile || customProfileMatchesPerDevice)) {
    if (device.simpleConfirmKeycode == 0x00 && customProfile->confirmCode != 0x00) {
      device.simpleConfirmKeycode = customProfile->confirmCode;
    }
    if (device.simpleCancelKeycode == 0x00 && customProfile->cancelCode != 0x00) {
      device.simpleCancelKeycode = customProfile->cancelCode;
    }
  }
}

void BluetoothHIDManager::refreshLearnedActionOverrides() {
  StateLock lock(_stateMutex);
  for (auto& device : _connectedDevices) {
    applyLearnedActionOverrides(device);
  }
}

bool BluetoothHIDManager::enable() {
  HalPowerManager::Lock powerLock;
  if (isEnabled()) {
    LOG_DBG("BT", "Already enabled");
    return true;
  }

  LOG_INF("BT", "Enabling Bluetooth...");

  // Keep this firmware path to one active radio stack at a time so the heap
  // budget and radio state remain predictable on the ESP32-C3.
  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BT", "Disabling WiFi before Bluetooth startup");
    WiFi.disconnect(true);  // true = turn off WiFi radio
    WiFi.mode(WIFI_OFF);
    delay(100);  // Brief delay to ensure WiFi is fully powered down
  }

  // Initialize NimBLE stack
  NimBLEDevice::init("CrossPoint");
  // NimBLE stack initialization can be asynchronous. Wait briefly before
  // allowing commands like startScan to ensure the radio task is ready.
  delay(20);
  // Pin local ATT MTU to the BLE minimum (23 bytes). HID Reports are <=20 bytes,
  // so larger MTUs only allocate oversized ATT buffers per connection. Saves a
  // few KB heap with no effect on remote responsiveness.
  NimBLEDevice::setMTU(23);
  // Page-turn remotes are normally held close to the reader. Use the ESP32-C3
  // default +3 dBm level instead of max +9 dBm to reduce radio power draw.
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_1M_MASK);
  NimBLEDevice::setSecurityAuth(true, false, true);

  {
    StateLock lock(_stateMutex);
    _enabled = true;
    disconnectedIdleSince = 0;
  }
  lastError = "";

  LOG_INF("BT", "Bluetooth enabled successfully");
  return true;
}

bool BluetoothHIDManager::disable() {
  HalPowerManager::Lock powerLock;
  if (!isEnabled()) {
    LOG_DBG("BT", "Already disabled");
    return true;
  }

  LOG_INF("BT", "Disabling Bluetooth...");

  if (isScanning()) {
    stopScan();
  }

  // Disconnect all devices
  while (true) {
    std::string address;
    {
      StateLock lock(_stateMutex);
      if (_connectedDevices.empty()) {
        break;
      }
      address = _connectedDevices.front().address;
    }
    if (!disconnectFromDevice(address)) {
      break;
    }
  }

  // Deinitialize NimBLE stack. Stability fixes applied to NimBLE-Arduino
  // prevent the previously reported crash on re-init, allowing us to reclaim
  // ~80 KB heap for the reader activity.
  NimBLEDevice::deinit(true);

  {
    StateLock lock(_stateMutex);
    _enabled = false;
    _scanning = false;
    _bondedOnlyScan = false;
    _discoveredDevices.clear();
    disconnectedIdleSince = 0;
  }
  lastError = "";

  LOG_INF("BT", "Bluetooth disabled");
  return true;
}

void BluetoothHIDManager::startScan(uint32_t durationMs, bool bondedOnly) {
  HalPowerManager::Lock powerLock;
  {
    StateLock lock(_stateMutex);
    if (!_enabled || _scanning) {
      LOG_DBG("BT", "Cannot scan: enabled=%d scanning=%d", _enabled, _scanning);
      return;
    }

    _scanning = true;
    _bondedOnlyScan = bondedOnly;
    disconnectedIdleSince = 0;
    _discoveredDevices.clear();
    // Reserve once per scan to avoid repeated vector growth in crowded BLE environments.
    _discoveredDevices.reserve(MAX_DISCOVERED_DEVICES);
  }

  LOG_INF("BT", "Starting BLE scan for %lu ms%s", durationMs,
          bondedOnly ? " (bonded remote only)" : " (non-blocking)");

  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (!pScan) {
    LOG_ERR("BT", "Failed to get scan object");
    {
      StateLock lock(_stateMutex);
      _scanning = false;
      _bondedOnlyScan = false;
    }
    lastError = "Scan failed";
    return;
  }

  pScan->setScanCallbacks(&scanCallbacks, false);
  // The manager keeps its own capped snapshot, so do not let NimBLE retain a
  // second internal result vector during crowded scans.
  pScan->setMaxResults(0);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  if (bondedOnly && !_bondedDeviceAddress.empty()) {
    // Hardware filtering: only wake the CPU for the specific bonded device.
    NimBLEDevice::whiteListAdd(NimBLEAddress(_bondedDeviceAddress, _bondedDeviceAddrType));
    pScan->setFilterPolicy(BLE_HCI_SCAN_FILT_USE_WL);
  } else {
    pScan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
  }

  // NimBLE 2.x: duration in ms; non-zero auto-stops the scan and triggers
  // onScanEnd. Returns immediately so the UI loop stays responsive.
  const bool started = pScan->start(durationMs, false);
  if (!started) {
    LOG_ERR("BT", "Failed to start scan!");
    {
      StateLock lock(_stateMutex);
      _scanning = false;
      _bondedOnlyScan = false;
    }
    lastError = "Scan failed";
  }
}

void BluetoothHIDManager::onScanEnded() {
  size_t foundCount = 0;
  {
    StateLock lock(_stateMutex);
    _scanning = false;
    _lastScanEndTime = millis();
    foundCount = _discoveredDevices.size();
  }
  LOG_INF("BT", "Scan complete, found %d devices", static_cast<int>(foundCount));
}

void BluetoothHIDManager::onClientDisconnected(const std::string& address, const int reason) {
  StateLock lock(_stateMutex);
  snprintf(_lastDisconnectAddress, sizeof(_lastDisconnectAddress), "%s", address.c_str());
  _lastDisconnectReason = reason;
  _lastDisconnectTime = millis();

  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
                         [&address](const ConnectedDevice& dev) { return dev.address == address; });
  if (it == _connectedDevices.end()) {
    return;
  }

  if (_buttonInjector && it->activeInjectedButton != 0xFF) {
    _buttonInjector(_buttonInjectorCtx, it->activeInjectedButton, false);
  }

  LOG_DBG("BT", "Removing disconnected client entry: %s (%s)", address.c_str(), describeDisconnectReason(reason));
  _connectedDevices.erase(it);
}

void BluetoothHIDManager::stopScan() {
  HalPowerManager::Lock powerLock;
  {
    StateLock lock(_stateMutex);
    if (!_scanning) return;
  }

  LOG_INF("BT", "Stopping scan");

  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan) {
    pScan->stop();
  }

  {
    StateLock lock(_stateMutex);
    _scanning = false;
    _lastScanEndTime = millis();
    _bondedOnlyScan = false;
  }
}

void BluetoothHIDManager::onScanResult(NimBLEAdvertisedDevice* advertisedDevice) {
  if (!advertisedDevice) return;

  struct PayloadClearGuard {
    NimBLEAdvertisedDevice* device;
    ~PayloadClearGuard() { device->clearPayload(); }
  } clearGuard{advertisedDevice};

  char addressBuf[18];
  advertisedDevice->getAddress().toChars(addressBuf);
  std::string address(addressBuf);
  {
    StateLock lock(_stateMutex);
    if (_bondedOnlyScan && address != _bondedDeviceAddress) {
      return;
    }
  }

  std::string name = advertisedDevice->getName();
  int rssi = advertisedDevice->getRSSI();
  const uint8_t advAddrType = advertisedDevice->getAddress().getType();

  // Check if device advertises HID service
  bool isHID = advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));

  // Pull lightweight identification hints from the advertisement so users can
  // distinguish anonymous devices (e.g. "HID" or "Apple") without connecting.
  uint16_t appearance = 0;
  if (advertisedDevice->haveAppearance()) {
    appearance = advertisedDevice->getAppearance();
  }
  uint16_t companyId = 0xFFFF;
  if (advertisedDevice->haveManufacturerData()) {
    const std::string mfg = advertisedDevice->getManufacturerData();
    if (mfg.size() >= 2) {
      // Bluetooth SIG company ID is little-endian in the first 2 bytes
      companyId = static_cast<uint16_t>(static_cast<uint8_t>(mfg[0])) |
                  (static_cast<uint16_t>(static_cast<uint8_t>(mfg[1])) << 8);
    }
  }
  // Check if we already have this device
  StateLock lock(_stateMutex);
  bool found = false;
  for (auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      dev.rssi = rssi; // Update RSSI
      if (!name.empty() && isPlaceholderDeviceName(dev.name)) {
        dev.name = name;
      }
      if (isHID) dev.isHID = true;
      dev.addrType = advAddrType;
      if (appearance != 0) dev.appearance = appearance;
      if (companyId != 0xFFFF) dev.companyId = companyId;
      // Re-sort since RSSI / HID flag may have changed.
      std::sort(_discoveredDevices.begin(), _discoveredDevices.end(), compareDiscoveredDevice);
      found = true;
      break;
    }
  }

  if (!found) {
    // Add new device
    BluetoothDevice device;
    device.address = address;
    if (!name.empty()) {
      device.name = name;
    } else if (address.size() >= 8) {
      // No advertised name — show last 3 MAC octets so anonymous devices can be told apart
      device.name = std::string("?-") + address.substr(address.size() - 8);
    } else {
      device.name = "Unknown";
    }
    device.rssi = rssi;
    device.isHID = isHID;
    device.addrType = advAddrType;
    device.appearance = appearance;
    device.companyId = companyId;

    const bool isBondedDevice = !_bondedDeviceAddress.empty() && address == _bondedDeviceAddress;
    if (isBondedDevice && _bondedOnlyScan) {
      LOG_INF("BT", "Bonded device found during scan, stopping scan to connect");
      // Stop scan immediately to reduce radio contention during connection
      NimBLEDevice::getScan()->stop();
      _scanning = false;
      _lastScanEndTime = millis();
      _bondedOnlyScan = false;

      // Note: we're inside the _stateMutex lock from the earlier block.
      // We cannot call connectToDevice() here as it takes its own powerLock and stops scan.
      // Instead, we just let checkAutoReconnect() pick it up on the next maintenance tick,
      // or we can trigger it asynchronously. For now, since we're in a callback,
      // we'll just stop the scan and let the next loop iteration handle it.
    }

    if (_discoveredDevices.size() >= MAX_DISCOVERED_DEVICES) {
      // Do not push past reserved capacity. In crowded BLE environments, vector
      // growth can throw std::bad_alloc; exceptions are disabled, so that aborts.
      // Keep the strongest/HID candidates and always keep the bonded remote.
      if (isBondedDevice || compareDiscoveredDevice(device, _discoveredDevices.back())) {
        _discoveredDevices.back() = std::move(device);
        std::sort(_discoveredDevices.begin(), _discoveredDevices.end(), compareDiscoveredDevice);
      }
    } else {
      _discoveredDevices.push_back(std::move(device));

      // Keep the list sorted: HID devices first, then regular devices, then non-HID Apple devices.
      // Inside each category, sort by RSSI descending (closest first).
      std::sort(_discoveredDevices.begin(), _discoveredDevices.end(), compareDiscoveredDevice);
    }
  }

  LOG_DBG("BT", "Found device: %s (%s) RSSI:%d HID:%d type:%u app:0x%04X mfg:0x%04X",
          address.c_str(), address.c_str(), rssi, isHID, advAddrType, appearance, companyId);
}

bool BluetoothHIDManager::connectToDevice(const std::string& address, uint8_t addrTypeOverride,
                                          bool useAddrTypeOverride) {
  HalPowerManager::Lock powerLock;
  if (!isEnabled()) {
    LOG_ERR("BT", "Cannot connect: Bluetooth not enabled");
    lastError = "Bluetooth not enabled";
    return false;
  }

  // Check if already connected
  if (isConnected(address)) {
    LOG_INF("BT", "Already connected to %s", address.c_str());
    return true;
  }

  // Stop scanning before attempting to connect, otherwise NimBLE may fail to connect
  if (isScanning()) {
    stopScan();
  }

  // Resolve the BLE address type — many peripherals (rings, mice, modern phones) use
  // random addresses, and connect() silently fails if the type is wrong. Look up from
  // the most recent scan results first, then fall back to the bonded-device record
  // (covers boot-time auto-reconnect where no scan has run yet).
  uint8_t resolvedAddrType = BLE_ADDR_PUBLIC;
  bool addrTypeFound = false;
  bool foundInScan = false;
  std::string scannedName;
  std::string bondedName;
  {
    StateLock lock(_stateMutex);
    if (useAddrTypeOverride) {
      resolvedAddrType = addrTypeOverride;
      addrTypeFound = true;
    } else {
      for (const auto& dev : _discoveredDevices) {
        if (dev.address == address) {
          resolvedAddrType = dev.addrType;
          addrTypeFound = true;
          foundInScan = true;
          scannedName = dev.name;
          break;
        }
      }
    }
    if (!_bondedDeviceAddress.empty() && _bondedDeviceAddress == address) {
      if (!addrTypeFound) {
        resolvedAddrType = _bondedDeviceAddrType;
      }
      addrTypeFound = true;
      bondedName = _bondedDeviceName;
    }
  }

  LOG_INF("BT", "Connecting to device %s (addrType=%u, resolved=%d)",
          address.c_str(), resolvedAddrType, addrTypeFound ? 1 : 0);

  NimBLEAddress bleAddress(address, resolvedAddrType);

  // Reuse existing disconnected client objects to avoid NimBLE deleteClient() on this target.
  NimBLEClient* pClient = NimBLEDevice::getClientByPeerAddress(bleAddress);
  const bool hadExistingClient = (pClient != nullptr);
  if (!pClient) {
    pClient = NimBLEDevice::getDisconnectedClient();
    if (pClient) {
      pClient->setPeerAddress(bleAddress);
    }
  }
  if (!pClient) {
    pClient = NimBLEDevice::createClient(bleAddress);
  }

  if (!pClient) {
    lastError = "Failed to create BLE client";
    LOG_ERR("BT", "Failed to create BLE client");
    return false;
  }

  // Set connection callbacks
  static ClientCallbacks clientCallbacks;

  // Keep client lifetime under manager control so disconnect callbacks do not free it in NimBLE context.
  pClient->setSelfDelete(false, false);
  pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
  pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT,
                               BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);
  pClient->setClientCallbacks(&clientCallbacks, false);

  if (!pClient->isConnected() && !hadExistingClient) {
    pClient->deleteServices();
  }

  // Connect to device.
  // Use deleteAttributes=false so we don't wipe the GATT cache.
  // Use asyncConnect=false to block until connected.
  // Use exchangeMTU=false because several cheap HID remotes disconnect during early MTU exchange.
  if (!pClient->connect(bleAddress, false, false, false)) {
    if (hadExistingClient) {
      LOG_INF("BT", "Reconnect with existing client failed for %s, retrying with fresh client", address.c_str());
      NimBLEClient* freshClient = NimBLEDevice::createClient(bleAddress);
      if (freshClient) {
        pClient = freshClient;
        pClient->setSelfDelete(false, false);
        pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
        pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY,
                                     BLE_CONN_TIMEOUT, BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);
        pClient->setClientCallbacks(&clientCallbacks, false);
      }
    }

    if (!pClient->connect(bleAddress, false, false, false)) {
      int recentDisconnectReason = 0;
      {
        StateLock lock(_stateMutex);
        if (strncmp(_lastDisconnectAddress, address.c_str(), sizeof(_lastDisconnectAddress)) == 0 &&
            (millis() - _lastDisconnectTime) <= RECENT_DISCONNECT_WINDOW_MS) {
          recentDisconnectReason = _lastDisconnectReason;
        }
      }

      const int clientError = pClient ? pClient->getLastError() : 0;
      const int reportedReason = recentDisconnectReason != 0 ? recentDisconnectReason : clientError;
      lastError = connectFailureMessage(reportedReason);
      LOG_ERR("BT", "Failed to connect to %s (err=%d, recentDisconnect=%d, %s)", address.c_str(),
              clientError, recentDisconnectReason, describeDisconnectReason(reportedReason));
      return false;
    }
  }

  {
    StateLock lock(_stateMutex);
    if (strncmp(_lastDisconnectAddress, address.c_str(), sizeof(_lastDisconnectAddress)) == 0) {
      _lastDisconnectAddress[0] = '\0';
      _lastDisconnectReason = 0;
      _lastDisconnectTime = 0;
    }
  }

    const bool connParamsUpdated =
        pClient->updateConnParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);
    LOG_INF("BT", "Connection params update request: %d", connParamsUpdated);

    const int connectedRssi = pClient->getRssi();
    LOG_INF("BT", "Connected RSSI for %s: %d dBm", address.c_str(), connectedRssi);

    // Explicitly initiate pairing/encryption.
    // Many HID rings hide their HID service or drop the connection if pairing isn't initiated immediately.
    LOG_INF("BT", "Initiating pairing/security...");
    if (!pClient->secureConnection()) {
      LOG_ERR("BT", "Failed to secure connection (proceeding anyway)");
    }

    // Get HID service
    NimBLERemoteService* pService = pClient->getService(HID_SERVICE_UUID);
    if (!pService) {
      lastError = "HID service not found";
      LOG_ERR("BT", "Device %s doesn't have HID service!", address.c_str());

      // Dump all services to help debug proprietary TikTok rings
      LOG_INF("BT", "Dumping available services for %s:", address.c_str());
      auto services = pClient->getServices(true); // force discovery
      for (auto* s : services) {
        LOG_INF("BT", " - Found Service UUID: %s", s->getUUID().toString().c_str());
      }

      pClient->disconnect();
      return false;
    }

    // Attempt to force Report Protocol mode (0x01) when supported.
    // Some remotes behave inconsistently unless protocol mode is explicit.
    if (auto* pProtocolMode = pService->getCharacteristic(HID_PROTOCOL_MODE_UUID)) {
      if (pProtocolMode->canWrite() || pProtocolMode->canWriteNoResponse()) {
        uint8_t reportMode = 0x01;
        const bool protocolSet = pProtocolMode->writeValue(&reportMode, 1, false);
        LOG_INF("BT", "Protocol mode write (Report=0x01): %d", protocolSet);
      }
    }

    ReportMapHints reportHints;
    if (auto* pReportMap = pService->getCharacteristic(HID_REPORT_MAP_UUID)) {
      if (pReportMap->canRead()) {
        std::string reportMap = pReportMap->readValue();
        reportHints = parseReportMapHints(reportMap);
        LOG_INF("BT", "Report map hints: keyboard=%d consumer=%d preferredByte=%d len=%u",
                reportHints.hasKeyboardPage, reportHints.hasConsumerPage,
                static_cast<int>(reportHints.preferredByteIndex), static_cast<unsigned>(reportMap.size()));
      }
    }

    LOG_INF("BT", "Found HID service, enumerating report characteristics...");

    // BLE HID has multiple report characteristics (input, output, feature)
    // We need to find one that supports NOTIFY or INDICATE (input report)
    // In NimBLE 2.x, getCharacteristics() returns std::vector<NimBLERemoteCharacteristic*>
    auto pCharacteristics = pService->getCharacteristics(true);
    NimBLERemoteCharacteristic* pReportChar = nullptr;

    int reportCount = 0;
    std::vector<NimBLERemoteCharacteristic*> reportChars;
    reportChars.reserve(pCharacteristics.size());

    for (auto it = pCharacteristics.begin(); it != pCharacteristics.end(); ++it) {
      auto* pChar = *it;
      LOG_DBG("BT", "Characteristic UUID: %s, canRead:%d canWrite:%d canNotify:%d canIndicate:%d",
              pChar->getUUID().toString().c_str(),
              pChar->canRead(), pChar->canWrite(), pChar->canNotify(), pChar->canIndicate());

      if (pChar->getUUID().equals(NimBLEUUID(HID_REPORT_UUID))) {
        reportCount++;

        // Check if this report supports notify or indicate (input report)
        if (pChar->canNotify() || pChar->canIndicate()) {
          reportChars.push_back(pChar);
          LOG_INF("BT", "Added Report char #%d for subscription", reportCount);
        }
      }
    }

    if (reportChars.empty()) {
      lastError = "No input report characteristic found";
      LOG_ERR("BT", "No Report characteristic with notify/indicate found");
      pClient->disconnect();
      return false;
    }

    // Subscribe to ALL Report characteristics with notify capability
    LOG_INF("BT", "Subscribing to %d Report characteristics...", reportChars.size());
    size_t successfulSubscriptions = 0;

    for (size_t i = 0; i < reportChars.size(); i++) {
      auto* pChar = reportChars[i];

      // Clear stale CCCD state on reused clients where possible.
      (void)pChar->unsubscribe();

      // Use notifications when available, otherwise indications.
      const bool useNotify = pChar->canNotify();
      bool subResult = pChar->subscribe(useNotify, onHIDNotify);
      LOG_INF("BT", "Report char #%d subscribe (%s) result: %d", i + 1, useNotify ? "notify" : "indicate",
              subResult);
      if (subResult) {
        successfulSubscriptions++;
      }

      if (!subResult) {
        LOG_INF("BT", "Failed to subscribe to Report char #%d (continuing)", i + 1);
      }
    }

    if (successfulSubscriptions == 0) {
      lastError = "Failed to subscribe to input reports";
      LOG_ERR("BT", "No HID report subscriptions succeeded for %s", address.c_str());
      pClient->disconnect();
      return false;
    }

    LOG_INF("BT", "Subscribed to %u/%u HID Report characteristics",
            static_cast<unsigned>(successfulSubscriptions), static_cast<unsigned>(reportChars.size()));

    // Save connection with activity timestamp
    ConnectedDevice connDev;
    connDev.address = address;
    connDev.client = pClient;
    connDev.reportChars = reportChars;
    connDev.connectedTime = millis();
    connDev.subscribed = true;
    connDev.lastActivityTime = millis();  // Initialize activity timer
    connDev.wasConnected = true;  // Mark for auto-reconnect if disconnected
    connDev.descriptorHasKeyboardPage = reportHints.hasKeyboardPage;
    connDev.descriptorHasConsumerPage = reportHints.hasConsumerPage;
    connDev.descriptorSuggestedIndex = reportHints.preferredByteIndex;

    // Detect device profile
    // First, try to find the device in scan results to get its name
    if (foundInScan) {
      connDev.name = scannedName;
      LOG_INF("BT", "Device found in scan results: %s (%s)", scannedName.c_str(), address.c_str());
    }

    if (!foundInScan) {
      LOG_INF("BT", "Device not in scan results (may be previously paired): %s", address.c_str());
      if (connDev.name.empty() && !bondedName.empty()) {
        connDev.name = bondedName;
        LOG_INF("BT", "Using bonded device name hint: %s", connDev.name.c_str());
      }
    }

    // If the advertisement didn't include a name (or it's our MAC-suffix placeholder),
    // try the GATT Device Name characteristic (0x2A00 in the Generic Access service 0x1800).
    const bool nameIsPlaceholder =
        connDev.name.empty() ||
        connDev.name == "Unknown" ||
        connDev.name.rfind("?-", 0) == 0;
    if (nameIsPlaceholder) {
      if (auto* pGap = pClient->getService("1800")) {
        if (auto* pDevName = pGap->getCharacteristic("2A00")) {
          if (pDevName->canRead()) {
            const std::string gattName = pDevName->readValue();
            if (!gattName.empty()) {
              connDev.name = gattName;
              LOG_INF("BT", "Resolved name via GATT 0x2A00: %s", gattName.c_str());
            }
          }
        }
      }
    }
    if (connDev.name.empty()) connDev.name = "Unknown";

    // Profile matching priority:
    //  1. Per-device learned profile by full MAC address (most specific)
    //  2. MAC-prefix exact match (hardware ID, precise)
    //  3. User-learned global custom profile (explicitly taught by the user)
    //  4. Fuzzy name-pattern match (last resort)

    DeviceProfiles::DeviceProfile perDeviceProfile;
    const bool hasPerDeviceProfile = DeviceProfiles::getCustomProfileForDevice(address, perDeviceProfile);
    if (hasPerDeviceProfile) {
      // If we have a per-device profile, it always takes precedence unless
      // a strict MAC-prefix match exists.
      static DeviceProfiles::DeviceProfile devCopy;
      devCopy = perDeviceProfile;
      connDev.profile = &devCopy;

      connDev.simpleConfirmKeycode = perDeviceProfile.confirmCode;
      connDev.simpleCancelKeycode = perDeviceProfile.cancelCode;
      LOG_INF("BT", "Using per-device learned profile for %s", address.c_str());
    }

    if (!connDev.profile) {
      connDev.profile = DeviceProfiles::findDeviceProfile(address.c_str(), nullptr);
    }

    const auto* customProfile = DeviceProfiles::getCustomProfile();
    const bool customProfileMatchesPerDevice =
        hasPerDeviceProfile && customProfile &&
        customProfile->pageUpCode == perDeviceProfile.pageUpCode &&
        customProfile->pageDownCode == perDeviceProfile.pageDownCode;
    if (customProfile && (!hasPerDeviceProfile || customProfileMatchesPerDevice)) {
      // Carry action keys into the per-connection override slots.
      if (connDev.simpleConfirmKeycode == 0x00 && customProfile->confirmCode != 0x00) {
        connDev.simpleConfirmKeycode = customProfile->confirmCode;
      }
      if (connDev.simpleCancelKeycode == 0x00 && customProfile->cancelCode != 0x00) {
        connDev.simpleCancelKeycode = customProfile->cancelCode;
      }
    }

    if (!connDev.profile) {
      // Check if a name-matched profile exists.
      const DeviceProfiles::DeviceProfile* nameMatch =
          DeviceProfiles::findDeviceProfile(nullptr, connDev.name.c_str());

      if (nameMatch) {
        connDev.profile = nameMatch;
      } else if (hasPerDeviceProfile) {
          // already set above
      } else if (customProfile) {
        connDev.profile = customProfile;
      }
    }

    if (connDev.profile == customProfile || (connDev.profile && !connDev.profile->strictProfile)) {
      if (hasPerDeviceProfile) {
        connDev.simpleFallbackEnabled = true;
        connDev.simpleBackKeycode = perDeviceProfile.pageUpCode;
        connDev.simpleForwardKeycode = perDeviceProfile.pageDownCode;
      } else if (customProfile) {
        connDev.simpleFallbackEnabled = true;
        connDev.simpleBackKeycode = customProfile->pageUpCode;
        connDev.simpleForwardKeycode = customProfile->pageDownCode;
      }
    }
    if (connDev.profile) {
      LOG_INF("BT", "✓ Using device profile: %s (byte[%d] for keycode)",
              connDev.profile->name, connDev.profile->reportByteIndex);
      connDev.simpleFallbackEnabled = (connDev.profile == customProfile || !connDev.profile->strictProfile);
    } else {
      LOG_INF("BT", "No known profile matched for %s, will auto-detect from HID codes", address.c_str());
      if (!connDev.simpleFallbackEnabled) {
        connDev.simpleFallbackEnabled = true;
        connDev.simpleForwardKeycode = 0x00;
        connDev.simpleBackKeycode = 0x00;
      }
    }

    {
      StateLock lock(_stateMutex);
      if (!connDev.name.empty() && !isPlaceholderDeviceName(connDev.name)) {
        for (auto& dev : _discoveredDevices) {
          if (dev.address == address && isPlaceholderDeviceName(dev.name)) {
            dev.name = connDev.name;
            break;
          }
        }
      }

      auto existing = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
                                   [&address](const ConnectedDevice& dev) { return dev.address == address; });
      if (existing != _connectedDevices.end()) {
        *existing = connDev;
      } else {
        _connectedDevices.push_back(connDev);
      }
    }

    LOG_INF("BT", "Successfully connected to %s", address.c_str());
    lastError = "Connected";
    return true;
}

bool BluetoothHIDManager::disconnectFromDevice(const std::string& address) {
  LOG_INF("BT", "Disconnecting from device %s", address.c_str());

  NimBLEClient* client = nullptr;
  {
    StateLock lock(_stateMutex);
    auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
      [&address](const ConnectedDevice& dev) { return dev.address == address; });

    if (it == _connectedDevices.end()) {
      LOG_INF("BT", "Device %s not in connected list", address.c_str());
      return false;
    }

    if (_buttonInjector && it->activeInjectedButton != 0xFF) {
      _buttonInjector(_buttonInjectorCtx, it->activeInjectedButton, false);
    }
    client = it->client;
    // Remove from our list BEFORE disconnecting to avoid race conditions with onDisconnect callbacks
    _connectedDevices.erase(it);
  }

  // Ensure normal CPU speed during BLE termination to avoid WDT in low-power mode.
  if (client && client->isConnected()) {
    HalPowerManager::Lock powerLock;
    client->disconnect();
  }

  LOG_INF("BT", "Disconnected from %s", address.c_str());
  return true;
}

bool BluetoothHIDManager::identifyDevice(const std::string& address) {
  HalPowerManager::Lock powerLock;
  if (!isEnabled()) {
    LOG_ERR("BT", "Cannot identify: Bluetooth not enabled");
    lastError = "Bluetooth not enabled";
    return false;
  }
  if (isConnected(address)) {
    LOG_INF("BT", "Identify: already connected to %s, skipping", address.c_str());
    return false;
  }

  // Stop scanning before attempting to connect
  if (isScanning()) {
    stopScan();
  }

  // Resolve address type from scan results (same logic as connectToDevice)
  uint8_t addrType = BLE_ADDR_PUBLIC;
  {
    StateLock lock(_stateMutex);
    for (const auto& dev : _discoveredDevices) {
      if (dev.address == address) {
        addrType = dev.addrType;
        break;
      }
    }
  }

  LOG_INF("BT", "Identify probe: %s (addrType=%u)", address.c_str(), addrType);

  NimBLEAddress bleAddress(address, addrType);
  NimBLEClient* pClient = NimBLEDevice::getClientByPeerAddress(bleAddress);
  bool reusedClient = (pClient != nullptr);
  if (!pClient) {
    pClient = NimBLEDevice::getDisconnectedClient();
    if (pClient) pClient->setPeerAddress(bleAddress);
  }
  if (!pClient) {
    pClient = NimBLEDevice::createClient(bleAddress);
  }
  if (!pClient) {
    lastError = "Identify: client alloc failed";
    LOG_ERR("BT", "Identify: failed to create client for %s", address.c_str());
    return false;
  }

  pClient->setSelfDelete(false, false);
  pClient->setConnectTimeout(5000);  // shorter than full HID connect

  std::string discoveredName;
  if (pClient->connect(bleAddress)) {
    if (auto* pGap = pClient->getService("1800")) {
      if (auto* pDevName = pGap->getCharacteristic("2A00")) {
        if (pDevName->canRead()) {
          discoveredName = pDevName->readValue();
        }
      }
    }
    pClient->disconnect();
  } else {
    LOG_ERR("BT", "Identify: connect failed for %s", address.c_str());
    lastError = "Identify failed";
  }

  // Free the temp client unless we reused an existing one (the reuse path is
  // managed by the rest of the manager; deleting here would race with it).
  if (!reusedClient) {
    NimBLEDevice::deleteClient(pClient);
  }

  if (discoveredName.empty()) {
    LOG_INF("BT", "Identify: %s did not expose a GATT name", address.c_str());
    return false;
  }

  // Update the scan-results entry so the UI label refreshes
  {
    StateLock lock(_stateMutex);
    for (auto& dev : _discoveredDevices) {
      if (dev.address == address) {
        dev.name = discoveredName;
        LOG_INF("BT", "Identify: %s -> %s", address.c_str(), discoveredName.c_str());
        return true;
      }
    }
  }
  return false;
}

bool BluetoothHIDManager::isConnected(const std::string& address) const {
  StateLock lock(_stateMutex);
  bool connected = std::find_if(_connectedDevices.begin(), _connectedDevices.end(), [&address](const ConnectedDevice& dev) {
           return dev.address == address && dev.client && dev.client->isConnected();
         }) != _connectedDevices.end();
  return connected;
}

ConnectedDevice* BluetoothHIDManager::findConnectedDevice(const std::string& address) {
  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; });

  if (it != _connectedDevices.end()) {
    return &(*it);
  }
  return nullptr;
}

void BluetoothHIDManager::processInputEvents() {
  // Input events are processed via notifications callback
  // This method is kept for potential polling-based implementations
}

// All callback setters lock the state mutex. The callbacks themselves are
// invoked from the BLE notification path on the NimBLE stack task, so a torn
// std::function write during reassignment could in principle expose a bad
// invocable. Guarding the assignment here keeps the swap atomic relative to
// other state-mutex holders; the BLE-side reads remain unguarded for latency
// reasons but only see whole-function-object publishes.
void BluetoothHIDManager::setInputCallback(InputCb callback, void* ctx) {
  StateLock lock(_stateMutex);
  _inputCallback = callback;
  _inputCallbackCtx = ctx;
  LOG_DBG("BT", "Input callback registered");
}

void BluetoothHIDManager::setLearnInputCallback(LearnInputCb callback, void* ctx) {
  StateLock lock(_stateMutex);
  const bool wasLearning = (_learnInputCallback != nullptr);
  const bool nowLearning = (callback != nullptr);
  _learnInputCallback = callback;
  _learnInputCallbackCtx = ctx;

  // When entering learn mode, force-release any virtual button currently held
  // via BLE injection. Otherwise a button held when the wizard opened would
  // stay "pressed" while the press-side injection is suppressed below, leaving
  // the activity to consume a phantom press.
  if (!wasLearning && nowLearning) {
    for (auto& dev : _connectedDevices) {
      if (_buttonInjector && dev.activeInjectedButton != 0xFF) {
        _buttonInjector(_buttonInjectorCtx, dev.activeInjectedButton, false);
        dev.activeInjectedButton = 0xFF;
      }
    }
  }

  LOG_DBG("BT", "Learn input callback %s", nowLearning ? "registered" : "cleared");
}

void BluetoothHIDManager::setDebugInputCallback(DebugInputCb callback, void* ctx) {
  StateLock lock(_stateMutex);
  _debugInputCallback = callback;
  _debugInputCallbackCtx = ctx;
  LOG_DBG("BT", "Debug input callback %s", callback ? "registered" : "cleared");
}

void BluetoothHIDManager::setButtonInjector(ButtonInjectorCb injector, void* ctx) {
  StateLock lock(_stateMutex);
  _buttonInjector = injector;
  _buttonInjectorCtx = ctx;
  LOG_DBG("BT", "Button injector registered");
}

void BluetoothHIDManager::setReaderContextCallback(ReaderContextCb callback, void* ctx) {
  StateLock lock(_stateMutex);
  _readerContextCallback = callback;
  _readerContextCallbackCtx = ctx;
  LOG_DBG("BT", "Reader context callback registered");
}

void BluetoothHIDManager::setButtonActivityNotifier(ButtonActivityCb notifier, void* ctx) {
  StateLock lock(_stateMutex);
  _buttonActivityNotifier = notifier;
  _buttonActivityNotifierCtx = ctx;
}

void BluetoothHIDManager::setBondedDevice(const std::string& address, const std::string& name, uint8_t addrType) {
  StateLock lock(_stateMutex);
  _bondedDeviceAddress = address;
  _bondedDeviceName = name;
  _bondedDeviceAddrType = addrType;
  LOG_INF("BT", "Bonded device set: %s type=%u (%s)", _bondedDeviceAddress.c_str(), addrType,
          _bondedDeviceName.c_str());
}

bool BluetoothHIDManager::hasRecentActivity() const {
  unsigned long now = millis();
  StateLock lock(_stateMutex);

  // Keep CPU at normal frequency while scanning or shortly after a scan ends.
  // This prevents NimBLE host/controller timeouts (HCI ACK wait) at 10MHz.
  if (_scanning || (now - _lastScanEndTime < 5000)) {
    return true;
  }

  // Check if any connected device has had activity in the last 4 minutes
  // This prevents power sleep while using BLE controller
  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      unsigned long timeSinceActivity = now - device.lastActivityTime;
      if (timeSinceActivity < 240000) {  // 4 minute (240 second) threshold to keep BLE alive
        return true;
      }
    }
  }
  return false;
}

bool BluetoothHIDManager::hadRecentFree2Input(unsigned long windowMs) const {
  const unsigned long now = millis();
  StateLock lock(_stateMutex);
  for (const auto& device : _connectedDevices) {
    if (device.lastNormalizedEventMs == 0 || (now - device.lastNormalizedEventMs) > windowMs) {
      continue;
    }

    // Keep the legacy method name for compatibility, but treat any recent BLE
    // page-turner input as a signal to prefer press-driven reader navigation.
    if (isFree2Profile(device.profile) || device.activeInjectedButton != 0xFF || device.lastNormalizedKeycode != 0x00) {
      return true;
    }
  }
  return false;
}

// Static callback for HID notifications
void BluetoothHIDManager::onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (!g_instance || !pData || length == 0) return;

  // Get the device address and find the connected device
  std::string deviceAddr;
  ConnectedDevice* device = nullptr;
  if (pChar && pChar->getRemoteService()) {
    auto client = pChar->getRemoteService()->getClient();
    if (client) {
      deviceAddr = client->getPeerAddress().toString();
    }
  }

  if (deviceAddr.empty()) return;

  StateLock lock(g_instance->_stateMutex);
  device = g_instance->findConnectedDevice(deviceAddr);

  if (!device) return;

  const unsigned long nowMs = millis();
  const bool free2Profile = isFree2Profile(device->profile);

  // GameBrick can occasionally miss a release tail, leaving a virtual button
  // latched as pressed. After a long idle gap, clear stale hold state so the
  // next tap is always treated as a fresh press.
  // Keep this comfortably above the reader's 700ms chapter-skip threshold so
  // a legitimate long press is not force-released early.
  if (device->profile && strncmp(device->profile->name, "IINE Game Brick", 15) == 0) {
    constexpr unsigned long STALE_GAMEBRICK_HOLD_RESET_MS = 1200;
    if (device->activeInjectedButton != 0xFF &&
        device->lastNormalizedEventMs > 0 &&
        (nowMs - device->lastNormalizedEventMs) > STALE_GAMEBRICK_HOLD_RESET_MS) {
      if (g_instance->_buttonInjector) {
        g_instance->_buttonInjector(g_instance->_buttonInjectorCtx, device->activeInjectedButton, false);
      }
      device->activeInjectedButton = 0xFF;
      device->lastButtonState = false;
      device->lastHIDKeycode = 0x00;
      device->lastNormalizedPressed = false;
      device->lastGameBrickActiveKey = 0x00;
      device->gameBrickCenterPressFrames = 0;
      LOG_DBG("BT", "Game Brick: cleared stale held state after %lu ms idle", nowMs - device->lastNormalizedEventMs);
    }
  }

  // Update activity timestamp to keep connection alive
  device->lastActivityTime = millis();
  // Only Free2 needs hold-time capping based on BLE activity. Other remotes,
  // including GameBrick, should keep the original virtual hold semantics so
  // long-press chapter skip continues to use the full press duration.
  if (free2Profile && g_instance->_buttonActivityNotifier && device->activeInjectedButton != 0xFF) {
    g_instance->_buttonActivityNotifier(g_instance->_buttonActivityNotifierCtx, device->activeInjectedButton);
  }


  if (g_instance->_debugCaptureEnabled) {
    char rawBuf[128] = {0};
    size_t offset = 0;
    const size_t dumpLen = length < 8 ? length : 8;
    for (size_t i = 0; i < dumpLen && offset + 4 < sizeof(rawBuf); i++) {
      offset += snprintf(rawBuf + offset, sizeof(rawBuf) - offset, "%02X ", static_cast<unsigned>(pData[i]));
    }
    LOG_INF("BTDBG", "addr=%s len=%u raw=%s", device->address.c_str(), static_cast<unsigned>(length), rawBuf);
  }

  auto releaseInjectedButton = [&]() {
    if (g_instance->_buttonInjector && device->activeInjectedButton != 0xFF) {
      g_instance->_buttonInjector(g_instance->_buttonInjectorCtx, device->activeInjectedButton, false);
    }
    device->activeInjectedButton = 0xFF;
    device->pendingGameBrickRelease = false;
    device->pendingGameBrickReleaseMs = 0;
    device->pendingGameBrickKeycode = 0x00;
    device->pendingGameBrickButton = 0xFF;
  };

  // Extract keycode based on device profile or auto-detect
  uint8_t keycode = 0xFF;
  uint8_t keycodeIndex = 0xFF;
  bool isPressed = false;
  bool isGameBrickProfile = false;
  bool debugDiagnosticEmitted = false;

  auto emitDebugDiagnostic = [&](const uint8_t mappedButton) {
    if (!g_instance->_debugCaptureEnabled) {
      return;
    }
    debugDiagnosticEmitted = true;
    const uint8_t rawLength = static_cast<uint8_t>(length < 8 ? length : 8);
    if (g_instance->_debugInputCallback) {
      g_instance->_debugInputCallback(g_instance->_debugInputCallbackCtx, keycode, keycodeIndex, mappedButton,
                                      isPressed, pData, rawLength);
    }
    LOG_INF("BTDBG", "decoded key=0x%02X idx=%u pressed=%u mapped=%s", keycode,
            static_cast<unsigned>(keycodeIndex), isPressed ? 1 : 0, debugButtonName(mappedButton));
  };

  if (length < 1) {
    LOG_DBG("BT", "HID report empty, ignoring");
    return;
  }

  // Determine keycode source and press state based on device profile
  if (device->profile) {
    // Use device profile's byte index for keycode
    if (length >= device->profile->reportByteIndex + 1) {
      keycode = pData[device->profile->reportByteIndex];
      keycodeIndex = device->profile->reportByteIndex;
    }

    // For custom/learned profiles: if the fixed-index byte is not one of the learned
    // keycodes, scan the entire report.  This handles remotes where the prev/next buttons
    // send their keycodes at different byte positions, or where they arrive on separate
    // HID report characteristics with their own frame layouts.
    const bool isCustomProfile = (strcmp(device->profile->name, "Custom BLE Remote") == 0);
    auto isCustomProfileCode = [device](uint8_t code) {
      return code == device->profile->pageUpCode ||
             code == device->profile->pageDownCode ||
             (device->profile->confirmCode != 0x00 && code == device->profile->confirmCode) ||
             (device->profile->cancelCode != 0x00 && code == device->profile->cancelCode);
    };
    if (isCustomProfile &&
        !isCustomProfileCode(keycode)) {
      for (size_t bi = 0; bi < length && bi < 8; bi++) {
        const uint8_t b = pData[bi];
        if (isCustomProfileCode(b)) {
          // Release-ramp packets from this remote contain the *opposite* learned code at
          // byte[3] as a transition artifact (e.g. press 0x10, ramp emits 0x20).
          // If a button is already held and we find a different code, this is a ramp
          // artifact — ignore it so it falls through as 0x00 and triggers a clean release,
          // preventing both double-fire and the subsequent silent-drop on the next press.
          // Exception: during learn mode the wizard relies on seeing the ramp code to
          // auto-learn the second button from a single physical press.
          const bool isLearning = (g_instance->_learnInputCallback != nullptr);
          if (!isLearning && device->lastButtonState && b != device->lastHIDKeycode) {
            LOG_DBG("BT", "Custom profile: ignoring cross-code 0x%02X while 0x%02X held (release ramp)",
                    b, device->lastHIDKeycode);
            break;
          }
          keycode = b;
          keycodeIndex = static_cast<uint8_t>(bi);
          LOG_DBG("BT", "Custom profile: found learned code 0x%02X at byte[%u] (vs fixed idx %u)",
                  keycode, static_cast<unsigned>(bi),
                  static_cast<unsigned>(device->profile->reportByteIndex));
          break;
        }
      }
    }

    // For Game Brick: press state from byte[0] bit 0
    // For standard HID keyboards: press state from keycode (non-zero = pressed)
    if (strncmp(device->profile->name, "IINE Game Brick", 15) == 0) {
      isGameBrickProfile = true;
      bool gameBrickStandardMode = false;

      // --- GameBrick V2 report format (confirmed via RAW captures) ---
      // byte[0]   : frame status (0x13 pressed/active, 0x12 release tail)
      // byte[1-2] : 16-bit cycling counter (+125/frame, ~8 ms), NOT button data
      // byte[3]   : horizontal (X) joystick axis, center = 0x98
      // byte[4]   : button / vertical axis
      //               0x08 = idle / joystick center
      //               0x07 = physical UP button (d-pad up)
      //               0x09 = physical DOWN button (d-pad down)
      // LEFT/RIGHT are joystick-only: byte[4]==0x08 with byte[3] offset from 0x98.
      //
      // IMPORTANT: ignore any pre-extracted keycode from profile byte index because
      // byte[2] can naturally pass through 0x07/0x09 and cause false button presses.
      keycode = 0x00;
      keycodeIndex = 0xFF;

      auto isGameBrickSupportedCode = [](uint8_t code) {
         return code == 0x07 || code == 0x09 ||
           code == GAMEBRICK_ACTION_A_CODE ||
           code == GAMEBRICK_ACTION_B_CODE ||
               code == DeviceProfiles::KEYBOARD_UP_ARROW ||
               code == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
               code == DeviceProfiles::KEYBOARD_LEFT_ARROW ||
               code == DeviceProfiles::KEYBOARD_RIGHT_ARROW ||
               code == DeviceProfiles::KEYBOARD_ENTER ||
               code == DeviceProfiles::KEYBOARD_SPACE ||
               code == DeviceProfiles::KEYBOARD_PAGE_UP ||
               code == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
               code == DeviceProfiles::STANDARD_PAGE_UP ||
               code == DeviceProfiles::STANDARD_PAGE_DOWN;
      };

      // Some GameBrick C/T/H modes expose standard keyboard/consumer reports.
      // Prefer that path when a clear standard keycode is present.
      const ExtractedHIDKey generic = extractGenericPageTurnKeycode(pData, length);
      auto isStandardGameBrickCode = [](uint8_t code) {
        return code == DeviceProfiles::KEYBOARD_UP_ARROW ||
               code == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
               code == DeviceProfiles::KEYBOARD_LEFT_ARROW ||
               code == DeviceProfiles::KEYBOARD_RIGHT_ARROW ||
               code == DeviceProfiles::KEYBOARD_ENTER ||
               code == DeviceProfiles::KEYBOARD_SPACE ||
               code == DeviceProfiles::KEYBOARD_PAGE_UP ||
               code == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
               code == DeviceProfiles::STANDARD_PAGE_UP ||
               code == DeviceProfiles::STANDARD_PAGE_DOWN;
      };

      if (isStandardGameBrickCode(generic.keycode)) {
        gameBrickStandardMode = true;
        keycode = generic.keycode;
        keycodeIndex = generic.reportIndex;
      }

      if (!gameBrickStandardMode && length >= 5) {
        // bytes[1,2] form a 16-bit LE cycling counter (~+125/frame, LE).
        // The counter FREEZES to 0x07D0 when any physical button is pressed and
        // remains frozen through the entire press AND release-ramp sequence.
        // Joystick motion keeps the counter cycling freely.
        const uint16_t counter =
            static_cast<uint16_t>(pData[1]) | (static_cast<uint16_t>(pData[2]) << 8);
        const bool counterFrozen = (counter == device->lastGameBrickCounter);
        device->lastGameBrickCounter = counter;

        const bool isReleaseTail = (pData[0] & 0x01) == 0;
        const bool activeFrame = ((pData[0] & 0x01) != 0);
        const bool isDirectionalFreezeWindow = (counter == 0x07D0);

        // Clear the d-pad latch once the counter resumes cycling or a release-tail arrives.
        if (!counterFrozen || isReleaseTail) {
          device->lastGameBrickActiveKey = 0x00;
        }
        const uint8_t b4 = pData[4];
        if (b4 == 0x07 || b4 == 0x09) {
          const bool directionalFreezeWindow =
              isDirectionalFreezeWindow || (counterFrozen && device->lastGameBrickActiveKey != 0x00);
          if (directionalFreezeWindow) {
            // D-pad UP/DOWN uses the special 0x07D0 frozen counter window.
            // While held, the release ramp can cross the opposite code, so latch the
            // first directional code seen until release-tail/counter-change.
            if (device->lastGameBrickActiveKey == 0x00) {
              device->lastGameBrickActiveKey = b4;
            }
            if (b4 == device->lastGameBrickActiveKey) {
              keycode = b4;
              keycodeIndex = 4;
            }
          } else {
            // Non-0x07D0 window: treat 0x07/0x09 as A/B button family.
            // This preserves menu semantics (A=Select, B=Back) outside page-reading context.
            keycode = (b4 == 0x07) ? GAMEBRICK_ACTION_A_CODE : GAMEBRICK_ACTION_B_CODE;
            keycodeIndex = 4;
          }
          device->gameBrickCenterPressFrames = 0;
        } else if (b4 == 0x08) {
          // Joystick horizontal:
          // - usually appears while counter is cycling
          // - can also appear in some frozen windows for horizontal-only presses
          //
          // But while vertical d-pad latch (0x07/0x09 in 0x07D0 window) is active,
          // b4==0x08 frames are release/overshoot noise and must be ignored.
          const bool allowHorizontal = !counterFrozen || device->lastGameBrickActiveKey == 0x00;
          if (!allowHorizontal) {
            // Transitional frame from vertical press/release.
            keycode = 0x00;
            device->gameBrickCenterPressFrames = 0;
          } else {
            const int dx = static_cast<int>(pData[3]) - 0x98;
            // Empirical tuning from logs:
            // RIGHT tends to be stronger than LEFT on some units, so keep LEFT
            // threshold lower to catch weak positive deflections.
            constexpr int kDeadzoneRight = 2;
            constexpr int kDeadzoneLeft = 0;
            if (dx < -kDeadzoneRight) {
              keycode = DeviceProfiles::KEYBOARD_RIGHT_ARROW;
              keycodeIndex = 3;
              device->gameBrickCenterPressFrames = 0;
            } else if (dx > kDeadzoneLeft) {
              keycode = DeviceProfiles::KEYBOARD_LEFT_ARROW;
              keycodeIndex = 3;
              device->gameBrickCenterPressFrames = 0;
            } else if (activeFrame && !counterFrozen && device->lastGameBrickActiveKey == 0x00) {
              // Some GameBrick units appear to emit LEFT as a centered b4==0x08 burst
              // (dx≈0) with a cycling counter. Require several consecutive frames so
              // transitional noise from other keys is ignored.
              if (device->gameBrickCenterPressFrames < 255) {
                device->gameBrickCenterPressFrames++;
              }
              if (device->gameBrickCenterPressFrames >= 6) {
                keycode = DeviceProfiles::KEYBOARD_LEFT_ARROW;
                keycodeIndex = 3;
              }
            } else {
              device->gameBrickCenterPressFrames = 0;
            }
            // else: centered idle → keycode stays 0x00
          }
        } else {
          device->gameBrickCenterPressFrames = 0;
        }
        // All other byte[4] values (ramp overshoot > 0x09 or < 0x07) → 0x00.
      }

      // If nothing found, keycode stays 0x00 → treated as release.

      // Game Brick: accept only stable digital-button report family (0x1x).
      // Ignore noisy transitional frames (commonly 0x2x/0x3x) that can trigger false presses.
      if (gameBrickStandardMode) {
        isPressed = (keycode != 0x00) && isGameBrickSupportedCode(keycode);
      } else {
        const bool stableButtonReport = (pData[0] & 0xF0) == 0x10;
        if (!stableButtonReport) {
          LOG_DBG("BT", "Game Brick: ignoring transitional report byte[0]=0x%02X, keycode=0x%02X", pData[0], keycode);
          // Keep the previous button state intact while skipping transitional frames.
          // Resetting state here can create a duplicate "new press" on the next stable
          // frame, which shows up as a double page-turn.
          return;
        }

        // Press is only valid with a supported decoded code plus active frame bit.
        isPressed = ((pData[0] & 0x01) != 0) && isGameBrickSupportedCode(keycode);
      }

      // Prevent initial stale pressed frame right after subscribe from triggering navigation.
      // Only allow presses after at least one clean release frame has been seen.
      if (!device->hasSeenRelease) {
        if (!isPressed) {
          device->hasSeenRelease = true;
        } else {
          // Some GameBrick variants do not emit an immediate release frame after
          // connect and would otherwise be blocked indefinitely. Arm input on
          // the first valid GameBrick press instead of discarding it.
          device->hasSeenRelease = true;
          LOG_DBG("BT", "Game Brick: arming on first valid press keycode=0x%02X", keycode);
        }
      }

#if LOG_LEVEL >= 2
      // Full raw dump so we can reverse-engineer D-pad encoding. Skipped
      // entirely in production builds (LOG_DBG stripped), and in dev builds
      // gated by debug-capture so the per-notify snprintf only runs when the
      // user actively wants the trace.
      if (g_instance->_debugCaptureEnabled) {
        char rawBuf[64];
        int pos = 0;
        for (size_t ri = 0; ri < length && ri < 8 && pos < 56; ri++) {
          pos += snprintf(rawBuf + pos, sizeof(rawBuf) - pos, "%02X ", pData[ri]);
        }
        LOG_DBG("BT", "Game Brick RAW[%u]: %s=> keycode=0x%02X idx=%u pressed=%d",
                static_cast<unsigned>(length), rawBuf, keycode,
                static_cast<unsigned>(keycodeIndex), isPressed);
      }
#endif
    } else {
      // Standard HID keyboards/custom profiles: keycode non-zero = pressed.
      // Learned Confirm/Back buttons can be reported in a different byte than
      // page-turn keys on some remotes. Scan the short report for those action
      // codes before falling back to the profile's fixed keycode byte.
      auto isLearnedActionCode = [device](uint8_t code) {
        if (code == 0x00 || code == 0xFF) return false;
        if (device->simpleConfirmKeycode != 0x00 && code == device->simpleConfirmKeycode) return true;
        if (device->simpleCancelKeycode != 0x00 && code == device->simpleCancelKeycode) return true;
        if (device->profile) {
          if (device->profile->confirmCode != 0x00 && code == device->profile->confirmCode) return true;
          if (device->profile->cancelCode != 0x00 && code == device->profile->cancelCode) return true;
        }
        return false;
      };
      if (!isLearnedActionCode(keycode)) {
        const size_t scanLen = length < 8 ? length : 8;
        for (size_t bi = 0; bi < scanLen; bi++) {
          const uint8_t b = pData[bi];
          if (!isLearnedActionCode(b)) {
            continue;
          }
          if (device->lastButtonState && device->lastHIDKeycode != 0x00 && device->lastHIDKeycode != 0xFF &&
              b != device->lastHIDKeycode) {
            LOG_DBG("BT", "Learned action scan ignored cross-code 0x%02X while 0x%02X held", b,
                    device->lastHIDKeycode);
            break;
          }
          keycode = b;
          keycodeIndex = static_cast<uint8_t>(bi);
          LOG_DBG("BT", "Found learned action code 0x%02X at byte[%u]", keycode, static_cast<unsigned>(bi));
          break;
        }
      }

      // Normalise 0xFF (= "nothing found in report") to 0x00 so that short
      // release frames (e.g. 1-byte consumer control [0x00]) are treated as
      // a key-release rather than a phantom press.
      if (keycode == 0xFF) {
        keycode = 0x00;
      }
      isPressed = (keycode != 0x00);
      LOG_DBG("BT", "Device %s: keycode=0x%02X, pressed=%d", device->profile->name, keycode, isPressed);
    }
  } else {
    // Auto-detect mode: support a wider range of generic HID remotes.
    const ExtractedHIDKey extracted = extractGenericPageTurnKeycode(pData, length);
    keycode = extracted.keycode;
    keycodeIndex = extracted.reportIndex;

    if (device->descriptorSuggestedIndex != 0xFF && length > device->descriptorSuggestedIndex) {
      const uint8_t hintedCode = pData[device->descriptorSuggestedIndex];
      if (hintedCode != 0x00 && hintedCode != 0xFF &&
          (keycode == 0x00 || keycode == 0xFF || DeviceProfiles::isCommonPageTurnCode(hintedCode))) {
        keycode = hintedCode;
        keycodeIndex = device->descriptorSuggestedIndex;
      }
    }

    // Some remotes emit noisy 0x07/0x09 bytes in parallel with true rolling keycodes.
    // If we selected 0x07/0x09, search the short report for a stronger non-GameBrick code.
    if ((keycode == 0x07 || keycode == 0x09) && length > 0) {
      const size_t scanLen = length < 8 ? length : 8;
      for (size_t i = 0; i < scanLen; i++) {
        const uint8_t candidate = pData[i];
        if (candidate == 0x00 || candidate == 0xFF || candidate == 0x07 || candidate == 0x09) {
          continue;
        }
        if (DeviceProfiles::isCommonPageTurnCode(candidate)) {
          keycode = candidate;
          keycodeIndex = static_cast<uint8_t>(i);
          break;
        }
      }
    }

    // Keep existing GameBrick bit0 press-state behavior when applicable.
    if (length >= 5 && (keycode == 0x07 || keycode == 0x09)) {
      isPressed = ((pData[0] & 0x01) != 0) || (keycode != 0x00);
      LOG_DBG("BT", "Auto-detect (GameBrick-like): keycode=0x%02X, pressed=%d", keycode, isPressed);
    } else {
      isPressed = (keycode != 0x00);
      LOG_DBG("BT", "Auto-detect (generic HID): keycode=0x%02X, pressed=%d", keycode, isPressed);
    }
  }

  // Update release state for startup noise gate
  // When we see the first release (isPressed = false), we enable button injection
  if (!isPressed && !device->hasSeenRelease) {
    device->hasSeenRelease = true;
  }

  // Ignore if no valid keycode detected
  if (keycode == 0x00 || keycode == 0xFF) {
    emitDebugDiagnostic(device->activeInjectedButton);
    releaseInjectedButton();
    // Track state for transition detection
    device->lastButtonState = isPressed;
    device->lastHIDKeycode = keycode;
    device->lastNormalizedDirection = 0xFF;
    return;
  }

  // CRITICAL GATE: Don't inject any buttons until we've seen the first release
  // This prevents startup transient noise from being interpreted as button presses
  if (!device->hasSeenRelease) {
    const bool likelyFree2Press =
        keycode == DeviceProfiles::FREE2_FORWARD_A || keycode == DeviceProfiles::FREE2_FORWARD_B ||
        keycode == DeviceProfiles::FREE2_FORWARD_C || keycode == DeviceProfiles::FREE2_FORWARD_D ||
        keycode == DeviceProfiles::FREE2_BACK_A || keycode == DeviceProfiles::FREE2_BACK_B ||
        keycode == DeviceProfiles::FREE2_BACK_C || keycode == DeviceProfiles::FREE2_BACK_D;

    if (device->profile == nullptr && likelyFree2Press && isPressed) {
      // Free 2 may not emit a clean initial release frame; arm on first valid press.
      device->hasSeenRelease = true;
      LOG_DBG("BT", "Arming auto-detect on first valid Free2 code: 0x%02X", keycode);
    }

    releaseInjectedButton();
    device->lastButtonState = isPressed;
    device->lastHIDKeycode = keycode;
    return;
  }

  const uint8_t free2Direction = free2Profile ? classifyFree2Direction(keycode) : 0xFF;

  // Detect button PRESS transition.
  // For most remotes, key changes while held are treated as a new press event.
  // For Game Brick, ignore key-change retriggers while held to avoid duplicate events.
  bool isNewPressEvent =
      isPressed && (!device->lastButtonState || (!isGameBrickProfile && keycode != device->lastHIDKeycode));

  // Free2 reports rolling keycodes while one button is held.
  // Collapse that family to one logical press and ignore family flips until release.
  if (free2Profile && isPressed) {
    if (!device->lastButtonState) {
      device->lastNormalizedDirection = free2Direction;
    } else if (device->lastNormalizedDirection != 0xFF && free2Direction == device->lastNormalizedDirection) {
      isNewPressEvent = false;
    } else if (device->lastNormalizedDirection != 0xFF && free2Direction != 0xFF &&
               free2Direction != device->lastNormalizedDirection) {
      isNewPressEvent = false;
      if (device->activeInjectedButton != 0xFF) {
        keycode = device->lastHIDKeycode;
      }
    }
  }

  if (isGameBrickProfile && isPressed && !isNewPressEvent && keycode == device->lastHIDKeycode &&
      device->lastNormalizedEventMs > 0) {
    constexpr unsigned long GAMEBRICK_REPRESS_IDLE_MS = 220;
    if ((nowMs - device->lastNormalizedEventMs) > GAMEBRICK_REPRESS_IDLE_MS) {
      isNewPressEvent = true;
      device->lastButtonState = false;
      device->lastNormalizedPressed = false;
      LOG_DBG("BT", "Game Brick: promoting same-key re-press after %lu ms idle (key=0x%02X)",
              nowMs - device->lastNormalizedEventMs, keycode);
    }
  }

  if (isNewPressEvent && device->lastNormalizedPressed && device->lastNormalizedKeycode == keycode &&
      (nowMs - device->lastNormalizedEventMs) < 90) {
    isNewPressEvent = false;
    if (g_instance->_debugCaptureEnabled) {
      LOG_INF("BTDBG", "Suppressed jitter duplicate key=0x%02X dt=%lu", keycode,
              nowMs - device->lastNormalizedEventMs);
    }
  }
  if (isNewPressEvent) {
    LOG_INF("BT", ">>> BUTTON PRESSED: keycode=0x%02X <<<", keycode);

    if (g_instance->_learnInputCallback && keycode != 0x00 && keycode != 0xFF && keycodeIndex != 0xFF) {
      g_instance->_learnInputCallback(g_instance->_learnInputCallbackCtx, keycode, keycodeIndex);
    }

    // Also call original callback if set
    if (g_instance->_inputCallback) {
      g_instance->_inputCallback(g_instance->_inputCallbackCtx, keycode);
    }
  }

  uint8_t mappedButton = isPressed ? g_instance->mapKeycodeToButton(keycode, device) : 0xFF;

  // Free2 can wobble briefly while a key is held, causing opposite-direction flips or
  // transient unmapped frames. Keep the active direction latched during a continuous hold
  // and wait for an actual release before changing direction.
  if (free2Profile && isPressed && device->lastButtonState && device->activeInjectedButton != 0xFF) {
    if (mappedButton == 0xFF) {
      mappedButton = device->activeInjectedButton;
    } else if (mappedButton != device->activeInjectedButton) {
      if (g_instance->_debugCaptureEnabled) {
        LOG_INF("BTDBG", "Hold wobble suppressed: active=%u incoming=%u key=0x%02X", device->activeInjectedButton,
                mappedButton, keycode);
      }
      mappedButton = device->activeInjectedButton;
      isNewPressEvent = false;
    }
  }

  const bool isGameBrickActionKey = isGameBrickProfile &&
                                    (keycode == GAMEBRICK_ACTION_A_CODE || keycode == GAMEBRICK_ACTION_B_CODE);
  const uint8_t gameBrickActionButton = isGameBrickActionKey ? g_instance->mapKeycodeToButton(keycode, device) : 0xFF;

  if (device->pendingGameBrickRelease) {
    if (isPressed && keycode == device->pendingGameBrickKeycode && mappedButton == device->pendingGameBrickButton) {
      device->pendingGameBrickRelease = false;
      device->pendingGameBrickReleaseMs = 0;
      device->pendingGameBrickKeycode = 0x00;
      device->pendingGameBrickButton = 0xFF;
      mappedButton = device->activeInjectedButton;
      isNewPressEvent = false;
    } else if (isPressed && mappedButton != device->pendingGameBrickButton) {
      releaseInjectedButton();
    }
  }

  if (!debugDiagnosticEmitted) {
    emitDebugDiagnostic(mappedButton);
  }

  if (isGameBrickProfile && g_instance->_debugCaptureEnabled && isPressed) {
    const char* keyLabel = "Unknown";
    switch (keycode) {
      case DeviceProfiles::KEYBOARD_UP_ARROW:
        keyLabel = "DPad Up";
        break;
      case DeviceProfiles::KEYBOARD_DOWN_ARROW:
        keyLabel = "DPad Down";
        break;
      case DeviceProfiles::KEYBOARD_LEFT_ARROW:
        keyLabel = "DPad Left";
        break;
      case DeviceProfiles::KEYBOARD_RIGHT_ARROW:
        keyLabel = "DPad Right";
        break;
      case GAMEBRICK_ACTION_A_CODE:
        keyLabel = "A";
        break;
      case GAMEBRICK_ACTION_B_CODE:
        keyLabel = "B";
        break;
      case 0x07:
        keyLabel = "Up";
        break;
      case 0x09:
        keyLabel = "Down";
        break;
      default:
        break;
    }

    const char* actionLabel = "Unmapped";
    switch (mappedButton) {
      case HalGPIO::BTN_UP:
        actionLabel = "Up/PageBack";
        break;
      case HalGPIO::BTN_DOWN:
        actionLabel = "Down/PageForward";
        break;
      case HalGPIO::BTN_LEFT:
        actionLabel = "Left";
        break;
      case HalGPIO::BTN_RIGHT:
        actionLabel = "Right";
        break;
      case HalGPIO::BTN_CONFIRM:
        actionLabel = "Select";
        break;
      case HalGPIO::BTN_BACK:
        actionLabel = "Back";
        break;
      default:
        break;
    }

    LOG_INF("BTDBG", "GameBrick %s (0x%02X) -> %s", keyLabel, keycode, actionLabel);
  }

  if (!isPressed || mappedButton == 0xFF) {
    if (isGameBrickActionKey && device->activeInjectedButton == gameBrickActionButton && gameBrickActionButton != 0xFF) {
      constexpr unsigned long GAMEBRICK_ACTION_RELEASE_GRACE_MS = 110;
      device->pendingGameBrickRelease = true;
      device->pendingGameBrickReleaseMs = nowMs + GAMEBRICK_ACTION_RELEASE_GRACE_MS;
      device->pendingGameBrickKeycode = keycode;
      device->pendingGameBrickButton = gameBrickActionButton;
    } else {
      releaseInjectedButton();
    }
  } else {
    if (device->activeInjectedButton != 0xFF && device->activeInjectedButton != mappedButton) {
      releaseInjectedButton();
    }

    // While the learn wizard is active, route presses to the learn callback only
    // and skip virtual-button injection. This prevents a previously-mapped BLE
    // button (e.g. the device's own Cancel key bound to BTN_BACK from a prior
    // learning) from also firing the activity's physical-button handler and
    // canceling the wizard mid-capture.
    const bool suppressForLearn = (g_instance->_learnInputCallback != nullptr);

    if (g_instance->_buttonInjector && device->activeInjectedButton == 0xFF && !suppressForLearn) {
      if (isGameBrickProfile && device->lastInjectedKeycode == keycode &&
          (millis() - device->lastInjectionTime) < 180) {
        LOG_DBG("BT", "Game Brick: debouncing duplicate key 0x%02X (%lu ms)", keycode,
                millis() - device->lastInjectionTime);
      } else {
      const char* buttonName = "Unknown";
      switch (mappedButton) {
        case HalGPIO::BTN_UP:
          buttonName = "Up/PageBack";
          break;
        case HalGPIO::BTN_DOWN:
          buttonName = "Down/PageForward";
          break;
        case HalGPIO::BTN_LEFT:
          buttonName = "Left";
          break;
        case HalGPIO::BTN_RIGHT:
          buttonName = "Right";
          break;
        case HalGPIO::BTN_CONFIRM:
          buttonName = "Select";
          break;
        case HalGPIO::BTN_BACK:
          buttonName = "Back";
          break;
        default:
          break;
      }
      if (g_instance->_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped key 0x%02X -> %s", keycode, buttonName);
      }
      g_instance->_buttonInjector(g_instance->_buttonInjectorCtx, mappedButton, true);
      device->activeInjectedButton = mappedButton;
      if (free2Profile && g_instance->_buttonActivityNotifier) {
        // Seed the hold timer on the very first injected Free2 press. This keeps a
        // missing release frame from letting a short tap age into a long-press skip.
        g_instance->_buttonActivityNotifier(g_instance->_buttonActivityNotifierCtx, mappedButton);
      }
      device->lastInjectionTime = millis();
      device->lastInjectedKeycode = keycode;
      }
    }
  }

  // Track the button state and keycode for next time
  device->lastButtonState = isPressed;
  device->lastHIDKeycode = keycode;
  device->lastNormalizedEventMs = nowMs;
  device->lastNormalizedKeycode = keycode;
  device->lastNormalizedPressed = isPressed;
  if (!isPressed) {
    device->lastNormalizedDirection = 0xFF;
  } else if (free2Profile && free2Direction != 0xFF) {
    device->lastNormalizedDirection = free2Direction;
  }
}

uint16_t BluetoothHIDManager::parseHIDReport(uint8_t* data, size_t length) {
  if (length < 3) {
    LOG_ERR("BT", "Invalid HID report length: %d", length);
    return 0;
  }

  uint8_t modifier = data[0];
  uint8_t keycode = data[2]; // First key in the report

  // If no key pressed (all zeros), return 0
  if (keycode == 0 && modifier == 0) {
    return 0;
  }

  // Log non-empty reports only during active debug capture to keep the hot path light.
  if (_debugCaptureEnabled) {
    LOG_INF("BT", "HID Report: mod=0x%02X key=0x%02X", modifier, keycode);
  }

  // Combine modifier and keycode (modifier in upper byte, keycode in lower)
  uint16_t combined = (static_cast<uint16_t>(modifier) << 8) | keycode;

  return combined;
}

// Map HID keycodes to navigator buttons based on device profile
// Only maps keycodes that match the current device's profile to prevent
// unwanted D-pad or other button inputs from triggering page turns
uint8_t BluetoothHIDManager::mapKeycodeToButton(uint8_t keycode, ConnectedDevice* device) {
  const DeviceProfiles::DeviceProfile* profile = device ? device->profile : nullptr;

  // Log keycode for debugging
  if (keycode != 0x00) {
    LOG_DBG("BT", "mapKeycodeToButton() called with keycode: 0x%02X", keycode);
  }

  // Learned action keys must win in every activity, including reader context.
  // Without this, strict profiles such as GameBrick can remap the same key to
  // page turn before the reader ever sees Confirm/Back.
  if (device) {
    if (device->simpleConfirmKeycode != 0x00 && keycode == device->simpleConfirmKeycode) {
      LOG_INF("BT", "Per-device learned key 0x%02X -> BTN_CONFIRM", keycode);
      return logicalConfirmButtonIndex();
    }
    if (device->simpleCancelKeycode != 0x00 && keycode == device->simpleCancelKeycode) {
      LOG_INF("BT", "Per-device learned key 0x%02X -> BTN_BACK", keycode);
      return logicalBackButtonIndex();
    }
  }

  // Generic keyboard/remote defaults: Enter and Space are always Confirm/Select
  // unless overridden by a learned mapping above.
  if (keycode == DeviceProfiles::KEYBOARD_ENTER || keycode == DeviceProfiles::KEYBOARD_SPACE) {
    return logicalConfirmButtonIndex();
  }

  // If we have a device profile, ONLY map keycodes specific to that profile
  if (profile) {
    // Optional learned menu-action codes on the profile itself. Check these
    // before page navigation so explicit Confirm/Back mappings are respected
    // inside the reader as well as in menus.
    if (profile->confirmCode != 0x00 && keycode == profile->confirmCode) {
      LOG_INF("BT", "Matched profile confirmCode 0x%02X -> BTN_CONFIRM", keycode);
      return logicalConfirmButtonIndex();
    }
    if (profile->cancelCode != 0x00 && keycode == profile->cancelCode) {
      LOG_INF("BT", "Matched profile cancelCode 0x%02X -> BTN_BACK", keycode);
      return logicalBackButtonIndex();
    }

    // Free 2 reports a rolling keycode family while button is held.
    // These groups are captured from device logs and map to stable page actions.
    if (strcmp(profile->name, "Free2-M") == 0 || strcmp(profile->name, "Free2 Style") == 0) {
      const bool isForward =
          keycode == 0x1C || keycode == 0xC4 || keycode == 0x6C || keycode == 0xBC;
      const bool isBack =
          keycode == 0xB4 || keycode == 0x0E || keycode == 0x66 || keycode == 0x16;

      if (isForward) {
        LOG_INF("BT", "Free2 rolling-code forward match: 0x%02X", keycode);
        return HalGPIO::BTN_DOWN;
      }

      if (isBack) {
        LOG_INF("BT", "Free2 rolling-code back match: 0x%02X", keycode);
        return HalGPIO::BTN_UP;
      }
    }

    if (strncmp(profile->name, "IINE Game Brick", 15) == 0) {
      bool inReaderContext = false;
      if (_readerContextCallback) {
        inReaderContext = _readerContextCallback(_readerContextCallbackCtx);
      }

      // Synthetic A/B mapping:
      // - Menus & Reader: A=Confirm, B=Back
      if (keycode == GAMEBRICK_ACTION_A_CODE) {
        return logicalConfirmButtonIndex();
      }

      if (keycode == GAMEBRICK_ACTION_B_CODE) {
        return logicalBackButtonIndex();
      }

      // Physical UP button (byte[4]=0x07 = profile->pageDownCode).
      // Maps to BTN_UP in all contexts: navigate up in menus, page-back in reader.
      if (keycode == profile->pageDownCode) {
        return HalGPIO::BTN_UP;
      }

      // Physical DOWN button (byte[4]=0x09 = profile->pageUpCode).
      // Maps to BTN_DOWN in all contexts: navigate down in menus, page-forward in reader.
      if (keycode == profile->pageUpCode) {
        return HalGPIO::BTN_DOWN;
      }

      // Keyboard/consumer-mode directional mappings (C/T/H mode variants).
      if (keycode == DeviceProfiles::KEYBOARD_UP_ARROW ||
          keycode == DeviceProfiles::KEYBOARD_PAGE_UP ||
          keycode == DeviceProfiles::STANDARD_PAGE_DOWN) {
        return HalGPIO::BTN_UP;
      }

      if (keycode == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
          keycode == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
          keycode == DeviceProfiles::STANDARD_PAGE_UP) {
        return HalGPIO::BTN_DOWN;
      }

      // Joystick LEFT/RIGHT (decoded from byte[3] offset when byte[4]=0x08).
      // In non-reader context: emit true LEFT/RIGHT so activities can decide
      // behavior (many menus already treat LEFT/RIGHT as prev/next via ButtonNavigator).
      // In reader context: suppress to avoid accidental exits/actions.
      if (!inReaderContext) {
        if (keycode == DeviceProfiles::KEYBOARD_LEFT_ARROW) return HalGPIO::BTN_LEFT;
        if (keycode == DeviceProfiles::KEYBOARD_RIGHT_ARROW) return HalGPIO::BTN_RIGHT;
      }

      if (keycode == DeviceProfiles::KEYBOARD_ENTER || keycode == DeviceProfiles::KEYBOARD_SPACE) {
        return logicalConfirmButtonIndex();
      }

      return 0xFF;
    }

    if (keycode == profile->pageUpCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Matched profile pageUpCode 0x%02X (%s) -> PageBack", keycode, profile->name);
      }
      return HalGPIO::BTN_UP;
    } else if (keycode == profile->pageDownCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Matched profile pageDownCode 0x%02X (%s) -> PageForward", keycode, profile->name);
      }
      return HalGPIO::BTN_DOWN;
    }

    // The known profile didn't recognise this keycode. For non-strict (standard layout)
    // profiles, also consult the user-learned custom mapping as a fallback. This covers
    // the common case where a device partially matches a known profile (e.g. its back
    // button matches MINI_KEYBOARD but its forward button uses a different code).
    const bool isStrict = profile->strictProfile;
    if (!isStrict) {
      if (const auto* learned = DeviceProfiles::getCustomProfile()) {
        if (learned->confirmCode != 0x00 && keycode == learned->confirmCode) {
          LOG_INF("BT", "Custom-fallback: 0x%02X -> BTN_CONFIRM (profile=%s)", keycode, profile->name);
          return logicalConfirmButtonIndex();
        }
        if (learned->cancelCode != 0x00 && keycode == learned->cancelCode) {
          LOG_INF("BT", "Custom-fallback: 0x%02X -> BTN_BACK (profile=%s)", keycode, profile->name);
          return logicalBackButtonIndex();
        }
        if (keycode == learned->pageUpCode) {
          LOG_INF("BT", "Custom-fallback: 0x%02X -> PageBack (profile=%s)", keycode, profile->name);
          return HalGPIO::BTN_UP;
        }
        if (keycode == learned->pageDownCode) {
          LOG_INF("BT", "Custom-fallback: 0x%02X -> PageForward (profile=%s)", keycode, profile->name);
          return HalGPIO::BTN_DOWN;
        }
      }
    }

    // Not matched by profile or fallback - ignore
    LOG_DBG("BT", "Keycode 0x%02X not in profile %s (expecting 0x%02X/0x%02X), ignoring",
            keycode, profile->name, profile->pageUpCode, profile->pageDownCode);
    return 0xFF;
  }

  // Global learned mappings for devices with no active profile.
  if (const auto* customProfile = DeviceProfiles::getCustomProfile()) {
    if (customProfile->confirmCode != 0x00 && keycode == customProfile->confirmCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped learned key 0x%02X -> BTN_CONFIRM", keycode);
      }
      return logicalConfirmButtonIndex();
    }
    if (customProfile->cancelCode != 0x00 && keycode == customProfile->cancelCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped learned key 0x%02X -> BTN_BACK", keycode);
      }
      return logicalBackButtonIndex();
    }
    if (keycode == customProfile->pageUpCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped learned key 0x%02X -> PageBack", keycode);
      }
      return HalGPIO::BTN_UP;
    }
    if (keycode == customProfile->pageDownCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped learned key 0x%02X -> PageForward", keycode);
      }
      return HalGPIO::BTN_DOWN;
    }
  }

  // No profile match - use broad common-key mapping for generic remotes/keyboards.
  bool pageForward = false;
  if (DeviceProfiles::mapCommonCodeToDirection(keycode, pageForward)) {
    if (_debugCaptureEnabled) {
      if (pageForward) {
        LOG_INF("BT", "Mapped generic key 0x%02X -> PageForward", keycode);
      } else {
        LOG_INF("BT", "Mapped generic key 0x%02X -> PageBack", keycode);
      }
    }
    return pageForward ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
  }

  if (keycode == 0x00) {
    return 0xFF;
  }

  if (device && device->simpleFallbackEnabled) {
    if (device->simpleForwardKeycode == 0x00) {
      device->simpleForwardKeycode = keycode;
      LOG_INF("BT", "Simple fallback learned FORWARD keycode 0x%02X", keycode);

      if (device->simpleBackKeycode != 0x00) {
        const uint8_t idx = (device->descriptorSuggestedIndex == 0xFF) ? 2 : device->descriptorSuggestedIndex;
        DeviceProfiles::setCustomProfileForDevice(device->address, device->simpleBackKeycode,
                                                  device->simpleForwardKeycode, idx);
      }
      return HalGPIO::BTN_DOWN;
    }

    if (keycode == device->simpleForwardKeycode) {
      return HalGPIO::BTN_DOWN;
    }

    if (device->simpleBackKeycode == 0x00) {
      device->simpleBackKeycode = keycode;
      LOG_INF("BT", "Simple fallback learned BACK keycode 0x%02X", keycode);
      const uint8_t idx = (device->descriptorSuggestedIndex == 0xFF) ? 2 : device->descriptorSuggestedIndex;
      DeviceProfiles::setCustomProfileForDevice(device->address, device->simpleBackKeycode,
                                                device->simpleForwardKeycode, idx);
      return HalGPIO::BTN_UP;
    }

    if (keycode == device->simpleBackKeycode) {
      return HalGPIO::BTN_UP;
    }

  }

  LOG_DBG("BT", "Unmapped keycode: 0x%02X (no profile)", keycode);
  return 0xFF;
}

void BluetoothHIDManager::updateActivity() {
  // Hot-path early-out: if BT is fully off there's nothing to maintain. Avoids
  // taking the state mutex on every loop iteration when the radio is disabled.
  if (!_enabled) return;
  // Cheap snapshot read (single-byte loads) — safe to check before taking the lock.
  // If there is nothing connected and we are not scanning, also skip the work.
  // The maintenance branch below would be a no-op anyway.
  {
    StateLock lock(_stateMutex);
    if (_connectedDevices.empty() && !_scanning && disconnectedIdleSince == 0) {
      return;
    }
  }
  unsigned long now = millis();
  const bool inReaderContext = _readerContextCallback && _readerContextCallback(_readerContextCallbackCtx);
  const bool runMaintenance = (now - lastMaintenanceCheck) >= 10000;
  std::string inactiveAddress;
  unsigned long inactiveTimeMs = 0;
  std::string bondedAddressToConnect;
  bool bondedFound = false;
  bool shouldDisableIdleRadio = false;

  {
    StateLock lock(_stateMutex);
    if (runMaintenance) {
      lastMaintenanceCheck = now;
    }

    // Single pass over connected devices. This is hit from loop(), so keep it tight.
    for (auto& device : _connectedDevices) {
      if (device.pendingGameBrickRelease && device.pendingGameBrickReleaseMs > 0 &&
          now >= device.pendingGameBrickReleaseMs) {
        if (_buttonInjector && device.activeInjectedButton != 0xFF) {
          _buttonInjector(_buttonInjectorCtx, device.activeInjectedButton, false);
        }
        device.activeInjectedButton = 0xFF;
        device.lastButtonState = false;
        device.lastHIDKeycode = 0x00;
        device.lastNormalizedPressed = false;
        device.pendingGameBrickRelease = false;
        device.pendingGameBrickReleaseMs = 0;
        device.pendingGameBrickKeycode = 0x00;
        device.pendingGameBrickButton = 0xFF;
        LOG_DBG("BT", "Game Brick: released deferred action button for %s", device.address.c_str());
      }

      const bool free2Profile = isFree2Profile(device.profile);
      if (device.activeInjectedButton != 0xFF) {
        bool releaseStaleButton = false;
        if (free2Profile) {
          // Fast path: Free2 often omits timely release frames.
          const unsigned long staleReleaseMs = inReaderContext ? FREE2_STALE_RELEASE_READER_MS
                                                               : FREE2_STALE_RELEASE_DEFAULT_MS;
          releaseStaleButton = (now - device.lastActivityTime) > staleReleaseMs;
        } else if (runMaintenance) {
          // Preserve the original slower stale-release maintenance for non-Free2 devices.
          releaseStaleButton = (now - device.lastActivityTime) > 250;
        }

        if (releaseStaleButton) {
          if (_buttonInjector) {
            _buttonInjector(_buttonInjectorCtx, device.activeInjectedButton, false);
          }
          device.activeInjectedButton = 0xFF;
          device.lastButtonState = false;
          device.lastHIDKeycode = 0x00;
          LOG_DBG("BT", "Released stale injected button for %s", device.address.c_str());
        }
      }

      if (runMaintenance && inactiveAddress.empty() && device.lastActivityTime != 0) {
        const unsigned long inactiveTime = now - device.lastActivityTime;
        if (inactiveTime > INACTIVITY_TIMEOUT_MS) {
          inactiveAddress = device.address;
          inactiveTimeMs = inactiveTime;
        }
      }
    }

    // Handle auto-reconnect from scan results without blocking the UI task.
    if (_scanning && _connectedDevices.empty() && !_bondedDeviceAddress.empty()) {
      for (const auto& dev : _discoveredDevices) {
        if (dev.address == _bondedDeviceAddress) {
          bondedFound = true;
          bondedAddressToConnect = _bondedDeviceAddress;
          break;
        }
      }
    }

    if (runMaintenance && _enabled) {
      const bool disconnectedIdle = !_scanning && _connectedDevices.empty();
      if (disconnectedIdle) {
        if (disconnectedIdleSince == 0) {
          disconnectedIdleSince = now;
        } else if ((now - disconnectedIdleSince) > DISCONNECTED_IDLE_DISABLE_MS) {
          shouldDisableIdleRadio = true;
        }
      } else {
        disconnectedIdleSince = 0;
      }
    }
  }

  if (!inactiveAddress.empty()) {
    LOG_INF("BT", "Device %s inactive for %lu ms, disconnecting", inactiveAddress.c_str(), inactiveTimeMs);
    disconnectFromDevice(inactiveAddress);
  }

  if (bondedFound) {
    LOG_INF("BT", "Bonded device %s found in scan, auto-connecting...", bondedAddressToConnect.c_str());
    stopScan();
    connectToDevice(bondedAddressToConnect);
  }

  if (shouldDisableIdleRadio) {
    LOG_INF("BT", "Bluetooth idle with no connected device; disabling radio to save battery");
    disable();
  }
}

void BluetoothHIDManager::checkAutoReconnect(bool userInputDetected) {
  if (!isEnabled()) {
    return;
  }

  static unsigned long lastReconnectCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  unsigned long now = millis();

  // Only check every 5 seconds to avoid hammering
  if (now - lastReconnectCheck < 5000) {
    return;
  }
  lastReconnectCheck = now;

  bool hasConnectedDevice = false;
  bool scanActive = false;
  std::string bondedDeviceAddress;

  {
    StateLock lock(_stateMutex);
    hasConnectedDevice = !_connectedDevices.empty();
    scanActive = _scanning;
    bondedDeviceAddress = _bondedDeviceAddress;
  }

  // Already connected.
  if (hasConnectedDevice) {
    // Already connected, nothing to do. Use DBG to avoid log spam.
    LOG_DBG("BT", "AutoReconnect skipped: already connected");
    return;
  }

  // Avoid reconnect storms.
  if (now - lastReconnectAttempt < 2000) {
    LOG_DBG("BT", "AutoReconnect skipped: cooldown active (%lu ms)", now - lastReconnectAttempt);
    return;
  }

  {
    StateLock lock(_stateMutex);

    // Remove stale disconnected clients from active list.
    for (auto it = _connectedDevices.begin(); it != _connectedDevices.end();) {
      if (!it->client || !it->client->isConnected()) {
        if (_buttonInjector && it->activeInjectedButton != 0xFF) {
          _buttonInjector(_buttonInjectorCtx, it->activeInjectedButton, false);
        }
        LOG_DBG("BT", "Pruning stale disconnected client entry: %s client=%p", it->address.c_str(), it->client);
        it = _connectedDevices.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Reconnect is user-driven while reading: require a local button event.
  if (!userInputDetected) {
    LOG_DBG("BT", "AutoReconnect skipped: no local user input");
    return;
  }

  lastReconnectAttempt = now;

  if (bondedDeviceAddress.empty()) {
    LOG_DBG("BT", "AutoReconnect skipped: no bonded device configured");
    return;
  }

  LOG_INF("BT", "Button activity detected while disconnected, starting scan to find bonded device %s",
          bondedDeviceAddress.c_str());

  if (!scanActive) {
    startScan(10000, true); // 10 second background scan for the bonded remote only
  }
}
