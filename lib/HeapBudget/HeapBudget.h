#pragma once

#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstddef>

namespace HeapBudget {

struct Snapshot {
  size_t free;
  size_t largest;
};

inline Snapshot snapshot() { return {heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)}; }

// Returns true if the heap has at least `needBytes + safetyMargin` free AND a
// contiguous block of at least `needBlock`. On failure, emits a LOG_DBG with
// the snapshot so callers do not need to repeat that boilerplate. `tag` is the
// LOG_DBG module tag (e.g. "GFX"); `label` describes the allocation site.
inline bool canAllocate(const size_t needBytes, const size_t needBlock, const size_t safetyMargin, const char* tag,
                        const char* label) {
  const Snapshot s = snapshot();
  const size_t needFree = needBytes + safetyMargin;
  if (s.free < needFree || s.largest < needBlock) {
    LOG_DBG(tag, "Skipping %s: free=%u maxAlloc=%u needFree=%u needBlock=%u", label, static_cast<unsigned>(s.free),
            static_cast<unsigned>(s.largest), static_cast<unsigned>(needFree), static_cast<unsigned>(needBlock));
    return false;
  }
  return true;
}

}  // namespace HeapBudget
