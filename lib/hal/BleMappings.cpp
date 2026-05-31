#include "BleMappings.h"

#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <map>

#include "../../src/CrossPointSettings.h"

namespace BleMappings {

namespace {

constexpr const char* MAPPINGS_FILE = "/.crosspoint/ble_mappings.txt";

// In-memory cache mirrors the on-disk file. Lazily loaded on first access.
// Keyed by lowercase MAC address so callers don't need to canonicalize.
std::map<std::string, Set> g_cache;
bool g_loaded = false;

std::string normalizeAddress(const std::string& address) {
  std::string out = address;
  for (auto& ch : out) {
    if (ch >= 'A' && ch <= 'F') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

bool isKnownAction(uint8_t raw) {
  // Lower bound (Action::None == 0) is implicit for an unsigned value.
  return raw <= static_cast<uint8_t>(Action::Right);
}

void writeFileFromCache() {
  Storage.mkdir("/.crosspoint");

  std::string all;
  // Conservative reserve: 30 bytes per line × typical 6 entries × few devices.
  all.reserve(g_cache.size() * 200);
  for (const auto& kv : g_cache) {
    const auto& addr = kv.first;
    const auto& set = kv.second;
    for (uint8_t i = 0; i < set.count; i++) {
      const auto& e = set.entries[i];
      if (e.keycode == 0 || e.action == Action::None) {
        continue;
      }
      char line[48];
      snprintf(line, sizeof(line), "%s,%02X,%u\n", addr.c_str(), static_cast<unsigned>(e.keycode),
               static_cast<unsigned>(e.action));
      all += line;
    }
  }

  if (all.empty()) {
    Storage.remove(MAPPINGS_FILE);
  } else {
    Storage.writeFile(MAPPINGS_FILE, all.c_str());
  }
}

void ensureLoaded() {
  if (g_loaded) {
    return;
  }
  g_loaded = true;
  g_cache.clear();

  if (!Storage.exists(MAPPINGS_FILE)) {
    return;
  }

  String content = Storage.readFile(MAPPINGS_FILE);
  if (content.isEmpty()) {
    return;
  }

  int start = 0;
  while (start < content.length()) {
    int end = content.indexOf('\n', start);
    if (end < 0) {
      end = content.length();
    }
    String line = content.substring(start, end);
    line.trim();
    if (!line.isEmpty()) {
      char addrBuf[32] = {0};
      unsigned int keycode = 0;
      unsigned int actionRaw = 0;
      const int parsed = sscanf(line.c_str(), "%31[^,],%x,%u", addrBuf, &keycode, &actionRaw);
      if (parsed == 3 && keycode > 0 && keycode <= 0xFF && isKnownAction(static_cast<uint8_t>(actionRaw))) {
        auto& set = g_cache[normalizeAddress(addrBuf)];
        if (set.count < MAX_MAPPINGS_PER_DEVICE) {
          set.entries[set.count].keycode = static_cast<uint8_t>(keycode);
          set.entries[set.count].action = static_cast<Action>(actionRaw);
          set.count++;
        }
      }
    }
    start = end + 1;
  }

  LOG_INF("BLEMAP", "Loaded mappings for %u device(s)", static_cast<unsigned>(g_cache.size()));
}

}  // namespace

Action Set::lookup(uint8_t keycode) const {
  if (keycode == 0) {
    return Action::None;
  }
  for (uint8_t i = 0; i < count; i++) {
    if (entries[i].keycode == keycode) {
      return entries[i].action;
    }
  }
  return Action::None;
}

const Set* get(const std::string& macAddress) {
  ensureLoaded();
  auto it = g_cache.find(normalizeAddress(macAddress));
  if (it == g_cache.end()) {
    return nullptr;
  }
  return &it->second;
}

void save(const std::string& macAddress, const Set& set) {
  ensureLoaded();
  const std::string key = normalizeAddress(macAddress);

  // Strip empty slots while copying so the on-disk file stays compact.
  Set compact;
  for (uint8_t i = 0; i < set.count && compact.count < MAX_MAPPINGS_PER_DEVICE; i++) {
    const auto& e = set.entries[i];
    if (e.keycode == 0 || e.action == Action::None) {
      continue;
    }
    compact.entries[compact.count++] = e;
  }

  if (compact.count == 0) {
    g_cache.erase(key);
  } else {
    g_cache[key] = compact;
  }
  writeFileFromCache();
  LOG_INF("BLEMAP", "Saved %u mapping(s) for %s", static_cast<unsigned>(compact.count), key.c_str());
}

void remove(const std::string& macAddress) {
  ensureLoaded();
  if (g_cache.erase(normalizeAddress(macAddress)) > 0) {
    writeFileFromCache();
    LOG_INF("BLEMAP", "Cleared mapping for %s", macAddress.c_str());
  }
}

void clearAll() {
  g_cache.clear();
  g_loaded = true;
  Storage.remove(MAPPINGS_FILE);
  LOG_INF("BLEMAP", "Cleared all mappings");
}

uint8_t actionToButtonIndex(Action action) {
  switch (action) {
    case Action::PageBack:
      return HalGPIO::BTN_UP;
    case Action::PageForward:
      return HalGPIO::BTN_DOWN;
    case Action::Left:
      return HalGPIO::BTN_LEFT;
    case Action::Right:
      return HalGPIO::BTN_RIGHT;
    case Action::Confirm: {
      const uint8_t idx = SETTINGS.frontButtonConfirm;
      return idx < CrossPointSettings::FRONT_BUTTON_HARDWARE::FRONT_BUTTON_HARDWARE_COUNT
                 ? idx
                 : static_cast<uint8_t>(HalGPIO::BTN_CONFIRM);
    }
    case Action::Back: {
      const uint8_t idx = SETTINGS.frontButtonBack;
      return idx < CrossPointSettings::FRONT_BUTTON_HARDWARE::FRONT_BUTTON_HARDWARE_COUNT
                 ? idx
                 : static_cast<uint8_t>(HalGPIO::BTN_BACK);
    }
    case Action::None:
      return 0xFF;
  }
  return 0xFF;
}

}  // namespace BleMappings
