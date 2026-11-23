// SPDX-License-Identifier: GPL-3.0-only
#include "processing.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ogg_input;
namespace {
constexpr double k = 0.7071067811865475244;
constexpr std::array<int, 4> depths{8, 16, 24, 32};
constexpr std::array<DitherPdf, 3> pdfs{
    DitherPdf::Rectangular, DitherPdf::Triangular, DitherPdf::Gaussian};
constexpr std::array<NoiseShape, 4> shapes{
    NoiseShape::None, NoiseShape::FirstOrder, NoiseShape::Ath, NoiseShape::AthLow};
constexpr std::array<ChannelMode, 7> modes{
    ChannelMode::Source, ChannelMode::Stereo, ChannelMode::Mono, ChannelMode::Quad,
    ChannelMode::LegacyUnmapped, ChannelMode::Surround, ChannelMode::SurroundII};

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Close(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 2e-6 * (1 + std::abs(expected)))
        throw std::runtime_error(message);
}
std::int64_t Decode(const unsigned char* pcm, int bits) {
    if (bits == 8) return static_cast<int>(pcm[0]) - 128;
    std::uint64_t value = 0;
    for (int b = 0; b < bits / 8; ++b) value |= static_cast<std::uint64_t>(pcm[b]) << (8 * b);
    const auto signed_value = static_cast<std::int64_t>(value);
    return (value & (UINT64_C(1) << (bits - 1))) ?
           signed_value - (INT64_C(1) << bits) : signed_value;
}
std::vector<unsigned char> Packed(Quantizer& quantizer, const std::vector<float>& input,
                                  int channels, int bits) {
    std::vector<unsigned char> result(input.size() * static_cast<std::size_t>(bits / 8));
    quantizer.Pack(result.data(), input.data(), static_cast<int>(input.size() / channels));
    return result;
}

void PackingAndBounds() {
    const float nan = (std::numeric_limits<float>::quiet_NaN)();
    const float inf = (std::numeric_limits<float>::infinity)();
    const float extreme = (std::numeric_limits<float>::max)();
    const std::vector<float> input{-2, -1, -.5f, 0, .5f, 1, 2, nan, inf, -inf, extreme, -extreme};
    for (int bits : depths) {
        Settings settings;
        settings.bits = bits;
        settings.dither = false;
        Quantizer quantizer(settings, 1, 48000);
        Check(quantizer.EffectiveShape() == NoiseShape::None, "disabled dither disables shaping");
        const auto scale = INT64_C(1) << (bits - 1);
        const std::array<std::int64_t, 12> expected{
            -scale, -scale, -scale / 2, 0, scale / 2, scale - 1, scale - 1,
            0, 0, 0, scale - 1, -scale};
        constexpr std::size_t guard = 19;
        const std::size_t size = input.size() * static_cast<std::size_t>(bits / 8);
        std::vector<unsigned char> buffer(guard + size + guard, 0xa5);
        quantizer.Pack(buffer.data() + guard, input.data(), static_cast<int>(input.size()));
        Check(std::all_of(buffer.begin(), buffer.begin() + guard, [](auto x) { return x == 0xa5; }) &&
              std::all_of(buffer.end() - guard, buffer.end(), [](auto x) { return x == 0xa5; }),
              "Pack writes exactly frames*channels*bytes");
        for (std::size_t i = 0; i < input.size(); ++i)
            Check(Decode(buffer.data() + guard + i * (bits / 8), bits) == expected[i],
                  "all PCM depths, signedness, finite handling and saturation");
        if (bits == 8) {
            Check(buffer[guard] == 0 && buffer[guard + 3] == 128 && buffer[guard + 5] == 255,
                  "8-bit PCM uses unsigned offset binary");
        } else {
            for (int b = 0; b < bits / 8; ++b)
                Check(buffer[guard + 5 * (bits / 8) + b] == (b == bits / 8 - 1 ? 0x7f : 0xff),
                      "signed PCM positive rail is packed little-endian");
        }
        const std::vector<float> ties{
            static_cast<float>(.5 / scale), static_cast<float>(-.5 / scale),
            static_cast<float>(1.5 / scale), static_cast<float>(-1.5 / scale)};
        const auto tied = Packed(quantizer, ties, 1, bits);
        const std::array<std::int64_t, 4> rounded{1, 0, 2, -1};
        for (int i = 0; i < 4; ++i)
            Check(Decode(tied.data() + i * (bits / 8), bits) == rounded[i], "defined half-LSB rounding");
    }
    Settings invalid;
    invalid.dither = false;
    invalid.bits = 7;
    Quantizer fallback(invalid, 1, 48000);
    const std::vector<float> zero{0};
    Check(Packed(fallback, zero, 1, 16) == std::vector<unsigned char>({0, 0}),
          "invalid bit depth falls back to PCM16");
    unsigned char sentinel = 0x5a;
    for (int channels : {0, -1, 9}) {
        Quantizer no_channels(invalid, channels, 48000);
        no_channels.Pack(&sentinel, zero.data(), 1);
    }
    fallback.Pack(nullptr, zero.data(), 1);
    fallback.Pack(&sentinel, nullptr, 1);
    fallback.Pack(&sentinel, zero.data(), 0);
    fallback.Pack(&sentinel, zero.data(), -1);
    Check(sentinel == 0x5a, "invalid buffers/counts are harmless no-ops");
}

