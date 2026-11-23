// SPDX-License-Identifier: GPL-3.0-only
// Self-contained generated fixtures; no checked-in media or external tools.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "backend.hpp"
#include "opus_native.hpp"
#include "ambisonics.hpp"
#include <ogg/ogg.h>
#include <opus_multistream.h>
#include <opus_projection.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ogg_input;
namespace {
constexpr double Pi = 3.14159265358979323846;
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void Le(std::vector<unsigned char>& data, unsigned value, int bytes) {
    for (int i = 0; i < bytes; ++i) data.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
struct OggWriter {
    ogg_stream_state stream{};
    std::ofstream file;
    std::int64_t number = 0;
    OggWriter(const std::filesystem::path& path, int serial) : file(path, std::ios::binary | std::ios::trunc) {
        Check(file.good() && ogg_stream_init(&stream, serial) == 0, "initialize fixture Ogg writer");
    }
    ~OggWriter() { ogg_stream_clear(&stream); }
    void Packet(std::vector<unsigned char>& data, std::int64_t granule, bool first, bool last, bool flush = true) {
        ogg_packet packet{data.data(), static_cast<long>(data.size()), first ? 1L : 0L, last ? 1L : 0L, granule, number++};
        Check(ogg_stream_packetin(&stream, &packet) == 0, "queue fixture packet");
        ogg_page page{};
        while (flush ? ogg_stream_flush(&stream, &page) : ogg_stream_pageout(&stream, &page)) {
            file.write(reinterpret_cast<char*>(page.header), page.header_len);
            file.write(reinterpret_cast<char*>(page.body), page.body_len);
        }
        Check(file.good(), "write fixture Ogg page");
    }
};
struct Options {
    int family = 0, channels = 2, frames48 = 7013, serial = 34001;
    int hint = 48000, group = 1, extra_packets = 0, late_qext_packets = 0;
    int projection_rows = 0;
    std::int64_t granule_offset = 0;
    bool hd = false, speech = false;
};
struct Encoder {
    OpusMSEncoder* multistream = nullptr;
    OpusProjectionEncoder* projection = nullptr;
    int streams = 0, coupled = 0;
    std::vector<unsigned char> mapping;
    ~Encoder() {
        if (multistream) opus_multistream_encoder_destroy(multistream);
        if (projection) opus_projection_encoder_destroy(projection);
    }
    void Qext(bool enabled) {
        for (int i = 0; i < streams; ++i) {
            OpusEncoder* state = nullptr;
            const int n = projection ? opus_projection_encoder_ctl(projection, OPUS_MULTISTREAM_GET_ENCODER_STATE(i, &state)) :
                opus_multistream_encoder_ctl(multistream, OPUS_MULTISTREAM_GET_ENCODER_STATE(i, &state));
            Check(n == OPUS_OK && state && opus_encoder_ctl(state, OPUS_SET_QEXT(enabled ? 1 : 0)) == OPUS_OK, "set encoder QEXT");
        }
    }
    void Bitrate(int value) {
        const int n = projection ? opus_projection_encoder_ctl(projection, OPUS_SET_BITRATE(value)) :
            opus_multistream_encoder_ctl(multistream, OPUS_SET_BITRATE(value));
        Check(n == OPUS_OK, "set fixture bitrate");
    }
};
std::vector<unsigned char> TagsPacket() {
    std::vector<unsigned char> tags{'O','p','u','s','T','a','g','s'};
    const std::string vendor = "Independent advanced Opus fixtures";
    Le(tags, static_cast<unsigned>(vendor.size()), 4); tags.insert(tags.end(), vendor.begin(), vendor.end());
    Le(tags, 2, 4);
    for (const std::string text : {"TITLE=Advanced Opus", "R128_TRACK_GAIN=256"}) {
        Le(tags, static_cast<unsigned>(text.size()), 4); tags.insert(tags.end(), text.begin(), text.end());
    }
    return tags;
}
void Fixture(const std::filesystem::path& path, Options options) {
    const int rate = options.hd ? 96000 : options.speech ? 16000 : 48000;
    const int block = rate / 50;
    Encoder encoder;
    encoder.mapping.resize(options.channels);
    int error = OPUS_OK;
    const int application = options.speech ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO;
    if (options.family == 3 && !options.projection_rows) {
        encoder.projection = opus_projection_ambisonics_encoder_create(rate, options.channels, 3,
            &encoder.streams, &encoder.coupled, application, &error);
    } else {
        encoder.multistream = opus_multistream_surround_encoder_create(rate, options.channels, options.projection_rows ? 255 : options.family,
            &encoder.streams, &encoder.coupled, encoder.mapping.data(), application, &error);
    }
    Check((encoder.multistream || encoder.projection) && error == OPUS_OK, "create advanced fixture encoder (QEXT build required for96k)");
    int lookahead = 0;
    Check((encoder.projection ? opus_projection_encoder_ctl(encoder.projection, OPUS_GET_LOOKAHEAD(&lookahead)) :
        opus_multistream_encoder_ctl(encoder.multistream, OPUS_GET_LOOKAHEAD(&lookahead))) == OPUS_OK, "fixture lookahead");
    Check((lookahead * 48000) % rate == 0, "fixture lookahead uses48k granule units");
    const int preskip = lookahead * 48000 / rate;
    if (options.hd) { encoder.Qext(true); encoder.Bitrate(options.channels * 640000); }
    else encoder.Bitrate(options.speech ? 12000 : options.channels * 64000);
    if (options.speech) {
        Check(opus_multistream_encoder_ctl(encoder.multistream, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND)) == OPUS_OK, "wideband speech limit");
        Check(opus_multistream_encoder_ctl(encoder.multistream, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) == OPUS_OK, "speech signal type");
        Check(opus_multistream_encoder_ctl(encoder.multistream, OPUS_SET_VBR(0)) == OPUS_OK, "speech CBR fixture");
    }
    int matrix_gain = 0;
    std::vector<unsigned char> matrix;
    if (encoder.projection) {
        int size = 0;
        Check(opus_projection_encoder_ctl(encoder.projection, OPUS_PROJECTION_GET_DEMIXING_MATRIX_SIZE(&size)) == OPUS_OK && size > 0, "projection matrix size");
        matrix.resize(size);
        Check(opus_projection_encoder_ctl(encoder.projection, OPUS_PROJECTION_GET_DEMIXING_MATRIX(matrix.data(), size)) == OPUS_OK, "projection matrix bytes");
        Check(opus_projection_encoder_ctl(encoder.projection, OPUS_PROJECTION_GET_DEMIXING_MATRIX_GAIN(&matrix_gain)) == OPUS_OK, "projection matrix gain");
    }
    const int header_channels = options.projection_rows ? options.projection_rows : options.channels;
    if (options.projection_rows) {
        // Route only the LAST coded channel into W. This detects an internal
        // projection implementation that silently drops columns for K>C.
        for (int column = 0; column < options.channels; ++column)
            for (int row = 0; row < header_channels; ++row)
                Le(matrix, column + 1 == options.channels && row == 0 ? 32767 : 0, 2);
    }
    std::vector<unsigned char> head{'O','p','u','s','H','e','a','d',1,static_cast<unsigned char>(header_channels)};
    Le(head, preskip, 2); Le(head, options.hint, 4); Le(head, static_cast<unsigned>(matrix_gain), 2);
    head.push_back(static_cast<unsigned char>(options.family));
    if (options.family) {
        head.push_back(static_cast<unsigned char>(encoder.streams)); head.push_back(static_cast<unsigned char>(encoder.coupled));
        if (options.family == 3) head.insert(head.end(), matrix.begin(), matrix.end());
        else head.insert(head.end(), encoder.mapping.begin(), encoder.mapping.end());
    }
    OggWriter writer(path, options.serial);
    writer.Packet(head, 0, true, false);
    auto tags = TagsPacket(); writer.Packet(tags, 0, false, false);
    const int packets = (options.frames48 + preskip + 959) / 960 + options.extra_packets;
    std::vector<float> pcm(static_cast<std::size_t>(block) * options.channels);
    std::vector<unsigned char> data(512 * 1024);
    for (int packet = 0; packet < packets; ++packet) {
        if (options.hd && options.late_qext_packets) encoder.Qext(packet >= options.late_qext_packets);
        for (int i = 0; i < block; ++i) {
            const auto sample = static_cast<std::int64_t>(packet) * block + i;
            const double t = static_cast<double>(sample) / rate;
            const bool audible = sample * 48000 < static_cast<std::int64_t>(options.frames48) * rate;
            for (int c = 0; c < options.channels; ++c) {
                double value = .16 * std::sin(2 * Pi * (300 + 137 * c) * t);
                if (options.hd) value = .12 * std::sin(2 * Pi * 1000 * t) + .22 * std::sin(2 * Pi * 30000 * t);
                if (options.speech) {
                    const double phase = 2 * Pi * (135 * t + .7 * std::sin(2 * Pi * 1.7 * t));
                    value = .19 * (std::sin(phase) + .45 * std::sin(2 * phase) + .24 * std::sin(3 * phase) + .12 * std::sin(5 * phase));
                    value *= .65 + .35 * std::sin(2 * Pi * 3.2 * t);
                }
                pcm[static_cast<std::size_t>(i) * options.channels + c] = audible ? static_cast<float>(value) : 0.f;
            }
        }
        const int bytes = encoder.projection ? opus_projection_encode_float(encoder.projection, pcm.data(), block, data.data(), static_cast<opus_int32>(data.size())) :
            opus_multistream_encode_float(encoder.multistream, pcm.data(), block, data.data(), static_cast<opus_int32>(data.size()));
        Check(bytes > 0, "encode advanced fixture packet");
        std::vector<unsigned char> encoded(data.begin(), data.begin() + bytes);
        const bool last = packet + 1 == packets;
        const auto granule = options.granule_offset + (last ? options.frames48 + preskip : (packet + 1) * 960);
        writer.Packet(encoded, granule, false, last, last || (packet + 1) % options.group == 0);
    }
}
std::vector<unsigned char> Bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Save(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); Check(file.good(), "save modified fixture");
}
void Append(const std::filesystem::path& destination, const std::filesystem::path& source) {
    auto bytes = Bytes(source); std::ofstream file(destination, std::ios::binary | std::ios::app);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); Check(file.good(), "append advanced chain");
}
std::size_t PageSize(const std::vector<unsigned char>& bytes, std::size_t at) {
    Check(at + 27 <= bytes.size() && !std::memcmp(bytes.data() + at, "OggS", 4), "fixture page boundary");
    const int segments = bytes[at + 26]; Check(at + 27 + segments <= bytes.size(), "fixture lacing boundary");
    std::size_t count = 27 + segments;
    for (int i = 0; i < segments; ++i) count += bytes[at + 27 + i];
    Check(at + count <= bytes.size(), "fixture page extent"); return count;
}
void Crc(std::vector<unsigned char>& bytes, std::size_t at) {
    const auto size = PageSize(bytes, at);
    const long header = 27 + bytes[at + 26];
    ogg_page page{bytes.data() + at, header, bytes.data() + at + header, static_cast<long>(size - header)};
    ogg_page_checksum_set(&page);
}
// One bounded loopback-only HTTP source, without ranges or Content-Length:
// ByteStream must expose it as forward-only even though the fixture is finite.
class LiveSource {
    SOCKET socket_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::atomic<bool> stop_{false};
    std::vector<unsigned char> body_;
    std::thread worker_;
    static bool Send(SOCKET socket, const char* data, int count) noexcept {
        while (count > 0) {
            const int n = send(socket, data, count, 0);
            if (n <= 0) return false;
            data += n; count -= n;
        }
        return true;
    }
    void Run() noexcept {
        while (!stop_) {
            fd_set ready; FD_ZERO(&ready); FD_SET(socket_, &ready);
            timeval wait{0, 100000};
            if (select(0, &ready, nullptr, nullptr, &wait) <= 0) continue;
            const SOCKET client = accept(socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            DWORD timeout = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            std::array<char, 8192> request{}; std::size_t used = 0;
            while (used + 1 < request.size()) {
                const int n = recv(client, request.data() + used, static_cast<int>(request.size() - 1 - used), 0);
                if (n <= 0) break;
                used += n;
                if (std::strstr(request.data(), "\r\n\r\n")) break;
            }
            constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Type: audio/ogg\r\nicy-name: Advanced fixture\r\nConnection: close\r\n\r\n";
            if (Send(client, response, sizeof(response) - 1))
                Send(client, reinterpret_cast<const char*>(body_.data()), static_cast<int>(body_.size()));
            shutdown(client, SD_BOTH); closesocket(client);
        }
    }
public:
    explicit LiveSource(std::vector<unsigned char> body) : body_(std::move(body)) {
        WSADATA data{}; Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "advanced loopback WSA startup");
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); Check(socket_ != INVALID_SOCKET, "advanced loopback socket");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "advanced loopback bind");
        int size = sizeof(address);
        Check(getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0 && listen(socket_, 2) == 0, "advanced loopback listen");
        port_ = ntohs(address.sin_port); worker_ = std::thread([this] { Run(); });
    }
    ~LiveSource() { stop_ = true; if (worker_.joinable()) worker_.join(); closesocket(socket_); WSACleanup(); }
    std::wstring Url() const { return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/live.ogg"; }
};

