#include "BluetoothHIDManager.h"

#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <nvs.h>

#include <algorithm>
#include <cstring>

#include "../../src/CrossPointSettings.h"

namespace {
// NVS key controlling whether BT controller static memory is reserved at
// boot. Lives in the same "crosspoint" namespace HalSystem/HalGPIO use.
constexpr const char* kBtNvsNamespace = "crosspoint";
constexpr const char* kBtAvailableKey = "bt_avail";
}  // namespace

// Strong override for Arduino-ESP32's weak btInUse() symbol. The framework
// calls this from initArduino() (after nvs_flash_init(), before app_main
// resumes) to decide whether to keep the BT controller's static DRAM
// allocated. Returning false lets initArduino() call
// esp_bt_controller_mem_release(ESP_BT_MODE_BTDM), which reclaims ~30-40KB
// of .bt_bss/.bt_data into the heap. That release is one-way, so a runtime
// re-enable requires a reboot with the NVS pref flipped.
//
// We deliberately do NOT include <esp32-hal-bt-mem.h> here. That header's
// constructor would set _btLibraryInUse=true unconditionally before
// btInUse() is even consulted, locking memory regardless of user choice.
extern "C" bool btInUse() {
  nvs_handle_t handle;
  if (nvs_open(kBtNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  uint8_t value = 0;
  nvs_get_u8(handle, kBtAvailableKey, &value);
  nvs_close(handle);
  return value != 0;
}

// HID Service and characteristic UUIDs
static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2A4D";
static const char* HID_INFO_UUID = "2A4A";
static const char* HID_REPORT_MAP_UUID = "2A4B";
static const char* HID_PROTOCOL_MODE_UUID = "2A4E";

namespace {
// BLE intervals are in 1.25ms units and timeout is in 10ms units.
// Keep latency at 0 for low input lag while allowing a longer supervision timeout
// to reduce disconnects at marginal range.
constexpr uint16_t BLE_CONN_MIN_INTERVAL = 12;  // 15ms
constexpr uint16_t BLE_CONN_MAX_INTERVAL = 24;  // 30ms
constexpr uint16_t BLE_CONN_LATENCY = 0;
constexpr uint16_t BLE_CONN_TIMEOUT = 600;  // 6s
constexpr uint16_t BLE_CONN_SCAN_INTERVAL = 60;
constexpr uint16_t BLE_CONN_SCAN_WINDOW = 30;
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 4000;
constexpr size_t MAX_DISCOVERED_DEVICES = 24;
constexpr unsigned long RECENT_DISCONNECT_WINDOW_MS = 15000;

// Stale-release maintenance for decoders that don't advertise a per-kind
// timeout (GenericExtract). Catches devices that drop the release frame
// outright; long enough not to interfere with normal long-press gestures.
constexpr unsigned long GENERIC_STALE_RELEASE_MS = 250;

// Jitter debounce: BLE remotes can emit a fake release/press doublet on a
// single physical click. Same-key re-presses inside this window collapse to
// a continuation.
constexpr unsigned long JITTER_DEBOUNCE_MS = 90;

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

bool isHIDAppearance(const uint16_t appearance) { return appearance >= 0x03C0 && appearance <= 0x03FF; }

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

// Parse the device's HID Report Map descriptor. The result feeds the
// GenericExtract decoder as a hint about which byte to read first.
BleDecoders::ReportHints parseDescriptorHints(const std::string& map) {
  BleDecoders::ReportHints hints;
  if (map.empty()) {
    return hints;
  }

  for (size_t i = 0; i + 1 < map.size(); i++) {
    const uint8_t b = static_cast<uint8_t>(map[i]);
    const uint8_t next = static_cast<uint8_t>(map[i + 1]);
    if (b == 0x05) {  // Usage Page (1 byte value)
      if (next == 0x0C) {
        hints.hasConsumerPage = true;
      } else if (next == 0x07) {
        hints.hasKeyboardPage = true;
      }
    }
  }

  // Keyboard-like reports place the keycode at byte[2]; consumer-control
  // reports are compact and tend to put keycode-like values at byte[1].
  if (hints.hasKeyboardPage) {
    hints.preferredByteIndex = 2;
  } else if (hints.hasConsumerPage) {
    hints.preferredByteIndex = 1;
  }

  return hints;
}

}  // namespace

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

static ScanCallbacks scanCallbacks;

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

bool BluetoothHIDManager::isAvailableAtBoot() {
  // BT memory availability is decided once, at boot, by Arduino's call to
  // btInUse() before app_main resumes. Toggling the NVS pref mid-session does
  // not change the actual memory state until the next reboot, so cache the
  // boot snapshot and answer subsequent calls from RAM. Avoids opening NVS
  // on every reader-loop pass through main.cpp.
  static int cached = -1;
  if (cached < 0) {
    Preferences prefs;
    if (!prefs.begin(kBtNvsNamespace, true)) {
      // NVS transiently unavailable — don't poison the cache.
      return false;
    }
    cached = prefs.getUChar(kBtAvailableKey, 0) != 0 ? 1 : 0;
    prefs.end();
  }
  return cached != 0;
}

bool BluetoothHIDManager::setAvailableAtBoot(bool value) {
  Preferences prefs;
  if (!prefs.begin(kBtNvsNamespace, false)) {
    LOG_ERR("BT", "Failed to open NVS namespace '%s' for write", kBtNvsNamespace);
    return false;
  }
  const size_t written = prefs.putUChar(kBtAvailableKey, value ? 1 : 0);
  prefs.end();
  if (written == 0) {
    LOG_ERR("BT", "Failed to write %s=%d to NVS", kBtAvailableKey, value ? 1 : 0);
    return false;
  }
  LOG_INF("BT", "BT boot pref set to %d (reboot required to take effect)", value ? 1 : 0);
  return true;
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

void BluetoothHIDManager::refreshDeviceMappings() {
  StateLock lock(_stateMutex);
  for (auto& device : _connectedDevices) {
    device.deviceMappings = BleMappings::get(device.address);
  }
}

bool BluetoothHIDManager::enable() {
  HalPowerManager::Lock powerLock;
  if (isEnabled()) {
    LOG_DBG("BT", "Already enabled");
    return true;
  }

  // BT controller memory was released at boot. NimBLEDevice::init() would
  // fail (or worse, run on top of memory now owned by the heap allocator).
  // Bail with a clear error before we touch the stack.
  if (!isAvailableAtBoot()) {
    LOG_ERR("BT", "Cannot enable: BT controller memory released at boot");
    lastError = "BT memory released — enable 'BT at Boot' and restart";
    return false;
  }

  LOG_INF("BT", "Enabling Bluetooth...");

  // Keep this firmware path to one active radio stack at a time so the heap
  // budget and radio state remain predictable on the ESP32-C3.
  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BT", "Disabling WiFi before Bluetooth startup");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  NimBLEDevice::init("CrossPoint");
  delay(20);
  // Pin local ATT MTU to the BLE minimum (23 bytes). HID Reports are <=20 bytes,
  // so larger MTUs only allocate oversized ATT buffers per connection.
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
    _discoveredDevices.reserve(MAX_DISCOVERED_DEVICES);
  }

  LOG_INF("BT", "Starting BLE scan for %lu ms%s", durationMs, bondedOnly ? " (bonded remote only)" : " (non-blocking)");

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
  pScan->setMaxResults(0);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  if (bondedOnly && !_bondedDeviceAddress.empty()) {
    NimBLEDevice::whiteListAdd(NimBLEAddress(_bondedDeviceAddress, _bondedDeviceAddrType));
    pScan->setFilterPolicy(BLE_HCI_SCAN_FILT_USE_WL);
  } else {
    pScan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
  }

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

  bool isHID = advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));

  uint16_t appearance = 0;
  if (advertisedDevice->haveAppearance()) {
    appearance = advertisedDevice->getAppearance();
  }
  uint16_t companyId = 0xFFFF;
  if (advertisedDevice->haveManufacturerData()) {
    const std::string mfg = advertisedDevice->getManufacturerData();
    if (mfg.size() >= 2) {
      companyId = static_cast<uint16_t>(static_cast<uint8_t>(mfg[0])) |
                  (static_cast<uint16_t>(static_cast<uint8_t>(mfg[1])) << 8);
    }
  }

  StateLock lock(_stateMutex);
  bool found = false;
  for (auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      dev.rssi = rssi;
      if (!name.empty() && isPlaceholderDeviceName(dev.name)) {
        dev.name = name;
      }
      if (isHID) dev.isHID = true;
      dev.addrType = advAddrType;
      if (appearance != 0) dev.appearance = appearance;
      if (companyId != 0xFFFF) dev.companyId = companyId;
      std::sort(_discoveredDevices.begin(), _discoveredDevices.end(), compareDiscoveredDevice);
      found = true;
      break;
    }
  }

  if (!found) {
    BluetoothDevice device;
    device.address = address;
    if (!name.empty()) {
      device.name = name;
    } else if (address.size() >= 8) {
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
      // Bonded device found — let updateActivity() on the main task pick this
      // up via the `_scanning && discovered-list-contains-bonded` branch and
      // call stopScan()/connectToDevice() from there.
      //
      // DO NOT call NimBLEDevice::getScan()->stop() here: stop() triggers
      // NimBLEScan::clearResults(), which deletes every NimBLEAdvertisedDevice
      // in m_scanResults — including the pointer NimBLE passed us. Our
      // PayloadClearGuard destructor would then write to freed memory at
      // scope exit, corrupting heap metadata. See clearPayload() in
      // NimBLE-Arduino: "Not thread-safe with concurrent scan callbacks".
      LOG_INF("BT", "Bonded device found during scan; deferring stop+connect to main task");
    }

    if (_discoveredDevices.size() >= MAX_DISCOVERED_DEVICES) {
      if (isBondedDevice || compareDiscoveredDevice(device, _discoveredDevices.back())) {
        _discoveredDevices.back() = std::move(device);
        std::sort(_discoveredDevices.begin(), _discoveredDevices.end(), compareDiscoveredDevice);
      }
    } else {
      _discoveredDevices.push_back(std::move(device));
      std::sort(_discoveredDevices.begin(), _discoveredDevices.end(), compareDiscoveredDevice);
    }
  }

  LOG_DBG("BT", "Found device: %s (%s) RSSI:%d HID:%d type:%u app:0x%04X mfg:0x%04X", address.c_str(), address.c_str(),
          rssi, isHID, advAddrType, appearance, companyId);
}

