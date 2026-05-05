#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, std::string_view format) {
  if (width * height > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "Image too large (%dx%d = %d pixels %.*s), max supported: %d pixels", width, height, width * height,
            static_cast<int>(format.size()), format.data(), MAX_SOURCE_PIXELS);
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(std::string_view feature, std::string_view imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%.*s' in image '%.*s'. Image may not display correctly.",
          static_cast<int>(feature.size()), feature.data(), static_cast<int>(imagePath.size()), imagePath.data());
}
