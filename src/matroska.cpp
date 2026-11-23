// SPDX-License-Identifier: GPL-3.0-only
#include "matroska.hpp"

#include <ogg/ogg.h>
#include <vorbis/codec.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace ogg_input {
namespace {

constexpr std::size_t kMaxCodecPrivateBytes = 1024 * 1024;
constexpr std::size_t kMaxAudioPacketBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxPendingBytes = 16 * 1024 * 1024;
constexpr int kMaximumChannels = 8;
constexpr long kMaximumRate = 768000;

// This GUID and name deliberately identify this independent implementation,
// rather than the service shipped by the historical in_vorbis module.
const GUID kIndependentVorbisMatroskaGuid = {
    0xB603D4EE, 0x5A22, 0x4EF5, {0x90, 0x91, 0x53, 0xA2, 0x68, 0x63, 0x8B, 0xD4}
};
constexpr char kServiceName[] = "Independent Vorbis Matroska Decoder";
constexpr char kServiceTestString[] = "Independent Vorbis Matroska Decoder";

template <class T>
bool ReadArgument(void** arguments, int argumentCount, int index, T& value) noexcept {
    if (!arguments || index < 0 || argumentCount <= index || !arguments[index]) return false;
    std::memcpy(&value, arguments[index], sizeof(value));
    return true;
}

template <class T>
bool WriteResult(void* result, const T& value) noexcept {
    if (!result) return false;
    std::memcpy(result, &value, sizeof(value));
    return true;
}

bool Multiply(std::size_t left, std::size_t right, std::size_t& product) noexcept {
    if (left && right > (std::numeric_limits<std::size_t>::max)() / left) return false;
    product = left * right;
    return true;
}

struct VorbisHeaders {
    const unsigned char* data[3]{};
    std::size_t length[3]{};
};

bool ReadXiphLength(const unsigned char* codecPrivate, std::size_t codecPrivateBytes,
    std::size_t& offset, std::size_t& length) noexcept {
    length = 0;
    for (;;) {
        if (offset >= codecPrivateBytes) return false;
        const unsigned char part = codecPrivate[offset++];
        if (length > (std::numeric_limits<std::size_t>::max)() - part) return false;
        length += part;
        if (part != 255) return true;
    }
}

bool ParseVorbisHeaders(const void* codecPrivate, std::size_t codecPrivateBytes,
    VorbisHeaders& headers) noexcept {
    if (!codecPrivate || codecPrivateBytes < 4 || codecPrivateBytes > kMaxCodecPrivateBytes) return false;
    const auto* bytes = static_cast<const unsigned char*>(codecPrivate);
    // Matroska stores "number of headers minus one" in this field.  Vorbis has
    // exactly identification, comment, and setup packets.
    if (bytes[0] != 2) return false;

    std::size_t offset = 1;
    if (!ReadXiphLength(bytes, codecPrivateBytes, offset, headers.length[0]) ||
        !ReadXiphLength(bytes, codecPrivateBytes, offset, headers.length[1])) return false;
    if (headers.length[0] > codecPrivateBytes - offset) return false;
    headers.data[0] = bytes + offset;
    offset += headers.length[0];
    if (headers.length[1] > codecPrivateBytes - offset) return false;
    headers.data[1] = bytes + offset;
    offset += headers.length[1];
    headers.data[2] = bytes + offset;
    headers.length[2] = codecPrivateBytes - offset;

    for (int i = 0; i != 3; ++i) {
        if (!headers.length[i] || headers.length[i] > static_cast<std::size_t>(LONG_MAX)) return false;
    }
    return true;
}

float FiniteSample(float sample) noexcept {
    return std::isfinite(sample) ? sample : 0.0f;
}

std::int32_t ToPcm16(float sample) noexcept {
    sample = std::clamp(FiniteSample(sample), -1.0f, 1.0f);
    if (sample <= -1.0f) return -32768;
    if (sample >= 1.0f) return 32767;
    return static_cast<std::int32_t>(std::lround(static_cast<double>(sample) * 32767.0));
}

std::int32_t ToPcm24(float sample) noexcept {
    sample = std::clamp(FiniteSample(sample), -1.0f, 1.0f);
    if (sample <= -1.0f) return -8388608;
    if (sample >= 1.0f) return 8388607;
    return static_cast<std::int32_t>(std::lround(static_cast<double>(sample) * 8388607.0));
}

unsigned int VorbisToWindowsChannel(unsigned int channels, unsigned int channel) noexcept {
    // Vorbis's standard multichannel order differs from interleaved Windows
    // PCM for 3, 5, 6, 7, and 8 channels.  This mirrors the documented
    // channel-position permutation without modifying the source planes.
    static constexpr unsigned int kThree[] = {0, 2, 1};
    static constexpr unsigned int kFive[] = {0, 2, 1, 3, 4};
    static constexpr unsigned int kSix[] = {0, 2, 1, 5, 3, 4};
    static constexpr unsigned int kSeven[] = {0, 2, 1, 6, 5, 3, 4};
    static constexpr unsigned int kEight[] = {0, 2, 1, 7, 5, 6, 3, 4};
    switch (channels) {
    case 3: return kThree[channel];
    case 5: return kFive[channel];
    case 6: return kSix[channel];
    case 7: return kSeven[channel];
    case 8: return kEight[channel];
    default: return channel;
    }
}

class VorbisAudioDecoder final : public matroska::AudioDecoder {
public:
    ~VorbisAudioDecoder() { Clear(); }

