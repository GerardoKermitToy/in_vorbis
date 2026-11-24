# Advanced decoding — 1.2.1

## Exact integer FLAC

Ogg FLAC decoding retains the signed integer samples supplied by libFLAC alongside
its floating-point output. With **32-bit output**, **dither off**, unity effective
gain, unchanged rate and an identity channel layout, `Preserve exact FLAC PCM`
passes the original integer samples directly to PCM packing. Sixteen- and 24-bit
sources are left-aligned in the 32-bit output; a 32-bit source retains every bit,
including values that cannot round-trip through float32.

This guarantee ends at the input plugin's output buffer. Winamp DSP, output-plugin
volume processing, the Windows mixer or a hardware driver can still change the
samples. Changing gain, channel mixing, rate, or enabling dither selects the normal
floating-point processing path instead. The IEEE float extended-read export is
necessarily not an exact representation of arbitrary 32-bit integer PCM. The
integer extended-read export uses undithered 32-bit output when requested.

The exact companion is retained across partial reads and native chained-format
boundaries, and reset on seek. When libFLAC cannot seek a valid very short Ogg
file, a cancellable decode/discard fallback starts at the beginning.

## Opus HD / QEXT

The pinned libopus 1.6.1 build includes QEXT. The independent Ogg packet reader
examines actual packet-padding extensions (QEXT extension ID 124); the informational
OpusHead input-rate field alone is **not** evidence of HD content.

With `Decode Opus HD at 96 kHz when present` enabled, a QEXT logical stream is
decoded by libopus at native 96000 Hz. Ordinary Opus stays at 48000 Hz. Explicit
output-rate settings are applied afterwards by the normal resampler; selecting
96000 Hz for a non-HD input does not manufacture QEXT audio.

Ogg granule positions, pre-skip and seek times remain in their specified 48000-Hz
timebase. End trimming is converted to the native decoder rate. Disabling HD
keeps the 48000-Hz decoding path; it is not a promise to preserve the ultrasonic
content of an HD source.

Seekable inputs are scanned for link metadata, duration and QEXT before playback.
This performs more input I/O than the old libopusfile path. Advanced Opus seeks
currently decode from the selected link's beginning so CELT/SILK and binaural
history are correct; seeking far into a long link can be slow. Forward-only
sources select their initial rate from the first audio page. A later first
appearance of QEXT is rejected rather than silently being called native HD.

## Optional speech enhancement

OSCE and blind bandwidth extension (BWE) are compiled in with checksum-pinned,
embedded upstream neural weight tables. Both switches are **off by default**.
No model download, Python/PyTorch execution or external model loading occurs
at playback time.

- Speech enhancement selects the libopus speech-enhancement complexity for
  eligible SILK material. It changes decoded audio and is not a lossless mode.
- BWE applies to eligible wideband speech decoded at 48000 Hz and synthesizes
  high-frequency content. It does not recover the original high frequencies.
- BWE is inapplicable at native 96000 Hz; that file still plays as HD with BWE
  disabled. The file-information decoder note reports the effective policy.
- CELT music and other ineligible material are not promised an enhancement.

These options are not DRED/FEC support and do not conceal missing Ogg pages.
HTTP uses TCP; lost transport packets are not the same as identified missing
Opus audio packets that a PLC/FEC decoder can reconstruct.

## Ambisonics

Mapping families **2** (ACN/SN3D multistream) and **3** (projection/demixing) use
separate validated decoder paths. Ambisonic coefficients are never mistaken
for a Windows speaker layout. Complete harmonic orders representable in the
Opus channel-count field are accepted, with an optional listener-relative,
non-diegetic stereo pair. Projection matrix sizes and channel/stream mappings
are bounded and checked before decoder creation. Rectangular family-3 matrices
are supported: when coded and output channel counts differ, each decoded coded
channel contributes through the full signed-Q15, column-major demixing matrix;
no extra matrix columns are silently dropped.

The renderer offers:

- stereo speakers;
- headphones with an independently implemented **analytic approximate binaural**
  model (interaural delay and head shadow);
- quad, Windows-order 5.1, and Windows-order 7.1 speakers.

The speaker renderer uses a cosine-tapered spherical-harmonic sampling matrix.
It is not an exact inverse for every speaker array. Surround LFE is intentionally
silent; non-diegetic stereo is routed directly to the front left/right outputs.
The headphone model is **not measured/personalized HRTF**, has no head tracking,
and is not a certified Dolby or other proprietary spatial renderer. Like other
mixing paths, large sums can require preamp headroom to avoid clipping.

## Safety and compatibility limits

The independent Opus framing path bounds packets, page batches, metadata and
logical-link counts, checks Ogg CRC/sequence/continuation and validates Opus
packet envelopes. After validated headers, a forward-only Icecast connection
may join the initial audio page at a higher sequence number; one incomplete
leading packet fragment can be discarded within the packet bound. This exception
does not apply to local files or later missing/damaged pages. Other damaged or
truncated input is rejected; this is deliberately not a damaged-media
recovery/concealment feature. Unrelated multiplexed streams
can be skipped but are not independently selectable audio tracks.

These are Ogg input extensions. They do not add native `.flac`, WebM or MP4 input,
change the sibling `in_opus` extension routing, or add Matroska Opus decoding.
Actual output-plugin/hardware compatibility at 96 kHz and integration in an
installed Winamp still require real-host validation.

## Winamp status and playlist updates

Instantaneous bitrate remains available without periodically requesting a title
or playlist refresh. The plugin reports the complete output format when playback
starts. A native chain with a different format reports rate/channels while
preserving bitrate with `-1`; a changed displayed kbps is sent separately with
rate/channels `-1`. Even a real output reopen therefore avoids the host's implicit
title-refresh path. The cached displayed bitrate is retained across a seek and
updated only when its value actually changes. Seeking within a link does not reparse ReplayGain/R128 or
invalidate unchanged metadata. Link-number changes, output-format negotiation
and recording errors do not themselves request a playlist refresh: title IPC is
sent only when the display title or actual tags change, including fields used by
Winamp title formatting. Identical-tag chains therefore remain quiet.

Host snapshots are updated on metadata revisions rather than string comparisons
for every PCM block. Visualization conversion is skipped when neither callback
is present. Paused and already-notified EOF workers sleep on control/stop events;
seek, resume and stop wake them. Output drain/backpressure and exact seek decoder
history are retained. Disabling the bitrate display is not required.
