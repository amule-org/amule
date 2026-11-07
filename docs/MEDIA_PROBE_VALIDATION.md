# Media probe restriction and fuzz validation

Downloaded media is parsed only after OS restrictions are installed. Failed
setup, rejected input, parser crashes, timeouts and oversized replies produce
no metadata. Failure diagnostics are bounded and retained separately from the
metadata protocol.

## Native restrictions

The [native workflow](https://github.com/3togo/amule-upstream/actions/runs/42389911324)
passed all five CTest groups on eight native build/test jobs at
`e933941cae892445e09ac2aa17e4a10951034e3d`:

- Ubuntu 24.04.5 LTS, x86-64 and arm64 (GCC 13.3.0).
- Alpine Linux 3.20.10, x86-64 and arm64 (GCC 13.2.1, statically linked musl).
- macOS 15.7.9 Intel and 15.8.2 Apple Silicon (Apple Clang 17).
- Windows Server 2025 Datacenter 10.0.26100 x64 and Windows 11 Enterprise
  10.0.26200 ARM64 (MSVC 19.51.36260).

The groups are `MediaProbeStartup`, `MediaProbeSandbox`, `MediaProbeCorpus`,
`MediaProbeHelper` and `MediaProbeHostile`. They check useful extraction under
the production policy and actual denials of unrelated file access, file writes,
network connections/listening and process creation. POSIX checks additionally
cover exec, inherited descriptors and Unix sockets; Linux covers alternate
`openat`. OS-specific cases are explicitly skipped on other systems. Codec
fixtures needing FFmpeg or optional encoders run only where those are available.
All eight native jobs skipped the two FFmpeg-dependent helper methods; the
additional Ubuntu 26.10 validation below includes generated codec fixtures.
The sandbox suite reports two platform skips on Linux and three on Windows/macOS;
the three corpus tests and 23 hostile-input methods run on every target.

Windows checks also verify that a caller's writable input handle is reduced to
read-only, pipe input is rejected, output handles cannot resize files, and
excessive metadata/diagnostics are stopped by the trusted launcher's byte limits.
The restricted primary token, untrusted parsing integrity, private desktop,
Job limits and explicit handle list remain active. Detached startup fixes the
`KERNELBASE.dll` initialization failure without granting console access.

## Additional Linux checks

A clean detached checkout on Ubuntu 26.10 (Stonking Stingray), x86-64, with
GCC 15.3.0, Python 3.14.8 and CMake 4.4.4 rebuilt the final code revision
`8920d2361094bd64049bd7f63ec9ca9e4383cf75` and passed all five standalone CTest
groups in 2.36 seconds. This includes the new archive-decompression guard and
FFmpeg-generated codec fixtures. Expected Windows-only sandbox methods are
skipped on Linux. The later three commits change only the CI workflow; production
C++ and regression test sources are identical. The built helper's SHA-256 is
`6c7dedb79cb06ab368aeffdbc68176cd9a5a8488674291adf1ee6ebb5987829a`.

A private control using the preceding policy reproduced `EPERM` for both
`readv(stdin)` and `writev(stdout)`. The final sandbox tests require those calls
to work while rejecting vectored reads from output descriptors and writes to
stdin. This retains the same file capabilities while supporting musl's I/O ABI.

The same environment replayed the saved 2,212-input corpus through the parser
harness compiled with GCC ASan and UBSan, with no findings. The harness was built
at `6aa9d2fddd2446a97f31fce59a73199b3e797966`; its parser and harness source files
are unchanged in the final code revision. Its SHA-256 is
`dd70a138a8bded8f3bcd5b45a19bbbe0d17db58d6ea262a54c2b71d9b007fabc`.

A second Ubuntu 26.10 environment with Clang 18.1.8, CMake 4.3.4 and wxWidgets
passed core `MediaProbeEnvTest` and `MediaProbeParseTest` during development.
These cover native crashes before/after the header, setup failures, partial
replies, oversized metadata/diagnostics and a held helper. Failed calls leave
output metadata untouched, and timeout returns without waiting for the helper's
full sleep. Final broader build and packaged-runtime results are separate checks.

Clang-format 18 passed complete changed C++ files. Clang-tidy 18 passed the
affected Linux translation units with the baseline and changed-line
configurations against upstream base
`73d5fae3d5d55ef54d62dcf51ff748da1029818d`. Only LLVM 18's unsupported
`ExcludeHeaderFilterRegex` key was removed in external configuration copies;
repository configurations are intact. Linux analysis cannot inspect code inside
Windows guards; native MSVC compilation and execution cover that implementation.

## Corpus and fuzzing

The seed uses generated repository fixtures and synthetic FFmpeg audio/video,
plus earlier exploratory mutations. No personal media or externally supplied
corpus was imported. Minimization produced a durable archive of **2,212 inputs**,
18,976,211 uncompressed bytes and 2,300,674 compressed bytes. SHA-256:
`fb5112f5ad291e466e9116889b935bd33bed0252c6281b091e93c238260fb573`.

The [ten-minute workflow run](https://github.com/3togo/amule-upstream/actions/runs/42388038432)
at `19cb303d1b481c9bcb2dd9ed87c15286ab904a2f` passed on Ubuntu 24.04.5 LTS,
x86-64, with Clang 18.1.3, libFuzzer, ASan and UBSan. Its two workers completed
**6,069,169 executions**, with peak RSS of 472 MiB and 470 MiB respectively and
no sanitizer findings or crash reproducers. It exercised the bounded corpus
importer and retained the updated minimized corpus, worker logs, merge log and
source/toolchain/corpus provenance as
[artifact 13580163279](https://github.com/3togo/amule-upstream/actions/runs/42388038432/artifacts/13580163279).
Parser and harness sources are unchanged by the subsequent static-library I/O
compatibility fix. The earlier run at `930cc8e4` also passed 4,810,122 executions;
that run is separate evidence, not added to the final run's execution count.

The parser fuzzer exercises the in-memory dispatcher. It does not exercise OS
policy setup, process launching, disk I/O or the metadata protocol. A short run
or high line coverage is not evidence that every large-file or adversarial
layout is safe.

## Packaged runtime checks and limits

A full [Packaging run](https://github.com/3togo/amule-upstream/actions/runs/42388035188)
was dispatched without an `only` filter at
`19cb303d1b481c9bcb2dd9ed87c15286ab904a2f`. Windows x64/ARM64 portable archives
and installers, macOS arm64, and both AppImage architectures passed. The static
Linux runtime checks exposed musl's vectored I/O requirement; the subsequent fix
passed the standalone static tests on both architectures. The remaining older
bundle jobs and the focused static package repeat are still in progress. A
running job is not a pass; the final whole-bundle repeat is separate acceptance
coverage.

See `MEDIA_PROBE.md` for the deprecated macOS sandbox API, lack of a hard macOS
RSS cap, Windows filesystem-ACL limitations and unsupported Linux ABIs. Native
success on the tested releases does not establish behavior on every older OS,
misconfigured filesystem or future kernel. Policy installation failures disable
extraction instead of falling back to an unrestricted parser.
