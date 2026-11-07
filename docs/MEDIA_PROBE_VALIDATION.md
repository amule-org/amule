# Media probe restriction and fuzz validation

The implementation is based on upstream `73d5fae3d5d55ef54d62dcf51ff748da1029818d`.
Results below describe the candidate changes for issue #2412. Native workflow
results must be matched to their exact commit before treating a platform as
verified.

## Linux validation

Two independently inspected Ubuntu 26.10 (Stonking Stingray), x86-64
environments were used. The requested validation environment has GCC 15.3.0,
Python 3.14.8 and CMake 4.4.4. A separate environment provides Clang 18.1.8,
CMake 4.3.4 and the wxWidgets dependencies for the core tests. Results from one
machine do not establish behavior on another.

- Standalone helper build passed.
- `MediaProbeSandbox` passed: input read/seek and allocation work; unrelated
  file reads/writes, file creation, networking, process creation, exec, inherited
  descriptors and alternate `openat` are denied. Writable/nonseekable stdin is
  refused without a protocol header or metadata.
- `MediaProbeHelper` and `MediaProbeHostile` passed under the restrictions,
  including generated codec fixtures when FFmpeg is available.
- `MediaProbeCorpus` passed archive round-trip and malicious-entry checks.
- The requested validation environment replayed the complete saved corpus
  through the parser harness compiled with GCC ASan and UBSan, with no findings.
- Clang-format 18 passed complete changed C++ files. Clang-tidy 18 passed the
  affected Linux translation units with the baseline and changed-line
  configurations. Only LLVM 18's unsupported `ExcludeHeaderFilterRegex` key was
  removed in external configuration copies; repository configurations are intact.
- Core `MediaProbeEnvTest` and `MediaProbeParseTest` passed, including native
  crashes before/after the header, setup failures, partial replies, excessive
  metadata/diagnostics and a held helper. Failed calls leave output metadata
  untouched, and timeout returns without waiting for the helper's full sleep.

## Corpus

The initial seed was generated from `test_helper.py` and `test_hostile.py`, using
the repository's synthetic headers and FFmpeg-generated synthetic audio/video.
The initial seed contained 1,017 distinct inputs, 5,485,024 uncompressed bytes.
An exploratory 600-second, eight-worker Clang/libFuzzer/ASan/UBSan run completed
12,354,447 executions with exit status 0 and no crash reproducers. That run used
the upstream parser sources before the diagnostic-only changes in this PR;
it is not presented as fuzz validation of the later source. Its logs and binary
identity were retained separately.

The saved union of those inputs and mutations was minimized with the updated
parser harness. The durable archive contains **2,212 inputs**, 18,976,211
uncompressed bytes, and is 2,300,674 bytes compressed. Its SHA-256 is
`fb5112f5ad291e466e9116889b935bd33bed0252c6281b091e93c238260fb573`.
The requested validation environment replayed all 2,212 inputs under GCC ASan
and UBSan without findings. No personal media or externally supplied corpus was
imported. The manual workflow separately validates the final committed source.

## Coverage limits

Windows and macOS checks are defined in `.github/workflows/media-probe.yml`;
they have not yet been verified for this candidate. Neither Linux execution nor
cross-compilation establishes native enforcement there. The complete aMule
Windows/macOS process-runner tests and final packaged-runtime tests remain
separate acceptance checks. See `MEDIA_PROBE.md` for deprecated macOS API,
macOS memory-limit, Windows filesystem-ACL and unsupported Linux ABI limits.

The parser fuzzer exercises the in-memory dispatcher. It does not exercise OS
policy setup, process launching, disk I/O or the metadata protocol. A short run
or high line coverage is not evidence that every large-file or adversarial
layout is safe.
