# Download write-buffer limits

Download block selection reserves the full uncompressed range before a block
can enter a request packet. The fixed safety limits are 64 MiB globally and
16 MiB per filesystem volume, shared across files and sources. The existing
file-buffer preference still controls normal flush batching. At 75% pressure,
small buffers on the affected volume flush early; global pressure flushes all
volumes. Unknown identities conservatively count against possible aliases.
Early flushing remains active until usage falls to 50%, avoiding threshold
oscillation while leaving admission caps unchanged.

The request and its buffered fragments share ownership of one reservation.
Disconnecting or cancelling releases unsatisfied requests; queued data keeps its
credit until its last buffer is harvested or discarded. Write errors retain
both the data and its reservation for retry. Overlapping duplicate packets only
buffer the missing ranges. Compression does not reduce the reserved size.

Volume identity belongs to the actual data file, including file symlinks into
another filesystem. POSIX reuses the descriptor identity from the existing
open/create length query; reading that cache adds no filesystem call. Otherwise
bounded background discovery runs before reserving network data. Unknown files
share conservative capacity until discovery succeeds. Identity and free-space
paths are retained for the file session; replacing/remounting an open backing
file is not dynamically reclassified.

Four workers run at most one batch per resolved volume. A batch contains up to
64 buffers. Admission allows 4,096 data jobs globally and 1,024 per volume, plus
64 separate discovery jobs; at most two workers perform discovery concurrently.
These independent discovery allowances and oldest-eligible scheduling let a
cold healthy file identify its volume even while resolved volumes replenish
their write queues. Discovery keeps its two-call concurrency limit. Queue saturation
leaves buffers ready for retry without synchronous I/O on the main thread.
Free-space samples and writes run within their volume's batch, so a slow probe
cannot hold a shared coordinator lock or another volume's write lane.

Cancellation revokes the file session and strips queued ownership. Active
provider calls keep the file pinned until they return; result publication
precedes the last pin release. Closing/deleting an active file and shutdown
still wait for its kernel calls. Workers are joined after draining accepted
ownership and are never detached. More blocked operations than the bounded pool
can cover still require external recovery; unresolved paths cannot promise the
same isolation as volumes with known identities.

When credit is exhausted, sources remain in the downloading state and already
requested responses remain admissible. Empty pipelines retry on download ticks
after writes drain; disk pressure does not trigger the no-needed-parts takeover.

These limits bound admitted payload, including network responses still owed.
They do not bound total process RSS, packet/decompression scratch space, buffer
metadata, mapped pages, or the operating system's page cache. This stage adds
write-worker isolation; it does not add write coalescing or a read cache.