    bool Initialize(const matroska::TrackEntryData& track, unsigned int preferredBits,
        unsigned int maxChannels, bool floatingPoint) noexcept {
        try {
            constexpr std::size_t privateEnd = offsetof(matroska::TrackEntryData, codec_private_len) +
                sizeof(std::size_t);
            if (track.struct_size < privateEnd) return false;

            VorbisHeaders headers;
            if (!ParseVorbisHeaders(track.codec_private, track.codec_private_len, headers)) return false;

            vorbis_info_init(&info_);
            infoInitialized_ = true;
            vorbis_comment_init(&comment_);
            commentInitialized_ = true;
            for (int i = 0; i != 3; ++i) {
                ogg_packet packet{};
                packet.packet = const_cast<unsigned char*>(headers.data[i]);
                packet.bytes = static_cast<long>(headers.length[i]);
                packet.b_o_s = i == 0 ? 1 : 0;
                packet.packetno = i;
                if (vorbis_synthesis_headerin(&info_, &comment_, &packet) != 0) {
                    Clear();
                    return false;
                }
            }

            if (info_.channels < 1 || info_.channels > kMaximumChannels ||
                info_.rate < 1 || info_.rate > kMaximumRate) {
                Clear();
                return false;
            }

            sourceChannels_ = static_cast<unsigned int>(info_.channels);
            if (maxChannels == 0 || sourceChannels_ <= maxChannels) {
                outputChannels_ = sourceChannels_;
                foldToStereo_ = false;
            } else if (maxChannels >= 2) {
                outputChannels_ = 2;
                foldToStereo_ = true;
            } else {
                Clear();
                return false;
            }

            isFloat_ = floatingPoint;
            bitsPerSample_ = floatingPoint ? 32u : preferredBits == 24 ? 24u : 16u;
            bytesPerSample_ = bitsPerSample_ / 8;
            if (!bytesPerSample_ || outputChannels_ == 0 ||
                !Multiply(bytesPerSample_, outputChannels_, bytesPerFrame_)) {
                Clear();
                return false;
            }

            int maximumBlockFrames = vorbis_info_blocksize(&info_, 1);
            if (maximumBlockFrames <= 0 ||
                !Multiply(static_cast<std::size_t>(maximumBlockFrames), bytesPerFrame_, maximumPacketBytes_) ||
                maximumPacketBytes_ > kMaxPendingBytes) {
                Clear();
                return false;
            }

            if (vorbis_synthesis_init(&dsp_, &info_) != 0) {
                Clear();
                return false;
            }
            dspInitialized_ = true;
            if (vorbis_block_init(&dsp_, &block_) != 0) {
                Clear();
                return false;
            }
            blockInitialized_ = true;
            ready_ = true;
            return true;
        } catch (...) {
            Clear();
            return false;
        }
    }

