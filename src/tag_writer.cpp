// SPDX-License-Identifier: GPL-3.0-only
// Independent, conservative Ogg Vorbis/Opus comment editor.
#include "tag_writer.hpp"

#include <ogg/ogg.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ogg_input {
namespace {

using Byte = unsigned char;

constexpr std::size_t kMaxHeaderBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxCommentEntries = 65536;
constexpr std::size_t kMaxLogicalStreams = 4096;
constexpr std::size_t kNoPlan = std::numeric_limits<std::size_t>::max();

bool Fail(std::wstring& error, const wchar_t* text) {
    error = text;
    return false;
}

bool FailWin32(std::wstring& error, const wchar_t* text) {
    error = std::wstring(text) + L" (Windows error " + std::to_wstring(GetLastError()) + L").";
    return false;
}

class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = other.Release();
        }
        return *this;
    }

    HANDLE Get() const noexcept { return value_; }
    bool Valid() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    HANDLE Release() noexcept {
        HANDLE result = value_;
        value_ = INVALID_HANDLE_VALUE;
        return result;
    }
    void Reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (Valid()) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct FileSnapshot {
    BY_HANDLE_FILE_INFORMATION info{};
};

bool SameFileTime(const FILETIME& a, const FILETIME& b) noexcept {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

bool SameSnapshot(const FileSnapshot& a, const FileSnapshot& b) noexcept {
    return a.info.dwVolumeSerialNumber == b.info.dwVolumeSerialNumber &&
           a.info.nFileIndexHigh == b.info.nFileIndexHigh &&
           a.info.nFileIndexLow == b.info.nFileIndexLow &&
           a.info.nFileSizeHigh == b.info.nFileSizeHigh &&
           a.info.nFileSizeLow == b.info.nFileSizeLow &&
           a.info.nNumberOfLinks == b.info.nNumberOfLinks &&
           SameFileTime(a.info.ftLastWriteTime, b.info.ftLastWriteTime);
}

bool Snapshot(HANDLE file, FileSnapshot& snapshot, std::wstring& error) {
    if (!GetFileInformationByHandle(file, &snapshot.info)) {
        return FailWin32(error, L"Cannot inspect the audio file");
    }
    return true;
}

bool SafeRegularFile(HANDLE file, FileSnapshot& snapshot, std::wstring& error) {
    const DWORD type = GetFileType(file);
    if (type != FILE_TYPE_DISK) return Fail(error, L"The audio path is not a regular disk file.");
    if (!Snapshot(file, snapshot, error)) return false;
    const DWORD attributes = snapshot.info.dwFileAttributes;
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return Fail(error, L"Refusing to edit a directory or reparse-point audio path.");
    }
    if (snapshot.info.nNumberOfLinks != 1) {
        return Fail(error, L"Refusing to edit a hard-linked audio file.");
    }
    return true;
}

bool OpenSafeSource(const std::wstring& path, Handle& file, FileSnapshot& snapshot, std::wstring& error) {
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return FailWin32(error, L"Cannot open the audio file safely");
    Handle opened(raw);
    if (!SafeRegularFile(opened.Get(), snapshot, error)) return false;
    file = std::move(opened);
    return true;
}

bool CurrentSnapshot(HANDLE file, const FileSnapshot& expected, std::wstring& error) {
    FileSnapshot current{};
    if (!Snapshot(file, current, error)) return false;
    if (!SameSnapshot(expected, current)) {
        return Fail(error, L"The audio file changed while its tags were being prepared.");
    }
    return true;
}

bool SourceStillAtPath(const std::wstring& path, const FileSnapshot& expected, std::wstring& error) {
    Handle current;
    FileSnapshot snapshot{};
    if (!OpenSafeSource(path, current, snapshot, error)) return false;
    if (!SameSnapshot(expected, snapshot)) {
        return Fail(error, L"The audio path was replaced while its tags were being prepared.");
    }
    return true;
}

bool AbsolutePath(const wchar_t* path, std::wstring& result, std::wstring& error) {
    if (!path || !*path) return Fail(error, L"No audio file was supplied.");
    const DWORD required = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (!required) return FailWin32(error, L"Cannot resolve the audio path");
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetFullPathNameW(path, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!written || written >= buffer.size()) return FailWin32(error, L"Cannot resolve the audio path");
    result.assign(buffer.data(), written);
    return true;
}

std::wstring MakeTemporaryName(const std::wstring& source, unsigned attempt) {
    const std::size_t slash = source.find_last_of(L"\\/");
    const std::wstring folder = slash == std::wstring::npos ? std::wstring() : source.substr(0, slash + 1);
    const DWORD tick = GetTickCount();
    return folder + L".ogg-tags-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(tick) + L"-" + std::to_wstring(attempt) + L".tmp";
}

class TemporaryOutput final {
public:
    ~TemporaryOutput() {
        file_.Reset();
        if (!path_.empty()) DeleteFileW(path_.c_str());
    }
    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;
    TemporaryOutput() = default;

    bool Create(const std::wstring& source, std::wstring& error) {
        for (unsigned attempt = 0; attempt != 256; ++attempt) {
            path_ = MakeTemporaryName(source, attempt);
            HANDLE raw = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (raw != INVALID_HANDLE_VALUE) {
                file_.Reset(raw);
                return true;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
                path_.clear();
                return FailWin32(error, L"Cannot create a temporary tag file");
            }
        }
        path_.clear();
        return Fail(error, L"Cannot create a unique temporary tag file.");
    }

    HANDLE File() const noexcept { return file_.Get(); }
    const std::wstring& Path() const noexcept { return path_; }
    bool FlushAndClose(std::wstring& error) {
        if (!FlushFileBuffers(file_.Get())) return FailWin32(error, L"Cannot flush the temporary tag file");
        file_.Reset();
        return true;
    }
    void KeepReplacement() noexcept { path_.clear(); }

private:
    Handle file_;
    std::wstring path_;
};