void DeterminismAndChunking() {
    constexpr int channels = 8, frames = 769;
    std::vector<float> input(frames * channels);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < channels; ++c)
            input[f * channels + c] = static_cast<float>(.6 * std::sin(.031 * f + .29 * c));
    for (int bits : depths) for (DitherPdf pdf : pdfs) for (NoiseShape shape : shapes) {
        Settings settings;
        settings.bits = bits; settings.dither_pdf = pdf; settings.noise_shape = shape;
        Quantizer whole(settings, channels, 48000), chunked(settings, channels, 48000);
        const auto expected = Packed(whole, input, channels, bits);
        std::vector<unsigned char> actual(expected.size(), 0);
        int offset = 0, iteration = 0;
        constexpr std::array<int, 7> chunks{1, 2, 31, 7, 128, 3, 257};
        while (offset < frames) {
            const int count = (std::min)(chunks[iteration++ % chunks.size()], frames - offset);
            chunked.Pack(actual.data() + offset * channels * (bits / 8),
                         input.data() + offset * channels, count);
            offset += count;
        }
        Check(expected == actual, "all PDFs/shapes/depths are chunk-size independent");
        whole.Reset();
        Check(Packed(whole, input, channels, bits) == expected, "Reset restores RNG and channel histories");
        Quantizer independent(settings, channels, 48000);
        Check(Packed(independent, input, channels, bits) == expected, "instances have deterministic private seeds");
        independent.Reset();
        independent.Pack(nullptr, input.data(), frames);
        independent.Pack(actual.data(), input.data(), 0);
        Check(Packed(independent, input, channels, bits) == expected, "no-op calls do not advance state");
    }
}

void ChannelIndependence() {
    constexpr int frames = 16384, channels = 8;
    std::vector<float> silence(frames * channels, 0), changed(silence), mono(frames, 0);
    for (int f = 0; f < frames; ++f) {
        changed[f * channels + 3] = f & 1 ? 20.f : -20.f;
        changed[f * channels + 6] = .4f;
    }
    for (DitherPdf pdf : pdfs) for (NoiseShape shape : shapes) {
        Settings settings;
        settings.bits = 16; settings.dither_pdf = pdf; settings.noise_shape = shape;
        Quantizer a(settings, channels, 48000), b(settings, channels, 48000), one(settings, 1, 48000);
        const auto original = Packed(a, silence, channels, 16);
        const auto altered = Packed(b, changed, channels, 16);
        const auto first = Packed(one, mono, 1, 16);
        for (int f = 0; f < frames; ++f) {
            Check(Decode(original.data() + f * channels * 2, 16) == Decode(first.data() + f * 2, 16),
                  "channel zero's RNG is independent of configured channel count");
            for (int c : {0, 1, 2, 4, 5, 7})
                Check(Decode(original.data() + (f * channels + c) * 2, 16) ==
                      Decode(altered.data() + (f * channels + c) * 2, 16),
                      "other channels and their overloads cannot alter a channel's state");
        }
        // Rectangular dither on exact silence rounds to zero, so use the two
        // nondegenerate zero-input distributions to check stream separation.
        if (pdf != DitherPdf::Rectangular) {
            int differences = 0;
            double cross = 0., energy0 = 0., energy1 = 0.;
            for (int f = 0; f < frames; ++f) {
                const double x = static_cast<double>(Decode(original.data() + f * channels * 2, 16));
                const double y = static_cast<double>(Decode(original.data() + (f * channels + 1) * 2, 16));
                differences += x != y ? 1 : 0;
                cross += x * y; energy0 += x * x; energy1 += y * y;
            }
            Check(differences > frames / 10, "channel seeds generate distinct noise streams");
            Check(std::abs(cross) < .08 * std::sqrt(energy0 * energy1), "channel noise is not correlated");
        }
    }
}

