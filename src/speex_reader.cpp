// SPDX-License-Identifier: GPL-3.0-only
#include "backend.hpp"
#include <ogg/ogg.h>
#include <speex/speex.h>
#include <speex/speex_bits.h>
#include <speex/speex_callbacks.h>
#include <speex/speex_header.h>
#include <speex/speex_stereo.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace ogg_input {
namespace {
constexpr int kReadChunk = 8192;
constexpr int kMaxLinks = 4096;
constexpr int kMaxComments = 2048;
constexpr int kMetadataBudget = 256 * 1024;
constexpr int kMaxHeaderBytes = 65536;
constexpr int kMaxPacketBytes = 256 * 1024;
constexpr int kMaxExtraHeaders = 64;
constexpr int kMaxFramesPerPacket = 64;
constexpr int kMaxFrameSize = 8192;
constexpr int kMaxDecodedFrames = 65536;
constexpr int kMaxCorruptPages = 32;
constexpr int kMaxPumpSteps = 65536;
constexpr int kMaxBytesWithoutPage = 1024 * 1024;

int DurationMs(std::int64_t frames, int rate) noexcept {
    if (frames < 0 || rate <= 0) return -1;
    const long double milliseconds = static_cast<long double>(frames) * 1000.0L / rate;
    return milliseconds >= INT_MAX ? INT_MAX : static_cast<int>(milliseconds);
}

std::int32_t ReadLe32(const unsigned char* bytes) noexcept {
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    return value <= static_cast<std::uint32_t>(INT_MAX) ? static_cast<std::int32_t>(value) :
        static_cast<std::int32_t>(static_cast<std::int64_t>(value) - 0x100000000LL);
}

struct RawSpeexHeader {
    int version = 0;
    int header_size = 0;
    int rate = 0;
    int mode = 0;
    int mode_bitstream_version = 0;
    int channels = 0;
    int bitrate = 0;
    int frame_size = 0;
    int frames_per_packet = 0;
    int extra_headers = 0;
};

bool ReadRawHeader(const ogg_packet& packet, RawSpeexHeader& header) noexcept {
    if (!packet.packet || packet.bytes < 80 || packet.bytes > kMaxHeaderBytes ||
        std::memcmp(packet.packet, "Speex   ", 8) != 0) return false;
    const auto* bytes = packet.packet;
    header.version = ReadLe32(bytes + 28);
    header.header_size = ReadLe32(bytes + 32);
    header.rate = ReadLe32(bytes + 36);
    header.mode = ReadLe32(bytes + 40);
    header.mode_bitstream_version = ReadLe32(bytes + 44);
    header.channels = ReadLe32(bytes + 48);
    header.bitrate = ReadLe32(bytes + 52);
    header.frame_size = ReadLe32(bytes + 56);
    header.frames_per_packet = ReadLe32(bytes + 64);
    header.extra_headers = ReadLe32(bytes + 68);
    const int reserved1 = ReadLe32(bytes + 72);
    const int reserved2 = ReadLe32(bytes + 76);
    return header.version >= 0 && header.version <= 1 && header.header_size >= 80 &&
        header.header_size <= packet.bytes && header.header_size <= kMaxHeaderBytes &&
        header.rate >= 1000 && header.rate <= 768000 && header.mode >= 0 &&
        header.mode < SPEEX_NB_MODES && header.mode_bitstream_version >= 0 &&
        (header.channels == 1 || header.channels == 2) && header.bitrate >= -1 &&
        header.bitrate <= 1000000000 && header.frame_size > 0 &&
        header.frame_size <= kMaxFrameSize && header.frames_per_packet > 0 &&
        header.frames_per_packet <= kMaxFramesPerPacket && header.extra_headers >= 0 &&
        header.extra_headers <= kMaxExtraHeaders && reserved1 == 0 && reserved2 == 0;
}

class SpeexReader final : public CodecReader {
    enum class Stage { Header, Comment, Extra, Audio };
    enum class PullResult { Packet, Eof, Error };
    enum class HeaderResult { Accepted, NotSpeex, Invalid };

