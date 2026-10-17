# ED2K extension conventions: OFFERFILES v1

Accelerated publication is experimental and **disabled by default**. Normal users
are discouraged from testing it: controlled loopback interoperability is tested,
but production server-load validation remains incomplete and accelerated offers
may cause server disconnections.

The extension is compiled only with `-DENABLE_ED2K_OFFERFILES_V1=ON`
(default OFF), or `-DENABLE_ALL_EXPERIMENTAL=ON`. Default builds omit the v1
policy, login opt-in, advertisement handling, and accelerated scheduler; they
also omit the checkbox and ignore any saved `ExperimentalED2KPublication` value.
Experimental CI builds and tests the extension through the shared switch list.
The policy unit tests also run with the switch off, compiling the policy only
into the test executable.

For controlled testing with a compatible server, first enable the build switch,
then enable **Advanced → Experimental
→ Enable experimental ED2K accelerated file publication**. Daemon users can set
`ExperimentalED2KPublication=1` in the `[eMule]` section of `amule.conf`; the remote
GUI does not configure this option. Resetting Advanced preferences disables it.

When enabled, the client requests v1 in its login and permits acceleration only
after a valid v1 advertisement on the current connection. Unsolicited capability
advertisements on a connection whose login did not request v1 are ignored.
A requested connection waits for the first SERVERIDENT before sizing its first
publication packet; if no identity arrives, legacy publication becomes eligible
after a one-minute grace period measured from the login request. The requested
identity may precede ID assignment in the welcome sequence; its policy remains
binding when login completes. A missing or malformed
first advertisement uses legacy pacing. A later advertisement is ignored and
cannot relax a valid first snapshot; the connection stays open. Disabling the
setting before login omits the request and uses the existing batch heuristic
(at most 200 records and strictly below any known hard boundary) and one-minute
pacing. Offline, empty, and rejected publication attempts do not advance the
legacy publication timer. If disabled during a negotiated
session, the server still enforces that session's snapshot: aMule keeps its
batch and budget limits and slows to at least one-minute pacing until reconnect. No server
identity or manual rate override enables acceleration.

Negotiated publication runs on each monotonic core timer tick (100 ms in the GUI and
300 ms in the daemon), separately from the one-second shared-file maintenance
loop. It sends at most one batch per eligible tick, measuring the next interval
from socket queue acceptance; late ticks do not grant catch-up bursts. Legacy
publication keeps the existing shared-file maintenance schedule.
Distinct candidate hashes are counted only after socket queue acceptance,
including legacy offers before negotiation on a connection that requested v1,
and reset on disconnect. Unrequested legacy connections do not retain a
distinct-hash set. Queue
acceptance does not confirm transmission or indexing. Already published files
are skipped until a rename or completion marks them
for republication. A re-offer of the same hash consumes packet capacity and
pacing time, but no new distinct-candidate slot, even after the soft budget is
full. Removing local files does not replenish that budget. Live nonzero
soft/hard limits can further restrict the
advertised snapshot. An idle publisher rechecks its library when live limits
change, so a temporarily restrictive limit cannot stall publication for the
rest of the connection. Unchanged limits do not trigger new scans.
Negotiated pacing is never persisted in `server.met`.

