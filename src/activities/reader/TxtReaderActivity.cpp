#include "TxtReaderActivity.h"

#include <BluetoothHIDManager.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include <memory>
#include <new>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 3;          // Increment when cache format changes
// Deferred-AA settle tuning is shared with the EPUB reader; see
// ReaderUtils::{RAPID_TURN_THRESHOLD_MS, SETTLE_THRESHOLD_MS,
// SETTLE_HEAP_GUARD_BYTES, SETTLE_BLOCK_GUARD_BYTES}.
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  clearAaSettle();
  txt.reset();
}

void TxtReaderActivity::loop() {
  // Edge-triggered settle for deferred AA — same shape as EpubReaderActivity.
  if (aaSettlePending.load() && !settleAlreadyRequested &&
      (millis() - lastPageTurnTime) > ReaderUtils::SETTLE_THRESHOLD_MS && !RenderLock::peek()) {
    settleAlreadyRequested = true;
    requestUpdate();
  }

  if (ReaderUtils::preferPressForBleInput() && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (!ReaderUtils::preferPressForBleInput() && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    activityManager.goHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  const auto triggerPageTurn = [this]() {
    const auto now = millis();
    // Only defer AA when a BLE remote is driving input (see EpubReaderActivity::
    // pageTurn). Local-button turns render AA inline immediately.
    const bool bleDrivingInput = BluetoothHIDManager::getInstance().hadRecentRemoteInput();
    currentTurnIsRapid =
        bleDrivingInput && (lastPageTurnTime != 0) && (now - lastPageTurnTime < ReaderUtils::RAPID_TURN_THRESHOLD_MS);
    clearAaSettle();
    lastPageTurnTime = now;
    requestUpdate();
  };

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    triggerPageTurn();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      triggerPageTurn();
    } else {
      activityManager.goHome();
    }
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  // Reserve to avoid repeated heap doublings on large files. ~2KB/page is a
  // conservative estimate; cap so we don't reserve absurdly for huge files.
  const size_t estimatedPages = std::min(static_cast<size_t>(fileSize / 2048 + 16), static_cast<size_t>(8192));
  pageOffsets.reserve(estimatedPages);
  pageOffsets.push_back(0);  // First page starts at offset 0

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[chunkSize + 1]);
  if (!buffer) {
    return false;
  }

  if (!txt->readContent(buffer.get(), offset, chunkSize)) {
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer.get()), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(&buffer[pos]), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed.
    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

      if (lineWidth <= viewportWidth) {
        outLines.push_back(line);
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        break;
      }

      // Find break point. Measure prefixes by temporarily null-terminating
      // line in place to avoid an allocation per iteration.
      size_t breakPos = line.length();
      while (breakPos > 0) {
        const char saved = line[breakPos];
        line[breakPos] = '\0';
        const int prefixWidth = renderer.getTextWidth(cachedFontId, line.c_str());
        line[breakPos] = saved;
        if (prefixWidth <= viewportWidth) {
          break;
        }
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          // Make sure we don't break in the middle of a UTF-8 sequence
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.emplace_back(line.data(), breakPos);

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line.erase(0, skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Deferred-AA settle path: if the stashed page still matches what's on screen,
  // run only the grayscale pass. currentPageLines is still loaded from the
  // previous render, so no SD reload is needed.
  if (aaSettlePending.load()) {
    if (pendingSettlePageIdx == currentPage && SETTINGS.textAntiAliasing && !currentPageLines.empty()) {
      LOG_DBG("TRS", "AA settle: rendering grayscale pass for page %d", pendingSettlePageIdx);
      renderAaSettle();
      clearAaSettle();
      return;
    }
    clearAaSettle();
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderLinesIntoFramebuffer() const {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  int y = cachedOrientedMarginTop;
  for (const auto& line : currentPageLines) {
    if (!line.empty()) {
      int x = cachedOrientedMarginLeft;

      // Apply text alignment
      switch (cachedParagraphAlignment) {
        case CrossPointSettings::LEFT_ALIGN:
        default:
          // x already set to left margin
          break;
        case CrossPointSettings::CENTER_ALIGN: {
          int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
          x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
          break;
        }
        case CrossPointSettings::RIGHT_ALIGN: {
          int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
          x = cachedOrientedMarginLeft + contentWidth - textWidth;
          break;
        }
        case CrossPointSettings::JUSTIFIED:
          // For plain text, justified is treated as left-aligned
          // (true justification would require word spacing adjustments)
          break;
      }

      renderer.drawText(cachedFontId, x, y, line.c_str());
    }
    y += lineHeight;
  }
}

void TxtReaderActivity::renderPage() {
  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLinesIntoFramebuffer();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  const bool bleCounterRefresh = ReaderUtils::shouldStrengthenBleStatusCounterRefresh(pagesUntilFullRefresh);
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0.0f;
  renderLinesIntoFramebuffer();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  if (bleCounterRefresh) {
    ReaderUtils::refreshStatusBarCounterWindow(renderer, progress, currentPage + 1, totalPages);
  }

  // Consume the burst flag set by loop(). Skip AA when in a rapid burst; settle
  // it later via renderAaSettle() once the user idles.
  const bool rapid = currentTurnIsRapid;
  currentTurnIsRapid = false;

  if (SETTINGS.textAntiAliasing) {
    if (rapid) {
      pendingSettlePageIdx = currentPage;
      settleAlreadyRequested = false;
      aaSettlePending.store(true);
    } else {
      ReaderUtils::renderAntiAliased(
          renderer, [this]() { renderLinesIntoFramebuffer(); },
          [this]() {
            renderLinesIntoFramebuffer();
            renderStatusBar();
          });
    }
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderAaSettle() {
  // Heap gate — skip the settle when fragmentation makes the fallback re-render
  // unsafe. The BW frame already on screen stays as the final state.
  const size_t freeHeap = esp_get_free_heap_size();
  const size_t maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap < ReaderUtils::SETTLE_HEAP_GUARD_BYTES || maxAlloc < ReaderUtils::SETTLE_BLOCK_GUARD_BYTES) {
    LOG_DBG("TRS", "AA settle: skipped (free=%u maxAlloc=%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(maxAlloc));
    return;
  }
  // skipBwStore=true: avoid the ~48KB storeBwBuffer allocation, which fragments
  // the heap and can fault the grayscale render on a dense page (see
  // EpubReaderActivity::renderAaSettle). Re-render the BW frame at the end instead.
  ReaderUtils::renderAntiAliased(
      renderer, [this]() { renderLinesIntoFramebuffer(); },
      [this]() {
        renderLinesIntoFramebuffer();
        renderStatusBar();
      },
      /*skipBwStore=*/true);
}

void TxtReaderActivity::clearAaSettle() {
  aaSettlePending.store(false);
  pendingSettlePageIdx = -1;
  settleAlreadyRequested = false;
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void TxtReaderActivity::saveProgress() const {
  HalFile f;
  if (Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
