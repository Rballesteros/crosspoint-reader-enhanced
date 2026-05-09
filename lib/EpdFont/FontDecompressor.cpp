#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdlib>

namespace {
constexpr size_t FONT_ALLOC_SAFETY_MARGIN = 8 * 1024;
constexpr size_t FONT_PREWARM_SAFETY_MARGIN = 16 * 1024;
constexpr size_t FONT_PREWARM_FRAGMENTATION_MARGIN = 8 * 1024;

bool hasHeapForAllocation(const size_t bytes, const size_t safetyMargin = FONT_ALLOC_SAFETY_MARGIN) {
  if (bytes == 0) return true;

  const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  return freeHeap >= bytes + safetyMargin && largestBlock >= bytes;
}
}  // namespace

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::clearCache() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::freePageBuffer() {
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    pageSlots[s].buffer.clear();
    pageSlots[s].buffer.shrink_to_fit();
    pageSlots[s].glyphs.clear();
    pageSlots[s].glyphs.shrink_to_fit();
    pageSlots[s].fontData = nullptr;
  }
  pageSlotCount = 0;
}
void FontDecompressor::freeHotGroup() {
  for (uint8_t i = 0; i < MAX_HOT_GROUPS; i++) {
    hotGroups[i].data.clear();
    hotGroups[i].data.shrink_to_fit();
    hotGroups[i].groupIndex = UINT16_MAX;
    hotGroups[i].fontData = nullptr;
  }
  hotGroupMruIdx = 0;
  hotGlyphBuf.clear();
  hotGlyphBuf.shrink_to_fit();
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Contiguous-group fonts: linear scan
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint32_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint32_t tDecomp = millis();
  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height) {
  if (width == 0 || height == 0) return;
  const uint32_t rowStride = (width + 3) / 4;
  if (width % 4 == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      outByte = (outByte << 2) | ((alignedSrc[y * rowStride + x / 4] >> ((3 - (x % 4)) * 2)) & 0x3);
      outBits += 2;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

// --- getBitmap: page buffer → hot group → decompress ---

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffer slots (populated by prewarmCache — one slot per font style)
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    const auto& slot = pageSlots[s];
    if (slot.fontData != fontData || slot.glyphs.empty()) continue;

    int left = 0, right = static_cast<int>(slot.glyphs.size()) - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == glyphIndex) {
        if (slot.glyphs[mid].bufferOffset != UINT32_MAX) {
          stats.cacheHits++;
          stats.getBitmapTimeUs += micros() - tStart;
          return &slot.buffer[slot.glyphs[mid].bufferOffset];
        }
        break;  // Not extracted during prewarm; fall through to hot-group path
      }
      if (slot.glyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
    break;  // Found the right slot but glyph wasn't in it; don't check other slots
  }

  // Fallback: hot group slots (2-entry MRU cache)
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Search for the group in the hot group MRU cache
  int foundIdx = -1;
  for (uint8_t i = 0; i < MAX_HOT_GROUPS; i++) {
    if (hotGroups[i].groupIndex == groupIndex && hotGroups[i].fontData == fontData) {
      foundIdx = i;
      break;
    }
  }

  if (foundIdx != -1) {
    // Cache hit
    stats.cacheHits++;
    hotGroupMruIdx = static_cast<uint8_t>(foundIdx);  // Mark as most recently used
  } else {
    // Cache miss
    stats.cacheMisses++;
    // Use the next slot in the MRU ring
    hotGroupMruIdx = (hotGroupMruIdx + 1) % MAX_HOT_GROUPS;
    HotGroup& slot = hotGroups[hotGroupMruIdx];

    const EpdFontGroup& group = fontData->groups[groupIndex];
    if (slot.data.capacity() < group.uncompressedSize && !hasHeapForAllocation(group.uncompressedSize)) {
      LOG_DBG("FDC", "Skipping hot group %u allocation: free=%u maxAlloc=%u need=%u", groupIndex,
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(group.uncompressedSize));
      slot.data.clear();
      slot.groupIndex = UINT16_MAX;
      slot.fontData = nullptr;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    slot.data.assign(group.uncompressedSize, 0);

    if (!decompressGroup(fontData, groupIndex, slot.data.data(), group.uncompressedSize)) {
      slot.data.clear();
      slot.data.shrink_to_fit();
      slot.groupIndex = UINT16_MAX;
      slot.fontData = nullptr;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    slot.groupIndex = groupIndex;
    slot.fontData = fontData;
    stats.hotGroupBytes = group.uncompressedSize;
    foundIdx = hotGroupMruIdx;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (glyph->dataLength > hotGlyphBuf.size()) {
    if (hotGlyphBuf.capacity() < glyph->dataLength && !hasHeapForAllocation(glyph->dataLength)) {
      LOG_DBG("FDC", "Skipping glyph scratch allocation: free=%u maxAlloc=%u need=%u",
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(glyph->dataLength));
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }
    hotGlyphBuf.resize(glyph->dataLength);
  }
  if (hotGlyphBuf.empty()) {
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  compactSingleGlyph(&hotGroups[foundIdx].data[alignedOff], hotGlyphBuf.data(), glyph->width, glyph->height);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf.data();
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;

  if (count == 0) return -1;

  // Binary search
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];

    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval->offset + (codepoint - interval->first));
    }
  }

  return -1;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, std::string_view utf8Text) {
  if (!fontData || !fontData->groups || utf8Text.empty()) return 0;

  // Allocate the next available slot (caller must call freePageBuffer/clearCache to reset)
  if (pageSlotCount >= MAX_PAGE_SLOTS) {
    LOG_ERR("FDC", "All %u page buffer slots full, cannot prewarm fontData=%p", MAX_PAGE_SLOTS, (void*)fontData);
    return -1;
  }
  PageSlot& slot = pageSlots[pageSlotCount];

  // Step 1: Collect unique glyph indices needed for this page
  uint32_t neededGlyphs[MAX_PAGE_GLYPHS];
  uint16_t glyphCount = 0;
  bool glyphCapWarned = false;

  std::string_view p = utf8Text;
  while (!p.empty()) {
    uint32_t cp = utf8NextCodepoint(p);
    if (cp == 0) break;

    int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx < 0) continue;

    // Deduplicate
    bool found = false;
    for (uint16_t i = 0; i < glyphCount; i++) {
      if (neededGlyphs[i] == static_cast<uint32_t>(glyphIdx)) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (glyphCount < MAX_PAGE_GLYPHS) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(glyphIdx);
      } else if (!glyphCapWarned) {
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
                MAX_PAGE_GLYPHS);
        glyphCapWarned = true;
      }
    }
  }

  // Add ligature output glyphs: if both input codepoints of a ligature pair are
  // in the needed set, the output glyph will be queried during rendering.
  if (fontData->ligaturePairs && fontData->ligaturePairCount > 0) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; li++) {
      uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
      uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;

      int32_t leftIdx = findGlyphIndex(fontData, leftCp);
      int32_t rightIdx = findGlyphIndex(fontData, rightCp);
      if (leftIdx < 0 || rightIdx < 0) continue;

      // Check if both inputs are in neededGlyphs
      bool hasLeft = false, hasRight = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(leftIdx)) hasLeft = true;
        if (neededGlyphs[i] == static_cast<uint32_t>(rightIdx)) hasRight = true;
        if (hasLeft && hasRight) break;
      }
      if (!hasLeft || !hasRight) continue;

      int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
      if (outIdx < 0) continue;

      // Deduplicate
      bool found = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(outIdx)) {
          found = true;
          break;
        }
      }
      if (!found) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(outIdx);
      }
    }
  }

  if (glyphCount == 0) return 0;

  // Step 2: Compute total buffer size and collect unique groups
  uint32_t totalBytes = 0;
  uint16_t neededGroups[128];
  uint8_t groupCount = 0;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    totalBytes += fontData->glyph[neededGlyphs[i]].dataLength;
    uint16_t gi = getGroupIndex(fontData, neededGlyphs[i]);
    bool found = false;
    for (uint8_t j = 0; j < groupCount; j++) {
      if (neededGroups[j] == gi) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (groupCount < 128) {
        neededGroups[groupCount++] = gi;
      } else if (!groupCapWarned) {
        LOG_DBG("FDC", "Group cap (128) reached during prewarm; some groups will use hot-group fallback");
        groupCapWarned = true;
      }
    }
  }

  stats.uniqueGroupsAccessed = groupCount;

  // Step 3: Allocate page buffer and lookup table for this slot
  uint32_t peakTempBytes = 0;
  for (uint8_t g = 0; g < groupCount; g++) {
    peakTempBytes = std::max(peakTempBytes, fontData->groups[neededGroups[g]].uncompressedSize);
  }

  const size_t glyphEntriesBytes = glyphCount * sizeof(PageGlyphEntry);
  const size_t prewarmWorkingSet = static_cast<size_t>(totalBytes) + glyphEntriesBytes + peakTempBytes;
  const size_t largestNeeded =
      std::max({static_cast<size_t>(totalBytes), glyphEntriesBytes, static_cast<size_t>(peakTempBytes)}) +
      FONT_PREWARM_FRAGMENTATION_MARGIN;
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeBefore < prewarmWorkingSet + FONT_PREWARM_SAFETY_MARGIN || largestBefore < largestNeeded) {
    LOG_DBG("FDC",
            "Skipping prewarm: free=%u maxAlloc=%u needFree=%u needBlock=%u glyphs=%u groups=%u pageBuf=%u peakTemp=%u",
            static_cast<unsigned>(freeBefore), static_cast<unsigned>(largestBefore),
            static_cast<unsigned>(prewarmWorkingSet + FONT_PREWARM_SAFETY_MARGIN),
            static_cast<unsigned>(largestNeeded), glyphCount, groupCount, static_cast<unsigned>(totalBytes),
            static_cast<unsigned>(peakTempBytes));
    return glyphCount;
  }

  slot.buffer.resize(totalBytes);
  slot.glyphs.resize(glyphCount);
  if (slot.buffer.empty() && totalBytes > 0) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes)", totalBytes);
    slot.buffer.clear();
    slot.glyphs.clear();
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.fontData = fontData;
  pageSlotCount++;

  // Initialize lookup entries (bufferOffset = UINT32_MAX means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    slot.glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
  }

  // Sort by glyphIndex for binary search in getBitmap()
  std::sort(slot.glyphs.begin(), slot.glyphs.end(),
            [](const PageGlyphEntry& a, const PageGlyphEntry& b) { return a.glyphIndex < b.glyphIndex; });

  // Step 3b: Pre-scan to compute each needed glyph's byte-aligned offset within its group.
  // This avoids recomputing aligned offsets per group during extraction in step 4.
  uint32_t groupAlignedTracker[128] = {};  // running byte-aligned offset for each needed group

  if (fontData->glyphToGroup) {
    // Frequency-grouped: single O(totalGlyphs) pass through glyphToGroup
    const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
    const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);

    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      // Find this glyph's group position in neededGroups
      uint8_t gpPos = groupCount;
      for (uint8_t j = 0; j < groupCount; j++) {
        if (neededGroups[j] == gi) {
          gpPos = j;
          break;
        }
      }
      if (gpPos == groupCount) continue;  // not a needed group

      const EpdGlyph& glyph = fontData->glyph[i];

      // Binary search in sorted slot.glyphs to find if glyph i is needed
      int left = 0, right = static_cast<int>(slot.glyphs.size()) - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (slot.glyphs[mid].glyphIndex == i) {
          slot.glyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (slot.glyphs[mid].glyphIndex < i)
          left = mid + 1;
        else
          right = mid - 1;
      }

      if (glyph.width > 0 && glyph.height > 0) {
        groupAlignedTracker[gpPos] += ((glyph.width + 3) / 4) * glyph.height;
      }
    }
  } else {
    // Contiguous-group: iterate each needed group's glyphs directly
    for (uint8_t g = 0; g < groupCount; g++) {
      const EpdFontGroup& group = fontData->groups[neededGroups[g]];
      uint32_t alignedOff = 0;
      for (uint16_t j = 0; j < group.glyphCount; j++) {
        const uint32_t glyphI = group.firstGlyphIndex + j;
        const EpdGlyph& glyph = fontData->glyph[glyphI];

        int left = 0, right = static_cast<int>(slot.glyphs.size()) - 1;
        while (left <= right) {
          const int mid = left + (right - left) / 2;
          if (slot.glyphs[mid].glyphIndex == glyphI) {
            slot.glyphs[mid].alignedOffset = alignedOff;
            break;
          }
          if (slot.glyphs[mid].glyphIndex < glyphI)
            left = mid + 1;
          else
            right = mid - 1;
        }

        if (glyph.width > 0 && glyph.height > 0) {
          alignedOff += ((glyph.width + 3) / 4) * glyph.height;
        }
      }
    }
  }

  // Step 4: For each unique group, decompress to temp buffer and extract needed glyphs
  uint32_t writeOffset = 0;
  int missed = 0;

  for (uint8_t g = 0; g < groupCount; g++) {
    uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];

    auto* tempBuf = static_cast<uint8_t*>(heap_caps_malloc(group.uncompressedSize, MALLOC_CAP_8BIT));
    if (!tempBuf && group.uncompressedSize > 0) {
      LOG_DBG("FDC", "Failed to allocate temp buffer (%u bytes) for group %u, free=%u maxAlloc=%u",
              group.uncompressedSize, groupIdx, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
      missed++;
      continue;
    }
    if (group.uncompressedSize > stats.peakTempBytes) {
      stats.peakTempBytes = group.uncompressedSize;
    }

    if (!decompressGroup(fontData, groupIdx, tempBuf, group.uncompressedSize)) {
      heap_caps_free(tempBuf);
      missed++;
      continue;
    }

    // Extract needed glyphs directly from the byte-aligned temp buffer, compacting on the fly.
    // alignedOffset was pre-computed in step 3b — no full-group compact scan needed.
    for (uint16_t i = 0; i < slot.glyphs.size(); i++) {
      if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (getGroupIndex(fontData, slot.glyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
      compactSingleGlyph(&tempBuf[slot.glyphs[i].alignedOffset], &slot.buffer[writeOffset], glyph.width, glyph.height);
      slot.glyphs[i].bufferOffset = writeOffset;
      writeOffset += glyph.dataLength;
    }

    heap_caps_free(tempBuf);
  }

  LOG_DBG("FDC", "Prewarm: %u glyphs in %u bytes from %u groups (%d missed)", glyphCount, writeOffset, groupCount,
          missed);

  return missed;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  LOG_DBG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_DBG("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs, stats.uniqueGroupsAccessed);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