bool BluetoothHIDManager::connectToDevice(const std::string& address, uint8_t addrTypeOverride,
                                          bool useAddrTypeOverride) {
  HalPowerManager::Lock powerLock;
  if (!isEnabled()) {
    LOG_ERR("BT", "Cannot connect: Bluetooth not enabled");
    lastError = "Bluetooth not enabled";
    return false;
  }

  if (isConnected(address)) {
    LOG_INF("BT", "Already connected to %s", address.c_str());
    return true;
  }

  if (isScanning()) {
    stopScan();
  }

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

  LOG_INF("BT", "Connecting to device %s (addrType=%u, resolved=%d)", address.c_str(), resolvedAddrType,
          addrTypeFound ? 1 : 0);

  NimBLEAddress bleAddress(address, resolvedAddrType);

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

  static ClientCallbacks clientCallbacks;

  pClient->setSelfDelete(false, false);
  pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
  pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT,
                               BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);
  pClient->setClientCallbacks(&clientCallbacks, false);

  if (!pClient->isConnected() && !hadExistingClient) {
    pClient->deleteServices();
  }

  if (!pClient->connect(bleAddress, false, false, false)) {
    if (hadExistingClient) {
      LOG_INF("BT", "Reconnect with existing client failed for %s, retrying with fresh client", address.c_str());
      NimBLEClient* freshClient = NimBLEDevice::createClient(bleAddress);
      if (freshClient) {
        pClient = freshClient;
        pClient->setSelfDelete(false, false);
        pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
        pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT,
                                     BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);
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
      LOG_ERR("BT", "Failed to connect to %s (err=%d, recentDisconnect=%d, %s)", address.c_str(), clientError,
              recentDisconnectReason, describeDisconnectReason(reportedReason));
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

  LOG_INF("BT", "Initiating pairing/security...");
  if (!pClient->secureConnection()) {
    LOG_ERR("BT", "Failed to secure connection (proceeding anyway)");
  }

  NimBLERemoteService* pService = pClient->getService(HID_SERVICE_UUID);
  if (!pService) {
    lastError = "HID service not found";
    LOG_ERR("BT", "Device %s doesn't have HID service!", address.c_str());
    LOG_INF("BT", "Dumping available services for %s:", address.c_str());
    auto services = pClient->getServices(true);
    for (auto* s : services) {
      LOG_INF("BT", " - Found Service UUID: %s", s->getUUID().toString().c_str());
    }
    pClient->disconnect();
    return false;
  }

  if (auto* pProtocolMode = pService->getCharacteristic(HID_PROTOCOL_MODE_UUID)) {
    if (pProtocolMode->canWrite() || pProtocolMode->canWriteNoResponse()) {
      uint8_t reportMode = 0x01;
      const bool protocolSet = pProtocolMode->writeValue(&reportMode, 1, false);
      LOG_INF("BT", "Protocol mode write (Report=0x01): %d", protocolSet);
    }
  }

  BleDecoders::ReportHints descriptorHints;
  if (auto* pReportMap = pService->getCharacteristic(HID_REPORT_MAP_UUID)) {
    if (pReportMap->canRead()) {
      std::string reportMap = pReportMap->readValue();
      descriptorHints = parseDescriptorHints(reportMap);
      LOG_INF("BT", "Report map hints: keyboard=%d consumer=%d preferredByte=%d len=%u",
              descriptorHints.hasKeyboardPage, descriptorHints.hasConsumerPage,
              static_cast<int>(descriptorHints.preferredByteIndex), static_cast<unsigned>(reportMap.size()));
    }
  }

  LOG_INF("BT", "Found HID service, enumerating report characteristics...");

  auto pCharacteristics = pService->getCharacteristics(true);
  int reportCount = 0;
  std::vector<NimBLERemoteCharacteristic*> reportChars;
  reportChars.reserve(pCharacteristics.size());

  for (auto* pChar : pCharacteristics) {
    LOG_DBG("BT", "Characteristic UUID: %s, canRead:%d canWrite:%d canNotify:%d canIndicate:%d",
            pChar->getUUID().toString().c_str(), pChar->canRead(), pChar->canWrite(), pChar->canNotify(),
            pChar->canIndicate());

    if (pChar->getUUID().equals(NimBLEUUID(HID_REPORT_UUID))) {
      reportCount++;
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

  LOG_INF("BT", "Subscribing to %d Report characteristics...", reportChars.size());
  size_t successfulSubscriptions = 0;

  for (size_t i = 0; i < reportChars.size(); i++) {
    auto* pChar = reportChars[i];
    (void)pChar->unsubscribe();

    const bool useNotify = pChar->canNotify();
    bool subResult = pChar->subscribe(useNotify, onHIDNotify);
    LOG_INF("BT", "Report char #%d subscribe (%s) result: %d", i + 1, useNotify ? "notify" : "indicate", subResult);
    if (subResult) {
      successfulSubscriptions++;
    } else {
      LOG_INF("BT", "Failed to subscribe to Report char #%d (continuing)", i + 1);
    }
  }

  if (successfulSubscriptions == 0) {
    lastError = "Failed to subscribe to input reports";
    LOG_ERR("BT", "No HID report subscriptions succeeded for %s", address.c_str());
    pClient->disconnect();
    return false;
  }

  LOG_INF("BT", "Subscribed to %u/%u HID Report characteristics", static_cast<unsigned>(successfulSubscriptions),
          static_cast<unsigned>(reportChars.size()));

  ConnectedDevice connDev;
  connDev.address = address;
  connDev.client = pClient;
  connDev.reportChars = reportChars;
  connDev.connectedTime = millis();
  connDev.subscribed = true;
  connDev.lastActivityTime = millis();
  connDev.wasConnected = true;

  if (foundInScan) {
    connDev.name = scannedName;
    LOG_INF("BT", "Device found in scan results: %s (%s)", scannedName.c_str(), address.c_str());
  } else {
    LOG_INF("BT", "Device not in scan results (may be previously paired): %s", address.c_str());
    if (connDev.name.empty() && !bondedName.empty()) {
      connDev.name = bondedName;
      LOG_INF("BT", "Using bonded device name hint: %s", connDev.name.c_str());
    }
  }

  // If the advertisement didn't include a name (or it's our MAC-suffix placeholder),
  // try the GATT Device Name characteristic (0x2A00 in the Generic Access service 0x1800).
  const bool nameIsPlaceholder = connDev.name.empty() || connDev.name == "Unknown" || connDev.name.rfind("?-", 0) == 0;
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

  // Decoder selection. Pure parsing config — no keycode-to-action mapping
  // happens here. MAC prefix first (precise), then fuzzy name match, then
  // GenericExtract for everything else.
  const auto profileMatch = BleDecoders::matchProfile(address.c_str(), connDev.name.c_str());
  connDev.decoderKind = profileMatch.kind;
  connDev.profileName = profileMatch.name;
  connDev.reportHints = descriptorHints;
  if (profileMatch.kind != BleDecoders::Kind::GenericExtract) {
    // Device-specific decoders bake the report byte index into their
    // implementation; surface it on the hints so the wizard can show it.
    connDev.reportHints.preferredByteIndex = profileMatch.reportByteIndex;
  }

  // User-learned mapping. Nullptr means the wizard hasn't run for this
  // device yet — onHIDNotify will silently drop input until the user maps
  // their buttons. No hardcoded defaults.
  connDev.deviceMappings = BleMappings::get(address);

  LOG_INF("BT", "Decoder=%s byte[%u]  mapping=%s", profileMatch.name,
          static_cast<unsigned>(connDev.reportHints.preferredByteIndex),
          connDev.deviceMappings ? "loaded from wizard" : "NONE — needs wizard");

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
    _connectedDevices.erase(it);
  }

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

  if (isScanning()) {
    stopScan();
  }

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
  const bool reusedClient = (pClient != nullptr);
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
  pClient->setConnectTimeout(5000);

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

  if (!reusedClient) {
    NimBLEDevice::deleteClient(pClient);
  }

  if (discoveredName.empty()) {
    LOG_INF("BT", "Identify: %s did not expose a GATT name", address.c_str());
    return false;
  }

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
  bool connected =
      std::find_if(_connectedDevices.begin(), _connectedDevices.end(), [&address](const ConnectedDevice& dev) {
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
  // Input events arrive via NimBLE notifications. This method exists for
  // potential polling-based implementations and is intentionally empty.
}

// All callback setters lock the state mutex. The callbacks themselves are
// invoked from the BLE notification path on the NimBLE stack task, so a torn
// std::function write during reassignment could in principle expose a bad
// invocable. Guarding the assignment here keeps the swap atomic relative to
// other state-mutex holders.
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
  // stay "pressed" while the press-side injection is suppressed below,
  // leaving the activity to consume a phantom press.
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
  // Prevents NimBLE host/controller timeouts (HCI ACK wait) at 10MHz.
  if (_scanning || (now - _lastScanEndTime < 5000)) {
    return true;
  }

  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      const unsigned long timeSinceActivity = now - device.lastActivityTime;
      if (timeSinceActivity < 240000) {  // 4 minute threshold
        return true;
      }
    }
  }
  return false;
}

bool BluetoothHIDManager::hadRecentRemoteInput(unsigned long windowMs) const {
  const unsigned long now = millis();
  StateLock lock(_stateMutex);
  for (const auto& device : _connectedDevices) {
    if (device.lastNormalizedEventMs == 0 || (now - device.lastNormalizedEventMs) > windowMs) {
      continue;
    }
    if (device.activeInjectedButton != 0xFF || device.lastNormalizedKeycode != 0x00) {
      return true;
    }
  }
  return false;
}

// HID notification handler — the BLE -> button-injection hot path.
//
// Flow:
//   1. Find the connected device for the notify's peer address.
//   2. Run the device's decoder (BleDecoders::decode) to turn the raw report
//      bytes into a stable {keycode, pressed} pair.
//   3. Gate startup noise: ignore press frames until a clean release is seen.
//   4. Detect press transitions and apply a short jitter debounce.
//   5. Dispatch to learn callbacks (wizard) and the raw input callback.
//   6. Look up the keycode in BleMappings — the user's learned mapping.
//      No mapping → no injection. There are NO hardcoded defaults.
//   7. Inject the resulting button, refresh held-time accounting on
//      continuations, drop the held button on release.
void BluetoothHIDManager::onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (!g_instance || !pData || length == 0) return;

  std::string deviceAddr;
  if (pChar && pChar->getRemoteService()) {
    auto client = pChar->getRemoteService()->getClient();
    if (client) {
      deviceAddr = client->getPeerAddress().toString();
    }
  }
  if (deviceAddr.empty()) return;

  StateLock lock(g_instance->_stateMutex);
  ConnectedDevice* device = g_instance->findConnectedDevice(deviceAddr);
  if (!device) return;

  const unsigned long nowMs = millis();
  device->lastActivityTime = nowMs;

  if (g_instance->_debugCaptureEnabled) {
    char rawBuf[128] = {0};
    size_t offset = 0;
    const size_t dumpLen = length < 8 ? length : 8;
    for (size_t i = 0; i < dumpLen && offset + 4 < sizeof(rawBuf); i++) {
      offset += snprintf(rawBuf + offset, sizeof(rawBuf) - offset, "%02X ", static_cast<unsigned>(pData[i]));
    }
    LOG_INF("BTDBG", "addr=%s len=%u raw=%s", device->address.c_str(), static_cast<unsigned>(length), rawBuf);
  }

  // 1-2. Decode the raw HID report.
  const BleDecoders::DecodedFrame frame =
      BleDecoders::decode(device->decoderKind, pData, length, device->decoderState, device->reportHints);

  // 3. Startup noise gate. After subscribe, the very first frame is often a
  // stale "pressed" snapshot. Wait for a release before honoring any press,
  // except for decoders that may never emit a clean release at subscribe
  // time (Free2/GameBrick), where we arm on the first valid press instead.
  if (!device->hasSeenRelease) {
    if (!frame.pressed) {
      device->hasSeenRelease = true;
    } else if (device->decoderKind == BleDecoders::Kind::Free2Rolling ||
               device->decoderKind == BleDecoders::Kind::GameBrickV2) {
      device->hasSeenRelease = true;
      LOG_DBG("BT", "Arming on first valid press for %s keycode=0x%02X", device->profileName, frame.keycode);
    } else {
      device->lastDecodedKeycode = frame.keycode;
      device->lastDecodedPressed = frame.pressed;
      return;
    }
  }

  // 4. Press transition detection.
  bool isNewPress = frame.pressed && (!device->lastDecodedPressed || frame.keycode != device->lastDecodedKeycode);

  // 4b. 90ms jitter debounce: same key going pressed within 90ms of the last
  // same-key event is treated as a continuation, not a new press.
  if (isNewPress && frame.keycode == device->lastDecodedKeycode &&
      (nowMs - device->lastDecodedEventMs) < JITTER_DEBOUNCE_MS) {
    isNewPress = false;
    if (g_instance->_debugCaptureEnabled) {
      LOG_INF("BTDBG", "Suppressed jitter duplicate key=0x%02X dt=%lu", frame.keycode,
              nowMs - device->lastDecodedEventMs);
    }
  }

  // 5. Wizard / raw input callback dispatch.
  if (isNewPress && frame.keycode != 0 && frame.reportIndex != 0xFF) {
    if (g_instance->_learnInputCallback) {
      g_instance->_learnInputCallback(g_instance->_learnInputCallbackCtx, frame.keycode, frame.reportIndex);
    }
    if (g_instance->_inputCallback) {
      g_instance->_inputCallback(g_instance->_inputCallbackCtx, frame.keycode);
    }
  }
  const bool learnModeActive = (g_instance->_learnInputCallback != nullptr);

  // Track normalized events so the reader can prefer press-driven nav.
  if (frame.pressed && frame.keycode != 0) {
    device->lastNormalizedEventMs = nowMs;
    device->lastNormalizedKeycode = frame.keycode;
    device->lastNormalizedPressed = true;
  } else if (!frame.pressed) {
    device->lastNormalizedPressed = false;
  }

  // 6. Look up the mapped action. No mapping → no injection.
  uint8_t targetButton = 0xFF;
  if (frame.pressed && device->deviceMappings) {
    const BleMappings::Action action = device->deviceMappings->lookup(frame.keycode);
    if (action != BleMappings::Action::None) {
      targetButton = BleMappings::actionToButtonIndex(action);
    }
  }

  if (g_instance->_debugCaptureEnabled) {
    LOG_INF("BTDBG", "decoded key=0x%02X idx=%u pressed=%u mapped=%s", frame.keycode,
            static_cast<unsigned>(frame.reportIndex), frame.pressed ? 1 : 0, debugButtonName(targetButton));
    if (g_instance->_debugInputCallback) {
      g_instance->_debugInputCallback(g_instance->_debugInputCallbackCtx, frame.keycode, frame.reportIndex,
                                      targetButton, frame.pressed, pData,
                                      static_cast<uint8_t>(length < 8 ? length : 8));
    }
  }

  // 7. Inject.
  if (!frame.pressed || targetButton == 0xFF || learnModeActive) {
    // Release path: drop any held button. Unmapped keycodes are a no-op.
    if (g_instance->_buttonInjector && device->activeInjectedButton != 0xFF) {
      g_instance->_buttonInjector(g_instance->_buttonInjectorCtx, device->activeInjectedButton, false);
      device->activeInjectedButton = 0xFF;
    }
  } else if (isNewPress) {
    // New press: release any old held button first, then press the new one.
    if (device->activeInjectedButton != 0xFF && device->activeInjectedButton != targetButton &&
        g_instance->_buttonInjector) {
      g_instance->_buttonInjector(g_instance->_buttonInjectorCtx, device->activeInjectedButton, false);
      device->activeInjectedButton = 0xFF;
    }
    if (g_instance->_buttonInjector && device->activeInjectedButton == 0xFF) {
      g_instance->_buttonInjector(g_instance->_buttonInjectorCtx, targetButton, true);
      device->activeInjectedButton = targetButton;
      device->lastInjectionTime = nowMs;
      device->lastInjectedKeycode = frame.keycode;
      if (g_instance->_buttonActivityNotifier) {
        g_instance->_buttonActivityNotifier(g_instance->_buttonActivityNotifierCtx, targetButton);
      }
    }
  } else if (device->activeInjectedButton == targetButton && g_instance->_buttonActivityNotifier) {
    // Continuation: same physical button still held. Update held-time
    // accounting so getHeldTime() tracks BLE-observed activity instead of
    // wall-clock between notifies.
    g_instance->_buttonActivityNotifier(g_instance->_buttonActivityNotifierCtx, targetButton);
  }

  device->lastDecodedKeycode = frame.keycode;
  device->lastDecodedPressed = frame.pressed;
  device->lastDecodedEventMs = nowMs;
}

