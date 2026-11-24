#pragma once
#include "settings.hpp"
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
namespace ogg_input {
bool IsHttp(const wchar_t*) noexcept;
std::wstring RequestUrl(const wchar_t*);
std::wstring Utf8(const char*, int length = -1);
std::string ToUtf8(const std::wstring&);
struct Station {
    std::wstring name, description, genre, website, content_type, title;
    std::wstring recording_path, recording_error;
    int bitrate = 0;
};
class ByteStream final {
public:
    ~ByteStream();
    bool Open(const wchar_t*, const Settings&, std::wstring& error);
    int Read(void*, int bytes) noexcept;
    bool Seek(std::int64_t, int origin) noexcept;
    std::int64_t Tell() const noexcept;
    std::int64_t Size() const noexcept { return size_; }
    // Includes any RIFF wrapper; Size() remains the seekable Ogg payload size.
    std::int64_t SourceSize() const noexcept { return source_size_; }
    bool Seekable() const noexcept { return file_ || !memory_.empty() || http_seekable_; }
    bool Radio() const noexcept { return radio_ && !http_seekable_; }
    bool Local() const noexcept { return !radio_; }
    void Cancel() noexcept;
    void FinishProbe();
    bool RewindProbe() noexcept;
    const Station& Info() const noexcept { return station_; }
private:
    int RawRead(void*, int) noexcept;
    int NetworkRead(void*, int) noexcept;
    bool Exact(void*, int) noexcept;
    std::wstring Header(const wchar_t*) const;
    FILE* file_ = nullptr;
    HINTERNET session_ = nullptr, connection_ = nullptr;
    std::atomic<HINTERNET> request_{nullptr};
    std::atomic<bool> cancelled_{false};
    std::vector<unsigned char> memory_, prefix_;
    std::int64_t position_ = 0, size_ = -1, source_size_ = -1, base_offset_ = 0;
    std::size_t replay_ = 0;
    bool radio_ = false, opening_ = true, replaying_ = false, http_seekable_ = false;
    std::wstring resource_;
    DWORD request_flags_ = 0;
    bool Range(std::int64_t offset) noexcept;
    int probe_limit_ = 1048576, network_bytes_ = 0, icy_interval_ = 0, icy_left_ = 0;
    ULONGLONG deadline_ = 0;
    Station station_;
    Settings settings_;
    std::vector<unsigned char> network_buffer_;
    int network_at_ = 0, network_used_ = 0;
    HANDLE recording_ = INVALID_HANDLE_VALUE;
    void Record(const void*, int) noexcept;
};
}
