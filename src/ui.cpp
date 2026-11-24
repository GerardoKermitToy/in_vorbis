// SPDX-License-Identifier: GPL-3.0-only
#include "ui.hpp"
#include "tag_writer.hpp"
#include "tag_editor.hpp"
#include "resource.h"
#include "language.hpp"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace ogg_input {
namespace {

int UiMessageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT flags) {
    return MessageBoxW(owner, Localized(text).c_str(), Localized(caption).c_str(), flags);
}

struct StandardField {
    int control;
    const char* key;
};

constexpr StandardField kStandardFields[] = {
    {IDC_TAG_TITLE, "title"},
    {IDC_TAG_ARTIST, "artist"},
    {IDC_TAG_ALBUM, "album"},
    {IDC_TAG_TRACK, "tracknumber"},
    {IDC_TAG_DATE, "date"},
    {IDC_TAG_GENRE, "genre"},
    {IDC_TAG_COMMENT, "comment"},
};

void InitializeCommonControls() noexcept {
    static const bool initialized = [] {
        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES;
        return InitCommonControlsEx(&controls) != FALSE;
    }();
    (void)initialized;
}

std::wstring ControlText(HWND window) {
    if (!window) return {};
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    return text.data();
}

std::wstring ControlText(HWND dialog, int control) {
    return ControlText(GetDlgItem(dialog, control));
}

void SetControlText(HWND dialog, int control, const std::wstring& text) {
    SetDlgItemTextW(dialog, control, text.c_str());
}

std::wstring Trim(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t c) {
        return std::iswspace(static_cast<wint_t>(c)) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t c) {
        return std::iswspace(static_cast<wint_t>(c)) != 0;
    }).base();
    if (first >= last) return {};
    return {first, last};
}

bool ParseInteger(const std::wstring& entered, int& result) noexcept {
    const std::wstring value = Trim(entered);
    if (value.empty()) return false;

    size_t at = 0;
    bool negative = false;
    if (value[at] == L'+' || value[at] == L'-') {
        negative = value[at] == L'-';
        if (++at == value.size()) return false;
    }

    const unsigned long long limit = negative
        ? static_cast<unsigned long long>(std::numeric_limits<int>::max()) + 1ULL
        : static_cast<unsigned long long>(std::numeric_limits<int>::max());
    unsigned long long parsed = 0;
    for (; at < value.size(); ++at) {
        const wchar_t c = value[at];
        if (c < L'0' || c > L'9') return false;
        const unsigned digit = static_cast<unsigned>(c - L'0');
        if (parsed > (limit - digit) / 10ULL) return false;
        parsed = parsed * 10ULL + digit;
    }

    if (negative) {
        if (parsed == limit) result = std::numeric_limits<int>::min();
        else result = -static_cast<int>(parsed);
    } else {
        result = static_cast<int>(parsed);
    }
    return true;
}

bool ParseDb(const std::wstring& entered, int& q8) noexcept {
    const std::wstring value = Trim(entered);
    if (value.empty()) return false;
    wchar_t* end = nullptr;
    const double db = std::wcstod(value.c_str(), &end);
    if (end == value.c_str() || *end != L'\0' || !std::isfinite(db) || db < -60.0 || db > 30.0) {
        return false;
    }
    q8 = static_cast<int>(std::lround(db * 256.0));
    return q8 >= -60 * 256 && q8 <= 30 * 256;
}

std::wstring FormatDb(int q8) {
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << static_cast<double>(q8) / 256.0;
    return out.str();
}

bool ShowInvalid(HWND dialog, int control, const wchar_t* message) {
    UiMessageBox(dialog, message, L"Ogg Input Settings", MB_OK | MB_ICONWARNING);
    HWND edit = GetDlgItem(dialog, control);
    if (edit) {
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
    }
    return false;
}

bool ReadRangedInteger(HWND dialog, int control, int minimum, int maximum,
                       const wchar_t* label, int& value) {
    if (!ParseInteger(ControlText(dialog, control), value) || value < minimum || value > maximum) {
        std::wstring message = Localized(label) + Localized(L" must be between ") +
            std::to_wstring(minimum) + Localized(L" and ") + std::to_wstring(maximum) + L".";
        return ShowInvalid(dialog, control, message.c_str());
    }
    return true;
}

void AddComboItem(HWND control, const wchar_t* text, int data) {
    const auto localized = Localized(text);
    const LRESULT item = SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(localized.c_str()));
    if (item != CB_ERR && item != CB_ERRSPACE) {
        SendMessageW(control, CB_SETITEMDATA, static_cast<WPARAM>(item), static_cast<LPARAM>(data));
    }
}

void SelectComboData(HWND control, int wanted) {
    const int count = static_cast<int>(SendMessageW(control, CB_GETCOUNT, 0, 0));
    for (int item = 0; item < count; ++item) {
        if (static_cast<int>(SendMessageW(control, CB_GETITEMDATA, item, 0)) == wanted) {
            SendMessageW(control, CB_SETCURSEL, item, 0);
            return;
        }
    }
    SendMessageW(control, CB_SETCURSEL, 0, 0);
}

bool SelectedComboData(HWND dialog, int control, int& value) {
    HWND combo = GetDlgItem(dialog, control);
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) return false;
    const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    if (data == CB_ERR) return false;
    value = static_cast<int>(data);
    return true;
}

bool IsStandardTag(const std::string& key) {
    if (key == "description") return true;
    for (const auto& field : kStandardFields) {
        if (key == field.key) return true;
    }
    return false;
}

std::wstring WideTagKey(const std::string& key) {
    return {key.begin(), key.end()};
}

bool MakeTagKey(const std::wstring& entered, std::string& key) {
    const std::wstring name = Trim(entered);
    if (name.empty() || name.size() > 128) return false;
    key.clear();
    key.reserve(name.size());
    for (const wchar_t c : name) {
        if (c < 0x20 || c > 0x7d || c == L'=') return false;
        char ascii = static_cast<char>(c);
        if (ascii >= 'A' && ascii <= 'Z') ascii = static_cast<char>(ascii - 'A' + 'a');
        key.push_back(ascii);
    }
    return true;
}

