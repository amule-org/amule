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

The write worker identifies volumes from the actual directory's device identity
(or Windows volume serial), including directory aliases. Discovery never runs
on the main thread. Unknown directories share one conservative allowance, which
also counts against each known volume until discovery succeeds. A volume change
with live reservations keeps the old identity until that ownership drains.

When credit is exhausted, sources remain in the downloading state and already
requested responses remain admissible. Empty pipelines retry on download ticks
after writes drain; disk pressure does not trigger the no-needed-parts takeover.

These limits bound admitted payload, including network responses still owed.
They do not bound total process RSS, packet/decompression scratch space, buffer
metadata, mapped pages, or the operating system's page cache. The existing single
write worker still serializes volumes; this change does not isolate a healthy
volume from a blocked write or add a read cache.
