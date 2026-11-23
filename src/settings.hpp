#pragma once
#include <algorithm>
#include <string>
namespace ogg_input {
enum class ChannelMode { Source, Stereo, Mono, Quad, LegacyUnmapped, Surround, SurroundII };
enum class DitherPdf { Rectangular, Triangular, Gaussian };
enum class NoiseShape { None, FirstOrder, Ath, AthLow };
enum class GainMode { Header, Track, Album, Automatic, Raw };
enum class AmbisonicMode { Stereo, Headphones, Quad, Surround51, Surround71 };
struct Settings {
    ChannelMode channels = ChannelMode::Source;
    int rate = 0; // Zero uses the source rate (and follows links in native playback).
    int bits = 16, quality = 5;
    DitherPdf dither_pdf = DitherPdf::Triangular;
    NoiseShape noise_shape = NoiseShape::Ath;
    bool native_chains = true, legacy_51_order = false;
    bool exact_flac = true, opus_hd = true, opus_speech = false, opus_bwe = false;
    AmbisonicMode ambisonic = AmbisonicMode::Stereo;
    bool use_host_replaygain = true, use_host_proxy = true;
    GainMode gain = GainMode::Header;
    int preamp_q8 = 0, radio_preamp_q8 = 0, target_lufs = -23;
    bool dither = true, prevent_clipping = true, instant_bitrate = true;
    bool unified_info = true, buffer_small_files = false, enhanced_speex = true;
    int timeout_ms = 5000, probe_kb = 1024, reconnect_attempts = 0, reconnect_delay_ms = 1000;
    int prebuffer_kb = 32, stream_buffer_kb = 128;
    bool save_streams = false, interactive_auth = false, http_seek = false;
    int proxy_mode = 2, underrun_percent = 75;
    int peak_mode = 1, non_rg_q8 = -6 * 256;
    bool preferred_gain_only = false;
    std::wstring proxy_url, save_directory;
};
inline Settings Validate(Settings s) noexcept {
    if (static_cast<unsigned>(s.channels) > 6) s.channels = ChannelMode::Source;
    if (static_cast<unsigned>(s.dither_pdf) > 2) s.dither_pdf = DitherPdf::Triangular;
    if (static_cast<unsigned>(s.noise_shape) > 3) s.noise_shape = NoiseShape::Ath;
    if (static_cast<unsigned>(s.ambisonic) > 4) s.ambisonic = AmbisonicMode::Stereo;
    if (s.rate != 0 && (s.rate < 8000 || s.rate > 192000)) s.rate = 0;
    if (s.bits != 8 && s.bits != 16 && s.bits != 24 && s.bits != 32) s.bits = 16;
    s.quality = std::clamp(s.quality, 0, 10);
    if (static_cast<unsigned>(s.gain) > 4) s.gain = GainMode::Header;
    s.preamp_q8 = std::clamp(s.preamp_q8, -60 * 256, 30 * 256);
    s.radio_preamp_q8 = std::clamp(s.radio_preamp_q8, -60 * 256, 30 * 256);
    s.target_lufs = std::clamp(s.target_lufs, -40, 0);
    s.timeout_ms = std::clamp(s.timeout_ms, 1000, 30000);
    s.probe_kb = std::clamp(s.probe_kb, 64, 4096);
    s.reconnect_attempts = std::clamp(s.reconnect_attempts, 0, 10);
    s.reconnect_delay_ms = std::clamp(s.reconnect_delay_ms, 250, 30000);
    s.stream_buffer_kb = std::clamp(s.stream_buffer_kb, 32, 4096);
    s.prebuffer_kb = std::clamp(s.prebuffer_kb, 0, std::min(s.stream_buffer_kb, s.probe_kb / 2));
    s.proxy_mode = std::clamp(s.proxy_mode, 0, 2);
    s.underrun_percent = std::clamp(s.underrun_percent, 0, 100);
    s.peak_mode = std::clamp(s.peak_mode, 0, 3);
    s.non_rg_q8 = std::clamp(s.non_rg_q8, -60 * 256, 30 * 256);
    return s;
}
Settings LoadSettings() noexcept;
bool SaveSettings(const Settings&) noexcept;
}
