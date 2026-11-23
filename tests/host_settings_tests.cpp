// SPDX-License-Identifier: GPL-3.0-only
#include "host_settings.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ogg_input;
using host_settings::Dispatchable;

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class T>
bool Argument(void** arguments, int count, T& value) {
    if (count != 1 || !arguments || !arguments[0]) return false;
    std::memcpy(&value, arguments[0], sizeof(value));
    return true;
}

template <class T>
int Result(void* destination, const T& value) {
    if (!destination) return 0;
    std::memcpy(destination, &value, sizeof(value));
    return 1;
}

bool SameGuid(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// Deliberately use literal public ABI values in the mock, not the resolver's
// message constants. A matching mistake in a wrapper and mock must not pass.
constexpr GUID kExpectedConfig = {
    0xAEFBF8BE, 0xE0AA, 0x4318, {0x8C, 0xC1, 0x43, 0x53, 0x41, 0x0B, 0x64, 0xDC}
};
constexpr GUID kExpectedPlayback = {
    0xB6CB4A7C, 0xA8D0, 0x4C55, {0x8E, 0x60, 0x9F, 0x7A, 0x7A, 0x23, 0xDA, 0x0F}
};
constexpr GUID kExpectedInternet = {
    0xC0A565DC, 0x0CFE, 0x405A, {0xA2, 0x7C, 0x46, 0x8B, 0x0C, 0x8A, 0x3A, 0x5C}
};

struct Counts {
    int ipc = 0;
    int lookups = 0;
    int served = 0;
    int released = 0;
    int playback = 0;
    int internet = 0;
    int writes = 0;
    int unexpected = 0;
};

struct MockItem final : Dispatchable {
    enum class Kind { Bool, Unsigned, Float, String } kind = Kind::Bool;
    Counts* counts = nullptr;
    unsigned char boolean = 0;
    std::uintptr_t number = 0;
    float db = 0;
    std::wstring text;
    bool overrideString = false;
    const wchar_t* stringReply = nullptr;
    bool supported = true;
    bool throwOnRead = false;
    bool omitResult = false;

    int _dispatch(int message, void* result, void**, int argumentCount) override {
        if (argumentCount != 0) { ++counts->unexpected; return 0; }
        switch (message) {
        case 40: case 60: case 80: case 100: case 130: case 160: case 180: case 200:
            ++counts->writes;
            return 0;
        default: break;
        }
        const int expected = kind == Kind::Bool ? 90 : kind == Kind::Unsigned ? 70 :
            kind == Kind::Float ? 190 : 30;
        if (message != expected) { ++counts->unexpected; return 0; }
        if (throwOnRead) throw std::runtime_error("mock read exception");
        if (!supported) return 0;
        if (omitResult) return 1;
        switch (kind) {
        case Kind::Bool: return Result(result, boolean);
        case Kind::Unsigned: return Result(result, number);
        case Kind::Float: return Result(result, db);
        case Kind::String: {
            const wchar_t* pointer = overrideString ? stringReply : text.c_str();
            return Result(result, pointer);
        }
        }
        return 0;
    }
};

struct MockGroup final : Dispatchable {
    explicit MockGroup(Counts& c) : counts(c) {}
    Counts& counts;
    std::map<std::wstring, MockItem> items;
    std::map<std::wstring, Dispatchable*> overrides;

    MockItem& Item(const wchar_t* name, MockItem::Kind kind) {
        auto& item = items[name];
        item.counts = &counts;
        item.kind = kind;
        return item;
    }
    MockItem& Bool(const wchar_t* name, unsigned char value) {
        auto& item = Item(name, MockItem::Kind::Bool);
        item.boolean = value;
        return item;
    }
    MockItem& Unsigned(const wchar_t* name, std::uintptr_t value) {
        auto& item = Item(name, MockItem::Kind::Unsigned);
        item.number = value;
        return item;
    }
    MockItem& Float(const wchar_t* name, float value) {
        auto& item = Item(name, MockItem::Kind::Float);
        item.db = value;
        return item;
    }
    MockItem& String(const wchar_t* name, const wchar_t* value) {
        auto& item = Item(name, MockItem::Kind::String);
        item.text = value;
        item.overrideString = false;
        return item;
    }

    int _dispatch(int message, void* result, void** arguments, int count) override {
        const wchar_t* name = nullptr;
        if (message != 10 || !Argument(arguments, count, name) || !name) {
            ++counts.unexpected;
            return 0;
        }
        Dispatchable* item = nullptr;
        const auto replacement = overrides.find(name);
        if (replacement != overrides.end()) item = replacement->second;
        else {
            const auto found = items.find(name);
            if (found != items.end()) item = &found->second;
        }
        return Result(result, item);
    }
};

struct MockConfig final : Dispatchable {
    explicit MockConfig(Counts& c) : counts(c) {}
    Counts& counts;
    Dispatchable* playback = nullptr;
    Dispatchable* internet = nullptr;
    bool supported = true;
    bool throwOnRead = false;

    int _dispatch(int message, void* result, void** arguments, int count) override {
        GUID guid{};
        if (message != 10 || !Argument(arguments, count, guid)) {
            ++counts.unexpected;
            return 0;
        }
        if (throwOnRead) throw std::runtime_error("mock group exception");
        if (!supported) return 0;
        Dispatchable* group = nullptr;
        if (SameGuid(guid, kExpectedPlayback)) { ++counts.playback; group = playback; }
        else if (SameGuid(guid, kExpectedInternet)) { ++counts.internet; group = internet; }
        else ++counts.unexpected;
        return Result(result, group);
    }
};

struct MockFactory final : Dispatchable {
    MockFactory(Counts& c, Dispatchable* api) : counts(c), validApi(api), reply(api) {}
    Counts& counts;
    Dispatchable* validApi = nullptr;
    Dispatchable* reply = nullptr;
    bool supported = true;

    int _dispatch(int message, void* result, void** arguments, int count) override {
        if (message == 300) {
            int globalLock = -1;
            if (!Argument(arguments, count, globalLock) || globalLock != 0) {
                ++counts.unexpected;
                return 0;
            }
            if (!supported) return 0;
            if (reply == validApi) ++counts.served;
            return Result(result, reply);
        }
        if (message == 310) {
            Dispatchable* returned = nullptr;
            if (!Argument(arguments, count, returned) || returned != validApi) {
                ++counts.unexpected;
                return 0;
            }
            ++counts.released;
            return Result(result, 1);
        }
        ++counts.unexpected;
        return 0;
    }
};

struct MockManager final : Dispatchable {
    MockManager(Counts& c, Dispatchable* f) : counts(c), factory(f) {}
    Counts& counts;
    Dispatchable* factory = nullptr;
    bool supported = true;

    int _dispatch(int message, void* result, void** arguments, int count) override {
        GUID guid{};
        if (message != 50 || !Argument(arguments, count, guid) || !SameGuid(guid, kExpectedConfig)) {
            ++counts.unexpected;
            return 0;
        }
        ++counts.lookups;
        return supported ? Result(result, factory) : 0;
    }
};

struct HostData {
    Counts* counts;
    Dispatchable* manager;
};

LRESULT CALLBACK MockWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_USER) {
        auto* data = reinterpret_cast<HostData*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!data) return 1;
        ++data->counts->ipc;
        if (wParam != 0 || lParam != 3025) { ++data->counts->unexpected; return 1; }
        return reinterpret_cast<LRESULT>(data->manager);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

