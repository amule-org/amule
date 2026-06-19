# amuleapi v0 — Server-Sent Events

This document is the contract for the `/api/v0/events` Server-Sent Events stream. For the REST surface see [REFERENCE.md](REFERENCE.md). For first-run setup see [../QUICKSTART-AMULEAPI.md](../QUICKSTART-AMULEAPI.md).

## Why SSE

Polling `/api/v0/downloads` every second for a few thousand transfers is a multi-MB-per-tick conversation that the ETag cache helps with but can't eliminate — even a 304 still costs the round trip. SSE lets the daemon push only the deltas the client hasn't seen: a single `download_updated` per transfer per second, against a JSON envelope of a few hundred bytes.

Clients connect once, leave the connection open, and react to typed events as they arrive. The browser EventSource API and `curl -N` both work out of the box.

## Connecting

`GET /api/v0/events` opens the stream. Auth runs synchronously BEFORE the worker thread is spawned and before the 32-slot streaming budget is touched, so an unauthenticated peer can't tie up a slot for the read-timeout window.

```sh
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
  -d '{"password":"adminpass"}' \
  "http://$HOST/api/v0/auth/login?type=bearer" | jq -r .token)

curl -N -H "Authorization: Bearer $TOKEN" http://$HOST/api/v0/events
```

Browser:

```js
const es = new EventSource("/api/v0/events", { withCredentials: true });
es.addEventListener("download_added",   (e) => { /* JSON.parse(e.data) */ });
es.addEventListener("download_updated", (e) => { /* ... */ });
es.addEventListener("download_removed", (e) => { /* ... */ });
es.addEventListener("resync",           (e) => { /* re-GET REST collections */ });
```

The cookie-based auth path is the default for browser EventSource — the HttpOnly cookie set by `/auth/login` is carried automatically. Bearer-auth clients send `Authorization: Bearer <jwt>` like any other endpoint.

### Auth failure shape

Auth failures land on the SSE endpoint with the same JSON error envelope as the REST surface, not as an event frame. The HTTP status reflects the failure (`401`, `403`, `429`) so well-behaved clients can react before the stream loop starts. Example:

```
HTTP/1.1 401 Unauthorized
Content-Type: application/json

{"error":{"code":"unauthorized","message":"missing bearer token or session cookie"}}
```

### Response headers

```
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
X-Accel-Buffering: no
Connection: keep-alive
```

`X-Accel-Buffering: no` tells nginx (the most common reverse proxy in front of amuleapi) not to coalesce chunks — without it, the stream stalls until the proxy buffer fills.

### Initial chunk

The first thing the client sees is a comment line:

```
: connected

```

Comment lines start with `:` and are discarded by SSE parsers. They keep the channel observably alive for browsers whose `onopen` fires only after a real chunk lands.

## Frame format

Every event the daemon emits has the same three-line shape:

```
event: <name>
id: <id>
data: <json>

```

The trailing blank line terminates the frame. `id` is a monotonically increasing `uint64` per amuleapi process — see §Last-Event-ID below. `data` is the JSON payload documented per event in §Event catalog. Payloads never contain literal newlines (the diff serializer escapes them) so one `data:` line is always enough.

## Channels and filtering

Every event belongs to a single channel. The full set, prefix-mapped from the event name:

| Channel | Event-name prefix | What changed |
|---------|-------------------|--------------|
| `downloads` | `download_*` | Transfers in the active queue |
| `shared` | `shared_*` | Shared file list |
| `servers` | `server_*` | Known ed2k servers |
| `clients` | `client_*` | Peers we're exchanging with |
| `status` | `status_*` | Connection state + headline counters |
| `logs` | `log_*` | amuled / serverinfo log buffers |

By default every channel is delivered. To subscribe to a subset, pass `?channels=` with a comma-separated list:

```sh
curl -N -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/events?channels=downloads,status"
```

Unknown channel names in the query are silently ignored — forward-compatibility hedge for future event families. The token cap on the filter set is 32 to bound the memory the parser allocates; passing more is silently truncated.

The synthetic `resync` event (see below) is ALWAYS delivered regardless of the filter. Its purpose is to signal a cache invalidation that the client cannot opt out of.

## Heartbeat

If 15 seconds pass with no event written to the wire, the daemon emits:

```
: keepalive

```

NAT / load balancers / browser EventSource implementations tend to drop idle TCP connections after 30–60 s of silence. The heartbeat keeps the connection warm. The interval is wall-clock-driven (not Drain-timeout-driven) so a busy bus paired with a restrictive `?channels=` filter — where each Drain returns immediately because events are pending but all of them get filtered out — still emits keepalives on schedule.

## Reconnect and Last-Event-ID

EventSource clients (and well-behaved SDK clients) handle the underlying socket dropping by reopening the stream and replaying the `id:` of the last event they processed via the `Last-Event-ID` request header. The daemon's reconnect path uses that to figure out what (if anything) it can resume:

| Scenario | Daemon response | What the client sees |
|----------|-----------------|----------------------|
| Header absent or unparseable | Start at the current newest id | New events as they arrive — no replay |
| `parsed_id + 1 >= OldestId` | Resume from `parsed_id` | First Drain returns the missed range immediately |
| `parsed_id + 1 < OldestId` (gap) | Emit a synthetic `resync` event with `reason: "gap"`, start at current newest | Client invalidates its cache and re-GETs the REST collections |
| `parsed_id > NewestId` (stale) | Emit a synthetic `resync` event with `reason: "restart"`, start at current newest | Client invalidates: amuleapi was restarted (ids are per-process and reset to 1 on each launch) |

