#include "HalSystem.h"

#include <Preferences.h>

#include <cstring>
#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }

  __real_panic_print_backtrace(frame, core);
}
}

namespace HalSystem {

void begin() {
  // This is mostly for the first boot, we need to initialize the panic info and logs to empty state
  // If we reboot from a panic state, we want to keep the panic info until we successfully dump it to the SD card, use
  // `clearPanic()` to clear it after dumping
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (!isRebootFromPanic()) {
    return;
  }

  // Persistent crash sequence number lives in NVS so it survives cold boots
  // and is independent of the SD-side log. Counter only advances when we
  // actually dump, so a crash that fails to write doesn't burn a slot.
  // The "crosspoint" namespace matches the convention used elsewhere
  // (HalGPIO uses Preferences the same way).
  constexpr const char* kCrashNvsNamespace = "crosspoint";
  constexpr const char* kCrashSeqKey = "crash_seq";

  uint32_t crashSeq = 0;
  {
    Preferences prefs;
    if (prefs.begin(kCrashNvsNamespace, false)) {
      crashSeq = prefs.getUInt(kCrashSeqKey, 0) + 1;
      prefs.putUInt(kCrashSeqKey, crashSeq);
      prefs.end();
    }
  }

  const auto panicInfo = getPanicInfo(true);
  // O_APPEND so successive crashes accumulate. Each entry is fenced by a
  // banner header so the file remains scannable in a plain text editor.
  auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    return;
  }

  char header[160];
  const int headerLen = snprintf(header, sizeof(header), "\n========== Crash #%u  ms_since_boot=%lu  ==========\n",
                                 static_cast<unsigned>(crashSeq), static_cast<unsigned long>(millis()));
  if (headerLen > 0) {
    file.write(header, static_cast<size_t>(headerLen));
  }
  file.write(panicInfo.c_str(), panicInfo.size());
  // Trailing blank line so the next entry's header starts on its own line.
  file.write("\n", 1);
  // No explicit close — DESTRUCTOR_CLOSES_FILE=1 closes `file` at scope exit.
  LOG_INF("SYS", "Dumped panic info to SD card (crash #%u)", static_cast<unsigned>(crashSeq));
}

void clearPanic() {
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    info += "\n\nPanic reason: " + std::string(panicMessage);
    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nHeap history:\n" + getHeapHistory();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  return resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP;
}

}  // namespace HalSystem
