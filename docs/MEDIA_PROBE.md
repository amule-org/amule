# Bundled media metadata extraction

The core launches `amule-media-probe`. It looks next to its own executable first
(the Windows, macOS, AppImage and static bundles, and the build tree, where the
macOS `aMule.app` gets a copy after each link), then at the
installed `libexec/amule/amule-media-probe`, relative to its own folder. It never
searches `PATH`: the helper is internal, so installs keep it out of `bin`. The
helper reads container headers with its own parsers and links no media library. It
consumes an already-open, seekable regular file on stdin;
the parent validates a bounded, versioned nine-line response containing duration
(rounded seconds), bitrate (kbps), codec, artist, album and title. The existing
metadata queue and enable/refresh controls remain.

The parent enforces a wall-clock deadline, cancellation and a 64 KiB output
limit while the child runs. It rejects malformed framing and numeric values
outside the helper protocol. Windows uses a Job object and an explicit inherited
handle list. POSIX builds limit CPU, memory, core dumps and file size (256 KiB:
stdout is a file the core reads, so 0 would block the reply). Bundle library search paths
are preserved. This is process isolation and resource limiting, not an operating
system sandbox.

`MediaMetadata/FFProbePath` is obsolete: a nonempty old setting is logged once
and the key is removed from `amule.conf`. The EC tag stays reserved and the frozen
`/api/v1` field `files.ffprobe_path` reads as empty; a PATCH that would change it
is `400 read_only_field`.
The old path, Browse and Detect controls are removed. When the core has no helper
(built with `-DENABLE_MEDIA_PROBE=OFF`, or installed without it), the toggle and the
"Re-extract media metadata" actions stay visible but disabled, and amuleapi reports
`files.media_metadata_available` as `false`. EC tag
`EC_TAG_FILES_MEDIA_METADATA_AVAILABLE` carries this to remote GUIs; an older core
sends nothing, and the controls stay enabled there. A re-extract request that still
arrives is refused with a reason (EC and REST) or a log line (the monolithic GUI).

Files with a legacy ffprobe failure marker (`FT_MEDIA_PROBE_FAILED=1`) receive
one retry. Only a helper file verdict writes marker `2`; the normal scheduling
path skips only `2`. Refresh ignores markers and success clears them. The helper
prints its `AMMP 1` header before it parses the file, so a crash after the header
is this file's doing and writes marker `2`. Missing helpers, crashes before the
header, malformed output and cancellation never become file verdicts.

## Scope and conditions

The helper publishes six fields: duration, bitrate, codec, artist, album and
title. They help users recognise fake files; no download, sharing or network
function depends on them. eMule, the most common client on the network,
publishes them only for MP3, AVI, WAV, RealMedia and, on Windows, ASF. Support is
therefore best effort and subject to the following conditions.

