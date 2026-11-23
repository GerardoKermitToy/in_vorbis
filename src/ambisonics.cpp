// SPDX-License-Identifier: GPL-3.0-only
#include "ambisonics.hpp"
#include <algorithm>
#include <cmath>

namespace ogg_input {
namespace {
constexpr double Pi = 3.14159265358979323846;
// Real spherical harmonics, ACN index l*(l+1)+m, SN3D normalization,
// no Condon-Shortley phase. Axes: X forward, Y left, Z up.
double Harmonic(int l, int m, double azimuth, double elevation) noexcept {
    const int a = std::abs(m);
    const double z = std::sin(elevation);
    double p = 1;
    for (int i = 1; i <= a; ++i) p *= (2 * i - 1) * std::sqrt(std::max(0., 1 - z * z));
    if (l > a) {
        double previous = p;
        p *= (2 * a + 1) * z;
        for (int n = a + 2; n <= l; ++n) {
            const double next = ((2 * n - 1) * z * p - (n + a - 1) * previous) / (n - a);
            previous = p;
            p = next;
        }
    }
    double ratio = 1;
    for (int n = l - a + 1; n <= l + a; ++n) ratio /= n;
    const double normalization = std::sqrt((a ? 2. : 1.) * ratio);
    const double phase = m < 0 ? std::sin(a * azimuth) : std::cos(a * azimuth);
    return normalization * p * phase;
}
}

bool AmbisonicShape(int channels, int& order, int& harmonics) noexcept {
    order = harmonics = 0;
    // The largest complete order representable by the 8-bit OpusHead count.
    for (int n = 1; n <= 15; ++n) {
        const int square = n * n;
        if (channels == square || channels == square + 2) {
            order = n - 1;
            harmonics = square;
            return true;
        }
    }
    return false;
}

bool AmbisonicRenderer::Init(int channels, int rate, AmbisonicMode mode) {
    int order = 0;
    if (!AmbisonicShape(channels, order, harmonics_) || (rate != 48000 && rate != 96000)) return false;
    source_channels_ = channels;
    headphones_ = mode == AmbisonicMode::Headphones;
    std::array<double, 8> azimuth{}, elevation{};
    switch (mode) {
    case AmbisonicMode::Stereo:
        output_channels_ = speakers_ = 2; azimuth = {30, -30}; break;
    case AmbisonicMode::Headphones:
        output_channels_ = 2; speakers_ = 8;
        azimuth = {30, -30, 90, -90, 150, -150, 45, -45};
        elevation = {0, 0, 0, 0, 0, 0, 45, 45}; break;
    case AmbisonicMode::Quad:
        output_channels_ = speakers_ = 4; azimuth = {45, -45, 135, -135}; break;
    case AmbisonicMode::Surround51:
        output_channels_ = speakers_ = 6; azimuth = {30, -30, 0, 0, 110, -110}; break;
    case AmbisonicMode::Surround71:
        output_channels_ = speakers_ = 8; azimuth = {30, -30, 0, 0, 150, -150, 90, -90}; break;
    default: return false;
    }
    matrix_.assign(static_cast<std::size_t>(speakers_) * harmonics_, 0);
    const bool lfe = !headphones_ && output_channels_ >= 6;
    const int active = speakers_ - (lfe ? 1 : 0);
    // A bounded, cosine-tapered sampling decoder. It deliberately makes no
    // promise of an exact inverse for underspecified/underdetermined arrays.
    const double scale = 1. / ((order + 1) * std::sqrt(static_cast<double>(active)));
    for (int speaker = 0; speaker < speakers_; ++speaker) {
        if (lfe && speaker == 3) continue; // Never synthesize an LFE signal.
        const double az = azimuth[speaker] * Pi / 180;
        const double el = elevation[speaker] * Pi / 180;
        for (int l = 0; l <= order; ++l) {
            const double weight = std::cos(Pi * l / (2 * (order + 1)));
            for (int m = -l; m <= l; ++m) {
                const int acn = l * (l + 1) + m;
                matrix_[static_cast<std::size_t>(speaker) * harmonics_ + acn] =
                    static_cast<float>(scale * weight * Harmonic(l, m, az, el));
            }
        }
        const double side = std::sin(az) * std::cos(el);
        left_[speaker] = side >= 0;
        itd_[speaker] = static_cast<float>(std::abs(side) * .00065 * rate);
        const double cutoff = 1800. + 6000. * (1 - std::abs(side));
        shadow_alpha_[speaker] = static_cast<float>(1 - std::exp(-2 * Pi * cutoff / rate));
    }
    delay_length_ = static_cast<int>(std::ceil(.00065 * rate)) + 2;
    delay_.assign(static_cast<std::size_t>(speakers_) * delay_length_, 0);
    shadow_state_.fill(0);
    delay_at_ = 0;
    return true;
}

void AmbisonicRenderer::Render(const float* source, float* output, int frames) noexcept {
    for (int frame = 0; frame < frames; ++frame) {
        const float* input = source + static_cast<std::size_t>(frame) * source_channels_;
        float* out = output + static_cast<std::size_t>(frame) * output_channels_;
        std::fill_n(out, output_channels_, 0.f);
        for (int speaker = 0; speaker < speakers_; ++speaker) {
            const float* row = matrix_.data() + static_cast<std::size_t>(speaker) * harmonics_;
            double sum = 0;
            for (int channel = 0; channel < harmonics_; ++channel) sum += row[channel] * input[channel];
            const float sample = static_cast<float>(sum);
            if (!headphones_) { out[speaker] = sample; continue; }
            float* ring = delay_.data() + static_cast<std::size_t>(speaker) * delay_length_;
            ring[delay_at_] = sample;
            const int whole = static_cast<int>(itd_[speaker]);
            const float fraction = itd_[speaker] - whole;
            const int a = (delay_at_ - whole + delay_length_) % delay_length_;
            const int b = (a - 1 + delay_length_) % delay_length_;
            const float delayed = ring[a] * (1 - fraction) + ring[b] * fraction;
            float& shadow = shadow_state_[speaker];
            shadow += shadow_alpha_[speaker] * (delayed - shadow);
            const int near_ear = left_[speaker] ? 0 : 1;
            out[near_ear] += .85f * sample;
            out[1 - near_ear] += .5f * shadow;
        }
        if (source_channels_ == harmonics_ + 2) {
            // Non-diegetic stereo is already listener-relative: route directly,
            // not through the spherical-harmonic decoder or head-shadow model.
            out[0] += input[harmonics_];
            out[1] += input[harmonics_ + 1];
        }
        if (++delay_at_ == delay_length_) delay_at_ = 0;
    }
}
}
