// SPDX-License-Identifier: GPL-3.0-only
#include "backend.hpp"
#include <FLAC/stream_decoder.h>
#include <ogg/ogg.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace ogg_input {
namespace {
constexpr int kMaxLinks = 4096;
constexpr int kMaxComments = 2048;
constexpr int kMetadataBudget = 256 * 1024;
constexpr unsigned kMaxFrameSamples = 65536;

int DurationMs(std::int64_t frames, int rate) noexcept {
    if (frames < 0 || rate <= 0) return -1;
    const long double milliseconds = static_cast<long double>(frames) * 1000.0L / rate;
    return milliseconds >= INT_MAX ? INT_MAX : static_cast<int>(milliseconds);
}

class FlacReader final : public CodecReader {
    std::shared_ptr<ByteStream> stream_;
    FLAC__StreamDecoder* decoder_ = nullptr;
    TrackInfo info_;
    std::vector<float> pending_;
    std::vector<std::int32_t> pending_integer_, exact_;
    int pending_frames_ = 0;
    int pending_offset_ = 0;
    int pending_channels_ = 0;
    int pending_link_ = -1;
    int metadata_link_ = -1;
    bool initialized_ = false;
    bool input_error_ = false;
    bool failed_ = false;
    bool seeking_ = false;
    bool indexed_ = false;
    bool seek_eof_ = false;
    struct EncodedLink { long serial; std::int64_t bytes; };
    std::vector<EncodedLink> encoded_links_;
    ogg_sync_state bitrate_sync_{};
    bool counting_bytes_ = false;
    int encoded_link_ = -1;

    bool CountEncodedBytes(const FLAC__byte* data, int bytes) noexcept {
        try {
            auto* buffer = ogg_sync_buffer(&bitrate_sync_, bytes);
            if (!buffer) return false;
            std::memcpy(buffer, data, bytes);
            if (ogg_sync_wrote(&bitrate_sync_, bytes)) return false;
            ogg_page page{};
            for (;;) {
                const int result = ogg_sync_pageout(&bitrate_sync_, &page);
                if (!result) return true;
                if (result < 0) return false;
                const auto serial = ogg_page_serialno(&page);
                if (encoded_link_ < 0 && ogg_page_bos(&page) && page.body_len >= 5 &&
                    !std::memcmp(page.body, "\177FLAC", 5)) {
                    if (encoded_links_.size() >= kMaxLinks) return false;
                    encoded_links_.push_back({serial, 0});
                    encoded_link_ = static_cast<int>(encoded_links_.size()) - 1;
                }
                if (encoded_link_ < 0 || encoded_links_[encoded_link_].serial != serial) continue;
                auto& link = encoded_links_[encoded_link_];
                const auto count = static_cast<std::int64_t>(page.header_len) + page.body_len;
                if (link.bytes > std::numeric_limits<std::int64_t>::max() - count) return false;
                link.bytes += count;
                if (ogg_page_eos(&page)) encoded_link_ = -1;
            }
        } catch (...) { return false; }
    }

    void ClearPending() noexcept {
        pending_.clear(); pending_integer_.clear(); exact_.clear();
        pending_frames_ = pending_offset_ = pending_channels_ = 0;
        pending_link_ = -1;
    }

    static bool ValidFormat(unsigned rate, unsigned channels, unsigned bits) noexcept {
        return rate >= 1000 && rate <= 768000 && channels >= 1 && channels <= MaxChannels && bits >= 4 && bits <= 32;
    }

    void RefreshKnownDuration() noexcept {
        if (info_.links.empty()) return;
        long double total = 0;
        for (const auto& link : info_.links) {
            if (link.frames < 0 || link.rate <= 0) {
                info_.duration_ms = -1;
                return;
            }
            total += static_cast<long double>(link.frames) * 1000 / link.rate;
        }
        info_.duration_ms = total >= INT_MAX ? INT_MAX : static_cast<int>(total);
    }

    void PublishPendingLink() {
        if (pending_link_ < 0 || pending_link_ >= static_cast<int>(info_.links.size())) {
            failed_ = true;
            return;
        }
        const auto& link = info_.links[pending_link_];
        // Avoid repeatedly copying the potentially large tag map for every frame.
        if (info_.current_link != pending_link_ || info_.current.rate != link.rate ||
            info_.current.channels != link.channels || info_.current.bitrate != link.bitrate) {
            info_.current_link = pending_link_;
            info_.current = link;
        }
    }

