# Third-party notices

Original project sources are GPL-3.0-only. No Winamp WCL implementation or artwork
is incorporated. The local Winamp ABI declarations and channel ordering helpers
are reused from our GPL-3.0-only `in_opus` project.

All audio libraries are source-built as static archives with the DLL CRT.

| Library | Version/revision | License |
|---|---|---|
| libogg | 1.3.6 | BSD-style |
| libopus | 1.6.1 | BSD-style, upstream patent grant |
| libopusfile | 0.12 | BSD-style |
| SpeexDSP resampler | 1.2.1 | BSD-style |
| libvorbis | 1.3.7 / 0657aee69dec8508a0011f47f3b69d7538e9d262 | BSD-style |
| libFLAC | 1.5.0 / 1507800de4b70e21be71f38caa0d9079d0bc6e45 | BSD-style (library only) |
| libspeex | 1.2.1 / 5dceaaf3e23ee7fd17c80cb5f02a838fd6c18e01 | BSD-style |

Pinned archives and SHA-256 values are in `cmake/Dependencies.cmake` and the sibling
`in_opus` dependency file. Upstream license texts are installed to `dist/licenses`.
Windows WinHTTP handles HTTP/TLS with certificate verification; no TLS library is
bundled. Winamp is a trademark of its respective owner. This is an independent
plugin, not an official Winamp release.

## Optional Opus decoder enhancements

The source-built libopus includes QEXT (Opus HD), OSCE and OSCE bandwidth
extension. Speech enhancement is off by default in the independent Ogg input.
OSCE pulls the upstream shared neural/PLC support into the library; the plugin
does not claim DRED or automatic recovery of missing HTTP audio.

The upstream neural data archive is checksum-pinned separately:

- `https://media.xiph.org/opus/models/opus_data-a5177ec6fb7d15058e99e57029746100121f68e4890b1467d4094aa336b6013e.tar.gz`
- SHA-256: `a5177ec6fb7d15058e99e57029746100121f68e4890b1467d4094aa336b6013e`.

Only the generated C/header weight tables are compiled. No training scripts,
Python/PyTorch objects or external model files are loaded at playback time. The
libopus upstream license and notices accompany these compiled data tables.