class MockHostWindow {
public:
    explicit MockHostWindow(HostData& data) {
        WNDCLASSW windowClass{};
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpfnWndProc = MockWindowProcedure;
        windowClass.lpszClassName = L"IndependentOggHostSettingsTestWindow";
        Check(RegisterClassW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "register mock host class");
        window_ = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, windowClass.hInstance, &data);
        Check(window_ != nullptr, "create mock host window");
    }
    ~MockHostWindow() { if (window_) DestroyWindow(window_); }
    HWND get() const noexcept { return window_; }
private:
    HWND window_ = nullptr;
};

struct Fixture {
    Counts counts;
    MockGroup playback{counts}, internet{counts};
    MockConfig api{counts};
    MockFactory factory{counts, &api};
    MockManager manager{counts, &factory};
    HostData data{&counts, &manager};
    MockHostWindow host{data};

    Fixture() { api.playback = &playback; api.internet = &internet; }

    Settings Run(Settings local) {
        Settings effective = ResolveHostSettings(host.get(), std::move(local));
        Check(counts.writes == 0 && counts.unexpected == 0, "only documented read-only dispatches");
        Check(counts.served == counts.released, "configuration interface released exactly once");
        return effective;
    }
};

Settings Local() {
    Settings local;
    local.gain = GainMode::Album;
    local.preamp_q8 = 123;
    local.radio_preamp_q8 = -456;
    local.target_lufs = -27;
    local.non_rg_q8 = -777;
    local.peak_mode = 3;
    local.prevent_clipping = false;
    local.preferred_gain_only = true;
    local.proxy_mode = 2;
    local.proxy_url = L"local.example:9000";
    local.channels = ChannelMode::Quad;
    local.rate = 48000;
    local.bits = 24;
    local.quality = 7;
    local.save_directory = L"local-recordings";
    return local;
}

