#pragma once

#include <Arduino.h>

#include <map>
#include <string>
#include <vector>

#include "BleDecoders.h"
#include "BleMappings.h"

// Forward declarations
class NimBLEClient;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

struct BluetoothDevice {
  std::string address;
  std::string name;
  int rssi;
  bool isHID = false;
  uint8_t addrType = 0;         // BLE_ADDR_PUBLIC; populated from advertisedDevice->getAddress().getType()
  uint16_t appearance = 0;      // BLE Appearance category (0 = unknown)
  uint16_t companyId = 0xFFFF;  // Manufacturer Bluetooth SIG company ID (0xFFFF = none)
};

struct ConnectedDevice {
  std::string address;
  std::string name;
  NimBLEClient* client = nullptr;
  std::vector<NimBLERemoteCharacteristic*> reportChars;
  unsigned long connectedTime = 0;  // Timestamp when BLE link was established
  bool subscribed = false;
  unsigned long lastActivityTime = 0;   // Timestamp of last HID report received
  unsigned long lastInjectionTime = 0;  // Cooldown for button injection to prevent flooding
  uint8_t activeInjectedButton = 0xFF;  // Currently held virtual button, if any
  bool wasConnected = false;            // Track if this device was previously connected for auto-reconnect
  bool hasSeenRelease = false;          // Ignore startup noise until a release frame is seen

  // Decoder selection — what report format this device speaks. Chosen at
  // connect time via BleDecoders::matchProfile. Decoder-private state lives
  // in `decoderState` and is mutated by the decoder on every notify.
  BleDecoders::Kind decoderKind = BleDecoders::Kind::GenericExtract;
  BleDecoders::ReportHints reportHints;
  BleDecoders::State decoderState;
  const char* profileName = "Generic";

  // Per-device user-learned mapping (BleMappings store). When null, the
  // device produces no input until the user runs the learn wizard. There are
  // no hardcoded defaults — unmapped keycodes are a deliberate no-op.
  const BleMappings::Set* deviceMappings = nullptr;

  // Last decoded frame (for press-transition detection and the jitter
  // debounce window).
  uint8_t lastDecodedKeycode = 0x00;
  bool lastDecodedPressed = false;
  unsigned long lastDecodedEventMs = 0;
  uint8_t lastInjectedKeycode = 0x00;  // Track last injected key for smarter cooldown

  // Tracked for hadRecentRemoteInput() — feeds the reader's press-vs-release
  // page-turn heuristic.
  unsigned long lastNormalizedEventMs = 0;
  uint8_t lastNormalizedKeycode = 0x00;
  bool lastNormalizedPressed = false;
};

class BluetoothHIDManager {
 public:
  // Singleton access
  static BluetoothHIDManager& getInstance();

  static constexpr const char* ERROR_CONNECTION_FAILED = "BT_CONNECTION_FAILED";
  static constexpr const char* ERROR_CONNECTION_TIMEOUT = "BT_CONNECTION_TIMEOUT";
  static constexpr const char* ERROR_REMOTE_SLEEP_RETRY = "BT_REMOTE_SLEEP_RETRY";

  // Boot-time preference: whether the BT controller's static DRAM (~30-40KB)
  // is reserved at boot. Default is "released" — it saves DRAM for users who
  // never pair a remote. Toggling this requires a reboot to take effect
  // because esp_bt_controller_mem_release() is one-way: once Arduino's
  // initArduino() releases the memory, esp_bt_controller_init() can't bring
  // it back. The current process gets memory reserved only if it was
  // reserved at the previous boot.
  static bool isAvailableAtBoot();
  static bool setAvailableAtBoot(bool value);

  // Lifecycle
  bool enable();
  bool disable();
  bool isEnabled() const;

  // Scanning
  void startScan(uint32_t durationMs = 10000, bool bondedOnly = false);
  void stopScan();
  bool isScanning() const;

  // Returns a thread-safe copy of the discovered devices
  std::vector<BluetoothDevice> getDiscoveredDevicesCopy() const;

  // Connection
  bool connectToDevice(const std::string& address, uint8_t addrTypeOverride = 0, bool useAddrTypeOverride = false);
  bool disconnectFromDevice(const std::string& address);
  bool isConnected(const std::string& address) const;
  bool hasConnectedDevice() const;
  std::vector<ConnectedDevice> getConnectedDevicesCopy() const;
  // Re-resolves the BleMappings::Set pointer on every connected device so
  // freshly saved/cleared mappings take effect on the live connection,
  // without waiting for a disconnect/reconnect cycle.
  void refreshDeviceMappings();

  // Probe an advertised device to read its GATT Device Name (0x2A00) without
  // committing to a full HID connection. Updates the matching entry in
  // _discoveredDevices on success. Returns true if a non-empty name was read.
  bool identifyDevice(const std::string& address);