    struct PageState {
        int packets = 0;
        int packet_number = 0;
        ogg_int64_t granule = -1;
        std::int64_t trim = 0;
        bool eos = false;
    };

    std::shared_ptr<ByteStream> stream_;
    TrackInfo info_;
    ogg_sync_state sync_{};
    ogg_stream_state ogg_{};
    SpeexBits bits_{};
    SpeexStereoState* stereo_ = nullptr;
    SpeexCallback stereo_callback_{};
    void* decoder_ = nullptr;
    std::vector<float> frame_;
    std::vector<float> pending_;
    int pending_frames_ = 0;
    int pending_offset_ = 0;
    int pending_channels_ = 0;
    int pending_link_ = -1;
    int active_link_ = -1;
    int active_serial_ = 0;
    int frame_size_ = 0;
    int frames_per_packet_ = 0;
    int extra_headers_ = 0;
    int lookahead_ = 0;
    int corrupt_pages_ = 0;
    int bytes_without_page_ = 0;
    ogg_int64_t last_granule_ = 0;
    Stage stage_ = Stage::Header;
    PageState page_{};
    bool sync_initialized_ = false;
    bool ogg_initialized_ = false;
    bool bits_initialized_ = false;
    bool link_ended_ = false;
    bool input_eof_ = false;
    bool input_error_ = false;
    bool failed_ = false;
    bool initial_headers_ready_ = false;
    bool enhanced_ = true;
    bool source_seekable_ = false;

    void ClearDecoder() noexcept {
        if (decoder_) speex_decoder_destroy(decoder_);
        decoder_ = nullptr;
        if (bits_initialized_) speex_bits_destroy(&bits_);
        bits_initialized_ = false;
        if (stereo_) speex_stereo_state_destroy(stereo_);
        stereo_ = nullptr;
        stereo_callback_ = {};
        frame_.clear();
        frame_size_ = 0;
        frames_per_packet_ = 0;
        extra_headers_ = 0;
        lookahead_ = 0;
    }

    void ClearOgg() noexcept {
        if (ogg_initialized_) ogg_stream_clear(&ogg_);
        ogg_initialized_ = false;
        active_serial_ = 0;
        page_ = {};
        link_ended_ = false;
    }

    void ClearPipeline() noexcept {
        ClearDecoder();
        ClearOgg();
        if (sync_initialized_) ogg_sync_clear(&sync_);
        sync_initialized_ = false;
        pending_.clear();
        pending_frames_ = pending_offset_ = pending_channels_ = 0;
        pending_link_ = -1;
        active_link_ = -1;
    }

    void ResetInfo() noexcept {
        info_ = {};
        info_.codec = Codec::Speex;
        info_.seekable = source_seekable_;
        info_.radio = stream_ && stream_->Radio();
        stage_ = Stage::Header;
        page_ = {};
        last_granule_ = 0;
        corrupt_pages_ = 0;
        bytes_without_page_ = 0;
        input_eof_ = input_error_ = failed_ = false;
        initial_headers_ready_ = false;
    }

    void RefreshKnownDuration() noexcept {
        if (info_.links.empty()) return;
        std::int64_t total = 0;
        for (const auto& link : info_.links) {
            if (link.duration_ms < 0 || total > INT_MAX - link.duration_ms) {
                info_.duration_ms = -1;
                return;
            }
            total += link.duration_ms;
        }
        info_.duration_ms = static_cast<int>(total);
    }

    void PublishLink(int index) {
        if (index < 0 || index >= static_cast<int>(info_.links.size())) {
            failed_ = true;
            return;
        }
        const auto& link = info_.links[static_cast<size_t>(index)];
        // Tags can be large; only copy the current-link metadata at transitions.
        if (info_.current_link != index || info_.current.rate != link.rate ||
            info_.current.channels != link.channels || info_.current.serial != link.serial) {
            info_.current_link = index;
            info_.current = link;
        }
    }

    void ClearPending() noexcept {
        pending_.clear();
        pending_frames_ = pending_offset_ = pending_channels_ = 0;
        pending_link_ = -1;
    }