void DistributionStatistics() {
    constexpr int count = 131072, phases = 4096;
    for (int bits : depths) for (DitherPdf pdf : pdfs) {
        Settings settings;
        settings.bits = bits; settings.dither_pdf = pdf; settings.noise_shape = NoiseShape::None;
        Quantizer quantizer(settings, 1, 48000);
        const double scale = std::ldexp(1., bits - 1);
        std::vector<float> input(count);
        // Dyadic quantization phases are represented exactly at every tested
        // bit depth, including PCM32. Uniform phases expose dither variance:
        // Var(total error) = Var(dither) + 1/12 for unshaped rounding.
        for (int i = 0; i < count; ++i) {
            const double phase = ((i * 2053 % phases) + .5) / phases - .5;
            input[i] = static_cast<float>(phase / scale);
        }
        const auto output = Packed(quantizer, input, 1, bits);
        double sum = 0., square = 0.;
        for (int i = 0; i < count; ++i) {
            const double error = static_cast<double>(Decode(output.data() + i * (bits / 8), bits)) - input[i] * scale;
            sum += error; square += error * error;
        }
        const double mean = sum / count;
        const double variance = square / count - mean * mean;
        const double dither_variance = pdf == DitherPdf::Rectangular ? 1. / 12 : 1. / 6;
        Check(std::abs(mean) < .008, "each distribution is statistically zero-mean at all depths");
        Check(std::abs(variance - (dither_variance + 1. / 12)) < .008,
              "rectangular/triangular/Gaussian statistical variances match their definitions");
    }
    for (DitherPdf pdf : pdfs) {
        Settings settings;
        settings.bits = 16; settings.dither_pdf = pdf; settings.noise_shape = NoiseShape::None;
        Quantizer quantizer(settings, 1, 48000);
        const std::vector<float> silence(count, 0);
        const auto output = Packed(quantizer, silence, 1, 16);
        int nonzero = 0, tails = 0;
        for (int i = 0; i < count; ++i) {
            const auto value = Decode(output.data() + i * 2, 16);
            nonzero += value != 0 ? 1 : 0;
            tails += std::abs(value) > 1 ? 1 : 0;
        }
        if (pdf == DitherPdf::Rectangular) Check(nonzero == 0, "rectangular half-LSB support");
        if (pdf == DitherPdf::Triangular) {
            Check(tails == 0, "triangular dither is bounded to one LSB");
            Check(std::abs(nonzero / static_cast<double>(count) - .25) < .008,
                  "triangular zero-input quantized variance");
        }
        if (pdf == DitherPdf::Gaussian) Check(tails > 3, "Gaussian has independently generated wider tails");
    }
}

