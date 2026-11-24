// SPDX-License-Identifier: GPL-3.0-only
// Independent implementation of Winamp's unified metadata-pane protocol.
#include "tag_editor.hpp"
#include "tag_writer.hpp"
#include "language.hpp"
#include "resource.h"

#include <commctrl.h>
#include <algorithm>
#include <cwchar>
#include <memory>
#include <set>
#include <utility>

namespace ogg_input {
namespace {
constexpr wchar_t kNoHostWrite[] = L"INBUILT_NOWRITEINFO";
constexpr UINT kConfigureHost = WM_APP + 61;
// Optional enhancement of Winamp's existing Basic Info control. Never create a
// replacement: unknown host layouts simply retain their own behavior.
constexpr int kHostFormatInfo = 1327;
constexpr LONG_PTR kFormatScrollStyle = WS_VSCROLL | ES_AUTOVSCROLL;

struct Row {
    std::wstring name, value;
};

bool FieldName(const std::wstring& wide, std::string& name) {
    if (wide.empty()) return false;
    name.clear();
    for (wchar_t c : wide) {
        // These are the Vorbis-comment field-name rules, not a UI length cap.
        if (c < 0x20 || c > 0x7d || c == L'=') return false;
        name.push_back(static_cast<char>(c));
    }
    return true;
}

std::string Lower(std::string key) {
    for (char& c : key) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return key;
}

std::wstring Wide(const std::string& name) { return {name.begin(), name.end()}; }

std::string HostName(std::string key, Codec codec) {
    key = Lower(std::move(key));
    for (const auto& alias : {std::pair{"date", "year"}, {"tracknumber", "track"},
            {"discnumber", "disc"}, {"description", "comment"}, {"album artist", "albumartist"},
            {"ensemble", "albumartist"}, {"organization", "publisher"},
            {"encoded-by", "tool"}, {"version", "subtitle"}}) {
        if (key == alias.first) return alias.second;
    }
    if (codec == Codec::Opus) {
        if (key == "r128_track_gain") return "replaygain_track_gain";
        if (key == "r128_album_gain") return "replaygain_album_gain";
    }
    return key;
}

std::wstring Text(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(std::wcslen(text.c_str()));
    return text;
}

struct Flag {
    bool& value;
    bool previous;
    explicit Flag(bool& input) : value(input), previous(input) { value = true; }
    ~Flag() { value = previous; }
};

struct Editor {
    HWND parent = nullptr, formatInfo = nullptr;
    LONG_PTR originalFormatStyle = 0;
    TrackInfo track;
    TagDocument saved;
    std::vector<Row> rows;
    void (*onSaved)() = nullptr;
    HANDLE previousMarker = nullptr;
    bool markerSet = false, dialogOwned = false;
    bool writable = false, rendering = false, broadcasting = false, cancelled = false;
    int selected = -1;