  // Input handling. Callbacks are raw fn-ptr + ctx pairs (instead of
  // std::function) to avoid the ~2-4 KB heap/binary cost per closure on the
  // memory-constrained ESP32-C3.
  using InputCb = void (*)(void* ctx, uint16_t keycode);
  using LearnInputCb = void (*)(void* ctx, uint8_t keycode, uint8_t reportIndex);
  using DebugInputCb = void (*)(void* ctx, uint8_t keycode, uint8_t reportIndex, uint8_t mappedButton, bool pressed,
                                const uint8_t* raw, uint8_t rawLength);
  using ButtonInjectorCb = void (*)(void* ctx, uint8_t buttonIndex, bool pressed);
  using ReaderContextCb = bool (*)(void* ctx);
  // Called on every BLE notify while a virtual button stays held by the same
  // key, so HalGPIO can keep getHeldTime() pinned to BLE-observed activity
  // rather than wall-clock. Without this, a BLE remote that drops release
  // frames would let held-time grow unbounded between notifies.
  using ButtonActivityCb = void (*)(void* ctx, uint8_t buttonIndex);

  void processInputEvents();
  void setInputCallback(InputCb callback, void* ctx);
  void setLearnInputCallback(LearnInputCb callback, void* ctx);
  void setDebugInputCallback(DebugInputCb callback, void* ctx);
  void setButtonInjector(ButtonInjectorCb injector, void* ctx);
  void setReaderContextCallback(ReaderContextCb callback, void* ctx);
  void setButtonActivityNotifier(ButtonActivityCb notifier, void* ctx);
  void setDebugCaptureEnabled(bool enabled) { _debugCaptureEnabled = enabled; }
  bool isDebugCaptureEnabled() const { return _debugCaptureEnabled; }
  void setBondedDevice(const std::string& address, const std::string& name = "", uint8_t addrType = 0);
  void updateActivity();  // Call periodically to check inactivity timeout
  unsigned long lastDisconnectTime() const;
  void checkAutoReconnect(bool userInputDetected = false);  // Reconnect bonded device when disconnected

  // Check if BLE has had activity recently (within last 4 minutes)
  // Used by power manager to prevent sleep during BLE use
  bool hasRecentActivity() const;
  // True if any connected BLE remote produced input in the last `windowMs`.
  // Used by reader logic to prefer press-driven nav and disable long-press
  // chapter skip when BLE remotes (which have unreliable release timing) are
  // in use.
  bool hadRecentRemoteInput(unsigned long windowMs = 1500) const;

  // Note: bonded-device persistence lives at the SETTINGS layer
  // (CrossPointSettings::bleBondedDevice*); this manager holds only the
  // in-memory snapshot pushed in via setBondedDevice().

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
  // Caller must hold _stateMutex while using the returned pointer.
  ConnectedDevice* findConnectedDevice(const std::string& address);

  bool _enabled = false;
  bool _scanning = false;
  unsigned long _lastScanEndTime = 0;
  std::vector<BluetoothDevice> _discoveredDevices;
  bool _bondedOnlyScan = false;
  mutable SemaphoreHandle_t _stateMutex = nullptr;
  std::vector<ConnectedDevice> _connectedDevices;
  InputCb _inputCallback = nullptr;
  void* _inputCallbackCtx = nullptr;
  LearnInputCb _learnInputCallback = nullptr;
  void* _learnInputCallbackCtx = nullptr;
  DebugInputCb _debugInputCallback = nullptr;
  void* _debugInputCallbackCtx = nullptr;
  ButtonInjectorCb _buttonInjector = nullptr;
  void* _buttonInjectorCtx = nullptr;
  ReaderContextCb _readerContextCallback = nullptr;
  void* _readerContextCallbackCtx = nullptr;
  ButtonActivityCb _buttonActivityNotifier = nullptr;
  void* _buttonActivityNotifierCtx = nullptr;
  bool _debugCaptureEnabled = false;
  std::string _bondedDeviceAddress;
  std::string _bondedDeviceName;
  uint8_t _bondedDeviceAddrType = 0;  // BLE_ADDR_PUBLIC default
  char _lastDisconnectAddress[18] = {0};
  int _lastDisconnectReason = 0;
  unsigned long _lastDisconnectTime = 0;

  // Inactivity timeout (milliseconds)
  static constexpr unsigned long INACTIVITY_TIMEOUT_MS = 300000;        // 5 minutes
  static constexpr unsigned long DISCONNECTED_IDLE_DISABLE_MS = 60000;  // 1 minute
  unsigned long lastMaintenanceCheck = 0;
  unsigned long disconnectedIdleSince = 0;
};