    int CopyPending(float* output, int capacity) {
        if (!pending_frames_ || pending_offset_ >= pending_frames_) return 0;
        PublishLink(pending_link_);
        if (failed_) return -1;
        const int frames = std::min(capacity, pending_frames_ - pending_offset_);
        const size_t first = static_cast<size_t>(pending_offset_) * pending_channels_;
        const size_t count = static_cast<size_t>(frames) * pending_channels_;
        std::copy_n(pending_.data() + first, count, output);
        pending_offset_ += frames;
        if (pending_offset_ == pending_frames_) ClearPending();
        return frames;
    }

    void ConsumePending(int frames) noexcept {
        if (frames <= 0 || !pending_frames_) return;
        pending_offset_ = std::min(pending_frames_, pending_offset_ + frames);
        if (pending_offset_ == pending_frames_) ClearPending();
    }

    bool ReadMore() {
        if (input_eof_ || input_error_ || !stream_) return false;
        char* destination = ogg_sync_buffer(&sync_, kReadChunk);
        if (!destination) {
            failed_ = true;
            return false;
        }
        const int received = stream_->Read(destination, kReadChunk);
        if (received > 0) {
            if (ogg_sync_wrote(&sync_, received) != 0) {
                failed_ = true;
                return false;
            }
            bytes_without_page_ += received;
            if (bytes_without_page_ > kMaxBytesWithoutPage) failed_ = true;
            return !failed_;
        }
        if (received < 0) input_error_ = true;
        else input_eof_ = true;
        return false;
    }

    void SetupPage(const ogg_page& page) {
        page_ = {};
        page_.packets = std::max(0, ogg_page_packets(&page));
        page_.granule = ogg_page_granulepos(&page);
        page_.eos = ogg_page_eos(&page) != 0;
        corrupt_pages_ = 0;
        bytes_without_page_ = 0;
        if (active_link_ < 0 || frame_size_ <= 0 || page_.granule < 0) return;

        if (stage_ == Stage::Audio) {
            const std::int64_t packet_frames = static_cast<std::int64_t>(page_.packets) * frames_per_packet_ * frame_size_;
            const std::int64_t delta = static_cast<std::int64_t>(page_.granule) - static_cast<std::int64_t>(last_granule_);
            if (delta < 0 || packet_frames > kMaxDecodedFrames * kMaxFramesPerPacket) {
                failed_ = true;
                return;
            }
            page_.trim = packet_frames - delta;
            if (page_.eos) page_.trim = -page_.trim;
        }
        last_granule_ = page_.granule;
    }

    bool StartOggStream(int serial) {
        if (ogg_stream_init(&ogg_, serial) != 0) {
            failed_ = true;
            return false;
        }
        ogg_initialized_ = true;
        active_serial_ = serial;
        active_link_ = -1;
        stage_ = Stage::Header;
        page_ = {};
        last_granule_ = 0;
        link_ended_ = false;
        return true;
    }

    void RejectCandidate() noexcept {
        ClearDecoder();
        ClearOgg();
        active_link_ = -1;
        stage_ = Stage::Header;
    }

    void FinishLink(ogg_int64_t granule) noexcept {
        if (active_link_ < 0 || active_link_ >= static_cast<int>(info_.links.size())) {
            failed_ = true;
            return;
        }
        auto& link = info_.links[static_cast<size_t>(active_link_)];
        if (granule >= 0 && granule <= static_cast<ogg_int64_t>(std::numeric_limits<std::int64_t>::max())) {
            link.frames = static_cast<std::int64_t>(granule);
            link.duration_ms = DurationMs(link.frames, link.rate);
            if (info_.current_link == active_link_ && info_.current.serial == link.serial) {
                info_.current.frames = link.frames;
                info_.current.duration_ms = link.duration_ms;
            }
        }
        RefreshKnownDuration();
        link_ended_ = true;
    }

    void RetireFinishedLink() noexcept {
        ClearDecoder();
        ClearOgg();
        active_link_ = -1;
        stage_ = Stage::Header;
    }

