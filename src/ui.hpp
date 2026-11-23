#pragma once
#include "backend.hpp"
#include <windows.h>
namespace ogg_input {
Settings Configure(HINSTANCE, HWND parent, Settings, const std::wstring& status);
void ShowInfo(HINSTANCE, HWND parent, const TrackInfo&);
HWND UnifiedPane(HINSTANCE, HWND parent, const TrackInfo&);
void ShowAbout(HINSTANCE, HWND parent);
}