void RatePoliciesAndOverload() {
    for (int rate : {-1, 0, 8000, 16000, 22050, 24000, 32000, 37800, 44100, 48000, 96000, 192000}) {
        for (NoiseShape shape : shapes) {
            Settings settings;
            settings.noise_shape = shape;
            const bool ath = rate == 22050 || rate == 32000 || rate == 37800 || rate == 44100 || rate == 48000;
            const bool low = rate == 44100 || rate == 48000;
            NoiseShape expected = shape;
            if ((shape == NoiseShape::Ath && !ath) || (shape == NoiseShape::AthLow && !low))
                expected = NoiseShape::FirstOrder;
            Quantizer quantizer(settings, 1, rate);
            Check(quantizer.EffectiveShape() == expected, "ATH rates and explicit first-order fallbacks");
            if (expected == NoiseShape::FirstOrder && shape != NoiseShape::FirstOrder) {
                Settings reference = settings;
                reference.noise_shape = NoiseShape::FirstOrder;
                Quantizer first_order(reference, 1, rate);
                std::vector<float> probe(257);
                for (int i = 0; i < static_cast<int>(probe.size()); ++i)
                    probe[i] = static_cast<float>(.05 * std::sin(.071 * i));
                Check(Packed(quantizer, probe, 1, 16) == Packed(first_order, probe, 1, 16),
                      "rate fallback uses exactly the first-order processor");
            }
            settings.dither = false;
            Check(Quantizer(settings, 1, rate).EffectiveShape() == NoiseShape::None,
                  "dither switch disables every shaping mode at every rate");
        }
    }
    for (int bits : depths) for (DitherPdf pdf : pdfs) for (NoiseShape shape : shapes) {
        Settings settings;
        settings.bits = bits; settings.dither_pdf = pdf; settings.noise_shape = shape;
        Quantizer a(settings, 1, 48000), b(settings, 1, 48000);
        std::vector<float> positive(97, .125f), negative(97, -.125f);
        Packed(a, positive, 1, bits); Packed(b, negative, 1, bits);
        const std::vector<float> clipped{(std::numeric_limits<float>::max)()};
        Check(Packed(a, clipped, 1, bits) == Packed(b, clipped, 1, bits), "extreme input saturates consistently");
        const std::vector<float> silence(257, 0);
        Check(Packed(a, silence, 1, bits) == Packed(b, silence, 1, bits), "overload clears all shaping history");
        std::vector<float> extremes(4096);
        for (int i = 0; i < static_cast<int>(extremes.size()); ++i) {
            switch (i % 6) {
            case 0: extremes[i] = (std::numeric_limits<float>::max)(); break;
            case 1: extremes[i] = -(std::numeric_limits<float>::max)(); break;
            case 2: extremes[i] = (std::numeric_limits<float>::quiet_NaN)(); break;
            case 3: extremes[i] = (std::numeric_limits<float>::infinity)(); break;
            case 4: extremes[i] = (std::numeric_limits<float>::denorm_min)(); break;
            default: extremes[i] = 0; break;
            }
        }
        const auto output = Packed(a, extremes, 1, bits);
        const auto scale = INT64_C(1) << (bits - 1);
        for (int i = 0; i < static_cast<int>(extremes.size()); ++i) {
            const auto code = Decode(output.data() + i * (bits / 8), bits);
            if (i % 6 == 0) Check(code == scale - 1, "positive extreme has no integer overflow");
            else if (i % 6 == 1) Check(code == -scale, "negative extreme has no integer overflow");
            else Check(std::abs(code) < 32, "finite history recovers after nonfinite/extreme input");
        }
    }
}

using Matrix = std::array<std::array<double, 8>, 8>;
Matrix ExpectedMatrix(ChannelMode mode, int source) {
    Matrix m{};
    if (mode == ChannelMode::Source || mode == ChannelMode::LegacyUnmapped ||
        (mode == ChannelMode::Quad && source <= 4)) {
        for (int c = 0; c < source; ++c) m[c][c] = 1;
    } else if (mode == ChannelMode::Mono) {
        for (int c = 0; c < source; ++c) m[0][c] = 1. / source;
    } else if (mode == ChannelMode::Stereo) {
        m[0][0] = 1; m[1][source == 1 ? 0 : 1] = 1;
        if (source >= 3 && source != 4) m[0][2] = m[1][2] = k;
        switch (source) {
        case 4: m[0][2] = m[1][3] = k; break;
        case 5: m[0][3] = m[1][4] = k; break;
        case 6: m[0][4] = m[1][5] = k; break;
        case 7: m[0][4] = m[1][4] = .5; m[0][5] = m[1][6] = k; break;
        case 8: m[0][4] = m[0][6] = m[1][5] = m[1][7] = k; break;
        default: break;
        }
        const std::array<double, 8> divisor{1, 1, 1 + k, 1 + k, 1 + 2 * k, 1 + 2 * k,
                                            1.5 + 2 * k, 1 + 3 * k};
        for (int row = 0; row < 2; ++row) for (int c = 0; c < source; ++c) m[row][c] /= divisor[source - 1];
    } else if (mode == ChannelMode::Quad) {
        m[0][0] = m[1][1] = 1;
        m[0][2] = m[1][2] = k;
        if (source >= 6) m[0][3] = m[1][3] = k;
        switch (source) {
        case 5: m[2][3] = m[3][4] = 1; break;
        case 6: m[2][4] = m[3][5] = 1; break;
        case 7: m[2][4] = m[3][4] = .5; m[2][5] = m[3][6] = 1; break;
        case 8: m[2][4] = m[2][6] = m[3][5] = m[3][7] = .5; break;
        default: break;
        }
    } else {
        m[0][0] = 1; m[1][source == 1 ? 0 : 1] = 1;
        if (source >= 3 && source != 4) m[0][2] = m[1][2] = k;
        if (source >= 6) m[0][3] = m[1][3] = k;
        // Speaker-to-rear-bus incidence, independent of the encoder equations.
        constexpr std::array<std::array<double, 8>, 8> left{{
            {}, {}, {}, {{0,0,1,0,0,0,0,0}}, {{0,0,0,1,0,0,0,0}},
            {{0,0,0,0,1,0,0,0}}, {{0,0,0,0,.5,1,0,0}}, {{0,0,0,0,1,0,1,0}}}};
        constexpr std::array<std::array<double, 8>, 8> right{{
            {}, {}, {}, {{0,0,0,1,0,0,0,0}}, {{0,0,0,0,1,0,0,0}},
            {{0,0,0,0,0,1,0,0}}, {{0,0,0,0,.5,0,1,0}}, {{0,0,0,0,0,1,0,1}}}};
        const double near = mode == ChannelMode::SurroundII ? std::pow(10., 1.5 / 20) : 1.;
        const double far = mode == ChannelMode::SurroundII ? std::pow(10., -1.5 / 20) : 1.;
        for (int c = 0; c < source; ++c) {
            m[0][c] -= k * (near * left[source - 1][c] + far * right[source - 1][c]);
            m[1][c] += k * (far * left[source - 1][c] + near * right[source - 1][c]);
        }
    }
    return m;
}