    int _dispatch(int message, void* result, void** arguments, int argumentCount) override {
        switch (message) {
        case matroska::AudioDecoderMessage::OutputFrameSize: {
            std::size_t* frameSize = nullptr;
            const int status = ReadArgument(arguments, argumentCount, 0, frameSize) && frameSize && ready_
                ? OutputFrameSize(frameSize) : matroska::kDecodeFailure;
            WriteResult(result, status);
            return 1;
        }
        case matroska::AudioDecoderMessage::GetOutputProperties: {
            unsigned int* sampleRate = nullptr;
            unsigned int* channels = nullptr;
            unsigned int* bits = nullptr;
            bool* isFloat = nullptr;
            const bool valid = ReadArgument(arguments, argumentCount, 0, sampleRate) &&
                ReadArgument(arguments, argumentCount, 1, channels) &&
                ReadArgument(arguments, argumentCount, 2, bits) &&
                ReadArgument(arguments, argumentCount, 3, isFloat) &&
                sampleRate && channels && bits && isFloat;
            const int status = valid ? GetOutputProperties(sampleRate, channels, bits, isFloat)
                : matroska::kDecodeFailure;
            WriteResult(result, status);
            return 1;
        }
        case matroska::AudioDecoderMessage::DecodeBlock: {
            const void* input = nullptr;
            std::size_t inputBytes = 0;
            void* output = nullptr;
            std::size_t* outputBytes = nullptr;
            const bool valid = ReadArgument(arguments, argumentCount, 0, input) &&
                ReadArgument(arguments, argumentCount, 1, inputBytes) &&
                ReadArgument(arguments, argumentCount, 2, output) &&
                ReadArgument(arguments, argumentCount, 3, outputBytes) && outputBytes;
            const int status = valid ? DecodeBlock(input, inputBytes, output, outputBytes)
                : matroska::kDecodeFailure;
            WriteResult(result, status);
            return 1;
        }
        case matroska::AudioDecoderMessage::Flush:
            Flush();
            return 1;
        case matroska::AudioDecoderMessage::Close:
            delete this;
            return 1;
        case matroska::AudioDecoderMessage::EndOfStream:
            EndOfStream();
            return 1;
        default:
            return 0;
        }
    }

private:
    vorbis_info info_{};
    vorbis_comment comment_{};
    vorbis_dsp_state dsp_{};
    vorbis_block block_{};
    std::vector<unsigned char> pending_;
    unsigned int sourceChannels_ = 0;
    unsigned int outputChannels_ = 0;
    unsigned int bitsPerSample_ = 0;
    std::size_t bytesPerSample_ = 0;
    std::size_t bytesPerFrame_ = 0;
    std::size_t maximumPacketBytes_ = 0;
    ogg_int64_t packetNumber_ = 3;
    bool infoInitialized_ = false;
    bool commentInitialized_ = false;
    bool dspInitialized_ = false;
    bool blockInitialized_ = false;
    bool ready_ = false;
    bool foldToStereo_ = false;
    bool isFloat_ = false;
    bool ended_ = false;

    void Clear() noexcept {
        pending_.clear();
        if (blockInitialized_) vorbis_block_clear(&block_);
        if (dspInitialized_) vorbis_dsp_clear(&dsp_);
        if (commentInitialized_) vorbis_comment_clear(&comment_);
        if (infoInitialized_) vorbis_info_clear(&info_);
        blockInitialized_ = false;
        dspInitialized_ = false;
        commentInitialized_ = false;
        infoInitialized_ = false;
        ready_ = false;
        ended_ = false;
    }

    void RestartAfterError() noexcept {
        pending_.clear();
        if (dspInitialized_) (void)vorbis_synthesis_restart(&dsp_);
        ended_ = false;
    }

