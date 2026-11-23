#pragma once
#include "backend.hpp"
#include <array>
#include <mutex>
#include <speex_resampler.h>
namespace ogg_input {
class Quantizer;
class Audio final {
public:
    // Native output negotiation is only safe for playback. Extended readers keep
    // the immutable format promised by their open function.
    bool Open(const wchar_t*, Settings, bool shuffle, std::wstring& error, bool negotiate = false);
    int Read(float*, int capacity_frames);
    const std::int32_t* IntegerPcm() const noexcept { return last_exact_ ? integer_.data() : nullptr; }
    void FallbackToStereo(float* pending, int frames);
    bool Seek(int milliseconds);
    void Cancel() noexcept;
    const TrackInfo& Info() const noexcept { return info_; }
    unsigned MetadataRevision() const noexcept { return metadata_revision_; }
    int Channels() const noexcept { return channels_; }
    int Rate() const noexcept { return rate_; }
    int SourceRate() const noexcept { return source_rate_; }
    int InstantBitrate() noexcept { return reader_ ? reader_->InstantBitrate() : -1; }
    const std::wstring& Error() const noexcept { return error_; }
private:
    void Gain();
    std::shared_ptr<ByteStream> stream_;
    std::mutex stream_mutex_;
    std::atomic<bool> cancelled_{false};
    std::unique_ptr<CodecReader> reader_;
    TrackInfo info_;
    unsigned metadata_revision_ = 0;
    Settings settings_;
    bool shuffle_ = false, negotiate_ = false;
    int channels_ = 2, rate_ = 48000, source_rate_ = 48000, gain_link_ = -1;
    double gain_ = 1;
    std::wstring error_;
    std::array<float, BlockFrames * MaxChannels> scratch_{};
    std::array<std::int32_t, BlockFrames * MaxChannels> integer_{};
    bool last_exact_ = false;
};
class Pcm final {
public:
    static constexpr int FormatChange = -2;
    Pcm(Audio&, Settings);
    ~Pcm();
    bool Ready() const noexcept { return ready_; }
    int Read(void*, int capacity_frames);
    int ReadFloat(float*, int capacity_frames);
    // FormatChange is returned only after all old-format PCM has been delivered.
    // The first new-format block is retained until the output has been reopened.
    bool AcceptFormat(bool stereo_fallback = false);
    int NextRate() const noexcept { return audio_.Rate(); }
    int NextChannels() const noexcept { return audio_.Channels(); }
    void Reset(int milliseconds = 0) noexcept;
    int PositionMs() const noexcept;
    int Rate() const noexcept { return rate_; }
    int Channels() const noexcept { return channels_; }
private:
    bool SetRate(int);
    void FinishSegment() noexcept;
    int Render(int capacity_frames);
    Audio& audio_;
    Settings settings_;
    std::unique_ptr<Quantizer> quantizer_;
    SpeexResamplerState* resampler_ = nullptr;
    bool ready_ = true, eof_ = false, format_pending_ = false;
    int rate_ = 0, channels_ = 0, next_frames_ = 0;
    int source_rate_ = 0, input_frames_ = 0, input_offset_ = 0, padding_ = 0, pending_error_ = 0;
    std::int64_t emitted_ = 0, output_target_ = 0;
    long double expected_ = 0, base_ms_ = 0;
    std::array<float, BlockFrames * MaxChannels> input_{}, output_{}, next_input_{};
    std::array<std::int32_t, BlockFrames * MaxChannels> integer_input_{}, integer_output_{}, next_integer_{};
    std::array<bool, BlockFrames> output_exact_{};
    bool input_exact_ = false, next_exact_ = false;
};
}
