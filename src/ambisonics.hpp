// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "settings.hpp"
#include <array>
#include <vector>

namespace ogg_input {
// RFC 8486: ACN/SN3D channels form a complete spherical-harmonic order,
// optionally followed by a non-diegetic left/right pair. Never a speaker map.
bool AmbisonicShape(int channels, int& order, int& harmonics) noexcept;

class AmbisonicRenderer {
public:
    bool Init(int source_channels, int rate, AmbisonicMode mode);
    int Channels() const noexcept { return output_channels_; }
    // Analytic, approximate binaural rendering, NOT a measured HRTF or a
    // certified spatial-audio implementation. Surround output is Windows order.
    void Render(const float* acn_sn3d, float* output, int frames) noexcept;
private:
    int source_channels_ = 0, harmonics_ = 0, output_channels_ = 0;
    int speakers_ = 0, delay_length_ = 0, delay_at_ = 0;
    bool headphones_ = false;
    std::vector<float> matrix_, delay_;
    std::array<float, 8> itd_{}, shadow_alpha_{}, shadow_state_{};
    std::array<bool, 8> left_{};
};
}
