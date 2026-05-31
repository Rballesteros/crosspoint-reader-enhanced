/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
}

void XtcReaderActivity::onPause() {
  Activity::onPause();

  // Clear font cache when backgrounded to release heap for other activities.
  if (renderer.getFontCacheManager()) {
    renderer.getFontCacheManager()->clearCache();
  }
}

void XtcReaderActivity::loop() {
  // Enter chapter selection activity
  if (ReaderUtils::actionTriggered(mappedInput, MappedInputManager::Button::Confirm)) {
    if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
      startActivityForResult(std::make_unique<XtcReaderChapterSelectionActivity>(this->renderer, this->mappedInput,
                                                                                 xtc, currentPage),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 currentPage = std::get<PageResult>(result.data).page;
                               }
                               requestUpdate();
                             });
    }
  }

  if (ReaderUtils::preferPressForBleInput() && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
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

  // Handle end of book
  if (currentPage >= xtc->getPageCount()) {
    currentPage = xtc->getPageCount() - 1;
    requestUpdate();
    return;
  }

  const bool skipPages = !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP &&
                         mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    requestUpdate();
  } else if (nextTriggered) {
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
    requestUpdate();
  }
}


void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title =
      SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const StatusBarOverlayPosition position) const {
  const bool drawBottom = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom);
}

