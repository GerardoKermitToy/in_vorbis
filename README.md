# Independent Winamp Ogg Input

A new C++17 input plugin for **32-bit Winamp**, licensed under **GPL-3.0-only**.
It is developed separately from `in_opus` and can be built alongside it.
No source, resource scripts, artwork, or logos from the WCL-licensed Winamp
repository are included.

## Playback

- Content-based decoding of **Ogg Vorbis, Ogg Opus, Ogg FLAC and Ogg Speex**.
- Registers `.ogg`, `.oga` and `.spx`. The sibling `in_opus` retains `.opus` and
  `.opu`; an Ogg Opus file need not be renamed to play through this plugin.
- Local Unicode paths, seeking, pause, output-plugin volume/pan, DSP and
  visualization callbacks. Historical RIFF/WAVE-wrapped Ogg is readable.
- Source/Windows or unmapped codec channel order, stereo/mono/quad and DS/DS2
  surround-matrix downmix, 8/16/24/32-bit integer PCM. Rectangular, triangular
  and Gaussian dither; no, first-order, analytic ATH and low-amplitude ATH
  noise shaping. Selectable SpeexDSP resampling quality.
- Chained-stream metadata, ReplayGain/R128, track/album selection, preferred-tag
  fallback, peak modes, separate radio and untagged-file preamps.
- Extended-read exports support integer PCM, IEEE float PCM, seeking and the
  host kill switch.
- An independently implemented Wasabi service supplies `A_VORBIS` decoding to a
  compatible host Matroska container plugin. Its ABI and packet handling are
  mock-tested; playback through an installed Winamp `in_mkv` is not yet verified.

Playback follows chained source formats by default: after draining the old
output, it reopens the output plugin when the effective sample rate or channel
count changes. DSP/visualization formats are updated and the media clock remains
cumulative. Rejected multichannel output falls back to stereo without dropping
the retained first block. A fixed output rate or channel mode is still honored;
`Follow chained source formats` can be disabled for a stable-format session.
Extended-read APIs always keep the format advertised at open, resampling and
using stereo fallback where required by that immutable ABI contract.

ATH filters are independently derived from an analytic hearing-threshold model,
not copied historical coefficient tables. ATH supports 22050/32000/37800/44100/
48000 Hz; low-amplitude ATH supports 44100/48000 Hz. Other rates fall back to
first-order shaping. DS/DS2 are legacy amplitude matrices, not certified Dolby
encoders; their unnormalized sums and quad mixing may clip without preamp
headroom. No bit-exact historical processor equivalence is claimed.

## Advanced decoding (1.2.0)

- **Exact integer FLAC:** unprocessed 32-bit integer output can preserve every
  original FLAC bit without a float32 round-trip. Requires dither off, unity
  effective gain, unchanged rate and an identity channel layout. DSP/output and
  Windows mixer processing remain outside this input-buffer guarantee.
- **Opus HD/QEXT:** actual packet extensions select native 96 kHz decoding when
  enabled; normal Opus stays at 48 kHz. An informational input-rate hint is not
  treated as evidence of HD. Explicit output-rate conversion still applies.
- **Optional Opus OSCE/BWE:** speech enhancement and synthetic bandwidth extension
  use embedded, checksum-pinned upstream neural tables. Both are off by default;
  BWE applies only to eligible 48 kHz wideband speech, not native 96 kHz HD.
- **Opus Ambisonics families 2/3:** ACN/SN3D and projection streams render to
  stereo, analytic approximate binaural headphones, quad, Windows 5.1 or 7.1.
  The headphone renderer is not measured HRTF; surround LFE stays silent.

See [advanced decoding](docs/ADVANCED_DECODING.md) for exact-mode requirements,
spatial-rendering limits, seek/index costs, forward-only QEXT detection and
strict damaged-input handling. These options are on the Processing settings page.

## Radio and HTTP files

HTTP(S) uses Windows WinHTTP, with certificate validation, bounded probing and
buffering, cancellation, redirects, ICY metadata stripping, proxy policies,
optional transient Basic/Digest authentication, reconnect attempts and stream
recording. Finite HTTP files may be seekable when enabled and when the server
provides a validated byte-range response; live radio is not seekable.

This URL is routed directly to the independent `in_vorbis.dll`, without an
artificial extension suffix:

```text
http://icecast.radiosega.net:8006/rs-opus.ogg
```

