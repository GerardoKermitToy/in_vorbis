# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project scope and build relationship

This is an independent GPL-3.0-only `in_vorbis.dll` for 32-bit Winamp, not a fork
of the WCL-licensed Winamp implementation. It detects Ogg Vorbis, Opus, FLAC and
Speex by content and registers `.ogg`, `.oga` and `.spx`. The sibling `in_opus`
plugin handles `.opus`/`.opu`. Native `.flac`, WebM and MP4 are not added by the
Ogg readers. The optional Matroska service claims `A_VORBIS` only, not Matroska
Opus/FLAC/Speex or the `.mkv`/`.mka` extensions.

The sibling GPL `in_opus` source tree is required at `../winamp` by default;
`OGG_INPUT_OPUS_SOURCE` selects another location. `cmake/Dependencies.cmake`
reuses its pinned Ogg/Opus/libopusfile/SpeexDSP targets and adds pinned
Vorbis/FLAC/Speex sources. `OGG_INPUT_BUILD_OPUS=OFF` skips the sibling DLL, not
the need for its dependency sources. Conversely, this repository can be added
by an `in_opus` build with `WINAMP_OPUS_BUILD_OGG_INPUT=ON`. The repositories have
separate Git histories and plugin state.

## Build and verification commands

Run commands in PowerShell from this repository's root. Use Windows, MSVC/Visual
Studio 2022 C++ Build Tools, a Windows SDK and **Win32**, not x64. The source
minimum is CMake 3.24; the version-6 presets below require CMake 3.25+.
Dependencies download on first configure unless their pinned archives are cached.

Configure, build and run the Release suite:

```powershell
cmake --preset vs2022-win32
```

```powershell
cmake --build --preset release
```

```powershell
ctest --preset release
```

Debug uses the same configured tree:

```powershell
cmake --build --preset debug
```

```powershell
ctest --preset debug
```

Build and run just the DLL-level notification regression:

```powershell
cmake --build --preset release --target ogg_notification_tests
```

```powershell
ctest --preset release -R '^ogg_notifications$' --output-on-failure
```

CTest names differ from executable target names. Other useful CTest names are
`ogg_audio`, `ogg_chains`, `ogg_flac_exact`, `ogg_opus_advanced`, `ogg_network`,
`ogg_transport` and `ogg_matroska`. CTest supplies required DLL and fixture paths;
prefer it to invoking test executables without their arguments. The standalone
build disables sibling Opus tests; a combined build launched from `in_opus` can
run both projects' suites. Do not assume their test counts are identical.

The existing wrapper combines Release build, tests and package staging:

```powershell
.\tools\Build.ps1 -Configuration Release -Test -Package
```

DLLs are built under `build/vs2022-win32/bin/Release` and staged in `dist/Plugins`.
Package staging does **not** install into Winamp. Actual installation is separate
via `tools/Install-Plugin.ps1`: Winamp must be closed and the existing
`in_vorbis.dll` backed up. Renaming an extra copy does not replace the preloaded
plugin's routing. Distribute Release only. Codec libraries are static but use
the same dynamic CRT as the DLL/test hosts: `/MD` in Release, `/MDd` in Debug.
Preserve Release IPO/LTO and the `ogg_dynamic_crt` regression.

There is no separate configured lint/format target. C++ builds use `/W4`,
`/permissive-` and `/utf-8`; check patch whitespace with:

```powershell
git diff --check
```

## Architecture

- **Host boundary:** [src/plugin.cpp](src/plugin.cpp) implements playback, the
  worker/control events, output negotiation, DSP/visualization and metadata
  exports. `src/winamp_input.hpp` and `src/in_vorbis.def` define the ABI surface.
  `in_vorbis` is the DLL target; `ogg_audio` is the static media-processing core.
- **Transport and readers:** `ByteStream` in `src/stream.*` handles local files,
  bounded probe replay, HTTP/HTTPS, ICY and recording. `src/probe.cpp` identifies
  the codec. `CodecReader` in `src/backend.hpp` unifies per-codec readers and
  describes the block just returned, never a block from the next logical link.
  Opus uses `src/opus_native.*` even for ordinary streams, unlike the sibling's
  optional libopusfile path. `src/ambisonics.*` renders Opus spatial streams.