bool WriteAll(HANDLE file, const Byte* data, std::size_t size, std::wstring& error) {
    while (size != 0) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(size, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, data, requested, &written, nullptr) || written == 0) {
            return FailWin32(error, L"Cannot write the temporary tag file");
        }
        data += written;
        size -= written;
    }
    return true;
}

bool SeekToBeginning(HANDLE file, std::wstring& error) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return FailWin32(error, L"Cannot rewind the audio file");
    return true;
}

std::uint32_t ReadLe32(const Byte* data) noexcept {
    return std::uint32_t(data[0]) |
        (std::uint32_t(data[1]) << 8) |
        (std::uint32_t(data[2]) << 16) |
        (std::uint32_t(data[3]) << 24);
}

std::uint64_t ReadLe64(const Byte* data) noexcept {
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift != 8; ++shift) result |= std::uint64_t(data[shift]) << (shift * 8);
    return result;
}

void PutLe32(Byte* data, std::uint32_t value) noexcept {
    data[0] = static_cast<Byte>(value);
    data[1] = static_cast<Byte>(value >> 8);
    data[2] = static_cast<Byte>(value >> 16);
    data[3] = static_cast<Byte>(value >> 24);
}

void AppendLe32(std::vector<Byte>& out, std::uint32_t value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    PutLe32(out.data() + at, value);
}

bool AppendBounded(std::vector<Byte>& out, const Byte* data, std::size_t count, std::wstring& error) {
    if (count > kMaxHeaderBytes - out.size()) return Fail(error, L"Ogg comment data exceeds the 16 MiB safety limit.");
    out.insert(out.end(), data, data + count);
    return true;
}

bool AppendLe32Bounded(std::vector<Byte>& out, std::uint32_t value, std::wstring& error) {
    if (out.size() > kMaxHeaderBytes - 4) return Fail(error, L"Ogg comment data exceeds the 16 MiB safety limit.");
    AppendLe32(out, value);
    return true;
}

void SetPageChecksum(std::vector<Byte>& raw, std::size_t headerSize, std::size_t bodySize) noexcept {
    ogg_page page{};
    page.header = raw.data();
    page.header_len = static_cast<long>(headerSize);
    page.body = raw.data() + headerSize;
    page.body_len = static_cast<long>(bodySize);
    ogg_page_checksum_set(&page);
}

struct Page {
    std::uint64_t offset = 0;
    std::vector<Byte> raw;
    std::size_t headerSize = 0, bodySize = 0;
    std::uint32_t serial = 0, sequence = 0;
    std::uint64_t granule = 0;
    bool continued = false, bos = false, eos = false;

    const Byte* Body() const noexcept { return raw.data() + headerSize; }
    const Byte* Segments() const noexcept { return raw.data() + 27; }
    bool HeaderGranuleIsNonAudio() const noexcept {
        return granule == 0 || granule == std::numeric_limits<std::uint64_t>::max();
    }
};

enum class PageRead { Page, End, Error };

bool ReadRest(HANDLE file, Byte* destination, std::size_t size, std::wstring& error) {
    while (size != 0) {
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(size, std::numeric_limits<DWORD>::max()));
        DWORD got = 0;
        if (!ReadFile(file, destination, wanted, &got, nullptr)) return FailWin32(error, L"Cannot read the audio file");
        if (got == 0) return Fail(error, L"The Ogg file is truncated.");
        destination += got;
        size -= got;
    }
    return true;
}

PageRead ReadPage(HANDLE file, std::uint64_t& offset, Page& page, std::wstring& error) {
    std::array<Byte, 27> fixed{};
    DWORD got = 0;
    if (!ReadFile(file, fixed.data(), static_cast<DWORD>(fixed.size()), &got, nullptr)) {
        FailWin32(error, L"Cannot read the audio file");
        return PageRead::Error;
    }
    if (got == 0) return PageRead::End;
    if (got < fixed.size() && !ReadRest(file, fixed.data() + got, fixed.size() - got, error)) return PageRead::Error;
    if (std::memcmp(fixed.data(), "OggS", 4) != 0 || fixed[4] != 0) {
        Fail(error, L"The audio file has an invalid Ogg page header.");
        return PageRead::Error;
    }
    if ((fixed[5] & ~Byte(0x07)) != 0) {
        Fail(error, L"The audio file uses unsupported Ogg page flags.");
        return PageRead::Error;
    }

    const std::size_t segmentCount = fixed[26];
    page.headerSize = 27 + segmentCount;
    page.raw.assign(fixed.begin(), fixed.end());
    page.raw.resize(page.headerSize);
    if (segmentCount != 0 && !ReadRest(file, page.raw.data() + 27, segmentCount, error)) return PageRead::Error;

    page.bodySize = 0;
    for (std::size_t i = 0; i != segmentCount; ++i) page.bodySize += page.raw[27 + i];
    page.raw.resize(page.headerSize + page.bodySize);
    if (page.bodySize != 0 && !ReadRest(file, page.raw.data() + page.headerSize, page.bodySize, error)) return PageRead::Error;

    page.offset = offset;
    if (page.raw.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
        Fail(error, L"The Ogg file is too large to process safely.");
        return PageRead::Error;
    }
    offset += page.raw.size();
    const std::uint32_t suppliedChecksum = ReadLe32(page.raw.data() + 22);
    SetPageChecksum(page.raw, page.headerSize, page.bodySize);
    if (ReadLe32(page.raw.data() + 22) != suppliedChecksum) {
        Fail(error, L"The Ogg file contains a page with an invalid checksum.");
        return PageRead::Error;
    }

    page.serial = ReadLe32(page.raw.data() + 14);
    page.sequence = ReadLe32(page.raw.data() + 18);
    page.granule = ReadLe64(page.raw.data() + 6);
    page.continued = (page.raw[5] & 0x01) != 0;
    page.bos = (page.raw[5] & 0x02) != 0;
    page.eos = (page.raw[5] & 0x04) != 0;
    return PageRead::Page;
}

