# Download write-buffer backpressure

Only received, unwritten payload counts toward the write budget (128 MiB by
default). Every part file lives in the one temp directory, so a single budget
covers them all. Selecting or sending block requests consumes no credit. This
leaves the normal source pipeline available regardless of source count or
round-trip time when storage keeps up.

`/eMule/WriteBufferLimitMiB` sets the budget from 1 to 1024 MiB. It is also
available as **Download write buffer limit (MiB)** in the advanced preferences
of amule and amulegui, and as `advanced.write_buffer_limit_mebibytes` in
amuleapi and its web UI. Changes take effect immediately, including for buffered
data; lowering the limit does not discard that data. The default applies to
existing profiles without this new setting. Configuration and EC values are
clamped to the supported range; the HTTP API rejects out-of-range values.

An older daemon without the setting disables the control in remote clients.
The API reports `advanced.write_buffer_limit_supported: false` and rejects an
attempt to set the limit with HTTP 409 (`option_not_supported`). The web UI omits
the unsupported setting when saving other preferences.

The larger default leaves room for short write stalls, including bursts of part
hashing on fast LAN downloads. It is a tradeoff against memory use, not a promise
of higher throughput on every disk. Larger values cannot fix a disk that is
persistently slower than incoming data. On memory-constrained systems, select a
smaller value (16 MiB restores the previous threshold). No memory is reserved
up front.

Stop selecting new blocks at the budget and resume selection at 50%. Flush
small buffers early at 12.5%, leaving headroom for arrivals between main-thread
scheduling ticks. Reception queues early writes immediately, and the periodic
file tick also checks pressure. Successful writes return capacity immediately
in the worker. The normal file-buffer preference still controls batching
below these thresholds.

Already requested responses remain admissible, including compressed responses.
Each missing fragment is charged by its uncompressed size and releases its own
charge as soon as the worker successfully writes and frees its payload, or
when it is discarded. Duplicate overlap is not buffered again.
Write errors retain data and its charge for retry; disconnecting a source does
not release dirty data. Empty pipelines keep their upload slots and retry on
download ticks after storage drains.

The thresholds are not hard payload caps: outstanding responses can overshoot
them. During a complete stall, retained payload is bounded by the threshold
plus all outstanding requested ranges when selection stops (at most 24 blocks
of 180 KiB per downloading source). Slow sources can contribute their full
outstanding ranges, so this allowance is not necessarily rate times RTT.
Changing source count changes this bound. This does not promise a fixed total
RSS bound: packet/decompression scratch space, fragment metadata, mapped pages,
and the operating system's page cache are outside payload accounting.