std::unique_ptr<CodecReader> Reader(const std::filesystem::path& path, Settings settings = {}) {
    auto stream = std::make_shared<ByteStream>(); std::wstring error;
    Check(stream->Open(path.c_str(), settings, error), "open fixture ByteStream");
    auto reader = OpenOpus(stream, settings, error);
    if (!reader) std::wcerr << L"OpenOpus: " << error << L'\n';
    Check(static_cast<bool>(reader), "open advanced fixture reader"); return reader;
}
bool Rejects(const std::filesystem::path& path) {
    auto stream = std::make_shared<ByteStream>(); Settings settings; std::wstring error;
    Check(stream->Open(path.c_str(), settings, error), "open malformed fixture ByteStream");
    return !OpenOpus(stream, settings, error);
}
struct Segment { int link = 0, rate = 0, channels = 0; bool windows = false; std::vector<float> pcm; };
std::vector<Segment> Decode(CodecReader& reader, int capacity = 37) {
    std::vector<Segment> segments;
    std::array<float, BlockFrames * MaxChannels + 16> buffer{};
    for (int guard = 0; guard < 100000; ++guard) {
        buffer.fill(123456.f);
        const int n = reader.Read(buffer.data() + 8, capacity);
        Check(n >= 0 && n <= capacity, "bounded advanced reader result");
        for (int i = 0; i < 8; ++i) Check(buffer[i] == 123456.f, "advanced lower output canary");
        if (!n) return segments;
        const auto& info = reader.Info();
        const int channels = info.current.channels;
        Check(channels >= 1 && channels <= MaxChannels, "rendered channel bound");
        for (std::size_t i = 8 + static_cast<std::size_t>(n) * channels; i < buffer.size(); ++i)
            Check(buffer[i] == 123456.f, "advanced upper output canary");
        if (segments.empty() || segments.back().link != info.current_link) segments.push_back({info.current_link, info.current.rate, channels, reader.WindowsOrder(), {}});
        auto& segment = segments.back();
        Check(segment.rate == info.current.rate && segment.channels == channels && segment.windows == reader.WindowsOrder(), "one logical link per read, matching metadata");
        for (int i = 0; i < n * channels; ++i) Check(std::isfinite(buffer[8 + i]), "finite decoded output");
        segment.pcm.insert(segment.pcm.end(), buffer.begin() + 8, buffer.begin() + 8 + n * channels);
    }
    throw std::runtime_error("bounded advanced decode loop");
}
double Tone(const Segment& segment, double frequency) {
    double real = 0, imaginary = 0;
    const auto frames = segment.pcm.size() / segment.channels;
    const std::size_t first = (std::min)(static_cast<std::size_t>(2000), frames / 4);
    for (std::size_t i = first; i < frames; ++i) {
        const double value = segment.pcm[i * segment.channels];
        const double phase = 2 * Pi * frequency * i / segment.rate;
        real += value * std::cos(phase); imaginary -= value * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / (frames - first);
}
double Difference(const std::vector<float>& a, const std::vector<float>& b) {
    Check(a.size() == b.size(), "speech comparison exact sample count");
    double sum = 0; for (std::size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i]);
    return sum / (std::max)(std::size_t(1), a.size());
}
void HeaderTests() {
    using opus_native::Header; using opus_native::ParseHeader;
    Header h;
    std::vector<unsigned char> head{'O','p','u','s','H','e','a','d',1,4,0,0,0,0,0,0,0,0,2,4,0,0,1,2,3};
    Check(ParseHeader(head.data(), head.size(), h), "valid family2 header");
    for (std::size_t size = 0; size < head.size(); ++size) Check(!ParseHeader(head.data(), size, h), "reject every truncated family2 header");
    auto bad = head; bad[9] = 5; Check(!ParseHeader(bad.data(), bad.size(), h), "reject incomplete harmonic order");
    bad = head; bad[19] = 0; Check(!ParseHeader(bad.data(), bad.size(), h), "reject zero stream count");
    bad = head; bad[20] = 5; Check(!ParseHeader(bad.data(), bad.size(), h), "reject excessive coupled streams");
    bad = head; bad[21] = 4; Check(!ParseHeader(bad.data(), bad.size(), h), "reject out-of-range map index");
    bad = head; bad[21] = 255; Check(ParseHeader(bad.data(), bad.size(), h), "allow explicitly silent mapped channel");
    bad = head; bad[8] = 16; Check(!ParseHeader(bad.data(), bad.size(), h), "reject unsupported major version");
    bad = head; bad[18] = 255; Check(!ParseHeader(bad.data(), bad.size(), h), "reject unknown mapping family");
    bad.assign(head.begin(), head.begin() + 21); bad[18] = 3;
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) Le(bad, row == column ? 32767 : 0, 2);
    Check(ParseHeader(bad.data(), bad.size(), h), "valid family3 matrix header");
    auto matrix = bad; matrix.pop_back(); Check(!ParseHeader(matrix.data(), matrix.size(), h), "reject short projection matrix");
    matrix = bad; matrix.push_back(0); Check(!ParseHeader(matrix.data(), matrix.size(), h), "reject extra projection matrix bytes");
    bad[19] = 255; bad[20] = 1; Check(!ParseHeader(bad.data(), bad.size(), h), "reject coded-channel sum overflow");
    int duration = 0; bool qext = false;
    const std::array<unsigned char, 6> padding_not_qext{0x83, 0x41, 2, 0, 0xf8, 0};
    Check(opus_native::InspectPacket(padding_not_qext.data(), padding_not_qext.size(), 1, duration, qext) && qext, "detect long QEXT padding extension");
    auto fake = padding_not_qext; fake[4] = 7; // Short unknown extension, byte0xf8 as its payload is not an extension header.
    fake[5] = 0xf8;
    Check(opus_native::InspectPacket(fake.data(), fake.size(), 1, duration, qext) && !qext, "do not find QEXT inside another extension payload");
    for (unsigned n = 0; n < 65536; ++n) {
        const std::array<unsigned char, 3> packet{static_cast<unsigned char>(n), static_cast<unsigned char>(n >> 8), 0xff};
        opus_native::InspectPacket(packet.data(), packet.size(), 1 + (n & 3), duration, qext);
    }
}
void RendererTests() {
    int order = 0, harmonics = 0;
    Check(AmbisonicShape(4, order, harmonics) && order == 1 && harmonics == 4, "FOA ACN shape");
    Check(AmbisonicShape(227, order, harmonics) && order == 14 && harmonics == 225, "maximum harmonic shape bound");
    Check(!AmbisonicShape(255, order, harmonics), "invalid maximum channel shape");
    AmbisonicRenderer renderer; Check(renderer.Init(4, 48000, AmbisonicMode::Stereo), "initialize analytic stereo");
    const std::array<float, 4> y{0, 1, 0, 0}; std::array<float, 8> output{};
    renderer.Render(y.data(), output.data(), 1);
    Check(output[0] > 0 && output[1] < 0 && std::abs(output[0] + output[1]) < 1e-6, "ACN Y is a harmonic, not a speaker channel");
    Check(renderer.Init(4, 48000, AmbisonicMode::Surround71), "initialize Windows7.1 decoder");
    renderer.Render(y.data(), output.data(), 1);
    Check(output[3] == 0 && output[0] > 0 && output[1] < 0 && output[4] > 0 && output[5] < 0 && output[6] > 0 && output[7] < 0, "7.1 Windows FL FR C LFE BL BR SL SR order");
    Check(renderer.Init(6, 48000, AmbisonicMode::Surround51), "initialize non-diegetic stereo");
    const std::array<float, 6> direct{0,0,0,0,.25f,-.5f}; renderer.Render(direct.data(), output.data(), 1);
    Check(output[0] == .25f && output[1] == -.5f && output[2] == 0 && output[3] == 0 && output[4] == 0 && output[5] == 0, "non-diegetic stereo routes directly to listener left/right");
    Check(renderer.Init(4, 96000, AmbisonicMode::Headphones), "initialize analytic headphone delay at96k");
    std::vector<float> impulse(200 * 4), headphones(200 * 2); impulse[0] = impulse[3] = .5f;
    renderer.Render(impulse.data(), headphones.data(), 200);
    Check(std::any_of(headphones.begin() + 2, headphones.end(), [](float x) { return std::abs(x) > 1e-8f; }), "analytic binaural has stateful ITD/head-shadow response");
}
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        Check(argc == 2, "usage: opus_advanced_tests DIRECTORY");
        const auto directory = std::filesystem::path(argv[1]); std::filesystem::create_directories(directory);
        HeaderTests(); RendererTests();
        std::cout << "Advanced Opus: headers and analytic rendering passed" << std::endl;
        const auto ordinary = directory / L"ordinary.ogg", hd = directory / L"qext96.ogg";
        Options options; options.hint = 96000; Fixture(ordinary, options);
        auto reader = Reader(ordinary); auto regular = Decode(*reader);
        Check(regular.size() == 1 && regular[0].rate == 48000 && regular[0].pcm.size() / 2 == 7013, "input rate hint alone is not HD; exact ordinary sample count");
        Check(reader->Info().current.frames == 7013 && reader->Info().duration_ms == 7013 / 48, "ordinary metadata timebase");
        std::cout << "Advanced Opus: ordinary48k passed" << std::endl;
        options.hd = true; options.hint = 48000; options.serial++; options.frames48 = 16013; Fixture(hd, options);
        reader = Reader(hd); auto high = Decode(*reader);
        Check(high.size() == 1 && high[0].rate == 96000 && high[0].pcm.size() / 2 == 32026, "native QEXT96k exact output count independent of input-rate hint");
        Check(reader->Info().current.frames == 32026 && reader->Info().duration_ms == 16013 / 48, "HD granules/pre-skip/duration stay48k units");
        const double ultrasonic = Tone(high[0], 30000);
        Check(ultrasonic > .025, "real >24k QEXT response, not48k upsampling");
        Settings no_hd; no_hd.opus_hd = false; reader = Reader(hd, no_hd); auto low = Decode(*reader);
        Check(low[0].rate == 48000 && low[0].pcm.size() / 2 == 16013, "HD disabled decodes native48k with correct count");
        Settings bwe96; bwe96.opus_bwe = true; reader = Reader(hd, bwe96); auto without_bwe96 = Decode(*reader);
        Check(without_bwe96[0].pcm == high[0].pcm && reader->Info().current.decoder_note.find(L"inapplicable") != std::wstring::npos, "BWE does not alter96k decode and is explicitly inapplicable");
        const auto late_hd = directory / L"late-qext.ogg";
        options.late_qext_packets = 2; options.serial++; Fixture(late_hd, options);
        reader = Reader(late_hd); auto late = Decode(*reader);
        Check(late[0].rate == 96000 && late[0].pcm.size() / 2 == 32026, "seekable scan detects QEXT beyond first packet");
        const auto offset = directory / L"offset.ogg";
        options = {}; options.serial = 34010; options.granule_offset = 480000; options.group = 4; Fixture(offset, options);
        reader = Reader(offset); auto with_offset = Decode(*reader);
        Check(with_offset[0].pcm.size() / 2 == 7013 && reader->Info().duration_ms == 7013 / 48, "initial granule offset is not extra audio");
        const auto trimmed = directory / L"page-trim.ogg";
        options.granule_offset = 0; options.group = 255; options.extra_packets = 3; options.serial++; Fixture(trimmed, options);
        reader = Reader(trimmed); auto page_trim = Decode(*reader);
        Check(page_trim[0].pcm.size() / 2 == 7013, "EOS page trims before earlier packets are exposed");
        std::cout << "Advanced Opus: native QEXT96k and granule trimming passed" << std::endl;
        const auto speech = directory / L"wideband-speech.ogg";
        options = {}; options.channels = 1; options.speech = true; options.frames48 = 48000; options.serial = 34020; Fixture(speech, options);
        reader = Reader(speech); auto plain_speech = Decode(*reader, BlockFrames);
        Settings enhanced; enhanced.opus_speech = true; reader = Reader(speech, enhanced); auto osce = Decode(*reader, BlockFrames);
        const double speech_difference = Difference(plain_speech[0].pcm, osce[0].pcm);
        Check(speech_difference > 1e-6, "optional OSCE changes eligible low-rate speech");
        Settings bwe; bwe.opus_bwe = true; reader = Reader(speech, bwe); auto extended = Decode(*reader, BlockFrames);
        const double bwe_difference = Difference(plain_speech[0].pcm, extended[0].pcm);
        Check(bwe_difference > 1e-6, "per-elementary-decoder OSCE_BWE changes eligible wideband speech");
        reader = Reader(speech); auto off_again = Decode(*reader, BlockFrames);
        Check(plain_speech[0].pcm == off_again[0].pcm, "both neural features default OFF deterministically");
        std::cout << "Advanced Opus: optional speech and BWE passed" << std::endl;
        std::vector<std::filesystem::path> ambisonic_files;
        for (int family : {2, 3}) {
            for (int channels : {4, 6, 9}) {
                const auto path = directory / (L"ambi-" + std::to_wstring(family) + L"-" + std::to_wstring(channels) + L".ogg");
                options = {}; options.family = family; options.channels = channels; options.serial = 34100 + family * 10 + channels; Fixture(path, options);
                ambisonic_files.push_back(path);
                for (auto mode : {AmbisonicMode::Stereo, AmbisonicMode::Headphones, AmbisonicMode::Quad, AmbisonicMode::Surround51, AmbisonicMode::Surround71}) {
                    Settings s; s.ambisonic = mode;
                    reader = Reader(path, s); auto rendered = Decode(*reader);
                    const int count = mode == AmbisonicMode::Quad ? 4 : mode == AmbisonicMode::Surround51 ? 6 : mode == AmbisonicMode::Surround71 ? 8 : 2;
                    Check(rendered.size() == 1 && rendered[0].channels == count && rendered[0].windows && rendered[0].pcm.size() / count == 7013, "family2/3 native decode and configured rendered layout");
                    if (count >= 6) for (std::size_t frame = 0; frame < 7013; ++frame) Check(rendered[0].pcm[frame * count + 3] == 0, "rendered LFE always silent");
                    if (mode == AmbisonicMode::Headphones) Check(reader->Info().current.decoder_note.find(L"not measured HRTF") != std::wstring::npos, "approximate binaural limitations visible");
                    if (mode == AmbisonicMode::Headphones) {
                        Check(reader->Seek(37), "seek analytic binaural from BOS with complete history");
                        auto sought = Decode(*reader); const std::size_t first = 37 * 48 * count;
                        Check(sought.size() == 1 && sought[0].pcm == std::vector<float>(rendered[0].pcm.begin() + first, rendered[0].pcm.end()), "headphone seek sample-exact versus uninterrupted rendering");
                    }
                }
            }
        }
        for (int coded_channels : {2, 6}) {
            const auto rectangle = directory / (L"rectangular-" + std::to_wstring(coded_channels) + L".ogg");
            options = {}; options.family = 3; options.channels = coded_channels; options.projection_rows = 4;
            options.serial = 34200 + coded_channels; Fixture(rectangle, options);
            reader = Reader(rectangle); auto audio = Decode(*reader);
            Check(audio.size() == 1 && audio[0].channels == 2 && audio[0].windows && audio[0].pcm.size() / 2 == 7013,
                "rectangular family3 output shape and count");
            Check(Tone(audio[0], 300 + 137 * (coded_channels - 1)) > .035,
                "rectangular family3 uses final coded column rather than dropping it");
            for (std::size_t frame = 0; frame < 7013; ++frame)
                Check(audio[0].pcm[frame * 2] == audio[0].pcm[frame * 2 + 1], "rectangular Q15 W-only matrix is centered");
            Check(reader->Info().current.decoder_note.find(L"rectangular Q15") != std::wstring::npos, "rectangular fallback explicitly identified");
        }
        const auto surround = directory / L"surround.ogg";
        options = {}; options.family = 1; options.channels = 6; options.serial = 34300; Fixture(surround, options);
        reader = Reader(surround); auto speakers = Decode(*reader);
        Check(speakers[0].channels == 6 && !speakers[0].windows && speakers[0].pcm.size() / 6 == 7013, "ordinary family1 retains codec order and48k default");
        const auto chain = directory / L"mixed-chain.ogg"; Save(chain, Bytes(ordinary));
        Append(chain, hd); Append(chain, ambisonic_files[0]); Append(chain, ambisonic_files[3]); Append(chain, surround);
        Settings chain_settings; chain_settings.ambisonic = AmbisonicMode::Surround51;
        reader = Reader(chain, chain_settings); auto segments = Decode(*reader);
        Check(segments.size() == 5 && segments[0].rate == 48000 && segments[1].rate == 96000 && segments[2].windows && segments[3].windows && !segments[4].windows, "ordinary/HD/family2/family3/speaker chain boundaries");
        Check(reader->Info().duration_ms == (4 * 7013 + 16013) / 48, "mixed chain accumulated duration uses48k granules");
        const std::array<int, 6> seek_times{0, 37, 180, 500, 650, 100000};
        for (int milliseconds : seek_times) {
            Check(reader->Seek(milliseconds), "mixed-link local seek");
            auto sought = Decode(*reader);
            std::int64_t target = static_cast<std::int64_t>(milliseconds) * 48; std::size_t link = 0;
            const std::array<int, 5> counts{7013,16013,7013,7013,7013};
            while (link + 1 < counts.size() && target >= counts[link]) target -= counts[link++];
            target = (std::min)(target, static_cast<std::int64_t>(counts[link]));
            if (target == counts[link] && link + 1 == counts.size()) { Check(sought.empty(), "seek beyond end clamps to EOF"); continue; }
            Check(sought.size() == segments.size() - link, "seek retains remaining link count");
            for (std::size_t i = 0; i < sought.size(); ++i) {
                const auto& reference = segments[link + i]; const std::size_t skip = i ? 0 : static_cast<std::size_t>(target) * (reference.rate / 48000) * reference.channels;
                Check(sought[i].rate == reference.rate && sought[i].channels == reference.channels && sought[i].pcm == std::vector<float>(reference.pcm.begin() + skip, reference.pcm.end()), "seek exact sample alignment across native rate/layout changes");
            }
        }
        // Exercise the same decoder over a genuine nonseekable ByteStream.
        // Initial metadata has an explicit unknown duration, but not a zero
        // bitrate, and querying the decoded-byte window must remain useful.
        for (const auto& source : {ordinary, hd}) {
            LiveSource server(Bytes(source)); Settings live_settings;
            live_settings.proxy_mode = 0; live_settings.prebuffer_kb = 0; live_settings.underrun_percent = 0;
            auto stream = std::make_shared<ByteStream>(); std::wstring error;
            Check(stream->Open(server.Url().c_str(), live_settings, error), "open forward-only live fixture");
            auto live = OpenOpus(stream, live_settings, error);
            Check(live && !live->Info().seekable && live->Info().radio && live->Info().duration_ms == -1 && live->Info().current.frames == -1,
                "forward-only initial duration is explicitly unknown");
            Check(live->Info().current.bitrate > 1000 && live->InstantBitrate() > 1000, "first audio-page bitrate available before EOS");
            stream->FinishProbe();
            std::array<float, BlockFrames * MaxChannels> output{};
            const int first = live->Read(output.data(), BlockFrames);
            Check(first > 0, "read live fixture first block");
            const int bitrate = live->InstantBitrate();
            Check(bitrate > 1000 && bitrate < 8000000 && live->InstantBitrate() == bitrate, "consumed live bitrate window retains last usable estimate");
            auto remainder = Decode(*live);
            const int expected_rate = source == hd ? 96000 : 48000;
            const int expected_frames = source == hd ? 32026 : 7013;
            Check(remainder.size() == 1 && remainder[0].rate == expected_rate &&
                first + remainder[0].pcm.size() / remainder[0].channels == static_cast<std::size_t>(expected_frames),
                "forward ordinary/HD counts and native format");
            Check(!live->Seek(0) && live->InstantBitrate() > 1000, "forward seek rejected and bitrate survives EOF");
        }
        {
            LiveSource server(Bytes(late_hd)); Settings live_settings;
            live_settings.proxy_mode = 0; live_settings.prebuffer_kb = 0; live_settings.underrun_percent = 0;
            auto stream = std::make_shared<ByteStream>(); std::wstring error;
            Check(stream->Open(server.Url().c_str(), live_settings, error), "open late-QEXT forward fixture");
            auto live = OpenOpus(stream, live_settings, error); Check(static_cast<bool>(live), "open initial ordinary page in late-QEXT stream");
            stream->FinishProbe();
            std::array<float, BlockFrames * MaxChannels> output{}; int n = 0, calls = 0;
            do { n = live->Read(output.data(), BlockFrames); } while (n > 0 && ++calls < 1000);
            Check(n < 0 && calls < 1000, "forward-only late QEXT fails explicitly rather than faking HD");
        }
        // A relay can replay identification/tags, then join a much later
        // audio page. Only that first audio boundary may recover a gap or an
        // incomplete continued packet. Later damage and local gaps stay fatal.
        for (bool continued : {false, true}) {
            auto joined = Bytes(ordinary); int page_number = 0;
            for (std::size_t at = 0; at < joined.size(); at += PageSize(joined, at), ++page_number) {
                if (page_number < 2) continue;
                const unsigned sequence = static_cast<unsigned>(100 + page_number - 2);
                for (int i = 0; i < 4; ++i) joined[at + 18 + i] = static_cast<unsigned char>(sequence >> (8 * i));
                if (page_number == 2 && continued) joined[at + 5] |= 1;
                Crc(joined, at);
            }
            const auto local = directory / (continued ? L"local-continuation.ogg" : L"local-gap.ogg");
            Save(local, joined); Check(Rejects(local), "local files never receive radio join recovery");
            {
                LiveSource server(joined); Settings live_settings;
                live_settings.proxy_mode = 0; live_settings.prebuffer_kb = 0; live_settings.underrun_percent = 0;
                auto stream = std::make_shared<ByteStream>(); std::wstring error;
                Check(stream->Open(server.Url().c_str(), live_settings, error), "open initial radio page-gap fixture");
                auto live = OpenOpus(stream, live_settings, error); Check(static_cast<bool>(live), "accept one initial forward-only audio join");
                stream->FinishProbe(); auto audio = Decode(*live);
                Check(audio.size() == 1 && audio[0].pcm.size() / 2 == static_cast<std::size_t>(continued ? 7013 - 960 : 7013),
                    "initial continued fragment discarded with correct48k granule count");
            }
            // Corrupt a later sequence while preserving its CRC. The initial
            // join exception must not turn into general hole concealment.
            std::size_t later = 0; for (int page = 0; page < 4; ++page) later += PageSize(joined, later);
            joined[later + 18] ^= 4; Crc(joined, later);
            {
                LiveSource server(joined); Settings live_settings;
                live_settings.proxy_mode = 0; live_settings.prebuffer_kb = 0; live_settings.underrun_percent = 0;
                auto stream = std::make_shared<ByteStream>(); std::wstring error;
                Check(stream->Open(server.Url().c_str(), live_settings, error), "open later damaged radio page");
                auto live = OpenOpus(stream, live_settings, error); Check(static_cast<bool>(live), "validated initial join before later damage");
                stream->FinishProbe(); std::array<float, BlockFrames * MaxChannels> output{}; int n = 0, calls = 0;
                do { n = live->Read(output.data(), BlockFrames); } while (n > 0 && ++calls < 1000);
                Check(n < 0 && calls < 1000, "later radio sequence gap remains fatal");
            }
        }
        auto damaged = Bytes(ordinary); const auto bad_crc = directory / L"crc.ogg";
        damaged[damaged.size() - 1] ^= 1; Save(bad_crc, damaged); Check(Rejects(bad_crc), "CRC damage rejected");
        const auto truncated = directory / L"truncated.ogg"; damaged = Bytes(ordinary); damaged.resize(damaged.size() - 3); Save(truncated, damaged); Check(Rejects(truncated), "truncated page rejected");
        const auto bad_sequence = directory / L"sequence.ogg"; damaged = Bytes(ordinary);
        const auto second = PageSize(damaged, 0); damaged[second + 18] ^= 3; Crc(damaged, second); Save(bad_sequence, damaged); Check(Rejects(bad_sequence), "selected stream sequence hole rejected");
        const auto bad_head = directory / L"bad-head.ogg"; damaged = Bytes(ambisonic_files[0]);
        damaged[27 + damaged[26] + 9] = 5; Crc(damaged, 0); Save(bad_head, damaged); Check(Rejects(bad_head), "malformed ambisonic header rejected before decode");
        const auto bad_matrix = directory / L"bad-matrix.ogg"; damaged = Bytes(ambisonic_files[3]);
        damaged[27 + damaged[26] + 19] = 0; Crc(damaged, 0); Save(bad_matrix, damaged); Check(Rejects(bad_matrix), "malformed projection counts rejected before allocation");
        const auto missing_eos = directory / L"missing-eos.ogg"; damaged = Bytes(ordinary);
        std::size_t last = 0; for (std::size_t at = 0; at < damaged.size(); at += PageSize(damaged, at)) last = at;
        damaged[last + 5] &= ~4; Crc(damaged, last); Save(missing_eos, damaged); Check(Rejects(missing_eos), "missing EOS is truncation, not successful EOF");
        const auto unrelated = directory / L"unrelated.ogg";
        { OggWriter writer(unrelated, 55555); std::vector<unsigned char> content{'N','o','t','O','p','u','s','!'}; writer.Packet(content, 0, true, true); }
        const auto multiplexed = directory / L"multiplexed.ogg"; auto original = Bytes(ordinary); auto other = Bytes(unrelated);
        const auto insert = PageSize(original, 0); original.insert(original.begin() + insert, other.begin(), other.end()); Save(multiplexed, original);
        reader = Reader(multiplexed); auto unmuxed = Decode(*reader);
        Check(unmuxed[0].pcm == regular[0].pcm, "unrelated multiplexed stream safely ignored");
        auto cancelled = std::make_shared<ByteStream>(); std::wstring error; Settings settings;
        Check(cancelled->Open(ordinary.c_str(), settings, error), "open cancellation fixture"); cancelled->Cancel();
        Check(!OpenOpus(cancelled, settings, error), "cancellation interrupts indexing/open");
        std::cout << "Advanced Opus: native QEXT96k (30kHz amplitude " << ultrasonic << "), 48k granules, OSCE difference " << speech_difference
            << ", BWE difference " << bwe_difference << ", families2/3, approximate binaural/Windows layouts, exact seeks/chains and malformed input passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
