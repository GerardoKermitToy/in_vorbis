// SPDX-License-Identifier: GPL-3.0-only
#include "codec_fixtures.hpp"
#include "registry_sandbox.hpp"
#include "winamp_input.hpp"
#include "resource.h"
#include "dialog_capture.hpp"
#include "audio.hpp"
#include <commctrl.h>
#include <iostream>

namespace {
HHOOK hook = nullptr;
std::string failure;
int seen = 0, config_count = 0;
std::filesystem::path screenshot_directory;
BOOL CALLBACK Bounds(HWND child, LPARAM param) {
    const auto parent = reinterpret_cast<HWND>(param);
    if (!IsWindowVisible(child)) return TRUE;
    RECT rectangle{}, client{};
    GetWindowRect(child, &rectangle);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rectangle), 2);
    GetClientRect(parent, &client);
    if (rectangle.left < 0 || rectangle.top < 0 || rectangle.right > client.right ||
        rectangle.bottom > client.bottom) failure = "visible dialog control clipped";
    return TRUE;
}
void Click(HWND window, int control) {
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(control, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, control)));
}
void Screenshot(HWND window, const std::wstring& name) {
    const auto path = screenshot_directory / (name + L".bmp");
    CaptureDialog(window, path.c_str());
}
void CALLBACK Inspect(HWND window, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(window, timer);
    try {
        wchar_t caption[128]{};
        GetWindowTextW(window, caption, 128);
        Check(caption[0], "dialog has caption");
        EnumChildWindows(window, Bounds, reinterpret_cast<LPARAM>(window));
        if (GetDlgItem(window, IDC_INFO_PATH)) {
            Check(GetWindowLongPtrW(GetDlgItem(window, IDC_INFO_PATH), GWL_STYLE) & ES_READONLY,
                  "file path read-only");
            Check(GetDlgItem(window, IDC_TAG_TITLE) && GetDlgItem(window, IDC_INFO_DETAILS),
                  "classic information panes");
            SetDlgItemTextW(window, IDC_TAG_TITLE, L"UI Unicode title — テスト");
            SetDlgItemTextW(window, IDC_TAG_NAME, L"ui-custom");
            SetDlgItemTextW(window, IDC_TAG_VALUE, L"Saved, then deleted");
            Click(window, IDC_TAG_ADD_UPDATE);
            Click(window, IDC_INFO_SAVE);
            const auto list = GetDlgItem(window, IDC_TAG_LIST);
            Check(ListView_GetItemCount(list) == 1, "custom tag shown after save");
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                 LVIS_SELECTED | LVIS_FOCUSED);
            Click(window, IDC_TAG_DELETE);
            Click(window, IDC_INFO_SAVE);
            Check(ListView_GetItemCount(list) == 0, "custom tag removed in UI");
        }
        if (const auto tabs = GetDlgItem(window, IDC_CONFIG_TABS)) {
            Check(TabCtrl_GetItemCount(tabs) == 5, "five settings tabs");
            for (int page = 0; page != 5; ++page) {
                TabCtrl_SetCurSel(tabs, page);
                NMHDR notification{tabs, IDC_CONFIG_TABS, TCN_SELCHANGE};
                SendMessageW(window, WM_NOTIFY, IDC_CONFIG_TABS,
                             reinterpret_cast<LPARAM>(&notification));
                EnumChildWindows(window, Bounds, reinterpret_cast<LPARAM>(window));
                Check(IsWindowVisible(GetDlgItem(window, IDC_CONFIG_PEAK_MODE)) == (page == 1),
                      "ReplayGain controls follow active tab");
                Check(IsWindowVisible(GetDlgItem(window, IDC_CONFIG_DITHER_PDF)) == (page == 4),
                      "processing controls follow active tab");
                Check(IsWindowVisible(GetDlgItem(window, IDC_CONFIG_HOST_PROXY)) == (page == 3),
                      "host proxy follows advanced tab");
                for (int control : {IDC_CONFIG_EXACT_FLAC, IDC_CONFIG_OPUS_HD, IDC_CONFIG_OPUS_SPEECH,
                                    IDC_CONFIG_OPUS_BWE, IDC_CONFIG_AMBISONIC})
                    Check(IsWindowVisible(GetDlgItem(window, control)) == (page == 4),
                          "advanced decoding controls follow Processing tab");
                Screenshot(window, L"config-" + std::to_wstring(config_count) +
                           L"-tab-" + std::to_wstring(page));
            }
            if (config_count++ == 0) {
                SendDlgItemMessageW(window, IDC_CONFIG_PEAK_MODE, CB_SETCURSEL, 2, 0);
                SetDlgItemTextW(window, IDC_CONFIG_NON_RG, L"-9.0");
                CheckDlgButton(window, IDC_CONFIG_PREFERRED_ONLY, BST_CHECKED);
                CheckDlgButton(window, IDC_CONFIG_NATIVE_CHAINS, BST_UNCHECKED);
                CheckDlgButton(window, IDC_CONFIG_HOST_REPLAYGAIN, BST_UNCHECKED);
                CheckDlgButton(window, IDC_CONFIG_HOST_PROXY, BST_UNCHECKED);
                SendDlgItemMessageW(window, IDC_CONFIG_DITHER_PDF, CB_SETCURSEL, 2, 0);
                SendDlgItemMessageW(window, IDC_CONFIG_NOISE_SHAPE, CB_SETCURSEL, 3, 0);
                CheckDlgButton(window, IDC_CONFIG_EXACT_FLAC, BST_UNCHECKED);
                CheckDlgButton(window, IDC_CONFIG_OPUS_HD, BST_UNCHECKED);
                CheckDlgButton(window, IDC_CONFIG_OPUS_SPEECH, BST_CHECKED);
                CheckDlgButton(window, IDC_CONFIG_OPUS_BWE, BST_CHECKED);
                SendDlgItemMessageW(window, IDC_CONFIG_AMBISONIC, CB_SETCURSEL, 1, 0);
                ++seen;
                Click(window, IDOK);
                return;
            }
        }
        Screenshot(window, L"dialog-" + std::to_wstring(seen));
        ++seen;
        PostMessageW(window, WM_COMMAND, IDCANCEL, 0);
    } catch (const std::exception& error) {
        failure = error.what();
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
}
LRESULT CALLBACK OnDialog(int code, WPARAM w, LPARAM l) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<CWPRETSTRUCT*>(l);
        if (message.message == WM_INITDIALOG) SetTimer(message.hwnd, 42, 100, Inspect);
    }
    return CallNextHookEx(hook, code, w, l);
}
}
int wmain(int argc, wchar_t** argv) {
    RegistrySandbox registry;
    HMODULE dll = nullptr;
    try {
        Check(argc == 3, "usage dialog_tests DLL DIRECTORY");
        screenshot_directory = std::filesystem::path(argv[2]);
        std::filesystem::create_directories(screenshot_directory);
        const auto path = screenshot_directory / L"metadata.ogg";
        MakeVorbis(path, 2, 44100, 7013, 9001);
        dll = LoadLibraryExW(argv[1], nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(dll, "load UI DLL");
        const auto get = reinterpret_cast<winamp::InModule*(__cdecl*)()>(
            GetProcAddress(dll, "winampGetInModule2"));
        Check(get, "UI module entry");
        auto* module = get();
        module->hDllInstance = dll;
        module->Init();
        hook = SetWindowsHookExW(WH_CALLWNDPROCRET, OnDialog, nullptr, GetCurrentThreadId());
        Check(hook, "dialog hook");
        module->InfoBox(path.c_str(), nullptr);
        ogg_input::Audio audio;
        ogg_input::Settings settings;
        std::wstring error;
        Check(audio.Open(path.c_str(), settings, false, error), "reopen UI-edited file");
        Check(audio.Info().current.tags.at("title") == L"UI Unicode title — テスト",
              "UI save writes title");
        Check(audio.Info().current.tags.find("ui-custom") == audio.Info().current.tags.end(),
              "UI deletion writes a tombstone");
        module->Config(nullptr);
        DWORD peak = 0, bytes = sizeof(peak);
        Check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\WinampOgg\\Input", L"PeakMode",
                           RRF_RT_REG_DWORD, nullptr, &peak, &bytes) == ERROR_SUCCESS && peak == 2,
              "UI saves selected peak mode");
        for (const auto* name : {L"NativeChains", L"UseHostReplayGain", L"UseHostProxy", L"ExactFlac", L"OpusHd"}) {
            DWORD stored = 1; bytes = sizeof(stored);
            Check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\WinampOgg\\Input", name,
                               RRF_RT_REG_DWORD, nullptr, &stored, &bytes) == ERROR_SUCCESS && stored == 0,
                  "UI saves independent inheritance/native-chain switches");
        }
        for (const auto& expected : {std::pair<const wchar_t*, DWORD>{L"DitherPdf", 2}, {L"NoiseShape", 3}, {L"OpusSpeech", 1}, {L"OpusBwe", 1}, {L"AmbisonicMode", 1}}) {
            DWORD stored = 0; bytes = sizeof(stored);
            Check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\WinampOgg\\Input", expected.first,
                               RRF_RT_REG_DWORD, nullptr, &stored, &bytes) == ERROR_SUCCESS && stored == expected.second,
                  "UI saves processing choices");
        }
        module->Config(nullptr); // Exercise Cancel as well as OK.
        module->About(nullptr);
        UnhookWindowsHookEx(hook);
        hook = nullptr;
        Check(failure.empty(), failure.c_str());
        Check(seen == 4 && config_count == 2, "all native dialogs inspected");
        module->Quit();
        FreeLibrary(dll);
        dll = nullptr;
        std::cout << "Native dialogs: all tabs, clipping, tag save/delete, settings OK/Cancel passed.\n";
        return 0;
    } catch (const std::exception& error) {
        if (hook) UnhookWindowsHookEx(hook);
        if (dll) FreeLibrary(dll);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
