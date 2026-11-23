// SPDX-License-Identifier: GPL-3.0-only
#include "language.hpp"
#include "matroska.hpp"
#include "resource.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <utility>

namespace ogg_input {
namespace {

// Independently authored English catalogue. Append IDs; never reorder IDs to
// match control IDs or borrow an existing module's string-number namespace.
constexpr LanguageStringEntry kStrings[] = {
    {20000, L"File Information"},
    {20001, L"File / URL"},
    {20002, L"Location:"},
    {20003, L"Tags"},
    {20004, L"Title:"},
    {20005, L"Artist:"},
    {20006, L"Album:"},
    {20007, L"Track:"},
    {20008, L"Date:"},
    {20009, L"Genre:"},
    {20010, L"Comment:"},
    {20011, L"Other Tags:"},
    {20012, L"Name:"},
    {20013, L"Value:"},
    {20014, L"Add / Update"},
    {20015, L"Delete"},
    {20016, L"File / Stream Information"},
    {20017, L"Logical Stream"},
    {20018, L"Previous"},
    {20019, L"Next"},
    {20020, L"Changes are written only when Save is selected."},
    {20021, L"Save"},
    {20022, L"Close"},
    {20023, L"Ogg Input Settings"},
    {20024, L"Channel output:"},
    {20025, L"Output rate:"},
    {20026, L"Sample depth:"},
    {20027, L"Resampler quality:"},
    {20028, L"Dither 8-bit, 16-bit, and 24-bit output"},
    {20029, L"Prevent clipping / peak limit"},
    {20030, L"Use enhanced Speex decoding"},
    {20031, L"Buffer small local files"},
    {20032, L"Show instant bitrate where available"},
    {20033, L"Enable read-only unified information pane"},
    {20034, L"ReplayGain mode:"},
    {20035, L"Regular preamp (dB):"},
    {20036, L"Radio preamp (dB):"},
    {20037, L"Target loudness (LUFS):"},
    {20038, L"Peak handling:"},
    {20039, L"Untagged preamp (dB):"},
    {20040, L"Use only the preferred gain tag (no track / album fallback)"},
    {20041, L"Preamps accept -60.0 through +30.0 dB. Target accepts -40 through 0 LUFS."},
    {20042, L"HTTP timeout (ms):"},
    {20043, L"Initial probe limit (KiB):"},
    {20044, L"Reconnect attempts:"},
    {20045, L"Reconnect delay (ms):"},
    {20046, L"Stream buffer (KiB):"},
    {20047, L"Startup prebuffer (KiB):"},
    {20048, L"Manual proxy URL (optional):"},
    {20049, L"Save incoming streams"},
    {20050, L"Save directory (optional):"},
    {20051, L"A blank proxy uses the system proxy. Credentials are never stored."},
    {20052, L"Proxy use:"},
    {20053, L"Allow HTTP Range seeking when the server reports a known length"},
    {20054, L"Rebuffer threshold (%):"},
    {20055, L"These options apply only to HTTP streams. Range seeking depends on server support."},
    {20056, L"OK"},
    {20057, L"Cancel"},
    {20058, L"About Ogg Input"},
    {20059, L"Ogg Information"},
    {20060, L"This pane is read-only."},
    {20061, L"Open File Information..."},
    {20062, L"Name"},
    {20063, L"Value"},
    {20064, L"Read-only: radio and URL streams cannot be written."},
    {20065, L"Read-only: no local file path is available."},
    {20066, L"Read-only: Ogg FLAC tag writing is not supported."},
    {20067, L"Read-only: Ogg Speex tag writing is not supported."},
    {20068, L"Read-only: this stream format cannot be written."},
    {20069, L"Editable local Ogg Vorbis or Ogg Opus tags."},
    {20070, L"Changes are kept in this dialog until Save is selected."},
    {20071, L"A tag name must contain 1 to 128 printable ASCII characters and no '=' sign."},
    {20072, L"Tag change pending. Select Save to write it to the file."},
    {20073, L"Select an Other Tags entry to delete."},
    {20074, L"Tag deletion pending. Select Save to write it to the file."},
    {20075, L"There are no changes for this logical stream."},
    {20076, L"The file could not be updated."},
    {20077, L"Unable to Save Tags"},
    {20078, L"Discard unsaved tag edits? No tags have been written automatically."},
    {20079, L"Automatic (source rate)"},
    {20080, L"Decoding"},
    {20081, L"ReplayGain"},
    {20082, L"Streaming"},
    {20083, L"Advanced Streaming"},
    {20084, L"Source (stereo fallback at chain changes)"},
    {20085, L"Stereo"},
    {20086, L"Mono"},
    {20087, L"Quad (downmix 5.1+)"},
    {20088, L"8-bit"},
    {20089, L"16-bit"},
    {20090, L"24-bit"},
    {20091, L"32-bit"},
    {20092, L"Header gain"},
    {20093, L"Track gain"},
    {20094, L"Album gain"},
    {20095, L"Automatic (track when shuffled)"},
    {20096, L"Raw / no gain adjustment"},
    {20097, L"Apply gain (allow clipping)"},
    {20098, L"Apply gain with peak protection"},
    {20099, L"Normalize using tagged peak"},
    {20100, L"Prevent clipping only"},
    {20101, L"Never use a proxy"},
    {20102, L"Only for HTTP port 80"},
    {20103, L"Always use system or manual proxy"},
    {20104, L"Output rate must be Automatic or a value from 8000 through 192000 Hz."},
    {20105, L"Select a valid channel output mode."},
    {20106, L"Select a valid sample depth."},
    {20107, L"Resampler quality must be between 0 and 10."},
    {20108, L"Select a valid ReplayGain mode."},
    {20109, L"Regular preamp must be a number from -60.0 through +30.0 dB."},
    {20110, L"Radio preamp must be a number from -60.0 through +30.0 dB."},
    {20111, L"Target loudness"},
    {20112, L"Select a valid peak handling mode."},
    {20113, L"Untagged preamp must be a number from -60.0 through +30.0 dB."},
    {20114, L"HTTP timeout"},
    {20115, L"Initial probe limit"},
    {20116, L"Reconnect attempts"},
    {20117, L"Reconnect delay"},
    {20118, L"Stream buffer"},
    {20119, L"Startup prebuffer"},
    {20120, L"Select a valid proxy policy."},
    {20121, L"Rebuffer threshold"},
    {20122, L"Proxy credentials cannot be saved. Enter a credential-free proxy URL."},
    {20123, L"Save directory must be a single local path."},
    {20124, L"Settings could not be saved to the current user's registry."},
    {20125, L"Read-only metadata. Use File Information to edit supported local files."},
    {20126, L"Processing"},
    {20127, L"Source (Windows channel order)"},
    {20128, L"Source (unmapped codec channel order)"},
    {20129, L"Stereo surround matrix (DS)"},
    {20130, L"Stereo surround matrix (DS2)"},
    {20131, L"Rectangular"},
    {20132, L"Triangular"},
    {20133, L"Gaussian"},
    {20134, L"None"},
    {20135, L"First-order / triangular shaping"},
    {20136, L"ATH psychoacoustic shaping"},
    {20137, L"ATH (low amplitude)"},
    {20138, L"Dither distribution:"},
    {20139, L"Noise shaping:"},
    {20140, L"Follow chained source formats (reopen output when needed)"},
    {20141, L"Legacy 5.1 input order (L, R, C, SL, SR, LFE)"},
    {20142, L"Dither must be enabled on the Decoding page. ATH modes fall back to first-order shaping at unsupported rates. DS/DS2 and quad matrices may clip; allow headroom with the preamp."},
    {20143, L"Use Winamp ReplayGain settings when available"},
    {20144, L"Use Winamp proxy settings when available"},
    {20145, L"Select a valid dither distribution."},
    {20146, L"Select a valid noise shaping mode."},
    {20147, L"Ogg Audio Information"},
    {20148, L"Cannot read this Ogg audio file."},
    {20149, L"Playback is stopped."},
    {20150, L"Ogg audio"},
    {20151, L"Link "},
    {20152, L" of "},
    {20153, L"Saved tags for logical stream "},
    {20154, L" must be between "},
    {20155, L" and "},
    {20156, L"Ogg Input Settings - "},
    {20157, L"Ogg Input for Winamp\r\n\r\n"
        L"Independent GPLv3 implementation. This project is not a copy of historical "
        L"in_vorbis code or resources and does not claim full feature or behavior parity.\r\n\r\n"
        L"Codec libraries in this build: libvorbis 1.3.7, libopus/opusfile, "
        L"FLAC 1.5.0, and Speex 1.2.1."},
    {20158, L"Dither integer output (8/16/24/32-bit)"},
    {20159, L"Preserve exact FLAC PCM (32-bit, no dither or processing)"},
    {20160, L"Decode Opus HD at 96 kHz when present"},
    {20161, L"Opus speech enhancement (OSCE, changes decoded audio)"},
    {20162, L"Opus speech BWE (48 kHz only; synthetic high frequencies)"},
    {20163, L"Ambisonics output:"},
    {20164, L"Stereo speakers"},
    {20165, L"Headphones (analytic binaural)"},
    {20166, L"Quad speakers"},
    {20167, L"5.1 speakers (silent LFE)"},
    {20168, L"7.1 speakers (silent LFE)"},
    {20169, L"Select a valid Ambisonics output mode."},
};
constexpr std::size_t kStringCount = sizeof(kStrings) / sizeof(kStrings[0]);
constexpr std::size_t kBufferCharacters = 4096;

constexpr bool SameText(const wchar_t* left, const wchar_t* right) noexcept {
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}
constexpr bool ValidCatalogue() noexcept {
    for (std::size_t i = 0; i < kStringCount; ++i) {
        if (!kStrings[i].id || kStrings[i].id > 65535 || !*kStrings[i].fallback) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (kStrings[i].id == kStrings[j].id ||
                SameText(kStrings[i].fallback, kStrings[j].fallback)) return false;
        }
    }
    return true;
}
static_assert(ValidCatalogue(), "Language IDs and fallback phrases must be unique.");

SRWLOCK cache_lock = SRWLOCK_INIT;
std::array<std::wstring, kStringCount> translations;

std::size_t StringIndex(const wchar_t* text) noexcept {
    if (text && *text) {
        for (std::size_t i = 0; i < kStringCount; ++i) {
            if (std::wcscmp(kStrings[i].fallback, text) == 0) return i;
        }
    }
    return kStringCount;
}

class SharedCacheLock {
public:
    SharedCacheLock() noexcept { AcquireSRWLockShared(&cache_lock); }
    ~SharedCacheLock() { ReleaseSRWLockShared(&cache_lock); }
    SharedCacheLock(const SharedCacheLock&) = delete;
    SharedCacheLock& operator=(const SharedCacheLock&) = delete;
};

bool ValidPointer(const void* pointer) noexcept {
    // IPC queries unsupported by old Winamp versions return the sentinel 1.
    return reinterpret_cast<std::uintptr_t>(pointer) > 65535u;
}

bool OnWindowThread(HWND window) noexcept {
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    return thread && thread == GetCurrentThreadId() && process == GetCurrentProcessId();
}

class LanguageInterface {
public:
    matroska::ServiceFactory* factory = nullptr;
    matroska::Dispatchable* service = nullptr;
    ~LanguageInterface() noexcept {
        if (factory && service) {
            void* pointer = service;
            void* args[] = {&pointer};
            int released = 0;
            try { factory->_dispatch(matroska::ServiceFactoryMessage::ReleaseInterface,
                    &released, args, 1); }
            catch (...) { /* Optional host service must not escape teardown. */ }
        }
    }
    LanguageInterface() = default;
    LanguageInterface(const LanguageInterface&) = delete;
    LanguageInterface& operator=(const LanguageInterface&) = delete;
};

HINSTANCE OwnLanguageModule(matroska::Dispatchable* service, HINSTANCE own) {
    GUID guid = language::kModuleGuid; // Wasabi passes GUID values by address.
    void* args[] = {&own, &guid};
    HINSTANCE module = nullptr;
    if (!service->_dispatch(language::Message::StartLanguageSupport, &module, args, 2) ||
        !ValidPointer(module) || module == own) return nullptr;
    return module;
}

HINSTANCE LegacyLanguageModule(matroska::Dispatchable* service, HINSTANCE own) {
    GUID guid = language::kLegacyModuleGuid;
    void* args[] = {&guid};
    HINSTANCE module = nullptr;
    // Never call StartLanguageSupport with somebody else's GUID/module handle.
    if (!service->_dispatch(language::Message::FindDllHandleByGuid, &module, args, 1) ||
        !ValidPointer(module) || module == own) return nullptr;
    return module;
}

std::wstring ReadTranslation(matroska::Dispatchable* service, HINSTANCE module, unsigned id) {
    if (!module) return {};
    // GetStringW's public contract does not promise an empty result for a
    // nonexistent resource (a host may return a diagnostic). Verify the actual
    // language STRINGTABLE entry first. Zero-capacity LoadStringW exposes its
    // counted resource text without requiring or assuming NUL termination.
    const wchar_t* resource_text = nullptr;
    const int resource_length = LoadStringW(module, id,
        reinterpret_cast<LPWSTR>(&resource_text), 0);
    if (resource_length <= 0 || !resource_text ||
        resource_length >= static_cast<int>(kBufferCharacters) - 1) return {};
    std::array<wchar_t, kBufferCharacters> buffer;
    buffer.fill(static_cast<wchar_t>(0xffff)); // Detect missing termination, not just empty output.
    wchar_t* output = buffer.data();
    std::size_t capacity = buffer.size();
    // Both handles deliberately name ONLY the language module. Passing our DLL
    // as the original fallback could load unrelated colliding resource IDs.
    HINSTANCE original = module;
    void* args[] = {&module, &original, &id, &output, &capacity};
    wchar_t* returned = nullptr;
    if (!service->_dispatch(language::Message::GetStringW, &returned, args, 5) ||
        returned != buffer.data()) return {}; // Never dereference an unexpected pointer.
    const auto end = std::find(buffer.begin(), buffer.end(), L'\0');
    if (end == buffer.begin() || end == buffer.end() || end == buffer.end() - 1) return {};
    return {buffer.data(), static_cast<std::size_t>(end - buffer.begin())};
}

unsigned LegacyId(unsigned id) noexcept {
    // Public historical resource.h: IDS_NAME=55, IDS_VALUE=56. These are tag
    // field/column labels, not translated dialogs, control IDs, or formats.
    if (id == 20062) return 55;
    if (id == 20063) return 56;
    return 0;
}

void TranslateWindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0 || length >= static_cast<int>(kBufferCharacters) - 1) return;
    std::array<wchar_t, kBufferCharacters> text{};
    if (GetWindowTextW(window, text.data(), static_cast<int>(text.size())) != length) return;
    if (!LanguageStringId(text.data())) return;
    const std::wstring translated = Localized(text.data());
    if (translated != text.data()) SetWindowTextW(window, translated.c_str());
}

