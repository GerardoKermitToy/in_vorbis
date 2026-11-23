# Independent Winamp Ogg Input 1.2.0

This package contains 32-bit Winamp input plugins:

- `Plugins/in_vorbis.dll` — independent GPLv3 Ogg Vorbis, Opus, FLAC and Speex input.
- `Plugins/in_opus.dll` — the sibling GPLv3 Opus input, built alongside it.

The independent `in_vorbis` handles Opus in `.ogg`/`.oga`, including the original
RadioSEGA URL without an extension workaround. The sibling retains `.opus`/`.opu`.
The plugins require the Microsoft Visual C++ v14 **x86** runtime. Debug DLLs are
not included.

## Documentation

- [Ogg input features, settings and build instructions](ogg-input/README.md)
- [Exact coverage and remaining differences](ogg-input/FEATURE_PARITY.md)
- [Build, test and binary verification](ogg-input/VERIFICATION.md)
- [Advanced FLAC/Opus decoding and exact limits](ogg-input/docs/ADVANCED_DECODING.md)
- [Language-pack integration and fallback behavior](ogg-input/docs/LANGUAGE_PACKS.md)
- [English language resource template](ogg-input/localization/en-US.rc)
- [Sibling Opus input documentation](opus-input/README.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md) and component texts in `licenses/`

## Installation is manual and explicit

Close Winamp completely and back up an existing `Plugins/in_vorbis.dll` **before**
replacing it. Renaming this independent DLL does not replace the original
preloaded plugin's Ogg URL routing. Do not keep two enabled copies of the same
input module. This archive does not run an installer or change Winamp settings.

The implementation is independent; no WCL Winamp source or dialog resources are
included. Full behavioral or bit-exact historical parity is not claimed. Host
settings and language services have mock-host coverage, but were not installed
or verified in the user's Winamp. Missing language strings retain English text.