void SameGain(const Settings& actual, const Settings& expected) {
    Check(actual.gain == expected.gain && actual.preamp_q8 == expected.preamp_q8 &&
        actual.radio_preamp_q8 == expected.radio_preamp_q8 && actual.target_lufs == expected.target_lufs &&
        actual.non_rg_q8 == expected.non_rg_q8 && actual.peak_mode == expected.peak_mode &&
        actual.prevent_clipping == expected.prevent_clipping &&
        actual.preferred_gain_only == expected.preferred_gain_only, "local ReplayGain fallback preserved");
}

void SameProxy(const Settings& actual, const Settings& expected) {
    Check(actual.proxy_mode == expected.proxy_mode && actual.proxy_url == expected.proxy_url,
        "local proxy fallback preserved");
}

void SameUnrelated(const Settings& actual, const Settings& expected) {
    Check(actual.channels == expected.channels && actual.rate == expected.rate && actual.bits == expected.bits &&
        actual.quality == expected.quality && actual.dither == expected.dither &&
        actual.enhanced_speex == expected.enhanced_speex && actual.unified_info == expected.unified_info &&
        actual.buffer_small_files == expected.buffer_small_files && actual.instant_bitrate == expected.instant_bitrate &&
        actual.timeout_ms == expected.timeout_ms && actual.probe_kb == expected.probe_kb &&
        actual.prebuffer_kb == expected.prebuffer_kb && actual.stream_buffer_kb == expected.stream_buffer_kb &&
        actual.reconnect_attempts == expected.reconnect_attempts && actual.reconnect_delay_ms == expected.reconnect_delay_ms &&
        actual.save_streams == expected.save_streams && actual.save_directory == expected.save_directory &&
        actual.interactive_auth == expected.interactive_auth && actual.http_seek == expected.http_seek &&
        actual.underrun_percent == expected.underrun_percent &&
        actual.use_host_replaygain == expected.use_host_replaygain && actual.use_host_proxy == expected.use_host_proxy,
        "unrelated and persistent settings unchanged");
}

void SameSettings(const Settings& actual, const Settings& expected) {
    SameGain(actual, expected);
    SameProxy(actual, expected);
    SameUnrelated(actual, expected);
}

void EnableGain(Fixture& fixture, int source = 0, int mode = 1) {
    fixture.playback.Bool(L"replaygain", 1);
    fixture.playback.Unsigned(L"replaygain_source", static_cast<std::uintptr_t>(source));
    fixture.playback.Unsigned(L"replaygain_mode", static_cast<std::uintptr_t>(mode));
    fixture.playback.Float(L"replaygain_preamp", 2.5f);
    fixture.playback.Float(L"non_replaygain", -7.25f);
    fixture.playback.Bool(L"replaygain_preferred_only", 0);
}