Republication refreshes source metadata; it does not guarantee that a server
replaces the searchable filename. On ed2k-server v0.9.82, a re-offer updates
the source's complete/partial status but retains the filename and keywords from
the first indexing. A renamed file becomes searchable under its new name only
after all sources leave the index and the file is offered again. This is
[that server's behavior](https://github.com/andrey23127/ed2k-server/issues/19#issuecomment-6154691399),
not an OFFERFILES v1 requirement. aMule requests ED2K republication after renaming
a complete shared file, but has no acknowledgement or search-name update check;
it updates its local name and Kad keywords independently.

The contract was confirmed and later amended by the companion server maintainer in
[ed2k-server #19](https://github.com/andrey23127/ed2k-server/issues/19#issuecomment-6096942323).
The [protocol clarification](https://github.com/andrey23127/ed2k-server/issues/19#issuecomment-6107202622)
distinguishes the client-side 200-record policy from the universal server hard boundary.
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
  server change. Ignore a repeated advertisement without changing the first
  snapshot; dynamic renegotiation would require its own specified protocol.
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
global server capacity. Every batch must remain strictly below the hard value,
regardless of packet encoding. Legacy clients may send more than 200 records in
a batch if they remain below that boundary; the negotiated `offerfiles_batch_max`
applies only after the client requests v1 and receives a valid advertisement.
The server accepts at most `offerfiles_batch_max` records in a v1 batch and
rejects an oversized v1 batch without disconnecting the client.

The policy calculation bounds every batch by the advertised batch, hard minus
one, and local cap of 200. Within that packet, only previously unseen hashes
consume the remaining soft candidate budget; refreshes may fill the other slots.
The interval is at least the advertised interval and 500 ms. This gives at most
400 records/second without a burst entitlement. These are experimental local
ceilings, not a production load recommendation.

## Server verification and remaining validation

Controlled interoperability tests use the companion server implementation,
including coalesced TCP frames, disconnects, overload backpressure, concurrent
publishers, and reconnect waves. The published companion implementation is
[ed2k-server 0.9.82 source](https://github.com/andrey23127/ed2k-server/tree/48496f4e95d5b601a1dd120d279b625c8b1fa1e7),
commit `48496f4e95d5b601a1dd120d279b625c8b1fa1e7`.
Its source includes the opt-in request, advertisement, snapshot, pacing and
overload acceptance tests from issue #19.
The server maintainer [confirmed that v1 is settled](https://github.com/andrey23127/ed2k-server/issues/19#issuecomment-6170221378);
incompatible changes require a new requested capability version.
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
  --large-library 60000 --queue-samples --output /tmp/offerfiles-results.json
```

The runner creates only temporary loopback servers and libraries, with private
server mode, empty seed lists, disabled updates and Kad, and isolated aMule
configuration directories. It requires Python 3 and a system libcrypto with the
MD4 function to generate real content hashes for its `known.met` fixtures.
Library fixtures are real 64-byte files with unique hashes and timestamps; their seeded
metadata excludes hashing throughput from these publication measurements.
A local relay observes actual OFFERFILES frames and can remove the compression
flag or cluster two frames. Assertions use the server's independent indexed
source counts and publication counters, not the client's published markers.

Cases also cover an unsolicited valid advertisement to an opted-out client and a
delayed advertisement spanning a due legacy publication tick. The former must
retain legacy limits; the latter must not queue an offer before receiving the
negotiated packet limit. Further cases reorder identity before ID assignment and
seed a small legacy hard limit in server.met, verifying that both negotiated and
legacy offers remain strictly below the known hard boundary.

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
`-DENABLE_ED2K_OFFERFILES_V1=ON -DENABLE_INTEGRATION_TESTS=ON -DOFFERFILES_INTEROP_SERVER=/absolute/path/to/ed2k-server`
and run `ctest --test-dir build --output-on-failure -R OfferFilesInteropTest`.
With the build switch off and the same server path, CTest instead registers
`OfferFilesCompiledOutInteropTest`: it verifies legacy publication, unsolicited
advertisements, and the hard boundary with a saved runtime opt-in set to 1.
The external test is not registered without that explicit binary path.

Queue sampling requires a Debug daemon with `VerboseDebug=1` and
`VerboseDebugLogfile=1` in `[eMule]`, and `Cat_Servers=1` in `[Debug]`.
During a negotiated session the Servers debug category records the monotonic
`tick_ms` and `pending_control_packets` on publication ticks, including idle ticks.
The count includes the partially sent control packet; it excludes bytes already
accepted by Asio or the OS and includes other server control packets. Logging is
optional and does not impose a queue limit.

With `--queue-samples`, the loopback suite requires these diagnostics and saves timestamped samples of
client queue depth alongside the server's `offer_v1` counters in the output JSON,
including both rounds of connections in the reconnect wave. The samples use the
most recent client log entry, whose tick identifies its age. A zero client queue count does
not prove indexing or bounded buffering elsewhere; the server index is checked
independently. The suite also renames an offered file twice with the distinct
budget exhausted, on plain and compressed connections, verifying the refreshed
hash/name on the wire and server record processing without increasing indexed
file count or offering an extra new file.
An independent loopback search client verifies that the server still returns
the original indexed filename and does not find the hash under the new rename
keyword. A completion fixture starts with one shared complete part and a missing
tail, exhausts a one-file candidate budget, and downloads the tail from an incoming
loopback peer. It verifies the finished bytes, partial/complete OFFERFILES markers,
and the server search result's complete-source count changing from zero to one
without reconnecting or consuming another distinct-file slot.

Queue-sampled cases also check that the client control queues drain and nobody
remains waiting at the global ceiling after indexing. The JSON includes sampled
peak and final queue counts per client. These sampled peaks are observations,
not queue bounds or measurements of Asio/kernel buffering.

The independent loopback scheduling regression test needs no external server:

```sh
python3 unittests/tests/OfferFilesLiveLimitsTest.py /absolute/path/to/amuled
```

It verifies resumption after a live UDP soft limit is raised and prompt legacy
publication after an empty library gains a file. Use `--legacy-only` for a
compiled-out daemon. CTest registers this test when integration tests are enabled.
