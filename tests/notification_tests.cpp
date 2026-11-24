// SPDX-License-Identifier: GPL-3.0-only
#include "codec_fixtures.hpp"
#include "registry_sandbox.hpp"
#include "settings.hpp"
#include "winamp_input.hpp"
#include <atomic>
#include <functional>
#include <iostream>
#include <vector>

using namespace ogg_input;
namespace {
struct Format { int rate, channels; };
struct Update { int bitrate, rate, channels; std::size_t generation; };
std::vector<Format> formats;
std::vector<Update> updates;
std::atomic<int> errors{0}, eof{0};
std::atomic<long long> written{0};
std::atomic<int> flushes{0};
struct SeekRequest { int write; std::vector<int> positions; };
std::vector<SeekRequest> seeks;
std::atomic<int> writes{0}, canwrite_calls{0}, playing_calls{0};
int pause_at = -1;
int title_updates = 0;
winamp::InModule* input = nullptr;
void Verify(bool value) { if (!value) ++errors; }
int __cdecl Open(int rate, int channels, int bits, int, int) {
    Verify(rate > 0 && channels > 0 && bits == 16);
    formats.push_back({rate, channels}); return 0;
}
void __cdecl Close() {}
int __cdecl Write(char* data, int bytes) {
    const int frame_bytes = formats.back().channels * 2;
    Verify(data && bytes > 0 && bytes <= 8192 && bytes % frame_bytes == 0);
    written += bytes / frame_bytes;
    // Keep playback alive across several 250-ms bitrate updates without a device.
    Sleep(8);
    ++writes;
    if (writes == pause_at) input->Pause();
    for (const auto& seek : seeks) if (seek.write == writes)
        for (int position : seek.positions) input->SetOutputTime(position);
    return 0;
}
int __cdecl CanWrite() { ++canwrite_calls; return 8192; }
int __cdecl Playing() { ++playing_calls; return 0; }
int __cdecl Pause(int value) { return value; }
void __cdecl Flush(int) { ++flushes; }
int __cdecl Time() { return 0; }
void __cdecl SetInfo(int bitrate, int rate, int channels, int sync) {
    Verify((bitrate > 0 || bitrate == -1) && sync == 1 && !formats.empty());
    if (formats.empty()) return;
    const auto& format = formats.back();
    Verify((rate == format.rate / 1000 && channels == format.channels) || (rate == -1 && channels == -1));
    updates.push_back({bitrate, rate, channels, formats.size() - 1});
}
LRESULT CALLBACK Host(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == winamp::kEndOfFile) { ++eof; return 0; }
    if (message == WM_USER && l == 243) {
        ++title_updates;
        wchar_t title[winamp::kTitleCapacity]{};
        input->GetFileInfo(nullptr, title, nullptr);
        Verify(*title != 0);
        return 0;
    }
    if (message == WM_USER) return 0; // No host services or settings overrides.
    return DefWindowProcW(window, message, w, l);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void Run(const char* name, const std::filesystem::path& path, bool instantaneous,
         const std::vector<Format>& expected, long long frames, bool chained, int expected_kbps = 0) {
    Settings settings; settings.instant_bitrate = instantaneous;
    settings.channels = expected.size() == 1 ? ChannelMode::Stereo : ChannelMode::Source;
    settings.use_host_replaygain = settings.use_host_proxy = false;
    Check(SaveSettings(settings), "save isolated notification settings");
    input->Init();
    Pump(); formats.clear(); updates.clear(); seeks.clear(); writes = 0; flushes = 0;
    errors = eof = 0; written = 0; title_updates = 0;
    const auto started = GetTickCount64();
    Check(input->Play(path.c_str()) == 0, "play notification fixture");
    while (!eof && GetTickCount64() - started < 15000) { Pump(); Sleep(1); }
    Pump(); input->Stop(); Pump(); // Join the worker before inspecting its records.
    Check(eof == 1 && written == frames, "notification playback frame count and EOF");
    Check(GetTickCount64() - started >= 1000, "exercise multiple bitrate intervals");
    Check(errors == 0, "notification callback contract");
    if (expected_kbps) Check(!updates.empty() && updates.front().bitrate == expected_kbps,
        "compressed FLAC bitrate reaches the initial Winamp SetInfo callback");
    Check(formats.size() == expected.size(), "output reopen count");
    for (std::size_t i = 0; i < expected.size(); ++i)
        Check(formats[i].rate == expected[i].rate && formats[i].channels == expected[i].channels,
            "actual source format changes are preserved");
    std::vector<int> full(expected.size(), 0);
    int displayed = -1;
    for (const auto& update : updates) {
        Check(update.generation < full.size(), "notification output generation");
        if (update.rate != -1) {
            ++full[update.generation];
        } else {
            Check(full[update.generation] == 1 && update.channels == -1,
                "bitrate-only update preserves the initialized format");
            Check(update.bitrate != displayed, "skip unchanged displayed bitrate");
        }
        if (update.bitrate > 0) displayed = update.bitrate;
        if (&update != &updates.front())
            Check(update.bitrate == -1 || update.rate == -1, "format changes must not implicitly refresh playlist titles");
    }
    for (const int count : full)
        Check(count == 1, "periodic bitrate updates must not resend the full format and refresh playlist titles");
    if (!instantaneous && expected.size() == 1) Check(updates.size() == 1, "average bitrate mode has no periodic notifications");
    else if (chained && expected.size() == 1) {
        Check(updates.size() >= 3, "changed instantaneous bitrates still reach Winamp");
        Check(updates.back().bitrate > updates[1].bitrate, "the higher bitrate next link is displayed");
    }
    Check(title_updates == (chained ? 1 : 0), "real chained titles update without periodic title IPC");
    std::cout << name << ": SetInfo=" << updates.size() << ", full-format=" << formats.size()
        << ", title=" << title_updates << '\n';
}
void WaitUntil(const std::function<bool()>& ready, const char* message) {
    const auto started = GetTickCount64();
    while (!ready() && GetTickCount64() - started < 15000) { Pump(); Sleep(1); }
    Check(ready(), message);
}
void RunIdle(const std::filesystem::path& path) {
    Settings settings; settings.instant_bitrate = false;
    settings.use_host_replaygain = settings.use_host_proxy = false;
    Check(SaveSettings(settings), "save idle notification settings"); input->Init();
    Pump(); formats.clear(); updates.clear(); seeks.clear(); errors = eof = 0; written = 0;
    title_updates = 0; writes = 0; flushes = 0; pause_at = 4;
    Check(input->Play(path.c_str()) == 0, "play idle fixture");
    WaitUntil([] { return input->IsPaused() != 0; }, "pause enters event wait");
    const int before = writes;
    input->SetOutputTime(100);
    WaitUntil([] { return flushes.load() == 1; }, "seek wakes paused worker");
    Sleep(80); Pump();
    Check(writes == before && title_updates == 0, "paused seek neither decodes nor redraws playlist");
    input->UnPause();
    WaitUntil([] { return eof.load() == 1; }, "resume wakes worker to EOF");
    Sleep(30); const int queries = canwrite_calls, playing = playing_calls;
    Sleep(80); Pump();
    Check(canwrite_calls == queries && playing_calls == playing, "EOF stops polling the output plugin");
    input->SetOutputTime(1800);
    WaitUntil([] { return eof.load() == 2; }, "seek restarts playback after EOF");
    const auto stopped = GetTickCount64(); input->Stop(); Pump();
    Check(GetTickCount64() - stopped < 1000, "stop wakes an idle worker");
    Check(formats.size() == 1 && updates.size() == 1 && title_updates == 0 && errors == 0,
        "idle control changes do not republish format or playlist metadata");
    pause_at = -1;
}
void RunSeeks(const char* name, const std::filesystem::path& path, int expected_titles, bool fixed_rate = true) {
    Settings settings; settings.instant_bitrate = false; settings.rate = fixed_rate ? 48000 : 0;
    settings.channels = ChannelMode::Stereo;
    settings.use_host_replaygain = settings.use_host_proxy = false;
    Check(SaveSettings(settings), "save seek notification settings");
    input->Init();
    Pump(); formats.clear(); updates.clear(); errors = eof = 0; written = 0;
    title_updates = 0; writes = 0; flushes = 0;
    seeks = {{4, {300, 500, 1400}}, {8, {1600}}, {12, {100}}};
    Check(input->Play(path.c_str()) == 0, "play repeated seek fixture");
    const auto started = GetTickCount64();
    while (!eof && GetTickCount64() - started < 15000) { Pump(); Sleep(1); }
    input->Stop(); Pump();
    Check(eof == 1 && errors == 0, "seek playback completes without ABI errors");
    Check(formats.size() == (fixed_rate ? 1u : 4u), "seek reopens output only for actual format changes");
    Check(flushes == static_cast<int>(3 + formats.size() - 1), "seek burst coalesces without redundant flushes");
    if (fixed_rate) Check(updates.size() == 1, "seek does not resend the output format or status");
    for (std::size_t i = 1; i < updates.size(); ++i)
        Check(updates[i].bitrate == -1 || updates[i].rate == -1, "format negotiation must not trigger playlist redraws");
    std::cout << name << " repeated seeks: title=" << title_updates << '\n';
    Check(title_updates == expected_titles, "seek and unchanged chained tags must not refresh the playlist");
    seeks.clear();
}
}
int wmain(int argc, wchar_t** argv) {
    HMODULE dll = nullptr; HWND window = nullptr;
    try {
        Check(argc == 3, "usage: ogg_notification_tests DLL DIRECTORY");
        RegistrySandbox registry;
        const std::filesystem::path directory(argv[2]); std::filesystem::create_directories(directory);
        const auto steady = directory / L"steady.oga", chain = directory / L"bitrate-chain.oga";
        const auto first = directory / L"first.oga", second = directory / L"second.oga";
        const auto vorbis = directory / L"vorbis.ogg", native = directory / L"native-chain.ogg", tail = directory / L"tail.ogg";
        MakeOpus(steady, 2, 96000, 15001);
        MakeOpus(first, 2, 48000, 15002);
        MakeOpus(second, 6, 48000, 15003, {"TITLE=Second track"});
        std::filesystem::copy_file(first, chain, std::filesystem::copy_options::overwrite_existing); AppendFile(chain, second);
        MakeVorbis(vorbis, 2, 44100, 88200, 15004);
        MakeVorbis(native, 6, 44100, 44100, 15005); MakeVorbis(tail, 2, 48000, 48000, 15006); AppendFile(native, tail);
        const auto unchanged = directory / L"unchanged.oga";
        MakeOpus(second, 2, 48000, 15007);
        std::filesystem::copy_file(first, unchanged, std::filesystem::copy_options::overwrite_existing); AppendFile(unchanged, second);
        const auto tags_only = directory / L"tags-only.oga";
        MakeOpus(second, 2, 48000, 15008, {"ALBUM=Second album"});
        std::filesystem::copy_file(first, tags_only, std::filesystem::copy_options::overwrite_existing); AppendFile(tags_only, second);
        WNDCLASSW cls{}; cls.lpfnWndProc = Host; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"OggNotificationTest";
        Check(RegisterClassW(&cls) != 0, "notification host class");
        window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
        Check(window != nullptr, "notification host window");
        dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(dll != nullptr, "load notification DLL");
        const auto get = reinterpret_cast<winamp::InModule* (__cdecl*)()>(GetProcAddress(dll, "winampGetInModule2"));
        Check(get != nullptr, "notification input export"); input = get(); input->hDllInstance = dll; input->hMainWindow = window;
        winamp::OutModule out{}; out.Open = Open; out.Close = Close; out.Write = Write; out.CanWrite = CanWrite;
        out.IsPlaying = Playing; out.Pause = Pause; out.Flush = Flush; out.GetOutputTime = Time; out.GetWrittenTime = Time;
        input->outMod = &out; input->SetInfo = SetInfo;
        RunSeeks("Opus", steady, 0);
        RunSeeks("Opus unchanged chain", unchanged, 0);
        RunSeeks("Opus changed chain", chain, 3);
        RunSeeks("Opus ATF tags", tags_only, 3);
        RunSeeks("Vorbis", vorbis, 0);
        RunSeeks("Vorbis unchanged chain", native, 0);
        RunSeeks("Vorbis native rate changes", native, 0, false);
        Run("Opus", steady, true, {{48000, 2}}, 96000, false);
        Run("Opus bitrate chain", chain, true, {{48000, 2}}, 96000, true);
        Run("Vorbis", vorbis, true, {{44100, 2}}, 88200, false);
        Run("Vorbis native chain", native, true, {{44100, 6}, {48000, 2}}, 92100, false);
        Run("Average bitrate", steady, false, {{48000, 2}}, 96000, false);
        const auto flac = directory / L"bitrate-flac.oga";
        MakeFlac(flac, 2, 96000, 192000, false);
        const int flac_kbps = static_cast<int>((std::filesystem::file_size(flac) * 4 + 500) / 1000);
        Check(flac_kbps > 1, "FLAC bitrate fixture is not the unknown-bitrate placeholder");
        Run("FLAC bitrate display", flac, true, {{96000, 2}}, 192000, false, flac_kbps);
        Run("FLAC average bitrate display", flac, false, {{96000, 2}}, 192000, false, flac_kbps);
        RunIdle(steady);
        input->Quit(); input = nullptr; FreeLibrary(dll); dll = nullptr; DestroyWindow(window); window = nullptr;
        std::cout << "Ogg notifications: no periodic full-format/title refresh, changed kbps, real title changes, native format reopening and average mode passed.\n";
        return 0;
    } catch (const std::exception& e) {
        if (input) { input->Stop(); input = nullptr; }
        if (dll) FreeLibrary(dll);
        if (window) DestroyWindow(window);
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
