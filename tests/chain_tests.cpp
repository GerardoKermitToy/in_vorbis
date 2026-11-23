// SPDX-License-Identifier: GPL-3.0-only
#include "audio.hpp"
#include "codec_fixtures.hpp"
#include "registry_sandbox.hpp"
#include "winamp_input.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

using namespace ogg_input;
namespace {
struct Format { int rate, channels; long long frames; };
void NativePcm(const std::filesystem::path& path, const std::vector<Format>& expected,
               Settings settings = {}) {
    Audio audio; std::wstring error;
    Check(audio.Open(path.c_str(), settings, false, error, true), "open native chained PCM");
    Pcm pcm(audio, settings); Check(pcm.Ready(), "native chained PCM ready");
    std::array<unsigned char, BlockFrames * MaxChannels * 4 + 16> storage{};
    std::vector<Format> actual{{pcm.Rate(), pcm.Channels(), 0}};
    int previous_time = 0, changes = 0;
    for (int guard = 0; guard != 100000; ++guard) {
        constexpr int capacity = 37;
        const int size = capacity * pcm.Channels() * settings.bits / 8;
        storage.fill(0xa5);
        const int n = pcm.Read(storage.data() + 8, capacity);
        for (int i = 0; i != 8; ++i) Check(storage[i] == 0xa5, "native lower guard");
        for (int i = size + 8; i != size + 16; ++i) Check(storage[i] == 0xa5, "native upper guard");
        Check(pcm.PositionMs() >= previous_time, "native timeline monotonic");
        previous_time = pcm.PositionMs();
        if (n == Pcm::FormatChange) {
            Check(++changes < 10 && pcm.AcceptFormat(), "acknowledge native boundary");
            actual.push_back({pcm.Rate(), pcm.Channels(), 0});
        } else {
            Check(n >= 0 && n <= capacity, "native PCM frame result");
            if (!n) break;
            actual.back().frames += n;
        }
    }
    Check(actual.size() == expected.size(), "native format segment count");
    long double milliseconds = 0;
    for (size_t i = 0; i != actual.size(); ++i) {
        Check(actual[i].rate == expected[i].rate && actual[i].channels == expected[i].channels,
              "native segment format");
        Check(std::abs(actual[i].frames - expected[i].frames) <= 1, "native segment exact frames");
        milliseconds += expected[i].frames * 1000.L / expected[i].rate;
    }
    Check(std::abs(pcm.PositionMs() - static_cast<int>(milliseconds)) <= 1, "native accumulated time");
}

std::mutex output_mutex;
std::vector<Format> segments;
std::atomic<long long> written{0};
std::atomic<bool> callback_error{false}, require_drain{true}, hold_drain{false};
int depth = 16, drain_ticks = 0, closed = 0, rejected = 0;
bool reject_surround = false;
std::vector<int> visualization_rates, dsp_rates;
int __cdecl Open(int rate, int channels, int bits, int, int) {
    std::lock_guard<std::mutex> guard(output_mutex);
    if (reject_surround && channels > 2) { ++rejected; return -1; }
    depth = bits; segments.push_back({rate, channels, 0}); drain_ticks = 0; return 0;
}
void __cdecl Close() {
    std::lock_guard<std::mutex> guard(output_mutex);
    if (require_drain && drain_ticks) callback_error = true;
    ++closed;
}
int __cdecl Write(char* data, int bytes) {
    std::lock_guard<std::mutex> guard(output_mutex);
    if (segments.empty() || !data || bytes <= 0 || bytes % (segments.back().channels * depth / 8)) {
        callback_error = true; return 1;
    }
    const auto frames = bytes / (segments.back().channels * depth / 8);
    segments.back().frames += frames; written += frames; drain_ticks = 3; return 0;
}
int __cdecl CanWrite() { return 8192; }
int __cdecl Playing() {
    std::lock_guard<std::mutex> guard(output_mutex);
    if (hold_drain) return drain_ticks != 0;
    if (drain_ticks) { --drain_ticks; return 1; }
    return 0;
}
int __cdecl Pause(int state) { return state; }
void __cdecl Flush(int) { std::lock_guard<std::mutex> guard(output_mutex); drain_ticks = 0; }
int __cdecl Time() { return 0; }
void __cdecl VisualInit(int, int rate) { visualization_rates.push_back(rate); }
void __cdecl VisualQuit() {}
int __cdecl DspActive() { return 1; }
int __cdecl Dsp(short*, int frames, int, int, int rate) { dsp_rates.push_back(rate); return frames; }
void ResetOutput() {
    std::lock_guard<std::mutex> guard(output_mutex);
    segments.clear(); visualization_rates.clear(); dsp_rates.clear();
    written = 0; callback_error = false; require_drain = true; hold_drain = false;
    drain_ticks = closed = rejected = 0; reject_surround = false;
}
void WaitFrames(long long frames) {
    const auto deadline = GetTickCount64() + 10000;
    while (written < frames && GetTickCount64() < deadline) Sleep(1);
    Check(written == frames, "DLL native chain frame count");
}
}
int wmain(int argc, wchar_t** argv) {
    RegistrySandbox registry; HMODULE dll = nullptr; winamp::InModule* in = nullptr;
    try {
        Check(argc == 3, "usage: chain_tests DLL DIRECTORY");
        const auto dir = std::filesystem::path(argv[2]); std::filesystem::create_directories(dir);
        constexpr int frames = 7013;
        auto chain = dir / L"native-vorbis.ogg", tail = dir / L"tail.ogg";
        MakeVorbis(chain, 6, 44100, frames, 11001); MakeVorbis(tail, 2, 48000, frames, 11002); AppendFile(chain, tail);
        for (int bits : {8, 16, 24, 32}) { Settings s; s.bits = bits;
            NativePcm(chain, {{44100, 6, frames}, {48000, 2, frames}}, s);
        }
        Settings stereo; stereo.channels = ChannelMode::Stereo;
        NativePcm(chain, {{44100, 2, frames}, {48000, 2, frames}}, stereo);
        Settings fixed; fixed.rate = 32000;
        NativePcm(chain, {{32000, 6, frames * 32000LL / 44100}, {32000, 2, frames * 32000LL / 48000}}, fixed);
        auto opus = dir / L"native-opus.oga", opus_tail = dir / L"opus-tail.oga";
        MakeOpus(opus, 2, frames, 11003); MakeOpus(opus_tail, 6, frames, 11004); AppendFile(opus, opus_tail);
        NativePcm(opus, {{48000, 2, frames}, {48000, 6, frames}});
        auto flac = dir / L"native-flac.oga", flac_tail = dir / L"flac-tail.oga";
        MakeFlac(flac, 2, 44100, frames); MakeFlac(flac_tail, 6, 48000, frames); AppendFile(flac, flac_tail);
        NativePcm(flac, {{44100, 2, frames}, {48000, 6, frames}});
#ifdef OGG_HAVE_SPEEX
        auto speex = dir / L"native-speex.spx", speex_tail = dir / L"speex-tail.spx";
        MakeSpeex(speex, 16000, frames, 11005); MakeSpeex(speex_tail, 8000, frames, 11006); AppendFile(speex, speex_tail);
        NativePcm(speex, {{16000, 1, frames}, {8000, 1, frames}});
#endif
        // A seek cancels a queued boundary; no retained new-format samples leak.
        {
            Audio a; Settings s; std::wstring error; Check(a.Open(chain.c_str(), s, false, error, true), "seek chain open");
            Pcm pcm(a, s); std::array<float, BlockFrames * MaxChannels> data{};
            int n = 0; while ((n = pcm.ReadFloat(data.data(), BlockFrames)) > 0) {}
            Check(n == Pcm::FormatChange && a.Seek(0), "seek with boundary pending"); pcm.Reset(0);
            long long total = 0;
            for (;;) { n = pcm.ReadFloat(data.data(), BlockFrames); if (n == Pcm::FormatChange) { Check(pcm.AcceptFormat(), "seek replay boundary"); continue; } Check(n >= 0, "seek replay read"); if (!n) break; total += n; }
            Check(total == 2 * frames, "seek discards queued boundary block exactly");
        }
        dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(dll, "load native chain DLL");
        const auto get = reinterpret_cast<winamp::InModule*(__cdecl*)()>(GetProcAddress(dll, "winampGetInModule2"));
        Check(get, "native chain plugin entry"); in = get();
        winamp::OutModule out{}; out.Open = Open; out.Close = Close; out.Write = Write;
        out.CanWrite = CanWrite; out.IsPlaying = Playing; out.Pause = Pause; out.Flush = Flush;
        out.GetOutputTime = Time; out.GetWrittenTime = Time;
        in->outMod = &out; in->hDllInstance = dll; in->SAVSAInit = VisualInit; in->SAVSADeInit = VisualQuit;
        in->dsp_isactive = DspActive; in->dsp_dosamples = Dsp; in->Init();
        ResetOutput(); Check(!in->Play(chain.c_str()), "DLL native chained play"); WaitFrames(2 * frames);
        require_drain = false; in->Stop();
        Check(segments.size() == 2 && segments[0].rate == 44100 && segments[0].channels == 6 &&
              segments[1].rate == 48000 && segments[1].channels == 2 &&
              segments[0].frames == frames && segments[1].frames == frames,
              "DLL reopens output at native rate/channel boundary");
        Check(closed == 2 && !callback_error, "DLL drains old output before reopening");
        Check(visualization_rates == std::vector<int>({44100, 48000}), "visualization rate renegotiation");
        Check(std::find(dsp_rates.begin(), dsp_rates.end(), 44100) != dsp_rates.end() &&
              std::find(dsp_rates.begin(), dsp_rates.end(), 48000) != dsp_rates.end(), "DSP sees both native rates");
        ResetOutput(); reject_surround = true; Check(!in->Play(opus.c_str()), "DLL fallback chain play"); WaitFrames(2 * frames);
        require_drain = false; in->Stop();
        Check(rejected == 1 && segments.size() == 2 && segments[1].channels == 2 &&
              segments[1].frames == frames && !callback_error, "unsupported multichannel output falls back without lost frames");
        ResetOutput(); Check(!in->Play(flac.c_str()), "DLL FLAC native chain play"); WaitFrames(2 * frames);
        Check(in->GetLength() >= 300, "DLL duration follows discovered FLAC links");
        require_drain = false; in->Stop();
        Check(segments.size() == 2 && segments[0].channels == 2 && segments[1].channels == 6 &&
              segments[0].rate == 44100 && segments[1].rate == 48000 && !callback_error,
              "DLL handles lazily discovered FLAC source layouts");
        // Stop must cancel a drain wait even if the output never reports empty.
        ResetOutput(); hold_drain = true; Check(!in->Play(chain.c_str()), "DLL drain cancellation play");
        WaitFrames(frames); require_drain = false; const auto before = GetTickCount64(); in->Stop();
        Check(GetTickCount64() - before < 2000, "stop interrupts native boundary drain");
        // Extended-read ABI must retain its advertised format across the chain.
        using OpenRead = intptr_t(__cdecl*)(const wchar_t*, int*, int*, int*, int*);
        using Read = size_t(__cdecl*)(intptr_t, char*, size_t, int*);
        using CloseRead = void(__cdecl*)(intptr_t);
        auto open = reinterpret_cast<OpenRead>(GetProcAddress(dll, "winampGetExtendedRead_openW"));
        auto read = reinterpret_cast<Read>(GetProcAddress(dll, "winampGetExtendedRead_getData"));
        auto close = reinterpret_cast<CloseRead>(GetProcAddress(dll, "winampGetExtendedRead_close"));
        Check(open && read && close, "chained extended-read exports");
        int size = 0, bits = 16, channels = 2, rate = 44100;
        const auto h = open(chain.c_str(), &size, &bits, &channels, &rate); Check(h && channels == 2 && rate == 44100, "fixed extended-read chain format");
        std::array<char, 4096> data{}; size_t total = 0, n = 0;
        while ((n = read(h, data.data(), data.size(), nullptr))) total += n;
        close(h); Check(std::abs(static_cast<long long>(total / 4) - (frames + frames * 44100LL / 48000)) <= 2,
                        "extended-read chain uses stable resampled duration");
        in->Quit(); in = nullptr; FreeLibrary(dll); dll = nullptr;
        std::cout << "Native chains: all codecs, exact segments, drain/reopen, fallback, DSP/vis, seek, stop and fixed extended read passed.\n";
        return 0;
    } catch (const std::exception& error) {
        require_drain = false; hold_drain = false;
        if (in) in->Quit(); if (dll) FreeLibrary(dll);
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