BOOL CALLBACK TranslateChild(HWND child, LPARAM parent) noexcept {
    // Do not descend into combo/edit internals or other child dialogs.
    if (GetParent(child) != reinterpret_cast<HWND>(parent)) return TRUE;
    try {
        wchar_t name[32]{};
        if (!GetClassNameW(child, name, static_cast<int>(sizeof(name) / sizeof(name[0])))) return TRUE;
        const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
        if (_wcsicmp(name, L"Static") == 0) {
            const LONG_PTR type = style & SS_TYPEMASK;
            if (type != SS_LEFT && type != SS_CENTER && type != SS_RIGHT &&
                type != SS_SIMPLE && type != SS_LEFTNOWORDWRAP) return TRUE;
            // These Static controls contain runtime status/data, not template
            // labels. Their fixed UI messages are localized by their owner.
            switch (GetDlgCtrlID(child)) {
            case IDC_INFO_WRITABLE_REASON: case IDC_CHAIN_LABEL: case IDC_INFO_STATUS:
            case IDC_CONFIG_QUALITY_VALUE: case IDC_ABOUT_TEXT: case IDC_UNIFIED_SUMMARY:
                return TRUE;
            default: break;
            }
        } else if (_wcsicmp(name, L"Button") == 0) {
            if ((style & (BS_ICON | BS_BITMAP)) || (style & BS_TYPEMASK) == BS_OWNERDRAW) return TRUE;
        } else return TRUE; // Includes read-only Edit controls and every editable/user field.
        TranslateWindowText(child);
    } catch (...) { /* Do not unwind through EnumChildWindows. */ }
    return TRUE;
}

} // namespace

