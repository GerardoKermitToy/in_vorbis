// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace ogg_input {
namespace matroska {

// This is the one-slot ABI used by Winamp's Wasabi service interfaces.  Do not
// add a virtual destructor or further virtual members: doing so changes the
// vtable seen by the host.
struct Dispatchable {
    virtual int _dispatch(int message, void* result, void** arguments = nullptr,
        int argument_count = 0) = 0;
};

struct ServiceManager : Dispatchable {};
struct ServiceFactory : Dispatchable {};
struct DecoderService : Dispatchable {};
struct AudioDecoder : Dispatchable {};

// Winamp's WM_WA_IPC/IPC_GET_API_SERVICE query.
inline constexpr UINT kServiceManagerMessage = WM_USER;
inline constexpr LPARAM kGetApiService = 3025;

// WaSvc::MKVDECODER, expressed by Wasabi as MK3CC('m', 'k', 'v').
inline constexpr std::uint32_t kDecoderServiceType = 0x006D6B76u;

namespace ServiceManagerMessage {
inline constexpr int Register = 10;
inline constexpr int Deregister = 20;
}

namespace ServiceFactoryMessage {
inline constexpr int GetServiceType = 100;
inline constexpr int GetServiceName = 200;
inline constexpr int GetGuid = 210;
inline constexpr int GetInterface = 300;
inline constexpr int SupportsNonLockingInterface = 301;
inline constexpr int ReleaseInterface = 310;
inline constexpr int GetTestString = 500;
inline constexpr int ServiceNotify = 600;
}

namespace DecoderServiceMessage {
inline constexpr int CreateAudioDecoder = 0;
inline constexpr int CreateVideoDecoder = 1;
}

namespace AudioDecoderMessage {
inline constexpr int OutputFrameSize = 0;
inline constexpr int GetOutputProperties = 1;
inline constexpr int DecodeBlock = 2;
inline constexpr int Flush = 3;
inline constexpr int Close = 4;
inline constexpr int EndOfStream = 5;
}

inline constexpr int kCreateDecoderSuccess = 0;
inline constexpr int kCreateDecoderNotMine = -1;
inline constexpr int kCreateDecoderFailure = 1;

inline constexpr int kDecodeSuccess = 0;
inline constexpr int kDecodeNeedMoreInput = -1;
inline constexpr int kDecodeFailure = 1;

// These structures are the public nsmkv ABI passed to decoder services.  The
// struct_size member permits newer hosts to append fields without changing the
// prefix used here.
#pragma pack(push, 8)
struct TrackEntryData {
    std::size_t struct_size;
    std::uint64_t track_number;
    std::uint64_t track_uid;
    std::uint64_t track_type;
    bool flag_enabled;
    bool flag_default;
    bool flag_forced;
    bool flag_lacing;
    bool decode_all;
    std::uint64_t min_cache;
    std::uint64_t max_cache;
    std::uint64_t default_duration;
    double track_timecode_scale;
    std::uint64_t max_block_additional_id;
    char* name;
    char* language;
    char* codec_id;
    void* codec_private;
    std::size_t codec_private_len;
    char* codec_name;
    std::uint64_t attachment_link;
};

struct AudioData {
    std::size_t struct_size;
    std::uint64_t sampling_frequency;
    std::uint64_t channels;
    std::uint64_t bit_depth;
    std::uint64_t output_sampling_frequency;
};
#pragma pack(pop)

static_assert(sizeof(void*) == 4, "The Winamp 5.9 Wasabi service ABI is 32-bit.");
static_assert(sizeof(bool) == 1, "The Matroska track ABI requires one-byte bools.");
static_assert(offsetof(TrackEntryData, codec_private) == 92, "TrackEntryData ABI drift.");
static_assert(offsetof(TrackEntryData, codec_private_len) == 96, "TrackEntryData ABI drift.");
static_assert(sizeof(TrackEntryData) == 112, "TrackEntryData ABI drift.");
static_assert(sizeof(AudioData) == 40, "AudioData ABI drift.");

} // namespace matroska

// Register and remove this module's A_VORBIS-only Matroska decoder service.
// They intentionally acquire the Wasabi manager through the supplied host
// window rather than relying on an input-module private service pointer.
void RegisterMatroska(HWND host) noexcept;
void UnregisterMatroska() noexcept;

} // namespace ogg_input
