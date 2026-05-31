#pragma once

#include <BluetoothHIDManager.h>
#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long SKIP_HOLD_MS = 700;

// Deferred anti-aliasing "settle" tuning, shared by the EPUB and TXT readers.
// During a rapid burst of page turns the grayscale pass is skipped and deferred
// until the user idles, so bursts stay responsive. Defined here so both readers
// stay in lockstep — diverging thresholds reintroduce the low-heap settle crash.
//
// If the previous page turn happened within RAPID_TURN_THRESHOLD_MS, the render
// skips the grayscale pass. The settle then fires once input has been idle for
// SETTLE_THRESHOLD_MS.
constexpr unsigned long RAPID_TURN_THRESHOLD_MS = 600;
constexpr unsigned long SETTLE_THRESHOLD_MS = 400;
// Heap headroom required to enter the AA settle. The settle does a BW store +
// grayscale renders; without this guard the fallback re-render can crash on a
// fragmented heap (~50 KB free / ~33 KB max-alloc was the failing point). 80 KB
// free covers the ~72 KB BW store + working headroom; the 12 KB block guard
// keeps us above the 8 KB storeBwBuffer minimum with margin.
constexpr size_t SETTLE_HEAP_GUARD_BYTES = 80 * 1024;
constexpr size_t SETTLE_BLOCK_GUARD_BYTES = 12 * 1024;

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

inline bool allowLongPressChapterSkip() {
  // BLE page-turn remotes can report delayed or synthetic release frames, which
  // makes release-driven page turns look ghostier than local buttons. Treat
  // recent BLE input as page-turn-only and keep chapter-skip semantics for the
  // local hardware buttons.
  return SETTINGS.longPressButtonBehavior != SETTINGS.OFF && !BluetoothHIDManager::getInstance().hadRecentRemoteInput();
}

inline bool preferPressForBleInput() { return BluetoothHIDManager::getInstance().hadRecentRemoteInput(); }

inline bool actionTriggered(const MappedInputManager& input, const MappedInputManager::Button button) {
  return preferPressForBleInput() ? input.wasPressed(button) : input.wasReleased(button);
}

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = !allowLongPressChapterSkip();
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool prev = tiltPrev || (usePress ? (input.wasPressed(MappedInputManager::Button::PageBack) ||
                                             input.wasPressed(MappedInputManager::Button::Left))
                                          : (input.wasReleased(MappedInputManager::Button::PageBack) ||
                                             input.wasReleased(MappedInputManager::Button::Left)));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool next = tiltNext || (usePress ? (input.wasPressed(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasPressed(MappedInputManager::Button::Right))
                                          : (input.wasReleased(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasReleased(MappedInputManager::Button::Right)));
  return {prev, next, tiltPrev || tiltNext};
}

inline bool shouldStrengthenBleStatusCounterRefresh(int pagesUntilFullRefresh) {
  return pagesUntilFullRefresh > 1 &&
         (SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage) &&
         BluetoothHIDManager::getInstance().hadRecentRemoteInput();
}

inline void refreshStatusBarCounterWindow(const GfxRenderer& renderer, float bookProgress, int currentPage,
                                          int pageCount) {
  if (!(SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage)) {
    return;
  }

  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  char progressStr[32] = {};
  if (SETTINGS.statusBarBookProgressPercentage && SETTINGS.statusBarChapterPageCount) {
    snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
  } else if (SETTINGS.statusBarBookProgressPercentage) {
    snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
  } else {
    snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
  }

  const int progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
  if (progressTextWidth <= 0) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const int progressTextX =
      renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight - progressTextWidth;
  int refreshX = progressTextX - 12;
  int refreshY = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - 8;
  if (refreshY < 0) {
    refreshY = 0;
  }

  refreshX &= ~0x7;  // byte-align for displayWindow
  if (refreshX < 0) {
    refreshX = 0;
  }

  int refreshWidth = renderer.getScreenWidth() - refreshX - orientedMarginRight;
  refreshWidth = (refreshWidth + 7) & ~0x7;
  if (refreshX + refreshWidth > renderer.getScreenWidth()) {
    refreshWidth = renderer.getScreenWidth() - refreshX;
    refreshWidth &= ~0x7;
  }

  const int refreshHeight = renderer.getScreenHeight() - refreshY;
  if (refreshWidth <= 0 || refreshHeight <= 0) {
    return;
  }

  renderer.displayWindow(refreshX, refreshY, refreshWidth, refreshHeight, HalDisplay::FAST_REFRESH);
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. If the BW framebuffer backup cannot be allocated, AA
// still runs and the caller-provided restore callback rebuilds the normal BW
// framebuffer afterward. Kept as templates to avoid std::function overhead.
//
// skipBwStore forces the re-render path: the ~48KB storeBwBuffer allocation is
// skipped entirely and restoreBwFn rebuilds the BW frame at the end. Callers
// running under heap pressure (e.g. the deferred-AA settle after a rapid burst)
// use this — the store fragments the heap and the subsequent grayscale
// page render then faults on a too-small contiguous block.
template <typename RenderFn, typename RestoreBwFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn, RestoreBwFn&& restoreBwFn,
                       bool skipBwStore = false) {
  const bool bwBufferStored = !skipBwStore && renderer.storeBwBuffer();
  if (!bwBufferStored && !skipBwStore) {
    LOG_DBG("READER", "BW buffer store failed; AA will use re-render fallback");
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  if (bwBufferStored) {
    renderer.restoreBwBuffer();
    LOG_DBG("READER", "AA fast path: restored BW buffer");
  } else {
    renderer.clearScreen();
    restoreBwFn();
    renderer.cleanupGrayscaleWithFrameBuffer();
    LOG_DBG("READER", "AA fallback: re-rendered BW buffer");
  }
}

template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  renderAntiAliased(renderer, renderFn, renderFn);
}

}  // namespace ReaderUtils
