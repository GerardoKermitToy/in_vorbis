// SPDX-License-Identifier: GPL-3.0-only
#include "processing.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ogg_input {
namespace {
constexpr double kRootHalf = 0.7071067811865475244;
constexpr double kPi = 3.14159265358979323846;

bool AthRate(int rate) noexcept {
    return rate == 22050 || rate == 32000 || rate == 37800 ||
           rate == 44100 || rate == 48000;
}

float FiniteFloat(double sample, double gain) noexcept {
    constexpr double limit = static_cast<double>((std::numeric_limits<float>::max)());
    return static_cast<float>(std::clamp(sample * gain, -limit, limit));
}
} // namespace

int OutputChannels(ChannelMode mode, int source_channels) noexcept {
    if (source_channels < 1 || source_channels > 8) return 0;
    switch (mode) {
    case ChannelMode::Stereo:
    case ChannelMode::Surround:
    case ChannelMode::SurroundII: return 2;
    case ChannelMode::Mono: return 1;
    case ChannelMode::Quad: return source_channels >= 4 ? 4 : source_channels;
    default: return source_channels;
    }
}

void MixFrame(const float* windows_order, int source_channels, float* dst,
              int output_channels, ChannelMode mode, double gain) noexcept {
    if (!dst || output_channels < 1 || output_channels > 8) return;
    if (!windows_order || source_channels < 1 || source_channels > 8) {
        std::fill_n(dst, output_channels, 0.f);
        return;
    }
    // Copy before writing so in-place mixing is also well-defined.
    std::array<double, 8> in{}, mixed{};
    for (int c = 0; c < source_channels; ++c)
        in[c] = std::isfinite(windows_order[c]) ? windows_order[c] : 0.;
    if (!std::isfinite(gain)) gain = 0.;
    const int selected_channels = OutputChannels(mode, source_channels);
    switch (mode) {
    case ChannelMode::Stereo: {
        double left = in[0], right = source_channels > 1 ? in[1] : in[0];
        double weight = 1.;
        // Preserve the existing normalized stereo matrix, including its LFE
        // exclusion. Each row's absolute coefficient sum is one.
        switch (source_channels) {
        case 3:
            left += kRootHalf * in[2]; right += kRootHalf * in[2];
            weight += kRootHalf; break;
        case 4:
            left += kRootHalf * in[2]; right += kRootHalf * in[3];
            weight += kRootHalf; break;
        case 5:
            left += kRootHalf * (in[2] + in[3]);
            right += kRootHalf * (in[2] + in[4]);
            weight += 2 * kRootHalf; break;
        case 6:
            left += kRootHalf * (in[2] + in[4]);
            right += kRootHalf * (in[2] + in[5]);
            weight += 2 * kRootHalf; break;
        case 7:
            left += kRootHalf * (in[2] + in[5]) + .5 * in[4];
            right += kRootHalf * (in[2] + in[6]) + .5 * in[4];
            weight += 2 * kRootHalf + .5; break;
        case 8:
            left += kRootHalf * (in[2] + in[4] + in[6]);
            right += kRootHalf * (in[2] + in[5] + in[7]);
            weight += 3 * kRootHalf; break;
        default: break;
        }
        mixed[0] = left / weight;
        mixed[1] = right / weight;
        break;
    }
    case ChannelMode::Mono:
        // Arithmetic mean, deliberately including LFE and every rear channel.
        for (int c = 0; c < source_channels; ++c)
            mixed[0] += in[c] / source_channels;
        break;
    case ChannelMode::Quad:
        if (source_channels < 4) {
            std::copy_n(in.begin(), source_channels, mixed.begin());
        } else if (source_channels == 4) {
            std::copy_n(in.begin(), 4, mixed.begin());
        } else {
            const double common = kRootHalf * (in[2] + (source_channels >= 6 ? in[3] : 0.));
            mixed[0] = in[0] + common;
            mixed[1] = in[1] + common;
            if (source_channels == 5) {
                mixed[2] = in[3]; mixed[3] = in[4];
            } else if (source_channels == 6) {
                mixed[2] = in[4]; mixed[3] = in[5];
            } else if (source_channels == 7) {
                mixed[2] = in[5] + .5 * in[4];
                mixed[3] = in[6] + .5 * in[4];
            } else {
                mixed[2] = .5 * (in[4] + in[6]);
                mixed[3] = .5 * (in[5] + in[7]);
            }
        }
        break;
    case ChannelMode::Surround:
    case ChannelMode::SurroundII: {
        double centre = 0., lfe = 0., rear_left = 0., rear_right = 0.;
        if (source_channels >= 3 && source_channels != 4) centre = in[2];
        if (source_channels >= 6) lfe = in[3];
        switch (source_channels) {
        case 4: rear_left = in[2]; rear_right = in[3]; break;
        case 5: rear_left = in[3]; rear_right = in[4]; break;
        case 6: rear_left = in[4]; rear_right = in[5]; break;
        case 7:
            rear_left = in[5] + .5 * in[4];
            rear_right = in[6] + .5 * in[4]; break;
        case 8:
            rear_left = in[4] + in[6];
            rear_right = in[5] + in[7]; break;
        default: break;
        }
        // +/-1.5 dB are amplitude ratios, derived rather than inherited table
        // entries. II preserves a different left/right rear ratio per side.
        const double near = mode == ChannelMode::SurroundII ? std::pow(10., 1.5 / 20.) : 1.;
        const double far = mode == ChannelMode::SurroundII ? std::pow(10., -1.5 / 20.) : 1.;
        mixed[0] = in[0] + kRootHalf * (centre + lfe - near * rear_left - far * rear_right);
        mixed[1] = (source_channels > 1 ? in[1] : in[0]) +
                   kRootHalf * (centre + lfe + far * rear_left + near * rear_right);
        break;
    }
    default:
        // Codec/native pass-through: caller alone is responsible for whether
        // the input was Windows-reordered or retained in raw codec order.
        std::copy_n(in.begin(), source_channels, mixed.begin());
        break;
    }
    for (int c = 0; c < output_channels; ++c)
        dst[c] = c < selected_channels ? FiniteFloat(mixed[c], gain) : 0.f;
}

