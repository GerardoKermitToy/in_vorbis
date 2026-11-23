#pragma once
#include "stream.hpp"
#include <map>
#include <memory>
#include <vector>
namespace ogg_input {
inline constexpr int MaxChannels = 8, BlockFrames = 576;
enum class Codec { Unknown, Vorbis, Opus, Flac, Speex };
using Tags = std::map<std::string, std::wstring>;
struct LinkInfo {
    int rate = 0, channels = 0, bitrate = 0, header_gain_q8 = 0;
    int bits_per_sample = 0;
    std::int64_t frames = -1;
    int duration_ms = -1;
    std::uint32_t serial = 0;
    std::wstring vendor, decoder_note;
    Tags tags;
};
struct TrackInfo {
    Codec codec = Codec::Unknown;
    LinkInfo current;
    std::vector<LinkInfo> links;
    int current_link = 0, duration_ms = -1;
    bool seekable = false, radio = false;
    Station station;
    std::wstring path, title;
};
const wchar_t* CodecName(Codec) noexcept;
void AddTag(Tags&, const char*, int);
void SetTitle(TrackInfo&);
std::wstring FormatInfo(const TrackInfo&);
bool Metadata(const TrackInfo&, const char* key, std::wstring&);
// Readers deliver interleaved floating point, in codec channel order. A Read
// returns samples from only one logical link. Info describes that returned block.
class CodecReader {
public:
    virtual ~CodecReader() = default;
    virtual int Read(float* samples, int capacity_frames) = 0;
    virtual bool Seek(int milliseconds) = 0;
    virtual const TrackInfo& Info() const noexcept = 0;
    virtual int InstantBitrate() noexcept = 0;
    virtual bool WindowsOrder() const noexcept { return false; }
    // Optional lossless companion to the last Read, signed and left-aligned to
    // 32 bits. Valid until the next Read/Seek; never inferred from float PCM.
    virtual const std::int32_t* IntegerPcm() const noexcept { return nullptr; }
};
Codec Probe(ByteStream&) noexcept;
std::unique_ptr<CodecReader> OpenOpus(std::shared_ptr<ByteStream>, const Settings&, std::wstring&);
std::unique_ptr<CodecReader> OpenVorbis(std::shared_ptr<ByteStream>, const Settings&, std::wstring&);
std::unique_ptr<CodecReader> OpenFlac(std::shared_ptr<ByteStream>, const Settings&, std::wstring&);
std::unique_ptr<CodecReader> OpenSpeex(std::shared_ptr<ByteStream>, const Settings&, std::wstring&);
}