    float OutputSample(float** pcm, int frame, unsigned int outputChannel) const noexcept {
        if (!foldToStereo_) {
            return FiniteSample(pcm[VorbisToWindowsChannel(sourceChannels_, outputChannel)][frame]);
        }
        float value = outputChannel == 0 ? FiniteSample(pcm[0][frame]) : FiniteSample(pcm[1][frame]);
        // The additional Vorbis channels have no universally portable Matroska
        // speaker-mask guarantee.  Add them equally at -6 dB instead of
        // silently discarding content when the host accepts only stereo.
        for (unsigned int channel = 2; channel < sourceChannels_; ++channel) {
            value += 0.5f * FiniteSample(pcm[channel][frame]);
        }
        return value;
    }

    bool AppendFrames(float** pcm, int frames) {
        if (!pcm || frames <= 0 || !bytesPerFrame_) return false;
        for (unsigned int channel = 0; channel < sourceChannels_; ++channel) {
            if (!pcm[channel]) return false;
        }
        const std::size_t frameCount = static_cast<std::size_t>(frames);
        std::size_t byteCount = 0;
        if (!Multiply(frameCount, bytesPerFrame_, byteCount) || byteCount > kMaxPendingBytes ||
            pending_.size() > kMaxPendingBytes - byteCount) return false;

        const std::size_t offset = pending_.size();
        pending_.resize(offset + byteCount);
        unsigned char* output = pending_.data() + offset;
        for (int frame = 0; frame < frames; ++frame) {
            for (unsigned int channel = 0; channel < outputChannels_; ++channel) {
                const float sample = OutputSample(pcm, frame, channel);
                if (isFloat_) {
                    const float finite = FiniteSample(sample);
                    std::memcpy(output, &finite, sizeof(finite));
                } else if (bitsPerSample_ == 24) {
                    const std::uint32_t value = static_cast<std::uint32_t>(ToPcm24(sample));
                    output[0] = static_cast<unsigned char>(value);
                    output[1] = static_cast<unsigned char>(value >> 8);
                    output[2] = static_cast<unsigned char>(value >> 16);
                } else {
                    const std::int16_t value = static_cast<std::int16_t>(ToPcm16(sample));
                    std::memcpy(output, &value, sizeof(value));
                }
                output += bytesPerSample_;
            }
        }
        return true;
    }

    bool DecodeToPending(const void* input, std::size_t inputBytes) {
        if (!input || !inputBytes || inputBytes > kMaxAudioPacketBytes ||
            inputBytes > static_cast<std::size_t>(LONG_MAX) ||
            packetNumber_ == (std::numeric_limits<ogg_int64_t>::max)()) return false;
        ogg_packet packet{};
        packet.packet = const_cast<unsigned char*>(static_cast<const unsigned char*>(input));
        packet.bytes = static_cast<long>(inputBytes);
        packet.granulepos = -1;
        packet.packetno = packetNumber_++;
        if (vorbis_synthesis(&block_, &packet) != 0 || vorbis_synthesis_blockin(&dsp_, &block_) != 0) return false;

        for (;;) {
            float** pcm = nullptr;
            const int frames = vorbis_synthesis_pcmout(&dsp_, &pcm);
            if (frames < 0) return false;
            if (!frames) return true;
            if (!AppendFrames(pcm, frames) || vorbis_synthesis_read(&dsp_, frames) != 0) return false;
        }
    }

    int TransferPending(void* output, std::size_t capacity, std::size_t* outputBytes) noexcept {
        const std::size_t required = pending_.size();
        if (!required) {
            *outputBytes = 0;
            return matroska::kDecodeNeedMoreInput;
        }
        if (!output || capacity < required) {
            *outputBytes = required;
            return matroska::kDecodeFailure;
        }
        std::memcpy(output, pending_.data(), required);
        pending_.clear();
        *outputBytes = required;
        return matroska::kDecodeSuccess;
    }

