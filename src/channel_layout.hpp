#pragma once
#include <cstdint>

namespace winamp_opus {
inline constexpr int kStereoChannels = 2;
inline constexpr int kMaxChannels = 8;

// Mapping family 1 uses Vorbis order; Windows PCM uses speaker-bit order.
// Call only for standard layouts with 1..8 channels.
void ReorderToWindows(std::int16_t* pcm, int frames, int channels) noexcept;
void ReorderToWindows(float* pcm, int frames, int channels) noexcept;
} // namespace winamp_opus
