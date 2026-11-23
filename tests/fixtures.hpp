#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

// Test-process only: preserve failing exit codes without opening Windows Error
// Reporting dialogs that block unattended CTest and interrupt the desktop.
inline const bool kNonInteractiveTestErrors = [] {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    return true;
}();

inline void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void MakeOpus(const std::filesystem::path& path, int channels, int frames, int serial,
    const std::vector<std::string>& extra_tags = {}, int header_gain_q8 = 0);
void MakeVorbisHeaders(const std::filesystem::path& path, int serial);
void AppendFile(const std::filesystem::path& destination, const std::filesystem::path& source);
