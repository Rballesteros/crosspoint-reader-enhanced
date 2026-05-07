#include "Logging.h"

#include <esp_heap_caps.h>
#include <string>

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16
#define MAX_HEAP_SAMPLES 20

// Simple ring buffer log, useful for error reporting when we encounter a crash
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;

struct HeapSample {
  uint32_t timestamp;
  uint32_t free;
  uint32_t maxBlock;
};
RTC_NOINIT_ATTR HeapSample heapHistory[MAX_HEAP_SAMPLES];
RTC_NOINIT_ATTR size_t heapHead = 0;

// Magic word written alongside logHead to detect uninitialized RTC memory.
// RTC_NOINIT_ATTR is not zeroed on cold boot, so logHead may appear in-range
// (0..MAX_LOG_LINES-1) by chance even though logMessages is garbage. The magic
// value is only set by clearLastLogs(), so its absence means the buffer was
// never properly initialized.
RTC_NOINIT_ATTR uint32_t rtcLogMagic;
static constexpr uint32_t LOG_RTC_MAGIC = 0xDEADBEEF;

void addToLogRingBuffer(const char* message) {
  // Add the message to the ring buffer, overwriting old messages if necessary.
  // If the magic is wrong or logHead is out of range (RTC_NOINIT_ATTR garbage
  // on cold boot), clear the entire buffer so subsequent reads are safe.
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    memset(logMessages, 0, sizeof(logMessages));
    logHead = 0;
    rtcLogMagic = LOG_RTC_MAGIC;
  }
  strncpy(logMessages[logHead], message, MAX_ENTRY_LEN - 1);
  logMessages[logHead][MAX_ENTRY_LEN - 1] = '\0';
  logHead = (logHead + 1) % MAX_LOG_LINES;
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  // add timestamp, level, origin and memory info
  {
    unsigned long ms = millis();
    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const int freeKB = static_cast<int>(freeHeap / 1024);
    const int maxKB = static_cast<int>(maxBlock / 1024);

    // Visual bar (10 characters) representing free heap up to 150KB
    char bar[11] = "..........";
    int bars = (freeKB * 10) / 150;
    if (bars > 10) bars = 10;

    // If the largest block is dangerously small (<16KB), use '!' to signal fragmentation risk
    char fillChar = (maxKB < 16) ? '!' : '#';
    for (int i = 0; i < bars; i++) bar[i] = fillChar;

    int len = snprintf(c, sizeof(buf), "[%8lu] [%s] [%-4s] [%s] %3d/%3dK ", ms, level, origin, bar, freeKB, maxKB);
    // error while writing => return
    if (len < 0) {
      va_end(args);
      return;
    }
    // clamp c to be in buffer range
    c += std::min(len, static_cast<int>(sizeof(buf) - 1));
  }
  // add the user message
  {
    int len = vsnprintf(c, sizeof(buf) - (c - buf), format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
  }
  va_end(args);
  if (logSerial) {
    logSerial.print(buf);
  }
  addToLogRingBuffer(buf);
}

std::string getLastLogs() {
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  std::string output;
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      const size_t len = strnlen(logMessages[idx], MAX_ENTRY_LEN);
      output.append(logMessages[idx], len);
    }
  }
  return output;
}

// Checks whether the RTC log state is consistent: rtcLogMagic must equal
// LOG_RTC_MAGIC and logHead must be in 0..MAX_LOG_LINES-1. Returns true if
// corruption is detected, in which case rtcLogMagic is still invalid and
// logMessages may contain garbage. Callers (e.g. HalSystem::begin on the
// panic-reboot path) must call clearLastLogs() after a true result to fully
// reinitialize the ring buffer and stamp the magic before getLastLogs() is used.
bool sanitizeLogHead() {
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    logHead = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  for (size_t i = 0; i < MAX_HEAP_SAMPLES; i++) {
    heapHistory[i].timestamp = 0;
  }
  heapHead = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
}

void recordHeapSample() {
  if (rtcLogMagic != LOG_RTC_MAGIC || heapHead >= MAX_HEAP_SAMPLES) {
    clearLastLogs();
  }
  heapHistory[heapHead].timestamp = millis();
  heapHistory[heapHead].free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  heapHistory[heapHead].maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  heapHead = (heapHead + 1) % MAX_HEAP_SAMPLES;
}

std::string getHeapHistory() {
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  std::string output;
  output += "Time(ms) | Free(KB) | MaxBlock(KB)\n";
  output += "---------|----------|-------------\n";
  for (size_t i = 0; i < MAX_HEAP_SAMPLES; i++) {
    size_t idx = (heapHead + i) % MAX_HEAP_SAMPLES;
    if (heapHistory[idx].timestamp != 0 || heapHistory[idx].free != 0) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%8u | %8u | %12u\n", heapHistory[idx].timestamp, heapHistory[idx].free / 1024,
               heapHistory[idx].maxBlock / 1024);
      output += buf;
    }
  }
  return output;
}
