# Verification — 1.2.0 — 2026-11-23

## Final build and regression results

- Independent `C:\in_vorbis` build: **14/14 Release**, **14/14 Debug** tests passed.
- Combined `C:\winamp` build: **25/25 Release**, **25/25 Debug** tests passed.
- MSVC/Visual Studio 2022, Win32/x86, source-built codecs, dynamic Microsoft CRT
  (`/MD`, `/MDd`), Release `/O2` and IPO/LTO. Final Release binaries do not rely
  on the temporary `/Zi`/`/DEBUG` diagnostic configuration used during debugging.
- Final C/C++ build logs contain no compiler warnings/errors. The upstream
  libogg CMake deprecation warning remains.
- Native dialog captures were visually inspected. All five settings pages,
  bounds, tag save/delete, new codec settings, host-inheritance switches and
  settings OK/Cancel are tested. The append-only language catalogue has 170 IDs.
- RadioSEGA's original `http://icecast.radiosega.net:8006/rs-opus.ogg` URL played
  through the new build-tree DLL without a suffix: **50112 output frames** in
  the recorded run. Live playback is non-seekable. Packaged DLLs are checked
  byte-for-byte against these Release outputs before archive creation.

## New exact FLAC coverage

`ogg_flac_exact` generates 16/24/32-bit Ogg FLAC at 44100 and 96000 Hz in mono,
stereo and six channels. It compares exact signed 32-bit output, including
INT32_MIN/MAX, adjacent endpoints and low-bit pseudo-random values. Tests cover
varying read sizes, output canaries, short-file seeks, rate/layout-changing
chains, seeks into a later link, processing/dither bypass prevention and the
DLL's integer extended-read export. The short-file seek fallback preserves the
first exact block instead of relying solely on libFLAC interpolation seeking.

Exactness requires the documented unprocessed 32-bit mode. Float output and
external Winamp DSP/output/hardware processing are not included in that claim.

## New Opus coverage

`ogg_opus_advanced` uses generated fixtures, not external copyrighted media:

- Actual QEXT extension detection and native 96000-Hz decoder creation; ordinary
  Opus and a misleading input-rate hint remain 48000 Hz.
- A 30000-Hz input tone survives native HD decoding with measured amplitude
  **0.219999** in the reference generated fixture. This distinguishes native HD
  from a 48000-Hz decode followed by upsampling.
- Correct 48000-Hz granule/pre-skip accounting, EOS trimming, grouped audio pages,
  offset granules, late-QEXT discovery in local files and mixed 48/96-kHz chains.
- Optional speech enhancement and BWE change eligible speech while preserving
  frame counts. Recorded mean absolute differences versus ordinary decoding:
  **0.0356574 OSCE**, **0.000729129 BWE**. These demonstrate activation, not a
  subjective-quality score or recovery of the original high frequencies.
- Ambisonics mapping families 2/3, complete ACN/SN3D order validation, optional
  non-diegetic stereo, stereo/analytic-headphone/quad/5.1/7.1 output, silent LFE,
  impulse direction and deterministic decoder/binaural history after seeking.
- Canonical projection API plus rectangular C=4/K=2 and C=4/K=6 demixing fixtures
  routing the final coded column, so unused columns are not silently dropped.
- Guarded output, finite PCM, seeks at/past EOF, mixed-family chains, malformed
  headers/matrices/packet envelopes, CRC/truncation rejection and bounded parsing.
- Real loopback forward-only HTTP tests cover ordinary/HD startup, initial and
  consumed/reset live bitrate, explicit late-QEXT failure, initial Icecast page
  sequence jumps and continued-fragment recovery. Local or later sequence damage
  still fails. Playback has no session-wide page/packet lifetime cap.

The upstream neural table archive is SHA-256 pinned and compiled into libopus;
no runtime model download or Python/PyTorch execution is used.

## Regressions retained

The existing suites retain Vorbis/Opus/FLAC/Speex sample/seek/resampling checks,
transactional Vorbis/Opus tags, Unicode/RIFF handling, gain/R128 policies, all
integer PCM formats, chained-output drain/reopen/stereo fallback, DSP/visualization
negotiation, HTTP/ICY/range/reconnect/recording/cancellation and dialog tests.
Host configuration and language services have resource-backed mock ABI tests.
Matroska A_VORBIS dispatch remains enabled and tested, not skipped.

During development, an early Release run crashed. New tests also identified an
invalid pending-PCM cursor after a seek beyond the end of a chain; the cursor
invariant was fixed and regression-tested. Final default-optimized binaries pass
the listed runs. Test processes suppress blocking Windows Error Reporting UI
only; failing exit codes remain visible to CTest. No tests are disabled to make
the final suite pass.

## Release package

The versioned staging directory is `dist/package-1.2.0`, archived as
`dist/winamp-ogg-1.2.0-win32.zip`. Both input DLLs are byte-identical to the tested
independent Release outputs. DLL integration/extended-read and native-dialog
checks also passed when run directly against the staged `in_vorbis.dll`.
The package includes GPL text, all seven component license texts,
feature/verification notes, advanced-decoding and language-pack documentation,
the English resource template and separate sibling Opus documentation/notices.
Root and sibling notices have distinct destinations to prevent timestamp-based
CMake installs from retaining the wrong component's notices. Older release
archives are retained. `SHA256SUMS.txt` records both DLL hashes; the archive's
checksum is in the adjacent `.zip.sha256` file.

Inspection confirmed:

- `in_vorbis.dll` version 1.2.0, PE32, machine `0x14c` (x86), ASLR and NX.
- 14 expected named Winamp input/metadata/extended-read exports.
- Dynamic MSVCP140, VCRUNTIME140 and UCRT imports; no Debug CRT imports.
- The packaged sibling Opus DLL is also PE32/x86 with ASLR/NX and Release CRT.

SHA-256:

```text
5a11d0d7d5d72f2811dc03144173a033ebed4322242b4efee48b5599799dfa77  Plugins/in_vorbis.dll
59a5fc6248ba37f0a38b7138a76be90c684583a223a8dd7aee1aa172637d259e  Plugins/in_opus.dll
```

## Not verified / not performed

No plugin or language pack was installed into the user's Winamp, no installed
Winamp DLL was replaced, and no commit, push or public release was made.
Actual Winamp configuration/language service integration, Matroska through an
installed in_mkv, and real output-plugin/hardware 96-kHz compatibility remain
unverified. A complete TLS/proxy/authentication matrix and large external
malformed-media corpus have not been run. Existing language packs generally
lack the independent module; long translations can clip.

The binaural renderer is an analytic approximation, not measured/personalized
HRTF or head tracking. Advanced Opus currently scans local links and seeks from
BOS, which can be slow for long files. Forward-only native HD must be detected
on the initial audio page. These additions do not add native .flac, MP4/WebM or
Matroska Opus support. See [docs/ADVANCED_DECODING.md](docs/ADVANCED_DECODING.md)
and [FEATURE_PARITY.md](FEATURE_PARITY.md) for the exact limits.