The ring buffer holds 100 events. A burst that adds more than 100 events between reconnects triggers the "gap" path.

The same gap detection also runs on the live path: if a publisher floods the bus between two `Drain()` calls and evicts events the current subscriber hadn't seen, the daemon emits the same `resync` frame and restarts the subscriber's cursor at the current newest. This catches the cold-start tick on a 5K-download library where ~5K `_added` events fire faster than any drainer can read.

### `resync` frame

```
event: resync
id: <current newest>
data: {"reason":"gap","since_id":<old cursor>,"newest_id":<new cursor>}

```

`reason` is `"gap"` (events evicted from the ring before the subscriber read them) or `"restart"` (subscriber's id was past the bus's newest — only possible after a daemon restart). On either, the client's correct response is:

1. Wipe its in-memory cache of whatever REST collections it tracked.
2. Re-GET those collections from the REST surface.
3. Continue accepting events from the new id.

Both `since_id` and `newest_id` are uint64. The client never has to compute them — it should treat them as opaque and use `id:` on subsequent events.

## Event catalog

Every event the bus publishes. The `_added` payload is always identical to the matching REST resource's list-item shape; the `_updated` payload is the FULL new snapshot (not a diff — Phase 8b's "any change publishes the whole object" model). `_removed` carries only the identity field so the client can drop the cache entry without needing the old object.

### `downloads` channel

#### `download_added`

```json
{
  "ecid": 17,
  "hash": "8b54a3c2...",
  "name": "ubuntu-26.04-desktop-amd64.iso",
  "ed2k_link": "ed2k://|file|ubuntu...|3825..|8b54...|/",
  "size": 3825205248,
  "size_done": 0,
  "size_xfer": 0,
  "speed_bps": 0,
  "status": "downloading",
  "priority": "normal",
  "priority_auto": true,
  "category": 0,
  "sources": { "total": 0, "not_current": 0, "transferring": 0, "a4af": 0 },
  "percent": 0.0
}
```

#### `download_updated`

Same shape as `download_added`. Fires on any field-level change.

#### `download_removed`

```json
{ "hash": "8b54a3c2..." }
```

Only the hash; clients look up and drop the cache entry by hash.

### `shared` channel

#### `shared_added`

```json
{
  "ecid": 91,
  "hash": "1a2b3c4d...",
  "name": "release-notes.txt",
  "size": 3217,
  "priority": "normal",
  "complete_sources": 12
}
```

#### `shared_updated`

Same shape as `shared_added`.

#### `shared_removed`

```json
{ "hash": "1a2b3c4d..." }
```

### `servers` channel

#### `server_added`

```json
{
  "ecid": 1,
  "name": "eMule Server",
  "address": "203.0.113.5:4242",
  "users": 312000,
  "files": 75000000,
  "priority": "normal",
  "static": false
}
```

#### `server_updated`

Same shape as `server_added`.

#### `server_removed`

```json
{ "ecid": 1 }
```

Servers are ECID-keyed (not hash-keyed) so the removed payload carries the integer ECID.

### `clients` channel

#### `client_added`

```json
{
  "ecid": 4382,
  "client_name": "AnonymousPeer",
  "user_hash": "1f2e3a...",
  "ip": "203.0.113.42",
  "port": 4662,
  "software": "eMule",
  "upload_state": "uploading",
  "download_state": "idle",
  "upload_speed_bps": 22000,
  "download_speed_bps": 0
}
```

#### `client_updated`

Same shape as `client_added`. Speed fields move on every tick during active transfers, so the `clients` channel can be the loudest one on a busy node.

#### `client_removed`

```json
{ "ecid": 4382 }
```

### `status` channel

#### `status_changed`

Fires whenever any field in the `/api/v0/status` envelope changes. The payload is the post-change snapshot, not a diff.

```json
{
  "ed2k_state": "connected",
  "kad_state": "connected",
  "ed2k_lowid": false,
  "kad_firewalled": false,
  "server_name": "eMule Server",
  "server_ip": "203.0.113.5",
  "server_port": 4242,
  "download_bps": 4500000,
  "upload_bps": 50000,
  "ul_queue_len": 12,
  "total_src_count": 1843
}
```

Subscribe to this channel alone for a thin "header bar" client that just wants connection state and headline counters.

### `logs` channel

#### `log_appended`

Emitted when amuled or the serverinfo buffer appends new lines.

```json
{ "buffer": "amule", "lines": ["2026-06-19 11:00:00: line one", "2026-06-19 11:00:01: line two"] }
```

`buffer` is `"amule"` or `"serverinfo"`. Multiple lines may be batched into a single event when the underlying buffer landed several lines between refresher ticks.

### Filter-bypass: `resync`

The synthetic `resync` event has no underscore prefix — it doesn't belong to any of the channel buckets above and is always delivered regardless of `?channels=`. Documented under §Reconnect.

## Single-publisher invariant

Only the wxApp refresher tick publishes diffs onto the bus. A future inline-refresh-then-publish path from an HTTP-thread mutation would silently race the refresher's diff walk; the daemon's debug build asserts this and the release build hard-aborts. End-user impact: events are strictly ordered by `id`, monotonically, with no interleavings between distinct publishers.

## Shutdown behaviour

When the daemon receives `SIGINT` / `SIGTERM`, the event bus is latched into a shutdown state, every in-flight `Drain()` wakes immediately and returns no events, and every live SSE socket is closed from the I/O thread. A subscriber loop sees the underlying stream go dead, exits the read loop, and reconnects on its normal backoff. EventSource handles this with no application code on the client side.
