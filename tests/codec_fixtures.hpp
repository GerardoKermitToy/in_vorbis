#pragma once
#include "fixtures.hpp"
void MakeVorbis(const std::filesystem::path&, int channels, int rate, int frames, int serial);
void MakeFlac(const std::filesystem::path&, int channels, int rate, int frames);
void MakeSpeex(const std::filesystem::path&, int rate, int frames, int serial = 753);