    int OutputFrameSize(std::size_t* frameSize) const noexcept {
        if (!ready_ || !frameSize || !maximumPacketBytes_) return matroska::kDecodeFailure;
        // in_mkv allocates its destination once from this value, so this is the
        // largest output a single legal Vorbis packet can produce, not merely
        // the byte stride of one interleaved PCM frame.
        *frameSize = maximumPacketBytes_;
        return matroska::kDecodeSuccess;
    }

    int GetOutputProperties(unsigned int* sampleRate, unsigned int* channels, unsigned int* bits,
        bool* isFloat) const noexcept {
        if (!ready_ || !sampleRate || !channels || !bits || !isFloat) return matroska::kDecodeFailure;
        *sampleRate = static_cast<unsigned int>(info_.rate);
        *channels = outputChannels_;
        *bits = bitsPerSample_;
        *isFloat = isFloat_;
        return matroska::kDecodeSuccess;
    }

    int DecodeBlock(const void* input, std::size_t inputBytes, void* output,
        std::size_t* outputBytes) noexcept {
        if (!outputBytes) return matroska::kDecodeFailure;
        const std::size_t capacity = *outputBytes;
        *outputBytes = 0;
        if (!ready_) return matroska::kDecodeFailure;
        if (!pending_.empty()) return TransferPending(output, capacity, outputBytes);
        if (ended_) return matroska::kDecodeFailure;
        if (!input || !inputBytes) return matroska::kDecodeNeedMoreInput;

        try {
            if (!DecodeToPending(input, inputBytes)) {
                RestartAfterError();
                return matroska::kDecodeFailure;
            }
            return TransferPending(output, capacity, outputBytes);
        } catch (...) {
            RestartAfterError();
            return matroska::kDecodeFailure;
        }
    }

    void Flush() noexcept {
        pending_.clear();
        if (dspInitialized_) (void)vorbis_synthesis_restart(&dsp_);
        ended_ = false;
    }

