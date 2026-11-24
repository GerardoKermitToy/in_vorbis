// SPDX-License-Identifier: GPL-3.0-only
#include "codec_fixtures.hpp"
#include "registry_sandbox.hpp"
#include "dialog_capture.hpp"
#include "audio.hpp"
#include "tag_writer.hpp"
#include "winamp_input.hpp"
#include "resource.h"
#include <commctrl.h>
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>

namespace {
using namespace ogg_input;
using AddPane = HWND(__cdecl*)(int, const wchar_t*, HWND, wchar_t*, int);
using GetTag = int(__cdecl*)(const wchar_t*, const char*, wchar_t*, int);

struct Host {
    HWND window = nullptr, pane = nullptr, formatInfo = nullptr;
    LONG_PTR formatStyle = 0;
    std::map<std::wstring, std::wstring> fields;
    int broadcasts = 0;
    bool recursion = false;
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            host = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        }
        if (message == WM_USER && host && wParam && lParam) {
            // Mirror the real unified host's synchronous broadcast to all tabs,
            // including the sender. An unguarded editor would recurse here.
            ++host->broadcasts;
            if (host->broadcasts > 1000) { host->recursion = true; return 0; }
            const std::wstring name = reinterpret_cast<const wchar_t*>(wParam);
            const std::wstring value = reinterpret_cast<const wchar_t*>(lParam);
            host->fields[name] = value;
            if (host->pane) SendMessageW(host->pane, WM_USER, wParam, lParam);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    Host() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"IndependentOggUnifiedInfoTest";
        Check(RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register unified mock host");
        window = CreateWindowW(wc.lpszClassName, L"Unified metadata test host", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 700, 430, nullptr, nullptr, wc.hInstance, this);
        Check(window, "create unified mock host");
        formatInfo = CreateWindowW(L"Edit", L"Line 1\r\nLine 2\r\nLine 3\r\nLine 4\r\nLine 5\r\nLine 6\r\nLine 7\r\nLine 8",
            WS_CHILD | ES_MULTILINE | ES_READONLY, 0, 0, 180, 40, window,
            reinterpret_cast<HMENU>(1327), wc.hInstance, nullptr);
        Check(formatInfo, "create host-owned native Format Info field");
        formatStyle = GetWindowLongPtrW(formatInfo, GWL_STYLE);
    }
    ~Host() { if (window) DestroyWindow(window); }
    void Close() {
        if (pane) DestroyWindow(pane);
        pane = nullptr;
    }
    void Open(AddPane add, const std::filesystem::path& path) {
        Close();
        wchar_t name[64]{};
        pane = add(0, path.c_str(), window, name, 64);
        Check(pane && std::wstring(name) == L"Advanced", "editable pane uses Advanced tab title");
        Check(GetParent(pane) == window && GetPropW(window, L"INBUILT_NOWRITEINFO"), "host suppresses duplicate metadata writes");
    }
    void Field(const wchar_t* name, const wchar_t* value) {
        SendMessageW(pane, WM_USER, reinterpret_cast<WPARAM>(name), reinterpret_cast<LPARAM>(value));
    }
};

void Click(HWND pane, int id) {
    SendMessageW(pane, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(pane, id)));
}
std::wstring Text(HWND pane, int id) {
    HWND control = GetDlgItem(pane, id);
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(std::wcslen(value.c_str()));
    return value;
}
int Find(HWND pane, const wchar_t* name, int occurrence = 0) {
    HWND list = GetDlgItem(pane, IDC_TAG_LIST);
    for (int i = 0; i < ListView_GetItemCount(list); ++i) {
        wchar_t text[1024]{};
        ListView_GetItemText(list, i, 0, text, 1024);
        if (!_wcsicmp(text, name) && occurrence-- == 0) return i;
    }
    return -1;
}
void Select(HWND pane, int item) {
    Check(item >= 0, "find editor row");
    HWND list = GetDlgItem(pane, IDC_TAG_LIST);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}
