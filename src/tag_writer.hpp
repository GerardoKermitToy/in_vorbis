#pragma once
#include "backend.hpp"
namespace ogg_input {
struct TagField {
    std::string name;
    std::wstring value;
    bool operator==(const TagField& other) const { return name == other.name && value == other.value; }
    bool operator!=(const TagField& other) const { return !(*this == other); }
};
using TagFields = std::vector<TagField>;
// A separate editor snapshot retains repeated fields instead of the playback
// map's joined display values. Artwork and opaque comments never enter the UI.
struct TagDocument {
    int link = 0;
    std::uint32_t serial = 0;
    TagFields fields;
};
bool ReadTagDocument(const wchar_t* path, int link, TagDocument&, std::wstring& error);
bool WriteTagDocument(const wchar_t* path, const TagDocument& original,
                      const TagFields& edited, std::wstring& error);
// Preserve encoded audio and other comments. Apply field updates for one link,
// or every audio link when link is -1. An empty value deletes that field.
// Fields absent from the map and embedded artwork are preserved. Atomic replacement.
bool WriteTags(const wchar_t* path, int link, const Tags&, std::wstring& error);
}