    PullResult PullPacket(ogg_packet& packet) {
        for (int steps = 0; steps < kMaxPumpSteps; ++steps) {
            if (failed_ || input_error_) return PullResult::Error;
            if (ogg_initialized_) {
                const int packet_result = ogg_stream_packetout(&ogg_, &packet);
                if (packet_result < 0) {
                    failed_ = true;
                    return PullResult::Error;
                }
                if (packet_result > 0) {
                    if (link_ended_) {
                        failed_ = true;
                        return PullResult::Error;
                    }
                    ++page_.packet_number;
                    return PullResult::Packet;
                }
                if (link_ended_) {
                    RetireFinishedLink();
                    continue;
                }
            }

            ogg_page page{};
            const int page_result = ogg_sync_pageout(&sync_, &page);
            if (page_result < 0) {
                if (++corrupt_pages_ > kMaxCorruptPages) {
                    failed_ = true;
                    return PullResult::Error;
                }
                continue;
            }
            if (page_result == 0) {
                if (input_eof_) {
                    if (active_link_ >= 0 || ogg_initialized_) {
                        failed_ = true;
                        return PullResult::Error;
                    }
                    RefreshKnownDuration();
                    return PullResult::Eof;
                }
                if (!ReadMore()) {
                    if (failed_ || input_error_) return PullResult::Error;
                    continue;
                }
                continue;
            }

            corrupt_pages_ = 0;
            bytes_without_page_ = 0;
            const int serial = ogg_page_serialno(&page);
            if (!ogg_initialized_) {
                if (!ogg_page_bos(&page) || !StartOggStream(serial)) continue;
            }
            if (serial != active_serial_) continue;
            if (ogg_stream_pagein(&ogg_, &page) != 0) {
                failed_ = true;
                return PullResult::Error;
            }
            SetupPage(page);
            if (failed_) return PullResult::Error;
        }
        failed_ = true;
        return PullResult::Error;
    }

