// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <windows.h>
#include <cstddef>
#include <string>

namespace ogg_input {

// Initialize on the host window's UI thread. Reinitialize to refresh a changed
// language pack. A missing/old service is normal and leaves the English text.
// Shutdown may be called after the host has gone away: it only clears our cache.
void InitializeLanguage(HWND host, HINSTANCE own_module) noexcept;
void ShutdownLanguage() noexcept;

// Returns an owned copy, safe against concurrent cache reset. Unknown, null,
// empty, or untranslated entries preserve their exact supplied fallback (null
// becomes empty). This function never calls Winamp and is safe on worker threads.
std::wstring Localized(const wchar_t* fallback);

// Stable, explicit STRINGTABLE IDs, unrelated to dialog/control IDs. Zero means
// an unknown phrase. Existing IDs must never be renumbered or reused.
unsigned LanguageStringId(const wchar_t* fallback) noexcept;

// UI-thread only. Text-only pass over the caption and direct Static/Button
// children; no foreign dialog templates, editable fields, or layout changes.
void LocalizeDialog(HWND dialog);

struct LanguageStringEntry {
    unsigned id;
    const wchar_t* fallback;
};

// Read-only catalogue for translation-template generation and validation.
const LanguageStringEntry* LanguageStrings(std::size_t& count) noexcept;

namespace language {
// Independently assigned module identity, NOT the historical in_vorbis identity.
inline constexpr GUID kModuleGuid =
    {0x65e0e78d, 0x78c1, 0x4c34, {0xb1, 0x1e, 0x87, 0xfd, 0x6f, 0x26, 0x48, 0x5c}};

// Public Winamp SDK contract constants only. The original module is queried
// read-only, solely for the exact Name/Value field-label string IDs 55/56.
inline constexpr GUID kLegacyModuleGuid =
    {0x5c5bca4e, 0x279e, 0x4867, {0x8e, 0x24, 0x58, 0xc8, 0xb1, 0x86, 0x95, 0x9a}};
inline constexpr GUID kServiceGuid =
    {0x30aed4e5, 0xef10, 0x4277, {0x8d, 0x49, 0x27, 0xab, 0x55, 0x70, 0xe8, 0x91}};
inline constexpr int kServiceByGuid = 50;
inline constexpr unsigned kGuidStringId = 65535; // LANG_DLL_GUID_STRING_ID
namespace Message {
inline constexpr int GetStringW = 11;
inline constexpr int FindDllHandleByGuid = 20;
inline constexpr int StartLanguageSupport = 30;
}
} // namespace language
} // namespace ogg_input
