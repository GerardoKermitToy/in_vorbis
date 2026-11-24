# Feature coverage and compatibility

This is an independent GPL-3.0-only implementation, not a modified build of
Winamp's WCL-licensed `in_vorbis`. The behavioral reference is
`manfromafar/winamp` revision `9c7f91496c56738fd707905f257643b792c05bb9`.
No original implementation, resource scripts, artwork or logos are included.

**Full historical feature parity is not claimed.** The supported features below
are implemented; the final section identifies known differences and areas that
still need real-host verification. A passing mock-host test is not a claim that
all Winamp versions or every file in the wild have been tested.

## Implemented and regression-tested

| Area | Implemented scope | Automated coverage |
|---|---|---|
| Codecs | Ogg Vorbis, Opus, FLAC, Speex | Native sample counts, seek, output formats and resampling for all four |
| Exact FLAC | Integer companion preserves all source bits in eligible undithered 32-bit output | Native 16/24/32-bit precision, edge values, chunking, seek, native chains, processing fallback and DLL conversion |
| Local FLAC bitrate | Per-link average from compressed Ogg page bytes and native duration; available at startup and retained across seeks | Exact byte/duration calculation, file/memory/RIFF, mixed-rate links, initial Winamp `SetInfo`, average/instantaneous-display fallback without periodic title refresh |
| Opus HD | Actual QEXT detection and native 96 kHz decode; ordinary Opus stays 48 kHz | Native sample counts, 30 kHz spectral response, hints, seek and mixed-rate chains |
| Opus speech | Optional OSCE and eligible 48 kHz BWE; embedded pinned neural tables | Speech/BWE output differences, unchanged frame counts, explicit off policy |
| Opus Ambisonics | Mapping families 2/3, ACN/SN3D and projection, optional non-diegetic stereo; stereo/headphone/quad/5.1/7.1 rendering | Header/matrix validation, impulse direction, finite guarded buffers, LFE silence, stateful seek and mixed-family chains |
| Ogg Opus routing | `.ogg`/`.oga` content detection; direct `.ogg` HTTP claim | Winamp input mock and actual RadioSEGA HTTP playback through the DLL |
| Local files | Unicode paths, read-only fullwidth-pipe path fallback, RIFF/WAVE data wrappers; startup duration and seeking for all four Ogg codecs, including FLAC with unknown STREAMINFO length | `ogg_audio`, `ogg_plugin`: file/memory transport, forward/backward/EOF/rewind, stable length/tags, paused seek/flush/resume |
| Multichannel | Vorbis layouts 1–8; source/Windows, raw codec order, stereo, mono, quad, DS and DS2; legacy 5.1 input order | Impulse matrices, channel/layout and exact ordering cases |
| Chains | Native output drain/reopen on rate/channel changes for playback; fixed-format extended read | Vorbis/Opus/FLAC/Speex segment counts, resampler tail drain, first-block retention, output rejection fallback, seek/stop, DSP/visualization negotiation |
| PCM | 8-bit unsigned, 16/24/32-bit signed integer; SpeexDSP resampling; rectangular/triangular/Gaussian dither; none/first-order/ATH/low-ATH shaping | Frame guards, distributions, deterministic reset/chunking/channel isolation, overloads, rate fallbacks and shaping spectra |
| Gain | Track/album/automatic, preferred-only/fallback, old RG aliases, R128, preamps, peak modes | Positive `+6.00 dB` parsing, gain multipliers, fallback and peak tests |
| Metadata | Common tags, aliases, rating mapping, technical fields, current chain/radio title | Direct backend and DLL metadata round trips |
| Opus metadata gain | Winamp ReplayGain fields translated to/from signed R128 Q8 tags | Setter/getter conversion, malformed input and deletion tests |
| Tag writes | Transactional local Vorbis/Opus text edits, selected or all sequential links, deletion tombstones | Bit-exact decoded audio preservation; chained edits; CRC-damaged source remains unchanged |
| Winamp lifecycle | Play/stop, output open/close, pause controls, seek, volume/pan callbacks, EOF/drain | Input/output host integration; bounded writes and output lifecycle |
| Extended read | Integer and IEEE float PCM, time seek, kill switch | Exact lengths, format contract, float checks and seek/kill cases |
| HTTP transport | Timeouts, bounded probing/buffering, cancellation, redirects, chunked bodies, ICY metadata stripping | Byte-exact loopback transport and decoder tests |
| HTTP seeking | Opt-in known-length HTTP Range access; validated status and Content-Range | Byte seeking, rewind, EOF and decoded time seeking |
| Stream recording | Ogg audio bytes without ICY metadata, explicit destination, visible write errors | Recorded bytes match the source Ogg fixture |
| Reconnect | Bounded retry policy with a stable output format | DLL reconnects and decodes a second loopback connection without reopening output |
| Dialogs | Native classic controls; five settings tabs; standalone standard/custom tags and chain navigation; editable unified Advanced tab synchronized with Winamp Basic Info | Classic tag save/delete; unified host broadcasts, repeated/long/multiline fields, rename, Delete/Delete All, OK/Cancel, stale-edit refusal and audio/artwork preservation |
| Settings | Independent per-user local fallback plus read-only Winamp ReplayGain/proxy snapshots | Defaults, persistence/bounds, documented config-service dispatch, all RG modes, port-80 restriction, absent/invalid host items and credential rejection |
| Language packs | Independent Wasabi language module, per-string English fallback, text-only native dialogs, compatible legacy Name/Value labels | Mock IPC/service lifetime, partial/missing/malformed translations, no user-text/layout changes, cache reset |
| Matroska | Optional Wasabi `A_VORBIS` decoder service, new independent GUID/name, Xiph-laced headers, 16/24/float PCM | Mock IPC registration, dispatch, decoder formats, malformed headers, output capacity guards/retry, flush/EOS/close |
| Build | Both input DLLs in one x86 CMake build; source-built codecs; dynamic CRT; Release IPO/LTO | Combined Release/Debug CTest; archive CRT directives and DLL imports |