bool IsFieldName(const std::string& name, std::string& canonical) {
    if (name.empty()) return false;
    canonical.clear();
    canonical.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 0x20 || c > 0x7d || c == '=') return false;
        canonical.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c));
    }
    return true;
}

bool IsFieldName(const Byte* data, std::size_t size, std::string& canonical) {
    std::size_t equal = 0;
    while (equal != size && data[equal] != '=') ++equal;
    if (equal == 0 || equal == size) return false;
    canonical.clear();
    canonical.reserve(equal);
    for (std::size_t i = 0; i != equal; ++i) {
        const Byte c = data[i];
        if (c < 0x20 || c > 0x7d || c == '=') return false;
        canonical.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c));
    }
    return true;
}

bool IsArtworkKey(const std::string& canonical) noexcept {
    return canonical == "metadata_block_picture" || canonical == "coverart";
}

struct RequestedTag {
    std::string name;
    std::string canonical;
    std::string value;
    bool remove = false;
};

bool ToUtf8Checked(const std::wstring& input, std::string& output, std::wstring& error) {
    if (input.find(L'\0') != std::wstring::npos) return Fail(error, L"Tag text cannot contain an embedded NUL character.");
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Fail(error, L"Tag text is too large to encode as UTF-8.");
    }
    if (input.empty()) {
        output.clear();
        return true;
    }
    const int chars = static_cast<int>(input.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), chars, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return Fail(error, L"Tag text contains an invalid Unicode surrogate.");
    output.resize(static_cast<std::size_t>(bytes));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), chars, output.data(), bytes, nullptr, nullptr) != bytes) {
        return Fail(error, L"Cannot encode tag text as UTF-8.");
    }
    return true;
}

bool PrepareRequestedTags(const Tags& tags, std::vector<RequestedTag>& requested, std::wstring& error) {
    if (tags.size() > kMaxCommentEntries) return Fail(error, L"Too many requested Ogg comment fields.");
    requested.clear();
    requested.reserve(tags.size());
    std::unordered_set<std::string> names;
    names.reserve(tags.size());
    std::size_t total = 0;
    for (const auto& tag : tags) {
        RequestedTag converted;
        converted.name = tag.first;
        if (!IsFieldName(converted.name, converted.canonical)) {
            return Fail(error, L"A requested Ogg comment field name is invalid.");
        }
        if (IsArtworkKey(converted.canonical)) {
            return Fail(error, L"Embedded artwork comments are preserved and cannot be edited here.");
        }
        if (!names.insert(converted.canonical).second) {
            return Fail(error, L"Requested Ogg comment field names must be unique ignoring case.");
        }
        if (!ToUtf8Checked(tag.second, converted.value, error)) return false;
        converted.remove = converted.value.empty();
        if (converted.name.size() > kMaxHeaderBytes || converted.value.size() > kMaxHeaderBytes ||
            converted.name.size() + 1 > kMaxHeaderBytes - converted.value.size()) {
            return Fail(error, L"A requested Ogg comment is too large.");
        }
        const std::size_t encoded = 4 + converted.name.size() + 1 + converted.value.size();
        if (encoded > kMaxHeaderBytes - total) return Fail(error, L"Requested Ogg comments exceed the 16 MiB safety limit.");
        total += encoded;
        requested.push_back(std::move(converted));
    }
    return true;
}

bool TakeLe32(const std::vector<Byte>& packet, std::size_t& at, std::uint32_t& value) {
    if (packet.size() - at < 4) return false;
    value = ReadLe32(packet.data() + at);
    at += 4;
    return true;
}

struct KeptComment {
    std::size_t at = 0;
    std::uint32_t size = 0;
};

enum class EditableCodec { Vorbis, Opus };

bool BuildCommentPacket(EditableCodec codec, const std::vector<Byte>& original,
    const std::vector<RequestedTag>& requested, std::vector<Byte>& replacement, std::wstring& error) {
    const std::size_t prefix = codec == EditableCodec::Vorbis ? 7 : 8;
    const bool validPrefix = codec == EditableCodec::Vorbis
        ? original.size() >= prefix && original[0] == 3 && std::memcmp(original.data() + 1, "vorbis", 6) == 0
        : original.size() >= prefix && std::memcmp(original.data(), "OpusTags", 8) == 0;
    if (!validPrefix) return Fail(error, L"The Ogg stream has an invalid comment header packet.");

    std::size_t at = prefix;
    std::uint32_t vendorLength = 0;
    if (!TakeLe32(original, at, vendorLength) || vendorLength > original.size() - at) {
        return Fail(error, L"The Ogg comment header has an invalid vendor string.");
    }
    const std::size_t vendorAt = at;
    at += vendorLength;
    std::uint32_t count = 0;
    if (!TakeLe32(original, at, count) || count > kMaxCommentEntries) {
        return Fail(error, L"The Ogg comment header has too many or malformed fields.");
    }

    std::unordered_set<std::string> replaced;
    replaced.reserve(requested.size());
    for (const auto& tag : requested) replaced.insert(tag.canonical);

    std::vector<KeptComment> kept;
    kept.reserve(std::min<std::size_t>(count, 4096));
    std::string key;
    for (std::uint32_t index = 0; index != count; ++index) {
        std::uint32_t length = 0;
        if (!TakeLe32(original, at, length) || length > original.size() - at) {
            return Fail(error, L"The Ogg comment header has a truncated field.");
        }
        const std::size_t fieldAt = at;
        at += length;
        const bool replace = IsFieldName(original.data() + fieldAt, length, key) &&
            !IsArtworkKey(key) && replaced.find(key) != replaced.end();
        if (!replace) kept.push_back({fieldAt, length});
    }
    std::size_t appended = 0;
    for (const auto& tag : requested) {
        if (!tag.remove) ++appended;
    }
    const std::size_t totalFields = kept.size() + appended;
    if (totalFields > kMaxCommentEntries) {
        return Fail(error, L"Too many Ogg comment fields after replacement.");
    }
    Byte vorbisFraming = 0;
    if (codec == EditableCodec::Vorbis) {
        if (at + 1 != original.size() || (original[at] & 1) == 0) {
            return Fail(error, L"A Vorbis comment header has an invalid framing byte.");
        }
        vorbisFraming = original[at];
    }
    replacement.clear();
    replacement.reserve(std::min<std::size_t>(kMaxHeaderBytes, original.size() + requested.size() * 8));
    if (!AppendBounded(replacement, original.data(), prefix, error) ||
        !AppendLe32Bounded(replacement, vendorLength, error) ||
        !AppendBounded(replacement, original.data() + vendorAt, vendorLength, error) ||
        !AppendLe32Bounded(replacement, static_cast<std::uint32_t>(totalFields), error)) return false;

    for (const KeptComment& field : kept) {
        if (!AppendLe32Bounded(replacement, field.size, error) ||
            !AppendBounded(replacement, original.data() + field.at, field.size, error)) return false;
    }
    for (const RequestedTag& tag : requested) {
        if (tag.remove) continue;
        const std::size_t bytes = tag.name.size() + 1 + tag.value.size();
        if (bytes > std::numeric_limits<std::uint32_t>::max() ||
            !AppendLe32Bounded(replacement, static_cast<std::uint32_t>(bytes), error) ||
            !AppendBounded(replacement, reinterpret_cast<const Byte*>(tag.name.data()), tag.name.size(), error)) return false;
        const Byte equals = '=';
        if (!AppendBounded(replacement, &equals, 1, error) ||
            !AppendBounded(replacement, reinterpret_cast<const Byte*>(tag.value.data()), tag.value.size(), error)) return false;
    }
    if (codec == EditableCodec::Vorbis) {
        if (!AppendBounded(replacement, &vorbisFraming, 1, error)) return false;
    } else {
        const std::size_t padding = original.size() - at;
        const std::size_t retained = std::min(padding, kMaxHeaderBytes - replacement.size());
        if (!AppendBounded(replacement, original.data() + at, retained, error)) return false;
    }
    return true;
}

