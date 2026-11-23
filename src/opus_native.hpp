// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "backend.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ogg_input {
namespace opus_native {
struct Header {
    int channels = 0, family = 0, streams = 0, coupled = 0;
    int preskip = 0, gain_q8 = 0;
    std::uint32_t input_rate = 0;
    std::vector<unsigned char> mapping, matrix;
};
// Exposed for header/packet regression tests; neither trusts unbounded input.
bool ParseHeader(const unsigned char*, std::size_t, Header&) noexcept;
bool InspectPacket(const unsigned char*, std::size_t, int streams,
                   int& duration_48k, bool& qext) noexcept;
}
std::unique_ptr<CodecReader> OpenNativeOpus(std::shared_ptr<ByteStream>, const Settings&, std::wstring&);
}
