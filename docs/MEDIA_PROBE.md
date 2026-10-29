# Bundled media metadata extraction

The core launches `amule-media-probe`. It looks next to its own executable first
(the Windows, macOS, AppImage and static bundles, and the build tree, where the
macOS `aMule.app` gets a copy after each link), then at the
installed `libexec/amule/amule-media-probe`, relative to its own folder. It never
searches `PATH`: the helper is internal, so installs keep it out of `bin`. The
helper uses MediaInfoLib only. It consumes an already-open, seekable regular file on stdin;
the parent validates a bounded, versioned nine-line response containing duration
(rounded seconds), bitrate (kbps), codec, artist, album and title. The existing
metadata queue and enable/refresh controls remain.

The parent enforces a wall-clock deadline, cancellation and a 64 KiB output
limit while the child runs. It rejects malformed framing and numeric values
outside the helper protocol. Windows uses a Job object and an explicit inherited
handle list. POSIX builds limit CPU, memory, core dumps and file size (256 KiB:
stdout is a file the core reads, so 0 would block the reply). Bundle library search paths
are preserved. This is process isolation and resource limiting, not an operating
system sandbox. Pinned bundle builds disable curl and libmms readers.

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
one retry. Only a MediaInfo file verdict writes marker `2`; the normal scheduling
path skips only `2`. Refresh ignores markers and success clears them. The helper
prints its `AMMP 1` header before it parses the file, so a crash after the header
is this file's doing and writes marker `2`. Missing helpers, crashes before the
header, malformed output and cancellation never become file verdicts.

## Build and tests

Install MediaInfoLib (`libmediainfo-dev` on Debian/Ubuntu, `libmediainfo-devel` on
Fedora, `libmediainfo` on Arch or Homebrew, `mingw-w64-<toolchain>-libmediainfo` on MSYS2). CMake builds
and installs the helper by default with either core target, to
`${CMAKE_INSTALL_LIBEXECDIR}/amule` (`bin` on Windows). Configure
`-DENABLE_MEDIA_PROBE=OFF` to omit it.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build -R MediaProbe --output-on-failure
```

The real-library tests generate WAV fixtures without extra dependencies. They
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

ffprobe agreement measures compatibility, not correctness. MediaInfo can differ
on encoder padding and bitrate accounting. For raw H.264, MediaInfo's duration
can describe only inspected frames even at full parse speed, so the helper
publishes codec but leaves duration and derived bitrate unknown. Reliable raw
H.264 timing remains follow-up work. Files lacking duration get a full scan up
to 512 MiB, bounded by the parent's watchdog.

## Packaging

All bundles build MediaInfoLib and ZenLib from checksum-pinned source archives.
AppImage, macOS, Windows and static Alpine use
`packaging/media/build-static.sh`; Flatpak manifests use the same pins and
explicitly disable curl and libmms. AppImage, macOS and Windows link these two
libraries statically into the helper to avoid platform package dependency
closures. No FFmpeg runtime or development libraries are needed.

```sh
packaging/media/build-static.sh /tmp/media-prefix /tmp/media-sources
cmake -S src/mediaprobe -B /tmp/media-helper \
  -DMEDIAINFO_LIBRARY=/tmp/media-prefix/lib/libmediainfo.a \
  -DMEDIA_ZEN_LIBRARY=/tmp/media-prefix/lib/libzen.a -DMEDIA_PROBE_STATIC=ON
cmake --build /tmp/media-helper -j2
ctest --test-dir /tmp/media-helper --output-on-failure
```

The helper installs MediaInfoLib, ZenLib, aes-gladman and tinyxml2 notices under
`share/licenses/amule-media-probe`; macOS places them in the app's Resources.
See `docs/THIRDPARTY.md`. Update the source pins in `packaging/linux/versions.env`,
render the Flatpak template with `packaging/linux/build.sh render-flatpak-manifest`.
The Flathub manifest remains pinned to 3.2.0, which uses ffprobe; add the media
modules with resolved pins when updating it to the first release with this helper.
Keep the exact sources
and build recipes with redistributed bundles.
Windows/macOS packaging, Flatpak runtime and the fully static Alpine image need
validation on their respective platforms.

## Packaged runtime validation

`packaging/media/test-runtime.py` checks the shipped notices, pinned helper
version, inherited-file input, duration/bitrate/codec, Unicode tags, exact
protocol framing and rejection of empty/malformed media. It then launches the
packaged daemon with isolated configuration and verifies persisted metadata and
failure marker 2. AppImage tests enter through the extracted AppRun; Flatpak
tests execute inside the installed sandbox. Windows tests use a relocated
extraction of the final zip, and macOS tests use the mounted dmg.

The Packaging workflow runs these checks against the bundle produced by each
build job. Flatpak clones the exact local HEAD commit rather than upstream
master, and its notices are checked in the installed bundle. Final PR validation
uses one full workflow dispatch with no `only` filter, so every build and runtime
check covers the same commit. For the Universal2 dmg, both slices are exercised
on macOS (Intel via Rosetta).

### System-library compatibility

The minimum supported MediaInfoLib is 21.09 (Ubuntu 22.04 package
`libmediainfo0v5_21.09+dfsg-4`, with ZenLib 0.4.39). All 15 tests in
`src/mediaprobe/test_backends.py` pass against that library and the bundled 26.05,
including the optional FFmpeg-generated codec/container and raw H.264 fixtures.
The suite checks Unicode tags, malformed input, duration rounding and the
removed `--backend` option as well as codec/tag field compatibility. Run it
against the helper linked to the system library:

```sh
python3 src/mediaprobe/test_backends.py build/src/amule-media-probe -v
```

## Compatibility scope and remaining gaps

The helper replaces the six fields aMule used from ffprobe: duration, bitrate,
codec, artist, album and title. It does not reproduce ffprobe's complete stream,
chapter, attachment or technical-field output. Codec names are translated
explicitly; an unknown MediaInfo format stays empty instead of publishing a
misleading identifier.

The expanded fixture audit covers WMA 1/2, WavPack, TTA, AMR-NB/WB, WAV GSM,
A-law/mu-law, IMA/MS/Yamaha ADPCM, TrueHD/MLP, float PCM in WAV/AIFF, Microsoft
MPEG-4 v2/v3, HuffYUV/FFVH, CineForm, JPEG 2000, DNxHR, raw YUV video and FLV1,
in addition to the original codec/container cases. CodecID distinguishes codec
variants within one MediaInfo Format family. A finished stream can also request
a seek to trailing tags; the helper follows that seek before finalizing, which
preserves TTA artist/album/title.

This is not universal ffprobe parity. In the expanded 36-file audit, 35 codec
names match ffprobe; raw GSM is not recognized by MediaInfoLib. Raw AMR-WB and
video-only Ogg/Theora fixtures identify their codec but provide no duration or
overall bitrate through MediaInfo, even with a full parse. Raw H.264 timing also
remains unknown as described above. RIFX float WAV is not recognized in the tested
library. Container overhead and encoder padding can produce duration/bitrate
differences, such as 193 versus 192 kbps for a Yamaha ADPCM WAV. These are library
coverage or accounting differences, not missing string translations; no duration
is invented when the library has insufficient timing data.