void SetValue(HWND pane, const wchar_t* value) {
    SetDlgItemTextW(pane, IDC_TAG_VALUE, value);
    // Multiline Edit does not emit EN_CHANGE for WM_SETTEXT. Deliver the
    // notification produced by an actual user edit, not just a programmatic fill.
    SendMessageW(pane, WM_COMMAND, MAKEWPARAM(IDC_TAG_VALUE, EN_CHANGE),
                 reinterpret_cast<LPARAM>(GetDlgItem(pane, IDC_TAG_VALUE)));
}
void New(HWND pane, const wchar_t* name, const wchar_t* value) {
    Click(pane, IDC_TAG_ADD_NEW);
    SetDlgItemTextW(pane, IDC_TAG_NAME, name);
    SetValue(pane, value);
}
std::vector<char> Bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
TagDocument Document(const std::filesystem::path& path, int link = 0) {
    TagDocument document;
    std::wstring error;
    Check(ReadTagDocument(path.c_str(), link, document, error), "read editor document");
    return document;
}
std::vector<std::wstring> Values(const TagDocument& document, const char* name) {
    std::vector<std::wstring> values;
    for (const auto& field : document.fields) if (!_stricmp(field.name.c_str(), name)) values.push_back(field.value);
    return values;
}
std::vector<float> Decode(const std::filesystem::path& path) {
    Settings settings;
    settings.gain = GainMode::Raw;
    Audio audio;
    std::wstring error;
    Check(audio.Open(path.c_str(), settings, false, error), "open audio after tag editing");
    std::vector<float> result;
    float block[BlockFrames * MaxChannels]{};
    for (;;) {
        const int frames = audio.Read(block, BlockFrames);
        Check(frames >= 0, "decode after tag editing");
        if (!frames) break;
        result.insert(result.end(), block, block + frames * audio.Info().current.channels);
    }
    return result;
}
BOOL CALLBACK Bounds(HWND child, LPARAM data) {
    HWND pane = reinterpret_cast<HWND>(data);
    RECT rect{}, client{};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, pane, reinterpret_cast<POINT*>(&rect), 2);
    GetClientRect(pane, &client);
    if (rect.left < 0 || rect.top < 0 || rect.right > client.right || rect.bottom > client.bottom)
        SetPropW(pane, L"Clipped", reinterpret_cast<HANDLE>(1));
    return TRUE;
}

