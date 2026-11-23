// SPDX-License-Identifier: GPL-3.0-only
#include "opus_native.hpp"
namespace ogg_input {
std::unique_ptr<CodecReader> OpenOpus(std::shared_ptr<ByteStream> stream, const Settings& settings, std::wstring& error) {
    // opusfile 0.12 has fixed 48k-sized decode buffers and cannot represent
    // mapping families 2/3 or true 96k QEXT output. Keep one bounded container
    // path for ordinary, ambisonic and HD links, including mixed chains.
    return OpenNativeOpus(std::move(stream), settings, error);
}
}