void ChannelMatrices() {
    for (ChannelMode mode : modes) for (int source = 1; source <= 8; ++source) {
        const int expected_channels = mode == ChannelMode::Mono ? 1 :
            mode == ChannelMode::Stereo || mode == ChannelMode::Surround || mode == ChannelMode::SurroundII ? 2 :
            mode == ChannelMode::Quad && source >= 4 ? 4 : source;
        Check(OutputChannels(mode, source) == expected_channels, "every mode's 1..8 output channel counts");
        const Matrix matrix = ExpectedMatrix(mode, source);
        for (int c = 0; c < source; ++c) {
            std::array<float, 8> impulse{};
            impulse[c] = 1;
            std::array<float, 10> guarded{};
            guarded.front() = 123; guarded[expected_channels + 1] = 456;
            MixFrame(impulse.data(), source, guarded.data() + 1, expected_channels, mode, 1.7);
            Check(guarded.front() == 123 && guarded[expected_channels + 1] == 456, "matrix output guards");
            for (int row = 0; row < expected_channels; ++row)
                Close(guarded[row + 1], 1.7 * matrix[row][c], "every impulse's matrix sign, coefficient and gain");
            auto in_place = impulse;
            MixFrame(in_place.data(), source, in_place.data(), expected_channels, mode, 1.7);
            for (int row = 0; row < expected_channels; ++row)
                Close(in_place[row], guarded[row + 1], "in-place matrix mixing");
        }
        std::array<float, 8> ones{}, out{};
        ones.fill(1);
        MixFrame(ones.data(), source, out.data(), expected_channels, mode, 1.);
        if (mode == ChannelMode::Stereo || mode == ChannelMode::Mono)
            for (int row = 0; row < expected_channels; ++row) Close(out[row], 1, "plain stereo/mono are normalized");
    }
    for (ChannelMode mode : modes) {
        Check(OutputChannels(mode, 0) == 0 && OutputChannels(mode, 9) == 0, "invalid layout counts are rejected");
    }
    Check(OutputChannels(static_cast<ChannelMode>(99), 6) == 6, "unknown mode safely preserves count");
    std::array<float, 8> raw{1, 2, 3, 4, 5, 6, 7, 8}, out{};
    MixFrame(raw.data(), 6, out.data(), 6, ChannelMode::LegacyUnmapped, 1.);
    Check(std::equal(raw.begin(), raw.begin() + 6, out.begin()), "raw six-channel legacy order is not reordered");
    MixFrame(raw.data(), 3, out.data(), 8, ChannelMode::Source, 1.);
    Check(out[0] == 1 && out[2] == 3 && out[3] == 0 && out[7] == 0, "explicit count mismatch zero-pads pass-through");
    MixFrame(raw.data(), 8, out.data(), 2, ChannelMode::Source, 1.);
    Check(out[0] == 1 && out[1] == 2, "explicit count mismatch truncates pass-through");
    raw.fill(0); raw[4] = 1;
    std::array<float, 2> rear_left{}, rear_right{};
    MixFrame(raw.data(), 6, rear_left.data(), 2, ChannelMode::SurroundII, 1.);
    raw[4] = 0; raw[5] = 1;
    MixFrame(raw.data(), 6, rear_right.data(), 2, ChannelMode::SurroundII, 1.);
    Check(rear_left[0] < 0 && rear_left[1] > 0 && rear_right[0] < 0 && rear_right[1] > 0,
          "DS2 rear signals are anti-phase");
    Check(std::abs(rear_left[0]) > std::abs(rear_right[0]) && rear_right[1] > rear_left[1],
          "DS2 preserves asymmetric rear separation");
    Close(-rear_left[0] / rear_left[1], std::pow(10., 3. / 20.), "DS2 rear ratio is independently derived +/-1.5dB");
    raw.fill(1);
    MixFrame(raw.data(), 6, out.data(), 2, ChannelMode::Surround, 1.);
    Check(out[1] > 3.8f, "DS is unnormalized before packing");
    Settings settings; settings.dither = false; settings.bits = 16;
    Quantizer quantizer(settings, 2, 48000);
    std::array<unsigned char, 4> packed{};
    quantizer.Pack(packed.data(), out.data(), 1);
    Check(Decode(packed.data() + 2, 16) == 32767, "DS over-range matrix clips at integer pack only");
    raw[0] = (std::numeric_limits<float>::quiet_NaN)();
    raw[1] = (std::numeric_limits<float>::infinity)();
    raw[2] = (std::numeric_limits<float>::max)();
    for (ChannelMode mode : modes) {
        const int count = OutputChannels(mode, 8);
        MixFrame(raw.data(), 8, out.data(), count, mode, (std::numeric_limits<double>::max)());
        for (int c = 0; c < count; ++c) Check(std::isfinite(out[c]), "extreme mix/gain stays within finite float range");
        MixFrame(raw.data(), 8, out.data(), count, mode, (std::numeric_limits<double>::quiet_NaN)());
        for (int c = 0; c < count; ++c) Check(out[c] == 0, "nonfinite gain becomes silence");
    }
    MixFrame(nullptr, 6, out.data(), 2, ChannelMode::Stereo, 1.);
    Check(out[0] == 0 && out[1] == 0, "missing mix input becomes silence");
}

