# Bundled media metadata extraction

The core launches `amule-media-probe` at a fixed location next to its executable
or in the installed `libexec/amule` directory (`bin` on Windows). It never
searches `PATH` and has no configurable parser path. The helper links FFmpeg's
libavformat, libavcodec and libavutil; it does not execute ffprobe. The small
frontend derives from [mini-media-probe](https://github.com/3togo/mini-media-probe).

The input is an already-open, seekable regular file inherited on stdin. The
versioned nine-line `AMMP 1` response retains the existing flat vocabulary:
rounded duration, overall bitrate in kbps, FFmpeg codec name, artist, album and
title, followed by `END`. Tags are valid UTF-8, escaped and limited to 4096 bytes
per field. `--json` is available for diagnostics and tests.

## Isolation and parser policy

The parent retains its 30-second watchdog, cancellation, strict response
validation and 64 KiB output limit. Windows uses a Job object and an explicit
inherited handle list. The helper sets POSIX CPU, address-space, core-dump and
file-size limits itself. These are process isolation and resource limits, not
an operating-system sandbox.

Every build, including a full distribution FFmpeg, applies these restrictions
in code before opening input:

- Custom AVIO reads only inherited stdin; `io_open` denies secondary opens and
  `protocol_whitelist` is empty. No network initialization is requested.
- `format_whitelist` permits AAC/ADTS, AC-3/E-AC-3, AIFF, AMR, APE, ASF/WMA/WMV,
  AU, AVI, CAF, DTS, DV, FLAC, FLV, GSM, raw H.264/HEVC, IVF, Matroska/WebM,
  MOV/MP4/M4A/3GP, MP3, Musepack SV7/SV8, MPEG-PS/TS/video, raw MPEG-4, MXF,
  Ogg, RealMedia/RealAudio, Wave64, WAV, WTV, WavPack and TTA. Playlists, SDP,
  concat and DASH are excluded. Other MediaInfo formats are intentionally outside this policy.
- An empty `codec_whitelist` prevents stream-info probing from opening decoders
  supplied by a full system FFmpeg. Parsers and demuxers provide the metadata.
- Reads total at most 1 GiB, repeated/backward seeks at most 256, individual
  libav allocations at most 128 MiB and parsing at most 25 seconds. The parent
  remains responsible for killing a stuck child.

The runtime whitelist in `src/mediaprobe/main.cpp` and the demuxer list in
`packaging/media/build-ffmpeg.py` / the Flatpak template must stay aligned.
Production scheduling and the GUI refresh gate share the audio/video registry
in `src/OtherFunctions.cpp`. A supported parser also needs an eligible file type;
the registry includes `.dv`, `.ivf`, `.wtv`, `.w64`, `.tta` and `.eac3`.
FFmpeg's runtime aliases differ from configure names, notably `mpeg`/`mpegps`.
Keep the MPEG video demuxer: container readers use it for stream identification.
`test_build_recipe.py` checks all three lists, normalizing runtime aliases. Raw
stream guesses below the extension probe score (50) are rejected so weak matches
in excluded containers are not published as codecs.

## Duration and tags

Reliable container/header duration uses the normal fast path. Raw MP3 duration
estimated from bitrate receives a bounded frame scan (at most 512 MiB), counting
complete Layer III frames across small gaps and ignoring trailing/partial bytes.
The audio bitrate uses only counted frame bytes and rounds to whole kbps; tags
and cover art are excluded. Reliable Xing/VBRI timing is retained. Above the scan
size limit, or when no complete total can be counted, FFmpeg's estimate remains
available. Scan deadlines are retryable, while read/seek budget exhaustion is a
file verdict. This counts encoded samples; absent gapless metadata, it does not
establish sample-exact original programme duration.

Raw H.264 without a reliable presentation timeline remains unknown. Variable-rate
pictures and a nominal frame rate cannot reconstruct missing timestamps. Valid
container timing is used for variable-rate video in timed containers.

Container artist/album/title take precedence. Attached pictures are not selected
as video. AVI's RIFF `product` is an album fallback; Ogg comments are read from
the selected video/audio stream. The existing single-audio FLAC/Opus/Vorbis/Speex
comment fallback also applies inside Matroska. Generic video track titles in
other containers are not promoted to programme titles. A tag valid as UTF-8
stays UTF-8; otherwise the whole tag is converted from Latin-1, preserving legacy
RIFF text. FFmpeg does not expose the three 3GP user-data fields found by MediaInfo
in the existing fixture; that artist/album/title loss is declared for review rather
than adding a second box parser.

Known timing differences remain for raw DTS, MPEG-1 video and MPEG-4 video:
the codec is identified but duration can remain unknown without decoder-derived
parameters or a container timeline. MediaInfo may estimate a duration for these.

## Configuration and failure markers

`ENABLE_MEDIA_PROBE` remains ON by default for core builds. Missing FFmpeg
headers/libraries fail configuration; `-DENABLE_MEDIA_PROBE=OFF` omits extraction.
The minimum FFmpeg is 4.4 (libavformat 58.76.100, libavcodec 58.134.100,
libavutil 56.70.100), covering Ubuntu 22.04 and Debian 12's FFmpeg 5.1.

`MediaMetadata/FFProbePath` remains obsolete and is removed when loading settings.
The EC tag stays reserved; `/api/v1`'s `files.ffprobe_path` remains read-only and
empty. Without an installed helper the metadata controls remain visible but
disabled; `files.media_metadata_available` reports false.

Failure markers retain their existing meaning: legacy ffprobe marker `1` gets
one retry; helper file verdicts write `2`; normal scheduling skips `2`. Refresh
ignores markers, and success clears them. The child flushes `AMMP 1` before
parsing. A crash after that header is a file verdict; a missing helper, a crash
before the header, malformed output or cancellation is not. Exit codes remain
0 (usable), 10 (no usable metadata), 11 (rejected input), 12 (configuration/I/O)
and 13 (internal deadline). The per-file allocation cap rejects oversized
container tables as file verdicts; partial stream information remains usable.
The core's startup and failure-classification code is shared with the previous
backend.

## Building and testing

Install `pkg-config`, `libavformat-dev`, `libavcodec-dev` and `libavutil-dev` on
Debian/Ubuntu; `ffmpeg-devel` on Fedora; `ffmpeg` on Arch/Homebrew; or the matching
`mingw-w64-clang-{x86_64,aarch64}-ffmpeg` package on MSYS2. Unlike the previous
MediaInfo C ABI bridge, the FFmpeg build requires development headers.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build -R MediaProbe --output-on-failure
```

The helper can also be built standalone from `src/mediaprobe`. WAV/encoding,
framing and production AVIO callback tests need no media generator. Additional
codec, MP3 duration and raw-video timing tests use ffmpeg/ffprobe only to generate
and inspect fixtures. `-DMEDIA_PROBE_REQUIRE_FIXTURES=ON` requires LAME/x264 rather
than silently skipping duration tests.

`test_hostile.py` supplies misleadingly named HLS/concat/SDP/DASH, encryption-key
URLs and a supported MOV with an external QuickTime track reference. On Linux it
uses strace to assert that no canary file is opened and no sockets are created.
Set `AMULE_REQUIRE_STRACE=1` to require that observation. The compatibility workflow
runs against full system FFmpeg on Debian 12 and Ubuntu 22.04/24.04, as well as
native bundled builds on Linux, macOS and Windows CLANG64/CLANGARM64.

The private FFmpeg builder retries transient source-download failures up to three
times using the official URL, restarting interrupted downloads and retaining
checksum verification. Permanent HTTP errors fail immediately.

The private FFmpeg builder records configure arguments and compiler/linker
flags, including macOS SDK/deployment settings. Changing them in a reused build
directory cleans old objects and reconfigures; unchanged settings reuse the build.

`packaging/media/compare.py` checks all six fields against the existing corpus
manifest and verifies fixture hashes. It can also compare a directory of real
media. Record versions, differences and run-specific results in the PR discussion;
agreement between backends alone is not correctness.

## Packaging

All binary bundles use checksum-pinned, metadata-only static FFmpeg archives.
AppImage, Windows, macOS and Alpine static builds use
`packaging/media/build-static.sh`; Flatpak uses the same pin and component list.
Decoders, encoders, programs, muxers, filters, external auto-detected libraries,
network and protocols are disabled. GPL/nonfree FFmpeg components are not enabled.
The existing macOS packaging builds each architecture before merging Universal2.

```sh
packaging/media/build-static.sh /tmp/media-prefix /tmp/media-sources
cmake -S src/mediaprobe -B /tmp/media-helper \
  -DCMAKE_PREFIX_PATH=/tmp/media-prefix -DMEDIA_PROBE_STATIC=ON
cmake --build /tmp/media-helper -j2
ctest --test-dir /tmp/media-helper --output-on-failure
```

The builder uses `gh` to download GitHub sources directly and checks SHA-256.
Linux wrappers pre-stage the archive so Docker builds consume it offline. The
exact source is retained under the prefix's `share/sources/amule-media-probe`;
the release workflow attaches that same archive as a release asset. LGPL text
and FFmpeg's licence description ship under `share/licenses/amule-media-probe`
(or macOS Resources). See `docs/THIRDPARTY.md`.

`packaging/media/test-runtime.py` checks installed notices and the pinned helper
version, inherited-file input, all six fields, Unicode, framing and malformed-file
rejection. It then starts the packaged daemon with isolated configuration and
checks persisted metadata and failure marker 2. AppImage enters through AppRun;
Flatpak executes inside its sandbox; Windows uses the relocated zip, and macOS
the mounted dmg, including both Universal2 slices. Run the complete Packaging
workflow at the reviewed head to establish actual bundle-platform coverage.
