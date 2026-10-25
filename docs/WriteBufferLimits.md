# Download write-buffer backpressure

Only received, unwritten payload counts toward the 16 MiB budget. Every part
file lives in the one temp directory, so a single budget covers them all.
Selecting or sending block requests consumes no credit.
This leaves the normal source pipeline available regardless of source count
or round-trip time when storage keeps up.

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