int blockedDialogs = 0;
LRESULT CALLBACK BlockModalDialog(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_CREATEWND) {
        const auto* creation = reinterpret_cast<const CBT_CREATEWNDW*>(lParam);
        wchar_t name[32]{};
        GetClassNameW(reinterpret_cast<HWND>(wParam), name, 32);
        if (!(creation->lpcs->style & WS_CHILD) && !std::wcscmp(name, L"#32770")) {
            ++blockedDialogs;
            // Abort creation, not activation: even an unexpected error must
            // fail this unattended test without displaying or waiting on a box.
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
struct ModalGuard {
    HHOOK hook = SetWindowsHookExW(WH_CBT, BlockModalDialog, nullptr, GetCurrentThreadId());
    ModalGuard() { Check(hook, "prevent modal dialogs in unattended UI tests"); }
    ~ModalGuard() { UnhookWindowsHookEx(hook); }
};

void EditingTests(Host& host, AddPane add, GetTag get, const std::filesystem::path& directory) {
    const auto path = directory / L"editable.ogg", tail = directory / L"tail.ogg";
    const std::string longName(200, 'N');
    const std::wstring longValue(70000, L'v');
    MakeVorbis(path, 2, 44100, 7013, 41001, {"DESCRIPTION=First line\r\nSecond line",
        "PERFORMER=First performer", "PERFORMER=Second performer", "ALBUM ARTIST=Ensemble name",
        "METADATA_BLOCK_PICTURE=preserve-picture-byte-for-byte", "COVERART=preserve-coverart",
        "OPAQUE=\xff", longName + "=Long name"});
    MakeVorbis(tail, 2, 44100, 7013, 41002);
    AppendFile(path, tail);
    const auto before = Bytes(path);
    const auto pcm = Decode(path);
    const auto second = Document(path, 1);
    host.Open(add, path);
    Check((GetWindowLongPtrW(host.formatInfo, GWL_STYLE) & (WS_VSCROLL | ES_AUTOVSCROLL)) == (WS_VSCROLL | ES_AUTOVSCROLL),
          "existing host Format Info supports scrolling without duplicate controls");
    const auto firstLine = SendMessageW(host.formatInfo, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(host.formatInfo, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    Check(SendMessageW(host.formatInfo, EM_GETFIRSTVISIBLELINE, 0, 0) > firstLine,
          "mouse wheel reaches extra technical information in host-owned field");
    Check(!GetDlgItem(host.pane, IDC_TAG_TITLE) && !GetDlgItem(host.pane, IDC_INFO_PATH) &&
        !GetDlgItem(host.pane, IDC_UNIFIED_OPEN_INFO) && !GetDlgItem(host.pane, IDOK) &&
        !GetDlgItem(host.pane, IDCANCEL), "pane does not duplicate Basic Info, path, OK/Cancel or launch another editor");
    Check(GetWindowLongPtrW(GetDlgItem(host.pane, IDC_TAG_VALUE), GWL_STYLE) & ES_MULTILINE,
          "Advanced value editor is multiline");
    Check(Find(host.pane, L"METADATA_BLOCK_PICTURE") < 0 && Find(host.pane, L"COVERART") < 0,
          "artwork is not a text-edit row");
    Check(Find(host.pane, L"PERFORMER", 1) >= 0 && Find(host.pane, std::wstring(200, L'N').c_str()) >= 0,
          "duplicate fields and long field names are separate editor rows");
    Select(host.pane, Find(host.pane, L"DESCRIPTION"));
    Check(Text(host.pane, IDC_TAG_VALUE) == L"First line\r\nSecond line", "multiline text is not flattened");
    SetValue(host.pane,L"Updated comment\r\nUnicode — テスト");
    Check(host.fields[L"comment"] == L"Updated comment\r\nUnicode — テスト", "Advanced comment updates Basic Info");
    Check(!host.recursion, "host echo does not recurse");
    host.Field(L"title", L"Basic Info title");
    Select(host.pane, Find(host.pane, L"TITLE"));
    Check(Text(host.pane, IDC_TAG_VALUE) == L"Basic Info title", "Basic Info updates Advanced");
    host.Field(L"albumartist", L"");
    Check(Find(host.pane, L"ALBUM ARTIST") < 0, "host deletion removes alternate album-artist field");
    Select(host.pane, Find(host.pane, L"PERFORMER", 1));
    SetValue(host.pane,L"Only second performer changed");
    New(host.pane, L"PERFORMER", L"Third performer");
    New(host.pane, L"empty-field", L"");
    New(host.pane, L"large-value", longValue.c_str());
    Select(host.pane, Find(host.pane, L"large-value"));
    Check(Text(host.pane, IDC_TAG_VALUE) == longValue, "long values are not truncated on selection");
    SetDlgItemTextW(host.pane, IDC_TAG_NAME, L"renamed-value");
    Check(Find(host.pane, L"large-value") < 0 && Find(host.pane, L"renamed-value") >= 0, "rename updates selected row");
    New(host.pane, L"delete-me", L"Temporary");
    NMLVKEYDOWN key{};
    key.hdr.hwndFrom = GetDlgItem(host.pane, IDC_TAG_LIST);
    key.hdr.idFrom = IDC_TAG_LIST; key.hdr.code = LVN_KEYDOWN; key.wVKey = VK_DELETE;
    SendMessageW(host.pane, WM_NOTIFY, IDC_TAG_LIST, reinterpret_cast<LPARAM>(&key));
    Check(Find(host.pane, L"delete-me") < 0, "Delete key deletes selected row");
    Check(Bytes(path) == before, "drafts do not write before host OK");
    EnumChildWindows(host.pane, Bounds, reinterpret_cast<LPARAM>(host.pane));
    Check(!GetPropW(host.pane, L"Clipped"), "Advanced controls fit host pane");
    if (GetEnvironmentVariableW(L"OGG_TEST_CAPTURE_UI", nullptr, 0)) {
        Select(host.pane, Find(host.pane, L"DESCRIPTION"));
        ShowWindow(host.window, SW_SHOWNOACTIVATE);
        RedrawWindow(host.window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        CaptureDialog(host.pane, (directory / L"advanced-pane.bmp").c_str());
        ShowWindow(host.window, SW_HIDE);
    }
    Click(host.pane, IDOK);
    const auto saved = Document(path);
    Check(Values(saved, "title") == std::vector<std::wstring>{L"Basic Info title"}, "host OK writes Basic Info edits");
    Check(Values(saved, "performer") == std::vector<std::wstring>{L"First performer", L"Only second performer changed", L"Third performer"},
          "duplicate values survive as distinct comment entries");
    Check(Values(saved, "empty-field") == std::vector<std::wstring>{L""}, "empty field is distinct from deletion");
    Check(Values(saved, "renamed-value") == std::vector<std::wstring>{longValue}, "renamed long value is saved in full");
    Check(Values(saved, longName.c_str()) == std::vector<std::wstring>{L"Long name"}, "long unchanged field name survives save");
    Check(Document(path, 1).fields == second.fields, "other chained link is unchanged");
    Check(Decode(path) == pcm, "tag editing preserves decoded audio bit-exactly");
    const auto bytes = Bytes(path);
    for (const std::string token : {"METADATA_BLOCK_PICTURE=preserve-picture-byte-for-byte", "COVERART=preserve-coverart", "OPAQUE=\xff"})
        Check(std::search(bytes.begin(), bytes.end(), token.begin(), token.end()) != bytes.end(), "artwork and opaque comment bytes preserved");
    wchar_t value[256]{};
    Check(get(path.c_str(), "title", value, 256) && std::wstring(value) == L"Basic Info title", "unified save invalidates DLL metadata cache");
    Click(host.pane, IDOK);
    Check(Bytes(path) == bytes, "repeated OK without edits is a byte-exact no-op");
    host.Close();
    Check(!GetPropW(host.window, L"INBUILT_NOWRITEINFO"), "parent marker removed when pane closes");
    Check(GetWindowLongPtrW(host.formatInfo, GWL_STYLE) == host.formatStyle,
          "host-owned Format Info style restored when editor closes");

    host.Open(add, path);
    Click(host.pane, IDC_TAG_DELETE_ALL);
    Check(ListView_GetItemCount(GetDlgItem(host.pane, IDC_TAG_LIST)) == 0, "Delete All clears text rows");
    Check(host.fields[L"title"].empty(), "Delete All synchronizes Basic Info");
    Click(host.pane, IDCANCEL);
    host.Close();
    Check(Bytes(path) == bytes, "Cancel discards Delete All");
    host.Open(add, path);
    Click(host.pane, IDC_TAG_DELETE_ALL);
    Click(host.pane, IDOK);
    Check(Document(path).fields.empty(), "Delete All commits only on OK");
    const auto deleted = Bytes(path);
    const std::string art = "COVERART=preserve-coverart";
    Check(std::search(deleted.begin(), deleted.end(), art.begin(), art.end()) != deleted.end(), "Delete All preserves artwork");
    Check(Decode(path) == pcm, "Delete All preserves audio");
    host.Close();
}

void GuardTests(Host& host, AddPane add, const std::filesystem::path& directory) {
    const auto path = directory / L"guarded.ogg";
    MakeVorbis(path, 2, 44100, 7013, 42001);
    SetPropW(host.window, L"INBUILT_NOWRITEINFO", reinterpret_cast<HANDLE>(7));
    host.Open(add, path);
    host.Close();
    Check(GetPropW(host.window, L"INBUILT_NOWRITEINFO") == reinterpret_cast<HANDLE>(7), "preexisting parent property restored");
    RemovePropW(host.window, L"INBUILT_NOWRITEINFO");
    const auto baseline = Document(path);
    auto draft = baseline.fields;
    for (auto& field : draft) if (!_stricmp(field.name.c_str(), "title")) field.value = L"Unsaved title";
    std::wstring error;
    Tags external{{"artist", L"External edit"}};
    Check(WriteTags(path.c_str(), 0, external, error), "write concurrent external edit");
    const auto concurrent = Bytes(path);
    Check(!WriteTagDocument(path.c_str(), baseline, draft, error) && !error.empty() && Bytes(path) == concurrent,
          "stale editor snapshot cannot overwrite newer tags");
    const auto current = Document(path);
    draft = current.fields;
    draft.push_back({"invalid=name", L"No write"});
    Check(!WriteTagDocument(path.c_str(), current, draft, error) && !error.empty() && Bytes(path) == concurrent,
          "invalid Ogg field name returns an error without writing");
    const auto flac = directory / L"readonly.oga";
    MakeFlac(flac, 2, 44100, 7013);
    const auto original = Bytes(flac);
    host.Open(add, flac);
    Check(!IsWindowEnabled(GetDlgItem(host.pane, IDC_TAG_ADD_NEW)), "FLAC remains read-only");
    host.Field(L"title", L"Cannot write");
    Click(host.pane, IDOK);
    Check(Bytes(flac) == original, "read-only pane ignores host edits and OK");
    host.Close();
}

struct BasicField {
    const char* key;
    const wchar_t* native;
    const wchar_t* value;
};
const BasicField kBasicFields[] = {
    {"track", L"tracknumber", L"2/12"}, {"disc", L"discnumber", L"1/2"},
    {"bpm", L"bpm", L"123.5"}, {"title", L"title", L"Title — テスト"},
    {"artist", L"artist", L"Artist"}, {"album", L"album", L"Album"},
    {"albumartist", L"albumartist", L"Album artist"}, {"year", L"date", L"2026"},
    {"genre", L"genre", L"Genre"}, {"comment", L"comment", L"Line one\r\nLine two"},
    {"composer", L"composer", L"Composer"}, {"publisher", L"publisher", L"Publisher"},
};

void BasicInfoTests(Host& host, AddPane add, GetTag get, const std::filesystem::path& directory) {
    const auto path = directory / L"basic-info.ogg";
    MakeVorbis(path, 2, 96000, 96000, 44001);
    std::wstring error;
    const auto initial = Document(path);
    Check(WriteTagDocument(path.c_str(), initial, {}, error), "create file with no text tags");
    wchar_t value[4096]{};
    for (const auto& field : kBasicFields) {
        std::fill(std::begin(value), std::end(value), L'!');
        Check(get(path.c_str(), field.key, value, 4096) == 1 && !value[0],
              (std::string("empty Basic Info field stays enabled: ") + field.key).c_str());
    }
    Check(!get(path.c_str(), "not_a_supported_or_present_tag", value, 4096),
          "missing unknown field is not advertised as supported");
    host.Open(add, path);
    for (const auto& field : kBasicFields) {
        const std::wstring name(field.key, field.key + std::strlen(field.key));
        host.Field(name.c_str(), field.value);
        Select(host.pane, Find(host.pane, field.native));
        Check(Text(host.pane, IDC_TAG_VALUE) == field.value, "every Basic Info field creates an Advanced row");
    }
    Click(host.pane, IDOK);
    host.Close();
    for (const auto& field : kBasicFields)
        Check(get(path.c_str(), field.key, value, 4096) && std::wstring(value) == field.value,
              "all twelve Basic Info fields save and reload");
    host.Open(add, path);
    for (const auto& field : kBasicFields) {
        Select(host.pane, Find(host.pane, field.native));
        const std::wstring updated = std::wstring(field.value) + L" updated";
        SetValue(host.pane, updated.c_str());
        const std::wstring name(field.key, field.key + std::strlen(field.key));
        Check(host.fields[name] == updated, "every Advanced standard field updates Basic Info");
    }
    Click(host.pane, IDOK);
    host.Close();
    host.Open(add, path);
    for (const auto& field : kBasicFields) {
        const std::wstring name(field.key, field.key + std::strlen(field.key));
        host.Field(name.c_str(), L"");
    }
    Click(host.pane, IDOK);
    host.Close();
    Check(Document(path).fields.empty(), "Basic Info clears all standard fields without leaving aliases");
    for (const auto& field : kBasicFields)
        Check(get(path.c_str(), field.key, value, 4096) == 1 && !value[0],
              "cleared Basic Info field remains supported and enabled");
    std::wstring technical(4096, L'\0');
    Check(get(path.c_str(), "formatinformation", technical.data(), static_cast<int>(technical.size())),
          "host receives Format Info");
    technical.resize(std::wcslen(technical.c_str()));
    const auto size = std::filesystem::file_size(path);
    const auto prefix = L"Length: 1\r\nAverage bitrate: " + std::to_wstring(size / 125) +
        L" kbps\r\nFile size: " + std::to_wstring(size) + L" bytes\r\n";
    Check(technical.find(prefix) == 0, "Format Info starts with original length, container bitrate and file size");
    Check(technical.find(L"Channels: 2\r\nSampling rate: 96000 Hz\r\nSerial number: 44001\r\nVersion: 0\r\nVendor:\r\n") != std::wstring::npos,
          "Format Info uses original technical labels and separate vendor line");
}

std::wstring TranslateTechnical(const wchar_t* text) {
    if (!std::wcscmp(text, L"Length: ")) return L"Длительность: ";
    return text;
}

void TechnicalInfoTests() {
    TrackInfo info;
    info.codec = Codec::Vorbis;
    info.duration_ms = 30000;
    info.file_size = 415902;
    info.seekable = true;
    info.current.rate = 96000;
    info.current.channels = 2;
    info.current.frames = 2880000;
    info.current.bitrate = 106000;
    info.current.serial = 688178262;
    info.current.format_version = 0;
    info.current.vendor = L"Lavf63.16.100";
    info.links = {info.current};
    const auto result = FormatInfo(info);
    const std::wstring original = L"Length: 30\r\nAverage bitrate: 110 kbps\r\nFile size: 415902 bytes\r\n"
        L"Channels: 2\r\nSampling rate: 96000 Hz\r\nSerial number: 688178262\r\nVersion: 0\r\nVendor:\r\nLavf63.16.100";
    Check(result.find(original) == 0, "supplied reference's complete original Format Info block");
    Check(result.find(L"\r\n\r\nCodec: Ogg Vorbis\r\nDuration: 30000 ms\r\nSamples per channel: 2880000") != std::wstring::npos,
          "useful additional facts follow, not replace, the original block");
    Check(result.find(L"Source bit depth") == std::wstring::npos && result.find(L"Logical stream:") == std::wstring::npos,
          "no invented lossy bit depth or redundant single-link counter");
    Check(FormatInfo(info, TranslateTechnical).find(L"Длительность: 30\r\n") == 0,
          "fixed technical labels are localizable");
    info.current.nominal_bitrate = 128000;
    info.current.minimum_bitrate = 64000;
    info.current.maximum_bitrate = 192000;
    auto text = FormatInfo(info);
    Check(text.find(L"Nominal bitrate: 128 kbps") != std::wstring::npos &&
          text.find(L"Minimum bitrate: 64 kbps") != std::wstring::npos &&
          text.find(L"Maximum bitrate: 192 kbps") != std::wstring::npos,
          "nominal and bitrate bounds use actual header values, not duplicated nominal values");
    for (const auto& duration : {std::pair{61000, L"Length: 1:01\r\n"}, {3661000, L"Length: 1:01:01\r\n"}}) {
        info.duration_ms = duration.first;
        Check(FormatInfo(info).find(duration.second) == 0, "original minute/hour duration formatting");
    }
    info.file_size = 5000000000LL;
    Check(FormatInfo(info).find(L"File size: 5000000000 bytes") != std::wstring::npos, "64-bit source size is not truncated");
    info.codec = Codec::Flac;
    info.current.bits_per_sample = 24;
    Check(FormatInfo(info).find(L"Source bit depth: 24") != std::wstring::npos, "actual lossless PCM bit depth is shown");
    info.codec = Codec::Opus;
    info.current.header_gain_q8 = -320;
    info.current.decoder_note = L"Actual decoder detail";
    info.links.push_back(info.current);
    text = FormatInfo(info);
    Check(text.find(L"Header gain: -1.25 dB") != std::wstring::npos && text.find(L"Actual decoder detail") != std::wstring::npos &&
          text.find(L"Logical stream: 1 / 2") != std::wstring::npos, "known Opus gain, decoder detail and chained-link identity");
    info = {};
    info.codec = Codec::Opus;
    info.radio = true;
    text = FormatInfo(info);
    Check(text.find(L"File size:") == std::wstring::npos && text.find(L"Average bitrate:") == std::wstring::npos &&
          text.find(L"Length:") == std::wstring::npos && text.find(L"Version:") == std::wstring::npos,
          "unknown stream size, duration and version are not fabricated");

    info.current.tags = {{"comment", L""}, {"description", L"Fallback comment"}, {"date", L""}, {"year", L"2026"},
        {"tracknumber", L""}, {"track", L"3"}, {"discnumber", L""}, {"disc", L"2"},
        {"albumartist", L""}, {"album artist", L""}, {"ensemble", L"Fallback ensemble"},
        {"publisher", L""}, {"organization", L"Fallback publisher"}, {"category", L""}, {"contentgroup", L"Grouping"}};
    for (const auto& field : {std::pair{"comment", L"Fallback comment"}, {"year", L"2026"}, {"track", L"3"}, {"disc", L"2"},
            {"albumartist", L"Fallback ensemble"}, {"publisher", L"Fallback publisher"}, {"category", L"Grouping"}}) {
        std::wstring value;
        Check(Metadata(info, field.first, value) && value == field.second, "empty primary tag still uses historical alias fallback");
    }
}

void OpusTests(Host& host, AddPane add, const std::filesystem::path& directory) {
    const auto path = directory / L"opus.ogg";
    MakeOpus(path, 2, 7013, 43001, {}, 256);
    host.Open(add, path);
    host.Field(L"replaygain_track_gain", L"-2.25 dB");
    Select(host.pane, Find(host.pane, L"r128_track_gain"));
    Check(Text(host.pane, IDC_TAG_VALUE) == L"-2112", "Basic Info ReplayGain maps to Opus R128 including header gain");
    host.Field(L"rating", L"4");
    Select(host.pane, Find(host.pane, L"rating"));
    Check(Text(host.pane, IDC_TAG_VALUE) == L"80", "host rating uses native 0-100 field");
    SetValue(host.pane,L"60");
    Check(host.fields[L"rating"] == L"3", "Advanced native rating maps back to host stars");
    Click(host.pane, IDOK);
    Check(Values(Document(path), "r128_track_gain") == std::vector<std::wstring>{L"-2112"}, "Opus unified edit saved");
    host.Close();
    host.Open(add, path);
    host.Field(L"replaygain_track_gain", L"");
    Check(Find(host.pane, L"r128_track_gain") < 0, "ReplayGain deletion removes native R128");
    Click(host.pane, IDOK);
    Check(Values(Document(path), "r128_track_gain").empty(), "Opus native gain deletion saved");
    host.Close();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    RegistrySandbox registry;
    HMODULE dll = nullptr;
    winamp::InModule* module = nullptr;
    try {
        Check(argc == 3, "usage: tag_editor_tests DLL DIRECTORY");
        const std::filesystem::path directory = argv[2];
        std::filesystem::create_directories(directory);
        Host host;
        dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(dll, "load tag editor DLL");
        auto getModule = reinterpret_cast<winamp::InModule*(__cdecl*)()>(GetProcAddress(dll, "winampGetInModule2"));
        const auto add = reinterpret_cast<AddPane>(GetProcAddress(dll, "winampAddUnifiedFileInfoPane"));
        const auto get = reinterpret_cast<GetTag>(GetProcAddress(dll, "winampGetExtendedFileInfoW"));
        Check(getModule && add && get, "unified metadata exports");
        module = getModule(); module->hDllInstance = dll; module->hMainWindow = host.window; module->Init();
        ModalGuard noDialogs;
        wchar_t name[64]{};
        Check(!add(1, L"unused", host.window, name, 64), "only one plugin-owned Advanced tab");
        EditingTests(host, add, get, directory);
        GuardTests(host, add, directory);
        BasicInfoTests(host, add, get, directory);
        TechnicalInfoTests();
        OpusTests(host, add, directory);
        Check(blockedDialogs == 0, "unattended editor tests must not attempt modal dialogs");
        host.Close();
        module->Quit(); module = nullptr;
        FreeLibrary(dll); dll = nullptr;
        std::cout << "Unified Advanced: host sync, multiline/long/repeated fields, rename, deletion, OK/Cancel, artwork/audio preservation passed.\n";
        return 0;
    } catch (const std::exception& error) {
        if (module) module->Quit();
        if (dll) FreeLibrary(dll);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
