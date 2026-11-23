// SPDX-License-Identifier: GPL-3.0-only
#include "host_settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace ogg_input {
namespace {

using host_settings::Dispatchable;
namespace Message = host_settings::Message;

constexpr UINT kIpcTimeoutMs = 250;

// Host replies are in-process pointers, not marshalled IPC data. Refuse remote
// windows, old Winamp's unsupported-IPC sentinel (1), inaccessible memory and
// non-executable vtable entries before dispatching. SEH also covers a pointer
// becoming invalid between the memory query and its use. This is defensive
// fault containment, not a sandbox for a malicious in-process host.
bool Readable(const void* pointer, std::size_t bytes) noexcept {
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address < 65536 || bytes > (std::numeric_limits<std::uintptr_t>::max)() - address)
        return false;
    const auto end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        switch (region.Protect & 0xff) {
        case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
            break;
        default: return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
        const std::uintptr_t next = base + static_cast<std::uintptr_t>(region.RegionSize);
        if (next <= address) return false;
        address = (std::min)(end, next);
    }
    return true;
}

int MemoryFaultFilter(DWORD code) noexcept {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool CopyMemoryChecked(void* destination, const void* source, std::size_t bytes) noexcept {
    if (!Readable(source, bytes)) return false;
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (MemoryFaultFilter(GetExceptionCode())) {
        return false;
    }
}

bool Callable(Dispatchable* object) noexcept {
    const void* table = nullptr;
    const void* function = nullptr;
    if (!CopyMemoryChecked(&table, object, sizeof(table)) ||
        !CopyMemoryChecked(&function, table, sizeof(function))) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(function, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    switch (region.Protect & 0xff) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

// Kept separate from C++ exception handling: MSVC cannot unwind C++ objects in
// a function containing __try. Host C++ exceptions still reach Dispatch().
bool GuardedDispatch(Dispatchable* object, int message, void* result,
    void** arguments, int argumentCount) {
    __try {
        return object->_dispatch(message, result, arguments, argumentCount) != 0;
    } __except (MemoryFaultFilter(GetExceptionCode())) {
        return false;
    }
}

bool Dispatch(Dispatchable* object, int message, void* result,
    void** arguments = nullptr, int argumentCount = 0) noexcept {
    if (!Callable(object)) return false;
    try {
        return GuardedDispatch(object, message, result, arguments, argumentCount);
    } catch (...) {
        return false;
    }
}

Dispatchable* GetByGuid(Dispatchable* object, int message, GUID guid) noexcept {
    void* arguments[] = {&guid};
    Dispatchable* value = nullptr;
    return Dispatch(object, message, &value, arguments, 1) && Callable(value) ? value : nullptr;
}

Dispatchable* GetItem(Dispatchable* group, const wchar_t* name) noexcept {
    if (!group) return nullptr;
    void* arguments[] = {&name};
    Dispatchable* value = nullptr;
    return Dispatch(group, Message::GetItem, &value, arguments, 1) && Callable(value) ? value : nullptr;
}

bool ReadBool(Dispatchable* group, const wchar_t* name, bool& value) noexcept {
    auto* item = GetItem(group, name);
    // Read into a byte, not bool: malformed host bool representations must not
    // enter a C++ bool object or silently turn an unsupported item into false.
    unsigned char raw = 0xff;
    if (!Dispatch(item, Message::GetBool, &raw) || raw > 1) return false;
    value = raw != 0;
    return true;
}

bool ReadUnsigned(Dispatchable* group, const wchar_t* name,
    std::uintptr_t maximum, int& value) noexcept {
    auto* item = GetItem(group, name);
    std::uintptr_t raw = (std::numeric_limits<std::uintptr_t>::max)();
    if (!Dispatch(item, Message::GetUnsigned, &raw) || raw > maximum) return false;
    value = static_cast<int>(raw);
    return true;
}

bool ReadDb(Dispatchable* group, const wchar_t* name, int& q8) noexcept {
    auto* item = GetItem(group, name);
    float raw = (std::numeric_limits<float>::quiet_NaN)();
    if (!Dispatch(item, Message::GetFloat, &raw) || !std::isfinite(raw) || raw < -60.f || raw > 30.f)
        return false;
    q8 = static_cast<int>(std::lround(static_cast<double>(raw) * 256.));
    return true;
}

class ConfigSnapshot {
public:
    explicit ConfigSnapshot(HWND host) noexcept {
        if (!host || !IsWindow(host)) return;
        DWORD process = 0;
        if (!GetWindowThreadProcessId(host, &process) || process != GetCurrentProcessId()) return;
        DWORD_PTR reply = 0;
        if (!SendMessageTimeoutW(host, host_settings::kIpcMessage, 0,
                host_settings::kGetApiService, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                kIpcTimeoutMs, &reply)) return;
        auto* manager = reinterpret_cast<Dispatchable*>(reply);
        factory_ = GetByGuid(manager, Message::FindService, host_settings::kConfigServiceGuid);
        if (!factory_) return;
        int globalLock = 0;
        void* arguments[] = {&globalLock};
        Dispatchable* api = nullptr;
        if (Dispatch(factory_, Message::GetInterface, &api, arguments, 1) && Callable(api)) api_ = api;
    }

    ~ConfigSnapshot() {
        if (!api_) return;
        void* arguments[] = {&api_};
        int ignored = 0;
        (void)Dispatch(factory_, Message::ReleaseInterface, &ignored, arguments, 1);
    }

    ConfigSnapshot(const ConfigSnapshot&) = delete;
    ConfigSnapshot& operator=(const ConfigSnapshot&) = delete;

    Dispatchable* Group(GUID guid) const noexcept {
        return api_ ? GetByGuid(api_, Message::GetGroup, guid) : nullptr;
    }

private:
    Dispatchable* factory_ = nullptr;
    Dispatchable* api_ = nullptr;
};

void ResolveReplayGain(Dispatchable* group, Settings& settings) noexcept {
    bool enabled = false;
    // Without an actual, supported enable item, preserve the entire local RG
    // choice. In particular, an absent config service is not "RG disabled".
    if (!ReadBool(group, L"replaygain", enabled)) return;
    if (!enabled) {
        settings.gain = GainMode::Header; // Never Raw: Opus header gain survives.
        settings.preamp_q8 = settings.radio_preamp_q8 = 0;
        settings.peak_mode = 0;
        settings.prevent_clipping = false;
        return;
    }

    int source = 0;
    if (ReadUnsigned(group, L"replaygain_source", 1, source))
        settings.gain = source == 0 ? GainMode::Track : GainMode::Album;
    int mode = 0;
    if (ReadUnsigned(group, L"replaygain_mode", 3, mode)) {
        settings.peak_mode = mode; // 0 gain, 1 gain/no-clip, 2 normalize, 3 no-clip only.
        settings.prevent_clipping = mode == 1;
    }
    int preamp = 0;
    if (ReadDb(group, L"replaygain_preamp", preamp))
        settings.preamp_q8 = settings.radio_preamp_q8 = preamp;
    (void)ReadDb(group, L"non_replaygain", settings.non_rg_q8);
    (void)ReadBool(group, L"replaygain_preferred_only", settings.preferred_gain_only);
    // Winamp uses conventional ReplayGain's 89 dB / -18 LUFS reference. For
    // Opus R128 tags (referenced to -23 LUFS), Audio::Gain adds the needed +5 dB.
    settings.target_lufs = -18;
}

struct ProxyBuffer {
    std::array<wchar_t, host_settings::kMaxProxyCharacters + 1> text{};
    ~ProxyBuffer() { SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t)); }
};

bool ReadProxyString(Dispatchable* group, ProxyBuffer& buffer, std::size_t& length) noexcept {
    auto* item = GetItem(group, L"proxy");
    const wchar_t* value = reinterpret_cast<const wchar_t*>(std::uintptr_t{1});
    if (!Dispatch(item, Message::GetString, &value)) return false;
    length = 0;
    // The public InternetConfigGroup uses a successful null string for "no proxy".
    if (!value) return true;
    const auto address = reinterpret_cast<std::uintptr_t>(value);
    if (address < 65536 || host_settings::kMaxProxyCharacters * sizeof(wchar_t) >
            (std::numeric_limits<std::uintptr_t>::max)() - address) return false;
    for (; length <= host_settings::kMaxProxyCharacters; ++length) {
        const auto* character = reinterpret_cast<const void*>(address + length * sizeof(wchar_t));
        if (!CopyMemoryChecked(&buffer.text[length], character, sizeof(wchar_t))) return false;
        if (!buffer.text[length]) return true;
    }
    return false;
}

bool AsciiHostCharacter(wchar_t c) noexcept {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
        (c >= L'0' && c <= L'9') || c == L'-' || c == L'.';
}

bool HexCharacter(wchar_t c) noexcept {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// Accept one credential-free HTTP proxy endpoint, not a WinHTTP proxy list,
// PAC URL, arbitrary URL path or a different protocol. Never strip credentials
// and silently use the remainder; a rejected host value keeps the local choice.
bool ProxyEndpoint(const wchar_t* text, std::size_t length, std::wstring& endpoint) {
    while (length && (*text == L' ' || *text == L'\t')) { ++text; --length; }
    while (length && (text[length - 1] == L' ' || text[length - 1] == L'\t')) --length;
    if (!length) { endpoint.clear(); return true; }
    if (length >= 7 && _wcsnicmp(text, L"http://", 7) == 0) {
        text += 7;
        length -= 7;
        if (length && text[length - 1] == L'/') --length;
    }
    if (!length) return false;
    // This rejects URL userinfo (also percent-encoded userinfo), header/list
    // injection, paths and schemes we cannot honor without changing security.
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = text[i];
        if (c <= 0x20 || c >= 0x7f || c == L'@' || c == L'%' || c == L'/' || c == L'\\' ||
            c == L'?' || c == L'#' || c == L';' || c == L'=' || c == L'\"' || c == L'\'') return false;
    }

    std::size_t hostEnd = 0;
    if (text[0] == L'[') {
        std::size_t colons = 0;
        hostEnd = 1;
        while (hostEnd < length && text[hostEnd] != L']') {
            const auto c = text[hostEnd++];
            if (c == L':') ++colons;
            else if (!HexCharacter(c) && c != L'.') return false;
        }
        if (hostEnd == length || hostEnd == 1 || colons < 2) return false;
        ++hostEnd;
    } else {
        while (hostEnd < length && text[hostEnd] != L':') {
            if (!AsciiHostCharacter(text[hostEnd])) return false;
            ++hostEnd;
        }
        if (!hostEnd || hostEnd > 253 || text[0] == L'.' || text[0] == L'-' ||
            text[hostEnd - 1] == L'-') return false;
        for (std::size_t i = 1; i < hostEnd; ++i) {
            if (text[i] == L'.' && text[i - 1] == L'.') return false;
        }
    }
    if (hostEnd < length) {
        if (text[hostEnd] != L':' || ++hostEnd == length || length - hostEnd > 5) return false;
        unsigned port = 0;
        for (std::size_t i = hostEnd; i < length; ++i) {
            if (text[i] < L'0' || text[i] > L'9') return false;
            port = port * 10 + static_cast<unsigned>(text[i] - L'0');
        }
        if (!port || port > 65535) return false;
    }
    endpoint.assign(text, length);
    return true;
}

void ResolveProxy(Dispatchable* group, Settings& settings) {
    ProxyBuffer buffer;
    std::size_t length = 0;
    if (!ReadProxyString(group, buffer, length)) return;
    std::wstring endpoint;
    if (!ProxyEndpoint(buffer.text.data(), length, endpoint)) return;
    if (endpoint.empty()) {
        // Empty host configuration means direct, not Windows' automatic proxy.
        settings.proxy_url.clear();
        settings.proxy_mode = 0;
        return;
    }
    bool onlyPort80 = false;
    // Do not widen an unknown host restriction by assuming proxy80 is false.
    // For nonempty endpoints the endpoint and policy form one atomic snapshot.
    if (!ReadBool(group, L"proxy80", onlyPort80)) return;
    settings.proxy_url = std::move(endpoint);
    settings.proxy_mode = onlyPort80 ? 1 : 2;
}

} // namespace

Settings ResolveHostSettings(HWND host, Settings local) noexcept {
    if (!local.use_host_replaygain && !local.use_host_proxy) return local;
    try {
        ConfigSnapshot snapshot(host);
        if (local.use_host_replaygain)
            ResolveReplayGain(snapshot.Group(host_settings::kPlaybackGroupGuid), local);
        if (local.use_host_proxy)
            ResolveProxy(snapshot.Group(host_settings::kInternetGroupGuid), local);
    } catch (...) {
        // Only proxy string allocation can throw within our own implementation.
        // No persistent settings, credentials or host configuration are written.
    }
    return local;
}

} // namespace ogg_input
