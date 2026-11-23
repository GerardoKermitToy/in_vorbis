// SPDX-License-Identifier: GPL-3.0-only
#include "language.hpp"
#include "matroska.hpp"
#include "resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ogg_input;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class T> void Result(void* target, const T& value) {
    if (target) std::memcpy(target, &value, sizeof(value));
}
template <class T> T Argument(void** args, int count, int index) {
    Check(args && count > index && args[index], "dispatch argument exists");
    T value{};
    std::memcpy(&value, args[index], sizeof(value));
    return value;
}

std::map<unsigned, std::wstring> CatalogueResourceEntries() {
    std::size_t count = 0;
    const auto* entries = LanguageStrings(count);
    std::map<unsigned, std::wstring> resources;
    for (std::size_t i = 0; i < count; ++i) resources.emplace(entries[i].id, L"present");
    return resources;
}

// A private, temporary resource-only copy of THIS independently authored test
// executable. No installed DLL, pack, compiler subprocess, or foreign template
// is used; LOAD_LIBRARY_AS_DATAFILE never executes the copy's code.
class ResourceModule {
public:
    explicit ResourceModule(const std::map<unsigned, std::wstring>& entries) {
        HANDLE update = nullptr;
        try {
            wchar_t directory[MAX_PATH]{};
            Check(GetTempPathW(MAX_PATH, directory) > 0, "temporary resource directory");
            Check(GetTempFileNameW(directory, L"ogl", 0, path_) != 0, "temporary language resource file");
            std::array<wchar_t, 32768> executable{};
            Check(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())) > 0,
                "test executable resource source");
            Check(CopyFileW(executable.data(), path_, FALSE) != FALSE, "copy independent executable for resources");
            update = BeginUpdateResourceW(path_, TRUE);
            Check(update != nullptr, "create private resource table");
            std::map<unsigned, std::array<std::wstring, 16>> blocks;
            for (const auto& entry : entries) {
                Check(entry.first > 0 && entry.first <= 65535 && entry.second.size() <= 65535,
                    "valid fixture string resource");
                blocks[entry.first / 16 + 1][entry.first % 16] = entry.second;
            }
            for (const auto& block : blocks) {
                std::vector<WORD> data;
                for (const auto& text : block.second) {
                    data.push_back(static_cast<WORD>(text.size()));
                    for (const wchar_t c : text) data.push_back(static_cast<WORD>(c));
                }
                Check(UpdateResourceW(update, RT_STRING, MAKEINTRESOURCEW(block.first),
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), data.data(),
                    static_cast<DWORD>(data.size() * sizeof(WORD))) != FALSE, "write private STRINGTABLE block");
            }
            const BOOL ended = EndUpdateResourceW(update, FALSE);
            update = nullptr;
            Check(ended != FALSE, "finish private language resources");
            module_ = LoadLibraryExW(path_, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            Check(module_ != nullptr, "map private language resources without executing code");
        } catch (...) {
            if (update) EndUpdateResourceW(update, TRUE);
            if (*path_) DeleteFileW(path_);
            throw;
        }
    }
    ~ResourceModule() {
        if (module_) FreeLibrary(module_);
        if (*path_) DeleteFileW(path_);
    }
    ResourceModule(const ResourceModule&) = delete;
    ResourceModule& operator=(const ResourceModule&) = delete;
    HINSTANCE get() const noexcept { return module_; }
private:
    wchar_t path_[MAX_PATH]{};
    HMODULE module_ = nullptr;
};

struct Reply {
    enum Kind { Text, Missing, Empty, Null, Unterminated, External, Shifted, Truncated, Unsupported, Throw } kind = Text;
    std::wstring text;
};

class MockLanguage final : public matroska::Dispatchable {
public:
    HINSTANCE own = GetModuleHandleW(nullptr);
    HINSTANCE module = nullptr;
    HINSTANCE legacy = nullptr;
    std::map<unsigned, Reply> strings;
    std::map<unsigned, Reply> legacy_strings;
    int startup_calls = 0, lookup_calls = 0, string_calls = 0, legacy_calls = 0;
    bool startup_supported = true, lookup_supported = true;