    void EndOfStream() noexcept { ended_ = true; }
};

class VorbisDecoderService final : public matroska::DecoderService {
public:
    int _dispatch(int message, void* result, void** arguments, int argumentCount) override {
        switch (message) {
        case matroska::DecoderServiceMessage::CreateAudioDecoder: {
            const char* codecId = nullptr;
            const matroska::TrackEntryData* track = nullptr;
            const matroska::AudioData* audio = nullptr;
            unsigned int preferredBits = 0;
            unsigned int maxChannels = 0;
            bool floatingPoint = false;
            matroska::AudioDecoder** decoder = nullptr;
            const bool valid = ReadArgument(arguments, argumentCount, 0, codecId) &&
                ReadArgument(arguments, argumentCount, 1, track) &&
                ReadArgument(arguments, argumentCount, 2, audio) &&
                ReadArgument(arguments, argumentCount, 3, preferredBits) &&
                ReadArgument(arguments, argumentCount, 4, maxChannels) &&
                ReadArgument(arguments, argumentCount, 5, floatingPoint) &&
                ReadArgument(arguments, argumentCount, 6, decoder) && decoder;
            const int status = valid ? CreateAudioDecoder(codecId, track, audio, preferredBits,
                maxChannels, floatingPoint, decoder) : matroska::kCreateDecoderFailure;
            WriteResult(result, status);
            return 1;
        }
        case matroska::DecoderServiceMessage::CreateVideoDecoder:
            WriteResult(result, matroska::kCreateDecoderNotMine);
            return 1;
        default:
            return 0;
        }
    }

private:
    static int CreateAudioDecoder(const char* codecId, const matroska::TrackEntryData* track,
        const matroska::AudioData* audio, unsigned int preferredBits, unsigned int maxChannels,
        bool floatingPoint, matroska::AudioDecoder** decoder) noexcept {
        *decoder = nullptr;
        if (!codecId || std::strcmp(codecId, "A_VORBIS") != 0) return matroska::kCreateDecoderNotMine;
        if (!track || !audio) return matroska::kCreateDecoderFailure;
        auto* instance = new (std::nothrow) VorbisAudioDecoder;
        if (!instance) return matroska::kCreateDecoderFailure;
        if (!instance->Initialize(*track, preferredBits, maxChannels, floatingPoint)) {
            delete instance;
            return matroska::kCreateDecoderFailure;
        }
        *decoder = instance;
        return matroska::kCreateDecoderSuccess;
    }
};

VorbisDecoderService g_decoderService;

class VorbisMatroskaServiceFactory final : public matroska::ServiceFactory {
public:
    int _dispatch(int message, void* result, void** arguments, int argumentCount) override {
        switch (message) {
        case matroska::ServiceFactoryMessage::GetServiceType:
            WriteResult(result, matroska::kDecoderServiceType);
            return 1;
        case matroska::ServiceFactoryMessage::GetServiceName: {
            const char* name = kServiceName;
            WriteResult(result, name);
            return 1;
        }
        case matroska::ServiceFactoryMessage::GetGuid:
            WriteResult(result, kIndependentVorbisMatroskaGuid);
            return 1;
        case matroska::ServiceFactoryMessage::GetInterface: {
            // The historical factory takes an optional global-lock flag.  This
            // static creator needs no lock, but consuming the argument keeps
            // the ABI layout identical for callers that pass it.
            int ignoredGlobalLock = 1;
            if (argumentCount > 0 && !ReadArgument(arguments, argumentCount, 0, ignoredGlobalLock)) {
                WriteResult<void*>(result, nullptr);
                return 1;
            }
            void* service = static_cast<matroska::DecoderService*>(&g_decoderService);
            WriteResult(result, service);
            return 1;
        }
        case matroska::ServiceFactoryMessage::SupportsNonLockingInterface:
            WriteResult(result, 1);
            return 1;
        case matroska::ServiceFactoryMessage::ReleaseInterface: {
            void* ignored = nullptr;
            const int status = ReadArgument(arguments, argumentCount, 0, ignored) ? 1 : 0;
            WriteResult(result, status);
            return 1;
        }
        case matroska::ServiceFactoryMessage::GetTestString: {
            const char* text = kServiceTestString;
            WriteResult(result, text);
            return 1;
        }
        case matroska::ServiceFactoryMessage::ServiceNotify:
            WriteResult(result, 1);
            return 1;
        default:
            return 0;
        }
    }
};

VorbisMatroskaServiceFactory g_serviceFactory;
std::mutex g_registrationMutex;
matroska::ServiceManager* g_registeredManager = nullptr;
bool g_registered = false;

} // namespace

void RegisterMatroska(HWND host) noexcept {
    if (!host) return;
    try {
        std::lock_guard<std::mutex> lock(g_registrationMutex);
        if (g_registered) return;
        const LRESULT reply = SendMessageW(host, matroska::kServiceManagerMessage, 0,
            matroska::kGetApiService);
        if (reply == 0 || reply == 1) return;
        auto* manager = reinterpret_cast<matroska::ServiceManager*>(
            static_cast<std::uintptr_t>(reply));
        if (!manager) return;

        void* factory = static_cast<matroska::ServiceFactory*>(&g_serviceFactory);
        void* arguments[] = {&factory};
        int registration = 0;
        if (manager->_dispatch(matroska::ServiceManagerMessage::Register, &registration,
                arguments, 1) && registration) {
            g_registeredManager = manager;
            g_registered = true;
        }
    } catch (...) {
        // A host callback is outside this module's control.  Registration must
        // never let an exception cross Winamp's C ABI boundary.
    }
}

void UnregisterMatroska() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_registrationMutex);
        if (!g_registered || !g_registeredManager) return;
        auto* manager = g_registeredManager;
        g_registeredManager = nullptr;
        g_registered = false;
        void* factory = static_cast<matroska::ServiceFactory*>(&g_serviceFactory);
        void* arguments[] = {&factory};
        int ignored = 0;
        (void)manager->_dispatch(matroska::ServiceManagerMessage::Deregister, &ignored,
            arguments, 1);
    } catch (...) {
        g_registeredManager = nullptr;
        g_registered = false;
    }
}

} // namespace ogg_input