Quantizer::Quantizer(const Settings& settings, int channels, int rate)
    : channels_(channels >= 1 && channels <= kMaxChannels ? channels : 0),
      bits_(settings.bits == 8 || settings.bits == 16 || settings.bits == 24 ||
            settings.bits == 32 ? settings.bits : 16),
      dither_(settings.dither), pdf_(settings.dither_pdf) {
    if (pdf_ != DitherPdf::Rectangular && pdf_ != DitherPdf::Triangular &&
        pdf_ != DitherPdf::Gaussian) pdf_ = DitherPdf::Triangular;
    if (dither_) {
        switch (settings.noise_shape) {
        case NoiseShape::None: break;
        case NoiseShape::FirstOrder: shape_ = NoiseShape::FirstOrder; break;
        case NoiseShape::Ath:
            shape_ = AthRate(rate) ? NoiseShape::Ath : NoiseShape::FirstOrder;
            break;
        case NoiseShape::AthLow:
            shape_ = rate == 44100 || rate == 48000 ? NoiseShape::AthLow : NoiseShape::FirstOrder;
            break;
        default: break;
        }
    }
    if (shape_ == NoiseShape::FirstOrder) {
        order_ = 1;
        feedback_[0] = -1.; // NTF(z) = 1-z^-1, triangular noise-power spectrum.
    } else if (shape_ == NoiseShape::Ath || shape_ == NoiseShape::AthLow) {
        DesignAth(rate);
    }
    Reset();
}

void Quantizer::DesignAth(int rate) noexcept {
    // Independent analytic design. The commonly published absolute threshold
    // in quiet approximation (f in kHz, threshold in dB SPL) is
    // T(f) = 3.64 f^-0.8 - 6.5 exp[-0.6(f-3.3)^2] + 0.001 f^4.
    // These constants describe a hearing model, not a quantizer coefficient
    // table. Sample that formula on an even spectrum, bound its enormous DC/
    // ultrasonic values, then take a real cepstrum. A causal minimum-phase
    // log-spectrum is 2*c[n] for n>0; discard c[0] to obtain h[0]=1. Convert its
    // exponential to a 12th-order FIR by the power-series recurrence below.
    // A raised-cosine cepstral window smooths the truncated approximation.
    // AthLow uses half the log-spectral contrast and a lower feedback-gain cap
    // to reduce total noise. Neither mode claims calibrated SPL or historical
    // bit equivalence; both are sample-rate-specific perceptual weighting.
    constexpr int grid = 512;
    std::array<double, grid> log_magnitude{};
    std::array<double, kOrder + 1> log_filter{}, impulse{};
    const double contrast = shape_ == NoiseShape::AthLow ? .25 : .5;
    for (int bin = 0; bin < grid; ++bin) {
        const int folded = (std::min)(bin, grid - bin);
        const double f = (std::max)(.1, folded * static_cast<double>(rate) / (grid * 1000.));
        const double delta = f - 3.3;
        const double threshold = 3.64 * std::pow(f, -.8) -
                                 6.5 * std::exp(-.6 * delta * delta) + .001 * f * f * f * f;
        log_magnitude[bin] = contrast * std::log(10.) / 20. * std::clamp(threshold, -8., 24.);
    }
    for (int lag = 1; lag <= kOrder; ++lag) {
        double cepstrum = 0.;
        for (int bin = 0; bin < grid; ++bin)
            cepstrum += log_magnitude[bin] * std::cos(2 * kPi * lag * bin / grid);
        const double window = .5 + .5 * std::cos(kPi * lag / (kOrder + 1));
        log_filter[lag] = 2 * cepstrum / grid * window;
    }
    impulse[0] = 1.;
    double absolute_sum = 0.;
    for (int n = 1; n <= kOrder; ++n) {
        for (int k = 1; k <= n; ++k)
            impulse[n] += k * log_filter[k] * impulse[n - k] / n;
        if (!std::isfinite(impulse[n])) {
            feedback_.fill(0.);
            feedback_[0] = -1.;
            order_ = 1;
            shape_ = NoiseShape::FirstOrder;
            return;
        }
        feedback_[n - 1] = impulse[n];
        absolute_sum += std::abs(impulse[n]);
    }
    const double limit = shape_ == NoiseShape::AthLow ? 1.25 : 2.5;
    if (absolute_sum > limit)
        for (double& coefficient : feedback_) coefficient *= limit / absolute_sum;
    order_ = kOrder;
}

