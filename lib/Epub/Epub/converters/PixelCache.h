#pragma once

#include <HalStorage.h>
#include <HeapBudget.h>
#include <Logging.h>
#include <stdint.h>

#include <cstring>
#include <string>
#include <string_view>

#include <esp_heap_caps.h>

// Cache buffer for storing 2-bit pixels (4 levels) during decode.
// Packs 4 pixels per byte, MSB first.
struct PixelCache {
  uint8_t* buffer{nullptr};
  size_t bufferSize{0};
  int width;
  int height;
  int bytesPerRow;
  int originX;  // config.x - to convert screen coords to cache coords
  int originY;  // config.y

  PixelCache() : width(0), height(0), bytesPerRow(0), originX(0), originY(0) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr size_t MAX_CACHE_BYTES = 128 * 1024;  // Lowered for C3 stability
  // Margin must cover JPEGDEC's working buffers (~6-8KB on top of the cache buffer).
  // The previous 24KB was sized for the home-cover code path with ~120KB free heap;
  // during a reader render the heap is ~35-50KB, so the old margin meant the .pxc
  // cache was *never* written and every page render re-decoded the image multiple
  // times. Lowering this lets the cache be created on first decode, after which
  // subsequent renders (and future page navigations) hit the SD cache instead.
  static constexpr size_t CACHE_HEAP_SAFETY_MARGIN = 8 * 1024;

  void clear() {
    if (buffer) {
      heap_caps_free(buffer);
      buffer = nullptr;
    }
    bufferSize = 0;
    width = 0;
    height = 0;
    bytesPerRow = 0;
    originX = 0;
    originY = 0;
  }

  bool allocate(int w, int h, int ox, int oy) {
    clear();
    if (w <= 0 || h <= 0) {
      LOG_ERR("IMG", "Invalid cache dimensions: %dx%d", w, h);
      return false;
    }

    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    bufferSize = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(h);

    if (bufferSize > MAX_CACHE_BYTES) {
      LOG_ERR("IMG", "Cache buffer too large: %zu bytes for %dx%d (limit %zu)", bufferSize, w, h, MAX_CACHE_BYTES);
      return false;
    }

    if (!HeapBudget::canAllocate(bufferSize, bufferSize, CACHE_HEAP_SAFETY_MARGIN, "IMG", "cache buffer")) {
      return false;
    }

    buffer = static_cast<uint8_t*>(heap_caps_malloc(bufferSize, MALLOC_CAP_8BIT));
    if (!buffer) {
      LOG_DBG("IMG", "Cache buffer allocation failed: %zu bytes for %dx%d", bufferSize, w, h);
      bufferSize = 0;
      return false;
    }

    memset(buffer, 0, bufferSize);
    LOG_DBG("IMG", "Allocated cache buffer: %zu bytes for %dx%d", bufferSize, w, h);
    return true;
  }

  void setPixel(int screenX, int screenY, uint8_t value) {
    if (!buffer) return;
    int localX = screenX - originX;
    int localY = screenY - originY;
    if (localX < 0 || localX >= width || localY < 0 || localY >= height) return;

    int byteIdx = localY * bytesPerRow + localX / 4;
    int bitShift = 6 - (localX % 4) * 2;  // MSB first: pixel 0 at bits 6-7
    buffer[byteIdx] = (buffer[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }

  bool writeToFile(std::string_view cachePath) {
    if (!buffer || bufferSize == 0) return false;

    HalFile cacheFile;
    if (!Storage.openFileForWrite("IMG", std::string(cachePath), cacheFile)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %.*s", static_cast<int>(cachePath.size()), cachePath.data());
      return false;
    }

    uint16_t w = width;
    uint16_t h = height;
    cacheFile.write(&w, 2);
    cacheFile.write(&h, 2);
    cacheFile.write(buffer, bufferSize);
    cacheFile.close();

    LOG_DBG("IMG", "Cache written: %.*s (%dx%d, %zu bytes)", static_cast<int>(cachePath.size()), cachePath.data(), width,
            height, 4 + bufferSize);
    return true;
  }

  ~PixelCache() { clear(); }
};