std::wstring DisplayTagValue(const Tags& tags, const char* key) {
    auto found = tags.find(key);
    if (found != tags.end()) return found->second;
    if (std::string(key) == "comment") {
        found = tags.find("description");
        if (found != tags.end()) return found->second;
    }
    return {};
}

void SetStandardTagValue(Tags& tags, const char* requestedKey, const std::wstring& value) {
    const char* storageKey = requestedKey;
    if (std::string(requestedKey) == "comment" &&
        tags.find("comment") == tags.end() && tags.find("description") != tags.end()) {
        storageKey = "description";
    }
    if (value.empty()) tags.erase(storageKey);
    else tags[storageKey] = value;
}

struct InfoContext {
    explicit InfoContext(HINSTANCE moduleInstance, const TrackInfo& input)
        : module(moduleInstance), track(input) {}

    HINSTANCE module = nullptr;
    TrackInfo track;
    std::vector<Tags> savedTags;
    std::vector<Tags> draftTags;
    int selectedLink = 0;
    bool writable = false;
    std::wstring writableReason;
    std::wstring status;
};

void InitializeInfoContext(InfoContext& context) {
    if (context.track.links.empty()) context.track.links.push_back(context.track.current);
    context.selectedLink = std::clamp(context.track.current_link, 0,
                                      static_cast<int>(context.track.links.size()) - 1);
    // Readers refresh current while playback moves across chained streams. Keep
    // that live view for the selected link before taking independent UI drafts.
    context.track.links[context.selectedLink] = context.track.current;
    context.track.current_link = context.selectedLink;
    context.savedTags.reserve(context.track.links.size());
    context.draftTags.reserve(context.track.links.size());
    for (const LinkInfo& link : context.track.links) {
        context.savedTags.push_back(link.tags);
        context.draftTags.push_back(link.tags);
    }

    if (context.track.radio || IsHttp(context.track.path.c_str())) {
        context.writableReason = Localized(L"Read-only: radio and URL streams cannot be written.");
    } else if (context.track.path.empty()) {
        context.writableReason = Localized(L"Read-only: no local file path is available.");
    } else if (context.track.codec == Codec::Flac) {
        context.writableReason = Localized(L"Read-only: Ogg FLAC tag writing is not supported.");
    } else if (context.track.codec == Codec::Speex) {
        context.writableReason = Localized(L"Read-only: Ogg Speex tag writing is not supported.");
    } else if (context.track.codec != Codec::Vorbis && context.track.codec != Codec::Opus) {
        context.writableReason = Localized(L"Read-only: this stream format cannot be written.");
    } else {
        context.writable = true;
        context.writableReason = Localized(L"Editable local Ogg Vorbis or Ogg Opus tags.");
    }
    context.status = Localized(L"Changes are kept in this dialog until Save is selected.");
}

void SetCurrentLink(InfoContext& context) {
    LinkInfo& selected = context.track.links[context.selectedLink];
    selected.tags = context.draftTags[context.selectedLink];
    context.track.current = selected;
    context.track.current_link = context.selectedLink;
}

bool CommitStandardEdits(HWND dialog, InfoContext& context) {
    Tags& tags = context.draftTags[context.selectedLink];
    bool changed = false;
    for (const auto& field : kStandardFields) {
        const std::wstring before = DisplayTagValue(tags, field.key);
        const std::wstring after = ControlText(dialog, field.control);
        if (before != after) {
            SetStandardTagValue(tags, field.key, after);
            changed = true;
        }
    }
    if (changed) SetCurrentLink(context);
    return changed;
}

void ConfigureList(HWND dialog) {
    HWND list = GetDlgItem(dialog, IDC_TAG_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    auto name = Localized(L"Name");
    column.pszText = name.data();
    column.cx = 95;
    column.iSubItem = 0;
    ListView_InsertColumn(list, 0, &column);
    auto value = Localized(L"Value");
    column.pszText = value.data();
    column.cx = 175;
    column.iSubItem = 1;
    ListView_InsertColumn(list, 1, &column);
}

int PopulateOtherTags(HWND dialog, const InfoContext& context, const std::string* selectKey = nullptr) {
    HWND list = GetDlgItem(dialog, IDC_TAG_LIST);
    ListView_DeleteAllItems(list);
    const Tags& tags = context.draftTags[context.selectedLink];
    int selected = -1;
    int index = 0;
    for (const auto& tag : tags) {
        if (IsStandardTag(tag.first)) continue;
        const std::wstring name = WideTagKey(tag.first);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        const int inserted = ListView_InsertItem(list, &item);
        if (inserted >= 0) {
            ListView_SetItemText(list, inserted, 1, const_cast<wchar_t*>(tag.second.c_str()));
            if (selectKey && tag.first == *selectKey) selected = inserted;
            ++index;
        }
    }
    if (selected >= 0) {
        ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selected, FALSE);
    }
    return selected;
}

bool SelectedOtherTag(HWND dialog, std::string& key) {
    HWND list = GetDlgItem(dialog, IDC_TAG_LIST);
    const int item = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (item < 0) return false;
    std::array<wchar_t, 130> name{};
    ListView_GetItemText(list, item, 0, name.data(), static_cast<int>(name.size()));
    return MakeTagKey(name.data(), key);
}

void UpdateInfoEditability(HWND dialog, const InfoContext& context) {
    for (const auto& field : kStandardFields) {
        SendDlgItemMessageW(dialog, field.control, EM_SETREADONLY, context.writable ? FALSE : TRUE, 0);
    }
    SendDlgItemMessageW(dialog, IDC_TAG_NAME, EM_SETREADONLY, context.writable ? FALSE : TRUE, 0);
    SendDlgItemMessageW(dialog, IDC_TAG_VALUE, EM_SETREADONLY, context.writable ? FALSE : TRUE, 0);
    EnableWindow(GetDlgItem(dialog, IDC_TAG_ADD_UPDATE), context.writable ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_TAG_DELETE), context.writable ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_INFO_SAVE), context.writable ? TRUE : FALSE);
}