void Quantizer::Reset() noexcept {
    for (int c = 0; c < kMaxChannels; ++c) {
        // SplitMix64's reversible mixing provides distinct nonzero seeds for
        // separate xorshift64* streams. No process-wide random state is used.
        std::uint64_t seed = UINT64_C(0x6a09e667f3bcc909) +
                             UINT64_C(0x9e3779b97f4a7c15) * (static_cast<std::uint64_t>(c) + 1);
        seed = (seed ^ (seed >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        seed = (seed ^ (seed >> 27)) * UINT64_C(0x94d049bb133111eb);
        seed ^= seed >> 31;
        state_[c].rng = seed ? seed : 1;
        state_[c].error.fill(0.);
    }
}

double Quantizer::Uniform(ChannelState& channel) noexcept {
    auto x = channel.rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    channel.rng = x;
    const auto bits = (x * UINT64_C(0x2545f4914f6cdd1d)) >> 11;
    return static_cast<double>(bits) * (1. / 9007199254740992.);
}

double Quantizer::Dither(ChannelState& channel) noexcept {
    switch (pdf_) {
    case DitherPdf::Rectangular: return Uniform(channel) - .5;
    case DitherPdf::Gaussian: {
        // Box-Muller, no shared/cached second variate. 1-U is in (0,1],
        // avoiding log(0). Finite 53-bit uniforms bound even the Gaussian tail.
        const double u = 1. - Uniform(channel);
        const double v = Uniform(channel);
        return std::sqrt(-2 * std::log(u) / 6.) * std::cos(2 * kPi * v);
    }
    default: {
        const double first = Uniform(channel);
        return first - Uniform(channel);
    }
    }
}

void Quantizer::Pack(void* target, const float* input, int frames) noexcept {
    if (!target || !input || frames <= 0 || channels_ == 0) return;
    auto* dst = static_cast<unsigned char*>(target);
    const double scale = std::ldexp(1., bits_ - 1);
    const double maximum = scale - 1.;
    for (int frame = 0; frame < frames; ++frame) {
        for (int c = 0; c < channels_; ++c) {
            auto& channel = state_[c];
            const float sample = *input++;
            if (!std::isfinite(sample)) channel.error.fill(0.);
            const double signal = std::isfinite(sample) ? static_cast<double>(sample) * scale : 0.;
            double code = 0.;
            if (signal <= -scale || signal >= maximum) {
                code = signal <= -scale ? -scale : maximum;
                channel.error.fill(0.);
            } else {
                double shaped = signal;
                for (int lag = 0; lag < order_; ++lag)
                    shaped += feedback_[lag] * channel.error[lag];
                const double rounded = std::floor(shaped + (dither_ ? Dither(channel) : 0.) + .5);
                code = std::clamp(rounded, -scale, maximum);
                if (rounded != code) {
                    channel.error.fill(0.);
                } else if (order_) {
                    for (int lag = order_ - 1; lag > 0; --lag)
                        channel.error[lag] = channel.error[lag - 1];
                    // Error includes non-subtractive dither, so both dither
                    // and rounding noise are shaped by 1+sum(h[k]z^-k).
                    // Gaussian's finite-uniform maximum plus rounding is
                    // below 4.5 LSB; retain an explicit hard history bound.
                    channel.error[0] = std::clamp(code - shaped, -4.5, 4.5);
                }
            }
            const auto integer = static_cast<std::int64_t>(code);
            if (bits_ == 8) {
                *dst++ = static_cast<unsigned char>(integer + 128);
            } else {
                const auto encoded = static_cast<std::uint64_t>(integer);
                for (int byte = 0; byte < bits_ / 8; ++byte)
                    *dst++ = static_cast<unsigned char>(encoded >> (8 * byte));
            }
        }
    }
}
} // namespace ogg_input