struct LinkPlan {
    EditableCodec codec = EditableCodec::Vorbis;
    std::uint32_t serial = 0, firstSequence = 0;
    std::uint64_t firstHeaderOffset = 0;
    std::vector<Byte> identification;
    std::vector<Byte> comment;
    std::vector<Byte> setup;
    std::vector<std::uint64_t> headerOffsets;
    std::size_t replacementPageCount = 0;
    std::int64_t sequenceDelta = 0;
    bool selected = false;
};

enum class HeaderPhase { First, Comment, Setup, Done, Other };
enum class FirstKind { Undecided, Vorbis, Opus };

struct StreamState {
    std::uint32_t serial = 0, nextSequence = 0, bosSequence = 0;
    std::uint64_t bosOffset = 0, finalHeaderOffset = 0;
    bool active = true, haveSequence = false, packetOpen = false, capturing = false;
    bool headerPageSafe = true, audio = false;
    HeaderPhase phase = HeaderPhase::First;
    FirstKind kind = FirstKind::Undecided;
    std::size_t plan = kNoPlan;
    std::vector<Byte> packet;
    std::vector<std::uint64_t> headerOffsets;
};

struct ScanContext {
    const std::vector<RequestedTag>& requested;
    std::vector<LinkPlan> plans;
    std::size_t retainedHeaders = 0;
    int activeAudio = 0;
};

void FreeVector(std::vector<Byte>& value) {
    std::vector<Byte>().swap(value);
}

void FreeVector(std::vector<std::uint64_t>& value) {
    std::vector<std::uint64_t>().swap(value);
}

bool RetainHeaderBytes(ScanContext& context, std::size_t bytes, std::wstring& error) {
    if (bytes > kMaxHeaderBytes - context.retainedHeaders) {
        return Fail(error, L"Ogg headers and comments exceed the 16 MiB safety limit.");
    }
    context.retainedHeaders += bytes;
    return true;
}

bool StartsWith(const std::vector<Byte>& data, const char* text, std::size_t size) noexcept {
    return data.size() >= size && std::memcmp(data.data(), text, size) == 0;
}

bool ResolveFirstPrefix(StreamState& stream, std::wstring& error) {
    if (stream.kind != FirstKind::Undecided) return true;
    if (stream.packet.size() >= 7 && stream.packet[0] == 1 && std::memcmp(stream.packet.data() + 1, "vorbis", 6) == 0) {
        stream.kind = FirstKind::Vorbis;
        return true;
    }
    if (StartsWith(stream.packet, "OpusHead", 8)) {
        stream.kind = FirstKind::Opus;
        return true;
    }
    if (StartsWith(stream.packet, "Speex   ", 8) ||
        (stream.packet.size() >= 5 && stream.packet[0] == 0x7f && std::memcmp(stream.packet.data() + 1, "FLAC", 4) == 0)) {
        return Fail(error, L"This Ogg file contains an unsupported audio logical stream.");
    }
    if (stream.packet.size() >= 8) {
        stream.phase = HeaderPhase::Other;
        stream.capturing = false;
        FreeVector(stream.packet);
        FreeVector(stream.headerOffsets);
    }
    return true;
}

bool AppendPacketData(StreamState& stream, const Byte* data, std::size_t size, std::wstring& error) {
    if (!stream.capturing || size == 0) return true;
    if (size > kMaxHeaderBytes - stream.packet.size()) {
        return Fail(error, L"An Ogg header packet exceeds the 16 MiB safety limit.");
    }
    stream.packet.insert(stream.packet.end(), data, data + size);
    if (stream.phase == HeaderPhase::First) return ResolveFirstPrefix(stream, error);
    return true;
}

bool IsVorbisIdentification(const std::vector<Byte>& packet) noexcept {
    return packet.size() >= 30 && packet[0] == 1 && std::memcmp(packet.data() + 1, "vorbis", 6) == 0;
}

