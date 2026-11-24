# Verification — 1.2.1 — 2026-11-24

Version 1.2.1 includes the local Speex seek-bar correction and the subsequent
local FLAC/all-codec seeking checks recorded below.
Earlier 1.2.0 runs, package names and checksums are retained as historical records.

## Final build and regression results

- The earlier independent periodic playlist fix passed **15/15 Release** and
  **15/15 Debug** tests (21.43 s / 36.20 s). The later seek cleanup, expanded
  notification scenarios and separately staged DLLs are recorded below.
- Combined `C:\winamp` build with both notification regressions: **30/30 Release**,
  **30/30 Debug** tests passed.
- The original 1.2.0 baseline had 14/14 independent and 25/25 combined tests; its
  archive and DLL hashes remain recorded in the baseline package section.
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

## Original 1.2.0 release package

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

## Playlist refresh correction

The playback loop previously sent bitrate, sample rate and channel count to
Winamp's `SetInfo` callback every 250 ms, even when the displayed bitrate did not
change. Supplying bitrate and sample rate together requests a title refresh in
Winamp. Periodic updates now send only changed displayed kbps using
`SetInfo(kbps, -1, -1, 1)`. Full-format updates are retained at playback startup
and after an actual native output-format change; each reopen resets the cached
displayed bitrate and the update timer. Real chained title notifications remain
independent and enabled.

The first standalone Release verification also encountered a stack exception
inside the fixture encoder before DLL loading. Regenerating stale MSVC incremental
LTO `.iobj`/`.ipdb` artifacts and relinking resolved the isolated network test.
No decoder changes, increased test stack, disabled checks or network-test source
changes were retained for that build-artifact issue.

The new `ogg_notifications` test loads the actual DLL into an isolated host,
paces output across several update intervals, and records every format/bitrate
callback and title IPC. It checks Opus, Vorbis, changing-bitrate Opus chains,
44.1-kHz six-channel to 48-kHz stereo Vorbis boundaries, and disabled instantaneous
bitrate. Repeated format/title refreshes were reproduced before the fix. Existing
shared codec fixture generators and network-test source remain unchanged.

The original versioned package and archive remain the historical 1.2.0 baseline.
The first periodic-update correction remains staged at
`dist/package-1.2.0-playlist-fix`; those DLLs and their historical hashes are kept
unchanged. The later seek correction uses the separate package recorded below.
SHA-256 of the earlier periodic-update correction:

```text
7583b94754e9ab20bac0294ad8c4a5f1ae10ed37ae917ee89608b9b600d4e9ea  Plugins/in_vorbis.dll
8905c41dcae2f8916a2fa1942a6c3495f7b1cadbbecaaccb91af32c4f4ab79ae  Plugins/in_opus.dll
```

Actual playlist appearance in an installed Winamp still needs a manual check.
No installed DLL is replaced by these tests.

## Seek notifications and playback work