void SetProxy(Fixture& fixture, const wchar_t* value, bool onlyPort80 = false) {
    fixture.internet.String(L"proxy", value);
    fixture.internet.Bool(L"proxy80", onlyPort80 ? 1 : 0);
}

void TestMissingHostAndServices() {
    const Settings local = Local();
    SameSettings(ResolveHostSettings(nullptr, local), local);
    SameSettings(ResolveHostSettings(reinterpret_cast<HWND>(1), local), local);
    Fixture fixture;
    for (const std::uintptr_t pointer : {std::uintptr_t(0), std::uintptr_t(1), std::uintptr_t(0x1234),
            (std::numeric_limits<std::uintptr_t>::max)()}) {
        fixture.data.manager = reinterpret_cast<Dispatchable*>(pointer);
        SameSettings(fixture.Run(local), local);
    }
    fixture.data.manager = &fixture.manager;
    fixture.manager.supported = false;
    SameSettings(fixture.Run(local), local);
    fixture.manager.supported = true;
    fixture.manager.factory = nullptr;
    SameSettings(fixture.Run(local), local);
    fixture.manager.factory = reinterpret_cast<Dispatchable*>(1);
    SameSettings(fixture.Run(local), local);
    fixture.manager.factory = &fixture.factory;
    fixture.factory.reply = nullptr;
    SameSettings(fixture.Run(local), local);
    fixture.factory.reply = reinterpret_cast<Dispatchable*>(1);
    SameSettings(fixture.Run(local), local);
    fixture.factory.reply = &fixture.api;
    fixture.factory.supported = false;
    SameSettings(fixture.Run(local), local);
    fixture.factory.supported = true;
    fixture.api.playback = fixture.api.internet = nullptr;
    SameSettings(fixture.Run(local), local);
    fixture.api.playback = &fixture.playback;
    fixture.api.internet = &fixture.internet;
    SameSettings(fixture.Run(local), local); // Service exists, but neither item exists.
    fixture.api.supported = false;
    SameSettings(fixture.Run(local), local);
    fixture.api.supported = true;
    fixture.api.throwOnRead = true;
    SameSettings(fixture.Run(local), local);
    Check(fixture.counts.served > 0, "missing-group tests exercised public service path");
}

void TestDisabledReplayGain() {
    Fixture fixture;
    Settings local = Local();
    local.gain = GainMode::Raw;
    local.use_host_proxy = false;
    EnableGain(fixture, 1, 2);
    fixture.playback.Bool(L"replaygain", 0);
    const auto effective = fixture.Run(local);
    Check(effective.gain == GainMode::Header, "disabled host RG retains Opus header gain, not raw output");
    Check(effective.preamp_q8 == 0 && effective.radio_preamp_q8 == 0,
        "disabled host RG does not retain local RG preamp");
    Check(effective.peak_mode == 0 && !effective.prevent_clipping, "disabled RG disables peak processing");
    Check(effective.target_lufs == local.target_lufs && effective.non_rg_q8 == local.non_rg_q8 &&
        effective.preferred_gain_only == local.preferred_gain_only, "disabled RG leaves irrelevant fallback fields alone");
    SameProxy(effective, local);
    SameUnrelated(effective, local);
    SameSettings(local, [&] { auto original = Local(); original.gain = GainMode::Raw;
        original.use_host_proxy = false; return original; }());
}

void TestReplayGainModes() {
    for (int source = 0; source != 2; ++source) {
        for (int mode = 0; mode != 4; ++mode) {
            Fixture fixture;
            EnableGain(fixture, source, mode);
            Settings local = Local();
            local.use_host_proxy = false;
            const auto effective = fixture.Run(local);
            Check(effective.gain == (source == 0 ? GainMode::Track : GainMode::Album), "track/album source mapping");
            Check(effective.peak_mode == mode && effective.prevent_clipping == (mode == 1),
                "all four host peak modes map to backend mode and clipping flag");
            Check(effective.preamp_q8 == 640 && effective.radio_preamp_q8 == 640, "host preamp used for files and radio");
            Check(effective.non_rg_q8 == -1856 && !effective.preferred_gain_only, "non-RG gain and fallback preference");
            Check(effective.target_lufs == -18, "conventional RG target also adjusts Opus R128 by five dB");
            SameProxy(effective, local);
            SameUnrelated(effective, local);
            Check(fixture.counts.ipc == 1 && fixture.counts.lookups == 1 && fixture.counts.playback == 1 &&
                fixture.counts.internet == 0, "single IPC acquisition and selected group only");
        }
    }
}

