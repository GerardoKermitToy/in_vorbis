// SPDX-License-Identifier: GPL-3.0-only
#include "matroska.hpp"

#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace ogg_input;

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class T>
void WriteDispatchResult(void* result, const T& value) {
    if (result) std::memcpy(result, &value, sizeof(value));
}

template <class T>
bool ReadDispatchArgument(void** arguments, int count, int index, T& value) {
    if (!arguments || index < 0 || count <= index || !arguments[index]) return false;
    std::memcpy(&value, arguments[index], sizeof(value));
    return true;
}

class MockServiceManager final : public matroska::ServiceManager {
public:
    int registered = 0;
    int deregistered = 0;
    matroska::ServiceFactory* factory = nullptr;

    int _dispatch(int message, void* result, void** arguments, int argumentCount) override {
        matroska::ServiceFactory* incoming = nullptr;
        if (message == matroska::ServiceManagerMessage::Register &&
            ReadDispatchArgument(arguments, argumentCount, 0, incoming) && incoming) {
            factory = incoming;
            ++registered;
            WriteDispatchResult(result, 1);
            return 1;
        }
        if (message == matroska::ServiceManagerMessage::Deregister &&
            ReadDispatchArgument(arguments, argumentCount, 0, incoming) && incoming == factory) {
            ++deregistered;
            WriteDispatchResult(result, 1);
            return 1;
        }
        WriteDispatchResult(result, 0);
        return 0;
    }
};