    int CopyPending(float* output, int capacity) {
        if (!pending_frames_ || pending_offset_ >= pending_frames_) return 0;
        PublishPendingLink();
        if (failed_) return -1;
        const int frames = std::min(capacity, pending_frames_ - pending_offset_);
        const auto first = static_cast<size_t>(pending_offset_) * pending_channels_;
        const auto count = static_cast<size_t>(frames) * pending_channels_;
        exact_.assign(pending_integer_.begin() + first, pending_integer_.begin() + first + count);
        std::copy_n(pending_.data() + first, count, output);
        pending_offset_ += frames;
        if (pending_offset_ == pending_frames_) {
            pending_.clear();
            pending_frames_ = pending_offset_ = pending_channels_ = 0;
            pending_link_ = -1;
        }
        return frames;
    }

    void OnMetadata(const FLAC__StreamMetadata* metadata) noexcept {
        if (!metadata || failed_) {
            failed_ = true;
            return;
        }
        try {
            if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
                const auto& streamInfo = metadata->data.stream_info;
                if (!ValidFormat(streamInfo.sample_rate, streamInfo.channels, streamInfo.bits_per_sample)) {
                    failed_ = true;
                    return;
                }
                if (indexed_ || seeking_) {
                    const int next = seeking_ ? metadata_link_ : metadata_link_ + 1;
                    if (next < 0 || next >= static_cast<int>(info_.links.size())) {
                        failed_ = true;
                        return;
                    }
                    const auto& known = info_.links[next];
                    if (known.rate != static_cast<int>(streamInfo.sample_rate) ||
                        known.channels != static_cast<int>(streamInfo.channels) ||
                        known.bits_per_sample != static_cast<int>(streamInfo.bits_per_sample)) {
                        failed_ = true;
                        return;
                    }
                    metadata_link_ = next;
                    return;
                }
                if (info_.links.size() >= kMaxLinks) {
                    failed_ = true;
                    return;
                }
                LinkInfo link;
                link.rate = static_cast<int>(streamInfo.sample_rate);
                link.channels = static_cast<int>(streamInfo.channels);
                link.bits_per_sample = static_cast<int>(streamInfo.bits_per_sample);
                if (streamInfo.total_samples <= static_cast<FLAC__uint64>(std::numeric_limits<std::int64_t>::max())) {
                    link.frames = static_cast<std::int64_t>(streamInfo.total_samples);
                    if (link.frames == 0) link.frames = -1;
                    else link.duration_ms = DurationMs(link.frames, link.rate);
                }
                info_.links.push_back(std::move(link));
                metadata_link_ = static_cast<int>(info_.links.size()) - 1;
                RefreshKnownDuration();
                return;
            }
            if (indexed_ || seeking_ || metadata->type != FLAC__METADATA_TYPE_VORBIS_COMMENT || metadata_link_ < 0 ||
                metadata_link_ >= static_cast<int>(info_.links.size())) return;

            auto& link = info_.links[metadata_link_];
            const auto& comments = metadata->data.vorbis_comment;
            if (comments.vendor_string.entry && comments.vendor_string.length <= 65536)
                link.vendor = Utf8(reinterpret_cast<const char*>(comments.vendor_string.entry), static_cast<int>(comments.vendor_string.length));
            if (comments.num_comments > kMaxComments || !comments.comments) return;
            int budget = kMetadataBudget;
            for (FLAC__uint32 n = 0; n < comments.num_comments && budget > 0; ++n) {
                const auto& entry = comments.comments[n];
                if (!entry.entry || entry.length == 0 || entry.length > static_cast<FLAC__uint32>(budget)) break;
                budget -= static_cast<int>(entry.length);
                if (entry.length <= 65536) AddTag(link.tags, reinterpret_cast<const char*>(entry.entry), static_cast<int>(entry.length));
            }
        } catch (...) {
            failed_ = true;
        }
    }