void TestMissingAndInvalidReplayGainItems() {
    Fixture fixture;
    const auto local = Local();
    EnableGain(fixture);
    fixture.playback.items.erase(L"replaygain");
    SameSettings(fixture.Run(local), local);
    fixture.playback.Bool(L"replaygain", 2);
    SameSettings(fixture.Run(local), local);
    fixture.playback.Bool(L"replaygain", 255);
    SameSettings(fixture.Run(local), local);
    fixture.playback.Bool(L"replaygain", 1).supported = false;
    SameSettings(fixture.Run(local), local);
    fixture.playback.items[L"replaygain"].supported = true;
    fixture.playback.items[L"replaygain"].throwOnRead = true;
    SameSettings(fixture.Run(local), local);
    fixture.playback.items[L"replaygain"].throwOnRead = false;
    fixture.playback.overrides[L"replaygain"] = reinterpret_cast<Dispatchable*>(1);
    SameSettings(fixture.Run(local), local);
    fixture.playback.overrides.clear();

    // Once enable is known, each missing/invalid optional value uses its local
    // field fallback; Winamp's fixed RG reference is the only implicit value.
    fixture.playback.Unsigned(L"replaygain_source", 2);
    fixture.playback.Unsigned(L"replaygain_mode", 4);
    fixture.playback.Float(L"replaygain_preamp", (std::numeric_limits<float>::quiet_NaN)());
    fixture.playback.Float(L"non_replaygain", (std::numeric_limits<float>::infinity)());
    fixture.playback.Bool(L"replaygain_preferred_only", 7);
    auto expected = local;
    expected.target_lufs = -18;
    SameSettings(fixture.Run(local), expected);
    fixture.playback.items.erase(L"replaygain_source");
    fixture.playback.items.erase(L"replaygain_mode");
    fixture.playback.items.erase(L"replaygain_preamp");
    fixture.playback.items.erase(L"non_replaygain");
    fixture.playback.items.erase(L"replaygain_preferred_only");
    SameSettings(fixture.Run(local), expected);
    fixture.playback.Bool(L"replaygain_preferred_only", 1);
    Check(fixture.Run(local).preferred_gain_only, "preferred-only true is supported");
}

void TestDbBoundsAndRounding() {
    Fixture fixture;
    EnableGain(fixture);
    const auto local = Local();
    for (float invalid : {(std::numeric_limits<float>::quiet_NaN)(),
            (std::numeric_limits<float>::infinity)(), -(std::numeric_limits<float>::infinity)(),
            (std::numeric_limits<float>::max)(), -60.01f, 30.01f}) {
        fixture.playback.Float(L"replaygain_preamp", invalid);
        fixture.playback.Float(L"non_replaygain", invalid);
        const auto effective = fixture.Run(local);
        Check(effective.preamp_q8 == local.preamp_q8 && effective.radio_preamp_q8 == local.radio_preamp_q8 &&
            effective.non_rg_q8 == local.non_rg_q8, "invalid dB value does not overwrite fallback or overflow");
    }
    const std::pair<float, int> valid[] = {
        {-60.f, -60 * 256}, {30.f, 30 * 256}, {-1.75f, -448}, {0.f, 0},
        {1.f / 512.f, 1}, {-1.f / 512.f, -1}
    };
    for (const auto& value : valid) {
        fixture.playback.Float(L"replaygain_preamp", value.first);
        fixture.playback.Float(L"non_replaygain", value.first);
        const auto effective = fixture.Run(local);
        Check(effective.preamp_q8 == value.second && effective.radio_preamp_q8 == value.second &&
            effective.non_rg_q8 == value.second, "finite dB bounds and symmetric Q8 rounding");
    }
    fixture.playback.Unsigned(L"replaygain_source", (std::numeric_limits<std::uintptr_t>::max)());
    fixture.playback.Unsigned(L"replaygain_mode", (std::numeric_limits<std::uintptr_t>::max)());
    const auto effective = fixture.Run(local);
    Check(effective.gain == local.gain && effective.peak_mode == local.peak_mode &&
        effective.prevent_clipping == local.prevent_clipping, "pointer-sized unsigned enum bounds");
}