    int _dispatch(int message, void* result, void** args, int count) override {
        if (message == language::Message::StartLanguageSupport) {
            Check(count == 2, "language startup arity");
            Check(Argument<HINSTANCE>(args, count, 0) == own, "startup uses our DLL only");
            Check(IsEqualGUID(Argument<GUID>(args, count, 1), language::kModuleGuid), "independent module GUID");
            ++startup_calls;
            Result(result, module);
            return startup_supported ? 1 : 0;
        }
        if (message == language::Message::FindDllHandleByGuid) {
            Check(count == 1, "module lookup arity");
            Check(IsEqualGUID(Argument<GUID>(args, count, 0), language::kLegacyModuleGuid), "read-only legacy lookup");
            ++lookup_calls;
            Result(result, legacy);
            return lookup_supported ? 1 : 0;
        }
        if (message != language::Message::GetStringW) return 0;
        Check(count == 5, "GetStringW arity");
        const auto localized = Argument<HINSTANCE>(args, count, 0);
        const auto original = Argument<HINSTANCE>(args, count, 1);
        const unsigned id = Argument<unsigned>(args, count, 2);
        auto* output = Argument<wchar_t*>(args, count, 3);
        const auto capacity = Argument<std::size_t>(args, count, 4);
        Check(localized && localized == original && original != own, "no colliding original resource fallback");
        Check(output && capacity >= 32, "caller-owned language buffer");
        const bool is_legacy = localized == legacy;
        auto& table = is_legacy ? legacy_strings : strings;
        ++string_calls;
        if (is_legacy) {
            ++legacy_calls;
            Check(id == 55 || id == 56, "only exact public legacy label IDs");
        }
        const auto found = table.find(id);
        const Reply reply = found == table.end() ? Reply{Reply::Missing, {}} : found->second;
        wchar_t* pointer = output;
        switch (reply.kind) {
        case Reply::Throw: throw std::runtime_error("mock optional service failure");
        case Reply::Unterminated: std::fill_n(output, capacity, L'x'); break;
        case Reply::Truncated: std::fill_n(output, capacity, L'x'); output[capacity - 1] = 0; break;
        case Reply::External: pointer = reinterpret_cast<wchar_t*>(1); break;
        case Reply::Shifted: output[0] = 0; pointer = output + 1; break;
        case Reply::Missing: pointer = nullptr; break;
        case Reply::Empty: output[0] = 0; break;
        case Reply::Null: output[0] = L'x'; output[1] = 0; pointer = nullptr; break;
        case Reply::Unsupported: output[0] = L'x'; output[1] = 0; break;
        case Reply::Text:
            Check(reply.text.size() + 1 < capacity, "mock string fits");
            std::copy(reply.text.begin(), reply.text.end(), output);
            output[reply.text.size()] = 0;
            break;
        }
        Result(result, pointer);
        return reply.kind == Reply::Unsupported ? 0 : 1;
    }
};

class MockFactory final : public matroska::ServiceFactory {
public:
    void* service = nullptr;
    bool supported = true;
    int acquired = 0, released = 0;
    int _dispatch(int message, void* result, void** args, int count) override {
        if (message == matroska::ServiceFactoryMessage::GetInterface) {
            Check(count == 1 && Argument<int>(args, count, 0) == FALSE, "non-global-lock acquisition");
            ++acquired;
            Result(result, service);
            return supported ? 1 : 0;
        }
        if (message == matroska::ServiceFactoryMessage::ReleaseInterface) {
            Check(count == 1 && Argument<void*>(args, count, 0) == service, "release exactly acquired interface");
            ++released;
            Result(result, 1);
            return 1;
        }
        throw std::runtime_error("unexpected factory operation");
    }
};

class MockManager final : public matroska::ServiceManager {
public:
    matroska::ServiceFactory* factory = nullptr;
    bool supported = true;
    int calls = 0;
    int _dispatch(int message, void* result, void** args, int count) override {
        Check(message == language::kServiceByGuid && count == 1, "lookup, never register/deregister foreign services");
        Check(IsEqualGUID(Argument<GUID>(args, count, 0), language::kServiceGuid), "language service GUID");
        ++calls;
        Result(result, factory);
        return supported ? 1 : 0;
    }
};

struct HostState { LRESULT manager = 0; int ipc_calls = 0; };
LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == matroska::kServiceManagerMessage && w == 0 && l == matroska::kGetApiService) {
        auto* state = reinterpret_cast<HostState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        ++state->ipc_calls;
        return state->manager;
    }
    return DefWindowProcW(window, message, w, l);
}

