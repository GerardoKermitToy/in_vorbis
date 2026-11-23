#include "fixtures.hpp"
#include "backend.hpp"
#include <ogg/ogg.h>
#include <opus_multistream.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
void Le(std::vector<unsigned char>& data, unsigned value, int bytes) {
    for (int i = 0; i < bytes; ++i) data.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
struct OggStream {
    ogg_stream_state value{};
    explicit OggStream(int serial) { Check(ogg_stream_init(&value, serial) == 0, "ogg_stream_init"); }
    ~OggStream() { ogg_stream_clear(&value); }
};
void Packet(OggStream& stream, std::ofstream& output, unsigned char* data,
            long bytes, bool first, bool last, ogg_int64_t granule, ogg_int64_t number) {
    ogg_packet packet{data, bytes, first ? 1L : 0L, last ? 1L : 0L, granule, number};
    Check(ogg_stream_packetin(&stream.value, &packet) == 0, "ogg_stream_packetin");
    ogg_page page{};
    while (ogg_stream_flush(&stream.value, &page)) {
        output.write(reinterpret_cast<char*>(page.header), page.header_len);
        output.write(reinterpret_cast<char*>(page.body), page.body_len);
    }
    Check(output.good(), "write Ogg page");
}
} // namespace

void MakeOpus(const std::filesystem::path& path, int channels, int frames, int serial,
    const std::vector<std::string>& extra_tags, int header_gain_q8) {
    constexpr int rate = 48000;
    const int family = channels > 2 ? 1 : 0;
    int streams = 0, coupled = 0, error = 0;
    std::array<unsigned char, 8> mapping{};
    std::unique_ptr<OpusMSEncoder, decltype(&opus_multistream_encoder_destroy)> encoder(
        opus_multistream_surround_encoder_create(rate, channels, family, &streams,
            &coupled, mapping.data(), OPUS_APPLICATION_AUDIO, &error), &opus_multistream_encoder_destroy);
    Check(encoder && error == OPUS_OK, "create encoder");
    int preskip = 0;
    Check(opus_multistream_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&preskip)) == OPUS_OK, "lookahead");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Check(output.good(), "create fixture");
    OggStream stream(serial);
    std::vector<unsigned char> head{'O','p','u','s','H','e','a','d',1,static_cast<unsigned char>(channels)};
    Le(head, preskip, 2);
    Le(head, 44100, 4); // Informational source rate; Opus output is still 48000 Hz.
    Le(head, static_cast<unsigned>(header_gain_q8), 2);
    head.push_back(static_cast<unsigned char>(family));
    if (family) {
        head.push_back(static_cast<unsigned char>(streams));
        head.push_back(static_cast<unsigned char>(coupled));
        head.insert(head.end(), mapping.begin(), mapping.begin() + channels);
    }
    Packet(stream, output, head.data(), static_cast<long>(head.size()), true, false, 0, 0);
    std::vector<unsigned char> tags{'O','p','u','s','T','a','g','s'};
    const std::string vendor = "Winamp Opus tests";
    Le(tags, static_cast<unsigned>(vendor.size()), 4);
    tags.insert(tags.end(), vendor.begin(), vendor.end());
    std::vector<std::string> comments{"TITLE=Проверка Opus", "ARTIST=Тест", "ALBUM=Test album"};
    comments.insert(comments.end(), extra_tags.begin(), extra_tags.end());
    Le(tags, static_cast<unsigned>(comments.size()), 4);
    for (const auto& comment : comments) {
        Le(tags, static_cast<unsigned>(comment.size()), 4);
        tags.insert(tags.end(), comment.begin(), comment.end());
    }
    Packet(stream, output, tags.data(), static_cast<long>(tags.size()), false, false, 0, 1);
    constexpr int block = 960;
    const int packets = (frames + preskip + block - 1) / block;
    std::vector<opus_int16> pcm(static_cast<std::size_t>(block) * channels);
    std::array<unsigned char, 16384> encoded{};
    for (int packet = 0; packet < packets; ++packet) {
        for (int i = 0; i < block; ++i) {
            const int sample = packet * block + i;
            for (int channel = 0; channel < channels; ++channel) {
                const double wave = sample < frames ? std::sin(sample * 6.283185307179586 * (220 + channel * 110) / rate) : 0;
                pcm[static_cast<std::size_t>(i) * channels + channel] = static_cast<opus_int16>(wave * 8000);
            }
        }
        const int bytes = opus_multistream_encode(encoder.get(), pcm.data(), block,
            encoded.data(), static_cast<opus_int32>(encoded.size()));
        Check(bytes > 0, "encode packet");
        const bool last = packet + 1 == packets;
        const ogg_int64_t granule = last ? frames + preskip : (packet + 1) * block;
        Packet(stream, output, encoded.data(), bytes, false, last, granule, packet + 2);
    }
}
void MakeVorbisHeaders(const std::filesystem::path& path, int serial) {
    // Real Ogg framing/CRC and a Vorbis identification packet; no Vorbis encoder
    // is needed to prove the Opus content probe rejects this codec's headers.
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Check(output.good(), "create Vorbis header fixture");
    OggStream stream(serial);
    std::vector<unsigned char> identification{1,'v','o','r','b','i','s'};
    Le(identification, 0, 4);
    identification.push_back(2);
    Le(identification, 48000, 4);
    Le(identification, 0, 4);
    Le(identification, 128000, 4);
    Le(identification, 0, 4);
    identification.push_back(0xB8);
    identification.push_back(1);
    Packet(stream, output, identification.data(), static_cast<long>(identification.size()), true, false, 0, 0);
    std::vector<unsigned char> comments{3,'v','o','r','b','i','s'};
    const std::string vendor = "Contains OpusHead text, but this is Vorbis";
    Le(comments, static_cast<unsigned>(vendor.size()), 4);
    comments.insert(comments.end(), vendor.begin(), vendor.end());
    Le(comments, 0, 4);
    comments.push_back(1);
    Packet(stream, output, comments.data(), static_cast<long>(comments.size()), false, false, 0, 1);
}

void AppendFile(const std::filesystem::path& destination, const std::filesystem::path& source) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::app);
    output << input.rdbuf();
    Check(input.good() && output.good(), "append chained stream");
}