void RenderInfo(HWND dialog, InfoContext& context) {
    SetCurrentLink(context);
    SetControlText(dialog, IDC_INFO_PATH, context.track.path);
    for (const auto& field : kStandardFields) {
        SetControlText(dialog, field.control,
                       DisplayTagValue(context.draftTags[context.selectedLink], field.key));
    }
    SetControlText(dialog, IDC_TAG_NAME, {});
    SetControlText(dialog, IDC_TAG_VALUE, {});
    PopulateOtherTags(dialog, context);
    SetControlText(dialog, IDC_INFO_DETAILS, FormatInfo(context.track, Localized));
    SetControlText(dialog, IDC_INFO_WRITABLE_REASON, context.writableReason);
    SetControlText(dialog, IDC_INFO_STATUS, context.status);

    const int count = static_cast<int>(context.track.links.size());
    SetControlText(dialog, IDC_CHAIN_LABEL,
                   Localized(L"Link ") + std::to_wstring(context.selectedLink + 1) + Localized(L" of ") +
                   std::to_wstring(count));
    EnableWindow(GetDlgItem(dialog, IDC_CHAIN_PREVIOUS), context.selectedLink > 0 ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_CHAIN_NEXT), context.selectedLink + 1 < count ? TRUE : FALSE);
    UpdateInfoEditability(dialog, context);
}

bool HasUnsavedEdits(const InfoContext& context) {
    return context.draftTags != context.savedTags;
}

void SwitchLink(HWND dialog, InfoContext& context, int requested) {
    CommitStandardEdits(dialog, context);
    const int count = static_cast<int>(context.track.links.size());
    context.selectedLink = std::clamp(requested, 0, count - 1);
    context.status = Localized(L"Changes are kept in this dialog until Save is selected.");
    RenderInfo(dialog, context);
}

void AddOrUpdateOtherTag(HWND dialog, InfoContext& context) {
    CommitStandardEdits(dialog, context);
    std::string key;
    if (!MakeTagKey(ControlText(dialog, IDC_TAG_NAME), key)) {
        UiMessageBox(dialog,
                    L"A tag name must contain 1 to 128 printable ASCII characters and no '=' sign.",
                    L"File Information", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_TAG_NAME));
        return;
    }

    context.draftTags[context.selectedLink][key] = ControlText(dialog, IDC_TAG_VALUE);
    SetCurrentLink(context);
    const int selected = PopulateOtherTags(dialog, context, &key);
    if (selected >= 0) {
        SetFocus(GetDlgItem(dialog, IDC_TAG_LIST));
    }
    context.status = Localized(L"Tag change pending. Select Save to write it to the file.");
    SetControlText(dialog, IDC_INFO_STATUS, context.status);
}

