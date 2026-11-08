# Bundled media metadata extraction

The core launches `amule-media-probe`. It looks next to its own executable first
(the Windows, macOS, AppImage and static bundles, and the build tree, where the
macOS `aMule.app` gets a copy after each link), then at the installed
`libexec/amule/amule-media-probe`, relative to its own folder. It never searches
`PATH`: the helper is internal, so installs keep it out of `bin`. The helper
reads container headers with its own parsers and links no media library. It
consumes an already-open, seekable regular file on stdin; the parent validates a
bounded, versioned nine-line response containing duration (rounded seconds),
bitrate (kbps), codec, artist, album and title. The existing metadata queue and
enable/refresh controls remain.

The parent enforces a wall-clock deadline, cancellation and a 64 KiB output
limit while the child runs. It rejects malformed framing and numeric values
outside the helper protocol. Windows uses a restricted primary token, nested Job objects and an explicit
inherited handle list. POSIX builds limit CPU, descriptors (256), core dumps and file size (256 KiB:
stdout is a file the core reads, so 0 would block the reply); Linux also caps the
helper address space at 1 GiB. Bundle library search paths are preserved during
trusted runtime initialization. Before reading media, the helper installs the
[operating system restrictions](#operating-system-restrictions) below.

`MediaMetadata/FFProbePath` is obsolete: a nonempty old setting is logged once
and the key is removed from `amule.conf`. The EC tag stays reserved and the
frozen `/api/v1` field `files.ffprobe_path` reads as empty; a PATCH that would
change it is `400 read_only_field`. The old path, Browse and Detect controls are
removed. When the core has no helper (built with `-DENABLE_MEDIA_PROBE=OFF`, or
installed without it), the toggle and the "Re-extract media metadata" actions
stay visible but disabled, and amuleapi reports `files.media_metadata_available`
as `false`. EC tag `EC_TAG_FILES_MEDIA_METADATA_AVAILABLE` carries this to
remote GUIs; an older core sends nothing, and the controls stay enabled there. A
re-extract request that still arrives is refused with a reason (EC and REST) or
a log line (the monolithic GUI).

Files with a legacy ffprobe failure marker (`FT_MEDIA_PROBE_FAILED=1`) receive
one retry. Only a helper file verdict writes marker `2`; the normal scheduling
path skips only `2`. Refresh ignores markers and success clears them. The helper
prints its `AMMP 1` header before it parses the file, so a crash after the
header is this file's doing and writes marker `2`. Missing helpers, crashes
before the header, malformed output and cancellation never become file verdicts.

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
   requirement of the feature. The implementations, verification and remaining
   platform limits are described below.

## Operating system restrictions

The threat is a hostile media file, including a parser exploit. Trusted helper,
loader and C++ runtime initialization precede confinement; **no media parsing starts
before confinement**. The input is an inherited read-only regular file;
the only permitted application writes are the metadata reply and diagnostics on
the two inherited output handles. They do not grant permission to open output
paths. No media paths or directory trees are added to a sandbox allow list. The parser
also receives a minimal runtime environment, without unrelated parent variables
that might contain credentials. Runtime library paths are preserved for bundle
and build-tree compatibility, including Windows `SystemRoot` and `PATH`. These
paths do not grant file access after confinement.

If OS confinement fails, the helper refuses to parse and returns exit 13, without a
protocol header or metadata. The core discards every failed, incomplete or
malformed reply. An infrastructure failure never becomes a persistent bad-file
marker. Existing metadata is updated only after a complete validated success.
The main application remains alive when a helper crashes, times out, exhausts
its budget or produces excessive output; metadata extraction may yield nothing.
No user-space sandbox can guarantee safety against operating-system/kernel
vulnerabilities or a system already compromised outside aMule.

- **Linux (x86-64, i386, arm64, armel/armhf, ppc64, riscv64 and s390x):** close all unrelated inherited descriptors, disable
  core-dump/ptrace exposure, set `no_new_privs`, and install a default-deny seccomp
  filter. Reads/seeks are limited to stdin and writes to stdout/stderr, including
  the vectored I/O used by musl's standard library in static builds. New file
  opens, sockets (including Unix sockets), process creation, execution, tracing,
  descriptor acquisition and io_uring are denied. Allocations remain available,
  but new executable mappings are denied. The filter checks the syscall ABI.
  Unsupported ABIs, unavailable seccomp and blocked setup return no metadata.
  A failed confinement setup is reported once and disables extraction until
  restart, without marking any media file as unprobeable. On s390x, the kernel's
  pointer-argument `mmap` cannot be inspected safely; it remains denied. glibc
  allocations use the bounded `brk` heap instead. Other allocators that require
  that syscall may fail to allocate, which also returns no metadata.
  Older kernels use `/proc/self/fd` to close descriptors when `close_range` is
  unavailable; an inaccessible descriptor directory also fails closed.
- **macOS:** close unrelated descriptors through `/dev/fd`, then apply a
  default-deny Seatbelt profile. Only inherited file handles remain available;
  unused socket descriptors may be created, but connections/listening are denied;
  the profile grants read-only system configuration queries for the runtime,
  and grants no file paths, network, Mach services or child processes.
  **Compatibility limit:** `sandbox_init` is deprecated and the custom profile
  language is not supported by Apple for third-party use. A rejected profile
  disables extraction rather than relaxing restrictions. App Sandbox is not a
  drop-in replacement for a standalone helper. The macOS 27 Endpoint Security
  descendant API requires a separate entitlement and does not cover older
  systems. macOS does not have the Linux address-space guarantee here: parser
  allocation/read limits and the parent deadline remain in effect, but this
  implementation does not claim a hard macOS RSS cap.
- **Windows:** a trusted outer helper starts the parser suspended with a
  restricted primary token (NULL restricting SID, other groups/user deny-only,
  privileges removed). Trusted loader/runtime startup uses low integrity and
  a restricted impersonation token retaining only the caller's existing access.
  Startup uses a separate, noninteractive window station and low-integrity
  desktop; the restricted token receives no access to the interactive desktop.
  Both stages start detached, using inherited standard handles without creating
  a console whose initialization would require additional permissions.
  The parser's output handles are pipes. The trusted launcher relays at most
  64 KiB of metadata and 8 KiB of diagnostics. It kills a parser that exceeds
  either bound; the parser cannot resize or preallocate the core's temporary
  output files, even between parent polling checks.
  Before parsing, the helper lowers its primary integrity to untrusted and drops
  the startup token. A failure at either step returns no metadata.
  Reverting again cannot restore privileges because its primary token is already
  restricted. Only the three standard handles are inherited. A nested Job
  requires a single parser process, caps memory at 1 GiB, restricts UI access and
  kills the parser when its launcher exits. The core's outer Job owns both the
  launcher and parser. Any token, Job, inheritance or startup failure returns no
  metadata. **Filesystem limit:** restricted-token access checks depend on
  securable resources with valid ACLs; FAT/exFAT or null-DACL resources do not
  provide the same guarantee as normally secured NTFS. This is not a claim of
  confinement against misconfigured/nonsecurable resources. The core supplies
  the read-only input handle. The launcher validates that it is a regular file
  and gives the parser a duplicate with read access only, including when an
  outside caller supplies a writable handle. Pipes and directories are refused.

Failure logs include the exit status or signal and a reason (setup, input I/O,
unsupported/malformed data, exhausted read budget, timeout, cancellation, output
limit or invalid framing). stderr is separate from the metadata protocol. The
core caps diagnostics at 8 KiB, logs at most 2 KiB, and replaces control and
non-ASCII bytes before logging. Bulk operations retain their existing limit on
naming individual files; further reasons go to the debug log after that limit.

`MediaProbeStartup` first checks one known-good WAV. If initialization fails,
the dependent parser/denial suites are skipped with an explicit setup failure,
rather than repeating a broken launch for every hostile input.
`MediaProbeSandbox` tests actual denials and successful inherited-file I/O on
native runners. `MediaProbeHelper` and `MediaProbeHostile` verify useful extraction
and malformed-input handling under the production restrictions. Core
`MediaProbeEnvTest` verifies crash, timeout, setup-error, partial-reply and output
failure containment, with no failed result published. The dedicated **Media
probe restrictions** workflow runs the standalone checks on Linux x86-64/arm64,
macOS Intel and Windows ARM64, with static musl on x86-64, ARM64 and native
i386. Build CI covers Linux x86-64, macOS ARM64 and Windows x64. The manual
**Media probe Linux CPU validation** workflow cross-builds static helpers and
boots full Debian kernels for armel, armhf, ppc64el, riscv64 and s390x. Guest
provisioning uses user emulation; restriction tests run only inside the full VM,
without host shares or network access. These jobs explicitly report optional
FFmpeg fixtures as skipped. A scheduled or queued job is not verification;
consult its completed results for the exact revision. Packaged-runtime checks
remain necessary because installed loaders, outer sandboxes and signing differ.

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
calls. MP3 and ADTS frame counts stop at 512 MiB and cross up to 64 KiB of
damage, and junk at the end of the file is ignored. Past either limit the
duration stays empty.

## Build and tests

The helper needs only the C++ standard library. CMake builds and installs it
with either core target, to `${CMAKE_INSTALL_LIBEXECDIR}/amule` (`bin` on
Windows); `-DENABLE_MEDIA_PROBE=OFF` omits it. Every bundle builds it from the
aMule sources, so no third-party sources, pins or licence notices ship with it.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build -R MediaProbe --output-on-failure
```

- `test_helper.py`: output for generated WAV fixtures (rounding, Unicode and
  invalid tags, framing), plus codec cases whose fixtures ffmpeg creates when
  it is installed.
- `test_hostile.py`: random data after every signature, impossible sizes and deep
  nesting, each of which must get a clean verdict, and layouts real encoders
  write, which must give the values FFmpeg reports.
- `MediaProbeParseTest` and `MediaProbeEnvTest`: the core's protocol parser and
  process runner (timeouts, cancellation, crashes, output limits). The runner
  tests that use shell scripts run on POSIX only.

The libFuzzer harness covers every parser through the format dispatch:

```sh
cmake -S src/mediaprobe -B /tmp/fuzz -DCMAKE_CXX_COMPILER=clang++ -DMEDIA_PROBE_FUZZ=ON
cmake --build /tmp/fuzz --target amule-media-probe-fuzz
/tmp/fuzz/amule-media-probe-fuzz /tmp/fuzz-corpus -max_len=262144
```

### Pre-release fuzzing and saved corpus

Run **Media probe pre-release fuzzing** manually before a release and after
parser changes. Its default is 600 seconds with Clang, libFuzzer, ASan and UBSan.

Two workers run in parallel with per-input timeout, memory and length bounds;
worker failures fail the job. The workflow always attempts to retain logs,
source/toolchain/corpus provenance, updated inputs and crash reproducers.
Sanitizer fuzzing intentionally calls the in-memory parser harness without the
production sandbox: it does not verify helper startup or OS containment.

Each run generates synthetic seeds from the helper and hostile-input fixtures.
Fuzzing starts from fresh seeds each time. After minimization, the evolving
corpus is archived with the run artifacts. Downloaded archives contain regular
SHA-256-named files; imports reject links, traversal, checksum errors and
excessive sizes/counts.
Logs, updated archives and crash reproducers are also retained as artifacts for
90 days. Preserve confirmed crash inputs as small, focused regression fixtures;
do not commit the evolving corpus to Git.

To recreate initial seeds from generated fixtures (ffmpeg extends the seeds when
installed), then minimize and archive a completed run:

```sh
python3 src/mediaprobe/corpus.py seed build/src/amule-media-probe /tmp/probe-corpus
# Stop all fuzz workers before minimizing or archiving their corpus.
mkdir /tmp/probe-minimized
build-fuzz/amule-media-probe-fuzz -merge=1 /tmp/probe-minimized /tmp/probe-corpus
python3 src/mediaprobe/corpus.py pack /tmp/probe-minimized /tmp/probe-corpus.tar.gz
# Import into a fresh destination:
python3 src/mediaprobe/corpus.py unpack /tmp/probe-corpus.tar.gz /tmp/probe-restored
```

A high line-coverage percentage is not proof of safety. Keep the larger-file,
long-loop and truncation regression tests; the 256 KiB fuzz input bound cannot
exercise every production file size or offset. Each workflow artifact records
the tested source, toolchain and seed checksum.

Durations can differ from FFmpeg's by a fraction of a second where containers
count encoder padding differently.

## Packaged runtime validation

`packaging/media/test-runtime.py` checks the packaged helper version,
inherited-file input, duration/bitrate/codec, Unicode tags, exact protocol
framing and rejection of empty/malformed media. It then launches the
packaged daemon with isolated configuration and verifies persisted metadata and
failure marker 2. AppImage tests enter through the extracted AppRun; Flatpak
tests execute inside the installed sandbox. Windows tests use a relocated
extraction of the final zip, and macOS tests use the mounted dmg.

The Packaging workflow runs these checks against the bundle produced by each
build job. Flatpak clones the exact local HEAD commit rather than upstream
master. Final PR validation uses one full workflow dispatch with no `only`
filter, so every build and runtime check covers the same commit. For the
Universal2 dmg, both slices are exercised on macOS (Intel via Rosetta).