bool IsOpusIdentification(const std::vector<Byte>& packet) noexcept {
    return packet.size() >= 19 && std::memcmp(packet.data(), "OpusHead", 8) == 0;
}

bool IsVorbisSetup(const std::vector<Byte>& packet) noexcept {
    return packet.size() >= 7 && packet[0] == 5 && std::memcmp(packet.data() + 1, "vorbis", 6) == 0;
}

bool ActivateAudio(StreamState& stream, EditableCodec codec, ScanContext& context, std::wstring& error) {
    if (!stream.headerPageSafe || stream.headerOffsets.empty()) {
        return Fail(error, L"An Ogg audio header has an unsafe granule position or EOS flag.");
    }
    if (context.activeAudio != 0) {
        return Fail(error, L"Simultaneous Ogg audio logical streams are not safe to retag.");
    }
    if (context.plans.size() >= kMaxLogicalStreams) return Fail(error, L"Too many Ogg audio links to retag safely.");
    LinkPlan plan;
    plan.codec = codec;
    plan.serial = stream.serial;
    plan.firstSequence = stream.bosSequence;
    plan.firstHeaderOffset = stream.bosOffset;
    plan.identification = std::move(stream.packet);
    if (!RetainHeaderBytes(context, plan.identification.size(), error)) return false;
    context.plans.push_back(std::move(plan));
    stream.plan = context.plans.size() - 1;
    stream.audio = true;
    ++context.activeAudio;
    stream.capturing = false;
    stream.phase = HeaderPhase::Comment;
    return true;
}

bool CompleteHeaders(StreamState& stream, const Page& page, ScanContext& context, std::wstring& error) {
    if (!stream.headerPageSafe || stream.headerOffsets.empty()) {
        return Fail(error, L"An Ogg audio header has an unsafe granule position or EOS flag.");
    }
    if (stream.plan == kNoPlan || stream.plan >= context.plans.size()) return Fail(error, L"Internal Ogg header tracking error.");
    LinkPlan& plan = context.plans[stream.plan];
    plan.headerOffsets = std::move(stream.headerOffsets);
    stream.finalHeaderOffset = page.offset;
    stream.phase = HeaderPhase::Done;
    stream.capturing = false;
    return true;
}

bool FinishPacket(StreamState& stream, const Page& page, ScanContext& context, std::wstring& error) {
    switch (stream.phase) {
    case HeaderPhase::First:
        if (stream.kind == FirstKind::Undecided) {
            stream.phase = HeaderPhase::Other;
            stream.capturing = false;
            FreeVector(stream.packet);
            FreeVector(stream.headerOffsets);
            return true;
        }
        if (stream.kind == FirstKind::Vorbis) {
            if (!IsVorbisIdentification(stream.packet)) return Fail(error, L"The Ogg Vorbis identification header is malformed.");
            return ActivateAudio(stream, EditableCodec::Vorbis, context, error);
        }
        if (!IsOpusIdentification(stream.packet)) return Fail(error, L"The Ogg Opus identification header is malformed.");
        return ActivateAudio(stream, EditableCodec::Opus, context, error);

    case HeaderPhase::Comment: {
        if (stream.plan == kNoPlan || stream.plan >= context.plans.size()) return Fail(error, L"Internal Ogg comment tracking error.");
        LinkPlan& plan = context.plans[stream.plan];
        std::vector<Byte> replacement;
        if (!BuildCommentPacket(plan.codec, stream.packet, context.requested, replacement, error)) return false;
        if (!RetainHeaderBytes(context, replacement.size(), error)) return false;
        plan.comment = std::move(replacement);
        FreeVector(stream.packet);
        if (plan.codec == EditableCodec::Vorbis) {
            stream.phase = HeaderPhase::Setup;
            stream.capturing = false;
            return true;
        }
        return CompleteHeaders(stream, page, context, error);
    }

    case HeaderPhase::Setup:
        if (!IsVorbisSetup(stream.packet)) return Fail(error, L"The Ogg Vorbis setup header is malformed.");
        if (stream.plan == kNoPlan || stream.plan >= context.plans.size()) return Fail(error, L"Internal Ogg setup tracking error.");
        if (!RetainHeaderBytes(context, stream.packet.size(), error)) return false;
        context.plans[stream.plan].setup = std::move(stream.packet);
        return CompleteHeaders(stream, page, context, error);

    case HeaderPhase::Done:
    case HeaderPhase::Other:
        return true;
    }
    return Fail(error, L"Internal Ogg packet state error.");
}

bool MarkHeaderPage(StreamState& stream, const Page& page) {
    if (stream.phase != HeaderPhase::First && stream.phase != HeaderPhase::Comment && stream.phase != HeaderPhase::Setup) return true;
    stream.headerOffsets.push_back(page.offset);
    if (!page.HeaderGranuleIsNonAudio() || page.eos) stream.headerPageSafe = false;
    return true;
}