The Matroska test originally passed a buffer-size value where the Wasabi
argument array required the address of a pointer. A debugger identified that
mock-call error; the test now uses the correct pointer indirection and also
unregisters its mock service before unwinding the manager's lifetime. It must
pass like the other suites; it is not disabled or skipped.

## Unified tag editor

For local files, the default File Info window is Winamp's own window: Winamp
supplies Basic Info, Artwork, the path and OK/Cancel. The plugin contributes one
Advanced child, not another set of standard fields or a second editor launcher.
Its two-column list, Name entry, multiline Value entry and Add New / Delete /
Delete All buttons are independently authored native controls.

Advanced and Basic Info exchange the host's `WM_USER` field broadcasts in both
directions. The pane sets `INBUILT_NOWRITEINFO` on its parent while alive so the
host does not also commit the same edit through the generic setters. The prior
property value is restored on destruction. OK commits; Cancel and closing discard
uncommitted drafts. The optional standalone dialog remains the fallback when
unified File Info is disabled.

All twelve Basic Info fields stay available even when their tags are absent:
Track #, Disc #, BPM, Title, Artist, Album, Album Artist, Year, Genre, Comment,
Composer and Publisher. The metadata getter returns success with an empty
value for supported-but-unset fields, rather than telling Winamp to disable
those controls. Historical alias fallbacks also work when the primary field
exists but is empty.

Format Info starts with the historical Length / Average bitrate / File size /
Channels / Sampling rate / Serial number / Version / Vendor block. Average
bitrate uses complete container bytes and native duration, not the audio-only
bitrate. Header bitrate bounds are shown when present. Additional known facts
follow: codec, exact duration and samples per channel, lossless source bit depth,
chained-link identity, Opus header gain and decoder details. Unknown facts and
lossy source bit depth are not invented. Labels use append-only language IDs.
Winamp 5.x's existing read-only Format Info edit receives vertical/mouse-wheel
scrolling so long vendor text and extra facts remain reachable. No replacement
control is added; the original style is restored on pane destruction. Unknown
host layouts retain their own behavior.

The editor reads an ordered comment snapshot rather than the playback display
map. Repeated names remain separate rows; empty values, multiline Unicode,
long names and long values round-trip without fixed UI-buffer truncation. Only
changed field groups are rewritten. Unchanged text, opaque comments, artwork,
other chained links and encoded audio are retained. The existing writer's
aggregate 16 MiB header bound and structural validation still apply. A changed
source comment snapshot is reported instead of silently overwriting a concurrent
edit. Unsupported writable formats have a read-only Advanced pane.

The `ogg_tag_editor` CTest loads the real DLL into a mock unified host. It runs
with hidden host windows by default; expected failures are tested directly at
the writer boundary, and an unexpected top-level dialog is blocked at creation
and fails the test. Visible screenshot capture requires the explicit
`OGG_TEST_CAPTURE_UI` environment variable. The test does not establish the
appearance of an installed Winamp skin or provide an artwork editing service.
A separate native-host check in an isolated Winamp 5.9.2 Build 10042 runtime
verified enabled empty fields, all twelve field round trips, Basic Info/Advanced
synchronization, OK/Cancel and scrolling of the existing Format Info field.
The installed player's DLL and profile were not used as writable test targets.