void DeleteOtherTag(HWND dialog, InfoContext& context) {
    CommitStandardEdits(dialog, context);
    std::string key;
    if (!SelectedOtherTag(dialog, key)) {
        UiMessageBox(dialog, L"Select an Other Tags entry to delete.", L"File Information",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    context.draftTags[context.selectedLink].erase(key);
    SetCurrentLink(context);
    SetControlText(dialog, IDC_TAG_NAME, {});
    SetControlText(dialog, IDC_TAG_VALUE, {});
    PopulateOtherTags(dialog, context);
    context.status = Localized(L"Tag deletion pending. Select Save to write it to the file.");
    SetControlText(dialog, IDC_INFO_STATUS, context.status);
}

void SaveInfo(HWND dialog, InfoContext& context) {
    if (!context.writable) return;
    CommitStandardEdits(dialog, context);
    if (context.draftTags[context.selectedLink] == context.savedTags[context.selectedLink]) {
        context.status = Localized(L"There are no changes for this logical stream.");
        SetControlText(dialog, IDC_INFO_STATUS, context.status);
        return;
    }

    Tags patch = context.draftTags[context.selectedLink];
    for (const auto& saved : context.savedTags[context.selectedLink]) {
        if (patch.find(saved.first) == patch.end()) patch[saved.first] = L"";
    }
    std::wstring error;
    if (!WriteTags(context.track.path.c_str(), context.selectedLink, patch, error)) {
        if (error.empty()) error = L"The file could not be updated.";
        UiMessageBox(dialog, error.c_str(), L"Unable to Save Tags", MB_OK | MB_ICONERROR);
        return;
    }

    context.savedTags[context.selectedLink] = context.draftTags[context.selectedLink];
    SetCurrentLink(context);
    context.status = Localized(L"Saved tags for logical stream ") +
        std::to_wstring(context.selectedLink + 1) + L".";
    SetControlText(dialog, IDC_INFO_STATUS, context.status);
}

bool CloseInfo(HWND dialog, InfoContext& context) {
    CommitStandardEdits(dialog, context);
    if (HasUnsavedEdits(context)) {
        const int choice = UiMessageBox(dialog,
            L"Discard unsaved tag edits? No tags have been written automatically.",
            L"File Information", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (choice != IDYES) return false;
    }
    EndDialog(dialog, IDCANCEL);
    return true;
}

INT_PTR CALLBACK InfoDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<InfoContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        LocalizeDialog(dialog);
        context = reinterpret_cast<InfoContext*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
        InitializeInfoContext(*context);
        ConfigureList(dialog);
        RenderInfo(dialog, *context);
        return TRUE;

    case WM_COMMAND:
        if (!context) break;
        switch (LOWORD(wParam)) {
        case IDC_CHAIN_PREVIOUS:
            if (HIWORD(wParam) == BN_CLICKED) SwitchLink(dialog, *context, context->selectedLink - 1);
            return TRUE;
        case IDC_CHAIN_NEXT:
            if (HIWORD(wParam) == BN_CLICKED) SwitchLink(dialog, *context, context->selectedLink + 1);
            return TRUE;
        case IDC_TAG_ADD_UPDATE:
            if (HIWORD(wParam) == BN_CLICKED && context->writable) AddOrUpdateOtherTag(dialog, *context);
            return TRUE;
        case IDC_TAG_DELETE:
            if (HIWORD(wParam) == BN_CLICKED && context->writable) DeleteOtherTag(dialog, *context);
            return TRUE;
        case IDC_INFO_SAVE:
            if (HIWORD(wParam) == BN_CLICKED) SaveInfo(dialog, *context);
            return TRUE;
        case IDC_INFO_CLOSE:
        case IDCANCEL:
            if (HIWORD(wParam) == BN_CLICKED) CloseInfo(dialog, *context);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_NOTIFY:
        if (!context) break;
        {
            const auto* notify = reinterpret_cast<const NMHDR*>(lParam);
            if (notify->idFrom == IDC_TAG_LIST && notify->code == LVN_ITEMCHANGED) {
                const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if ((changed->uChanged & LVIF_STATE) != 0 &&
                    (changed->uNewState & LVIS_SELECTED) != 0 && changed->iItem >= 0) {
                    std::array<wchar_t, 130> name{};
                    std::array<wchar_t, 4096> value{};
                    HWND list = GetDlgItem(dialog, IDC_TAG_LIST);
                    ListView_GetItemText(list, changed->iItem, 0, name.data(), static_cast<int>(name.size()));
                    ListView_GetItemText(list, changed->iItem, 1, value.data(), static_cast<int>(value.size()));
                    SetControlText(dialog, IDC_TAG_NAME, name.data());
                    SetControlText(dialog, IDC_TAG_VALUE, value.data());
                }
                return TRUE;
            }
        }
        break;

    case WM_CLOSE:
        if (context) CloseInfo(dialog, *context);
        return TRUE;
    }
    return FALSE;
}

struct ConfigContext {
    ConfigContext(HINSTANCE moduleInstance, Settings input, const std::wstring& currentStatus)
        : module(moduleInstance), original(std::move(input)), result(original), status(currentStatus) {}

    HINSTANCE module = nullptr;
    Settings original;
    Settings result;
    std::wstring status;
    int page = 0;
    bool accepted = false;
};

constexpr int kDecodingControls[] = {
    IDC_CONFIG_DECODE_LABEL_CHANNELS, IDC_CONFIG_CHANNELS,
    IDC_CONFIG_DECODE_LABEL_RATE, IDC_CONFIG_RATE,
    IDC_CONFIG_DECODE_LABEL_BITS, IDC_CONFIG_BITS,
    IDC_CONFIG_DECODE_LABEL_QUALITY, IDC_CONFIG_QUALITY, IDC_CONFIG_QUALITY_VALUE,
    IDC_CONFIG_DITHER, IDC_CONFIG_PREVENT_CLIPPING, IDC_CONFIG_ENHANCED_SPEEX,
    IDC_CONFIG_BUFFER_SMALL_FILES, IDC_CONFIG_INSTANT_BITRATE, IDC_CONFIG_UNIFIED_INFO,
    IDC_CONFIG_NATIVE_CHAINS,
};

constexpr int kGainControls[] = {
    IDC_CONFIG_GAIN_LABEL_MODE, IDC_CONFIG_GAIN_MODE,
    IDC_CONFIG_GAIN_LABEL_PREAMP, IDC_CONFIG_PREAMP,
    IDC_CONFIG_GAIN_LABEL_RADIO, IDC_CONFIG_RADIO_PREAMP,
    IDC_CONFIG_GAIN_LABEL_TARGET, IDC_CONFIG_TARGET_LUFS, IDC_CONFIG_GAIN_NOTE,
    IDC_CONFIG_GAIN_LABEL_PEAK, IDC_CONFIG_PEAK_MODE,
    IDC_CONFIG_GAIN_LABEL_NON_RG, IDC_CONFIG_NON_RG, IDC_CONFIG_PREFERRED_ONLY,
    IDC_CONFIG_HOST_REPLAYGAIN,
};

constexpr int kStreamingControls[] = {
    IDC_CONFIG_STREAM_LABEL_TIMEOUT, IDC_CONFIG_TIMEOUT,
    IDC_CONFIG_STREAM_LABEL_PROBE, IDC_CONFIG_PROBE,
    IDC_CONFIG_STREAM_LABEL_ATTEMPTS, IDC_CONFIG_RECONNECT_ATTEMPTS,
    IDC_CONFIG_STREAM_LABEL_DELAY, IDC_CONFIG_RECONNECT_DELAY,
    IDC_CONFIG_STREAM_LABEL_BUFFER, IDC_CONFIG_STREAM_BUFFER,
    IDC_CONFIG_STREAM_LABEL_PREBUFFER, IDC_CONFIG_PREBUFFER,
    IDC_CONFIG_STREAM_LABEL_PROXY, IDC_CONFIG_PROXY,
    IDC_CONFIG_SAVE_STREAMS, IDC_CONFIG_STREAM_LABEL_DIRECTORY, IDC_CONFIG_SAVE_DIRECTORY,
    IDC_CONFIG_STREAM_NOTE,
};

constexpr int kAdvancedStreamingControls[] = {
    IDC_CONFIG_ADV_LABEL_PROXY_MODE, IDC_CONFIG_PROXY_MODE,
    IDC_CONFIG_HTTP_SEEK, IDC_CONFIG_ADV_LABEL_UNDERRUN, IDC_CONFIG_UNDERRUN_PERCENT,
    IDC_CONFIG_ADV_NOTE, IDC_CONFIG_HOST_PROXY,
};

constexpr int kProcessingControls[] = {
    IDC_CONFIG_PDF_LABEL, IDC_CONFIG_DITHER_PDF,
    IDC_CONFIG_SHAPE_LABEL, IDC_CONFIG_NOISE_SHAPE,
    IDC_CONFIG_LEGACY_51, IDC_CONFIG_PROCESSING_NOTE,
    IDC_CONFIG_EXACT_FLAC, IDC_CONFIG_OPUS_HD, IDC_CONFIG_OPUS_SPEECH, IDC_CONFIG_OPUS_BWE,
    IDC_CONFIG_AMBISONIC_LABEL, IDC_CONFIG_AMBISONIC,
};

template <size_t Count>
void ShowControls(HWND dialog, const int (&controls)[Count], bool visible) {
    for (const int control : controls) {
        ShowWindow(GetDlgItem(dialog, control), visible ? SW_SHOW : SW_HIDE);
    }
}

void UpdateQualityLabel(HWND dialog) {
    const int quality = static_cast<int>(SendDlgItemMessageW(dialog, IDC_CONFIG_QUALITY, TBM_GETPOS, 0, 0));
    SetControlText(dialog, IDC_CONFIG_QUALITY_VALUE, std::to_wstring(quality) + L" / 10");
}

void UpdateSaveDirectoryEnabled(HWND dialog) {
    const bool enabled = IsDlgButtonChecked(dialog, IDC_CONFIG_SAVE_STREAMS) == BST_CHECKED;
    EnableWindow(GetDlgItem(dialog, IDC_CONFIG_SAVE_DIRECTORY), enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_CONFIG_STREAM_LABEL_DIRECTORY), enabled ? TRUE : FALSE);
}

void ShowConfigPage(HWND dialog, ConfigContext& context, int page) {
    context.page = std::clamp(page, 0, 4);
    ShowControls(dialog, kDecodingControls, context.page == 0);
    ShowControls(dialog, kGainControls, context.page == 1);
    ShowControls(dialog, kStreamingControls, context.page == 2);
    ShowControls(dialog, kAdvancedStreamingControls, context.page == 3);
    ShowControls(dialog, kProcessingControls, context.page == 4);
    if (context.page == 2) UpdateSaveDirectoryEnabled(dialog);
}

void AddRateChoices(HWND combo) {
    AddComboItem(combo, L"Automatic (source rate)", 0);
    constexpr int kRates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 37800,
                              44100, 48000, 88200, 96000, 176400, 192000};
    for (const int rate : kRates) {
        AddComboItem(combo, (std::to_wstring(rate) + L" Hz").c_str(), rate);
    }
}

