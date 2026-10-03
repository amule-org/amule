# ED2K extension conventions: OFFERFILES v1 draft

This client draft recognizes and validates the agreed OFFERFILES advertisement.
**Accelerated publication is not enabled.** Publication continues to use the
existing legacy batch heuristic (at most 200 records) and one-minute interval.
The validated policy is a socket-owned diagnostic snapshot, not a persisted
server property. It is not wired into the publication scheduler yet.

The contract was confirmed by the companion server maintainer in
[ed2k-server #19](https://github.com/andrey23127/ed2k-server/issues/19#issuecomment-5930455238).
The implementation request and reusable-conventions discussion are in
[aMule #1699](https://github.com/amule-org/amule/issues/1699).

## Reusable conventions

- Carry an extension in an existing packet that legacy clients can parse.
  Unknown tags must remain ignorable; do not infer support from identity,
  software strings, or large limits.
- Use explicitly versioned string names rather than inventing numeric IDs.
  Reject unsupported versions; do not guess their semantics.
- Validate the entire advertisement before committing any extension state.
  Missing, duplicate, wrong-type, zero, or inconsistent required fields prevent
  use of the extension. Truncated packets never commit a partial policy.
- Keep capability state on the live connection and clear it on disconnect or
  server change. A repeated advertisement invalidates this v1 snapshot; dynamic
  renegotiation would require its own specified protocol.
- Apply local resource ceilings in addition to server limits. Avoid bursts,
  catch-up loops, and arithmetic overflow. Use monotonic time for scheduling.
- Without acknowledgements, a client can count candidates offered, not files
  accepted by the indexer. A compliant packet must not be silently discarded
  merely because the server is busy; the agreed server behavior is backpressure.

## Advertisement

One post-login `OP_SERVERIDENT` packet carries each field exactly once:

| Field | Encoding | Validation |
|---|---|---|
| `offerfiles_v` | string-named uint32 tag | exactly 1 |
| `offerfiles_batch_max` | string-named uint32 tag | greater than 0 |
| `offerfiles_min_interval_ms` | string-named uint32 tag | greater than 0 |
| `ST_SOFTFILES` | existing numeric uint32 tag | greater than 0 |
| `ST_HARDFILES` | existing numeric uint32 tag | greater than advertised batch |

The draft requires the uint32 tag encoding, not uint8/uint16/uint64 substitutes.
String field names are case-sensitive. The soft limit is a per-connection
indexing budget, while the hard value is a per-packet boundary. It is not a
global server capacity. To accommodate historical plain/compressed differences,
every negotiated batch must be strictly smaller than the hard value.

The policy calculation bounds a future batch by the remaining soft candidate
budget, advertised batch, hard minus one, and local cap of 200. The future
interval is at least the advertised interval and 500 ms. This gives at most
400 records/second without a burst entitlement. These are experimental local
ceilings, not a production load recommendation.

## Remaining before enabling acceleration

Wire the validated snapshot into a monotonic, sub-second scheduler; count
actual distinct candidate records sent throughout the connection, including
legacy batches sent before SERVERIDENT. Reset that accounting with the socket.
Update live soft/hard limits consistently without persisting pacing capability
state. Add detailed fallback and budget-exhaustion diagnostics.

Run interoperability tests against the companion server implementation,
including coalesced TCP frames, disconnects, overload backpressure, concurrent
publishers, and reconnect waves. The companion capability was agreed but not
implemented at the time of this draft. Acknowledgements and dynamic policy
updates remain separate extensions.
