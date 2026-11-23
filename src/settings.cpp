// SPDX-License-Identifier: GPL-3.0-only
#include "settings.hpp"

#include <windows.h>

#include <limits>
#include <string>

namespace ogg_input {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\WinampOgg\\Input";

bool ReadDword(HKEY key, const wchar_t* name, DWORD& value) noexcept {
    DWORD type = 0;
    DWORD size = sizeof(value);
    return RegQueryValueExW(key, name, nullptr, &type,
                             reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
           type == REG_DWORD && size == sizeof(value);
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value) noexcept {
    return RegSetValueExW(key, name, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

// Registry DWORDs are unsigned, but several Settings fields intentionally use
// signed values. Round-trip through LONG to retain the original 32-bit pattern.
int ReadSigned(DWORD value) noexcept {
    return static_cast<int>(static_cast<LONG>(value));
}

DWORD WriteSigned(int value) noexcept {
    return static_cast<DWORD>(static_cast<LONG>(value));
}

void ReadInt(HKEY key, const wchar_t* name, int& value) noexcept {
    DWORD stored = 0;
    if (ReadDword(key, name, stored)) value = ReadSigned(stored);
}

void ReadBool(HKEY key, const wchar_t* name, bool& value) noexcept {
    DWORD stored = 0;
    if (ReadDword(key, name, stored)) value = stored != 0;
}

bool ReadString(HKEY key, const wchar_t* name, std::wstring& value) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes % sizeof(wchar_t) != 0 || bytes > 128u * 1024u) {
        return false;
    }
    if (bytes == 0) {
        value.clear();
        return true;
    }

    std::wstring stored(bytes / sizeof(wchar_t), L'\0');
    DWORD queried = bytes;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(stored.data()), &queried) != ERROR_SUCCESS ||
        type != REG_SZ) {
        return false;
    }
    while (!stored.empty() && stored.back() == L'\0') stored.pop_back();
    value = std::move(stored);
    return true;
}

bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value) noexcept {
    constexpr DWORD kMaxChars = std::numeric_limits<DWORD>::max() / sizeof(wchar_t);
    if (value.size() >= kMaxChars) return false;
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
}

} // namespace

Settings LoadSettings() noexcept {
    Settings settings;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return settings;
    }

    int value = 0;
    ReadInt(key, L"ChannelMode", value);
    settings.channels = static_cast<ChannelMode>(value);
    ReadInt(key, L"OutputRate", settings.rate);
    ReadInt(key, L"OutputBits", settings.bits);
    ReadInt(key, L"ResamplerQuality", settings.quality);
    value = static_cast<int>(settings.dither_pdf);
    ReadInt(key, L"DitherPdf", value); settings.dither_pdf = static_cast<DitherPdf>(value);
    value = static_cast<int>(settings.noise_shape);
    ReadInt(key, L"NoiseShape", value); settings.noise_shape = static_cast<NoiseShape>(value);
    ReadBool(key, L"NativeChains", settings.native_chains);
    ReadBool(key, L"ExactFlac", settings.exact_flac);
    ReadBool(key, L"OpusHd", settings.opus_hd);
    ReadBool(key, L"OpusSpeech", settings.opus_speech);
    ReadBool(key, L"OpusBwe", settings.opus_bwe);
    value = static_cast<int>(settings.ambisonic);
    ReadInt(key, L"AmbisonicMode", value); settings.ambisonic = static_cast<AmbisonicMode>(value);
    ReadBool(key, L"Legacy51Order", settings.legacy_51_order);
    ReadBool(key, L"UseHostReplayGain", settings.use_host_replaygain);
    ReadBool(key, L"UseHostProxy", settings.use_host_proxy);
    value = static_cast<int>(settings.gain);
    ReadInt(key, L"GainMode", value);
    settings.gain = static_cast<GainMode>(value);
    ReadInt(key, L"PreampQ8", settings.preamp_q8);
    ReadInt(key, L"RadioPreampQ8", settings.radio_preamp_q8);
    ReadInt(key, L"TargetLufs", settings.target_lufs);
    ReadInt(key, L"PeakMode", settings.peak_mode);
    ReadInt(key, L"NonReplayGainQ8", settings.non_rg_q8);
    ReadBool(key, L"PreferredGainOnly", settings.preferred_gain_only);
    ReadBool(key, L"Dither", settings.dither);
    ReadBool(key, L"PreventClipping", settings.prevent_clipping);
    ReadBool(key, L"InstantBitrate", settings.instant_bitrate);
    ReadBool(key, L"UnifiedInfo", settings.unified_info);
    ReadBool(key, L"BufferSmallFiles", settings.buffer_small_files);
    ReadBool(key, L"EnhancedSpeex", settings.enhanced_speex);
    ReadInt(key, L"HttpTimeoutMs", settings.timeout_ms);
    ReadInt(key, L"ProbeLimitKb", settings.probe_kb);
    ReadInt(key, L"ReconnectAttempts", settings.reconnect_attempts);
    ReadInt(key, L"ReconnectDelayMs", settings.reconnect_delay_ms);
    ReadInt(key, L"PrebufferKb", settings.prebuffer_kb);
    ReadInt(key, L"StreamBufferKb", settings.stream_buffer_kb);
    ReadBool(key, L"SaveStreams", settings.save_streams);
    ReadBool(key, L"HttpSeek", settings.http_seek);
    ReadInt(key, L"ProxyMode", settings.proxy_mode);
    ReadInt(key, L"UnderrunPercent", settings.underrun_percent);
    // Credentials are intentionally never accepted or persisted here. Proxy URLs
    // are configuration only; authenticated requests remain an ephemeral choice.
    try {
        ReadString(key, L"ProxyUrl", settings.proxy_url);
        ReadString(key, L"SaveDirectory", settings.save_directory);
    } catch (...) {
        settings.proxy_url.clear();
        settings.save_directory.clear();
    }

    RegCloseKey(key);
    return Validate(settings);
}

