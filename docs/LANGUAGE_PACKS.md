# Optional Winamp language packs

This is an independently authored GPL-3.0-only text-localization layer. It uses
public Winamp/Wasabi ABI facts; it does not incorporate historical `in_vorbis`
implementation code, dialog resources, images, or translations.

## Scope and compatibility

The plugin always works without a language service or language pack. Its own
English text is the fallback for each individual string, not a substitute string
from a resource with an accidentally matching number.

The independent language-module GUID is:

```text
{65E0E78D-78C1-4C34-B11E-87FD6F26485C}
```

This deliberately differs from the historical `InVorbisLangGUID`. Existing packs
usually do not contain this new module. Accordingly, most text in an existing
pack will remain English until a translator adds the independent module.

There are exactly two narrowly scoped read-only compatibility mappings:

| Independent English phrase | Independent ID | Historical string constant | Historical ID |
| --- | ---: | --- | ---: |
| `Name` | 20062 | `IDS_NAME` | 55 |
| `Value` | 20063 | `IDS_VALUE` | 56 |

These are tag field/column labels. They are read from a module already indexed
under the public historical GUID
`{5C5BCA4E-279E-4867-8E24-58C8B186959A}`. The independent module's translation has
priority. No other historical string mappings are guessed: `Name:` is not
silently treated as `Name`, for example. The historical module is never started
or registered as our module, and its dialogs are never instantiated.

## Integration

The public API is in `src/language.hpp`:

```cpp
void InitializeLanguage(HWND host, HINSTANCE own_module) noexcept;
void ShutdownLanguage() noexcept;
std::wstring Localized(const wchar_t* fallback);
unsigned LanguageStringId(const wchar_t* fallback) noexcept;
void LocalizeDialog(HWND dialog);
```

- Call `InitializeLanguage` from plugin initialization on the host window's own
  UI thread, passing the real host window and this independent DLL's instance.
- Call `ShutdownLanguage` during plugin teardown. It does not access the host,
  and remains safe if the host/service has already disappeared.
- Use `Localized(L"fixed English UI phrase")` for built-in combo/tab text,
  columns, messages, and fixed pieces of dynamic UI text. Do not pass paths,
  metadata values, user-entered content, settings identifiers, or protocol data
  for translation. An unknown phrase is returned unchanged.
- Call `LocalizeDialog` during `WM_INITDIALOG`. It changes recognized caption
  text and direct textual `Static`/`Button` children only. Editable and read-only
  edit fields, combo-box contents, nested control interiors, and runtime-data
  static controls are not rewritten. Localize fixed runtime messages explicitly
  where they are produced.
- Continue creating every dialog from **our own** `HINSTANCE` and independently
  authored dialog resources. The language module is a string source only.

`LocalizeDialog` neither changes control IDs nor moves/resizes controls, changes
fonts, or substitutes templates. Long translations can therefore clip; translators
must fit the independent layout. It is a UI-thread API. `Localized` returns an
owned `std::wstring`, is synchronized with cache reset, and can be called on
worker threads without dispatching to Winamp. Reinitialize on the host UI thread
to refresh a changed pack; already-open windows are not automatically rewritten.

Build integration is intentionally small:

- Add `src/language.cpp` to the `in_vorbis` target.
- The independent test target uses `tests/language_tests.cpp` and
  `src/language.cpp`, includes `src`, links `user32`, and uses the project's
  normal Win32 C++17/Unicode compile settings. It does not need audio libraries,
  `matroska.cpp`, or an installed Winamp SDK.
- `matroska.hpp` supplies the existing one-vtable-slot Wasabi dispatch ABI, which
  is deliberately restricted to 32-bit Winamp.

The integrated test target is named `ogg_language_tests` and its CTest name is
`ogg_language`.

## Stable IDs and translation templates

IDs are an explicit append-only catalogue in `src/language.cpp`, not a truncated
hash or the dialog/control resource numbers. The initial catalogue has IDs
20000 through 20169, including the Processing tab, surround modes, dither/noise
shaping, native chained output, host-setting labels, exact FLAC, Opus HD/OSCE,
and Ambisonics rendering options.

`LanguageStringId` performs an exact, case-sensitive phrase lookup and returns
zero for an unknown, empty, or null phrase. `LanguageStrings(size_t&)` exposes the
same immutable catalogue for tools. Compile-time validation and tests reject
repeated IDs and ambiguous duplicate phrases.

To produce the complete English `STRINGTABLE` template:

```powershell
.\build\vs2022-win32\tests\Release\ogg_language_tests.exe --template | Set-Content -Encoding UTF8 .\ogg-language.rc
```

A ready-to-edit copy is also provided as `localization/en-US.rc`.

Use the actual executable location if your build directory differs. The output
contains all string IDs and the public language-module identification resource:

