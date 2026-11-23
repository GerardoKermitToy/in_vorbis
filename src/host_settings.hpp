// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "settings.hpp"

#include <windows.h>

#include <cstddef>

namespace ogg_input {

// Take a transient, read-only snapshot on the host/UI thread, before starting
// playback/probe workers. Never save the returned snapshot: local preferences
// and the two use_host_* switches are the persistent settings. Missing services
// or individual unsupported/invalid items leave their local fallbacks intact.
Settings ResolveHostSettings(HWND host, Settings local) noexcept;

namespace host_settings {

// Independent, minimal declarations of the public Winamp/Wasabi ABI. The only
// virtual member is _dispatch; adding a virtual destructor changes that ABI.
// Names, GUIDs and dispatch numbers were checked against the public headers:
// Src/Wasabi/api/service/{api_service,waservicefactory}.h,
// Src/Agave/Config/{api_config,ifc_configgroup,ifc_configitem}.h and
// Src/Winamp/{PlaybackConfigGroup,InternetConfigGroup}.h in the Winamp SDK.
// No SDK implementation or historical input-plugin implementation is included.
struct Dispatchable {
    virtual int _dispatch(int message, void* result, void** arguments = nullptr,
        int argument_count = 0) = 0;
};

inline constexpr UINT kIpcMessage = WM_USER;
inline constexpr LPARAM kGetApiService = 3025;
inline constexpr std::size_t kMaxProxyCharacters = 2048;

inline constexpr GUID kConfigServiceGuid = {
    0xAEFBF8BE, 0xE0AA, 0x4318, {0x8C, 0xC1, 0x43, 0x53, 0x41, 0x0B, 0x64, 0xDC}
};
inline constexpr GUID kPlaybackGroupGuid = {
    0xB6CB4A7C, 0xA8D0, 0x4C55, {0x8E, 0x60, 0x9F, 0x7A, 0x7A, 0x23, 0xDA, 0x0F}
};
inline constexpr GUID kInternetGroupGuid = {
    0xC0A565DC, 0x0CFE, 0x405A, {0xA2, 0x7C, 0x46, 0x8B, 0x0C, 0x8A, 0x3A, 0x5C}
};

namespace Message {
inline constexpr int FindService = 50;
inline constexpr int GetInterface = 300;
inline constexpr int ReleaseInterface = 310;
inline constexpr int GetGroup = 10;
inline constexpr int GetItem = 10;
inline constexpr int GetString = 30;
inline constexpr int GetUnsigned = 70;
inline constexpr int GetBool = 90;
inline constexpr int GetFloat = 190;
} // namespace Message

} // namespace host_settings
} // namespace ogg_input
