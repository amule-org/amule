# ED2K extension conventions: OFFERFILES v1

Accelerated publication is experimental and **disabled by default**. Normal users
are discouraged from testing it: controlled loopback interoperability is tested,
but production server-load validation remains incomplete and accelerated offers
may cause server disconnections.

For controlled testing with a compatible server, enable **Advanced → Experimental
→ Enable experimental ED2K accelerated file publication**. Daemon users can set
`ExperimentalED2KPublication=1` in the `[eMule]` section of `amule.conf`; the remote
GUI does not configure this option. Resetting Advanced preferences disables it.

When enabled, the client requests v1 in its login and permits acceleration only
after a valid v1 advertisement on the current connection. Missing, malformed or
repeated advertisements retain legacy pacing. Disabling the setting before
login omits the request and uses the existing batch heuristic (at most 200
records) and one-minute pacing. If disabled during a negotiated session, the
server still enforces that session's snapshot: aMule keeps its batch and budget
limits and slows to at least one-minute pacing until reconnect. No server
identity or manual rate override enables acceleration.

Negotiated publication runs on each monotonic core timer tick (100 ms in the GUI and
300 ms in the daemon), separately from the one-second shared-file maintenance
loop. It sends at most one batch per eligible tick, measuring the next interval
from socket queue acceptance; late ticks do not grant catch-up bursts. Legacy
publication keeps the existing shared-file maintenance schedule.
Distinct candidate hashes are counted only after socket queue acceptance,
including legacy offers before negotiation, and reset on disconnect. Queue
acceptance does not confirm transmission or indexing. Already offered hashes are
skipped during accelerated publication; the soft budget is not replenished by
removing local files. Live nonzero soft/hard limits can further restrict the
advertised snapshot. Negotiated pacing is never persisted in `server.met`.

The contract was confirmed and later amended by the companion server maintainer in
[ed2k-server #19](https://github.com/andrey23127/ed2k-server/issues/19#issuecomment-6096942323).
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

## Request and advertisement

With the experimental setting enabled, `OP_LOGINREQUEST` contains one additional
tag, `offerfiles_v = 1`, after the four standard login tags. It is a string-named
uint32 tag: type `0x03`, a uint16 name length, the case-sensitive name, and a
uint32 value. The login tag count is five. With the setting disabled, the tag
count remains four and the request is absent. A server that does not understand
this tag ignores it. A v1 server advertises and applies its policy only to a
client that asked; absence of an advertisement keeps this client at legacy pace.

One post-login `OP_SERVERIDENT` packet carries each field exactly once:

| Field | Encoding | Validation |
|---|---|---|
| `offerfiles_v` | string-named uint32 tag | exactly 1 |
| `offerfiles_batch_max` | string-named uint32 tag | greater than 0 |
| `offerfiles_min_interval_ms` | string-named uint32 tag | greater than 0 |
| `ST_SOFTFILES` | existing numeric uint32 tag | greater than 0 |
| `ST_HARDFILES` | existing numeric uint32 tag | greater than advertised batch |

Version 1 requires the uint32 tag encoding, not uint8/uint16/uint64 substitutes.
String field names are case-sensitive. The soft limit is a per-connection
indexing budget, while the hard value is a per-packet boundary. It is not a
global server capacity. To accommodate historical plain/compressed differences,
every negotiated batch must be strictly smaller than the hard value.

The policy calculation bounds a batch by the remaining soft candidate
budget, advertised batch, hard minus one, and local cap of 200. The
interval is at least the advertised interval and 500 ms. This gives at most
400 records/second without a burst entitlement. These are experimental local
ceilings, not a production load recommendation.

## Server verification and remaining validation

Controlled interoperability tests use the companion server implementation,
including coalesced TCP frames, disconnects, overload backpressure, concurrent
publishers, and reconnect waves. The published companion implementation is
[ed2k-server 0.9.80 source](https://github.com/andrey23127/ed2k-server/tree/ea88f175f12765d2afb4734c6b3a5924bdb7b37a),
commit `ea88f175f12765d2afb4734c6b3a5924bdb7b37a`.
Its source includes the opt-in request, advertisement, snapshot, pacing and
overload acceptance tests from issue #19.
This opt-in and controlled local measurements do not establish production
readiness. Acknowledgements, automatic retries after indexing
failures, and dynamic policy renegotiation remain separate extensions.

## Reproducing local interoperability

Build the companion server from the tag above using its unmodified lockfile:

```sh
cargo +1.90.0 build --locked
cargo +1.90.0 test --locked --test integration offerfiles_v1
```

The published lockfile's ICU dependencies require Rust 1.88 or newer even though
the server package declares an older minimum. Rust 1.90 was used for these
checks; no dependency downgrade or server-source patch is needed.

Run the actual aMule daemon against the actual server binary:

```sh
python3 unittests/tests/OfferFilesInteropTest.py \
  /absolute/path/to/amuled /absolute/path/to/ed2k-server \
  --large-library 60000 --output /tmp/offerfiles-results.json
```

The runner creates only temporary loopback servers and libraries, with private
server mode, empty seed lists, disabled updates and Kad, and isolated aMule
configuration directories. It requires Python 3 and a system libcrypto with the
MD4 function to generate real content hashes for its `known.met` fixtures.
Fixtures are real 64-byte files with unique hashes and timestamps; their seeded
metadata excludes hashing throughput from these publication measurements.
A local relay observes actual OFFERFILES frames and can remove the compression
flag or cluster two frames. Assertions use the server's independent indexed
source counts and publication counters, not the client's published markers.

Cases cover plain and compressed offers, clustered arrivals, soft candidate
budget, reconnect reset, disabled server/client settings, invalid advertisement
configuration, global backpressure, and four concurrent publishers. The optional
large-library case measures 60,000 files. All cases retain the local 200-record
cap and strict per-packet hard boundary.

The reconnect-wave case starts four fresh sessions together, then disconnects
and reconnects all four to verify budget reset under the global ceiling.
Large libraries receive a longer startup allowance because loading the fixture
precedes the publication measurements.

For the smaller suite through CTest, configure with
`-DENABLE_INTEGRATION_TESTS=ON -DOFFERFILES_INTEROP_SERVER=/absolute/path/to/ed2k-server`
and run `ctest --test-dir build --output-on-failure -R OfferFilesInteropTest`.
The external test is not registered without that explicit binary path.
