#include "I18n.h"

#include <cstddef>
#include <cstring>

#include "I18nStrings.h"

using namespace i18n_strings;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  const uint16_t off = _offsets[index];
  // If bit 15 of the offset is set, apply the offset to the English lookup table
  if (off & 0x8000) return STRINGS_EN_DATA + (off & 0x7FFF);
  return _data + off;
}

void I18n::updateCache() {
  const LangStrings lang = getLanguageStrings(_language);
  _offsets = lang.offsets;
  _data = lang.data;
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
  updateCache();
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