LRESULT CALLBACK MockWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == matroska::kServiceManagerMessage && wParam == 0 &&
        lParam == matroska::kGetApiService) {
        auto* manager = reinterpret_cast<MockServiceManager*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return reinterpret_cast<LRESULT>(manager);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

class MockHostWindow {
public:
    explicit MockHostWindow(MockServiceManager* manager) {
        WNDCLASSW windowClass{};
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpfnWndProc = MockWindowProcedure;
        windowClass.lpszClassName = L"IndependentVorbisMatroskaTestWindow";
        const ATOM atom = RegisterClassW(&windowClass);
        Check(atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register mock window class");
        window_ = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, windowClass.hInstance, manager);
        Check(window_ != nullptr, "create mock host window");
    }

    ~MockHostWindow() {
        if (window_) DestroyWindow(window_);
    }

    HWND get() const noexcept { return window_; }

private:
    HWND window_ = nullptr;
};

struct EncodedVorbis {
    std::array<std::vector<unsigned char>, 3> headers;
    std::vector<std::vector<unsigned char>> packets;
    int channels = 0;
    int rate = 0;

    EncodedVorbis(int channelCount, int sampleRate, int frames) : channels(channelCount), rate(sampleRate) {
        vorbis_info info{};
        vorbis_comment comments{};
        vorbis_dsp_state analysis{};
        vorbis_block block{};
        bool infoReady = false;
        bool commentsReady = false;
        bool analysisReady = false;
        bool blockReady = false;
        try {
            vorbis_info_init(&info);
            infoReady = true;
            Check(vorbis_encode_init_vbr(&info, channelCount, sampleRate, 0.35f) == 0,
                "initialize Vorbis encoder");
            vorbis_comment_init(&comments);
            commentsReady = true;
            vorbis_comment_add_tag(&comments, const_cast<char*>("TITLE"), const_cast<char*>("Matroska test"));
            Check(vorbis_analysis_init(&analysis, &info) == 0, "initialize Vorbis analysis");
            analysisReady = true;
            Check(vorbis_block_init(&analysis, &block) == 0, "initialize Vorbis analysis block");
            blockReady = true;

            ogg_packet identification{};
            ogg_packet comment{};
            ogg_packet setup{};
            Check(vorbis_analysis_headerout(&analysis, &comments, &identification, &comment, &setup) == 0,
                "write Vorbis headers");
            CopyPacket(identification, headers[0]);
            CopyPacket(comment, headers[1]);
            CopyPacket(setup, headers[2]);

            float** samples = vorbis_analysis_buffer(&analysis, frames);
            Check(samples != nullptr, "allocate Vorbis analysis samples");
            for (int frame = 0; frame < frames; ++frame) {
                for (int channel = 0; channel < channelCount; ++channel) {
                    const float frequency = 0.004f * static_cast<float>(channel + 1);
                    samples[channel][frame] = 0.20f * std::sin(frequency * static_cast<float>(frame));
                }
            }
            Check(vorbis_analysis_wrote(&analysis, frames) == 0, "submit Vorbis analysis samples");
            Drain(&analysis, &block, packets);
            Check(vorbis_analysis_wrote(&analysis, 0) == 0, "finish Vorbis analysis");
            Drain(&analysis, &block, packets);
            Check(!packets.empty(), "produce Vorbis packets");

            vorbis_block_clear(&block);
            blockReady = false;
            vorbis_dsp_clear(&analysis);
            analysisReady = false;
            vorbis_comment_clear(&comments);
            commentsReady = false;
            vorbis_info_clear(&info);
            infoReady = false;
        } catch (...) {
            if (blockReady) vorbis_block_clear(&block);
            if (analysisReady) vorbis_dsp_clear(&analysis);
            if (commentsReady) vorbis_comment_clear(&comments);
            if (infoReady) vorbis_info_clear(&info);
            throw;
        }
    }

private:
    static void CopyPacket(const ogg_packet& packet, std::vector<unsigned char>& destination) {
        Check(packet.packet && packet.bytes > 0, "valid generated Vorbis packet");
        destination.assign(packet.packet, packet.packet + packet.bytes);
    }

    static void Drain(vorbis_dsp_state* analysis, vorbis_block* block,
        std::vector<std::vector<unsigned char>>& packets) {
        while (vorbis_analysis_blockout(analysis, block) == 1) {
            Check(vorbis_analysis(block, nullptr) == 0, "analyze Vorbis block");
            Check(vorbis_bitrate_addblock(block) == 0, "submit Vorbis bitrate block");
            ogg_packet packet{};
            while (vorbis_bitrate_flushpacket(analysis, &packet) != 0) {
                std::vector<unsigned char> bytes;
                CopyPacket(packet, bytes);
                packets.push_back(std::move(bytes));
            }
        }
    }
};

void WriteXiphLength(std::vector<unsigned char>& output, std::size_t length) {
    while (length >= 255) {
        output.push_back(255);
        length -= 255;
    }
    output.push_back(static_cast<unsigned char>(length));
}

std::vector<unsigned char> MakeCodecPrivate(const EncodedVorbis& fixture) {
    std::vector<unsigned char> privateData;
    privateData.push_back(2);
    WriteXiphLength(privateData, fixture.headers[0].size());
    WriteXiphLength(privateData, fixture.headers[1].size());
    for (const auto& header : fixture.headers) {
        privateData.insert(privateData.end(), header.begin(), header.end());
    }
    return privateData;
}

struct TrackFixture {
    std::vector<unsigned char> codecPrivate;
    matroska::TrackEntryData track{};
    matroska::AudioData audio{};

    explicit TrackFixture(const EncodedVorbis& fixture) : codecPrivate(MakeCodecPrivate(fixture)) {
        track.struct_size = sizeof(track);
        track.codec_id = const_cast<char*>("A_VORBIS");
        track.codec_private = codecPrivate.data();
        track.codec_private_len = codecPrivate.size();
        audio.struct_size = sizeof(audio);
        audio.sampling_frequency = static_cast<std::uint64_t>(fixture.rate);
        audio.channels = static_cast<std::uint64_t>(fixture.channels);
        audio.bit_depth = 16;
    }
};

matroska::DecoderService* GetDecoderService(matroska::ServiceFactory* factory) {
    int globalLock = 0;
    void* arguments[] = {&globalLock};
    void* service = nullptr;
    Check(factory->_dispatch(matroska::ServiceFactoryMessage::GetInterface, &service, arguments, 1) == 1,
        "get Matroska creator interface");
    Check(service != nullptr, "Matroska creator interface exists");
    return static_cast<matroska::DecoderService*>(service);
}

matroska::AudioDecoder* CreateDecoder(matroska::DecoderService* service, const char* codec,
    const TrackFixture& fixture, unsigned int bits, unsigned int maxChannels, bool floatingPoint,
    int expectedStatus) {
    const char* codecArgument = codec;
    const matroska::TrackEntryData* track = &fixture.track;
    const matroska::AudioData* audio = &fixture.audio;
    matroska::AudioDecoder* decoder = nullptr;
    matroska::AudioDecoder** output = &decoder;
    void* arguments[] = {&codecArgument, &track, &audio, &bits, &maxChannels, &floatingPoint, &output};
    int status = std::numeric_limits<int>::min();
    Check(service->_dispatch(matroska::DecoderServiceMessage::CreateAudioDecoder, &status, arguments, 7) == 1,
        "dispatch audio decoder creation");
    Check(status == expectedStatus, "expected audio decoder creation status");
    if (status == matroska::kCreateDecoderSuccess) Check(decoder != nullptr, "created audio decoder");
    else Check(decoder == nullptr, "failed creation leaves null decoder");
    return decoder;
}

void CloseDecoder(matroska::AudioDecoder* decoder) {
    if (!decoder) return;
    Check(decoder->_dispatch(matroska::AudioDecoderMessage::Close, nullptr, nullptr, 0) == 1,
        "close audio decoder");
}

struct DecoderProperties {
    unsigned int rate = 0;
    unsigned int channels = 0;
    unsigned int bits = 0;
    bool floating = false;
    std::size_t maximumPacketBytes = 0;
};

DecoderProperties GetProperties(matroska::AudioDecoder* decoder) {
    DecoderProperties properties;
    unsigned int* rate = &properties.rate;
    unsigned int* channels = &properties.channels;
    unsigned int* bits = &properties.bits;
    bool* floating = &properties.floating;
    void* propertyArguments[] = {&rate, &channels, &bits, &floating};
    int status = 0;
    Check(decoder->_dispatch(matroska::AudioDecoderMessage::GetOutputProperties, &status,
            propertyArguments, 4) == 1 && status == matroska::kDecodeSuccess,
        "get decoder output properties");

    std::size_t* frameBytes = &properties.maximumPacketBytes;
    void* sizeArguments[] = {&frameBytes};
    Check(decoder->_dispatch(matroska::AudioDecoderMessage::OutputFrameSize, &status,
            sizeArguments, 1) == 1 && status == matroska::kDecodeSuccess,
        "get maximum packet output size");
    Check(properties.maximumPacketBytes != 0, "positive maximum packet output size");
    return properties;
}

struct DecodeResult {
    int status = matroska::kDecodeFailure;
    std::size_t bytes = 0;
};

DecodeResult Decode(matroska::AudioDecoder* decoder, const void* packet, std::size_t packetBytes,
    void* output, std::size_t outputCapacity) {
    const void* input = packet;
    std::size_t inputSize = packetBytes;
    void* destination = output;
    std::size_t outputSize = outputCapacity;
    std::size_t* outputSizePointer = &outputSize;
    void* arguments[] = {&input, &inputSize, &destination, &outputSizePointer};
    DecodeResult result;
    Check(decoder->_dispatch(matroska::AudioDecoderMessage::DecodeBlock, &result.status, arguments, 4) == 1,
        "dispatch audio block decode");
    result.bytes = outputSize;
    return result;
}

std::size_t FindFirstOutputPacket(matroska::DecoderService* service, const TrackFixture& fixture,
    const EncodedVorbis& encoded, std::size_t capacity) {
    matroska::AudioDecoder* decoder = CreateDecoder(service, "A_VORBIS", fixture, 16, 0, false,
        matroska::kCreateDecoderSuccess);
    std::vector<unsigned char> output(capacity);
    std::size_t packetIndex = encoded.packets.size();
    for (std::size_t i = 0; i < encoded.packets.size(); ++i) {
        const auto result = Decode(decoder, encoded.packets[i].data(), encoded.packets[i].size(),
            output.data(), output.size());
        Check(result.status == matroska::kDecodeSuccess || result.status == matroska::kDecodeNeedMoreInput,
            "decode generated Vorbis packet");
        if (result.status == matroska::kDecodeSuccess && result.bytes != 0) {
            packetIndex = i;
            break;
        }
    }
    CloseDecoder(decoder);
    Check(packetIndex != encoded.packets.size(), "find a Vorbis packet with PCM output");
    return packetIndex;
}

void TestFactoryAndDecoder() {
    MockServiceManager manager;
    MockHostWindow host(&manager);
    struct RegistrationGuard { ~RegistrationGuard() { UnregisterMatroska(); } } registration;
    RegisterMatroska(host.get());
    Check(manager.registered == 1 && manager.factory != nullptr, "register independent Matroska factory");

    std::uint32_t serviceType = 0;
    Check(manager.factory->_dispatch(matroska::ServiceFactoryMessage::GetServiceType, &serviceType, nullptr, 0) == 1,
        "get service type");
    Check(serviceType == matroska::kDecoderServiceType, "Matroska service type");
    const char* serviceName = nullptr;
    Check(manager.factory->_dispatch(matroska::ServiceFactoryMessage::GetServiceName, &serviceName, nullptr, 0) == 1 &&
        serviceName && std::strstr(serviceName, "Independent") != nullptr, "independent service name");

    auto* service = GetDecoderService(manager.factory);
    EncodedVorbis encoded(6, 48000, 4096);
    TrackFixture fixture(encoded);

    Check(CreateDecoder(service, "A_OPUS", fixture, 16, 0, false,
        matroska::kCreateDecoderNotMine) == nullptr, "do not claim Matroska Opus");
    const std::vector<unsigned char> savedPrivate = fixture.codecPrivate;
    fixture.codecPrivate = {2, 255};
    fixture.track.codec_private = fixture.codecPrivate.data();
    fixture.track.codec_private_len = fixture.codecPrivate.size();
    Check(CreateDecoder(service, "A_VORBIS", fixture, 16, 0, false,
        matroska::kCreateDecoderFailure) == nullptr, "reject malformed Xiph lacing");
    fixture.codecPrivate = savedPrivate;
    fixture.track.codec_private = fixture.codecPrivate.data();
    fixture.track.codec_private_len = fixture.codecPrivate.size();

    auto* source = CreateDecoder(service, "A_VORBIS", fixture, 16, 0, false,
        matroska::kCreateDecoderSuccess);
    const DecoderProperties sourceProperties = GetProperties(source);
    Check(sourceProperties.rate == 48000 && sourceProperties.channels == 6 &&
        sourceProperties.bits == 16 && !sourceProperties.floating, "preserve source PCM format");
    Check(sourceProperties.maximumPacketBytes >= sourceProperties.channels * 2,
        "packet output allocation is larger than a frame stride");
    CloseDecoder(source);

    auto* packed24 = CreateDecoder(service, "A_VORBIS", fixture, 24, 0, false,
        matroska::kCreateDecoderSuccess);
    const DecoderProperties packed24Properties = GetProperties(packed24);
    Check(packed24Properties.channels == 6 && packed24Properties.bits == 24 && !packed24Properties.floating,
        "packed 24-bit output properties");
    CloseDecoder(packed24);

    auto* floating = CreateDecoder(service, "A_VORBIS", fixture, 16, 2, true,
        matroska::kCreateDecoderSuccess);
    const DecoderProperties floatingProperties = GetProperties(floating);
    Check(floatingProperties.channels == 2 && floatingProperties.bits == 32 && floatingProperties.floating,
        "stereo-capped float output properties");
    CloseDecoder(floating);

    const std::size_t outputPacket = FindFirstOutputPacket(service, fixture, encoded,
        sourceProperties.maximumPacketBytes);
    auto* guardedDecoder = CreateDecoder(service, "A_VORBIS", fixture, 16, 0, false,
        matroska::kCreateDecoderSuccess);
    std::vector<unsigned char> normalOutput(sourceProperties.maximumPacketBytes);
    for (std::size_t i = 0; i < outputPacket; ++i) {
        const auto result = Decode(guardedDecoder, encoded.packets[i].data(), encoded.packets[i].size(),
            normalOutput.data(), normalOutput.size());
        Check(result.status == matroska::kDecodeSuccess || result.status == matroska::kDecodeNeedMoreInput,
            "decode packets preceding capacity test");
    }

    std::vector<unsigned char> smallBuffer(9, 0xA5);
    const auto undersized = Decode(guardedDecoder, encoded.packets[outputPacket].data(),
        encoded.packets[outputPacket].size(), smallBuffer.data() + 4, 1);
    Check(undersized.status == matroska::kDecodeFailure && undersized.bytes > 1,
        "report required bytes for undersized output");
    for (int index = 0; index < 4; ++index) {
        Check(smallBuffer[static_cast<std::size_t>(index)] == 0xA5, "lower output guard");
        Check(smallBuffer[static_cast<std::size_t>(index + 5)] == 0xA5, "upper output guard");
    }

    std::vector<unsigned char> retried(undersized.bytes + 8, 0xA5);
    const auto retry = Decode(guardedDecoder, nullptr, 0, retried.data() + 4, undersized.bytes);
    Check(retry.status == matroska::kDecodeSuccess && retry.bytes == undersized.bytes,
        "retry preserved PCM with sufficient output capacity");
    for (int index = 0; index < 4; ++index) {
        Check(retried[static_cast<std::size_t>(index)] == 0xA5, "lower retry guard");
        Check(retried[retry.bytes + 4 + static_cast<std::size_t>(index)] == 0xA5, "upper retry guard");
    }

    Check(guardedDecoder->_dispatch(matroska::AudioDecoderMessage::EndOfStream, nullptr, nullptr, 0) == 1,
        "signal decoder end of stream");
    const auto afterEnd = Decode(guardedDecoder, nullptr, 0, normalOutput.data(), normalOutput.size());
    Check(afterEnd.status == matroska::kDecodeFailure, "reject new input after end of stream");
    Check(guardedDecoder->_dispatch(matroska::AudioDecoderMessage::Flush, nullptr, nullptr, 0) == 1,
        "flush decoder");
    const auto afterFlush = Decode(guardedDecoder, encoded.packets.front().data(),
        encoded.packets.front().size(), normalOutput.data(), normalOutput.size());
    Check(afterFlush.status == matroska::kDecodeSuccess || afterFlush.status == matroska::kDecodeNeedMoreInput,
        "flush restarts decoder synthesis");
    CloseDecoder(guardedDecoder);

    UnregisterMatroska();
    Check(manager.deregistered == 1, "deregister independent Matroska factory");
}

} // namespace

int wmain() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        TestFactoryAndDecoder();
        std::cout << "Matroska Vorbis bridge tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        UnregisterMatroska();
        return 1;
    }
}