bool ScanInput(HANDLE file, const std::vector<RequestedTag>& requested, std::vector<LinkPlan>& plans, std::wstring& error) {
    if (!SeekToBeginning(file, error)) return false;
    ScanContext context{requested};
    std::unordered_map<std::uint32_t, StreamState> streams;
    streams.reserve(32);
    std::uint64_t offset = 0;

    for (;;) {
        Page page;
        const PageRead status = ReadPage(file, offset, page, error);
        if (status == PageRead::End) break;
        if (status == PageRead::Error) return false;

        StreamState* stream = nullptr;
        if (page.bos) {
            if (page.continued) return Fail(error, L"An Ogg BOS page illegally continues a packet.");
            if (streams.find(page.serial) != streams.end()) return Fail(error, L"The Ogg file reuses a logical-stream serial number.");
            if (streams.size() >= kMaxLogicalStreams) return Fail(error, L"The Ogg file has too many logical streams.");
            StreamState created;
            created.serial = page.serial;
            created.bosSequence = page.sequence;
            created.bosOffset = page.offset;
            auto inserted = streams.emplace(page.serial, std::move(created));
            stream = &inserted.first->second;
        } else {
            const auto found = streams.find(page.serial);
            if (found == streams.end() || !found->second.active) {
                return Fail(error, L"The Ogg file has a page without an active logical stream.");
            }
            stream = &found->second;
        }

        if (stream->haveSequence && page.sequence != stream->nextSequence) {
            return Fail(error, L"The Ogg file has non-contiguous page sequence numbers.");
        }
        stream->haveSequence = true;
        stream->nextSequence = page.sequence + 1;
        if (page.continued != stream->packetOpen) {
            return Fail(error, L"The Ogg file has an invalid packet-continuation flag.");
        }
        MarkHeaderPage(*stream, page);

        const Byte* body = page.Body();
        std::size_t bodyAt = 0;
        const Byte* lacing = page.Segments();
        const std::size_t segments = page.headerSize - 27;
        for (std::size_t segment = 0; segment != segments; ++segment) {
            if (!stream->packetOpen) {
                if (stream->phase == HeaderPhase::Done && stream->finalHeaderOffset == page.offset) {
                    return Fail(error, L"An audio packet shares the final Ogg header page.");
                }
                stream->capturing = stream->phase == HeaderPhase::First || stream->phase == HeaderPhase::Comment || stream->phase == HeaderPhase::Setup;
                if (stream->capturing) stream->packet.clear();
            }
            const std::size_t bytes = lacing[segment];
            if (!AppendPacketData(*stream, body + bodyAt, bytes, error)) return false;
            bodyAt += bytes;
            if (bytes < 255) {
                stream->packetOpen = false;
                if (!FinishPacket(*stream, page, context, error)) return false;
            } else {
                stream->packetOpen = true;
            }
        }
        if (bodyAt != page.bodySize) return Fail(error, L"The Ogg page lacing table is malformed.");

        if (page.eos) {
            if (stream->packetOpen) return Fail(error, L"The Ogg file ends a logical stream in the middle of a packet.");
            if (stream->phase != HeaderPhase::Other && stream->phase != HeaderPhase::Done) {
                return Fail(error, L"The Ogg audio stream ends before all required headers.");
            }
            stream->active = false;
            if (stream->audio) --context.activeAudio;
        }
    }

    if (streams.empty()) return Fail(error, L"The file does not contain Ogg pages.");
    for (const auto& entry : streams) {
        if (entry.second.active) return Fail(error, L"The Ogg file has a logical stream without an EOS page.");
    }
    if (context.activeAudio != 0) return Fail(error, L"The Ogg file has an unfinished audio logical stream.");
    if (context.plans.empty()) return Fail(error, L"No editable Ogg Vorbis or Opus stream was found.");
    plans = std::move(context.plans);
    return true;
}

bool GenerateHeaderPages(const LinkPlan& plan, std::vector<std::vector<Byte>>& pages, std::wstring& error) {
    if (plan.identification.empty() || plan.comment.empty() ||
        (plan.codec == EditableCodec::Vorbis && plan.setup.empty())) {
        return Fail(error, L"The Ogg audio headers are incomplete.");
    }
    ogg_stream_state stream{};
    if (ogg_stream_init(&stream, static_cast<int>(plan.serial)) != 0) {
        return Fail(error, L"Cannot initialize the Ogg page writer.");
    }
    auto clear = [&]() { ogg_stream_clear(&stream); };
    auto packetIn = [&](const std::vector<Byte>& bytes, long bos) -> bool {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
            return Fail(error, L"An Ogg header packet is too large.");
        }
        ogg_packet packet{};
        packet.packet = const_cast<Byte*>(bytes.data());
        packet.bytes = static_cast<long>(bytes.size());
        packet.b_o_s = bos;
        packet.e_o_s = 0;
        packet.granulepos = 0;
        if (ogg_stream_packetin(&stream, &packet) != 0) return Fail(error, L"Cannot queue an Ogg header packet.");
        return true;
    };

    if (!packetIn(plan.identification, 1) || !packetIn(plan.comment, 0) ||
        (plan.codec == EditableCodec::Vorbis && !packetIn(plan.setup, 0))) {
        clear();
        return false;
    }

    pages.clear();
    ogg_page generated{};
    while (ogg_stream_flush(&stream, &generated) != 0) {
        if (generated.header_len < 27 || generated.body_len < 0 || pages.size() == std::numeric_limits<std::size_t>::max()) {
            clear();
            return Fail(error, L"The Ogg page writer returned an invalid page.");
        }
        const std::size_t header = static_cast<std::size_t>(generated.header_len);
        const std::size_t body = static_cast<std::size_t>(generated.body_len);
        std::vector<Byte> raw;
        raw.reserve(header + body);
        raw.insert(raw.end(), generated.header, generated.header + header);
        raw.insert(raw.end(), generated.body, generated.body + body);
        if (raw.size() < 27 || (raw[5] & 0x04) != 0) {
            clear();
            return Fail(error, L"The Ogg page writer generated an invalid header page.");
        }
        PutLe32(raw.data() + 18, plan.firstSequence + static_cast<std::uint32_t>(pages.size()));
        SetPageChecksum(raw, header, body);
        pages.push_back(std::move(raw));
    }
    clear();
    if (pages.empty() || (pages.front()[5] & 0x02) == 0) {
        return Fail(error, L"The Ogg page writer did not generate a BOS header page.");
    }
    return true;
}