## Implemented, but not exhaustively tested

- HTTPS certificate validation and downgrade restrictions use WinHTTP defaults
  and explicit policy. A real TLS/proxy/authentication deployment matrix has not
  been run.
- System/manual proxy policies and transient Basic/Digest authentication are
  implemented. Credential dialogs have not been automated with real passwords.
  Credentials are not persisted, and URL-embedded credentials are rejected.
- DSP and visualization callbacks are wired. Every third-party DSP/output plugin
  is not covered by the mock host.
- Additional malformed, multiplexed, chained, damaged or unusually paginated
  media needs a larger external corpus. Tests generate their own fixtures;
  private user recordings are not bundled.
- Matroska dispatch and struct sizes are checked for the 32-bit ABI, but actual
  Winamp `in_mkv` container playback has not been verified in an installed player.

## Known differences and remaining historical parity gaps

1. **Host settings:** Winamp-wide ReplayGain and proxy settings are read through
   the public configuration service at playback startup. Separate inheritance
   switches preserve explicit local choices. Missing services/items use local
   fallbacks; no host preference is written. This is a snapshot, not mid-track
   live synchronization, and actual installed-host integration remains unverified.
2. **Chained output negotiation:** native playback follows effective rate/channel
   changes with drain/close/open, cumulative time and first-block retention. A
   rejected multichannel format falls back to stereo. Explicit output settings
   still apply; disabling native chains and all extended-read sessions use a
   stable format. Reconnect preserves the currently opened transport format.
3. **Processing equivalence:** rectangular/triangular/Gaussian dither, first-order,
   ATH/low-ATH shaping and mono/quad/DS/DS2 matrices are independently implemented.
   ATH filters are derived from an analytic hearing-threshold curve, not the
   historical coefficient tables; unsupported rates use first-order shaping.
   DS/DS2 are amplitude matrices, not phase-quadrature encoders. No bit-exact
   historical processing equivalence is claimed. Quad and matrix sums can clip;
   use preamp headroom. Raw codec order and the legacy six-channel interpretation
   are explicit compatibility options, not recommended modern Windows output.
4. **32-bit PCM:** ordinary Winamp output is signed 32-bit integer. IEEE float is
   explicitly available through the float extended-read export and Matroska
   service, not through ordinary output-plugin negotiation.
5. **Tag editing:** FLAC/Speex, radio and RIFF wrappers are read-only. Vorbis/Opus
   files with simultaneous editable audio streams, unsafe shared audio/header
   pages, bad CRC/lifecycle, reused serials or missing EOS are rejected. Retained
   headers/tags are bounded to 16 MiB. Reparse-point leafs and multiply hard-linked
   source files are not replaced. Embedded artwork is preserved, not edited.
6. **Language-pack scope:** strings use an independent module GUID and explicit
   stable IDs. Historical packs generally lack that module; compatible legacy
   Name/Value column labels can be reused read-only. All absent/empty/invalid
   strings retain the built-in English fallback. No foreign dialog templates
   or user data are loaded as UI translations. Actual installed-host language
   pack discovery has not been verified.
7. **Matroska scope:** the service claims `A_VORBIS`, like the historical service.
   It does not claim Matroska Opus/FLAC/Speex or register `.mkv`/`.mka` extensions;
   the host container plugin supplies those files.

8. **Advanced decoding:** the exact-FLAC guarantee ends at eligible 32-bit input
   output buffers, not downstream DSP, output plugins or the Windows mixer.
   The Opus HD/OSCE/Ambisonics paths have independent Ogg framing with strict
   CRC/sequence checks. Local inputs are scanned before playback; advanced seeks
   currently decode from the selected link BOS and can be slow. Forward-only HD
   must be identified on the first audio page; late first QEXT is rejected.
   OSCE/BWE alter eligible speech, are off by default and are not PLC/FEC/DRED.
   Spatial decoding is a cosine-tapered sampling renderer; headphones use an
   analytic approximation, not measured/personalized HRTF or head tracking.
   Native .flac, WebM, MP4 and Matroska Opus are not added by these Ogg features.

Do not describe this release as a drop-in, 100% behavioral clone of every
historical `in_vorbis` feature. Closing the explicit gaps above is necessary
before making that claim.