void TestProxyPolicyAndEmptyProxy() {
    Fixture fixture;
    auto local = Local();
    local.use_host_replaygain = false;
    for (bool onlyPort80 : {false, true}) {
        SetProxy(fixture, L"http://proxy.example:8080/", onlyPort80);
        const auto effective = fixture.Run(local);
        Check(effective.proxy_url == L"proxy.example:8080", "HTTP proxy URL normalized for WinHTTP");
        Check(effective.proxy_mode == (onlyPort80 ? 1 : 2), "host proxy80 maps to port-80-only policy");
        SameGain(effective, local);
        SameUnrelated(effective, local);
    }
    SetProxy(fixture, L"");
    fixture.internet.items.erase(L"proxy80");
    auto effective = fixture.Run(local);
    Check(effective.proxy_mode == 0 && effective.proxy_url.empty(), "host empty proxy means direct, not automatic proxy");
    auto& item = fixture.internet.items[L"proxy"];
    item.overrideString = true;
    item.stringReply = nullptr;
    effective = fixture.Run(local);
    Check(effective.proxy_mode == 0 && effective.proxy_url.empty(), "successful null public proxy string means direct");
    fixture.internet.items.erase(L"proxy");
    SameSettings(fixture.Run(local), local);
    SetProxy(fixture, L"proxy.example:3128");
    fixture.internet.items.erase(L"proxy80");
    SameSettings(fixture.Run(local), local);
    fixture.internet.Bool(L"proxy80", 2);
    SameSettings(fixture.Run(local), local);
    fixture.internet.Bool(L"proxy80", 1).supported = false;
    SameSettings(fixture.Run(local), local);
    Check(fixture.counts.playback == 0, "proxy-only snapshot does not query ReplayGain group");
}

void TestProxyEndpointsAndCredentials() {
    Fixture fixture;
    auto local = Local();
    local.use_host_replaygain = false;
    const std::pair<const wchar_t*, const wchar_t*> valid[] = {
        {L"proxy.example:8080", L"proxy.example:8080"},
        {L"  HTTP://proxy.example:80/ \t", L"proxy.example:80"},
        {L"127.0.0.1:3128", L"127.0.0.1:3128"},
        {L"[::1]:3128", L"[::1]:3128"},
        {L"[2001:db8::1]", L"[2001:db8::1]"},
        {L"proxy", L"proxy"},
        {L"proxy.example:65535", L"proxy.example:65535"}
    };
    for (const auto& value : valid) {
        SetProxy(fixture, value.first);
        const auto effective = fixture.Run(local);
        Check(effective.proxy_url == value.second && effective.proxy_mode == 2, "accepted single HTTP proxy endpoint");
    }
    const wchar_t* invalid[] = {
        L"user:password@proxy.example:8080", L"http://user:password@proxy.example:8080/",
        L"http://user@proxy.example", L"http://user%3apassword%40proxy.example",
        L"https://proxy.example:443", L"socks5://proxy.example:1080", L"http://",
        L"proxy.example:0", L"proxy.example:65536", L"proxy.example:-1", L"proxy.example:abc",
        L"proxy.example:", L"proxy.example:000080", L"proxy.example:80/path",
        L"proxy.example:80?password=secret", L"proxy.example:80#fragment", L"proxy.example:80\\path",
        L"proxy.example:80\r\nAuthorization: secret", L"http=proxy.example:80;https=another:80",
        L"proxy.example:80 other.example:80", L".example:80", L"-example:80", L"example-:80",
        L"example..test:80", L"[]:80", L"[::1]:", L"[::1", L"[::1]:65536", L"::1:80",
        L"proxy.éxample:80"
    };
    for (const auto* value : invalid) {
        SetProxy(fixture, value);
        SameSettings(fixture.Run(local), local);
    }
    // The persistent object supplied by the caller is never rewritten, even
    // when a host URL contains credentials. Only our bounded stack copy saw it.
    Check(local.proxy_url == L"local.example:9000", "host credentials never enter caller settings");
}