    Editor(HWND owner, const TrackInfo& info, void (*callback)())
        : parent(owner), track(info), onSaved(callback) {}
    ~Editor() {
        if (IsWindow(formatInfo) && GetWindowLongPtrW(formatInfo, GWL_STYLE) == (originalFormatStyle | kFormatScrollStyle)) {
            SetWindowLongPtrW(formatInfo, GWL_STYLE, originalFormatStyle);
            SetWindowPos(formatInfo, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        if (markerSet && IsWindow(parent) && GetPropW(parent, kNoHostWrite) == reinterpret_cast<HANDLE>(1)) {
            if (previousMarker) SetPropW(parent, kNoHostWrite, previousMarker);
            else RemovePropW(parent, kNoHostWrite);
        }
    }

    void Load() {
        std::wstring error;
        writable = !track.radio && !IsHttp(track.path.c_str()) &&
            (track.codec == Codec::Vorbis || track.codec == Codec::Opus) &&
            ReadTagDocument(track.path.c_str(), track.current_link, saved, error);
        if (writable) {
            for (const auto& field : saved.fields) rows.push_back({Wide(field.name), field.value});
        } else {
            for (const auto& field : track.current.tags) rows.push_back({Wide(field.first), field.second});
        }
    }

    TrackInfo Preview() const {
        TrackInfo info = track;
        info.current.tags.clear();
        for (const auto& row : rows) {
            std::string key;
            if (!FieldName(row.name, key)) continue;
            key = Lower(std::move(key));
            auto found = info.current.tags.find(key);
            if (found == info.current.tags.end()) info.current.tags.emplace(std::move(key), row.value);
            else found->second += L"; " + row.value;
        }
        return info;
    }

    void Notify(const std::vector<std::wstring>& names) {
        if (broadcasting || !parent) return;
        std::set<std::string> fields;
        for (const auto& name : names) {
            std::string key;
            if (FieldName(name, key)) fields.insert(HostName(std::move(key), track.codec));
        }
        const auto info = Preview();
        Flag guard(broadcasting);
        for (const auto& field : fields) {
            std::wstring value;
            Metadata(info, field.c_str(), value);
            const auto wide = Wide(field);
            SendMessageW(parent, WM_USER, reinterpret_cast<WPARAM>(wide.c_str()),
                         reinterpret_cast<LPARAM>(value.c_str()));
        }
    }
};

void ConfigureNativeFormatInfo(Editor& editor) {
    if (editor.formatInfo || !IsWindow(editor.parent)) return;
    EnumChildWindows(editor.parent, [](HWND control, LPARAM data) -> BOOL {
        if (GetDlgCtrlID(control) != kHostFormatInfo) return TRUE;
        wchar_t name[16]{};
        GetClassNameW(control, name, 16);
        const auto style = GetWindowLongPtrW(control, GWL_STYLE);
        if (_wcsicmp(name, L"Edit") || (style & (ES_MULTILINE | ES_READONLY)) != (ES_MULTILINE | ES_READONLY)) return TRUE;
        if ((style & kFormatScrollStyle) == kFormatScrollStyle) return FALSE;
        if (!SetWindowLongPtrW(control, GWL_STYLE, style | kFormatScrollStyle)) return TRUE;
        auto& state = *reinterpret_cast<Editor*>(data);
        state.formatInfo = control;
        state.originalFormatStyle = style;
        SetWindowPos(control, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return FALSE;
    }, reinterpret_cast<LPARAM>(&editor));
}

void Selection(HWND dialog, Editor& editor, int selected) {
    Flag guard(editor.rendering);
    editor.selected = selected >= 0 && static_cast<std::size_t>(selected) < editor.rows.size() ? selected : -1;
    const Row empty;
    const auto& row = editor.selected < 0 ? empty : editor.rows[editor.selected];
    SetDlgItemTextW(dialog, IDC_TAG_NAME, row.name.c_str());
    SetDlgItemTextW(dialog, IDC_TAG_VALUE, row.value.c_str());
    const bool active = editor.selected >= 0;
    EnableWindow(GetDlgItem(dialog, IDC_TAG_NAME), active);
    EnableWindow(GetDlgItem(dialog, IDC_TAG_VALUE), active);
    EnableWindow(GetDlgItem(dialog, IDC_TAG_DELETE), active && editor.writable);
}

void Render(HWND dialog, Editor& editor, int selected) {
    Flag guard(editor.rendering);
    HWND list = GetDlgItem(dialog, IDC_TAG_LIST);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (std::size_t i = 0; i < editor.rows.size(); ++i) {
        const auto& row = editor.rows[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(row.name.c_str());
        const int inserted = ListView_InsertItem(list, &item);
        if (inserted >= 0) ListView_SetItemText(list, inserted, 1, const_cast<wchar_t*>(row.value.c_str()));
    }
    if (selected >= 0 && static_cast<std::size_t>(selected) < editor.rows.size()) {
        ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selected, FALSE);
    }
    Selection(dialog, editor, selected);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    EnableWindow(GetDlgItem(dialog, IDC_TAG_DELETE_ALL), editor.writable && !editor.rows.empty());
}

void EditRow(HWND dialog, Editor& editor) {
    if (!editor.writable || editor.rendering || editor.selected < 0) return;
    auto& row = editor.rows[editor.selected];
    const auto before = row.name;
    row = {Text(dialog, IDC_TAG_NAME), Text(dialog, IDC_TAG_VALUE)};
    HWND list = GetDlgItem(dialog, IDC_TAG_LIST);
    ListView_SetItemText(list, editor.selected, 0, row.name.data());
    ListView_SetItemText(list, editor.selected, 1, row.value.data());
    editor.Notify({before, row.name});
}

void HostEdit(HWND dialog, Editor& editor, const wchar_t* name, const wchar_t* value) {
    if (!editor.writable || editor.broadcasting || !name || !value) return;
    std::string key;
    if (!FieldName(name, key)) return;
    Tags patch;
    if (!MetadataPatch(editor.track, Lower(std::move(key)).c_str(), value, patch)) return;
    for (const auto& field : patch) {
        // Do not conflate native R128 and ReplayGain while applying the patch:
        // the latter's deletion must not erase the former's new value.
        const auto family = HostName(field.first, Codec::Vorbis);
        std::size_t first = editor.rows.size();
        std::wstring storedName = Wide(field.first);
        for (std::size_t i = 0; i < editor.rows.size();) {
            std::string native;
            if (FieldName(editor.rows[i].name, native) && HostName(native, Codec::Vorbis) == family) {
                if (first == editor.rows.size()) { first = i; storedName = editor.rows[i].name; }
                editor.rows.erase(editor.rows.begin() + static_cast<std::ptrdiff_t>(i));
            } else ++i;
        }
        if (!field.second.empty()) {
            first = std::min(first, editor.rows.size());
            editor.rows.insert(editor.rows.begin() + static_cast<std::ptrdiff_t>(first), {storedName, field.second});
        }
    }
    Render(dialog, editor, std::min(editor.selected, static_cast<int>(editor.rows.size()) - 1));
}

bool Save(HWND dialog, Editor& editor) {
    if (!editor.writable || editor.cancelled) return true;
    TagFields fields;
    for (std::size_t i = 0; i < editor.rows.size(); ++i) {
        std::string name;
        if (!FieldName(editor.rows[i].name, name)) {
            Render(dialog, editor, static_cast<int>(i));
            MessageBoxW(dialog, Localized(L"A tag name must contain printable ASCII characters other than '='.").c_str(),
                Localized(L"Unable to Save Tags").c_str(), MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dialog, IDC_TAG_NAME));
            return false;
        }
        fields.push_back({std::move(name), editor.rows[i].value});
    }
    if (fields == editor.saved.fields) return true;
    std::wstring error;
    if (!WriteTagDocument(editor.track.path.c_str(), editor.saved, fields, error)) {
        MessageBoxW(dialog, error.c_str(), Localized(L"Unable to Save Tags").c_str(), MB_OK | MB_ICONERROR);
        return false;
    }
    if (editor.onSaved) editor.onSaved();
    // A changed field can move behind untouched comments during repagination.
    // Refresh the baseline so repeated OK/Apply messages remain idempotent.
    if (ReadTagDocument(editor.track.path.c_str(), editor.saved.link, editor.saved, error)) {
        editor.rows.clear();
        for (const auto& field : editor.saved.fields) editor.rows.push_back({Wide(field.name), field.value});
        Render(dialog, editor, -1);
    } else {
        editor.writable = false;
        EnableWindow(GetDlgItem(dialog, IDC_TAG_ADD_NEW), FALSE);
        EnableWindow(GetDlgItem(dialog, IDC_TAG_DELETE_ALL), FALSE);
        for (int id : {IDC_TAG_NAME, IDC_TAG_VALUE})
            SendDlgItemMessageW(dialog, id, EM_SETREADONLY, TRUE, 0);
        SetDlgItemTextW(dialog, IDC_UNIFIED_SUMMARY, Localized(L"Advanced (read-only)").c_str());
        Selection(dialog, editor, editor.selected);
    }
    return true;
}

INT_PTR CALLBACK Procedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try {
        switch (message) {
        case WM_INITDIALOG: {
            editor = reinterpret_cast<Editor*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
            LocalizeDialog(dialog);
            editor->Load();
            editor->previousMarker = GetPropW(editor->parent, kNoHostWrite);
            editor->markerSet = SetPropW(editor->parent, kNoHostWrite, reinterpret_cast<HANDLE>(1)) != FALSE;
            if (!editor->markerSet) editor->writable = false;
            HWND list = GetDlgItem(dialog, IDC_TAG_LIST);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            auto name = Localized(L"Name"), value = Localized(L"Value");
            column.pszText = name.data(); column.cx = 85;
            ListView_InsertColumn(list, 0, &column);
            column.pszText = value.data(); column.cx = 230;
            ListView_InsertColumn(list, 1, &column);
            // No 100/1024/4096-character UI truncation. The bounded writer checks
            // the complete encoded Ogg header at save time.
            SendDlgItemMessageW(dialog, IDC_TAG_NAME, EM_SETLIMITTEXT, 16u * 1024u * 1024u, 0);
            SendDlgItemMessageW(dialog, IDC_TAG_VALUE, EM_SETLIMITTEXT, 16u * 1024u * 1024u, 0);
            for (int control : {IDC_TAG_NAME, IDC_TAG_VALUE})
                SendDlgItemMessageW(dialog, control, EM_SETREADONLY, !editor->writable, 0);
            EnableWindow(GetDlgItem(dialog, IDC_TAG_ADD_NEW), editor->writable);
            if (!editor->writable) SetDlgItemTextW(dialog, IDC_UNIFIED_SUMMARY, Localized(L"Advanced (read-only)").c_str());
            Render(dialog, *editor, -1);
            ConfigureNativeFormatInfo(*editor);
            // Winamp may create Basic Info after requesting the plugin panes.
            PostMessageW(dialog, kConfigureHost, 0, 0);
            return TRUE;
        }
        case kConfigureHost:
            if (editor) ConfigureNativeFormatInfo(*editor);
            return TRUE;
        case WM_USER:
            if (editor) HostEdit(dialog, *editor, reinterpret_cast<const wchar_t*>(wParam), reinterpret_cast<const wchar_t*>(lParam));
            return TRUE;
        case WM_COMMAND:
            if (!editor) break;
            switch (LOWORD(wParam)) {
            case IDC_TAG_NAME: case IDC_TAG_VALUE:
                if (HIWORD(wParam) == EN_CHANGE) EditRow(dialog, *editor);
                return TRUE;
            case IDC_TAG_ADD_NEW:
                if (editor->writable) {
                    editor->rows.push_back({});
                    Render(dialog, *editor, static_cast<int>(editor->rows.size()) - 1);
                    SetFocus(GetDlgItem(dialog, IDC_TAG_NAME));
                }
                return TRUE;
            case IDC_TAG_DELETE:
                if (editor->writable && editor->selected >= 0) {
                    auto name = editor->rows[editor->selected].name;
                    const int selected = editor->selected;
                    editor->rows.erase(editor->rows.begin() + selected);
                    Render(dialog, *editor, std::min(selected, static_cast<int>(editor->rows.size()) - 1));
                    editor->Notify({name});
                }
                return TRUE;
            case IDC_TAG_DELETE_ALL:
                if (editor->writable) {
                    std::vector<std::wstring> names;
                    for (const auto& row : editor->rows) names.push_back(row.name);
                    editor->rows.clear();
                    Render(dialog, *editor, -1);
                    editor->Notify(names);
                }
                return TRUE;
            case IDOK:
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, Save(dialog, *editor) ? 0 : 1);
                return TRUE;
            case IDCANCEL:
                editor->cancelled = true;
                return TRUE;
            }
            break;
        case WM_NOTIFY:
            if (editor && !editor->rendering) {
                const auto* notice = reinterpret_cast<const NMHDR*>(lParam);
                if (notice->idFrom == IDC_TAG_LIST && notice->code == LVN_ITEMCHANGED &&
                    (reinterpret_cast<const NMLISTVIEW*>(lParam)->uChanged & LVIF_STATE)) {
                    HWND list = GetDlgItem(dialog, IDC_TAG_LIST);
                    Selection(dialog, *editor, ListView_GetNextItem(list, -1, LVNI_SELECTED));
                } else if (notice->idFrom == IDC_TAG_LIST && notice->code == LVN_KEYDOWN &&
                           reinterpret_cast<const NMLVKEYDOWN*>(lParam)->wVKey == VK_DELETE) {
                    SendMessageW(dialog, WM_COMMAND, IDC_TAG_DELETE, 0);
                }
            }
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(dialog, DWLP_USER, 0);
            if (editor && editor->dialogOwned) delete editor;
            return FALSE;
        }
    } catch (...) {
        // Never unwind C++ exceptions through a host-owned window procedure.
        if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
            MessageBoxW(dialog, Localized(L"The file could not be updated.").c_str(),
                Localized(L"Unable to Save Tags").c_str(), MB_OK | MB_ICONERROR);
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, 1);
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace

HWND CreateTagEditorPane(HINSTANCE module, HWND parent, const TrackInfo& info, void (*saved)()) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) return nullptr;
    auto editor = std::make_unique<Editor>(parent, info, saved);
    HWND dialog = CreateDialogParamW(module, MAKEINTRESOURCEW(IDD_UNIFIED_PANE), parent,
                                    Procedure, reinterpret_cast<LPARAM>(editor.get()));
    if (dialog) { editor->dialogOwned = true; editor.release(); }
    return dialog;
}
} // namespace ogg_input
