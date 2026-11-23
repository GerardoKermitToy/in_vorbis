// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "settings.hpp"
#include <array>
#include <cstdint>

namespace ogg_input {

// Standard Windows speaker-bit order for 1..8 channels. The seven-channel
// layout is FL FR FC LFE BC SL SR; eight channels are FL FR FC LFE BL BR SL SR.
// Invalid source counts return zero. Unknown modes behave as Source.
int OutputChannels(ChannelMode mode, int source_channels) noexcept;

// Source and LegacyUnmapped copy without reordering. Other modes expect Windows
// order. Normally output_channels is OutputChannels(mode, source_channels);
// an explicitly different count truncates or zero-pads the selected matrix.
// Source/destination may alias. Nonfinite inputs/gain become silence; finite
// overflow is bounded to the float range, not clipped to PCM full scale.
// Plain stereo retains the normalized, LFE-excluding playback matrix. Mono is
// the arithmetic mean including LFE; quad adds (C+LFE)/sqrt(2) to each front
// without normalization. Seven-channel quad shares BC equally between rears;
// eight-channel quad averages back+side per rear output.
// Surround/SurroundII are unnormalized amplitude matrices, NOT phase-quadrature
// encoders or bit-exact reproductions of historical processors.
void MixFrame(const float* windows_order, int source_channels, float* dst,
              int output_channels, ChannelMode mode, double gain) noexcept;

// Stateful, interleaved PCM quantization; frames is a frame count, not a sample
// count. PCM8 is unsigned; PCM16/24/32 is signed little-endian. Round-to-nearest
// ties go toward +infinity. No allocation or locking occurs in Pack or Reset.
// Each instance/channel has a fixed, independent pseudo-random stream; Reset
// restores both seeds and histories. The dither switch disables shaping too.
// Rectangular U(-.5,.5), triangular U(0,1)-U(0,1), and Gaussian sigma=sqrt(1/6)
// are in output-LSB units. Dither is supported at all four integer bit depths.
//
// ATH is an independently derived, finite-order psychoacoustic approximation,
// not a coefficient-table clone: see the analytic design in processing.cpp.
// Ath supports 22050/32000/37800/44100/48000 Hz; AthLow (lower noise gain) supports
// 44100/48000 Hz. Unsupported rates fall back to FirstOrder. EffectiveShape is
// None when dither is disabled. Invalid channel counts make Pack a no-op;
// invalid bit depths fall back to 16. Nonfinite samples become silence. PCM
// overload clears that channel's feedback history to avoid overload ringing.
class Quantizer {
public:
    Quantizer(const Settings& settings, int channels, int rate);
    void Pack(void* target, const float* input, int frames) noexcept;
    void Reset() noexcept;
    NoiseShape EffectiveShape() const noexcept { return shape_; }

private:
    static constexpr int kMaxChannels = 8;
    static constexpr int kOrder = 12;
    struct ChannelState {
        std::uint64_t rng = 0;
        std::array<double, kOrder> error{};
    };

    void DesignAth(int rate) noexcept;
    double Uniform(ChannelState& channel) noexcept;
    double Dither(ChannelState& channel) noexcept;

    int channels_ = 0;
    int bits_ = 16;
    int order_ = 0;
    bool dither_ = false;
    DitherPdf pdf_ = DitherPdf::Triangular;
    NoiseShape shape_ = NoiseShape::None;
    std::array<double, kOrder> feedback_{};
    std::array<ChannelState, kMaxChannels> state_{};
};

} // namespace ogg_input