- **Audio pipeline:** `Audio` in `src/audio.*` owns transport/reader selection,
  metadata revisions, per-link ReplayGain/R128, channel order and mixing. `Pcm`
  in the same files owns resampling, quantization and chained-format handoff;
  `src/processing.*` supplies matrices and quantizer state. FLAC additionally
  exposes an exact signed-integer companion to the float blocks.
- **Other consumers:** `src/extended_read.cpp` creates independent `Audio`/`Pcm`
  sessions with an immutable advertised output format. `src/matroska.*` is a
  separate Wasabi Vorbis packet-decoder service, not a container parser or the
  normal playback pipeline.
- **Metadata/UI:** `src/tag_writer.*` validates and atomically replaces editable
  local Vorbis/Opus comments. `src/ui.*` and `src/dialogs.rc` own native dialogs;
  `src/settings.*` defines validation and registry persistence.
  `src/host_settings.*` resolves read-only startup snapshots and
  `src/language.*` provides independent text-only localization.

## Behavior that changes must preserve

- Chained playback follows source rate/channels by default (`native_chains`).
  `Pcm::FormatChange` appears only after old-format PCM/tails are delivered; the
  first new-format block is retained until output drains/reopens and
  `AcceptFormat()` succeeds. Preserve the cumulative clock and retained block on
  stereo fallback. Explicit rate/channel choices still apply; extended-read
  sessions never renegotiate the format promised by their open call.
- Full `SetInfo(kbps, rate_kHz, channels, 1)` belongs at initial startup. Later
  bitrate changes use `SetInfo(kbps, -1, -1, 1)`; real output-format changes use
  `SetInfo(-1, rate_kHz, channels, 1)` and report changed bitrate separately.
  Combining bitrate and rate causes Winamp title refreshes. Title IPC
  (`WM_USER`, lParam `243`) is only for actual title/tag changes, including fields
  used by ATF, not seeks, link numbers, recording errors or unchanged tags.
  Metadata snapshots follow revisions; paused/notified EOF workers wait on
  control/stop events rather than polling output.
- Exact FLAC output relies on the reader's signed, left-aligned integer PCM,
  never values reconstructed from float32. Preserve it only for eligible 32-bit,
  dither-off, unity-gain, unchanged-rate, identity-layout output. DSP/output and
  Windows mixer processing are outside this input-buffer guarantee.
- OpusHead gain must be applied exactly once. Actual QEXT, not an input-rate
  hint, determines 96 kHz decoding. Ambisonics is rendered before speaker mixing;
  raw channel order is not a way to expose ACN coefficients as Windows speakers.
- Only safe local Vorbis/Opus tag layouts are writable. Preserve encoded packets,
  unrelated comments and artwork; FLAC/Speex, radio, RIFF wrappers and unsafe
  source aliases remain read-only in the writer. Do not broaden this based only
  on what the playback decoders can read.
- Resolve host ReplayGain/proxy snapshots at startup without modifying host or
  local preferences. Localization uses an independent module and stable,
  append-only IDs in `src/language.cpp`, with `localization/en-US.rc` as the
  template. Translate fixed strings, not tags/paths; do not replace dialog
  templates with historical or language-pack resources.
- Keep bounded/cancellable transport probing and reads, TLS verification,
  redirect/auth isolation and non-persistence of credentials. Live radio is
  forward-only; seekable finite HTTP input needs validated Range behavior.

Tests generate codec fixtures, serve loopback HTTP and load the real DLL into
mock hosts. `tests/registry_sandbox.hpp` redirects settings tests away from the
user's registry. Such tests do not prove installed-host Wasabi, language-pack,
Matroska or audio-hardware integration.

[FEATURE_PARITY.md](FEATURE_PARITY.md) defines feature boundaries;
[docs/ADVANCED_DECODING.md](docs/ADVANCED_DECODING.md) covers decoder limits and
[docs/LANGUAGE_PACKS.md](docs/LANGUAGE_PACKS.md) describes localization.
[VERIFICATION.md](VERIFICATION.md) is the record of actual past verification.
