#pragma once
#include "backend.hpp"
namespace ogg_input {
// Preserve encoded audio and other comments. Apply field updates for one link,
// or every audio link when link is -1. An empty value deletes that field.
// Fields absent from the map and embedded artwork are preserved. Atomic replacement.
bool WriteTags(const wchar_t* path, int link, const Tags&, std::wstring& error);
}