class Window {
public:
    HWND handle = nullptr;
    ~Window() { if (handle) DestroyWindow(handle); }
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

class Fixture {
public:
    ResourceModule module{CatalogueResourceEntries()};
    ResourceModule legacy_module{{{55, L"present"}, {56, L"present"}}};
    MockLanguage service;
    MockFactory factory;
    MockManager manager;
    HostState state;
    Window host;
    Fixture() {
        service.module = module.get();
        factory.service = &service;
        manager.factory = &factory;
        state.manager = reinterpret_cast<LRESULT>(&manager);
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = HostProc;
        type.lpszClassName = L"IndependentOggLanguageTestHost";
        Check(RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register mock host");
        host.handle = CreateWindowExW(0, type.lpszClassName, L"File Information", WS_OVERLAPPED,
            0, 0, 400, 240, nullptr, nullptr, type.hInstance, &state);
        Check(host.handle != nullptr, "create mock host");
    }
    ~Fixture() { ShutdownLanguage(); }
    void Initialize() { InitializeLanguage(host.handle, service.own); }
};

std::wstring Text(HWND window) {
    wchar_t text[512]{};
    GetWindowTextW(window, text, 512);
    return text;
}
HWND Child(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style, int id) {
    HWND child = CreateWindowExW(0, type, text, WS_CHILD | style, 8, 10 + id % 80, 180, 20,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    Check(child != nullptr, "create independent test control");
    return child;
}

void CatalogueTests() {
    std::size_t count = 0;
    const auto* strings = LanguageStrings(count);
    Check(strings && count >= 150, "complete independent UI catalogue");
    std::set<unsigned> ids;
    std::set<std::wstring> phrases;
    for (std::size_t i = 0; i < count; ++i) {
        Check(strings[i].id > 0 && strings[i].id <= 65535, "Win32 string ID range");
        Check(ids.insert(strings[i].id).second, "no stable ID collisions");
        Check(phrases.insert(strings[i].fallback).second, "no ambiguous phrase mappings");
        Check(LanguageStringId(strings[i].fallback) == strings[i].id, "catalogue round trip");
        Check(Localized(strings[i].fallback) == strings[i].fallback, "exact fallback for every phrase");
    }
    Check(LanguageStringId(L"File Information") == 20000, "stable caption ID");
    Check(LanguageStringId(L"Processing") == 20126, "stable new tab ID");
    Check(!LanguageStringId(nullptr) && !LanguageStringId(L"") && !LanguageStringId(L"unknown user text"), "unknown ID zero");
    Check(Localized(nullptr).empty() && Localized(L"").empty(), "null and empty fallback");
    Check(Localized(L"  unknown user text \r\n") == L"  unknown user text \r\n", "unknown fallback unchanged");
    Check(!IsEqualGUID(language::kModuleGuid, language::kLegacyModuleGuid), "never impersonate original module");
}

void MissingServiceTests() {
    Fixture fixture;
    InitializeLanguage(nullptr, fixture.service.own);
    InitializeLanguage(fixture.host.handle, nullptr);
    Check(fixture.state.ipc_calls == 0, "invalid input makes no IPC query");
    for (const LRESULT absent : {LRESULT{0}, LRESULT{1}}) {
        fixture.state.manager = absent;
        fixture.Initialize();
        Check(Localized(L"Close") == L"Close", "absent/unsupported IPC fallback");
    }
    fixture.state.manager = reinterpret_cast<LRESULT>(&fixture.manager);
    fixture.manager.supported = false;
    fixture.Initialize();
    Check(fixture.factory.acquired == 0, "unsupported manager result ignored");
    fixture.manager.supported = true;
    fixture.manager.factory = nullptr;
    fixture.Initialize();
    Check(fixture.factory.acquired == 0, "missing service factory");
    fixture.manager.factory = &fixture.factory;
    fixture.factory.supported = false;
    fixture.Initialize();
    Check(fixture.factory.released == 0, "unsupported interface not released");
    fixture.factory.supported = true;
    fixture.factory.service = reinterpret_cast<void*>(1);
    fixture.Initialize();
    Check(fixture.factory.released == 0, "sentinel interface not released");
    fixture.factory.service = &fixture.service;
    fixture.service.module = fixture.service.own;
    fixture.Initialize();
    Check(fixture.service.string_calls == 0, "own-module absence fallback cannot collide with own resources");
    Check(fixture.factory.released == 1, "valid acquired service released even without a module");
    fixture.service.module = nullptr;
    fixture.Initialize();
    Check(fixture.service.string_calls == 0 && fixture.factory.released == 2, "null module fallback");
    fixture.service.module = reinterpret_cast<HINSTANCE>(1);
    fixture.Initialize();
    Check(fixture.service.string_calls == 0, "sentinel module rejected");
    fixture.service.module = fixture.module.get();
    fixture.service.startup_supported = false;
    fixture.Initialize();
    Check(fixture.service.string_calls == 0, "unhandled startup return ignored");
}

void PartialAndMalformedTests() {
    Fixture fixture;
    ResourceModule partial{{{LanguageStringId(L"Close"), L"present"},
        {LanguageStringId(L"Cancel"), L""}}};
    fixture.service.module = partial.get();
    fixture.service.strings[LanguageStringId(L"Close")] = {Reply::Text, L"Fermer"};
    fixture.service.strings[LanguageStringId(L"Save")] = {Reply::Text, L"service diagnostic for missing entry"};
    fixture.service.strings[LanguageStringId(L"Cancel")] = {Reply::Text, L"service diagnostic for empty entry"};
    fixture.service.strings[LanguageStringId(L"File Information")] = {Reply::Text, L"service diagnostic for absent block"};
    fixture.Initialize();
    Check(Localized(L"Close") == L"Fermer", "partial module translated entry");
    Check(Localized(L"Save") == L"Save" && Localized(L"Cancel") == L"Cancel" &&
        Localized(L"File Information") == L"File Information", "resource absence/empty entry exact fallback");
    Check(fixture.service.string_calls == 1, "missing resources never ask service for diagnostic text");
    Check(fixture.factory.acquired == 1 && fixture.factory.released == 1, "no retained raw service pointers");
    const std::wstring saved = Localized(L"Close");
    fixture.service.strings.clear();
    Check(Localized(L"Close") == L"Fermer" && saved == L"Fermer", "owned snapshot lifetime");
    fixture.service.module = fixture.module.get();
    fixture.Initialize();
    Check(Localized(L"Close") == L"Close", "reinitialize drops stale translations");
    const std::array<Reply::Kind, 8> bad{{Reply::Missing, Reply::Empty, Reply::Null,
        Reply::Unterminated, Reply::External, Reply::Shifted, Reply::Truncated, Reply::Unsupported}};
    const std::array<const wchar_t*, 8> labels{{L"Close", L"Save", L"Cancel", L"OK", L"Tags", L"Next", L"Previous", L"Stereo"}};
    for (std::size_t i = 0; i < labels.size(); ++i)
        fixture.service.strings[LanguageStringId(labels[i])] = {bad[i], {}};
    fixture.Initialize();
    for (const auto* label : labels) Check(Localized(label) == label, "malformed optional string exact fallback");
    fixture.service.strings[LanguageStringId(L"File Information")] = {Reply::Throw, {}};
    const int releases = fixture.factory.released;
    fixture.Initialize();
    Check(fixture.factory.released == releases + 1, "exception path releases valid interface");
    Check(Localized(L"Close") == L"Close", "exception leaves English cache");
}

void LegacyTests() {
    Fixture fixture;
    fixture.service.module = nullptr;
    fixture.service.legacy = fixture.legacy_module.get();
    fixture.service.legacy_strings[55] = {Reply::Text, L"Nom"};
    fixture.service.legacy_strings[56] = {Reply::Text, L"Valeur"};
    fixture.Initialize();
    Check(Localized(L"Name") == L"Nom" && Localized(L"Value") == L"Valeur", "existing pack field-label reuse");
    Check(Localized(L"Name:") == L"Name:" && Localized(L"Close") == L"Close", "no guessed legacy semantics");
    Check(fixture.service.startup_calls == 1 && fixture.service.lookup_calls == 1 &&
        fixture.service.legacy_calls == 2, "original module read-only, no foreign startup");
    fixture.service.module = fixture.module.get();
    fixture.service.strings[LanguageStringId(L"Name")] = {Reply::Text, L"Own Name"};
    fixture.Initialize();
    Check(Localized(L"Name") == L"Own Name", "independent module wins over legacy");
    fixture.service.legacy_strings[56] = {Reply::Empty, {}};
    fixture.Initialize();
    Check(Localized(L"Value") == L"Value", "missing legacy entry exact fallback");
    ShutdownLanguage();
    Check(Localized(L"Name") == L"Name", "shutdown clears legacy cache too");
}

void DialogTests() {
    Fixture fixture;
    fixture.service.strings[LanguageStringId(L"File Information")] = {Reply::Text, L"Informations"};
    fixture.service.strings[LanguageStringId(L"Location:")] = {Reply::Text, L"Adresse :"};
    fixture.service.strings[LanguageStringId(L"Close")] = {Reply::Text, L"Fermer"};
    fixture.Initialize();
    HWND label = Child(fixture.host.handle, L"Static", L"Location:", SS_LEFT, -1);
    HWND button = Child(fixture.host.handle, L"Button", L"Close", BS_PUSHBUTTON, IDOK);
    HWND edit = Child(fixture.host.handle, L"Edit", L"Close", ES_AUTOHSCROLL, IDC_TAG_ARTIST);
    HWND readonly_edit = Child(fixture.host.handle, L"Edit", L"Close", ES_READONLY, IDC_INFO_DETAILS);
    HWND combo = Child(fixture.host.handle, L"ComboBox", L"Close", CBS_DROPDOWN, IDC_CONFIG_RATE);
    SetWindowTextW(combo, L"Close");
    HWND status = Child(fixture.host.handle, L"Static", L"Close", SS_LEFT, IDC_INFO_STATUS);
    HWND nested = Child(button, L"Static", L"Location:", SS_LEFT, 9000);
    SendMessageW(edit, EM_SETSEL, 1, 3);
    const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
    RECT before{}, after{};
    GetWindowRect(button, &before);
    const int calls = fixture.service.string_calls;
    LocalizeDialog(fixture.host.handle);
    Check(Text(fixture.host.handle) == L"Informations", "dialog caption text only");
    Check(Text(label) == L"Adresse :" && Text(button) == L"Fermer", "static/button labels translated");
    Check(Text(edit) == L"Close" && Text(readonly_edit) == L"Close", "editable and read-only user fields untouched");
    Check(Text(combo) == L"Close" && Text(status) == L"Close", "editable combo and runtime status untouched");
    Check(Text(nested) == L"Location:", "nested/foreign control interiors untouched");
    DWORD first = 0, last = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
    Check(first == 1 && last == 3, "edit selection unchanged");
    GetWindowRect(button, &after);
    Check(EqualRect(&before, &after) && GetWindowLongPtrW(button, GWL_STYLE) == style &&
        GetDlgCtrlID(button) == IDOK, "layout/styles/control IDs preserved");
    Check(fixture.service.string_calls == calls, "dialog localization never touches released service");
    LocalizeDialog(fixture.host.handle);
    Check(Text(button) == L"Fermer", "repeated pass harmless");
    ShutdownLanguage();
    Check(Localized(L"Close") == L"Close", "shutdown exact fallback");
}

void ThreadTests() {
    Fixture fixture;
    std::thread wrong_thread([&] { fixture.Initialize(); LocalizeDialog(fixture.host.handle); });
    wrong_thread.join();
    Check(fixture.state.ipc_calls == 0, "no cross-thread host/UI calls");
    fixture.service.strings[LanguageStringId(L"Close")] = {Reply::Text, L"Fermer"};
    std::atomic<bool> stop{false}, valid{true};
    std::thread reader([&] {
        while (!stop.load()) {
            const auto text = Localized(L"Close");
            if (text != L"Close" && text != L"Fermer") valid.store(false);
        }
    });
    for (int i = 0; i < 10; ++i) { fixture.Initialize(); ShutdownLanguage(); }
    stop.store(true);
    reader.join();
    Check(valid.load(), "owned lookup copies synchronized with reset");
}

void PrintTemplate() {
    std::wcout << L"// Independent Ogg language module, text resources only.\n"
        L"#include <windows.h>\nLANGUAGE LANG_ENGLISH, SUBLANG_ENGLISH_US\nSTRINGTABLE\nBEGIN\n"
        L"    65535 \"{65E0E78D-78C1-4C34-B11E-87FD6F26485C}\"\n";
    std::size_t count = 0;
    const auto* strings = LanguageStrings(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::wcout << L"    " << strings[i].id << L" \"";
        for (const wchar_t* p = strings[i].fallback; *p; ++p) {
            switch (*p) {
            case L'\\': std::wcout << L"\\\\"; break;
            case L'\"': std::wcout << L"\\\""; break;
            case L'\r': std::wcout << L"\\r"; break;
            case L'\n': std::wcout << L"\\n"; break;
            case L'\t': std::wcout << L"\\t"; break;
            default: std::wcout << *p; break;
            }
        }
        std::wcout << L"\"\n";
    }
    std::wcout << L"END\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wcscmp(argv[1], L"--template") == 0) { PrintTemplate(); return 0; }
        CatalogueTests();
        MissingServiceTests();
        PartialAndMalformedTests();
        LegacyTests();
        DialogTests();
        ThreadTests();
        std::cout << "Language tests: stable IDs, optional Wasabi service, partial/malformed packs, legacy labels, UI text-only behavior, lifetime and threads passed.\n";
        return 0;
    } catch (const std::exception& error) {
        ShutdownLanguage();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