    HeaderResult ProcessHeader(const ogg_packet& packet) {
        if (!packet.packet || packet.bytes < 8 || std::memcmp(packet.packet, "Speex   ", 8) != 0)
            return HeaderResult::NotSpeex;
        RawSpeexHeader raw;
        if (!ReadRawHeader(packet, raw)) return HeaderResult::Invalid;

        auto* header = speex_packet_to_header(reinterpret_cast<char*>(packet.packet), packet.bytes);
        if (!header) return HeaderResult::Invalid;
        const bool matches = header->speex_version_id == raw.version && header->header_size == raw.header_size &&
            header->rate == raw.rate && header->mode == raw.mode &&
            header->mode_bitstream_version == raw.mode_bitstream_version && header->nb_channels == raw.channels &&
            header->bitrate == raw.bitrate && header->frame_size == raw.frame_size &&
            header->frames_per_packet == raw.frames_per_packet && header->extra_headers == raw.extra_headers;
        if (!matches) {
            speex_header_free(header);
            return HeaderResult::Invalid;
        }

        const SpeexMode* mode = speex_lib_get_mode(raw.mode);
        if (!mode || mode->bitstream_version != raw.mode_bitstream_version) {
            speex_header_free(header);
            return HeaderResult::Invalid;
        }
        void* decoder = speex_decoder_init(mode);
        if (!decoder) {
            speex_header_free(header);
            return HeaderResult::Invalid;
        }
        spx_int32_t enhanced = enhanced_ ? 1 : 0;
        int decoded_frame_size = 0;
        int decoded_lookahead = 0;
        bool valid = speex_decoder_ctl(decoder, SPEEX_SET_ENH, &enhanced) == 0 &&
            speex_decoder_ctl(decoder, SPEEX_SET_SAMPLING_RATE, &header->rate) == 0 &&
            speex_decoder_ctl(decoder, SPEEX_GET_FRAME_SIZE, &decoded_frame_size) == 0 &&
            speex_decoder_ctl(decoder, SPEEX_GET_LOOKAHEAD, &decoded_lookahead) == 0 &&
            decoded_frame_size == raw.frame_size && decoded_frame_size > 0 &&
            decoded_frame_size <= kMaxFrameSize && decoded_lookahead >= 0 &&
            decoded_lookahead <= kMaxFrameSize;
        if (!valid) {
            speex_decoder_destroy(decoder);
            speex_header_free(header);
            return HeaderResult::Invalid;
        }

        SpeexStereoState* stereo = nullptr;
        SpeexCallback callback{};
        if (raw.channels == 2) {
            stereo = speex_stereo_state_init();
            if (!stereo) {
                speex_decoder_destroy(decoder);
                speex_header_free(header);
                return HeaderResult::Invalid;
            }
            callback.callback_id = SPEEX_INBAND_STEREO;
            callback.func = speex_std_stereo_request_handler;
            callback.data = stereo;
            if (speex_decoder_ctl(decoder, SPEEX_SET_HANDLER, &callback) != 0) {
                speex_stereo_state_destroy(stereo);
                speex_decoder_destroy(decoder);
                speex_header_free(header);
                return HeaderResult::Invalid;
            }
        }

        try {
            const size_t samples = static_cast<size_t>(decoded_frame_size) * raw.channels;
            if (samples > static_cast<size_t>(kMaxFrameSize) * MaxChannels) throw std::bad_alloc();
            frame_.assign(samples, 0.0f);
            LinkInfo link;
            link.rate = raw.rate;
            link.channels = raw.channels;
            link.bitrate = std::max(0, raw.bitrate);
            link.serial = static_cast<std::uint32_t>(active_serial_);
            if (info_.links.size() >= kMaxLinks) throw std::bad_alloc();
            info_.links.push_back(std::move(link));
        } catch (...) {
            if (stereo) speex_stereo_state_destroy(stereo);
            speex_decoder_destroy(decoder);
            speex_header_free(header);
            frame_.clear();
            return HeaderResult::Invalid;
        }
        speex_header_free(header);

        decoder_ = decoder;
        stereo_ = stereo;
        stereo_callback_ = callback;
        speex_bits_init(&bits_);
        bits_initialized_ = true;
        active_link_ = static_cast<int>(info_.links.size()) - 1;
        frame_size_ = decoded_frame_size;
        frames_per_packet_ = raw.frames_per_packet;
        extra_headers_ = raw.extra_headers;
        lookahead_ = decoded_lookahead;
        last_granule_ = page_.granule >= 0 ? page_.granule : 0;
        stage_ = Stage::Comment;
        return HeaderResult::Accepted;
    }