void XtcReaderActivity::renderPage() {
  xtc::PageInfo pageInfo{};
  const bool hasPageInfo = xtc->getPageInfo(currentPage, pageInfo);
  const uint16_t pageWidth = hasPageInfo ? pageInfo.width : xtc->getPageWidth();
  const uint16_t pageHeight = hasPageInfo ? pageInfo.height : xtc->getPageHeight();
  const uint8_t bitDepth = hasPageInfo ? pageInfo.bitDepth : xtc->getBitDepth();

  if (pageWidth == 0 || pageHeight == 0) {
    LOG_ERR("XTR", "Invalid page geometry for page %lu: %ux%u", currentPage, pageWidth, pageHeight);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTCH 2-bit pages are rendered in streaming passes to avoid the extra ~96KB
    // full-page heap allocation. This keeps large grayscale books openable on-device.
    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const size_t colBytes = (pageHeight + 7) / 8;
    const uint32_t pageDataOffset = pageInfo.offset + sizeof(xtc::XtgPageHeader);

    auto withPageFile = [&](size_t byteOffset, auto&& callback) -> bool {
      FsFile pageFile;
      if (!Storage.openFileForRead("XTR", xtc->getPath(), pageFile)) {
        LOG_ERR("XTR", "Failed to open XTCH file for streaming: %s", xtc->getPath().c_str());
        return false;
      }
      if (!pageFile.seek(pageDataOffset + byteOffset)) {
        LOG_ERR("XTR", "Failed to seek XTCH page data to offset %lu", pageDataOffset + byteOffset);
        pageFile.close();
        return false;
      }
      const bool ok = callback(pageFile);
      pageFile.close();
      return ok;
    };

    auto forEachPlaneByte = [&](size_t planeStartOffset, auto&& visitByte) -> bool {
      return withPageFile(planeStartOffset, [&](FsFile& pageFile) {
        uint8_t chunk[1024];
        size_t remaining = planeSize;
        size_t planeOffset = 0;
        while (remaining > 0) {
          const size_t toRead = std::min(remaining, sizeof(chunk));
          const size_t bytesRead = pageFile.read(chunk, toRead);
          if (bytesRead != toRead) {
            LOG_ERR("XTR", "Short read while streaming XTCH plane: expected %u got %u", toRead, bytesRead);
            return false;
          }
          for (size_t i = 0; i < bytesRead; i++) {
            visitByte(planeOffset + i, chunk[i]);
          }
          planeOffset += bytesRead;
          remaining -= bytesRead;
        }
        return true;
      });
    };

    auto forEachPlanePairByte = [&](auto&& visitPair) -> bool {
      FsFile plane1File;
      FsFile plane2File;
      if (!Storage.openFileForRead("XTR", xtc->getPath(), plane1File) ||
          !Storage.openFileForRead("XTR", xtc->getPath(), plane2File)) {
        LOG_ERR("XTR", "Failed to open XTCH planes for paired streaming: %s", xtc->getPath().c_str());
        if (plane1File) plane1File.close();
        if (plane2File) plane2File.close();
        return false;
      }
      if (!plane1File.seek(pageDataOffset) || !plane2File.seek(pageDataOffset + planeSize)) {
        LOG_ERR("XTR", "Failed to seek XTCH paired planes");
        plane1File.close();
        plane2File.close();
        return false;
      }

      uint8_t plane1Chunk[1024];
      uint8_t plane2Chunk[1024];
      size_t remaining = planeSize;
      size_t planeOffset = 0;
      while (remaining > 0) {
        const size_t toRead = std::min(remaining, sizeof(plane1Chunk));
        const size_t bytesRead1 = plane1File.read(plane1Chunk, toRead);
        const size_t bytesRead2 = plane2File.read(plane2Chunk, toRead);
        if (bytesRead1 != toRead || bytesRead2 != toRead) {
          LOG_ERR("XTR", "Short read while streaming XTCH plane pair: %u/%u expected %u", bytesRead1, bytesRead2,
                  toRead);
          plane1File.close();
          plane2File.close();
          return false;
        }
        for (size_t i = 0; i < toRead; i++) {
          visitPair(planeOffset + i, plane1Chunk[i], plane2Chunk[i]);
        }
        planeOffset += toRead;
        remaining -= toRead;
      }

      plane1File.close();
      plane2File.close();
      return true;
    };

    auto visitPackedByte = [&](size_t planeOffset, uint8_t packed, auto&& onBit) {
      const size_t colIndex = planeOffset / colBytes;
      if (colIndex >= pageWidth) {
        return;
      }
      const uint16_t srcX = pageWidth - 1 - static_cast<uint16_t>(colIndex);
      const uint16_t yBase = static_cast<uint16_t>((planeOffset % colBytes) * 8);
      for (uint8_t bit = 0; bit < 8; bit++) {
        const uint16_t srcY = yBase + bit;
        if (srcY >= maxSrcY) {
          break;
        }
        const bool bitSet = ((packed >> (7 - bit)) & 0x01) != 0;
        onBit(srcX, srcY, bitSet);
      }
    };

    const bool bwOk =
        forEachPlaneByte(0, [&](size_t planeOffset, uint8_t packed) {
          visitPackedByte(planeOffset, packed, [this](uint16_t srcX, uint16_t srcY, bool bitSet) {
            if (bitSet) {
              renderer.drawPixel(srcX, srcY, true);
            }
          });
        }) &&
        forEachPlaneByte(planeSize, [&](size_t planeOffset, uint8_t packed) {
          visitPackedByte(planeOffset, packed, [this](uint16_t srcX, uint16_t srcY, bool bitSet) {
            if (bitSet) {
              renderer.drawPixel(srcX, srcY, true);
            }
          });
        });

    if (!bwOk) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }

    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1 => bit1=0, bit2=1)
    renderer.clearScreen(0x00);
    const bool lsbOk = forEachPlanePairByte([&](size_t planeOffset, uint8_t packed1, uint8_t packed2) {
      const size_t colIndex = planeOffset / colBytes;
      if (colIndex >= pageWidth) {
        return;
      }
      const uint16_t srcX = pageWidth - 1 - static_cast<uint16_t>(colIndex);
      const uint16_t yBase = static_cast<uint16_t>((planeOffset % colBytes) * 8);
      for (uint8_t bit = 0; bit < 8; bit++) {
        const uint16_t srcY = yBase + bit;
        if (srcY >= maxSrcY) {
          break;
        }
        const bool bit1 = ((packed1 >> (7 - bit)) & 0x01) != 0;
        const bool bit2 = ((packed2 >> (7 - bit)) & 0x01) != 0;
        if (!bit1 && bit2) {
          renderer.drawPixel(srcX, srcY, false);
        }
      }
    });
    if (!lsbOk) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2 => bit1 XOR bit2)
    renderer.clearScreen(0x00);
    const bool msbOk = forEachPlanePairByte([&](size_t planeOffset, uint8_t packed1, uint8_t packed2) {
      const size_t colIndex = planeOffset / colBytes;
      if (colIndex >= pageWidth) {
        return;
      }
      const uint16_t srcX = pageWidth - 1 - static_cast<uint16_t>(colIndex);
      const uint16_t yBase = static_cast<uint16_t>((planeOffset % colBytes) * 8);
      for (uint8_t bit = 0; bit < 8; bit++) {
        const uint16_t srcY = yBase + bit;
        if (srcY >= maxSrcY) {
          break;
        }
        const bool bit1 = ((packed1 >> (7 - bit)) & 0x01) != 0;
        const bool bit2 = ((packed2 >> (7 - bit)) & 0x01) != 0;
        if (bit1 != bit2) {
          renderer.drawPixel(srcX, srcY, false);
        }
      }
    });
    if (!msbOk) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer for the next frame
    renderer.clearScreen();
    const bool restoreBwOk =
        forEachPlaneByte(0, [&](size_t planeOffset, uint8_t packed) {
          visitPackedByte(planeOffset, packed, [this](uint16_t srcX, uint16_t srcY, bool bitSet) {
            if (bitSet) {
              renderer.drawPixel(srcX, srcY, true);
            }
          });
        }) &&
        forEachPlaneByte(planeSize, [&](size_t planeOffset, uint8_t packed) {
          visitPackedByte(planeOffset, packed, [this](uint16_t srcX, uint16_t srcY, bool bitSet) {
            if (bitSet) {
              renderer.drawPixel(srcX, srcY, true);
            }
          });
        });

    if (!restoreBwOk) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }

    renderer.cleanupGrayscaleWithFrameBuffer();

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale streaming)", currentPage + 1, xtc->getPageCount());
    return;
  } else {
    // 1-bit XTC pages are row-major and can be rendered directly from the SD stream.
    // This avoids a full-page heap allocation (~48KB for 480x800), which is what
    // caused memory errors for larger books with many pages.
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width
    const xtc::XtcError streamErr = xtc->loadPageStreaming(
        currentPage,
        [this, srcRowBytes, pageWidth, maxSrcY](const uint8_t* data, size_t size, size_t offset) {
          for (size_t i = 0; i < size; i++) {
            const size_t globalByte = offset + i;
            const uint16_t srcY = globalByte / srcRowBytes;
            if (srcY >= maxSrcY) {
              break;
            }

            const uint16_t srcByteInRow = globalByte % srcRowBytes;
            const uint16_t srcXBase = srcByteInRow * 8;
            const uint8_t packed = data[i];

            for (uint8_t bit = 0; bit < 8; bit++) {
              const uint16_t srcX = srcXBase + bit;
              if (srcX >= pageWidth) {
                break;
              }

              const bool isBlack = ((packed >> (7 - bit)) & 0x01) == 0;  // XTC: 0 = black, 1 = white
              if (isBlack) {
                renderer.drawPixel(srcX, srcY, true);
              }
            }
          }
        },
        1024);

    if (streamErr != xtc::XtcError::OK) {
      LOG_ERR("XTR", "Failed to stream page %lu (%s)", currentPage, xtc::errorToString(streamErr));
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
  }
  // White pixels are already cleared by clearScreen()

  if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(StatusBarOverlayPosition::Bottom);
  }

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

void XtcReaderActivity::saveProgress() const {
  HalFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    const uint32_t totalPages = xtc ? xtc->getPageCount() : 0;
    uint8_t data[8];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    data[4] = totalPages & 0xFF;
    data[5] = (totalPages >> 8) & 0xFF;
    data[6] = (totalPages >> 16) & 0xFF;
    data[7] = (totalPages >> 24) & 0xFF;
    f.write(data, sizeof(data));
    f.close();
  }
}

void XtcReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[8] = {0};
    const int read = f.read(data, sizeof(data));
    if (read >= 4) {
      currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                    (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    // Clamp to last valid page to avoid sentinel value (currentPage == pageCount)
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
