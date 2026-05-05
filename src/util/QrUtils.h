#pragma once

#include <GfxRenderer.h>

#include <string_view>

#include "components/themes/BaseTheme.h"

namespace QrUtils {

// Renders a QR code with the given text payload within the specified bounding box.
void drawQrCode(const GfxRenderer& renderer, const Rect& bounds, std::string_view textPayload);

}  // namespace QrUtils
