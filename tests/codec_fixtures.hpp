#pragma once
#include "fixtures.hpp"
void MakeVorbis(const std::filesystem::path&, int channels, int rate, int frames, int serial,
                const std::vector<std::string>& comments = {});
void MakeFlac(const std::filesystem::path&, int channels, int rate, int frames, bool known_total = true);
void MakeSpeex(const std::filesystem::path&, int rate, int frames, int serial = 753, int packets_per_page = 1);
