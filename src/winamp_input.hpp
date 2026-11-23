#pragma once

// Minimal public Winamp 5.x input/output ABI, declared locally rather than
// redistributing SDK implementation code. Field order matches IN2.H / OUT.H.
// Reference: https://github.com/alexfreud/winamp/tree/community/Src/Winamp
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace winamp {
inline constexpr int kInputVersion = 0x0F000100; // Unicode paths, ANSI description
inline constexpr int kUsesOutputPlugin = 1;
inline constexpr UINT kEndOfFile = WM_USER + 2;
inline constexpr std::size_t kTitleCapacity = 2048;

struct OutModule {
    int version;
    char* description;
    std::intptr_t id;
    HWND hMainWindow;
    HINSTANCE hDllInstance;
    void (__cdecl* Config)(HWND);
    void (__cdecl* About)(HWND);
    void (__cdecl* Init)();
    void (__cdecl* Quit)();
    int (__cdecl* Open)(int, int, int, int, int);
    void (__cdecl* Close)();
    int (__cdecl* Write)(char*, int);
    int (__cdecl* CanWrite)();
    int (__cdecl* IsPlaying)();
    int (__cdecl* Pause)(int);
    void (__cdecl* SetVolume)(int);
    void (__cdecl* SetPan)(int);
    void (__cdecl* Flush)(int);
    int (__cdecl* GetOutputTime)();
    int (__cdecl* GetWrittenTime)();
};

struct InModule {
    int version;
    char* description;
    HWND hMainWindow;
    HINSTANCE hDllInstance;
    char* FileExtensions;
    int is_seekable;
    int UsesOutputPlug;
    void (__cdecl* Config)(HWND);
    void (__cdecl* About)(HWND);
    void (__cdecl* Init)();
    void (__cdecl* Quit)();
    void (__cdecl* GetFileInfo)(const wchar_t*, wchar_t*, int*);
    int (__cdecl* InfoBox)(const wchar_t*, HWND);
    int (__cdecl* IsOurFile)(const wchar_t*);
    int (__cdecl* Play)(const wchar_t*);
    void (__cdecl* Pause)();
    void (__cdecl* UnPause)();
    int (__cdecl* IsPaused)();
    void (__cdecl* Stop)();
    int (__cdecl* GetLength)();
    int (__cdecl* GetOutputTime)();
    void (__cdecl* SetOutputTime)(int);
    void (__cdecl* SetVolume)(int);
    void (__cdecl* SetPan)(int);
    void (__cdecl* SAVSAInit)(int, int);
    void (__cdecl* SAVSADeInit)();
    void (__cdecl* SAAddPCMData)(void*, int, int, int);
    int (__cdecl* SAGetMode)();
    int (__cdecl* SAAdd)(void*, int, int);
    void (__cdecl* VSAAddPCMData)(void*, int, int, int);
    int (__cdecl* VSAGetMode)(int*, int*);
    int (__cdecl* VSAAdd)(void*, int);
    void (__cdecl* VSASetInfo)(int, int);
    int (__cdecl* dsp_isactive)();
    int (__cdecl* dsp_dosamples)(short*, int, int, int, int);
    void (__cdecl* EQSet)(int, char[10], int);
    void (__cdecl* SetInfo)(int, int, int, int);
    OutModule* outMod;
    void* service;
};

static_assert(sizeof(void*) == 4, "Winamp 5.9 is a 32-bit host");
static_assert(std::is_standard_layout_v<InModule>);
static_assert(sizeof(OutModule) == 80);
static_assert(sizeof(InModule) == 156);
static_assert(offsetof(InModule, outMod) == 148);
} // namespace winamp