The earlier 250-ms fix was not enough: the Opus sibling still published a new
metadata revision on every seek. In this Ogg plugin a link-number change or
recording error also unconditionally requested a title refresh. The new DLL-level
tests reproduced **three unwanted title IPCs** while seeking between links with
unchanged tags. The community Winamp
[`in_flac` seek path](https://github.com/manfromafar/winamp/blob/community/Src/Plugins/Input/in_flac/DecodeThread.cpp)
was compared: it seeks/flushes without title IPC. A separate test reproduced Winamp's implicit title-refresh trigger
when native rate/channel reopening resubmitted bitrate and sample rate together.

The Ogg playback host now consumes Audio metadata revisions, copies the snapshot
only on changes, and notifies the playlist only if the display title or actual
tags differ. Album-only changes still notify Winamp for custom title formatting;
link/format changes and recording errors alone do not. Native format notifications
use `SetInfo(-1, rate, channels, 1)` and send a changed kbps separately, so even an
actual output reopen does not implicitly request a title refresh.

Same-link seek no longer invalidates parsed ReplayGain/R128. Visualization
padding/conversion runs only for an installed callback. Pause and notified EOF
wait on stop/control events; seek/resume/stop wake them without periodic output
polling. Decoder validation, exact native history/preroll, real metadata updates,
output draining and necessary format negotiation remain intact.

Expanded `ogg_notifications` covers Opus and Vorbis repeated forward/backward
seeks, coalesced seek bursts, identical tags, real title changes, album-only ATF
changes, fixed output and native 44.1/48-kHz reopening, paused seek/resume,
EOF seek/restart, no post-EOF polling and stop wakeup. Existing fixture generators,
codec accuracy checks and network tests remain enabled.

## Final seek-cleanup verification

- Combined Winamp build: **30/30 Release** (105.75 s),
  **30/30 Debug** (123.83 s).
- Independent Ogg build: **15/15 Release** (43.77 s),
  **15/15 Debug** (50.80 s).
- Compiler logs have no C/C++ warnings/errors; dynamic-CRT, decoder, DLL,
  resampler, surround, exact FLAC, radio, dialogs and notification checks pass.
- On both DLL paths, repeated same-link seeks and identical-tag chains produce
  **zero title IPCs**. Actual title and album-only tag changes still notify the
  host. Native rate/channel reopening never combines a new rate with bitrate.
- Pause/seek/resume, EOF seek/restart, no output polling after EOF and stop wakeup
  pass. No installed Winamp was changed; real playlist appearance was not tested.

The final package is `dist/package-1.2.0-seek-fix`. Previous packages and
archives are retained. Both staged DLLs are byte-compared with this repository's
final Release build. SHA-256 (build-specific; separate build trees may differ):

```text
3a248a2d675bad85ef571cf217150ec85673ed9abeffa38076b2f5454d8b60af  Plugins/in_opus.dll
e52acf8062c7eb49381e4eeac05f9a37197047cfa66039b6455d5de7d667c6b0  Plugins/in_vorbis.dll
```

## Local Speex seek-bar correction — 1.2.1 — 2026-11-24

The final 1.2.1 Win32 builds passed **15/15 Release** (44.26 s) and
**15/15 Debug** (54.47 s) tests. Both DLLs were checked for FileVersion,
ProductVersion and plugin-description version **1.2.1**. The supplied Speex
sample also passed the standalone core and real-DLL checks in both builds.

Seekable local Speex input previously reported an unknown duration until EOS.
New core and DLL regressions reproduced `duration_ms == -1` before playback,
which prevents Winamp from exposing its seek bar even though seeking is enabled.

Local files now get a header/EOS-granule indexing pass without decoding PCM.
The complete link catalogue, durations and comments survive rewinds; reparsing
comments does not append duplicate tag values. Seeking past EOF succeeds with
no pending PCM, and a subsequent rewind still decodes the complete file.
HTTP startup retains its probe limits rather than downloading an entire file
for this index; live radio remains forward-only and finite HTTP byte/time seeking
still uses the transport's validated Range path.

Regression fixtures cover 8/16/32-kHz Speex, grouped audio pages, file/memory
transport, mixed-rate chains, repeated forward/backward seeks, EOF/rewind,
stable metadata, and the DLL's startup length, paused seek/flush and resume.
The core test's `--file FILE` mode and the DLL test's optional local-file argument
also exercised the supplied 32-kHz mono sample: **263589 ms** before playback,
with successful seeks to **197691 ms**, **65897 ms** and zero. These are mock-host
DLL checks, not an installed Winamp or audio-hardware check.

## Local FLAC duration and all-codec seek regressions — 2026-11-24

Final independent Win32 verification passed **15/15 Release** (45.75 s) and
**15/15 Debug** (64.72 s). Both supplied `.oga` and `.spx` files passed the
standalone core and real-DLL mock-host seek checks in both configurations.

The supplied `.oga` sample is 96-kHz Ogg FLAC with STREAMINFO total_samples
unset. Core and DLL tests reproduced an unknown startup duration (`-1 ms`).
Local FLAC now gets a cancellable frame-skip scan without PCM reconstruction,
using the EOS granules to retain complete per-link counts, rates and comments.
Duration is available before playback, including unknown-length headers and
mixed-rate chains, and the catalogue survives seeks and short-file fallbacks.

The shared local-file matrix covers Vorbis, Opus, FLAC and Speex through file
and memory transport, including RIFF/WAVE wrappers: startup duration, native
sample count, repeated forward/backward seeking, seek past EOF, rewind, and
stable duration/tags. Vorbis past-EOF seeking now clamps to PCM EOF. DLL checks
cover startup GetFileInfo/GetLength/is_seekable, paused seek/Flush and resume
for every codec, unknown-length FLAC and a mixed-format FLAC chain.

FLAC-specific coverage includes known/unknown STREAMINFO counts at 44.1/96 kHz,
mixed-rate/channel chains and repeated 32-bit exact-PCM seeks in both directions.
A new exact regression caught libFLAC reporting a successful short-chain seek
with PCM from the wrong link. Chained FLAC therefore uses cancellable,
time-based decode/discard with each link's native rate; ordinary single-link
FLAC retains the sample-seek path and short-file fallback. Long chained seeks
can be slower. Loopback tests use FLAC larger than a 64-KiB startup probe and
verify bounded live/finite HTTP opening, forward-only radio, validated Range
seeking and the remaining decoded sample counts.

The supplied FLAC sample reports **30000 ms** before playback and passed core
seeks to **22500 ms**, **7500 ms** and zero plus the real-DLL mock-host seek
checks. These are not installed Winamp or audio-hardware checks.

The refreshed `dist/Plugins/in_vorbis.dll` is byte-identical to the tested Release
output, retains FileVersion/ProductVersion **1.2.1**, and passed the supplied
FLAC and Speex seek checks directly from staging. Release and Debug build logs
contain no compiler warnings/errors. No installed Winamp files were changed.
SHA-256 of the refreshed Release DLL:

```text
82903fc8080ab1c1519b95cd63dba52be73ae0ed7c19cb3832eb12fca11a4b21  Plugins/in_vorbis.dll
```

## Local FLAC bitrate display correction — 2026-11-24

Final independent Win32 verification passed **15/15 Release** (56.56 s) and
**15/15 Debug** (76.17 s), including dynamic CRT, notification, exact-FLAC and
network regressions. Both supplied files passed core and DLL seek checks in both
configurations. Both rebuilt DLLs remain PE32/x86 with ASLR/NX; C/C++ build logs
contain no compiler warnings/errors.

The FLAC reader left LinkInfo.bitrate at zero, so Winamp's initial status used
the unknown-bitrate placeholder **1 kbps**. New core and DLL notification tests
reproduced the zero field and incorrect initial SetInfo value.

The existing local frame-skip/indexing pass now also counts compressed Ogg page
bytes per selected FLAC link, including its headers and comments. Per-link native
sample counts/rates provide the exact timebase for average bitrate. Counting is
bounded and occurs on the reads already needed for indexing: it adds no second
file scan, PCM decode, HTTP startup download or credential handling. RIFF wrapper
bytes and unrelated Ogg serials are not counted as FLAC data.

The average is available before the first PCM read, survives seeks and chain
rewinds, and is used as the stable FLAC fallback when instantaneous display is
enabled. It is not claimed to be a moving per-frame bitrate estimate. Existing
changed-kbps-only status notifications and title-refresh rules remain unchanged.

Core regressions compare the calculated bitrate against compressed bytes/native
duration for known/unknown STREAMINFO, file/memory input, RIFF wrappers and
mixed-rate links. Real-DLL notification tests verify the initial compressed kbps
in both average and instantaneous-display modes, with no repeated format/status
or playlist-title updates. The supplied 96-kHz FLAC reports **1652148 bps** and
the real-DLL mock host receives **1652 kbps**, while the **30000-ms** duration and
forward/backward/zero/EOF seeking remain intact.

The updated `dist/Plugins/in_vorbis.dll` is byte-identical to the tested Release
output and passed the supplied-file checks directly from staging, including the
initial **1652-kbps** callback. FileVersion/ProductVersion remain **1.2.1**.
SHA-256 (supersedes the earlier seek-only staging hash):

```text
aef1d68888e77ecc9682cd08605d62e443ff5afdbde7a134d29c79a30954582e  Plugins/in_vorbis.dll
```

No installed Winamp DLL was replaced. Actual player skin rendering was not
verified; these status values were captured through the real DLL in mock hosts.

## 2026-11-24 — Unified Winamp tag editor

The default local File Info integration now contributes one editable Advanced
pane to Winamp's own Basic Info / Artwork / Advanced window. The plugin does not
recreate the host's path, standard fields, tab strip, OK/Cancel or artwork UI.
The independently authored Advanced resource contains a Name/Value list, Name
entry, multiline Value entry and Add New / Delete / Delete All buttons. A native
child-window capture from the mock host was visually inspected; this is not a
claim that an installed Winamp skin was tested.

A separate ordered comment document preserves repeated field names, empty
values and long/multiline Unicode text. The tests include a 200-character field
name and a 70000-character value, independent editing of repeated PERFORMER
entries, rename and keyboard deletion. Host WM_USER broadcasts synchronize
Basic Info and Advanced without recursive echoes. OK commits and repeated OK
without changes is a byte-exact no-op; Cancel discards drafts, including Delete
All. Tests verify parent INBUILT_NOWRITEINFO ownership/restoration, metadata-cache
invalidation, native Opus R128/host ReplayGain and rating conversion, untouched
chained links, opaque/artwork bytes and bit-exact decoded audio.

The initial test harness attempted to auto-dismiss deliberately triggered error
dialogs and could interrupt the desktop. Those UI failure paths were removed
from the unattended test: stale snapshots and invalid field names are now
checked directly through WriteTagDocument. The mock host stays hidden by default;
a thread-local creation hook blocks unexpected top-level dialogs and treats them
as failures. Visible capture is opt-in through OGG_TEST_CAPTURE_UI.

Final Win32/MSVC verification passed **16/16 Release** (60.19 s) and **16/16 Debug**
(79.10 s). The new ogg_tag_editor test passed in 3.31 s and 3.33 s respectively,
without modal dialogs or user interaction. The complete runs include notification,
network, language, dynamic-CRT, exact-FLAC and advanced-Opus regressions. Final
C/C++ build logs contain no compiler warnings/errors, and git diff --check passes.

The Release DLL is PE32/x86 and is staged at dist/Plugins/in_vorbis.dll. Its bytes
match the build output; the staged DLL also passed the noninteractive editor
regression directly. FileVersion/ProductVersion remain 1.2.1. The new staging
SHA-256 supersedes the preceding entry:

```text
346bd74b3e8d196ed4689e748cd3740a4f85257aad7d358721d2b00e2d6df2f1  Plugins/in_vorbis.dll
```

No installed Winamp DLL was replaced. Actual installed-host rendering, host
artwork editing and full historical editor parity are not claimed. Existing
Vorbis/Opus writer validation and unsupported-format boundaries remain in place.

## 2026-11-24 — Basic Info field support and original technical layout

The earlier Advanced-only checks missed a real-host ABI error: an absent text
tag returned failure from the extended metadata getter, which Winamp interprets
as an unsupported field and disables in Basic Info. A new regression reproduced
this first at the empty Track # field before the fix. Supported-but-unset fields
now return success with an empty value. All twelve standard fields are checked
when empty, after creation, after edits in both directions and after deletion.
Empty primary tags also retain historical alias fallbacks.

Format Info now starts with the original Length, Average bitrate, File size,
Channels, Sampling rate, Serial number, Version and separate Vendor lines.
The supplied 415902-byte / 30-second example is asserted as **110 kbps**, not
106-kbps audio-only bitrate. Native sample counts avoid millisecond truncation;
file sizes remain 64-bit and include RIFF wrappers. Actual Vorbis header version
and nominal/minimum/maximum bitrates are retained separately from decoded audio
bitrate. Useful additional facts include codec, millisecond duration, PCM sample
count, actual lossless bit depth, chain position, Opus header gain and available
decoder details. Missing facts are omitted; lossy source PCM depth is not guessed.
Technical labels have independent append-only language IDs.

The real host exposed another problem: its fixed-height Format Info edit did
not support wheel scrolling, so the extra lines were present but inaccessible.
The pane now enables vertical/automatic scrolling on that existing read-only
multiline control, without adding or replacing controls. It handles Basic Info
being created after the plugin panes, guards the recognized native control and
restores its prior style on destruction. The hidden regression checks wheel
movement and style restoration.

Final MSVC Win32 Release and Debug suites passed **16/16** each (**62.24 s** /
**83.66 s**). The expanded ogg_tag_editor regression passed in **3.66 s** /
**3.97 s**. No compiler warnings/errors were found in the final build log;
git diff --check passes.

The final Release DLL was also exercised in an **actual Winamp 5.9.2 Build 10042**
runtime copied into an isolated build directory, with a separate profile and a
generated one-second 96-kHz Vorbis fixture. This was not a reconstructed host
window. Checks verified all twelve empty controls enabled, edits of all twelve
fields, bidirectional Basic Info/Advanced synchronization, persistence through
native OK, discard through native Cancel, and native Format Info wheel scrolling
(first visible line **0 -> 3**). Basic Info, scrolled Format Info and Advanced
captures were inspected. The isolated player was closed afterward; the installed
DLL's SHA-256 was checked unchanged. Installed profiles were not modified.

Release staging at dist/Plugins/in_vorbis.dll matches the tested build output.
PE32/x86, Release dynamic CRT and IPO/LTO remain enabled. The final staging
SHA-256 supersedes the preceding entries:

```text
a649a776303f5f5aa5509c05ee3a1f17e3af6cc0028398e27396f16eff010217  Plugins/in_vorbis.dll
```

This verifies this Winamp version's default native UI, not every skin/version,
artwork writing, the configuration-service matrix or complete historical parity.

## Not verified / not performed

No plugin or language pack was installed into the user's Winamp, no installed
Winamp DLL was replaced, and no push or public release was made.
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
