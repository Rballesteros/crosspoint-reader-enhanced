#pragma once

#include "ImageToFramebufferDecoder.h"

class PngToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(std::string_view imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(std::string_view imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(std::string_view imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(std::string_view extension);
  const char* getFormatName() const override { return "PNG"; }
};