1. **Closed list.** Only the formats listed under [Supported formats](#supported-formats)
   are read. Any other format, or an unusual layout of a supported one, leaves
   the fields empty and records failure marker 2. This is expected behaviour,
   not a defect.
2. **New formats.** A format is added only with evidence that it is widely
   used for sharing files on the Internet, not because another media library
   reads it.
3. **Correctness.** A wrong value for a supported format is a defect. A value
   that cannot be established, including a duration cut short by a scan limit,
   is left empty; partial values are never published. In files with video, the
   duration and codec are those of the video stream, as in eMule.
4. **Security.** Any crash, hang or out-of-bounds read on any input is a defect.
   Every parser change keeps `test_hostile.py` passing and is fuzzed with the
   libFuzzer harness. Security fixes are not optional.
5. **Isolation.** The helper runs in a separate process under the limits
   described above and the read budget described under [Parsers](#parsers).
6. **Operating system restrictions.** On Linux, macOS and Windows the helper
   must be confined by the operating system to reading the file it receives,
   with no network access, no writes and no new processes. This is a
   requirement of the feature and is delivered in a separate change; until
   then, process isolation and resource limits are the only protection.

## Supported formats

Formats in current use for sharing audio and video:

| Container | Extensions | Codecs | Parser |
|---|---|---|---|
| Matroska, WebM | `.mkv` `.mka` `.webm` | H.264, HEVC, AV1, VP8, VP9, MPEG-1/2/4 video, VC-1; AAC, AC-3, E-AC-3, TrueHD, DTS (including DTS-HD), FLAC, Opus, Vorbis, MP3, MP2, ALAC, PCM | `matroska.cpp` |
| MP4, QuickTime, including fragmented MP4 | `.mp4` `.m4a` `.m4v` `.mov` `.3gp` | H.264, HEVC, AV1, VP9, MPEG-2/4 video; AAC, AC-3, E-AC-3, TrueHD, DTS, ALAC, FLAC, Opus, MP3, PCM | `isobmff.cpp` |
| MPEG transport stream | `.ts` `.m2ts` `.mts` | H.264, HEVC, MPEG-1/2/4 video, VC-1; AAC, MP2, AC-3, E-AC-3, TrueHD, DTS (including DTS-HD), LPCM | `legacy.cpp` |
| MPEG program stream | `.mpg` `.mpeg` `.vob` | MPEG-1/2 video; MP2, AC-3, DTS, LPCM | `legacy.cpp` |
| AVI, including OpenDML | `.avi` | MPEG-4 Part 2 (Xvid, DivX), H.264, HEVC, MPEG-1/2 video; MP3, AC-3, DTS, AAC, PCM | `riff.cpp` |
| Ogg | `.ogg` `.oga` `.opus` | Vorbis, Opus, FLAC | `ogg.cpp` |
| FLAC, MP3 | `.flac` `.mp3` | FLAC; MP3, MP2 | `audio.cpp` |
| WAV | `.wav` | PCM | `riff.cpp` |

Older formats, kept because eMule publishes metadata for them or because they
remain on the network:

| Container | Extensions | Codecs | Parser |
|---|---|---|---|
| ASF | `.wma` `.wmv` `.asf` | WMA, WMV, VC-1 | `legacy.cpp` |
| RealMedia | `.rm` `.rmvb` | RealVideo; Cook, ATRAC3, AAC, RealAudio | `legacy.cpp` |
| FLV | `.flv` | Sorenson H.263, VP6, H.264, HEVC; MP3, AAC, Speex | `legacy.cpp` |
| Ogg, legacy streams | `.ogv` `.ogm` `.spx` | Theora, OGM video; Speex | `ogg.cpp` |
| AIFF, AIFF-C | `.aif` `.aiff` | PCM, IMA ADPCM, A-law, mu-law | `riff.cpp` |
| Raw audio streams | `.aac` `.mp2` `.tta` `.wv` `.amr` | ADTS AAC, MP1/MP2, TTA, WavPack, AMR | `audio.cpp` |

Tags are read from ID3v1, ID3v2, APEv2, Vorbis comments, Matroska Tags, ASF,
RealMedia, FLV and the iTunes, QuickTime and 3GPP atoms (`tags.cpp`). Codec
names follow FFmpeg's vocabulary; an identifier missing from the parser tables
leaves the codec empty.

Out of scope: disc images and structures (ISO, `VIDEO_TS`, `BDMV`), archives,
subtitle and HDR metadata, and codecs not yet widely used for sharing, such as
VVC and AV2. Raw H.264 and HEVC streams have no timeline, so only their codec is
published.

## Parsers

All reads go through `Reader` (`probe.cpp`), which checks every range against the file
size and charges a fixed budget of reads and bytes. Malformed structure, an
out-of-range read or an exhausted budget is a file verdict (exit 11). Loops have
iteration caps and recursion has a fixed depth, so no input can make a parser spin
or exhaust the stack. A file may use at most 640 MiB of reads and 8 Mi read
calls; the MP3 and ADTS frame count stops at 512 MiB.

## Build and tests

The helper has no dependencies beyond the C++ standard library. CMake builds and
installs it by default with either core target, to
`${CMAKE_INSTALL_LIBEXECDIR}/amule` (`bin` on Windows). Configure
`-DENABLE_MEDIA_PROBE=OFF` to omit it.

`test_hostile.py` feeds random data after every supported signature, impossible
sizes, deeply nested boxes, a self-referencing Matroska SeekHead and endless MPEG
frames, and requires a clean verdict with valid output for each. The libFuzzer
harness covers every parser through the format dispatch:

```sh
cmake -S src/mediaprobe -B /tmp/fuzz -DCMAKE_CXX_COMPILER=clang++ -DMEDIA_PROBE_FUZZ=ON
cmake --build /tmp/fuzz --target amule-media-probe-fuzz
/tmp/fuzz/amule-media-probe-fuzz /tmp/fuzz-corpus seeds/ -max_len=262144
```

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build -R MediaProbe --output-on-failure
```

The helper tests generate WAV fixtures without extra dependencies. They
cover rounding, subsecond audio, empty and malformed files, Unicode, invalid tag
encoding, large tags and protocol framing. Optional ffmpeg-generated fixtures
cover raw AVC, Xvid, ProRes, H.263, WMA and AVI/FLV metadata. ffmpeg is a test
fixture generator only. The runner tests cover cancellation, timeout, crashes,
strict output validation, partial results and file-verdict classification. The
real WAV runner test is portable; shell-based failure tests run on POSIX.

For a short comparison, generate a tagged fixture and run both tools:

```sh
ffmpeg -f lavfi -i sine=duration=3 -metadata artist=Example /tmp/probe.wav
build/src/amule-media-probe --json < /tmp/probe.wav
ffprobe -v quiet -show_format -show_streams -of json /tmp/probe.wav
```

ffprobe agreement measures compatibility, not correctness. Durations can differ
by a fraction of a second where containers count encoder padding differently.

## Packaging

The helper has no third-party dependencies, so every bundle (AppImage, Flatpak,
macOS, Windows and static Alpine) builds it from the aMule sources with the rest
of the tree. No MediaInfoLib, ZenLib or FFmpeg sources, pins or licence notices
are shipped for it.

## Packaged runtime validation

`packaging/media/test-runtime.py` checks the packaged helper version (native
parsers), inherited-file input, duration/bitrate/codec, Unicode tags, exact
protocol framing and rejection of empty/malformed media. It then launches the
packaged daemon with isolated configuration and verifies persisted metadata and
failure marker 2. AppImage tests enter through the extracted AppRun; Flatpak
tests execute inside the installed sandbox. Windows tests use a relocated
extraction of the final zip, and macOS tests use the mounted dmg.

The Packaging workflow runs these checks against the bundle produced by each
build job. Flatpak clones the exact local HEAD commit rather than upstream
master. Final PR validation uses one full workflow dispatch with no `only`
filter, so every build and runtime check covers the same commit. For the
Universal2 dmg, both slices are exercised on macOS (Intel via Rosetta).
