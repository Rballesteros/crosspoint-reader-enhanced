#pragma once

#include <Arduino.h>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include "DeviceProfiles.h"

// Forward declarations
class NimBLEClient;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

struct BluetoothDevice {
  std::string address;
  std::string name;
  int rssi;
  bool isHID = false;
  uint8_t addrType = 0;  // BLE_ADDR_PUBLIC; populated from advertisedDevice->getAddress().getType()
  uint16_t appearance = 0;     // BLE Appearance category (0 = unknown)
  uint16_t companyId = 0xFFFF; // Manufacturer Bluetooth SIG company ID (0xFFFF = none)
};

struct ConnectedDevice {
  std::string address;
  std::string name;
  NimBLEClient* client = nullptr;
  std::vector<NimBLERemoteCharacteristic*> reportChars;
  unsigned long connectedTime = 0;    // Timestamp when BLE link was established
  bool subscribed = false;
  unsigned long lastActivityTime = 0;  // Timestamp of last HID report received
  uint8_t lastHIDKeycode = 0x00;       // Track last keycode to detect press/release transitions
  unsigned long lastInjectionTime = 0; // Cooldown for button injection to prevent flooding
  uint8_t lastInjectedKeycode = 0x00;  // Track last injected key for smarter cooldown
  uint8_t activeInjectedButton = 0xFF; // Currently held virtual button, if any
  bool wasConnected = false;           // Track if this device was previously connected for auto-reconnect
  bool hasSeenRelease = false;         // Ignore startup noise until a release frame is seen
  bool lastButtonState = false;        // Track button pressed state (from byte[0])
  const DeviceProfiles::DeviceProfile* profile = nullptr;  // Device-specific HID profile
  bool simpleFallbackEnabled = false;
  uint8_t simpleForwardKeycode = 0x00;
  uint8_t simpleBackKeycode = 0x00;
  // Optional learned menu-action keycodes for clickers with more than two
  // buttons. 0x00 means "not learned" — leave page-nav behavior unchanged.
  uint8_t simpleConfirmKeycode = 0x00;
  uint8_t simpleCancelKeycode = 0x00;
  bool descriptorHasConsumerPage = false;
  bool descriptorHasKeyboardPage = false;
  uint8_t descriptorSuggestedIndex = 0xFF;
  unsigned long lastNormalizedEventMs = 0;
  uint8_t lastNormalizedKeycode = 0x00;
  bool lastNormalizedPressed = false;
  uint8_t lastNormalizedDirection = 0xFF;  // 0x00=back, 0x01=forward, 0xFF=unknown
  uint16_t lastGameBrickCounter = 0xFFFF;  // For counter-freeze detection (button vs joystick)
  uint8_t lastGameBrickActiveKey = 0x00;   // Latched first key per freeze-window (prevents overshoot misfires)
  uint8_t gameBrickCenterPressFrames = 0;  // Centered horizontal active-frame streak (LEFT fallback)
  bool pendingGameBrickRelease = false;    // Delay short A/B release tails so one long hold stays merged
  unsigned long pendingGameBrickReleaseMs = 0;
  uint8_t pendingGameBrickKeycode = 0x00;
  uint8_t pendingGameBrickButton = 0xFF;
};

class BluetoothHIDManager {
public:
  // Singleton access
  static BluetoothHIDManager& getInstance();

  static constexpr const char* ERROR_CONNECTION_FAILED = "BT_CONNECTION_FAILED";
  static constexpr const char* ERROR_CONNECTION_TIMEOUT = "BT_CONNECTION_TIMEOUT";
  static constexpr const char* ERROR_REMOTE_SLEEP_RETRY = "BT_REMOTE_SLEEP_RETRY";

  // Lifecycle
  bool enable();
  bool disable();
  bool isEnabled() const;

  // Scanning
  void startScan(uint32_t durationMs = 10000);
  void stopScan();
  bool isScanning() const;

