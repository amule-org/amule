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

## Parsers

Each container family has its own parser in `src/mediaprobe/`. They read headers and
indexes only, never audio or video data:

| File | Formats |
|---|---|
| `riff.cpp` | WAV (RIFF, RIFX, RF64), AVI including OpenDML, AIFF and AIFF-C |
| `isobmff.cpp` | MP4, M4A, M4V, MOV, 3GP (iTunes, QuickTime and 3GPP tags) |
| `matroska.cpp` | MKV, MKA, WebM (Info, Tracks and global Tags, through SeekHead) |
| `audio.cpp` | MP1/MP2/MP3 (Xing, VBRI or a bounded frame count), ADTS AAC, FLAC, TTA, WavPack, AMR |
| `ogg.cpp` | Vorbis, Opus, FLAC, Speex, Theora and OGM streams in Ogg |
| `legacy.cpp` | ASF (WMA, WMV), FLV, MPEG program and transport streams, RealMedia (RM, RMVB) |
| `tags.cpp` | ID3v2, ID3v1, APEv2 and Vorbis comments |
| `codecs.cpp` | FourCC, WAVE format tags and Matroska CodecIDs to FFmpeg codec names |

All reads go through `Reader` (`probe.cpp`), which checks every range against the file
size and charges a fixed budget of reads and bytes. Malformed structure, an
out-of-range read or an exhausted budget is a file verdict (exit 11). Loops have
iteration caps and recursion has a fixed depth, so no input can make a parser spin
or exhaust the stack. Files in other formats publish no metadata.

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
Raw H.264 and HEVC streams have no timeline, so the helper publishes their codec
but leaves duration and derived bitrate unknown. MP3 files without a Xing or VBRI
header and raw ADTS AAC get a frame count up to 512 MiB, bounded by the read
budget and the parent's watchdog.

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

## Compatibility scope and remaining gaps

The helper replaces the six fields aMule used from ffprobe: duration, bitrate,
codec, artist, album and title. It does not reproduce ffprobe's complete stream,
chapter, attachment or technical-field output. Codec names are translated
explicitly; an unknown codec identifier stays empty instead of publishing a
misleading name. Formats outside the parser table are reported as unreadable
and get failure marker 2.