bool SaveSettings(const Settings& input) noexcept {
    const Settings settings = Validate(input);
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return false;
    }

    const bool ok =
        WriteDword(key, L"ChannelMode", WriteSigned(static_cast<int>(settings.channels))) &&
        WriteDword(key, L"OutputRate", WriteSigned(settings.rate)) &&
        WriteDword(key, L"OutputBits", WriteSigned(settings.bits)) &&
        WriteDword(key, L"ResamplerQuality", WriteSigned(settings.quality)) &&
        WriteDword(key, L"DitherPdf", WriteSigned(static_cast<int>(settings.dither_pdf))) &&
        WriteDword(key, L"NoiseShape", WriteSigned(static_cast<int>(settings.noise_shape))) &&
        WriteDword(key, L"NativeChains", settings.native_chains ? 1u : 0u) &&
        WriteDword(key, L"ExactFlac", settings.exact_flac ? 1u : 0u) &&
        WriteDword(key, L"OpusHd", settings.opus_hd ? 1u : 0u) &&
        WriteDword(key, L"OpusSpeech", settings.opus_speech ? 1u : 0u) &&
        WriteDword(key, L"OpusBwe", settings.opus_bwe ? 1u : 0u) &&
        WriteDword(key, L"AmbisonicMode", WriteSigned(static_cast<int>(settings.ambisonic))) &&
        WriteDword(key, L"Legacy51Order", settings.legacy_51_order ? 1u : 0u) &&
        WriteDword(key, L"UseHostReplayGain", settings.use_host_replaygain ? 1u : 0u) &&
        WriteDword(key, L"UseHostProxy", settings.use_host_proxy ? 1u : 0u) &&
        WriteDword(key, L"GainMode", WriteSigned(static_cast<int>(settings.gain))) &&
        WriteDword(key, L"PreampQ8", WriteSigned(settings.preamp_q8)) &&
        WriteDword(key, L"RadioPreampQ8", WriteSigned(settings.radio_preamp_q8)) &&
        WriteDword(key, L"TargetLufs", WriteSigned(settings.target_lufs)) &&
        WriteDword(key, L"PeakMode", WriteSigned(settings.peak_mode)) &&
        WriteDword(key, L"NonReplayGainQ8", WriteSigned(settings.non_rg_q8)) &&
        WriteDword(key, L"PreferredGainOnly", settings.preferred_gain_only ? 1u : 0u) &&
        WriteDword(key, L"Dither", settings.dither ? 1u : 0u) &&
        WriteDword(key, L"PreventClipping", settings.prevent_clipping ? 1u : 0u) &&
        WriteDword(key, L"InstantBitrate", settings.instant_bitrate ? 1u : 0u) &&
        WriteDword(key, L"UnifiedInfo", settings.unified_info ? 1u : 0u) &&
        WriteDword(key, L"BufferSmallFiles", settings.buffer_small_files ? 1u : 0u) &&
        WriteDword(key, L"EnhancedSpeex", settings.enhanced_speex ? 1u : 0u) &&
        WriteDword(key, L"HttpTimeoutMs", WriteSigned(settings.timeout_ms)) &&
        WriteDword(key, L"ProbeLimitKb", WriteSigned(settings.probe_kb)) &&
        WriteDword(key, L"ReconnectAttempts", WriteSigned(settings.reconnect_attempts)) &&
        WriteDword(key, L"ReconnectDelayMs", WriteSigned(settings.reconnect_delay_ms)) &&
        WriteDword(key, L"PrebufferKb", WriteSigned(settings.prebuffer_kb)) &&
        WriteDword(key, L"StreamBufferKb", WriteSigned(settings.stream_buffer_kb)) &&
        WriteDword(key, L"SaveStreams", settings.save_streams ? 1u : 0u) &&
        WriteDword(key, L"HttpSeek", settings.http_seek ? 1u : 0u) &&
        WriteDword(key, L"ProxyMode", WriteSigned(settings.proxy_mode)) &&
        WriteDword(key, L"UnderrunPercent", WriteSigned(settings.underrun_percent)) &&
        WriteString(key, L"ProxyUrl", settings.proxy_url) &&
        WriteString(key, L"SaveDirectory", settings.save_directory);

    RegCloseKey(key);
    return ok;
}

} // namespace ogg_input
