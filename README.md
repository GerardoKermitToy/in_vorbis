# Independent Winamp Ogg Input

A **GPL-3.0-only** input plugin for **32-bit Winamp**.

## Features

- **Ogg Vorbis, Ogg Opus, Ogg FLAC and Ogg Speex**, detected by content.
  Registers `.ogg`, `.oga` and `.spx`; the separate `in_opus` plugin handles
  `.opus` and `.opu`.
- Unicode paths and tags, seeking, pause, DSP and visualization support.
- Source channels, stereo/mono/quad and DS/DS2 downmix; 8/16/24/32-bit integer
  PCM, resampling, configurable dither and noise shaping.
- ReplayGain/R128 with track/album selection, preamps and tagged-peak policies.
- HTTP/HTTPS streaming with ICY titles, proxy/authentication settings, optional
  recording and bounded reconnect. Live radio is not seekable; finite HTTP files
  require server support for validated byte ranges.
- Local Vorbis/Opus tag editing in Winamp's **File Info → Basic Info / Advanced**
  dialog, without reencoding audio. Advanced provides Name/Value editing,
  Add New, Delete and Delete All; OK saves and Cancel discards drafts. Repeated
  fields, multiline values, other comments and embedded artwork are preserved;
  artwork editing by this plugin is not supported. All standard Basic Info
  fields remain editable when unset. Format Info retains the historical
  technical-data layout, adds known codec details and supports scrolling.
- Integer and IEEE-float extended-read PCM APIs with seeking and cancellation.
- Optional Opus HD/QEXT, OSCE/BWE and Ambisonics rendering; exact integer FLAC
  output under the conditions described in [advanced decoding](docs/ADVANCED_DECODING.md).
- Native settings dialogs and optional [language packs](docs/LANGUAGE_PACKS.md).

Configure the plugin under **Preferences → Plug-ins → Input**. Winamp ReplayGain
and proxy settings are inherited by default; disable the corresponding switches
for local control. Changes take effect on the next playback start.

Playback follows sample-rate/channel changes in chained streams by default.
Disable **Follow chained source formats** for a stable output format.

## Installation

Requires 32-bit Winamp and the **Microsoft Visual C++ v14 Redistributable (x86)**.
Use Release DLLs, not Debug builds.

Close Winamp and back up `Plugins\in_vorbis.dll` before replacing it. Keep the
filename `in_vorbis.dll`: a differently named copy does not replace Winamp's
preloaded Ogg input. Do not keep two enabled copies of the same module.

The installer script creates and verifies a backup. Preview the operation first:

```powershell
.\tools\Install-Plugin.ps1 -WinampDirectory 'C:\Program Files (x86)\Winamp' -WhatIf
```

Remove `-WhatIf` to install. A protected directory may require an elevated shell.
Building and testing do not modify an installed Winamp.

## Build and test

Use Visual Studio 2022 C++ Build Tools, a Windows SDK and CMake 3.24+.
The build also requires the sibling GPL `in_opus` sources, located at `../winamp`
by default. Set `OGG_INPUT_OPUS_SOURCE` in CMake for another location.

Run from this repository:

```powershell
.\tools\Build.ps1 -Configuration Release -Test -Package
```

The package is written to `dist`, with the DLLs in `dist\Plugins`.
The first build downloads pinned dependencies unless they are already cached.

To run Debug tests:

```powershell
.\tools\Build.ps1 -Configuration Debug -Test
```

## Documentation

- [Feature coverage and limitations](FEATURE_PARITY.md)
- [Advanced decoding](docs/ADVANCED_DECODING.md)
- [Language packs](docs/LANGUAGE_PACKS.md)
- [Build and test verification](VERIFICATION.md)

## License

[GPL-3.0-only](LICENSE). Third-party libraries retain their own licenses;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