bool FinalizePlans(std::vector<LinkPlan>& plans, int link, std::wstring& error) {
    if (link < -1) return Fail(error, L"The requested Ogg link index is invalid.");
    if (link >= 0 && static_cast<std::size_t>(link) >= plans.size()) return Fail(error, L"The requested Ogg link does not exist.");
    for (std::size_t index = 0; index != plans.size(); ++index) {
        LinkPlan& plan = plans[index];
        plan.selected = link == -1 || static_cast<std::size_t>(link) == index;
        if (!plan.selected) continue;
        if (plan.headerOffsets.empty() || plan.headerOffsets.front() != plan.firstHeaderOffset) {
            return Fail(error, L"The Ogg header page layout is unsafe to rewrite.");
        }
        std::vector<std::vector<Byte>> generated;
        if (!GenerateHeaderPages(plan, generated, error)) return false;
        plan.replacementPageCount = generated.size();
        plan.sequenceDelta = static_cast<std::int64_t>(generated.size()) - static_cast<std::int64_t>(plan.headerOffsets.size());
    }
    return true;
}

bool RewriteToTemporary(HANDLE source, HANDLE destination, const std::vector<LinkPlan>& plans, std::wstring& error) {
    std::unordered_map<std::uint64_t, std::size_t> headerPages;
    std::unordered_map<std::uint32_t, std::size_t> selectedSerials;
    std::vector<bool> emitted(plans.size(), false);
    for (std::size_t index = 0; index != plans.size(); ++index) {
        const LinkPlan& plan = plans[index];
        if (!plan.selected) continue;
        if (!selectedSerials.emplace(plan.serial, index).second) return Fail(error, L"Duplicate Ogg audio serial numbers cannot be retagged safely.");
        for (const std::uint64_t offset : plan.headerOffsets) {
            if (!headerPages.emplace(offset, index).second) return Fail(error, L"Overlapping Ogg header pages cannot be retagged safely.");
        }
    }

    if (!SeekToBeginning(source, error)) return false;
    std::uint64_t offset = 0;
    for (;;) {
        Page page;
        const PageRead status = ReadPage(source, offset, page, error);
        if (status == PageRead::End) break;
        if (status == PageRead::Error) return false;

        const auto header = headerPages.find(page.offset);
        if (header != headerPages.end()) {
            const std::size_t index = header->second;
            const LinkPlan& plan = plans[index];
            if (page.offset == plan.firstHeaderOffset) {
                if (emitted[index]) return Fail(error, L"Duplicate Ogg header section during rewrite.");
                std::vector<std::vector<Byte>> generated;
                if (!GenerateHeaderPages(plan, generated, error)) return false;
                if (generated.size() != plan.replacementPageCount) return Fail(error, L"Ogg header pagination changed while rewriting.");
                for (const auto& output : generated) {
                    if (!WriteAll(destination, output.data(), output.size(), error)) return false;
                }
                emitted[index] = true;
            }
            continue;
        }

        const auto selected = selectedSerials.find(page.serial);
        if (selected != selectedSerials.end()) {
            const LinkPlan& plan = plans[selected->second];
            if (!emitted[selected->second]) return Fail(error, L"An Ogg audio page precedes its replacement header section.");
            PutLe32(page.raw.data() + 18, page.sequence + static_cast<std::uint32_t>(plan.sequenceDelta));
            SetPageChecksum(page.raw, page.headerSize, page.bodySize);
        }
        if (!WriteAll(destination, page.raw.data(), page.raw.size(), error)) return false;
    }

    for (std::size_t index = 0; index != plans.size(); ++index) {
        if (plans[index].selected && !emitted[index]) return Fail(error, L"An Ogg header section disappeared during rewrite.");
    }
    return true;
}

void RestoreBestEffortMetadata(const std::wstring& path, const FileSnapshot& original) noexcept {
    Handle replacement(CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (replacement.Valid()) {
        SetFileTime(replacement.Get(), &original.info.ftCreationTime, &original.info.ftLastAccessTime, &original.info.ftLastWriteTime);
    }
    DWORD attributes = original.info.dwFileAttributes & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
    if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
    SetFileAttributesW(path.c_str(), attributes);
}

bool ExtractFields(const LinkPlan& plan, TagFields& fields, std::wstring& error) {
    // ScanInput already validated the packet's lengths and framing. Text that
    // cannot round-trip as Unicode is opaque and stays untouched by this editor.
    const auto& packet = plan.comment;
    std::size_t at = plan.codec == EditableCodec::Vorbis ? 7 : 8;
    std::uint32_t vendor = 0, count = 0;
    if (!TakeLe32(packet, at, vendor) || vendor > packet.size() - at) return false;
    at += vendor;
    if (!TakeLe32(packet, at, count)) return false;
    fields.clear();
    for (std::uint32_t i = 0; i != count; ++i) {
        std::uint32_t length = 0;
        if (!TakeLe32(packet, at, length) || length > packet.size() - at) {
            return Fail(error, L"The Ogg comment header has a truncated field.");
        }
        const Byte* text = packet.data() + at;
        at += length;
        std::string canonical;
        if (!IsFieldName(text, length, canonical) || IsArtworkKey(canonical)) continue;
        const Byte* equal = std::find(text, text + length, static_cast<Byte>('='));
        const Byte* value = equal + 1;
        const int bytes = static_cast<int>(text + length - value);
        if (std::find(value, value + bytes, Byte{0}) != value + bytes) continue;
        std::wstring wide;
        if (bytes) {
            const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(value), bytes, nullptr, 0);
            if (!chars) continue;
            wide.resize(chars);
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(value), bytes, wide.data(), chars)) continue;
        }
        fields.push_back({std::string(reinterpret_cast<const char*>(text),
            static_cast<std::size_t>(equal - text)), std::move(wide)});
    }
    return true;
}