void BluetoothHIDManager::updateActivity() {
  if (!_enabled) return;
  {
    StateLock lock(_stateMutex);
    if (_connectedDevices.empty() && !_scanning && disconnectedIdleSince == 0) {
      return;
    }
  }

  const unsigned long now = millis();
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

    for (auto& device : _connectedDevices) {
      // Synthesize a release for any decoder that didn't emit one. The
      // timeout is decoder-specific: 0 means "trust the device", anything
      // else is the maximum quiet window before we drop the held button.
      if (device.activeInjectedButton != 0xFF) {
        const unsigned long decoderTimeoutMs = BleDecoders::staleReleaseTimeoutMs(device.decoderKind, inReaderContext);
        unsigned long timeoutMs = decoderTimeoutMs;
        if (timeoutMs == 0 && runMaintenance) {
          // Generic devices: long catch-all only on maintenance ticks.
          timeoutMs = GENERIC_STALE_RELEASE_MS;
        }
        if (timeoutMs > 0 && (now - device.lastActivityTime) > timeoutMs) {
          if (_buttonInjector) {
            _buttonInjector(_buttonInjectorCtx, device.activeInjectedButton, false);
          }
          device.activeInjectedButton = 0xFF;
          device.lastDecodedPressed = false;
          device.lastDecodedKeycode = 0x00;
          LOG_DBG("BT", "Released stale injected button for %s (decoder=%s, idle=%lu ms)", device.address.c_str(),
                  device.profileName, now - device.lastActivityTime);
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
  const unsigned long now = millis();

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

  if (hasConnectedDevice) {
    return;
  }

  if (now - lastReconnectAttempt < 2000) {
    LOG_DBG("BT", "AutoReconnect skipped: cooldown active (%lu ms)", now - lastReconnectAttempt);
    return;
  }

  {
    StateLock lock(_stateMutex);
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
    startScan(10000, true);
  }
}
