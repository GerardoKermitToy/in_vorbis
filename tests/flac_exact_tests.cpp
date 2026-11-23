// SPDX-License-Identifier: GPL-3.0-only
#include "audio.hpp"
#include "fixtures.hpp"
#include <FLAC/stream_encoder.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <iostream>
#include <vector>
using namespace ogg_input;
namespace {
std::vector<std::int32_t> MakeExact(const std::filesystem::path& path, int channels, int rate, int bits, int frames) {
    auto* encoder = FLAC__stream_encoder_new();
    Check(encoder != nullptr, "allocate exact FLAC encoder");
    Check(FLAC__stream_encoder_set_channels(encoder, channels) &&
          FLAC__stream_encoder_set_bits_per_sample(encoder, bits) &&
          FLAC__stream_encoder_set_sample_rate(encoder, rate) &&
          FLAC__stream_encoder_set_total_samples_estimate(encoder, frames), "configure exact FLAC");
    FILE* file = nullptr;
    _wfopen_s(&file, path.c_str(), L"w+b");
    Check(file != nullptr, "open exact FLAC fixture");
    Check(FLAC__stream_encoder_init_ogg_FILE(encoder, file, nullptr, nullptr) == FLAC__STREAM_ENCODER_INIT_STATUS_OK,
          "initialize exact Ogg FLAC");
    const std::int64_t factor = INT64_C(1) << (32 - bits);
    std::vector<FLAC__int32> pcm(static_cast<size_t>(frames) * channels);
    std::vector<std::int32_t> expected(pcm.size());
    const std::int64_t low = -(INT64_C(1) << (bits - 1)), high = -low - 1;
    std::uint32_t random = 0x12345678u;
    for (size_t i = 0; i < pcm.size(); ++i) {
        random = random * 1664525u + 1013904223u;
        std::int32_t signed_random = 0;
        std::memcpy(&signed_random, &random, sizeof(random));
        const std::int64_t edges[] = {low, high, -1, 0, 1, high - 1, low + 1};
        const auto value = i < std::size(edges) ? edges[i] : signed_random / factor;
        pcm[i] = static_cast<FLAC__int32>(value);
        expected[i] = static_cast<std::int32_t>(value * factor);
    }
    Check(FLAC__stream_encoder_process_interleaved(encoder, pcm.data(), frames) &&
          FLAC__stream_encoder_finish(encoder), "encode exact FLAC fixture");
    FLAC__stream_encoder_delete(encoder);
    return expected;
}
std::vector<std::int32_t> DrainExact(Audio& audio, Settings settings, int* changes = nullptr) {
    Pcm pcm(audio, settings);
    Check(pcm.Ready(), "exact pipeline ready");
    std::array<std::int32_t, BlockFrames * MaxChannels + 2> buffer{};
    std::vector<std::int32_t> result;
    const int chunks[] = {1, 7, 575, 576, 17};
    int iteration = 0;
    for (;;) {
        const int capacity = chunks[iteration++ % std::size(chunks)];
        buffer.front() = 0x12345678;
        const int guard = capacity * pcm.Channels() + 1;
        buffer[guard] = 0x23456789;
        const int n = pcm.Read(buffer.data() + 1, capacity);
        Check(buffer.front() == 0x12345678 && buffer[guard] == 0x23456789, "exact integer buffer guards");
        if (n == Pcm::FormatChange) {
            Check(changes && pcm.AcceptFormat(), "accept exact chain format");
            ++*changes;
            continue;
        }
        Check(n >= 0 && n <= capacity, "exact pipeline read");
        if (!n) break;
        result.insert(result.end(), buffer.begin() + 1, buffer.begin() + 1 + n * pcm.Channels());
    }
    return result;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 3, "usage: ogg_flac_exact_tests DIRECTORY DLL");
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        Settings settings;
        settings.bits = 32; settings.dither = false; settings.gain = GainMode::Raw;
        std::wstring error;
        for (int bits : {16, 24, 32}) for (int channels : {1, 2, 6}) for (int rate : {44100, 96000}) {
            const auto path = directory / (std::to_wstring(bits) + L"-" + std::to_wstring(channels) + L"-" + std::to_wstring(rate) + L".oga");
            const auto expected = MakeExact(path, channels, rate, bits, 1453);
            Audio audio;
            Check(audio.Open(path.c_str(), settings, false, error), "open exact FLAC");
            Check(audio.Info().current.bits_per_sample == bits, "report native FLAC precision");
            Check(DrainExact(audio, settings) == expected, "all integer FLAC bits preserved");
            Check(audio.Open(path.c_str(), settings, false, error) && audio.Seek(10), "seek exact FLAC");
            const auto offset = static_cast<size_t>(rate / 100) * channels;
            Check(DrainExact(audio, settings) == std::vector<std::int32_t>(expected.begin() + offset, expected.end()),
                  "exact FLAC seek preserves low bits");
        }
        const auto first = directory / L"chain.oga", tail = directory / L"tail.oga";
        auto expected = MakeExact(first, 2, 44100, 32, 1601);
        const auto second = MakeExact(tail, 6, 96000, 24, 1453);
        AppendFile(first, tail);
        expected.insert(expected.end(), second.begin(), second.end());
        Audio chained;
        Check(chained.Open(first.c_str(), settings, false, error, true), "open exact native chain");
        int changes = 0;
        Check(DrainExact(chained, settings, &changes) == expected && changes == 1,
              "exact chain retains first new-format block");
        Audio chain_seek;
        Check(chain_seek.Open(first.c_str(), settings, false, error, true) && chain_seek.Seek(40),
              "seek across FLAC rate/layout boundary");
        const size_t tail_frame = static_cast<size_t>((INT64_C(40) * 44100 - INT64_C(1601) * 1000) * 96000 / (INT64_C(44100) * 1000));
        int seek_changes = 0;
        Check(DrainExact(chain_seek, settings, &seek_changes) ==
              std::vector<std::int32_t>(second.begin() + tail_frame * 6, second.end()),
              "cross-link seek preserves exact remaining FLAC samples");
        const auto path = directory / L"low-bits.oga";
        const auto original = MakeExact(path, 2, 48000, 32, 1453);
        for (int mode = 0; mode < 3; ++mode) {
            Settings processed = settings;
            if (mode == 0) processed.exact_flac = false;
            if (mode == 1) processed.preamp_q8 = -256;
            if (mode == 2) processed.dither = true;
            Audio audio;
            Check(audio.Open(path.c_str(), processed, false, error), "open processed FLAC");
            Check(DrainExact(audio, processed) != original, "processing does not bypass gain/dither policy");
        }
        HMODULE module = LoadLibraryW(argv[2]);
        Check(module != nullptr, "load FLAC extended-read DLL");
        using Open = intptr_t (__cdecl*)(const wchar_t*, int*, int*, int*, int*);
        using Get = size_t (__cdecl*)(intptr_t, char*, size_t, int*);
        using Close = void (__cdecl*)(intptr_t);
        auto open = reinterpret_cast<Open>(GetProcAddress(module, "winampGetExtendedRead_openW"));
        auto get = reinterpret_cast<Get>(GetProcAddress(module, "winampGetExtendedRead_getData"));
        auto close = reinterpret_cast<Close>(GetProcAddress(module, "winampGetExtendedRead_close"));
        Check(open && get && close, "resolve exact FLAC exports");
        int size = 0, bits = 32, channels = 0, rate = 0, kill = 0;
        const auto handle = open(path.c_str(), &size, &bits, &channels, &rate);
        Check(handle && bits == 32 && channels == 2 && rate == 48000, "open exact FLAC conversion");
        std::vector<std::int32_t> converted(original.size());
        Check(get(handle, reinterpret_cast<char*>(converted.data()), converted.size() * 4, &kill) == converted.size() * 4,
              "exact conversion complete length");
        Check(converted == original, "DLL extended read preserves all FLAC bits");
        close(handle); FreeLibrary(module);
        std::cout << "Exact FLAC: 16/24/32-bit, edge values, channels, chunks, seeks, chains, processing fallback and DLL passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
