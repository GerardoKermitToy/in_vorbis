// SPDX-License-Identifier: GPL-3.0-only
#include "backend.hpp"
#include <vorbis/vorbisfile.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>

namespace ogg_input {
namespace {
constexpr int kMaxLinks = 4096;
constexpr int kMaxCommentCount = 2048;
constexpr int kMetadataBudget = 256 * 1024;
constexpr int kMaxVendorBytes = 65536;

int DurationMs(std::int64_t frames, int rate) noexcept {
    if (frames < 0 || rate <= 0) return -1;
    const long double milliseconds = static_cast<long double>(frames) * 1000.0L / rate;
    if (milliseconds >= INT_MAX) return INT_MAX;
    return static_cast<int>(milliseconds);
}

int DurationMs(double seconds) noexcept {
    if (!std::isfinite(seconds) || seconds < 0.0) return -1;
    const long double milliseconds = static_cast<long double>(seconds) * 1000.0L;
    if (milliseconds >= INT_MAX) return INT_MAX;
    return static_cast<int>(milliseconds);
}

int BoundedCStringLength(const char* text, int limit) noexcept {
    if (!text || limit <= 0) return 0;
    int length = 0;
    while (length < limit && text[length]) ++length;
    return length;
}

size_t VorbisRead(void* destination, size_t size, size_t count, void* source) noexcept {
    if (!destination || !source || !size || !count || size > static_cast<size_t>(INT_MAX)) return 0;
    const size_t elements = std::min(count, static_cast<size_t>(INT_MAX) / size);
    if (!elements) return 0;
    const int read = static_cast<ByteStream*>(source)->Read(destination, static_cast<int>(elements * size));
    return read > 0 ? std::min(elements, static_cast<size_t>(read) / size) : 0;
}

int VorbisSeek(void* source, ogg_int64_t offset, int origin) noexcept {
    if (!source || (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)) return -1;
    if (offset < static_cast<ogg_int64_t>(std::numeric_limits<std::int64_t>::min()) ||
        offset > static_cast<ogg_int64_t>(std::numeric_limits<std::int64_t>::max())) return -1;
    return static_cast<ByteStream*>(source)->Seek(static_cast<std::int64_t>(offset), origin) ? 0 : -1;
}

long VorbisTell(void* source) noexcept {
    if (!source) return -1;
    const auto position = static_cast<ByteStream*>(source)->Tell();
    if (position < 0 || position > std::numeric_limits<long>::max()) return -1;
    return static_cast<long>(position);
}

class VorbisReader final : public CodecReader {
    std::shared_ptr<ByteStream> stream_;
    OggVorbis_File file_{};
    TrackInfo info_;
    std::uint32_t serial_ = 0;
    bool opened_ = false;

    static bool Valid(const LinkInfo& link) noexcept {
        return link.rate > 0 && link.rate <= 768000 && link.channels >= 1 && link.channels <= MaxChannels;
    }

    LinkInfo Link(int index) {
        LinkInfo link;
        const auto* format = ov_info(&file_, index);
        if (!format) return link;
        link.rate = format->rate;
        link.channels = format->channels;
        link.bitrate = std::max(0L, ov_bitrate(&file_, index));
        link.serial = static_cast<std::uint32_t>(ov_serialnumber(&file_, index));
        const auto frames = ov_pcm_total(&file_, index);
        if (frames >= 0) {
            link.frames = frames;
            link.duration_ms = DurationMs(link.frames, link.rate);
        }

        const auto* comment = ov_comment(&file_, index);
        if (!comment) return link;
        const int vendorLength = BoundedCStringLength(comment->vendor, kMaxVendorBytes);
        if (vendorLength) link.vendor = Utf8(comment->vendor, vendorLength);
        int budget = kMetadataBudget;
        const int comments = std::clamp(comment->comments, 0, kMaxCommentCount);
        for (int i = 0; i < comments && budget > 0; ++i) {
            if (!comment->user_comments || !comment->comment_lengths || !comment->user_comments[i]) continue;
            const int length = comment->comment_lengths[i];
            if (length <= 0 || length > budget) break;
            budget -= length;
            if (length <= 65536) AddTag(link.tags, comment->user_comments[i], length);
        }
        return link;
    }