double BandPower(const std::vector<double>& noise, double first, double last) {
    constexpr int probes = 48;
    double total = 0;
    for (int probe = 0; probe < probes; ++probe) {
        const double frequency = first + (last - first) * (probe + .5) / probes;
        const double bin = std::floor(frequency * static_cast<double>(noise.size()) / 48000 + .5);
        const double coefficient = 2 * std::cos(2 * 3.14159265358979323846 * bin / static_cast<double>(noise.size()));
        double previous = 0., previous2 = 0.;
        for (double value : noise) {
            const double current = value + coefficient * previous - previous2;
            previous2 = previous; previous = current;
        }
        total += previous * previous + previous2 * previous2 - coefficient * previous * previous2;
    }
    return total / probes;
}
void ShapingSpectrum() {
    constexpr int warmup = 512, count = 32768;
    double ath_energy = 0, low_energy = 0;
    for (NoiseShape shape : {NoiseShape::FirstOrder, NoiseShape::Ath, NoiseShape::AthLow}) {
        Settings settings;
        settings.bits = 16; settings.noise_shape = shape; settings.dither_pdf = DitherPdf::Triangular;
        Quantizer quantizer(settings, 1, 48000);
        const std::vector<float> silence(warmup + count, 0);
        const auto output = Packed(quantizer, silence, 1, 16);
        std::vector<double> noise(count);
        double energy = 0;
        for (int i = 0; i < count; ++i) {
            noise[i] = static_cast<double>(Decode(output.data() + (warmup + i) * 2, 16));
            energy += noise[i] * noise[i];
        }
        Check(BandPower(noise, 1000, 4000) < .8 * BandPower(noise, 13000, 19000),
              "first-order and analytic ATH filters move noise out of sensitive frequencies");
        if (shape == NoiseShape::Ath) ath_energy = energy;
        if (shape == NoiseShape::AthLow) low_energy = energy;
    }
    Check(low_energy < ath_energy, "low-amplitude ATH has lower total output noise than full ATH");
}
} // namespace

int main() {
    try {
        PackingAndBounds();
        DeterminismAndChunking();
        ChannelIndependence();
        DistributionStatistics();
        RatePoliciesAndOverload();
        ChannelMatrices();
        ShapingSpectrum();
        std::cout << "Processing: PCM8/16/24/32, distributions, deterministic state, channel isolation, "
                     "ATH policy/spectra, overloads and all 1..8-channel matrices passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