class GuardedMemory {
public:
    GuardedMemory() {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        pageSize = system.dwPageSize;
        memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, pageSize * 2,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(memory != nullptr, "allocate bounded-pointer test pages");
        DWORD old = 0;
        Check(VirtualProtect(memory + pageSize, pageSize, PAGE_NOACCESS, &old) != 0,
            "protect bounded-pointer test page");
    }
    ~GuardedMemory() { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
    unsigned char* memory = nullptr;
    std::size_t pageSize = 0;
};

void TestBoundedHostMemory() {
    Fixture fixture;
    auto local = Local();
    local.use_host_replaygain = false;
    SetProxy(fixture, L"proxy.example:8080");
    auto& proxy = fixture.internet.items[L"proxy"];
    proxy.overrideString = true;
    for (std::uintptr_t pointer : {std::uintptr_t(1), (std::numeric_limits<std::uintptr_t>::max)()}) {
        proxy.stringReply = reinterpret_cast<const wchar_t*>(pointer);
        SameSettings(fixture.Run(local), local);
    }
    std::vector<wchar_t> unbounded(host_settings::kMaxProxyCharacters + 2, L'a');
    proxy.stringReply = unbounded.data();
    SameSettings(fixture.Run(local), local);

    GuardedMemory guarded;
    auto* edge = reinterpret_cast<wchar_t*>(guarded.memory + guarded.pageSize - 4 * sizeof(wchar_t));
    const wchar_t endpoint[] = L"a:80";
    std::memcpy(edge, endpoint, 4 * sizeof(wchar_t)); // No terminator before the unreadable page.
    proxy.stringReply = edge;
    SameSettings(fixture.Run(local), local);
    edge = reinterpret_cast<wchar_t*>(guarded.memory + guarded.pageSize - sizeof(endpoint));
    std::memcpy(edge, endpoint, sizeof(endpoint));
    proxy.stringReply = edge;
    Check(fixture.Run(local).proxy_url == L"a:80", "bounded string may terminate at readable page edge");

    DWORD old = 0;
    Check(VirtualProtect(guarded.memory + guarded.pageSize, guarded.pageSize,
        PAGE_READWRITE | PAGE_GUARD, &old) != 0, "set guard page");
    proxy.stringReply = reinterpret_cast<const wchar_t*>(guarded.memory + guarded.pageSize);
    SameSettings(fixture.Run(local), local);
    MEMORY_BASIC_INFORMATION region{};
    Check(VirtualQuery(guarded.memory + guarded.pageSize, &region, sizeof(region)) != 0 &&
        (region.Protect & PAGE_GUARD), "guard page rejected without touching or consuming guard");

    fixture.data.manager = reinterpret_cast<Dispatchable*>(guarded.memory + guarded.pageSize);
    SameSettings(fixture.Run(local), local);
    // A readable fake object/vtable must not dispatch into non-executable data.
    int data = 0;
    const void* table[] = {&data};
    const void* object[] = {table};
    fixture.data.manager = reinterpret_cast<Dispatchable*>(const_cast<void**>(object));
    SameSettings(fixture.Run(local), local);
}

void TestIndependentMissingGroupsAndGetterFailures() {
    Fixture fixture;
    EnableGain(fixture);
    SetProxy(fixture, L"host.example:8080", true);
    const auto local = Local();
    fixture.api.playback = nullptr;
    auto effective = fixture.Run(local);
    SameGain(effective, local);
    Check(effective.proxy_url == L"host.example:8080" && effective.proxy_mode == 1,
        "missing playback group does not disable host proxy inheritance");
    fixture.api.playback = &fixture.playback;
    fixture.api.internet = nullptr;
    effective = fixture.Run(local);
    Check(effective.gain == GainMode::Track, "missing internet group does not disable host ReplayGain");
    SameProxy(effective, local);
    fixture.api.internet = &fixture.internet;
    auto& enable = fixture.playback.items[L"replaygain"];
    enable.omitResult = true;
    effective = fixture.Run(local);
    SameGain(effective, local);
    Check(effective.proxy_mode == 1, "malformed enable reply is not interpreted as disabled RG");
    enable.omitResult = false;
    auto& proxy = fixture.internet.items[L"proxy"];
    proxy.omitResult = true;
    effective = fixture.Run(local);
    Check(effective.gain == GainMode::Track, "invalid proxy getter does not disable host ReplayGain");
    SameProxy(effective, local);
    proxy.omitResult = false;
    proxy.supported = false;
    SameProxy(fixture.Run(local), local);
    proxy.supported = true;
    proxy.throwOnRead = true;
    SameProxy(fixture.Run(local), local);
    proxy.throwOnRead = false;
    enable.throwOnRead = true;
    effective = fixture.Run(local);
    SameGain(effective, local);
    Check(effective.proxy_mode == 1, "exception from one group does not suppress the other group");
}

void TestIndependentOverridesAndRefresh() {
    Fixture fixture;
    EnableGain(fixture);
    SetProxy(fixture, L"first.example:80", true);
    auto local = Local();
    local.use_host_replaygain = local.use_host_proxy = false;
    SameSettings(fixture.Run(local), local);
    Check(fixture.counts.ipc == 0, "explicit local override performs no host IPC");
    local.use_host_replaygain = true;
    auto effective = fixture.Run(local);
    Check(effective.gain == GainMode::Track, "host ReplayGain independently enabled");
    SameProxy(effective, local);
    Check(fixture.counts.internet == 0, "local proxy override does not read host proxy");
    local.use_host_replaygain = false;
    local.use_host_proxy = true;
    effective = fixture.Run(local);
    SameGain(effective, local);
    Check(effective.proxy_url == L"first.example:80" && effective.proxy_mode == 1,
        "host proxy independently enabled");
    Check(fixture.counts.playback == 1, "local RG override does not query host RG");
    local.use_host_replaygain = true;
    fixture.playback.Bool(L"replaygain", 0);
    SetProxy(fixture, L"second.example:8080", false);
    effective = fixture.Run(local);
    Check(effective.gain == GainMode::Header && effective.proxy_url == L"second.example:8080" &&
        effective.proxy_mode == 2, "new playback snapshot observes live host changes without caching");
    Check(fixture.counts.ipc == 3 && fixture.counts.served == 3 && fixture.counts.released == 3,
        "no stale service pointer retained across snapshots");
}

} // namespace

int main() {
    try {
        static_assert(sizeof(void*) == 4, "Run these public Winamp ABI tests as Win32.");
        Check(SameGuid(host_settings::kConfigServiceGuid, kExpectedConfig) &&
            SameGuid(host_settings::kPlaybackGroupGuid, kExpectedPlayback) &&
            SameGuid(host_settings::kInternetGroupGuid, kExpectedInternet), "public configuration GUIDs");
        TestMissingHostAndServices();
        TestDisabledReplayGain();
        TestReplayGainModes();
        TestMissingAndInvalidReplayGainItems();
        TestDbBoundsAndRounding();
        TestProxyPolicyAndEmptyProxy();
        TestProxyEndpointsAndCredentials();
        TestBoundedHostMemory();
        TestIndependentMissingGroupsAndGetterFailures();
        TestIndependentOverridesAndRefresh();
        std::cout << "Host settings mock IPC/service tests passed (no installed host or network).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