    bool ProcessComments(const ogg_packet& packet) {
        if (!packet.packet || packet.bytes < 8 || packet.bytes > kMetadataBudget || active_link_ < 0 ||
            active_link_ >= static_cast<int>(info_.links.size())) return false;
        const auto* bytes = packet.packet;
        size_t at = 0;
        const size_t size = static_cast<size_t>(packet.bytes);
        const auto get32 = [&](std::uint32_t& value) noexcept {
            if (size - at < 4) return false;
            value = static_cast<std::uint32_t>(bytes[at]) |
                (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
                (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
                (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
            at += 4;
            return true;
        };
        std::uint32_t vendor_length = 0;
        if (!get32(vendor_length) || vendor_length > size - at || vendor_length > static_cast<std::uint32_t>(kMetadataBudget)) return false;
        auto& link = info_.links[static_cast<size_t>(active_link_)];
        int budget = kMetadataBudget - static_cast<int>(vendor_length);
        if (vendor_length && vendor_length <= 65536)
            link.vendor = Utf8(reinterpret_cast<const char*>(bytes + at), static_cast<int>(vendor_length));
        at += vendor_length;
        std::uint32_t comment_count = 0;
        if (!get32(comment_count) || comment_count > kMaxComments) return false;
        for (std::uint32_t index = 0; index < comment_count; ++index) {
            std::uint32_t length = 0;
            if (!get32(length) || length > size - at) return false;
            if (length > static_cast<std::uint32_t>(budget)) return false;
            budget -= static_cast<int>(length);
            if (length && length <= 65536)
                AddTag(link.tags, reinterpret_cast<const char*>(bytes + at), static_cast<int>(length));
            at += length;
        }
        return true;
    }

    bool DecodeAudio(const ogg_packet& packet) {
        if (!decoder_ || !bits_initialized_ || active_link_ < 0 || !packet.packet || packet.bytes <= 0 ||
            packet.bytes > kMaxPacketBytes || frame_size_ <= 0 || frames_per_packet_ <= 0) return false;
        const int channels = info_.links[static_cast<size_t>(active_link_)].channels;
        const std::int64_t decoded_frames = static_cast<std::int64_t>(frame_size_) * frames_per_packet_;
        if (channels < 1 || channels > 2 || decoded_frames <= 0 || decoded_frames > kMaxDecodedFrames ||
            static_cast<size_t>(decoded_frames) * channels > static_cast<size_t>(kMaxDecodedFrames) * MaxChannels) return false;

        try {
            pending_.clear();
            pending_.reserve(static_cast<size_t>(decoded_frames) * channels);
        } catch (...) {
            return false;
        }
        speex_bits_read_from(&bits_, reinterpret_cast<char*>(packet.packet), packet.bytes);
        for (int frame = 0; frame < frames_per_packet_; ++frame) {
            const int result = speex_decode(decoder_, &bits_, frame_.data());
            if (result != 0 || speex_bits_remaining(&bits_) < 0) return false;
            if (channels == 2) speex_decode_stereo(frame_.data(), frame_size_, stereo_);

            int first = 0;
            int last = frame_size_;
            if (page_.packet_number == 1 && frame == 0 && page_.trim > 0) {
                const std::int64_t drop = page_.trim + lookahead_;
                first = static_cast<int>(std::clamp<std::int64_t>(drop, 0, frame_size_));
            }
            if (page_.packet_number == page_.packets && page_.trim < 0) {
                const std::int64_t end = static_cast<std::int64_t>(frames_per_packet_) * frame_size_ +
                    page_.trim + lookahead_ - static_cast<std::int64_t>(frame) * frame_size_;
                last = static_cast<int>(std::clamp<std::int64_t>(end, 0, frame_size_));
            }
            last = std::max(first, last);
            for (int sample = first; sample < last; ++sample) {
                for (int channel = 0; channel < channels; ++channel)
                    pending_.push_back(frame_[static_cast<size_t>(sample) * channels + channel] / 32768.0f);
            }
        }
        pending_channels_ = channels;
        pending_frames_ = static_cast<int>(pending_.size() / static_cast<size_t>(channels));
        pending_offset_ = 0;
        pending_link_ = pending_frames_ ? active_link_ : -1;
        return true;
    }

    bool ProcessPacket(const ogg_packet& packet) {
        if (active_link_ < 0) {
            const auto result = ProcessHeader(packet);
            if (result == HeaderResult::NotSpeex) {
                RejectCandidate();
                return true;
            }
            if (result != HeaderResult::Accepted) return false;
            return true;
        }
        switch (stage_) {
        case Stage::Comment:
            if (!ProcessComments(packet)) return false;
            if (extra_headers_ > 0) stage_ = Stage::Extra;
            else {
                stage_ = Stage::Audio;
                if (!initial_headers_ready_) {
                    initial_headers_ready_ = true;
                    PublishLink(active_link_);
                }
            }
            return !failed_;
        case Stage::Extra:
            if (--extra_headers_ == 0) {
                stage_ = Stage::Audio;
                if (!initial_headers_ready_) {
                    initial_headers_ready_ = true;
                    PublishLink(active_link_);
                }
            }
            return !failed_;
        case Stage::Audio:
            if (!DecodeAudio(packet)) return false;
            if (packet.e_o_s) FinishLink(packet.granulepos >= 0 ? packet.granulepos : page_.granule);
            return !failed_;
        case Stage::Header:
            return false;
        }
        return false;
    }

    bool Initialize() {
        if (ogg_sync_init(&sync_) != 0) return false;
        sync_initialized_ = true;
        for (int steps = 0; steps < kMaxPumpSteps && !initial_headers_ready_; ++steps) {
            ogg_packet packet{};
            const auto result = PullPacket(packet);
            if (result != PullResult::Packet || !ProcessPacket(packet)) return false;
        }
        return initial_headers_ready_ && !failed_ && !input_error_ && !info_.links.empty();
    }

    enum class PendingResult { Available, Eof, Error };

    PendingResult ProducePending() {
        if (pending_frames_ > pending_offset_) return PendingResult::Available;
        for (int steps = 0; steps < kMaxPumpSteps; ++steps) {
            ogg_packet packet{};
            const auto result = PullPacket(packet);
            if (result == PullResult::Eof) return PendingResult::Eof;
            if (result != PullResult::Packet || !ProcessPacket(packet)) return PendingResult::Error;
            if (pending_frames_ > pending_offset_) return PendingResult::Available;
        }
        failed_ = true;
        return PendingResult::Error;
    }

    bool ResetForSeek() {
        if (!stream_ || !source_seekable_ || !stream_->Seek(0, SEEK_SET)) return false;
        ClearPipeline();
        ResetInfo();
        return Initialize();
    }

    bool DiscardTo(int milliseconds) {
        long double remaining = static_cast<long double>(milliseconds) / 1000.0L;
        while (remaining > 0.0L) {
            const auto result = ProducePending();
            if (result != PendingResult::Available) return false;
            if (pending_link_ < 0 || pending_link_ >= static_cast<int>(info_.links.size())) return false;
            PublishLink(pending_link_);
            if (failed_) return false;
            const int rate = info_.links[static_cast<size_t>(pending_link_)].rate;
            const int available = pending_frames_ - pending_offset_;
            if (rate <= 0 || available <= 0) return false;
            const long double duration = static_cast<long double>(available) / rate;
            if (remaining >= duration) {
                ConsumePending(available);
                remaining -= duration;
                continue;
            }
            const auto frames = static_cast<int>(std::clamp<long double>(
                std::floor(remaining * rate), 0.0L, static_cast<long double>(available)));
            ConsumePending(frames);
            return true;
        }
        return true;
    }

public:
    ~SpeexReader() override { ClearPipeline(); }

    bool Open(std::shared_ptr<ByteStream> stream, const Settings& settings, std::wstring& error) {
        if (!stream) {
            error = L"No input stream was supplied.";
            return false;
        }
        stream_ = std::move(stream);
        enhanced_ = settings.enhanced_speex;
        source_seekable_ = stream_->Seekable();
        ResetInfo();
        if (!Initialize()) {
            error = L"Cannot open Ogg Speex stream.";
            return false;
        }
        return true;
    }

    int Read(float* samples, int capacityFrames) override {
        if (!samples || capacityFrames <= 0 || failed_ || input_error_) return -1;
        const int available = CopyPending(samples, capacityFrames);
        if (available) return available;
        if (failed_) return -1;
        const auto result = ProducePending();
        if (result == PendingResult::Error) return -1;
        if (result == PendingResult::Eof) return 0;
        return CopyPending(samples, capacityFrames);
    }

    bool Seek(int milliseconds) override {
        if (milliseconds < 0 || !source_seekable_ || !ResetForSeek()) return false;
        return DiscardTo(milliseconds);
    }

    const TrackInfo& Info() const noexcept override { return info_; }

    int InstantBitrate() noexcept override {
        if (decoder_) {
            spx_int32_t bitrate = 0;
            if (speex_decoder_ctl(decoder_, SPEEX_GET_BITRATE, &bitrate) == 0) return std::max(0, static_cast<int>(bitrate));
        }
        return std::max(0, info_.current.bitrate);
    }
};
}

std::unique_ptr<CodecReader> OpenSpeex(std::shared_ptr<ByteStream> stream, const Settings& settings, std::wstring& error) {
    auto reader = std::make_unique<SpeexReader>();
    if (!reader->Open(std::move(stream), settings, error)) return {};
    return reader;
}
}
