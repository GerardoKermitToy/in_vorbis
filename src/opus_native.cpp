// SPDX-License-Identifier: GPL-3.0-only
#include "opus_native.hpp"
#include "ambisonics.hpp"
#include <ogg/ogg.h>
#include <opus_multistream.h>
#include <opus_projection.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <utility>

namespace ogg_input {
namespace {
constexpr std::size_t MaxPacketBytes = 1024 * 1024;
constexpr std::size_t MaxBatchBytes = MaxPacketBytes + 65025;
constexpr unsigned MaxPages = 4 * 1024 * 1024;
constexpr unsigned MaxPackets = 16 * 1024 * 1024;
constexpr unsigned MaxLinks = 256;
constexpr unsigned MaxSkippedPages = 8192;
constexpr std::size_t MaxMetadataBytes = 8 * 1024 * 1024;
constexpr std::int64_t I64Max = (std::numeric_limits<std::int64_t>::max)();

std::uint32_t Le32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
int Le16(const unsigned char* p) noexcept { return p[0] | (p[1] << 8); }
int Milliseconds(std::int64_t frames48) noexcept {
    return frames48 < 0 ? -1 : static_cast<int>((std::min)(static_cast<std::int64_t>(INT_MAX), frames48 / 48));
}

// Parse only the packet envelope, not CELT/SILK entropy data. All pointer
// movement is checked against the enclosing Ogg packet. Self-delimited sizes
// are necessary for every elementary stream except the last in multistream.
bool FrameSize(const unsigned char*& p, const unsigned char* end, int& size) noexcept {
    if (p == end) return false;
    const int first = *p++;
    if (first < 252) { size = first; return true; }
    if (p == end) return false;
    size = first + 4 * *p++;
    return true;
}
bool ExtensionPayload(const unsigned char*& p, const unsigned char* end,
                      unsigned byte, std::size_t trailing, std::size_t& payload) noexcept {
    const unsigned id = byte >> 1, l = byte & 1;
    payload = 0;
    if ((id == 0 && l) || id == 2) return true;
    if (id > 0 && id < 32) payload = l;
    else if (!l) {
        if (trailing > static_cast<std::size_t>(end - p)) return false;
        payload = static_cast<std::size_t>(end - p) - trailing;
    } else {
        unsigned n = 0;
        do {
            if (p == end) return false;
            n = *p++;
            payload += n;
            if (payload > static_cast<std::size_t>(end - p)) return false;
        } while (n == 255);
    }
    if (payload > static_cast<std::size_t>(end - p)) return false;
    p += payload;
    return true;
}
bool QextPadding(const unsigned char* p, std::size_t bytes, int frames, bool& found) noexcept {
    const unsigned char* end = p + bytes;
    // There can be at most bytes extension headers. Store only the current
    // repetition group as offsets; no allocation in this untrusted parser.
    const unsigned char* group = p;
    const unsigned char* last_long_end = nullptr;
    std::size_t trailing_short = 0;
    int frame = 0;
    while (p < end) {
        const unsigned char* header = p;
        const unsigned byte = *p++, id = byte >> 1, l = byte & 1;
        std::size_t payload = 0;
        if (!ExtensionPayload(p, end, byte, 0, payload)) return false;
        if (id == 1) {
            const int increment = l ? header[1] : 1;
            if (!increment) continue;
            frame += increment;
            if (frame >= frames) return false;
            group = p; last_long_end = nullptr; trailing_short = 0;
        } else if (id == 2) {
            // Repeated long extensions may have different payload lengths.
            // Skip their payloads with the original headers, not by searching
            // arbitrary padding bytes for 0xf8 (which would produce false HD).
            for (int f = frame + 1; f < frames; ++f) {
                const unsigned char* s = group;
                while (s < header) {
                    unsigned repeated = *s++;
                    std::size_t ignored = 0;
                    if (!ExtensionPayload(s, header, repeated, 0, ignored)) return false;
                    if (repeated <= 3) continue;
                    if (!l && f + 1 == frames && s == last_long_end) repeated &= ~1U;
                    if (!ExtensionPayload(p, end, repeated, trailing_short, ignored)) return false;
                }
            }
            group = p; last_long_end = nullptr; trailing_short = 0;
            if (!l && ++frame >= frames) return true;
        } else if (id > 2) {
            if (id >= 32) { last_long_end = p; trailing_short = 0; }
            else trailing_short += l;
            if (id == 124 && payload) found = true; // Pinned libopus 1.6.1 QEXT ID.
        }
    }
    return true;
}
bool ElementaryPacket(const unsigned char* data, std::size_t bytes, bool self_delimited,
                      std::size_t& consumed, int& duration, bool& qext) noexcept {
    if (!bytes || bytes > MaxPacketBytes) return false;
    const unsigned char* p = data;
    const unsigned char* physical_end = data + bytes;
    const unsigned toc = *p++;
    int count = 1;
    bool cbr = false;
    std::size_t pad = 0;
    std::array<int, 48> sizes{};
    if ((toc & 3) == 1) { count = 2; cbr = true; }
    else if ((toc & 3) == 2) {
        count = 2;
        if (!FrameSize(p, physical_end, sizes[0])) return false;
    } else if ((toc & 3) == 3) {
        if (p == physical_end) return false;
        const unsigned flags = *p++;
        count = flags & 63;
        if (count < 1 || count > 48) return false;
        if (flags & 64) {
            unsigned n = 0;
            do {
                if (p == physical_end) return false;
                n = *p++;
                pad += n == 255 ? 254 : n;
                if (pad > static_cast<std::size_t>(physical_end - p)) return false;
            } while (n == 255);
        }
        cbr = !(flags & 128);
        if (!cbr) {
            const unsigned char* header_end = physical_end - pad;
            for (int i = 0; i < count - 1; ++i)
                if (!FrameSize(p, header_end, sizes[i])) return false;
        }
    }
    if (pad > static_cast<std::size_t>(physical_end - p)) return false;
    const unsigned char* end = physical_end - pad;
    if (self_delimited) {
        if (!FrameSize(p, end, sizes[count - 1])) return false;
        if (cbr) std::fill_n(sizes.data(), count, sizes[count - 1]);
    } else {
        const std::size_t available = static_cast<std::size_t>(end - p);
        if (cbr) {
            if (available % count || available / count > 1275) return false;
            std::fill_n(sizes.data(), count, static_cast<int>(available / count));
        } else {
            std::size_t prior = 0;
            for (int i = 0; i < count - 1; ++i) prior += sizes[i];
            if (prior > available || available - prior > 1275) return false;
            sizes[count - 1] = static_cast<int>(available - prior);
        }
    }
    std::size_t payload = 0;
    for (int i = 0; i < count; ++i) {
        if (sizes[i] < 0 || sizes[i] > 1275) return false;
        payload += sizes[i];
    }
    if (payload > static_cast<std::size_t>(end - p)) return false;
    const unsigned char* padding = p + payload;
    consumed = static_cast<std::size_t>(padding - data) + pad;
    if (consumed > bytes || (!self_delimited && consumed != bytes)) return false;
    duration = opus_packet_get_samples_per_frame(data, 48000) * count;
    if (duration < 120 || duration > 5760) return false;
    bool extension = false;
    if (pad && !QextPadding(padding, pad, count, extension)) {
        // Invalid/unknown extension syntax is ignored by ordinary Opus
        // decoders as padding. It must never be mistaken for positive HD.
        extension = false;
    }
    qext = qext || ((toc & 128) && extension);
    return true;
}
}

namespace opus_native {
bool ParseHeader(const unsigned char* data, std::size_t size, Header& result) noexcept {
    try {
        Header h;
        if (!data || size < 19 || size > 65025 || std::memcmp(data, "OpusHead", 8) ||
            data[8] < 1 || data[8] > 15 || !data[9]) return false;
        h.channels = data[9]; h.preskip = Le16(data + 10); h.input_rate = Le32(data + 12);
        const int gain = Le16(data + 16);
        h.gain_q8 = gain >= 32768 ? gain - 65536 : gain;
        h.family = data[18];
        if (h.family == 0) {
            if (h.channels > 2 || size != 19) return false;
            h.streams = 1; h.coupled = h.channels - 1;
            h.mapping = h.channels == 2 ? std::vector<unsigned char>{0, 1} : std::vector<unsigned char>{0};
        } else {
            if (size < 21) return false;
            h.streams = data[19]; h.coupled = data[20];
            if (!h.streams || h.coupled > h.streams || h.streams + h.coupled > 255) return false;
            if (h.family == 1) {
                if (h.channels > MaxChannels) return false;
            } else if (h.family == 2 || h.family == 3) {
                int order = 0, harmonics = 0;
                if (!AmbisonicShape(h.channels, order, harmonics)) return false;
            } else return false;
            if (h.family == 3) {
                const std::size_t matrix_size = static_cast<std::size_t>(h.channels) * (h.streams + h.coupled) * 2;
                if (size != 21 + matrix_size) return false;
                h.matrix.assign(data + 21, data + size);
            } else {
                if (size != 21 + static_cast<std::size_t>(h.channels)) return false;
                h.mapping.assign(data + 21, data + size);
                for (unsigned channel : h.mapping)
                    if (channel != 255 && channel >= static_cast<unsigned>(h.streams + h.coupled)) return false;
            }
        }
        result = std::move(h);
        return true;
    } catch (...) { return false; }
}

bool InspectPacket(const unsigned char* data, std::size_t size, int streams,
                   int& duration_48k, bool& qext) noexcept {
    duration_48k = 0; qext = false;
    if (!data || !size || size > MaxPacketBytes || streams < 1 || streams > 255) return false;
    std::size_t at = 0;
    for (int stream = 0; stream < streams; ++stream) {
        std::size_t used = 0; int duration = 0;
        if (!ElementaryPacket(data + at, size - at, stream + 1 < streams, used, duration, qext) || !used) return false;
        if (stream && duration != duration_48k) return false;
        duration_48k = duration;
        at += used;
        if (at > size) return false;
    }
    return at == size;
}
}

namespace {
struct Packet {
    std::vector<unsigned char> data;
    std::int64_t granule = -1, bos_offset = 0;
    std::uint32_t serial = 0;
    int duration = 0;
    bool bos = false, eos = false;
};

// Strict, bounded Ogg framing. libogg computes CRC, but packet assembly is
// local so an unbounded run of 255 laces cannot grow libogg's internal buffers.
// Unrelated multiplexed streams are skipped without creating decoder states.
class Demux {
    std::shared_ptr<ByteStream> stream_;
    std::deque<Packet> ready_;
    std::vector<unsigned char> partial_;
    std::uint32_t serial_ = 0, sequence_ = 0;
    std::int64_t bos_offset_ = 0;
    unsigned pages_ = 0, packets_ = 0;
    bool active_ = false, first_ = false, bounded_index_ = false;
    bool join_armed_ = false, discarding_join_fragment_ = false;
    std::size_t join_fragment_bytes_ = 0;
    std::wstring error_;
    int Exact(unsigned char* out, int bytes, bool allow_eof = false) {
        int at = 0;
        while (at < bytes) {
            const int n = stream_->Read(out + at, bytes - at);
            if (n < 0) { error_ = L"Ogg input read failed or was cancelled."; return -1; }
            if (!n) {
                if (!at && allow_eof) return 0;
                error_ = L"Truncated Ogg page."; return -1;
            }
            if (n > bytes - at) { error_ = L"Invalid input read size."; return -1; }
            at += n;
        }
        return 1;
    }
    bool Fail(const wchar_t* text) { error_ = text; return false; }
    int Page() {
        std::array<unsigned char, 282> header{};
        const auto offset = stream_->Tell();
        const int read = Exact(header.data(), 27, true);
        if (read <= 0) {
            if (!read && (active_ || !partial_.empty())) { Fail(L"Ogg Opus ended without an EOS page."); return -1; }
            return read;
        }
        if ((bounded_index_ && ++pages_ > MaxPages) || std::memcmp(header.data(), "OggS", 4) || header[4] || (header[5] & ~7)) {
            Fail(L"Invalid Ogg page or page-count limit exceeded."); return -1;
        }
        const int segments = header[26];
        if (Exact(header.data() + 27, segments) < 0) return -1;
        int body_size = 0;
        for (int n = 0; n < segments; ++n) body_size += header[27 + n];
        std::vector<unsigned char> body(static_cast<std::size_t>(body_size));
        if (Exact(body.data(), body_size) < 0) return -1;
        const auto crc = Le32(header.data() + 22);
        ogg_page page{header.data(), 27 + segments, body.data(), body_size};
        ogg_page_checksum_set(&page);
        if (Le32(header.data() + 22) != crc) { Fail(L"Ogg CRC mismatch."); return -1; }
        const auto serial = Le32(header.data() + 14);
        bool selected_bos = false;
        if (!active_) {
            if (!(header[5] & 2) || (header[5] & 1) || body_size < 8 || std::memcmp(body.data(), "OpusHead", 8)) return 2;
            active_ = true; first_ = true; serial_ = serial; sequence_ = 0; bos_offset_ = offset; selected_bos = true;
        }
        if (serial != serial_) return 2;
        const auto sequence = Le32(header.data() + 18);
        const bool initial_audio = join_armed_;
        join_armed_ = false;
        const bool gap = sequence != sequence_;
        if (((header[5] & 2) != 0) != selected_bos || (gap && (!initial_audio || sequence < sequence_))) {
            Fail(L"Ogg Opus page sequence mismatch."); return -1;
        }
        const bool continued = (header[5] & 1) != 0;
        if (gap) {
            // Icecast may replay only the headers before joining the current
            // audio page. Recover once, only immediately after validated tags,
            // never for seekable files or a later damaged page.
            partial_.clear();
            discarding_join_fragment_ = continued;
            join_fragment_bytes_ = 0;
        }
        if (continued != (!partial_.empty() || discarding_join_fragment_)) {
            if (initial_audio && continued && partial_.empty()) {
                discarding_join_fragment_ = true; join_fragment_bytes_ = 0;
            } else { Fail(L"Ogg Opus continuation mismatch."); return -1; }
        }
        sequence_ = sequence + 1;
        const std::uint64_t gp = static_cast<std::uint64_t>(Le32(header.data() + 6)) |
            (static_cast<std::uint64_t>(Le32(header.data() + 10)) << 32);
        if (gp > static_cast<std::uint64_t>(I64Max) && gp != UINT64_MAX) {
            Fail(L"Invalid Ogg granule position."); return -1;
        }
        std::size_t at = 0;
        bool discarded_complete = false;
        for (int n = 0; n < segments; ++n) {
            const std::size_t count = header[27 + n];
            if (discarding_join_fragment_) {
                if (join_fragment_bytes_ > MaxPacketBytes - count) { Fail(L"Initial Ogg join fragment exceeds the packet limit."); return -1; }
                join_fragment_bytes_ += count;
                at += count;
                if (count < 255) { discarding_join_fragment_ = false; join_fragment_bytes_ = 0; discarded_complete = true; }
                continue;
            }
            if (partial_.size() > MaxPacketBytes - count) { Fail(L"Ogg Opus packet exceeds the 1 MiB limit."); return -1; }
            partial_.insert(partial_.end(), body.begin() + at, body.begin() + at + count);
            at += count;
            if (count < 255) {
                if (bounded_index_ && ++packets_ > MaxPackets) { Fail(L"Ogg packet-count limit exceeded."); return -1; }
                Packet packet; packet.data = std::move(partial_); partial_.clear();
                packet.serial = serial_; packet.bos_offset = bos_offset_; packet.bos = first_; first_ = false;
                ready_.push_back(std::move(packet));
            }
        }
        if ((header[5] & 4) && (!partial_.empty() || discarding_join_fragment_)) {
            Fail(L"Truncated packet on Ogg EOS page."); return -1;
        }
        if (ready_.empty()) {
            if ((gp != UINT64_MAX || (header[5] & 4)) && !discarded_complete) {
                Fail(L"Ogg page has no complete packet for its granule/EOS."); return -1;
            }
            if (header[5] & 4) active_ = false;
        } else {
            ready_.back().granule = gp == UINT64_MAX ? -1 : static_cast<std::int64_t>(gp);
            if (header[5] & 4) { ready_.back().eos = true; active_ = false; }
        }
        return 1;
    }
public:
    explicit Demux(std::shared_ptr<ByteStream> stream, bool bounded_index = false)
        : stream_(std::move(stream)), bounded_index_(bounded_index) {}
    const std::wstring& Error() const noexcept { return error_; }
    void ArmInitialAudioJoin() noexcept { join_armed_ = !stream_->Seekable(); }
    int Next(Packet& packet) {
        unsigned skipped = 0;
        while (ready_.empty()) {
            const int result = Page();
            if (result <= 0) return result;
            if (++skipped > MaxSkippedPages) { Fail(L"Too many Ogg pages without an Opus packet."); return -1; }
        }
        packet = std::move(ready_.front()); ready_.pop_front();
        return 1;
    }
};

struct Logical {
    opus_native::Header header;
    LinkInfo info;
    std::int64_t bos_offset = 0, base = -1, cursor = 0, end = -1, frames48 = -1;
    std::int64_t audio_bytes = 0;
    bool ended = false, qext = false;
};
struct Batch {
    std::vector<Packet> packets;
    std::int64_t start = 0, limit = 0, decode_at = 0;
    std::size_t at = 0;
};

bool Comments(const std::vector<unsigned char>& data, LinkInfo& link, std::size_t& metadata) {
    if (data.size() < 16 || std::memcmp(data.data(), "OpusTags", 8)) return false;
    std::size_t at = 8;
    auto length = [&](std::uint32_t& n) {
        if (at > data.size() || data.size() - at < 4) return false;
        n = Le32(data.data() + at); at += 4;
        return static_cast<std::size_t>(n) <= data.size() - at;
    };
    std::uint32_t n = 0;
    if (!length(n) || n > 65536) return false;
    link.vendor = Utf8(reinterpret_cast<const char*>(data.data() + at), static_cast<int>(n));
    metadata += n; at += n;
    if (data.size() - at < 4) return false;
    const auto count = Le32(data.data() + at); at += 4;
    if (count > 2048) return false;
    std::size_t budget = 256 * 1024;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!length(n)) return false;
        if (n <= budget) {
            if (n <= 65536) AddTag(link.tags, reinterpret_cast<const char*>(data.data() + at), static_cast<int>(n));
            budget -= n; metadata += n;
        }
        at += n;
        if (metadata > MaxMetadataBytes) return false;
    }
    // RFC 7845 permits trailing application data in OpusTags.
    return metadata <= MaxMetadataBytes;
}
int Headers(Demux& demux, Logical& logical, std::size_t& metadata, std::wstring& error) {
    Packet head;
    const int result = demux.Next(head);
    if (result <= 0) { if (result < 0) error = demux.Error(); return result; }
    if (!head.bos || head.eos || head.granule != 0 ||
        !opus_native::ParseHeader(head.data.data(), head.data.size(), logical.header)) {
        error = L"Invalid or unsupported OpusHead/mapping family."; return -1;
    }
    logical.bos_offset = head.bos_offset;
    logical.info.serial = head.serial;
    logical.info.header_gain_q8 = logical.header.gain_q8;
    Packet tags;
    if (demux.Next(tags) != 1 || tags.bos || tags.eos || tags.serial != head.serial || tags.granule != 0 ||
        !Comments(tags.data, logical.info, metadata)) {
        error = L"Missing, oversized or malformed OpusTags."; return -1;
    }
    demux.ArmInitialAudioJoin();
    return 1;
}
bool AudioBatch(Demux& demux, Logical& logical, Batch& batch, std::wstring& error) {
    batch = {};
    std::int64_t duration = 0;
    std::size_t bytes = 0;
    std::int64_t granule = -1;
    bool eos = false;
    do {
        Packet packet;
        if (demux.Next(packet) != 1) { error = demux.Error().empty() ? L"Truncated Ogg Opus audio." : demux.Error(); return false; }
        bool qext = false;
        if (packet.bos || packet.serial != logical.info.serial ||
            !opus_native::InspectPacket(packet.data.data(), packet.data.size(), logical.header.streams, packet.duration, qext)) {
            error = L"Invalid Opus audio packet."; return false;
        }
        logical.qext = logical.qext || qext;
        bytes += packet.data.size(); duration += packet.duration;
        if (batch.packets.size() >= 255 || bytes > MaxBatchBytes) { error = L"Ogg Opus audio-page packet limit exceeded."; return false; }
        granule = packet.granule; eos = packet.eos;
        batch.packets.push_back(std::move(packet));
        if (eos && granule < 0) { error = L"Ogg EOS has no granule position."; return false; }
    } while (granule < 0);
    if (logical.base < 0) {
        if (!eos && granule < duration) { error = L"Invalid initial Opus granule position."; return false; }
        logical.base = (std::max)(static_cast<std::int64_t>(0), granule - duration);
        logical.cursor = logical.base;
        if (logical.base > I64Max - logical.header.preskip) { error = L"Opus granule overflow."; return false; }
    }
    if (logical.cursor > I64Max - duration) { error = L"Opus duration overflow."; return false; }
    const auto expected = logical.cursor + duration;
    if ((!eos && granule != expected) || (eos && (granule < logical.cursor || granule > expected ||
        granule < logical.base + logical.header.preskip))) {
        error = L"Discontinuous or invalid Opus granule position."; return false;
    }
    batch.start = batch.decode_at = logical.cursor;
    batch.limit = granule;
    logical.cursor = expected;
    logical.audio_bytes += static_cast<std::int64_t>(bytes);
    if (eos) {
        logical.ended = true; logical.end = granule;
        logical.frames48 = granule - logical.base - logical.header.preskip;
    }
    return true;
}
void Format(Logical& logical, const Settings& settings) {
    logical.info.rate = settings.opus_hd && logical.qext ? 96000 : 48000;
    if (logical.header.family >= 2) {
        switch (settings.ambisonic) {
        case AmbisonicMode::Quad: logical.info.channels = 4; break;
        case AmbisonicMode::Surround51: logical.info.channels = 6; break;
        case AmbisonicMode::Surround71: logical.info.channels = 8; break;
        default: logical.info.channels = 2; break;
        }
    } else logical.info.channels = logical.header.channels;
    auto& note = logical.info.decoder_note;
    note = logical.info.rate == 96000 ? L"Native QEXT 96000 Hz" : logical.qext ? L"QEXT, native 48000 Hz (HD disabled)" : L"Native Opus 48000 Hz";
    if (logical.header.family >= 2) {
        int order = 0, harmonics = 0;
        AmbisonicShape(logical.header.channels, order, harmonics);
        note += L"; ambisonics family " + std::to_wstring(logical.header.family) + L", order " + std::to_wstring(order);
        if (logical.header.family == 3 && logical.header.streams + logical.header.coupled != logical.header.channels)
            note += L", rectangular Q15 demix";
        switch (settings.ambisonic) {
        case AmbisonicMode::Headphones: note += L", analytic approximate binaural (not measured HRTF)"; break;
        case AmbisonicMode::Quad: note += L", Windows quad"; break;
        case AmbisonicMode::Surround51: note += L", Windows 5.1, silent LFE"; break;
        case AmbisonicMode::Surround71: note += L", Windows 7.1, silent LFE"; break;
        default: note += L", stereo"; break;
        }
    }
    note += settings.opus_speech ? L"; optional OSCE speech (complexity 7, eligible SILK)" : L"; OSCE speech off";
    if (settings.opus_bwe) note += logical.info.rate == 48000 ? L"; optional OSCE BWE (eligible 48k wideband speech)" : L"; OSCE BWE inapplicable at 96k";
    else note += L"; OSCE BWE off";
    if (logical.frames48 >= 0) {
        logical.info.frames = logical.frames48 <= I64Max / 2 ? logical.frames48 * (logical.info.rate / 48000) : -1;
        logical.info.duration_ms = Milliseconds(logical.frames48);
    }
    const auto measured48 = logical.frames48 > 0 ? logical.frames48 : logical.base >= 0 ? logical.cursor - logical.base : 0;
    if (measured48 > 0) {
        const long double bitrate = static_cast<long double>(logical.audio_bytes) * 8 * 48000 / measured48;
        logical.info.bitrate = static_cast<int>((std::min)(static_cast<long double>(INT_MAX), bitrate));
    }
}

