# Upload disk baseline — issue #1586

This is temporary measurement instrumentation, before any prefetch implementation.
PR #1949 stays a draft for collecting evidence; the profiler is not proposed for
merging into the upload path. A subsequent prefetch PR must contain the optimization
and measured results, without this instrumentation. Close this draft when that PR
is open, or report the measurements on #1586 if reads prove insignificant.
No CSV is opened and no per-read clock is sampled unless `AMULE_UPLOAD_IO_PROFILE`
is set to a nonempty output path. The file is overwritten at startup. An
allocation, sample-limit or output failure logs one message and disables
profiling without stopping uploads. Such captures lack a completion marker and
are rejected by the summary tool.
Captures start with `amule-upload-io,2` followed by the CSV column header.
Version 2 measures complete-file open latency; the analyzer rejects unversioned
and unknown-version captures so older timing semantics cannot enter an A/B result.
CSV numbers use the classic locale so regional grouping cannot corrupt fields.
Only the upload disk thread writes the CSV. No peer identifiers or file paths are recorded.

## First screening run

1. Configure and build the instrumented application:

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_MMAP=ON
   cmake --build build --target amule -j "$(($(nproc)-1))"
   ```

   Start with **MMapEnabled** off and keep it fixed throughout the run. Current upstream also calls `CFileArea::Prefault()`
   inside the measured read path, so separate mmap-enabled runs include page
   faults. The summary script rejects a run that mixes mmap modes. Record the
   build options along with the runtime setting.
2. On a machine with known HDD storage, prepare many active upload slots serving
   different shared files. Record the commit/local patch, filesystem, storage,
   slot count, upload cap, dataset size, RAM and download activity.
3. Start device monitoring in a separate terminal:

   ```sh
   iostat -x -y 1 > /tmp/upload-iostat.txt
   ```

4. Launch the application with an absolute output path on a different device or
   tmpfs where possible, so profiling writes do not compete with shared-file reads:

   ```sh
   AMULE_UPLOAD_IO_PROFILE=/dev/shm/upload-cold.csv ./build/src/amule
   ```

5. Collect a fixed observation window (for example 5 minutes once slots are busy),
   note aMule's sustained upload rate, and stop aMule normally. Summarize:

   ```sh
   python3 experiments/upload-io/summarize.py /dev/shm/upload-cold.csv
   ```

Repeat under warm cache with the same settings and workload. Start measurement
runs with slots already arranged where practical: startup and waiting for peers
count toward wall time and can depress rates. `bytes_per_second` is disk-thread
payload read throughput, **not** actual network upload throughput.

For cold cache, use a dedicated Linux test machine. After preparing the data,
`sync` followed by `echo 3 | sudo tee /proc/sys/vm/drop_caches` drops system caches;
this affects other workloads on that machine. Record precisely when you do it.
Do not interpret a small cached dataset as a cold disk baseline.

## What is measured

Each attempted read records steady-clock nanoseconds. The timer starts immediately
before `CPartFile::ReadData()` (including its mutex acquisition), or before the
complete-file `file.Open()`. It ends after `CFileArea::Prefault()` and
`CFileArea::CheckError()`. Complete-file open and close time is included, so the
cost removed by cached handles is visible, including on network mounts. Open
failures now count as failed attempts; validation failures before `ReadBlock()`
are not reads.
Returns or exceptions after timing starts count as failed/deferred attempts,
separate from successful latency percentiles and bytes. Their time is included
in the read share.

Each disk-thread iteration records active time from before the upload-list lock
through completion processing and throttler notification, ending before CSV
serialization and the condition-variable wait. Thus idle sleep and CSV writes
are excluded from active time; mutex waits, packet building and compression are
included. On current upstream, disk reads run without the upload-list and
block-list locks; part-file reads still acquire the part-file handle mutex.
Successful latency percentiles (p50/p90/p95/p99) use the nearest-rank definition. Read and byte
rates count successful reads; failed/deferred attempts are reported separately. Rates
use wall time from profiler startup through explicit capture completion, including
sleep and CSV writes. Read rows are buffered until that loop ends and share its
end timestamp. The CSV is flushed once per loop. Normal worker shutdown writes an `end` record
and flushes it; stop normally before analyzing. A missing footer, incomplete row,
inconsistent timestamp, impossible duration or mixed mmap mode invalidates the
capture instead of producing a potentially misleading baseline. Captures from the
original draft, which lack an `end` record, must be collected again. Captures
from before the open-timer correction also need to be recollected for A/B
comparisons: their durations omit open latency and cannot be corrected afterward.
All unversioned captures, including ones with completion markers, are rejected.
The analyzer also rejects non-decimal numeric fields and loops exceeding the
collector's 4096-sample bound.

Instrumentation still has clock, sample-storage and flush overhead. It is a
screening tool, not a zero-overhead benchmark. CSV grows with the number of reads;
watch tmpfs/container memory use and copy results out after each run. The worker
preallocates space for at most 4096 read samples per loop (about 100 KiB), performs
no allocations in read timers, and disables profiling if that limit is exceeded.
An overflow invalidates the whole capture, rather than dropping samples and
biasing the percentiles. Reduce the workload per loop before repeating that run.

## Compare master and #1953 first

[#1953](https://github.com/amule-org/amule/pull/1953) keeps complete-file read
handles open, enabling the kernel to maintain readahead state between blocks.
Independently verify that change before attributing any gain to explicit prefetch.
Use the same upstream base for both arms; record its SHA, the #1953 head SHA,
and this instrumentation's SHA. If #1953 rebases, recreate both arms and rerun.
Do not compare binaries built from different master revisions.

Prepare two isolated worktrees from the same master revision. Apply only the
profiling commits from this branch to the first (master + profiler). For the
second, apply #1953 to that same revision, then the same profiling commits
(#1953 + profiler). Both edit `ReadBlock()`; resolve its complete-file conflict
using #1953's cached handle and this timer placement:

```cpp
OpenFile_Struct *openFile = req->pFileStruct;
readTimer.Start();
if (!openFile->file.IsOpened() &&
    !openFile->file.Open(req->path, CFile::read)) {
    result = READ_OPEN_FAILED;
} else {
    req->area.ReadAt(openFile->file, req->uStartOffset, togo);
}
```

Keep the timer before the lazy-open check, and preserve the shared
`Prefault()` / `CheckError()` / `readTimer.Finish()` sequence afterward. Keep
#1953's handle ownership, pin/release and eviction code intact. A cache hit must
still produce a read sample; a failed open must produce a failed sample.
Build both arms with identical options and run the collector and upload tests.

Run the two binaries serially on the same storage, files and peer request trace.
Use several peers and include sequential complete-file requests (to exercise
readahead), scattered complete-file requests, and part-file requests. Keep those
workloads in separate captures so an aggregate does not hide a regression.
Reset the cache independently before each cold arm; do not let the first arm
warm the second. Repeat warm arms with the same warm-up, duration and upload cap,
and alternate arm order across repetitions. Retain each CSV, summary, iostat log
and actual network upload rate with its settings and commit IDs.

## Decision and subsequent validation

Record one row per arm, storage/cache state, workload and repetition:

| Arm | Storage/cache | Workload | p50/p90/p95/p99 ms | Reads/s | Read MiB/s | Read % active | Upload MiB/s | r_await | aqu-sz | %util |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Master + profiler | HDD cold | sequential complete | pending | | | | | | | |
| #1953 + profiler | HDD cold | sequential complete | pending | | | | | | | |
| Master + profiler | HDD warm | sequential complete | pending | | | | | | | |
| #1953 + profiler | HDD warm | sequential complete | pending | | | | | | | |

Repeat on a RAID array when available and on SSD as a regression check, with
and without concurrent downloads and in a memory-limited container, as required
by #1586. Mark unavailable storage as unmeasured. Do not infer HDD/RAID gains
from loopback or SSD results. Published #1953 results are motivation for this
comparison, not independent measurements from this branch.

The #1953 arm becomes the baseline for the prefetch decision. If read stalls are
small relative to active work, stop and report that evidence on #1586. If reads
remain significant and upload delivery is disk-limited, test a capped prototype
using `posix_fadvise(POSIX_FADV_WILLNEED)` on Linux and `F_RDADVISE` on macOS for
blocks already queued but not yet read. Focus on part files, cold tail latency
and scattered requests that automatic readahead leaves behind. A high read share
alone is insufficient: a network upload cap can mask any disk benefit. No
universal numeric cutoff is assumed.

Repeat the same matrix with #1953 + profiler + prefetch, changing only prefetch
between those arms. Include the hint's cost in active-loop time. Require lower
tail latency and improved sustained HDD/RAID upload delivery without SSD
regressions before opening a prefetch PR. Keep profiling on this branch or as a
local patch; do not include it in that optimization PR.

No representative baseline or prefetch results are included yet. The local
review environment exposes non-rotating NVMe/flash storage and cannot supply
HDD/RAID evidence. The matrix remains pending until representative storage and
a controlled many-peer workload are available.

Check the instrumentation and summary tool without a running aMule:

```sh
python3 experiments/upload-io/test_profile.py
```

With `BUILD_TESTING=ON`, native GNU/Clang builds also register this regression
suite as `UploadIOProfileTest` in CTest. Cross-compiled captures can be analyzed
on the host; the compiled collector harness requires a native executable.