const LanguageStringEntry* LanguageStrings(std::size_t& count) noexcept {
    count = kStringCount;
    return kStrings;
}

unsigned LanguageStringId(const wchar_t* fallback) noexcept {
    const std::size_t index = StringIndex(fallback);
    return index == kStringCount ? 0 : kStrings[index].id;
}

std::wstring Localized(const wchar_t* fallback) {
    if (!fallback) return {};
    const std::size_t index = StringIndex(fallback);
    if (index != kStringCount) {
        SharedCacheLock guard;
        if (!translations[index].empty()) return translations[index];
    }
    return fallback;
}

void ShutdownLanguage() noexcept {
    // No host or resource handles survive initialization, even when no module
    // was found. Teardown is consequently safe after the service manager dies.
    AcquireSRWLockExclusive(&cache_lock);
    for (auto& text : translations) text.clear();
    ReleaseSRWLockExclusive(&cache_lock);
}

void InitializeLanguage(HWND host, HINSTANCE own_module) noexcept {
    ShutdownLanguage();
    if (!host || !ValidPointer(own_module) || !OnWindowThread(host)) return;
    try {
        auto* manager = reinterpret_cast<matroska::ServiceManager*>(
            SendMessageW(host, matroska::kServiceManagerMessage, 0, matroska::kGetApiService));
        if (!ValidPointer(manager)) return;
        GUID guid = language::kServiceGuid;
        void* lookup[] = {&guid};
        matroska::ServiceFactory* factory = nullptr;
        if (!manager->_dispatch(language::kServiceByGuid, &factory, lookup, 1) ||
            !ValidPointer(factory)) return;
        LanguageInterface interface;
        interface.factory = factory;
        int global_lock = FALSE;
        void* request[] = {&global_lock};
        void* service = nullptr;
        if (!factory->_dispatch(matroska::ServiceFactoryMessage::GetInterface, &service, request, 1) ||
            !ValidPointer(service)) return;
        interface.service = static_cast<matroska::Dispatchable*>(service);
        const HINSTANCE module = OwnLanguageModule(interface.service, own_module);
        const HINSTANCE legacy = LegacyLanguageModule(interface.service, own_module);
        std::array<std::wstring, kStringCount> snapshot;
        for (std::size_t i = 0; i < kStringCount; ++i) {
            snapshot[i] = ReadTranslation(interface.service, module, kStrings[i].id);
            if (snapshot[i].empty()) {
                const unsigned legacy_id = LegacyId(kStrings[i].id);
                if (legacy_id) snapshot[i] = ReadTranslation(interface.service, legacy, legacy_id);
            }
        }
        AcquireSRWLockExclusive(&cache_lock);
        translations.swap(snapshot);
        ReleaseSRWLockExclusive(&cache_lock);
        // interface releases through the factory here. No host pointers cached.
    } catch (...) { /* All optional-service/alloc failures leave exact English. */ }
}

void LocalizeDialog(HWND dialog) {
    if (!dialog || !OnWindowThread(dialog)) return;
    TranslateWindowText(dialog);
    EnumChildWindows(dialog, TranslateChild, reinterpret_cast<LPARAM>(dialog));
}

} // namespace ogg_input