void InitializeConfig(HWND dialog, ConfigContext& context) {
    HWND tabs = GetDlgItem(dialog, IDC_CONFIG_TABS);
    TCITEMW tab{};
    tab.mask = TCIF_TEXT;
    tab.pszText = const_cast<wchar_t*>(L"Decoding");
    TabCtrl_InsertItem(tabs, 0, &tab);
    tab.pszText = const_cast<wchar_t*>(L"ReplayGain");
    TabCtrl_InsertItem(tabs, 1, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Streaming");
    TabCtrl_InsertItem(tabs, 2, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Advanced Streaming");
    TabCtrl_InsertItem(tabs, 3, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Processing");
    TabCtrl_InsertItem(tabs, 4, &tab);
    for (int page = 0; page != 5; ++page) {
        wchar_t text[128]{}; TCITEMW item{}; item.mask = TCIF_TEXT;
        item.pszText = text; item.cchTextMax = 128; TabCtrl_GetItem(tabs, page, &item);
        auto localized = Localized(text); item.pszText = localized.data();
        TabCtrl_SetItem(tabs, page, &item);
    }

    HWND channels = GetDlgItem(dialog, IDC_CONFIG_CHANNELS);
    AddComboItem(channels, L"Source (Windows channel order)", static_cast<int>(ChannelMode::Source));
    AddComboItem(channels, L"Stereo", static_cast<int>(ChannelMode::Stereo));
    AddComboItem(channels, L"Mono", static_cast<int>(ChannelMode::Mono));
    AddComboItem(channels, L"Quad (downmix 5.1+)", static_cast<int>(ChannelMode::Quad));
    AddComboItem(channels, L"Source (unmapped codec channel order)", static_cast<int>(ChannelMode::LegacyUnmapped));
    AddComboItem(channels, L"Stereo surround matrix (DS)", static_cast<int>(ChannelMode::Surround));
    AddComboItem(channels, L"Stereo surround matrix (DS2)", static_cast<int>(ChannelMode::SurroundII));
    SelectComboData(channels, static_cast<int>(context.original.channels));

    HWND rate = GetDlgItem(dialog, IDC_CONFIG_RATE);
    AddRateChoices(rate);
    SelectComboData(rate, context.original.rate);

    HWND bits = GetDlgItem(dialog, IDC_CONFIG_BITS);
    AddComboItem(bits, L"8-bit", 8);
    AddComboItem(bits, L"16-bit", 16);
    AddComboItem(bits, L"24-bit", 24);
    AddComboItem(bits, L"32-bit", 32);
    SelectComboData(bits, context.original.bits);

    SendDlgItemMessageW(dialog, IDC_CONFIG_QUALITY, TBM_SETRANGEMIN, FALSE, 0);
    SendDlgItemMessageW(dialog, IDC_CONFIG_QUALITY, TBM_SETRANGEMAX, FALSE, 10);
    SendDlgItemMessageW(dialog, IDC_CONFIG_QUALITY, TBM_SETTICFREQ, 1, 0);
    SendDlgItemMessageW(dialog, IDC_CONFIG_QUALITY, TBM_SETPOS, TRUE, context.original.quality);
    UpdateQualityLabel(dialog);

    CheckDlgButton(dialog, IDC_CONFIG_DITHER, context.original.dither ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_NATIVE_CHAINS, context.original.native_chains ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_LEGACY_51, context.original.legacy_51_order ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_EXACT_FLAC, context.original.exact_flac ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_OPUS_HD, context.original.opus_hd ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_OPUS_SPEECH, context.original.opus_speech ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_OPUS_BWE, context.original.opus_bwe ? BST_CHECKED : BST_UNCHECKED);
    HWND spatial = GetDlgItem(dialog, IDC_CONFIG_AMBISONIC);
    AddComboItem(spatial, L"Stereo speakers", static_cast<int>(AmbisonicMode::Stereo));
    AddComboItem(spatial, L"Headphones (analytic binaural)", static_cast<int>(AmbisonicMode::Headphones));
    AddComboItem(spatial, L"Quad speakers", static_cast<int>(AmbisonicMode::Quad));
    AddComboItem(spatial, L"5.1 speakers (silent LFE)", static_cast<int>(AmbisonicMode::Surround51));
    AddComboItem(spatial, L"7.1 speakers (silent LFE)", static_cast<int>(AmbisonicMode::Surround71));
    SelectComboData(spatial, static_cast<int>(context.original.ambisonic));
    CheckDlgButton(dialog, IDC_CONFIG_HOST_REPLAYGAIN, context.original.use_host_replaygain ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_HOST_PROXY, context.original.use_host_proxy ? BST_CHECKED : BST_UNCHECKED);
    HWND pdf = GetDlgItem(dialog, IDC_CONFIG_DITHER_PDF);
    AddComboItem(pdf, L"Rectangular", static_cast<int>(DitherPdf::Rectangular));
    AddComboItem(pdf, L"Triangular", static_cast<int>(DitherPdf::Triangular));
    AddComboItem(pdf, L"Gaussian", static_cast<int>(DitherPdf::Gaussian));
    SelectComboData(pdf, static_cast<int>(context.original.dither_pdf));
    HWND shape = GetDlgItem(dialog, IDC_CONFIG_NOISE_SHAPE);
    AddComboItem(shape, L"None", static_cast<int>(NoiseShape::None));
    AddComboItem(shape, L"First-order / triangular shaping", static_cast<int>(NoiseShape::FirstOrder));
    AddComboItem(shape, L"ATH psychoacoustic shaping", static_cast<int>(NoiseShape::Ath));
    AddComboItem(shape, L"ATH (low amplitude)", static_cast<int>(NoiseShape::AthLow));
    SelectComboData(shape, static_cast<int>(context.original.noise_shape));
    CheckDlgButton(dialog, IDC_CONFIG_PREVENT_CLIPPING,
                   context.original.prevent_clipping ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_ENHANCED_SPEEX,
                   context.original.enhanced_speex ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_BUFFER_SMALL_FILES,
                   context.original.buffer_small_files ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_INSTANT_BITRATE,
                   context.original.instant_bitrate ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_CONFIG_UNIFIED_INFO,
                   context.original.unified_info ? BST_CHECKED : BST_UNCHECKED);

    HWND gain = GetDlgItem(dialog, IDC_CONFIG_GAIN_MODE);
    AddComboItem(gain, L"Header gain", static_cast<int>(GainMode::Header));
    AddComboItem(gain, L"Track gain", static_cast<int>(GainMode::Track));
    AddComboItem(gain, L"Album gain", static_cast<int>(GainMode::Album));
    AddComboItem(gain, L"Automatic (track when shuffled)", static_cast<int>(GainMode::Automatic));
    AddComboItem(gain, L"Raw / no gain adjustment", static_cast<int>(GainMode::Raw));
    SelectComboData(gain, static_cast<int>(context.original.gain));
    SetControlText(dialog, IDC_CONFIG_PREAMP, FormatDb(context.original.preamp_q8));
    SetControlText(dialog, IDC_CONFIG_RADIO_PREAMP, FormatDb(context.original.radio_preamp_q8));
    SetControlText(dialog, IDC_CONFIG_TARGET_LUFS, std::to_wstring(context.original.target_lufs));
    HWND peak = GetDlgItem(dialog, IDC_CONFIG_PEAK_MODE);
    AddComboItem(peak, L"Apply gain (allow clipping)", 0);
    AddComboItem(peak, L"Apply gain with peak protection", 1);
    AddComboItem(peak, L"Normalize using tagged peak", 2);
    AddComboItem(peak, L"Prevent clipping only", 3);
    SelectComboData(peak, context.original.peak_mode);
    SetControlText(dialog, IDC_CONFIG_NON_RG, FormatDb(context.original.non_rg_q8));
    CheckDlgButton(dialog, IDC_CONFIG_PREFERRED_ONLY,
                   context.original.preferred_gain_only ? BST_CHECKED : BST_UNCHECKED);

    SetControlText(dialog, IDC_CONFIG_TIMEOUT, std::to_wstring(context.original.timeout_ms));
    SetControlText(dialog, IDC_CONFIG_PROBE, std::to_wstring(context.original.probe_kb));
    SetControlText(dialog, IDC_CONFIG_RECONNECT_ATTEMPTS,
                   std::to_wstring(context.original.reconnect_attempts));
    SetControlText(dialog, IDC_CONFIG_RECONNECT_DELAY,
                   std::to_wstring(context.original.reconnect_delay_ms));
    SetControlText(dialog, IDC_CONFIG_STREAM_BUFFER,
                   std::to_wstring(context.original.stream_buffer_kb));
    SetControlText(dialog, IDC_CONFIG_PREBUFFER, std::to_wstring(context.original.prebuffer_kb));
    SetControlText(dialog, IDC_CONFIG_PROXY, context.original.proxy_url);
    CheckDlgButton(dialog, IDC_CONFIG_SAVE_STREAMS,
                   context.original.save_streams ? BST_CHECKED : BST_UNCHECKED);
    SetControlText(dialog, IDC_CONFIG_SAVE_DIRECTORY, context.original.save_directory);

    HWND proxyMode = GetDlgItem(dialog, IDC_CONFIG_PROXY_MODE);
    AddComboItem(proxyMode, L"Never use a proxy", 0);
    AddComboItem(proxyMode, L"Only for HTTP port 80", 1);
    AddComboItem(proxyMode, L"Always use system or manual proxy", 2);
    SelectComboData(proxyMode, context.original.proxy_mode);
    CheckDlgButton(dialog, IDC_CONFIG_HTTP_SEEK,
                   context.original.http_seek ? BST_CHECKED : BST_UNCHECKED);
    SetControlText(dialog, IDC_CONFIG_UNDERRUN_PERCENT,
                   std::to_wstring(context.original.underrun_percent));

    if (!context.status.empty()) {
        SetWindowTextW(dialog, (Localized(L"Ogg Input Settings") + L" - " + context.status).c_str());
    }
    TabCtrl_SetCurSel(tabs, 0);
    ShowConfigPage(dialog, context, 0);
}

bool ReadRate(HWND dialog, int& rate) {
    HWND combo = GetDlgItem(dialog, IDC_CONFIG_RATE);
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected != CB_ERR) {
        const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
        if (data != CB_ERR) {
            rate = static_cast<int>(data);
            return true;
        }
    }
    const std::wstring entered = Trim(ControlText(combo));
    if (entered == L"Automatic" || entered == L"automatic") {
        rate = 0;
        return true;
    }
    if (!ParseInteger(entered, rate) || rate < 8000 || rate > 192000) {
        return ShowInvalid(dialog, IDC_CONFIG_RATE,
                           L"Output rate must be Automatic or a value from 8000 through 192000 Hz.");
    }
    return true;
}

bool ContainsLineBreak(const std::wstring& value) {
    return value.find(L'\r') != std::wstring::npos || value.find(L'\n') != std::wstring::npos;
}

bool AcceptConfig(HWND dialog, ConfigContext& context) {
    Settings chosen = context.original; // preserves ephemeral interactive_auth unchanged.
    int value = 0;
    if (!SelectedComboData(dialog, IDC_CONFIG_CHANNELS, value) || value < 0 || value > 6) {
        return ShowInvalid(dialog, IDC_CONFIG_CHANNELS, L"Select a valid channel output mode.");
    }
    chosen.channels = static_cast<ChannelMode>(value);
    if (!ReadRate(dialog, chosen.rate)) return false;
    if (!SelectedComboData(dialog, IDC_CONFIG_BITS, chosen.bits) ||
        (chosen.bits != 8 && chosen.bits != 16 && chosen.bits != 24 && chosen.bits != 32)) {
        return ShowInvalid(dialog, IDC_CONFIG_BITS, L"Select a valid sample depth.");
    }

    chosen.quality = static_cast<int>(SendDlgItemMessageW(dialog, IDC_CONFIG_QUALITY, TBM_GETPOS, 0, 0));
    if (chosen.quality < 0 || chosen.quality > 10) {
        return ShowInvalid(dialog, IDC_CONFIG_QUALITY, L"Resampler quality must be between 0 and 10.");
    }
    chosen.dither = IsDlgButtonChecked(dialog, IDC_CONFIG_DITHER) == BST_CHECKED;
    chosen.native_chains = IsDlgButtonChecked(dialog, IDC_CONFIG_NATIVE_CHAINS) == BST_CHECKED;
    chosen.legacy_51_order = IsDlgButtonChecked(dialog, IDC_CONFIG_LEGACY_51) == BST_CHECKED;
    chosen.exact_flac = IsDlgButtonChecked(dialog, IDC_CONFIG_EXACT_FLAC) == BST_CHECKED;
    chosen.opus_hd = IsDlgButtonChecked(dialog, IDC_CONFIG_OPUS_HD) == BST_CHECKED;
    chosen.opus_speech = IsDlgButtonChecked(dialog, IDC_CONFIG_OPUS_SPEECH) == BST_CHECKED;
    chosen.opus_bwe = IsDlgButtonChecked(dialog, IDC_CONFIG_OPUS_BWE) == BST_CHECKED;
    if (!SelectedComboData(dialog, IDC_CONFIG_AMBISONIC, value) || value < 0 || value > 4)
        return ShowInvalid(dialog, IDC_CONFIG_AMBISONIC, L"Select a valid Ambisonics output mode.");
    chosen.ambisonic = static_cast<AmbisonicMode>(value);
    chosen.use_host_replaygain = IsDlgButtonChecked(dialog, IDC_CONFIG_HOST_REPLAYGAIN) == BST_CHECKED;
    chosen.use_host_proxy = IsDlgButtonChecked(dialog, IDC_CONFIG_HOST_PROXY) == BST_CHECKED;
    if (!SelectedComboData(dialog, IDC_CONFIG_DITHER_PDF, value) || value < 0 || value > 2)
        return ShowInvalid(dialog, IDC_CONFIG_DITHER_PDF, L"Select a valid dither distribution.");
    chosen.dither_pdf = static_cast<DitherPdf>(value);
    if (!SelectedComboData(dialog, IDC_CONFIG_NOISE_SHAPE, value) || value < 0 || value > 3)
        return ShowInvalid(dialog, IDC_CONFIG_NOISE_SHAPE, L"Select a valid noise shaping mode.");
    chosen.noise_shape = static_cast<NoiseShape>(value);
    chosen.prevent_clipping = IsDlgButtonChecked(dialog, IDC_CONFIG_PREVENT_CLIPPING) == BST_CHECKED;
    chosen.enhanced_speex = IsDlgButtonChecked(dialog, IDC_CONFIG_ENHANCED_SPEEX) == BST_CHECKED;
    chosen.buffer_small_files = IsDlgButtonChecked(dialog, IDC_CONFIG_BUFFER_SMALL_FILES) == BST_CHECKED;
    chosen.instant_bitrate = IsDlgButtonChecked(dialog, IDC_CONFIG_INSTANT_BITRATE) == BST_CHECKED;
    chosen.unified_info = IsDlgButtonChecked(dialog, IDC_CONFIG_UNIFIED_INFO) == BST_CHECKED;

    if (!SelectedComboData(dialog, IDC_CONFIG_GAIN_MODE, value) || value < 0 || value > 4) {
        return ShowInvalid(dialog, IDC_CONFIG_GAIN_MODE, L"Select a valid ReplayGain mode.");
    }
    chosen.gain = static_cast<GainMode>(value);
    if (!ParseDb(ControlText(dialog, IDC_CONFIG_PREAMP), chosen.preamp_q8)) {
        return ShowInvalid(dialog, IDC_CONFIG_PREAMP,
                           L"Regular preamp must be a number from -60.0 through +30.0 dB.");
    }
    if (!ParseDb(ControlText(dialog, IDC_CONFIG_RADIO_PREAMP), chosen.radio_preamp_q8)) {
        return ShowInvalid(dialog, IDC_CONFIG_RADIO_PREAMP,
                           L"Radio preamp must be a number from -60.0 through +30.0 dB.");
    }
    if (!ReadRangedInteger(dialog, IDC_CONFIG_TARGET_LUFS, -40, 0,
                            L"Target loudness", chosen.target_lufs)) return false;
    if (!SelectedComboData(dialog, IDC_CONFIG_PEAK_MODE, chosen.peak_mode) ||
        chosen.peak_mode < 0 || chosen.peak_mode > 3) {
        return ShowInvalid(dialog, IDC_CONFIG_PEAK_MODE, L"Select a valid peak handling mode.");
    }
    if (!ParseDb(ControlText(dialog, IDC_CONFIG_NON_RG), chosen.non_rg_q8)) {
        return ShowInvalid(dialog, IDC_CONFIG_NON_RG,
                           L"Untagged preamp must be a number from -60.0 through +30.0 dB.");
    }
    chosen.preferred_gain_only = IsDlgButtonChecked(dialog, IDC_CONFIG_PREFERRED_ONLY) == BST_CHECKED;

    if (!ReadRangedInteger(dialog, IDC_CONFIG_TIMEOUT, 1000, 30000,
                            L"HTTP timeout", chosen.timeout_ms)) return false;
    if (!ReadRangedInteger(dialog, IDC_CONFIG_PROBE, 64, 4096,
                            L"Initial probe limit", chosen.probe_kb)) return false;
    if (!ReadRangedInteger(dialog, IDC_CONFIG_RECONNECT_ATTEMPTS, 0, 10,
                            L"Reconnect attempts", chosen.reconnect_attempts)) return false;
    if (!ReadRangedInteger(dialog, IDC_CONFIG_RECONNECT_DELAY, 250, 30000,
                            L"Reconnect delay", chosen.reconnect_delay_ms)) return false;
    if (!ReadRangedInteger(dialog, IDC_CONFIG_STREAM_BUFFER, 32, 4096,
                            L"Stream buffer", chosen.stream_buffer_kb)) return false;
    if (!ReadRangedInteger(dialog, IDC_CONFIG_PREBUFFER, 0,
                            std::min(chosen.stream_buffer_kb, chosen.probe_kb / 2),
                            L"Startup prebuffer", chosen.prebuffer_kb)) return false;

    if (!SelectedComboData(dialog, IDC_CONFIG_PROXY_MODE, chosen.proxy_mode) ||
        chosen.proxy_mode < 0 || chosen.proxy_mode > 2) {
        return ShowInvalid(dialog, IDC_CONFIG_PROXY_MODE, L"Select a valid proxy policy.");
    }
    chosen.http_seek = IsDlgButtonChecked(dialog, IDC_CONFIG_HTTP_SEEK) == BST_CHECKED;
    if (!ReadRangedInteger(dialog, IDC_CONFIG_UNDERRUN_PERCENT, 0, 100,
                            L"Rebuffer threshold", chosen.underrun_percent)) return false;

    chosen.proxy_url = Trim(ControlText(dialog, IDC_CONFIG_PROXY));
    if (ContainsLineBreak(chosen.proxy_url) || chosen.proxy_url.find(L'@') != std::wstring::npos) {
        return ShowInvalid(dialog, IDC_CONFIG_PROXY,
                           L"Proxy credentials cannot be saved. Enter a credential-free proxy URL.");
    }
    chosen.save_streams = IsDlgButtonChecked(dialog, IDC_CONFIG_SAVE_STREAMS) == BST_CHECKED;
    chosen.save_directory = Trim(ControlText(dialog, IDC_CONFIG_SAVE_DIRECTORY));
    if (ContainsLineBreak(chosen.save_directory)) {
        return ShowInvalid(dialog, IDC_CONFIG_SAVE_DIRECTORY,
                           L"Save directory must be a single local path.");
    }

    chosen = Validate(chosen);
    if (!SaveSettings(chosen)) {
        UiMessageBox(dialog, L"Settings could not be saved to the current user's registry.",
                    L"Ogg Input Settings", MB_OK | MB_ICONERROR);
        return false;
    }
    context.result = std::move(chosen);
    context.accepted = true;
    EndDialog(dialog, IDOK);
    return true;
}

INT_PTR CALLBACK ConfigDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<ConfigContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        LocalizeDialog(dialog);
        context = reinterpret_cast<ConfigContext*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
        InitializeConfig(dialog, *context);
        return TRUE;

    case WM_HSCROLL:
        if (context && reinterpret_cast<HWND>(lParam) == GetDlgItem(dialog, IDC_CONFIG_QUALITY)) {
            UpdateQualityLabel(dialog);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (!context) break;
        if (LOWORD(wParam) == IDC_CONFIG_SAVE_STREAMS && HIWORD(wParam) == BN_CLICKED) {
            UpdateSaveDirectoryEnabled(dialog);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK && HIWORD(wParam) == BN_CLICKED) {
            AcceptConfig(dialog, *context);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL && HIWORD(wParam) == BN_CLICKED) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_NOTIFY:
        if (context) {
            const auto* notify = reinterpret_cast<const NMHDR*>(lParam);
            if (notify->idFrom == IDC_CONFIG_TABS && notify->code == TCN_SELCHANGE) {
                ShowConfigPage(dialog, *context,
                               TabCtrl_GetCurSel(GetDlgItem(dialog, IDC_CONFIG_TABS)));
                return TRUE;
            }
        }
        break;

    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM) {
    switch (message) {
    case WM_INITDIALOG:
        LocalizeDialog(dialog);
        SetControlText(dialog, IDC_ABOUT_TEXT, Localized(
            L"Ogg Input for Winamp\r\n\r\n"
            L"Independent GPLv3 implementation. This project is not a copy of historical "
            L"in_vorbis code or resources and does not claim full feature or behavior parity.\r\n\r\n"
            L"Codec libraries in this build: libvorbis 1.3.7, libopus/opusfile, "
            L"FLAC 1.5.0, and Speex 1.2.1."));
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

} // namespace

Settings Configure(HINSTANCE module, HWND parent, Settings current, const std::wstring& status) {
    InitializeCommonControls();
    ConfigContext context(module, std::move(current), status);
    DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_CONFIG), parent, ConfigDialogProc,
                    reinterpret_cast<LPARAM>(&context));
    return context.accepted ? context.result : context.original;
}

void ShowInfo(HINSTANCE module, HWND parent, const TrackInfo& info) {
    InitializeCommonControls();
    InfoContext context(module, info);
    DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_FILE_INFO), parent, InfoDialogProc,
                    reinterpret_cast<LPARAM>(&context));
}

HWND UnifiedPane(HINSTANCE module, HWND parent, const TrackInfo& info, void (*saved)()) {
    return CreateTagEditorPane(module, parent, info, saved);
}

void ShowAbout(HINSTANCE module, HWND parent) {
    InitializeCommonControls();
    DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_ABOUT), parent, AboutDialogProc, 0);
}

} // namespace ogg_input
