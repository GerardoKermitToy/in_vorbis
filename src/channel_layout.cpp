#include "channel_layout.hpp"
#include <utility>

namespace winamp_opus {
namespace {
template<int Channels, typename Sample>
void ReorderFrames(Sample* pcm, int frames) noexcept {
    // Fixed strides; touch only samples whose Windows position differs.
    for (int i = 0; i < frames; ++i, pcm += Channels) {
        std::swap(pcm[1], pcm[2]);
        if constexpr (Channels >= 6) {
            const auto original3 = pcm[3];
            if constexpr (Channels == 6) {
                // Windows selects Opus indices 0,2,1,5,3,4.
                pcm[3] = pcm[5];
                pcm[5] = pcm[4];
                pcm[4] = original3;
            } else if constexpr (Channels == 7) {
                // Windows selects Opus indices 0,2,1,6,5,3,4.
                pcm[3] = pcm[6];
                pcm[6] = pcm[4];
                pcm[4] = pcm[5];
                pcm[5] = original3;
            } else if constexpr (Channels == 8) {
                // Windows selects Opus indices 0,2,1,7,5,6,3,4.
                pcm[3] = pcm[7];
                pcm[7] = pcm[4];
                pcm[4] = pcm[5];
                pcm[5] = pcm[6];
                pcm[6] = original3;
            }
        }
    }
}
} // namespace

template<typename Sample>
void Reorder(Sample* pcm, int frames, int channels) noexcept {
    // Select the layout once per block, never once per sample. Mono, stereo,
    // and quad already have Windows order; unsupported counts are unchanged.
    switch (channels) {
    case 3: ReorderFrames<3>(pcm, frames); break;
    case 5: ReorderFrames<5>(pcm, frames); break;
    case 6: ReorderFrames<6>(pcm, frames); break;
    case 7: ReorderFrames<7>(pcm, frames); break;
    case 8: ReorderFrames<8>(pcm, frames); break;
    default: break;
    }
}
void ReorderToWindows(std::int16_t* pcm, int frames, int channels) noexcept { Reorder(pcm, frames, channels); }
void ReorderToWindows(float* pcm, int frames, int channels) noexcept { Reorder(pcm, frames, channels); }
} // namespace winamp_opus
