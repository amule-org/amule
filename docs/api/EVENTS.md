# amuleapi v0 — Server-Sent Events

This document is the contract for the `/api/v0/events` Server-Sent Events stream. For the REST surface see [REFERENCE.md](REFERENCE.md). For first-run setup see [../QUICKSTART-AMULEAPI.md](../QUICKSTART-AMULEAPI.md).

Event payloads follow the same machine contract as the REST responses: **English text and C-locale numbers**, independent of the `amuleapi`/`amuled` `--locale` (see [Localization and number formatting](REFERENCE.md#localization-and-number-formatting)). The same out-of-scope carve-outs apply (log line content and user/external data are not English-normalized).

## Why SSE

Polling `/api/v0/downloads` every second for a few thousand transfers is a multi-MB-per-tick conversation that the ETag cache helps with but can't eliminate — even a 304 still costs the round trip. SSE lets the daemon push only the deltas the client hasn't seen: a single `download_updated` per transfer per second, against a JSON envelope of a few hundred bytes.

Clients connect once, leave the connection open, and react to typed events as they arrive. The browser EventSource API and `curl -N` both work out of the box.

## Bootstrap: snapshot + stream

REST snapshots and the `/events` stream need a specific call ordering or events that fire between them are silently lost. The right sequence:

1. **Open `/api/v0/events` first** — buffer arrivals, don't apply yet. No `Last-Event-ID` is fine; the cursor anchors on whatever id was newest at handshake time.
2. **`GET` the REST collections** in parallel.
3. **Load, drain, flip** — load each snapshot into the store, drain the buffer in arrival order, then switch to direct-apply, all in one synchronous turn so no event can land between drain and flip.

Buffer-then-replay (rather than merging the snapshot into a live store) is required because of `_removed` events: a snapshot built before the refresher's exclusive lock for a removing tick can still contain the deleted entity, and a merge-style load would `set()` it back over a buffered delete. With buffer-then-replay the snapshot lands first, the buffered `_removed` then clears the stale entry.

```js
// 1. Open SSE first. One dispatcher per event type, behaviour switched
//    by a boot flag so we don't have to add-then-remove listeners.
let booting = true;
const buffered = [];

function onEvent(ev) {
  const entry = { type: ev.type, data: JSON.parse(ev.data) };
  if (booting) buffered.push(entry);
  else applyEvent(entry);
}

const EVENT_TYPES = [
  "download_added", "download_updated", "download_removed",
  "shared_added",   "shared_updated",   "shared_removed",
  "client_added",   "client_updated",   "client_removed",
  "server_added",   "server_updated",   "server_removed",
  "status_changed", "log_appended",
  "search_result_added", "search_progress",
];
const es = new EventSource("/api/v0/events", { withCredentials: true });
for (const t of EVENT_TYPES) es.addEventListener(t, onEvent);
es.addEventListener("resync", () => location.reload()); // simplest recovery

// 2. Pull baseline snapshots in parallel.
const [downloads, shared, clients, servers, status] = await Promise.all([
  fetch("/api/v0/downloads").then((r) => r.json()),
  fetch("/api/v0/shared").then((r) => r.json()),
  fetch("/api/v0/clients").then((r) => r.json()),
  fetch("/api/v0/servers").then((r) => r.json()),
  fetch("/api/v0/status").then((r) => r.json()),
]);

// 3. Load each snapshot into the store, drain the buffer, flip the flag —
//    single synchronous block so no event can fire between drain and flip.
//    `loadSnapshot` is your store-specific "replace this collection" call,
//    e.g. `store.set(name, payload)` or `collections.set(name, new Map(...))`.
loadSnapshot("downloads", downloads);
loadSnapshot("shared",    shared);
loadSnapshot("clients",   clients);
loadSnapshot("servers",   servers);
loadSnapshot("status",    status);
for (const ev of buffered) applyEvent(ev);
buffered.length = 0;
booting = false;
```

If the daemon restarts between steps 1 and 2, or the ring buffer overflows on a very busy bus, the synthetic `resync` event tells the client to wipe its cache and re-GET. See [Reconnect and Last-Event-ID](#reconnect-and-last-event-id) for the recovery rules — the bootstrap path is the same `GET` sweep, just on a non-fresh cache.

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
es.addEventListener("error",            ()  => {
  // EventSource auto-reconnects with backoff; only surface to UI on terminal failure.
  if (es.readyState === EventSource.CLOSED) { /* show "disconnected" */ }
});
```

The cookie-based auth path is the default for browser EventSource — the HttpOnly cookie set by `/auth/login` is carried automatically. Bearer-auth works for `curl -N` and any HTTP client that lets you set request headers, but the native browser `EventSource` API doesn't, so browser bearer-on-SSE needs a polyfill (e.g. [`@microsoft/fetch-event-source`](https://github.com/Azure/fetch-event-source)). For browser SPAs the cookie path is the friction-free choice.

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

The trailing blank line terminates the frame. `id` is a monotonically increasing `uint64` per amuleapi process — see [Reconnect and Last-Event-ID](#reconnect-and-last-event-id) below. `data` is the JSON payload documented per event in [Event catalog](#event-catalog). Payloads never contain literal newlines (the diff serializer escapes them) so one `data:` line is always enough.

## Channels and filtering

Every event belongs to a single channel. The full set, prefix-mapped from the event name:

| Channel | Event-name prefix | What changed |
|---------|-------------------|--------------|
| `downloads` | `download_*` | Transfers in the active queue |
| `shared` | `shared_*` | Shared file list |
| `servers` | `server_*` | Known ed2k servers |
| `clients` | `client_*` | Peers we're exchanging with |
| `status` | `status_*` | Connection state + headline counters |
| `logs` | `log_*` | amuled log buffer (live tail; serverinfo is poll-only) |
| `search` | `search_*` | Result deltas + completion of an active `POST /search` |

By default every channel is delivered. To subscribe to a subset, pass `?channels=` with a comma-separated list:

```sh
curl -N -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/events?channels=downloads,status"
```

Unknown channel names in the query are silently ignored — forward-compatibility hedge for future event families. The token cap on the filter set is 32 to bound the memory the parser allocates; passing more is silently truncated.

The synthetic `resync` event (see below) is ALWAYS delivered regardless of the filter. Its purpose is to signal a cache invalidation that the client cannot opt out of.

Mirror your filter in the bootstrap: only `GET` the REST collections matching the channels you subscribed to. Pulling a collection whose channel you filtered out leaves that snapshot silently stale — it never receives updates from the stream.

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

The ring buffer holds 16 384 events by default — sized to absorb a cold-start tick on a busy node (5 K downloads + 5 K shared can publish ~10 K `*_added` events in a single tick before any subscriber has had a chance to drain). Operators with very heavy workloads can raise the cap via `amuleapi.conf[Streaming]/EventBusRingCapacity`; values below the bus's compile-time floor (16) are clamped up so an operator can't accidentally disable replay. Worst-case memory ≈ capacity × ~1 KB JSON payload. A burst that adds more events than capacity between reconnects triggers the "gap" path.

The same gap detection also runs on the live path: if a publisher floods the bus between two `Drain()` calls and evicts events the current subscriber hadn't seen, the daemon emits the same `resync` frame and restarts the subscriber's cursor at the current newest. This catches the cold-start tick described above when capacity is set lower than the burst size.

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

Every event the bus publishes. The `_added` and `_updated` payloads are BYTE-FOR-BYTE identical to the matching REST resource's list-item shape — clients receiving a `*_updated` event get the full new state and never need to re-GET. `_removed` carries only the identity field — `hash` for files (`download_removed`, `shared_removed`), `client_ecid` for `client_removed`, `ecid` for `server_removed` — so the client can drop the cache entry without needing the old object. Two events don't fit the collection-delta model: `status_changed` ships a full status envelope (replace, not merge) and `log_appended` is an append operation (`{lines}` — push the lines onto the amule log buffer, don't replace). Branch on the event type in your dispatcher accordingly.

### `downloads` channel

#### `download_added` / `download_updated`

Identical to the REST [`/api/v0/downloads`](REFERENCE.md#get-apiv0downloads) list-item shape. `_updated` fires on any field-level change including `size_done`, `size_xfer`, `speed_bps`, and the source counters — clients see live progress without polling.

```json
{
  "hash":          "8b54a3c2...",
  "name":          "ubuntu-26.04-desktop-amd64.iso",
  "ed2k_link":     "ed2k://|file|ubuntu...|3825..|8b54...|/",
  "size":          3825205248,
  "size_done":     1142000000,
  "size_xfer":     1102450000,
  "speed_bps":     4500000,
  "status":        "downloading",
  "priority":      "normal",
  "priority_auto": true,
  "category":      0,
  "sources":  { "total": 217, "not_current": 23, "transferring": 8, "a4af": 4 },
  "progress": { "percent": 29.85 }
}
```

#### `download_removed`

```json
{ "hash": "8b54a3c2..." }
```

Only the hash; clients look up and drop the cache entry by hash.

### `shared` channel

#### `shared_added` / `shared_updated`

Identical to the REST [`/api/v0/shared`](REFERENCE.md#get-apiv0shared) list-item shape. `_updated` fires on any field-level change including `priority`, `priority_auto`, `xfer.session`, `xfer.total`, `requests.*`, and `accepts.*` — clients see live upload counters (and priority changes) without polling.

```json
{
  "hash":             "1a2b3c4d...",
  "name":             "release-notes.txt",
  "ed2k_link":        "ed2k://|file|release-notes.txt|3217|1a2b...|/",
  "size":             3217,
  "priority":         "normal",
  "priority_auto":    false,
  "complete_sources": 12,
  "xfer":     { "session": 5242880, "total": 314572800 },
  "requests": { "session": 42,      "total": 1837 },
  "accepts":  { "session": 18,      "total": 921 }
}
```

#### `shared_removed`

```json
{ "hash": "1a2b3c4d..." }
```

### `servers` channel

#### `server_added` / `server_updated`

Identical to the REST [`/api/v0/servers`](REFERENCE.md#get-apiv0servers) list-item shape.

```json
{
  "ecid":        1,
  "name":        "eMule Server",
  "description": "Public server",
  "version":     "17.15",
  "address":     "203.0.113.5:4242",
  "country_code": "de",
  "port":        4242,
  "users":       312000,
  "max_users":   500000,
  "files":       75000000,
  "priority":    "normal",
  "ping_ms":     42,
  "failed":      0,
  "static":      false
}
```

#### `server_removed`

```json
{ "ecid": 1 }
```

Servers are ECID-keyed (not hash-keyed) so the removed payload carries the integer ECID.

### `clients` channel

#### `client_added` / `client_updated`

Identical to the REST [`/api/v0/clients`](REFERENCE.md#get-apiv0clients) list-item shape. Speed fields move on every tick during active transfers, so the `clients` channel can be the loudest one on a busy node.

```json
{
  "client_ecid":            4382,
  "client_name":            "AnonymousPeer",
  "user_hash":              "1f2e3a...",
  "ip":                     "203.0.113.42",
  "country_code":           "de",
  "port":                   4662,
  "software":               "eMule",
  "software_version":       "0.50a",
  "os_info":                "Linux",
  "upload_state":           "uploading",
  "download_state":         "idle",
  "ident_state":            "verified",
  "upload_file_hash":       "8b54a3c20fae9e4b9f7e0c2c8c01b6b1",
  "download_file_hash":     "",
  "download_file_name":     "",
  "xfer": {
    "up_session":   22000000,
    "down_session": 0,
    "up_total":     452000000,
    "down_total":   189000000
  },
  "upload_speed_bps":       22000,
  "download_speed_bps":     0,
  "queue_waiting_position": 0,
  "remote_queue_rank":      0,
  "score":                  150,
  "obfuscation_status":     "obfuscated",
  "friend_slot":            false
}
```

`upload_file_hash` (file we're uploading TO this peer) and `download_file_hash` (file we're downloading FROM this peer) are 32-char MD4 hex hashes — directly resolvable against [`/api/v0/downloads/{hash}`](REFERENCE.md#get-apiv0downloadshash) (in-progress) or the corresponding entry in [`/api/v0/shared`](REFERENCE.md#get-apiv0shared) by `.hash`. Either field can be empty when the peer is queued / idle in that direction.

#### `client_removed`

```json
{ "client_ecid": 4382 }
```

### `status` channel

#### `status_changed`

Identical to the REST [`/api/v0/status`](REFERENCE.md#get-apiv0status) envelope. The payload is the post-change snapshot, not a diff. Fires when any field anywhere in the envelope changes — ed2k state, Kad state, Kad network counters, headline speeds, queue length, or `ec_connected`.

```json
{
  "ec_connected": true,
  "ed2k": {
    "state":       "connected",
    "low_id":      false,
    "server_name": "eMule Server",
    "server_ip":   "203.0.113.5",
    "server_port": 4242
  },
  "kad": {
    "state":      "connected",
    "firewalled": false,
    "network":    { "users": 5400000, "files": 1400000000, "nodes": 2400 }
  },
  "speeds": { "download_bps": 4500000, "upload_bps": 50000 },
  "queue":  { "upload_queue_length": 12, "total_source_count": 1843 }
}
```

Subscribe to this channel alone for a thin "header bar" client that just wants connection state and headline counters.

### `logs` channel

#### `log_appended`

Emitted when the amuled log buffer appends new lines.

```json
{ "lines": ["2026-06-19 11:00:00: line one", "2026-06-19 11:00:01: line two"] }
```

Only the amuled log has a live channel; the serverinfo buffer has no SSE feed and is fetched by polling [`GET /logs/serverinfo`](REFERENCE.md#get-apiv0logsserverinfo). Multiple lines may be batched into a single event when the buffer landed several lines between refresher ticks. The [Bootstrap example](#bootstrap-snapshot--stream) doesn't pull `/logs/amule` — fetch it in step 2 if your UI shows historical log lines, otherwise treat `log_appended` as a live-only feed.

### `search` channel

Driven by the refresher state machine that owns the `POST /search` → completion lifecycle (see [REFERENCE.md](REFERENCE.md#search-results)). Events only fire while a search is active; the channel is silent at idle. The [Bootstrap example](#bootstrap-snapshot--stream) omits `/search/results` because searches are normally client-initiated post-boot; if your UI persists a "search-in-progress" state across reloads, fetch `/search/results` and `/search/progress` in step 2 too.

#### `search_result_added`

Emitted per new result that appears in the results map between refresher ticks.

```json
{
  "hash": "0123456789abcdef0123456789abcdef",
  "name": "ubuntu-24.04-desktop-amd64.iso",
  "size": 5765873664,
  "sources": { "total": 12, "complete": 7 },
  "already_have": false,
  "rating": 0,
  "status": "new",
  "type": "videos",
  "media": { "length_s": 5400, "bitrate": 1500, "codec": "h264", "artist": "", "album": "", "title": "" },
  "children": []
}
```

Key results by `hash`. The payload is byte-for-byte identical to a `/search/results` array entry (including `status`, `type`, and the `children[]` grouping array — see [REFERENCE.md](REFERENCE.md#get-apiv0searchresults)); `sources` is the nested `{total, complete}` object, `media` — the audio/video metadata object — is present only for locally-known/probed hits and omitted otherwise, and `children` holds the same-hash/different-name alternatives (empty for a single-name hit), same as the REST endpoint. Only parent results fire this event — children are folded into their parent's `children[]`, never emitted as their own `search_result_added`. amuled wipes its searchlist on every new `POST /search`, so subscribers must treat each search as a fresh result space — clear prior results when you start a new query.

#### `search_progress`

Emitted whenever the current search's completion advances and once more on completion. Two triggers, both off the daemon's unambiguous `EC_TAG_SEARCH_LIFECYCLE_*` tags (see [REFERENCE.md](REFERENCE.md#search-results)): the `percent` changing between refresher ticks while the search runs, and the lifecycle flipping to finished (the `state` `running` → `finished` edge). The completion frame is just the terminal `search_progress` with `"state": "finished"` — there is **no** separate `search_finished` event.

```json
{ "state": "running", "percent": 47, "results": 88, "kind": "kad" }
```

```json
{ "state": "finished", "percent": 100, "results": 153, "kind": "local" }
```

- `state` — `"running"` while the search is in flight, `"finished"` on the terminal frame.
- `percent` — `0..100`, daemon-computed for every search kind. For **global** it is the real server-queue progress. For **Kad**, which has no measurable progress, it is a cosmetic time-ramp derived from the fixed 45 s keyword-search lifetime (capped at 99 until the daemon authoritatively reports completion, then 100); see [REFERENCE.md](REFERENCE.md#search-results). Treat the Kad value as a liveliness indicator, not an accurate completion estimate.
- `kind` — the originally-requested search type (`"local"` | `"global"` | `"kad"`).
- `results` — the current results-map size; subscribers can reconcile against any `search_result_added` they may have missed via `GET /search/results`.

A Kad search hitting its result cap (`SEARCHKEYWORD_TOTAL`, 300) before the 45 s deadline finishes early — the lifecycle flips to `finished` and `percent` jumps straight to 100 ahead of the ramp.

### Filter-bypass: `resync`

The synthetic `resync` event has no underscore prefix — it doesn't belong to any of the channel buckets above and is always delivered regardless of `?channels=`. Documented under [Reconnect and Last-Event-ID](#reconnect-and-last-event-id).

## Single-publisher invariant

Only the wxApp refresher tick publishes diffs onto the bus. A future inline-refresh-then-publish path from an HTTP-thread mutation would silently race the refresher's diff walk; the daemon's debug build asserts this and the release build hard-aborts. End-user impact: events are strictly ordered by `id`, monotonically, with no interleavings between distinct publishers.

## Shutdown behaviour

When the daemon receives `SIGINT` / `SIGTERM`, the event bus is latched into a shutdown state, every in-flight `Drain()` wakes immediately and returns no events, and every live SSE socket is closed from the I/O thread. A subscriber loop sees the underlying stream go dead, exits the read loop, and reconnects on its normal backoff. EventSource handles this with no application code on the client side.