    FLAC__StreamDecoderWriteStatus OnFrame(const FLAC__Frame* frame, const FLAC__int32* const buffer[]) noexcept {
        if (!frame || !buffer || metadata_link_ < 0 || metadata_link_ >= static_cast<int>(info_.links.size()) || pending_frames_) {
            failed_ = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        const auto& header = frame->header;
        if (!ValidFormat(header.sample_rate, header.channels, header.bits_per_sample) || header.blocksize == 0 ||
            header.blocksize > kMaxFrameSamples || static_cast<size_t>(header.blocksize) * header.channels > static_cast<size_t>(kMaxFrameSamples) * MaxChannels) {
            failed_ = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        for (unsigned channel = 0; channel < header.channels; ++channel) {
            if (!buffer[channel]) {
                failed_ = true;
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
        }
        try {
            auto& link = info_.links[metadata_link_];
            link.rate = static_cast<int>(header.sample_rate);
            link.channels = static_cast<int>(header.channels);
            link.bits_per_sample = static_cast<int>(header.bits_per_sample);
            const double scale = std::ldexp(1.0, static_cast<int>(header.bits_per_sample) - 1);
            pending_.resize(static_cast<size_t>(header.blocksize) * header.channels);
            pending_integer_.resize(pending_.size());
            const std::int64_t integer_scale = INT64_C(1) << (32 - header.bits_per_sample);
            for (unsigned sample = 0; sample < header.blocksize; ++sample) {
                for (unsigned channel = 0; channel < header.channels; ++channel) {
                    const auto index = static_cast<size_t>(sample) * header.channels + channel;
                    pending_integer_[index] = static_cast<std::int32_t>(buffer[channel][sample] * integer_scale);
                    pending_[index] = static_cast<float>(static_cast<double>(buffer[channel][sample]) / scale);
                }
            }
            pending_frames_ = static_cast<int>(header.blocksize);
            pending_offset_ = 0;
            pending_channels_ = static_cast<int>(header.channels);
            pending_link_ = metadata_link_;
        } catch (...) {
            failed_ = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static FLAC__StreamDecoderReadStatus ReadCallback(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* client) noexcept {
        auto* self = static_cast<FlacReader*>(client);
        if (!self || !buffer || !bytes || !*bytes) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        const size_t wanted = std::min(*bytes, static_cast<size_t>(INT_MAX));
        const int received = self->stream_->Read(buffer, static_cast<int>(wanted));
        if (received > 0) {
            if (self->counting_bytes_ && !self->CountEncodedBytes(buffer, received)) {
                self->input_error_ = true;
                *bytes = 0;
                return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
            }
            *bytes = static_cast<size_t>(received);
            return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
        }
        *bytes = 0;
        if (received < 0) self->input_error_ = true;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    static FLAC__StreamDecoderSeekStatus SeekCallback(const FLAC__StreamDecoder*, FLAC__uint64 absolute, void* client) noexcept {
        auto* self = static_cast<FlacReader*>(client);
        if (!self || absolute > static_cast<FLAC__uint64>(std::numeric_limits<std::int64_t>::max())) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
        return self->stream_->Seek(static_cast<std::int64_t>(absolute), SEEK_SET) ?
            FLAC__STREAM_DECODER_SEEK_STATUS_OK : FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }

    static FLAC__StreamDecoderTellStatus TellCallback(const FLAC__StreamDecoder*, FLAC__uint64* position, void* client) noexcept {
        auto* self = static_cast<FlacReader*>(client);
        if (!self || !position) return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
        const auto current = self->stream_->Tell();
        if (current < 0) return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
        *position = static_cast<FLAC__uint64>(current);
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    static FLAC__StreamDecoderLengthStatus LengthCallback(const FLAC__StreamDecoder*, FLAC__uint64* length, void* client) noexcept {
        auto* self = static_cast<FlacReader*>(client);
        if (!self || !length) return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
        const auto size = self->stream_->Size();
        if (size < 0) return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
        *length = static_cast<FLAC__uint64>(size);
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }

    static FLAC__bool EofCallback(const FLAC__StreamDecoder*, void* client) noexcept {
        auto* self = static_cast<FlacReader*>(client);
        if (!self) return true;
        const auto size = self->stream_->Size();
        return size >= 0 && self->stream_->Tell() >= size;
    }

    static FLAC__StreamDecoderWriteStatus WriteCallback(const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const buffer[], void* client) noexcept {
        auto* self = static_cast<FlacReader*>(client);
        return self ? self->OnFrame(frame, buffer) : FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    static void MetadataCallback(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata, void* client) noexcept {
        if (auto* self = static_cast<FlacReader*>(client)) self->OnMetadata(metadata);
    }

    static void ErrorCallback(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* client) noexcept {
        if (auto* self = static_cast<FlacReader*>(client)) self->failed_ = true;
    }

    bool ProcessNext() {
        for (int attempts = 0; attempts < 128; ++attempts) {
            if (pending_frames_) return true;
            if (failed_ || input_error_) return false;
            const auto state = FLAC__stream_decoder_get_state(decoder_);
            if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
                RefreshKnownDuration();
                return true;
            }
            if (state == FLAC__STREAM_DECODER_END_OF_LINK) {
                if (!FLAC__stream_decoder_finish_link(decoder_) || !FLAC__stream_decoder_process_until_end_of_metadata(decoder_) || failed_ || input_error_) return false;
                continue;
            }
            if (!FLAC__stream_decoder_process_single(decoder_) || failed_ || input_error_) return false;
        }
        return false;
    }

    bool Restart() {
        ClearPending();
        metadata_link_ = -1;
        failed_ = input_error_ = seeking_ = seek_eof_ = false;
        if (!indexed_) {
            info_.links.clear();
            info_.duration_ms = -1;
        }
        // reset() preserves libFLAC's Ogg byte/sample index but rewinds parsing.
        if (!FLAC__stream_decoder_reset(decoder_) ||
            !FLAC__stream_decoder_process_until_end_of_metadata(decoder_) ||
            failed_ || input_error_ || metadata_link_ != 0) return false;
        info_.current_link = 0;
        info_.current = info_.links.front();
        return true;
    }

    bool Index() {
        // STREAMINFO may omit total_samples or describe only the first link.
        // Skip frames without reconstructing PCM, retaining libFLAC's frame
        // integrity checks and each link's EOS granule/native sample rate.
        for (;;) {
            const auto state = FLAC__stream_decoder_get_state(decoder_);
            if (state == FLAC__STREAM_DECODER_END_OF_STREAM) break;
            if (state == FLAC__STREAM_DECODER_END_OF_LINK) {
                const auto before = info_.links.size();
                if (!FLAC__stream_decoder_finish_link(decoder_) ||
                    !FLAC__stream_decoder_process_until_end_of_metadata(decoder_) || failed_ || input_error_ ||
                    info_.links.size() != before + 1) return false;
            } else if (!FLAC__stream_decoder_skip_single_frame(decoder_) || failed_ || input_error_) return false;
        }
        FLAC__uint64* lengths = nullptr;
        const int count = FLAC__stream_decoder_get_link_lengths(decoder_, &lengths);
        const std::unique_ptr<FLAC__uint64, decltype(&std::free)> owner(lengths, &std::free);
        if (!lengths || count != static_cast<int>(info_.links.size()) ||
            count != static_cast<int>(encoded_links_.size()) || encoded_link_ >= 0) return false;
        for (int i = 0; i < count; ++i) {
            if (lengths[i] > static_cast<FLAC__uint64>(std::numeric_limits<std::int64_t>::max())) return false;
            auto& link = info_.links[i];
            link.frames = static_cast<std::int64_t>(lengths[i]);
            link.duration_ms = DurationMs(link.frames, link.rate);
            link.serial = static_cast<std::uint32_t>(encoded_links_[i].serial);
            if (link.frames > 0) {
                const auto bitrate = static_cast<long double>(encoded_links_[i].bytes) * 8 * link.rate / link.frames;
                link.bitrate = static_cast<int>(std::clamp(bitrate, 1.0L, static_cast<long double>(INT_MAX)));
            }
        }
        RefreshKnownDuration();
        indexed_ = true;
        counting_bytes_ = false;
        ogg_sync_clear(&bitrate_sync_);
        encoded_links_.clear();
        return Restart();
    }

    bool LinearSeek(int milliseconds) {
        // libFLAC's interpolation seek cannot position some very short Ogg
        // files. Decode/discard from the start, keeping exact integer samples
        // and the complete local link catalogue.
        if (!Restart()) return false;
        long double remaining = milliseconds;
        while (remaining > 0) {
            if (!ProcessNext()) return false;
            if (!pending_frames_) return true;
            const int rate = info_.links[pending_link_].rate;
            const auto skip = static_cast<std::int64_t>(remaining * rate / 1000.0L + 1e-9L);
            const int available = pending_frames_ - pending_offset_;
            if (skip < available) {
                pending_offset_ += static_cast<int>(skip);
                PublishPendingLink();
                return !failed_;
            }
            remaining -= static_cast<long double>(available) * 1000 / rate;
            pending_.clear(); pending_integer_.clear();
            pending_frames_ = pending_offset_ = pending_channels_ = 0;
            pending_link_ = -1;
        }
        return true;
    }

public:
    ~FlacReader() override {
        if (counting_bytes_) ogg_sync_clear(&bitrate_sync_);
        if (decoder_) {
            if (initialized_) FLAC__stream_decoder_finish(decoder_);
            FLAC__stream_decoder_delete(decoder_);
        }
    }

    bool Open(std::shared_ptr<ByteStream> stream, std::wstring& error) {
        if (!stream) {
            error = L"No input stream was supplied.";
            return false;
        }
        stream_ = std::move(stream);
        decoder_ = FLAC__stream_decoder_new();
        if (!decoder_) {
            error = L"Cannot allocate an Ogg FLAC decoder.";
            return false;
        }
        if (!FLAC__stream_decoder_set_decode_chained_stream(decoder_, true) ||
            !FLAC__stream_decoder_set_metadata_respond(decoder_, FLAC__METADATA_TYPE_VORBIS_COMMENT)) {
            error = L"Cannot configure the Ogg FLAC decoder.";
            return false;
        }
        const bool seekable = stream_->Seekable();
        // Observe the existing sequential local scan, including metadata reads.
        // Transport tell offsets alone overcount links because libFLAC reads ahead.
        if (seekable && stream_->Local()) {
            if (ogg_sync_init(&bitrate_sync_)) {
                error = L"Cannot initialize Ogg FLAC bitrate indexing.";
                return false;
            }
            counting_bytes_ = true;
        }
        const auto status = FLAC__stream_decoder_init_ogg_stream(
            decoder_, ReadCallback, seekable ? SeekCallback : nullptr, seekable ? TellCallback : nullptr,
            seekable ? LengthCallback : nullptr, seekable ? EofCallback : nullptr,
            WriteCallback, MetadataCallback, ErrorCallback, this);
        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            error = L"Cannot initialize Ogg FLAC decoder (error " + std::to_wstring(static_cast<int>(status)) + L").";
            return false;
        }
        initialized_ = true;
        if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder_) || failed_ || input_error_ || info_.links.empty()) {
            error = L"Cannot read Ogg FLAC metadata.";
            return false;
        }
        info_.codec = Codec::Flac;
        info_.seekable = seekable;
        info_.radio = stream_->Radio();
        info_.current_link = 0;
        info_.current = info_.links.front();
        if (info_.links.size() == 1) RefreshKnownDuration();
        // HTTP startup must remain bounded, even when Range seeking is enabled.
        if (seekable && stream_->Local() && !Index()) {
            error = L"Cannot index local Ogg FLAC links.";
            return false;
        }
        return true;
    }

    int Read(float* samples, int capacityFrames) override {
        if (!samples || capacityFrames <= 0 || !decoder_) return -1;
        exact_.clear();
        if (seek_eof_) return 0;
        const int available = CopyPending(samples, capacityFrames);
        if (available) return available;
        if (failed_) return -1;
        if (!ProcessNext()) return -1;
        const int decoded = CopyPending(samples, capacityFrames);
        if (decoded) return decoded;
        return FLAC__stream_decoder_get_state(decoder_) == FLAC__STREAM_DECODER_END_OF_STREAM ? 0 : -1;
    }

    bool Seek(int milliseconds) override {
        if (!decoder_ || !info_.seekable || milliseconds < 0) return false;
        // Unindexed HTTP and chains use time-based discard, with each link's
        // own rate. libFLAC's interpolation can report success in the wrong
        // link on very short chains; do not publish that PCM as a valid seek.
        if (!indexed_ || info_.links.size() != 1) return LinearSeek(milliseconds);
        ClearPending();
        input_error_ = failed_ = seek_eof_ = false;
        const auto& link = info_.links.front();
        const auto target = static_cast<FLAC__uint64>(static_cast<long double>(milliseconds) * link.rate / 1000);
        if (target >= static_cast<FLAC__uint64>(link.frames)) {
            seek_eof_ = true;
            return true;
        }
        metadata_link_ = 0;
        seeking_ = true;
        const bool sought = FLAC__stream_decoder_seek_absolute(decoder_, target);
        seeking_ = false;
        if (!sought || failed_ || input_error_) {
            const bool can_retry = !failed_ && !input_error_;
            return can_retry && LinearSeek(milliseconds);
        }
        return true;
    }

    const TrackInfo& Info() const noexcept override { return info_; }
    int InstantBitrate() noexcept override { return std::max(0, info_.current.bitrate); }
    bool WindowsOrder() const noexcept override { return true; }
    const std::int32_t* IntegerPcm() const noexcept override { return exact_.empty() ? nullptr : exact_.data(); }
};
}

std::unique_ptr<CodecReader> OpenFlac(std::shared_ptr<ByteStream> stream, const Settings&, std::wstring& error) {
    auto reader = std::make_unique<FlacReader>();
    if (!reader->Open(std::move(stream), error)) return {};
    return reader;
}
}