Installing a differently named extra DLL does not replace Winamp's original
preloaded `in_vorbis.dll`. The independent DLL must take its place after an
explicit backup; the installer script below makes that backup. Installation is
not performed by the build or tests.

Credentials are never saved in plugin settings or recording names. Credentials
embedded in URLs are rejected. Recording stores incoming audio bytes without
ICY metadata; choose a writable destination directory explicitly.

## Windows and metadata

The classic, native Windows configuration and file-information dialogs use
system controls and fonts. They provide decoding, ReplayGain, streaming,
advanced streaming and processing pages, standard and custom text tags, logical-stream
navigation and technical stream information. The unified information pane is
read-only and can open the full information dialog.

Saving local Vorbis/Opus tags uses a same-directory temporary file and atomic
replacement. It preserves encoded audio and unrelated comments, and supports a
selected chained link or all editable links through the metadata API. Empty
values delete fields. Unsafe or damaged layouts are rejected without modifying
the source. Embedded artwork is preserved, not edited. FLAC/Speex, radio,
RIFF wrappers, hard-link aliases and reparse-point leafs are read-only in the tag
writer. See [FEATURE_PARITY.md](FEATURE_PARITY.md) for exact scope and remaining
compatibility differences.

Settings are per-user under `HKCU\Software\WinampOgg\Input`; tests redirect
registry access into a disposable sandbox. `Use Winamp ReplayGain settings` and
`Use Winamp proxy settings` are enabled by default. At playback startup the
plugin reads the host's playback/internet configuration service: RG enable,
track/album source, preamps, preferred-tag policy, all four peak modes, proxy
endpoint and the port-80-only policy. Missing services/items retain local
fallbacks; host-disabled RG preserves the mandatory Opus header gain. Local and
host preferences are never overwritten by the effective snapshot. Host proxy
credentials are rejected rather than persisted. Changes take effect on the next
playback start; turn off either inheritance switch to use local settings.

Winamp language support is optional and text-only. An independent module GUID
and stable string IDs allow a matching language pack to translate labels,
combo choices and messages without replacing our dialog templates. Missing or
empty translations fall back individually to the built-in English text; user
tags, paths and editable controls are never translated. Existing historical
packs can supply the compatible Name/Value column labels; they generally lack
this independent module's new strings. See [language-pack documentation](docs/LANGUAGE_PACKS.md)
and the [English resource template](localization/en-US.rc). Real installed-host
localization is not yet verified.

## Build and test

Requirements: Visual Studio 2022 C++ Build Tools, a Windows SDK, CMake 3.24+
(CMake presets require 3.25+), and the sibling GPL `in_opus` source directory.
The default layout is:

```text
C:\winamp       in_opus source
C:\in_vorbis    this project
```

From this directory:

```powershell
.\tools\Build.ps1 -Configuration Release -Test -Package
```

Debug verification:

```powershell
.\tools\Build.ps1 -Configuration Debug -Test
```

The Release package is written to `dist`; the two DLLs are in `dist\Plugins`.
Source-built codec libraries are linked statically, but the Microsoft CRT is
**dynamic**: `/MD` for Release and `/MDd` for Debug. Release uses IPO/LTO when
supported. Distribution requires the Microsoft Visual C++ x86 runtime; Debug
builds are not for distribution.

Alternatively, the sibling `in_opus` project's
`WINAMP_OPUS_BUILD_OGG_INPUT=ON` option builds this project in its own build tree.
`WINAMP_OPUS_OGG_INPUT_SOURCE` selects a non-default sibling location.

Dependencies are pinned by revision and SHA-256. The ignored `third_party` and
build directories contain downloaded sources; they are not part of this
repository. The initial build requires network access unless archives are
already cached. No user music files are copied into the repository.

## Explicit installation

Close Winamp before running the installer. The existing plugin is backed up
before replacement; source, backup and installed-file hashes are checked.

```powershell
.\tools\Install-Plugin.ps1 -WinampDirectory 'C:\Program Files (x86)\Winamp' -WhatIf
```

Remove `-WhatIf` only when ready to replace the original DLL. An elevated shell
may be needed for a protected installation directory. Building and testing do
not change an installed Winamp.

## Licensing

This implementation is GPL-3.0-only. The input ABI declarations and channel
reordering helpers originate from the sibling GPL `in_opus` project. Codec
libraries retain their own permissive licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The historical Winamp repository was a behavioral reference, not a source
supplier. WCL 1.0.1 restrictions on distributing modified Winamp code do not
grant permission to relicense that code; none is included here.
