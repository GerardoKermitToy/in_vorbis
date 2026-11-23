// SPDX-License-Identifier: GPL-3.0-only
#include "settings.hpp"
#include "registry_sandbox.hpp"
#include <iostream>

using namespace ogg_input;
int wmain() {
    try {
        RegistrySandbox registry;
        const auto defaults = LoadSettings();
        Check(defaults.gain == GainMode::Header && defaults.bits == 16,
              "missing registry retains defaults");
        Settings desired;
        desired.channels = ChannelMode::Quad;
        desired.rate = 96000;
        desired.bits = 24;
        desired.quality = 9;
        desired.dither_pdf = DitherPdf::Gaussian;
        desired.noise_shape = NoiseShape::AthLow;
        desired.native_chains = false;
        desired.exact_flac = false; desired.opus_hd = false;
        desired.opus_speech = true; desired.opus_bwe = true;
        desired.ambisonic = AmbisonicMode::Headphones;
        desired.legacy_51_order = true;
        desired.use_host_replaygain = false;
        desired.use_host_proxy = false;
        desired.gain = GainMode::Album;
        desired.preamp_q8 = -1536;
        desired.radio_preamp_q8 = 768;
        desired.target_lufs = -18;
        desired.peak_mode = 2;
        desired.non_rg_q8 = -2304;
        desired.preferred_gain_only = true;
        desired.dither = false;
        desired.prevent_clipping = false;
        desired.instant_bitrate = false;
        desired.unified_info = false;
        desired.buffer_small_files = true;
        desired.enhanced_speex = false;
        desired.timeout_ms = 12000;
        desired.probe_kb = 512;
        desired.reconnect_attempts = 3;
        desired.reconnect_delay_ms = 1750;
        desired.prebuffer_kb = 40;
        desired.stream_buffer_kb = 256;
        desired.http_seek = true;
        desired.save_streams = true;
        desired.interactive_auth = true; // Deliberately not persisted.
        desired.proxy_mode = 1;
        desired.underrun_percent = 25;
        desired.proxy_url = L"127.0.0.1:8080";
        desired.save_directory = L"C:\\Radio recordings テスト";
        Check(SaveSettings(desired), "save independent settings");
        const auto actual = LoadSettings();
        Check(actual.channels == desired.channels && actual.rate == desired.rate &&
              actual.bits == desired.bits && actual.quality == desired.quality,
              "decode settings roundtrip");
        Check(actual.dither_pdf == DitherPdf::Gaussian && actual.noise_shape == NoiseShape::AthLow &&
              !actual.native_chains && actual.legacy_51_order &&
              !actual.use_host_replaygain && !actual.use_host_proxy,
              "processing and host preference switches roundtrip");
        Check(!actual.exact_flac && !actual.opus_hd && actual.opus_speech && actual.opus_bwe &&
              actual.ambisonic == AmbisonicMode::Headphones, "advanced codec options roundtrip");
        Check(actual.gain == desired.gain && actual.preamp_q8 == desired.preamp_q8 &&
              actual.radio_preamp_q8 == desired.radio_preamp_q8 &&
              actual.target_lufs == desired.target_lufs && actual.peak_mode == 2 &&
              actual.non_rg_q8 == -2304 && actual.preferred_gain_only,
              "all ReplayGain settings roundtrip");
        Check(!actual.dither && !actual.prevent_clipping && !actual.instant_bitrate &&
              !actual.unified_info && actual.buffer_small_files && !actual.enhanced_speex,
              "decode switches roundtrip");
        Check(actual.timeout_ms == 12000 && actual.probe_kb == 512 &&
              actual.reconnect_attempts == 3 && actual.reconnect_delay_ms == 1750 &&
              actual.prebuffer_kb == 40 && actual.stream_buffer_kb == 256 &&
              actual.http_seek && actual.save_streams && actual.proxy_mode == 1 &&
              actual.underrun_percent == 25 && actual.proxy_url == desired.proxy_url &&
              actual.save_directory == desired.save_directory,
              "network settings roundtrip");
        Check(!actual.interactive_auth, "authentication consent is not stored");
        HKEY key = nullptr;
        Check(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\WinampOgg\\Input", 0,
                            KEY_SET_VALUE, &key) == ERROR_SUCCESS, "open isolated settings");
        Check(RegDeleteValueW(key, L"GainMode") == ERROR_SUCCESS, "delete partial setting");
        RegCloseKey(key);
        Check(LoadSettings().gain == GainMode::Header, "partial registry retains gain default");
        desired.rate = 1;
        desired.bits = 7;
        desired.prebuffer_kb = 999999;
        desired.peak_mode = 99;
        desired.non_rg_q8 = -999999;
        desired.dither_pdf = static_cast<DitherPdf>(99);
        desired.noise_shape = static_cast<NoiseShape>(99);
        desired.channels = static_cast<ChannelMode>(99);
        desired.ambisonic = static_cast<AmbisonicMode>(99);
        const auto bounded = Validate(desired);
        Check(bounded.rate == 0 && bounded.bits == 16 && bounded.prebuffer_kb == 256 &&
              bounded.peak_mode == 3 && bounded.non_rg_q8 == -60 * 256 &&
              bounded.channels == ChannelMode::Source && bounded.dither_pdf == DitherPdf::Triangular &&
              bounded.noise_shape == NoiseShape::Ath && bounded.ambisonic == AmbisonicMode::Stereo,
              "settings bounds");
        std::cout << "Settings: defaults, all options, ReplayGain, bounds and ephemeral auth passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