class NativeReader final : public CodecReader {
    std::shared_ptr<ByteStream> stream_;
    Settings settings_;
    TrackInfo info_;
    std::unique_ptr<Demux> demux_;
    std::vector<Logical> known_;
    Logical logical_;
    Batch batch_;
    OpusMSDecoder* decoder_ = nullptr;
    OpusProjectionDecoder* projection_ = nullptr;
    AmbisonicRenderer renderer_;
    std::vector<float> decoded_, projected_, rendered_, pending_;
    std::size_t pending_at_ = 0;
    std::size_t metadata_ = 0;
    std::int64_t skip_ = 0, bitrate_bytes_ = 0, bitrate_frames48_ = 0;
    int link_ = -1, rate_ = 48000, channels_ = 0, decode_channels_ = 0, instant_bitrate_ = 0;
    bool rectangular_projection_ = false;
    bool active_ = false, failed_ = false, eof_ = false, scanned_ = false, windows_ = false;
    std::wstring error_;

    void DestroyDecoder() noexcept {
        if (decoder_) opus_multistream_decoder_destroy(decoder_);
        if (projection_) opus_projection_decoder_destroy(projection_);
        decoder_ = nullptr; projection_ = nullptr;
    }
    bool Decoder() {
        DestroyDecoder();
        int error = OPUS_OK;
        auto& h = logical_.header;
        rectangular_projection_ = h.family == 3 && h.streams + h.coupled != h.channels;
        decode_channels_ = rectangular_projection_ ? h.streams + h.coupled : h.channels;
        if (h.family == 3 && !rectangular_projection_) {
            projection_ = opus_projection_decoder_create(rate_, h.channels, h.streams, h.coupled,
                h.matrix.data(), static_cast<opus_int32>(h.matrix.size()), &error);
        } else {
            std::array<unsigned char, 255> identity{};
            for (int channel = 0; channel < decode_channels_; ++channel) identity[channel] = static_cast<unsigned char>(channel);
            decoder_ = opus_multistream_decoder_create(rate_, decode_channels_, h.streams, h.coupled,
                rectangular_projection_ ? identity.data() : h.mapping.data(), &error);
        }
        if ((!decoder_ && !projection_) || error != OPUS_OK) {
            error_ = rate_ == 96000 ? L"This libopus cannot decode QEXT at 96000 Hz." : L"Cannot create the Opus decoder.";
            return false;
        }
#ifndef OGG_OPUS_HAVE_OSCE
        if (settings_.opus_speech) { error_ = L"Opus speech enhancement was not built into this decoder."; return false; }
#endif
#ifndef OGG_OPUS_HAVE_OSCE_BWE
        if (settings_.opus_bwe) { error_ = L"Opus blind bandwidth extension was not built into this decoder."; return false; }
#endif
        const bool effective_bwe = settings_.opus_bwe && rate_ == 48000;
        const int complexity = settings_.opus_speech ? 7 : effective_bwe ? 4 : 0;
        // BWE is NOT forwarded by libopus 1.6.1's multistream ctl switch.
        // Projection supports GET_DECODER_STATE too: configure every elementary
        // decoder explicitly, with gain zero (the host's gain stage owns gain).
        for (int stream = 0; stream < h.streams; ++stream) {
            OpusDecoder* state = nullptr;
            const int result = projection_ ? opus_projection_decoder_ctl(projection_, OPUS_MULTISTREAM_GET_DECODER_STATE(stream, &state)) :
                opus_multistream_decoder_ctl(decoder_, OPUS_MULTISTREAM_GET_DECODER_STATE(stream, &state));
            if (result != OPUS_OK || !state || opus_decoder_ctl(state, OPUS_SET_GAIN(0)) != OPUS_OK ||
                opus_decoder_ctl(state, OPUS_SET_COMPLEXITY(complexity)) != OPUS_OK) {
                error_ = L"Cannot configure Opus decoder complexity."; return false;
            }
            const int bwe = opus_decoder_ctl(state, OPUS_SET_OSCE_BWE(effective_bwe ? 1 : 0));
            if (bwe != OPUS_OK && (effective_bwe || bwe != OPUS_UNIMPLEMENTED)) {
                error_ = L"Cannot configure Opus blind bandwidth extension."; return false;
            }
        }
        if (h.family >= 2 && !renderer_.Init(h.channels, rate_, settings_.ambisonic)) {
            error_ = L"Cannot initialize the ACN/SN3D renderer."; return false;
        }
        return true;
    }
    void Duration() {
        if (!scanned_ && !eof_) { info_.duration_ms = -1; return; }
        std::int64_t total = 0;
        for (const auto& link : known_) {
            if (link.frames48 < 0 || total > I64Max - link.frames48) { info_.duration_ms = -1; return; }
            total += link.frames48;
        }
        info_.duration_ms = Milliseconds(total);
    }
    bool Scan() {
        Demux scan(stream_, true);
        std::size_t metadata = 0;
        for (;;) {
            Logical link;
            const int n = Headers(scan, link, metadata, error_);
            if (n < 0) return false;
            if (!n) break;
            if (known_.size() >= MaxLinks) { error_ = L"Too many chained Opus links."; return false; }
            Batch batch;
            do { if (!AudioBatch(scan, link, batch, error_)) return false; } while (!link.ended);
            Format(link, settings_);
            known_.push_back(std::move(link));
        }
        if (known_.empty()) { error_ = L"No Ogg Opus logical stream."; return false; }
        scanned_ = true;
        for (const auto& link : known_) info_.links.push_back(link.info);
        Duration();
        return true;
    }
    void Publish() {
        const auto& source = known_[link_].info;
        if (info_.current_link != link_ || info_.current.rate != source.rate || info_.current.channels != source.channels ||
            info_.current.frames != source.frames || info_.current.bitrate != source.bitrate || !info_.current.rate) {
            info_.current_link = link_; info_.current = source;
        }
        windows_ = known_[link_].header.family >= 2;
    }
    int StartLink() {
        logical_ = {};
        const int result = Headers(*demux_, logical_, metadata_, error_);
        if (result <= 0) { if (!result) { eof_ = true; Duration(); } return result; }
        ++link_;
        if (link_ >= static_cast<int>(MaxLinks)) { error_ = L"Too many chained Opus links."; return -1; }
        if (!AudioBatch(*demux_, logical_, batch_, error_)) return -1;
        if (scanned_) {
            if (link_ >= static_cast<int>(known_.size()) || logical_.bos_offset != known_[link_].bos_offset ||
                logical_.info.serial != known_[link_].info.serial || logical_.header.channels != known_[link_].header.channels ||
                logical_.header.family != known_[link_].header.family) {
                error_ = L"Ogg Opus changed since its link index was read."; return -1;
            }
            rate_ = known_[link_].info.rate;
        } else {
            Format(logical_, settings_);
            known_.push_back(logical_); info_.links.push_back(logical_.info);
            rate_ = logical_.info.rate;
        }
        channels_ = known_[link_].info.channels;
        if (!Decoder()) return -1;
        active_ = true;
        return 1;
    }
    bool FinishMetadata() {
        if (!scanned_) {
            Format(logical_, settings_);
            known_[link_] = logical_; info_.links[link_] = logical_.info;
        }
        return true;
    }
    int Fill() {
        for (;;) {
            if (!active_) {
                const int result = StartLink();
                if (result <= 0) return result;
            }
            if (batch_.at == batch_.packets.size()) {
                if (logical_.ended) { FinishMetadata(); active_ = false; continue; }
                if (!AudioBatch(*demux_, logical_, batch_, error_)) return -1;
                // Forward-only sources cannot be rewound to retroactively
                // decode earlier audio at 96k. Fail rather than silently lose
                // late QEXT or mislabel a 48k decode as HD. Seekable files are
                // scanned completely and have no first-audio-page limitation.
                if (!scanned_ && settings_.opus_hd && logical_.qext && rate_ != 96000) {
                    error_ = L"QEXT first appeared after the initial audio page on a forward-only stream."; return -1;
                }
                FinishMetadata();
            }
            Packet& packet = batch_.packets[batch_.at++];
            const int ratio = rate_ / 48000;
            const int capacity = packet.duration * ratio;
            decoded_.resize(static_cast<std::size_t>(capacity) * decode_channels_);
            const int frames = projection_ ? opus_projection_decode_float(projection_, packet.data.data(),
                static_cast<opus_int32>(packet.data.size()), decoded_.data(), capacity, 0) :
                opus_multistream_decode_float(decoder_, packet.data.data(), static_cast<opus_int32>(packet.data.size()), decoded_.data(), capacity, 0);
            if (frames != capacity) { error_ = L"Opus packet decode failed or returned the wrong duration."; return -1; }
            if (std::any_of(decoded_.begin(), decoded_.end(), [](float value) { return !std::isfinite(value); })) {
                error_ = L"Opus decoder returned non-finite PCM."; return -1;
            }
            if (bitrate_frames48_ >= 48000) { bitrate_bytes_ = 0; bitrate_frames48_ = 0; }
            bitrate_bytes_ += static_cast<std::int64_t>(packet.data.size());
            bitrate_frames48_ += packet.duration;
            const float* source = decoded_.data();
            if (rectangular_projection_) {
                // libopus 1.6.1 projection's internal identity map uses C,
                // which can omit coded columns when K>C. Decode K explicitly
                // and apply the RFC8486 CxK signed-Q15 column-major matrix.
                const int rows = logical_.header.channels;
                projected_.assign(static_cast<std::size_t>(frames) * rows, 0.f);
                for (int column = 0; column < decode_channels_; ++column) {
                    for (int row = 0; row < rows; ++row) {
                        const auto at = static_cast<std::size_t>(column * rows + row) * 2;
                        const int word = Le16(logical_.header.matrix.data() + at);
                        const float weight = (word >= 32768 ? word - 65536 : word) / 32768.f;
                        if (!weight) continue;
                        for (int frame = 0; frame < frames; ++frame)
                            projected_[static_cast<std::size_t>(frame) * rows + row] +=
                                weight * decoded_[static_cast<std::size_t>(frame) * decode_channels_ + column];
                    }
                }
                source = projected_.data();
            }
            if (logical_.header.family >= 2) {
                rendered_.resize(static_cast<std::size_t>(frames) * channels_);
                renderer_.Render(source, rendered_.data(), frames);
                source = rendered_.data();
            }
            const auto begin = batch_.decode_at;
            const auto end = begin + packet.duration;
            batch_.decode_at = end;
            const auto first = (std::max)(begin, logical_.base + logical_.header.preskip);
            const auto last = (std::min)(end, batch_.limit);
            if (last <= first) continue;
            const auto offset = static_cast<std::size_t>((first - begin) * ratio) * channels_;
            const auto samples = static_cast<std::size_t>((last - first) * ratio) * channels_;
            pending_.assign(source + offset, source + offset + samples);
            pending_at_ = 0;
            if (skip_) {
                const auto discard = (std::min)(skip_, static_cast<std::int64_t>(samples / channels_));
                pending_at_ = static_cast<std::size_t>(discard) * channels_;
                skip_ -= discard;
                if (pending_at_ == pending_.size()) { pending_.clear(); pending_at_ = 0; continue; }
            }
            return 1;
        }
    }
public:
    explicit NativeReader(const Settings& settings) : settings_(Validate(settings)) {}
    ~NativeReader() override { DestroyDecoder(); }
    bool Open(std::shared_ptr<ByteStream> stream, std::wstring& error) {
        stream_ = std::move(stream);
        info_.codec = Codec::Opus; info_.radio = stream_->Radio(); info_.seekable = stream_->Seekable();
        const auto beginning = stream_->Tell();
        if (info_.seekable) {
            if (!Scan() || !stream_->Seek(beginning, SEEK_SET)) {
                error = error_.empty() ? L"Cannot rewind indexed Ogg Opus." : error_; return false;
            }
        }
        demux_ = std::make_unique<Demux>(stream_);
        if (StartLink() != 1) { error = error_.empty() ? L"No Ogg Opus audio." : error_; return false; }
        Publish();
        return true;
    }
    int Read(float* output, int frames) override {
        if (!output || frames < 1 || frames > BlockFrames || failed_) return -1;
        try {
            if (pending_at_ >= pending_.size()) {
                pending_.clear(); pending_at_ = 0;
                if (eof_) return 0;
                const int result = Fill();
                if (result <= 0) { if (result < 0) failed_ = true; return result; }
            }
            Publish();
            const int count = (std::min)(frames, static_cast<int>((pending_.size() - pending_at_) / channels_));
            std::copy_n(pending_.data() + pending_at_, static_cast<std::size_t>(count) * channels_, output);
            pending_at_ += static_cast<std::size_t>(count) * channels_;
            return count;
        } catch (...) { failed_ = true; return -1; }
    }
    bool Seek(int milliseconds) override {
        if (!info_.seekable || !scanned_ || milliseconds < 0) return false;
        try {
            std::int64_t target = static_cast<std::int64_t>(milliseconds) * 48;
            std::size_t index = 0;
            for (; index + 1 < known_.size(); ++index) {
                if (target < known_[index].frames48) break;
                target -= known_[index].frames48;
            }
            target = (std::min)(target, known_[index].frames48);
            if (!stream_->Seek(known_[index].bos_offset, SEEK_SET)) return false;
            demux_ = std::make_unique<Demux>(stream_);
            pending_.clear(); pending_at_ = 0; skip_ = target * (known_[index].info.rate / 48000);
            bitrate_bytes_ = bitrate_frames48_ = 0; instant_bitrate_ = 0;
            link_ = static_cast<int>(index) - 1;
            metadata_ = 0; active_ = false; failed_ = false; eof_ = false;
            if (StartLink() != 1) { failed_ = true; return false; }
            // Decode from the link BOS, including pre-skip, for exact CELT/SILK
            // and analytic binaural history. Linear seeks favor correctness
            // over a coarse granule-based seek with insufficient preroll.
            while (skip_) {
                const int n = Fill();
                if (n < 0 || (!n && skip_)) { failed_ = true; return false; }
            }
            Publish();
            return true;
        } catch (...) { failed_ = true; return false; }
    }
    const TrackInfo& Info() const noexcept override { return info_; }
    int InstantBitrate() noexcept override {
        if (bitrate_frames48_ > 0) {
            instant_bitrate_ = static_cast<int>((std::min)(static_cast<std::int64_t>(INT_MAX),
                bitrate_bytes_ * 8 * 48000 / bitrate_frames48_));
            bitrate_bytes_ = bitrate_frames48_ = 0;
        }
        return instant_bitrate_ ? instant_bitrate_ : info_.current.bitrate;
    }
    bool WindowsOrder() const noexcept override { return windows_; }
};
}

std::unique_ptr<CodecReader> OpenNativeOpus(std::shared_ptr<ByteStream> stream, const Settings& settings, std::wstring& error) {
    try {
        auto reader = std::make_unique<NativeReader>(settings);
        if (!reader->Open(std::move(stream), error)) return {};
        return reader;
    } catch (...) { error = L"Cannot allocate or initialize the native Ogg Opus decoder."; return {}; }
}
}