  // Returns a thread-safe copy of the discovered devices
  std::vector<BluetoothDevice> getDiscoveredDevicesCopy() const;

  // Connection
  bool connectToDevice(const std::string& address, uint8_t addrTypeOverride = 0, bool useAddrTypeOverride = false);
  bool disconnectFromDevice(const std::string& address);
  bool isConnected(const std::string& address) const;
  std::vector<ConnectedDevice> getConnectedDevicesCopy() const;

  // Probe an advertised device to read its GATT Device Name (0x2A00) without
  // committing to a full HID connection. Updates the matching entry in
  // _discoveredDevices on success. Returns true if a non-empty name was read.
  bool identifyDevice(const std::string& address);

  // Input handling
  void processInputEvents();
  void setInputCallback(std::function<void(uint16_t keycode)> callback);
  void setLearnInputCallback(std::function<void(uint8_t keycode, uint8_t reportIndex)> callback);
  void setButtonInjector(std::function<void(uint8_t buttonIndex, bool pressed)> injector);
  void setReaderContextCallback(std::function<bool()> callback);
  void setButtonActivityNotifier(std::function<void(uint8_t buttonIndex)> notifier);
  void setDebugCaptureEnabled(bool enabled) { _debugCaptureEnabled = enabled; }
  bool isDebugCaptureEnabled() const { return _debugCaptureEnabled; }
  void setBondedDevice(const std::string& address, const std::string& name = "", uint8_t addrType = 0);
  void updateActivity();  // Call periodically to check inactivity timeout
  void checkAutoReconnect(bool userInputDetected = false);  // Reconnect bonded device when disconnected

  // Check if BLE has had activity recently (within last 4 minutes)
  // Used by power manager to prevent sleep during BLE use
  bool hasRecentActivity() const;
  bool hadRecentFree2Input(unsigned long windowMs = 1500) const;

  // State persistence
  void saveState();
  void loadState();

  std::string lastError;

  // BLE callbacks (public for NimBLE callbacks)
  void onScanResult(NimBLEAdvertisedDevice* advertisedDevice);
  void onScanEnded();
  void onClientDisconnected(const std::string& address, int reason);
  static void onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);

private:
  BluetoothHIDManager();
  ~BluetoothHIDManager();
  BluetoothHIDManager(const BluetoothHIDManager&) = delete;
  BluetoothHIDManager& operator=(const BluetoothHIDManager&) = delete;

  void cleanup();
  uint16_t parseHIDReport(uint8_t* data, size_t length);
  // Caller must hold _stateMutex while using the returned pointer.
  ConnectedDevice* findConnectedDevice(const std::string& address);
  uint8_t mapKeycodeToButton(uint8_t keycode, ConnectedDevice* device);

  bool _enabled = false;
  bool _scanning = false;
  std::vector<BluetoothDevice> _discoveredDevices;
  mutable SemaphoreHandle_t _stateMutex = nullptr;
  std::vector<ConnectedDevice> _connectedDevices;
  std::function<void(uint16_t)> _inputCallback;
  std::function<void(uint8_t, uint8_t)> _learnInputCallback;
  std::function<void(uint8_t, bool)> _buttonInjector;
  std::function<bool()> _readerContextCallback;
  std::function<void(uint8_t)> _buttonActivityNotifier;
  bool _debugCaptureEnabled = false;
  std::string _bondedDeviceAddress;
  std::string _bondedDeviceName;
  uint8_t _bondedDeviceAddrType = 0;  // BLE_ADDR_PUBLIC default
  char _lastDisconnectAddress[18] = {0};
  int _lastDisconnectReason = 0;
  unsigned long _lastDisconnectTime = 0;

  // Inactivity timeout (milliseconds)
  static constexpr unsigned long INACTIVITY_TIMEOUT_MS = 300000;  // 5 minutes
  unsigned long lastMaintenanceCheck = 0;
};