bool PrepareDocumentEdits(const TagFields& original, const TagFields& edited,
                         std::vector<RequestedTag>& requested, std::wstring& error) {
    if (edited.size() > kMaxCommentEntries) return Fail(error, L"Too many requested Ogg comment fields.");
    std::map<std::string, TagFields> before, after;
    for (const auto& field : original) {
        std::string canonical;
        if (!IsFieldName(field.name, canonical)) return Fail(error, L"An Ogg comment field name is invalid.");
        before[canonical].push_back(field);
    }
    std::size_t total = 0;
    std::vector<RequestedTag> converted;
    for (const auto& field : edited) {
        RequestedTag tag;
        tag.name = field.name;
        if (!IsFieldName(tag.name, tag.canonical)) return Fail(error, L"An Ogg comment field name is invalid.");
        if (IsArtworkKey(tag.canonical)) return Fail(error, L"Embedded artwork comments are preserved and cannot be edited here.");
        if (!ToUtf8Checked(field.value, tag.value, error)) return false;
        if (tag.name.size() > kMaxHeaderBytes || tag.value.size() > kMaxHeaderBytes ||
            tag.name.size() + 1 > kMaxHeaderBytes - tag.value.size()) {
            return Fail(error, L"A requested Ogg comment is too large.");
        }
        const std::size_t size = 4 + tag.name.size() + 1 + tag.value.size();
        if (size > kMaxHeaderBytes - total) return Fail(error, L"Requested Ogg comments exceed the 16 MiB safety limit.");
        total += size;
        after[tag.canonical].push_back(field);
        converted.push_back(std::move(tag));
    }
    requested.clear();
    std::unordered_set<std::string> changed;
    for (const auto& entry : after) {
        const auto found = before.find(entry.first);
        if (found == before.end() || found->second != entry.second) changed.insert(entry.first);
    }
    for (auto& tag : converted) {
        if (changed.find(tag.canonical) != changed.end()) requested.push_back(std::move(tag));
    }
    for (const auto& entry : before) {
        if (after.find(entry.first) == after.end()) requested.push_back({entry.first, entry.first, {}, true});
    }
    return true;
}

bool WritePrepared(const wchar_t* path, int link, const std::vector<RequestedTag>& requested,
                   const TagDocument* expected, std::wstring& error) {
    std::wstring sourcePath;
    if (!AbsolutePath(path, sourcePath, error)) return false;
    Handle source;
    FileSnapshot original{};
    if (!OpenSafeSource(sourcePath, source, original, error)) return false;
    std::vector<LinkPlan> plans;
    const std::vector<RequestedTag> noChanges;
    if (!ScanInput(source.Get(), expected ? noChanges : requested, plans, error)) return false;
    if (expected) {
        if (link < 0 || static_cast<std::size_t>(link) >= plans.size()) return Fail(error, L"The requested Ogg link does not exist.");
        TagFields current;
        auto& plan = plans[link];
        if (!ExtractFields(plan, current, error)) return false;
        if (plan.serial != expected->serial || current != expected->fields) {
            return Fail(error, L"The file's tags changed after this editor was opened. Reopen File Information before saving.");
        }
        std::vector<Byte> replacement;
        if (!BuildCommentPacket(plan.codec, plan.comment, requested, replacement, error)) return false;
        std::size_t retained = 0;
        for (const auto& item : plans) {
            const std::size_t bytes = item.identification.size() + item.setup.size() +
                (&item == &plan ? replacement.size() : item.comment.size());
            if (bytes > kMaxHeaderBytes - retained) return Fail(error, L"Ogg headers and comments exceed the 16 MiB safety limit.");
            retained += bytes;
        }
        plan.comment = std::move(replacement);
    }
    if (!FinalizePlans(plans, link, error)) return false;
    if (!CurrentSnapshot(source.Get(), original, error)) return false;
    TemporaryOutput temporary;
    if (!temporary.Create(sourcePath, error)) return false;
    if (!RewriteToTemporary(source.Get(), temporary.File(), plans, error)) return false;
    if (!CurrentSnapshot(source.Get(), original, error)) return false;
    if (!SourceStillAtPath(sourcePath, original, error)) return false;
    if (!temporary.FlushAndClose(error)) return false;
    if (!ReplaceFileW(sourcePath.c_str(), temporary.Path().c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return FailWin32(error, L"Cannot atomically replace the audio file");
    }
    temporary.KeepReplacement();
    RestoreBestEffortMetadata(sourcePath, original);
    return true;
}

} // namespace

bool ReadTagDocument(const wchar_t* path, int link, TagDocument& document, std::wstring& error) {
    error.clear();
    try {
        std::wstring resolved;
        if (!AbsolutePath(path, resolved, error)) return false;
        Handle source;
        FileSnapshot snapshot{};
        if (!OpenSafeSource(resolved, source, snapshot, error)) return false;
        std::vector<LinkPlan> plans;
        if (!ScanInput(source.Get(), {}, plans, error)) return false;
        if (link < 0 || static_cast<std::size_t>(link) >= plans.size()) return Fail(error, L"The requested Ogg link does not exist.");
        TagDocument result;
        result.link = link;
        result.serial = plans[link].serial;
        if (!ExtractFields(plans[link], result.fields, error) || !CurrentSnapshot(source.Get(), snapshot, error)) return false;
        document = std::move(result);
        return true;
    } catch (...) {
        return Fail(error, L"Cannot read the Ogg comments for editing.");
    }
}

bool WriteTagDocument(const wchar_t* path, const TagDocument& original,
                      const TagFields& edited, std::wstring& error) {
    error.clear();
    try {
        std::vector<RequestedTag> requested;
        if (!PrepareDocumentEdits(original.fields, edited, requested, error)) return false;
        if (requested.empty()) return true;
        return WritePrepared(path, original.link, requested, &original, error);
    } catch (...) {
        return Fail(error, L"Unexpected failure while writing Ogg tags.");
    }
}

bool WriteTags(const wchar_t* path, int link, const Tags& tags, std::wstring& error) {
    error.clear();
    try {
        std::vector<RequestedTag> requested;
        if (!PrepareRequestedTags(tags, requested, error)) return false;
        return WritePrepared(path, link, requested, nullptr, error);
    } catch (const std::bad_alloc&) {
        return Fail(error, L"Not enough memory to process Ogg tags safely.");
    } catch (...) {
        return Fail(error, L"Unexpected failure while writing Ogg tags.");
    }
}

} // namespace ogg_input