    bool SelectLink(int bitstream) {
        if (info_.seekable) {
            if (bitstream < 0 || bitstream >= static_cast<int>(info_.links.size())) return false;
            if (bitstream == info_.current_link && Valid(info_.current)) return true;
            const auto& link = info_.links[static_cast<size_t>(bitstream)];
            if (!Valid(link)) return false;
            info_.current_link = bitstream;
            info_.current = link;
            serial_ = link.serial;
            return true;
        }

        const auto serial = static_cast<std::uint32_t>(ov_serialnumber(&file_, -1));
        if (serial == serial_ && Valid(info_.current)) return true;
        const int next = info_.current_link + 1;
        if (next < 0 || next >= kMaxLinks) return false;
        auto link = Link(-1);
        if (!Valid(link)) return false;
        if (next == static_cast<int>(info_.links.size())) info_.links.push_back(link);
        else if (next < static_cast<int>(info_.links.size())) info_.links[static_cast<size_t>(next)] = link;
        else return false;
        info_.current_link = next;
        info_.current = std::move(link);
        serial_ = serial;
        return true;
    }

public:
    ~VorbisReader() override {
        if (opened_) ov_clear(&file_);
    }

    bool Open(std::shared_ptr<ByteStream> stream, std::wstring& error) {
        if (!stream) {
            error = L"No input stream was supplied.";
            return false;
        }
        stream_ = std::move(stream);
        const bool canSeek = stream_->Seekable();
        const ov_callbacks callbacks{
            VorbisRead,
            canSeek ? VorbisSeek : nullptr,
            nullptr,
            canSeek ? VorbisTell : nullptr
        };
        const int result = ov_open_callbacks(stream_.get(), &file_, nullptr, 0, callbacks);
        if (result != 0) {
            error = L"Cannot open Ogg Vorbis (error " + std::to_wstring(result) + L").";
            return false;
        }
        opened_ = true;
        info_.codec = Codec::Vorbis;
        info_.seekable = canSeek && ov_seekable(&file_) != 0;
        info_.radio = stream_->Radio();

        const long streams = ov_streams(&file_);
        if (streams < 1 || streams > kMaxLinks) {
            error = L"Invalid number of Ogg Vorbis logical streams.";
            return false;
        }
        if (info_.seekable) {
            for (long i = 0; i < streams; ++i) {
                auto link = Link(static_cast<int>(i));
                if (!Valid(link)) {
                    error = L"Unsupported Ogg Vorbis sample rate or channel count.";
                    return false;
                }
                info_.links.push_back(std::move(link));
            }
            info_.duration_ms = DurationMs(ov_time_total(&file_, -1));
            // libvorbisfile's seekable open finishes with a raw seek.  On a
            // chained file whose first link has a single EOS audio page, that
            // state can be poised at the following link.  Establish an exact
            // decoded position at PCM zero before normal playback begins.
            if (ov_pcm_seek(&file_, 0) != 0) {
                error = L"Cannot seek to the beginning of Ogg Vorbis.";
                return false;
            }
        } else {
            auto link = Link(-1);
            if (!Valid(link)) {
                error = L"Unsupported Ogg Vorbis sample rate or channel count.";
                return false;
            }
            info_.links.push_back(std::move(link));
        }
        info_.current_link = 0;
        info_.current = info_.links.front();
        serial_ = info_.current.serial;
        return true;
    }

    int Read(float* samples, int capacityFrames) override {
        if (!samples || capacityFrames <= 0 || !opened_) return -1;
        float** planes = nullptr;
        int bitstream = -1;
        long decoded = 0;
        int holes = 0;
        do {
            decoded = ov_read_float(&file_, &planes, capacityFrames, &bitstream);
        } while (decoded == OV_HOLE && ++holes < 32);
        if (decoded == 0) return 0;
        if (decoded < 0 || decoded > capacityFrames || !planes || !SelectLink(bitstream)) return -1;

        const int channels = info_.current.channels;
        for (long frame = 0; frame < decoded; ++frame) {
            for (int channel = 0; channel < channels; ++channel) {
                if (!planes[channel]) return -1;
                samples[static_cast<size_t>(frame) * channels + channel] = planes[channel][frame];
            }
        }
        return static_cast<int>(decoded);
    }

    bool Seek(int milliseconds) override {
        if (!opened_ || !info_.seekable || milliseconds < 0) return false;
        const double target = static_cast<double>(milliseconds) / 1000.0;
        if (ov_time_seek(&file_, target) != 0) return false;

        double elapsed = 0.0;
        int link = 0;
        for (; link + 1 < static_cast<int>(info_.links.size()); ++link) {
            const auto duration = ov_time_total(&file_, link);
            if (!std::isfinite(duration) || duration < 0.0 || target < elapsed + duration) break;
            elapsed += duration;
        }
        if (!SelectLink(link)) return false;
        return true;
    }

    const TrackInfo& Info() const noexcept override { return info_; }

    int InstantBitrate() noexcept override {
        return opened_ ? std::max(0L, ov_bitrate_instant(&file_)) : 0;
    }
};
}

std::unique_ptr<CodecReader> OpenVorbis(std::shared_ptr<ByteStream> stream, const Settings&, std::wstring& error) {
    auto reader = std::make_unique<VorbisReader>();
    if (!reader->Open(std::move(stream), error)) return {};
    return reader;
}
}
