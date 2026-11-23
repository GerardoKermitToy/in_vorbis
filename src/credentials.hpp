#pragma once
#include <windows.h>
#include <string>
namespace ogg_input {
// A native prompt supplies credentials only for this connection; they are never
// persisted to the registry or included in logs, filenames, or metadata.
bool AskCredentials(HWND parent, bool proxy, const std::wstring& server,
                    std::wstring& user, std::wstring& password);
}