```rc
65535 "{65E0E78D-78C1-4C34-B11E-87FD6F26485C}"
```

Resource ID 65535 is the SDK's `LANG_DLL_GUID_STRING_ID`, not a translatable UI
phrase. Keep it unchanged. Change the `LANGUAGE` directive and the UI text for
the destination language, preserving numeric IDs, significant spaces, and
escaped newlines. Do not add or copy dialog templates.

An example resource-only module build from a Visual Studio x86 tools prompt is:

```powershell
rc.exe /nologo /c65001 /fo ogg-language.res ogg-language.rc
```

```powershell
link.exe /nologo /dll /noentry /machine:x86 /out:in_vorbis_gpl.lng ogg-language.res
```

Package the resulting resource module using Winamp's normal language-pack
mechanism so the language service indexes it by the independent GUID. Use a
distinct module filename rather than overwriting a historical module. These
commands create a resource-only PE, not a complete language-pack installer;
actual pack discovery and distribution remain the translator's responsibility.

When adding UI text, append a new, unused ID and its exact English fallback to
`kStrings`. Never renumber/reuse an old ID. A revised phrase gets a new ID; retain
the old entry so existing translation templates remain stable. Unknown new text
continues to display its exact English fallback until the catalogue is updated.

## Missing-string and lifetime behavior

Initialization obtains the optional service through `WM_USER` /
`IPC_GET_API_SERVICE` (3025), looks up the language service by its public GUID,
and acquires the interface without requesting a global interface lock.

All translated strings are copied during initialization. The factory interface
is released before initialization returns. No service-manager, factory,
language-interface, resource-string pointer, or module handle is retained for
subsequent lookups or teardown. Modules are host-owned and are not freed by this
layer. No service is registered by this component, so there is no service to
deregister. The public header exposes no callable language-shutdown method;
this layer does not invent a signature for the reserved dispatch number 31.

Fallback behavior includes:

- Missing host/service/factory/interface; old hosts returning the unsupported
  IPC sentinel `1`; and unsupported dispatch calls.
- An absent module, including `StartLanguageSupport` returning the original DLL
  handle rather than a translation module.
- Missing/empty actual `STRINGTABLE` entries. These are checked with the counted
  resource form of `LoadStringW` **before** asking the service, so a service's
  diagnostic text for a missing string cannot replace the English fallback.
- A null or unexpected `GetStringW` return pointer, an empty result, no terminator,
  or a result at the truncation boundary. Unexpected pointers are not dereferenced.
  The accepted limit is 4094 UTF-16 code units per string.
- Reinitialization and shutdown, which clear both independent and legacy cached
  translations. Null input to `Localized` returns an empty string.

Both `GetStringW` module arguments deliberately name the selected **language
module**, never our original DLL. An absent module is not queried at all. This
prevents fallback into unrelated original resources with colliding numbers.
Optional-service/allocation C++ failures during initialization leave the English
cache usable; the host's in-process ABI must still be valid.

## Tests and verification limits

The tests use mocked Wasabi IPC/services and temporary, independently generated
resource-only copies of the test executable. They do not execute those copies,
load a Winamp installation, write the registry, install a pack, or fetch a
third-party resource/template.

Coverage includes stable-ID uniqueness; no service and unsupported IPC; absent
modules; a genuinely partial resource module; missing/empty resources even when
the service would return a diagnostic; null/empty/unterminated/unexpected-pointer
service strings; supported legacy mappings and independent-module priority;
interface release; copied-string lifetime; cache reset; cross-thread lookup;
and caption/static/button localization with editable/read-only fields,
selection, nested controls, IDs, styles, and geometry left intact.

The standalone Win32 MSVC test build and its mock/resource/UI tests were run.
No real Winamp language-pack installation or live pack-switching behavior has
been verified; existing packs normally translate only the two compatible field
labels until the independent module is supplied.

## Public ABI references

Only interface signatures, GUIDs, dispatch numbers, and resource identifiers were
used from these public SDK headers:

- [Wasabi dispatch ABI](https://github.com/alexfreud/winamp/blob/community/Src/Wasabi/bfc/dispatch.h)
- [Service-manager API](https://github.com/alexfreud/winamp/blob/community/Src/Wasabi/api/service/api_service.h)
- [Service-factory API](https://github.com/alexfreud/winamp/blob/community/Src/Wasabi/api/service/waservicefactory.h)
- [Language-service API](https://github.com/alexfreud/winamp/blob/community/Src/Agave/Language/api_language.h)
- [Language GUID/resource constants](https://github.com/alexfreud/winamp/blob/community/Src/Agave/Language/lang.h)
- [Historical public string IDs](https://github.com/alexfreud/winamp/blob/community/Src/Plugins/Input/in_vorbis/resource.h)
