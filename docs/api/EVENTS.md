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
  "friend_added",   "friend_updated",   "friend_removed",
  "status_changed", "log_appended",
  "search_result_added", "search_progress", "search_closed",
  "comments_updated",
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
es.addEventListener("comments_updated", (e) => { /* refresh the file's comments view */ });
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
| `friends` | `friend_*` | The friends list, and whether each one is online |
| `status` | `status_*` | Connection state + headline counters |
| `logs` | `log_*` | amuled log buffer (live tail; serverinfo is poll-only) |
| `search` | `search_*` | Result deltas, completion, and the freeing of a search |
| `chats` | `chat_*` | Peer chat messages, and conversations being closed |
| `comments` | `comments_*` | Comment/rating lists on a download |

By default every channel is delivered. To subscribe to a subset, pass `?channels=` with a comma-separated list:

```sh
curl -N -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/events?channels=downloads,status"
```

Unknown channel names in the query are silently ignored — forward-compatibility hedge for future event families. The token cap on the filter set is 32 to bound the memory the parser allocates; passing more is silently truncated.

The `resync` event (see below) is ALWAYS delivered regardless of the filter. Its purpose is to signal a cache invalidation that the client cannot opt out of.

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

`reason` is one of:

- `"gap"` — events were evicted from the ring before the subscriber read them.
- `"restart"` — the subscriber's id was past the bus's newest, only possible after a daemon restart.
- `"idle"` — the daemon stopped diffing because nothing had been subscribed for several ticks, so no collection change is represented on the bus for that period. A cursor from before it cannot be trusted however in-range it looks — and it may well still be in range, because the chat publisher keeps ids moving regardless.

On any of them, the client's correct response is:

1. Wipe its in-memory cache of whatever REST collections it tracked.
2. Re-GET those collections from the REST surface.
3. Continue accepting events from the new id.

`gap` and `restart` are synthesised per subscriber, at connect, and carry `since_id` and `newest_id` — both uint64, opaque; the client never has to compute them and should use `id:` on subsequent events.

`idle` is published on the bus instead, by the tick that resumes diffing, and carries only `reason`: it is broadcast rather than addressed at one subscriber, so there is no single cursor to report, and the frame's own `id:` is the new one. The order matters — the tick re-establishes its baseline first and announces afterwards, so a client re-GETs against state that is already current. Announcing at connect instead would leave the changes of the intervening tick in neither the client's GET nor any event.

## Event catalog

Every event the bus publishes. The `_added` and `_updated` payloads are BYTE-FOR-BYTE identical to the matching REST resource's list-item shape — clients receiving a `*_updated` event get the full new state and never need to re-GET. `_removed` carries only the identity field — `hash` for files (`download_removed`, `shared_removed`) and `ecid` for every ECID-keyed collection (`client_removed`, `friend_removed`, `server_removed`) — so the client can drop the cache entry without needing the old object. Three events don't fit the collection-delta model: `status_changed` ships a full status envelope (replace, not merge), `log_appended` is an append operation (`{lines}` — push the lines onto the amule log buffer, don't replace), and `search_closed` retires a whole result space rather than one item (`{search_id}` — drop the view, not a row). Branch on the event type in your dispatcher accordingly.

### `downloads` channel

#### `download_added` / `download_updated`

Identical to the REST [`/api/v0/downloads`](REFERENCE.md#get-apiv0downloads) list-item shape. `_updated` fires on any field-level change including `size_done`, `size_xfer`, `speed_bps`, the source counters, `kad_comment_search_running` and `hashing_progress` — clients see live progress (and the Kad-notes lookup start → finish edge, and a running hash) without polling.

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
  "progress": { "percent": 29.85 },
  "kad_comment_search_running": false,
  "hashing_progress": 0
}
```

A `POST /downloads/{hash}/comments` flips `kad_comment_search_running` to `true`, producing a `download_updated`; when the Kad lookup finishes (typically ~45 s, or sooner) it flips back to `false`, producing another. Retrieved notes are then read via `GET /downloads/{hash}/comments`.

#### `download_removed`

```json
{ "hash": "8b54a3c2..." }
```

Only the hash; clients look up and drop the cache entry by hash.

#### `comments_updated`

Fires whenever a download's comment/rating list changes — a Kad note arriving during a `POST /downloads/{hash}/comments` lookup, **or** a connected ed2k source reporting its comment. Payload is the `GET /downloads/{hash}/comments` body plus `hash`, so a client can update its comments view directly from the event without re-fetching. The extra key is there because nothing else in the frame identifies the file:

```json
{
  "hash": "8b54a3c2...",
  "kad_comment_search_running": false,
  "count": 2,
  "comments": [
    { "username": "alice",    "filename": "Some.Movie.mkv", "rating": 5, "comment": "great quality" },
    { "username": "Kad user", "filename": "some_movie.avi", "rating": 4, "comment": "" }
  ]
}
```

Downloads only, but it rides the `comments` channel, not `downloads` -- `?channels=downloads` alone will not deliver it. It's kept separate from `download_updated` so the per-tick download frame stays lean — comments ride their own event and only when they actually change.

### `shared` channel

#### `shared_added` / `shared_updated`

Identical to the REST [`/api/v0/shared`](REFERENCE.md#get-apiv0shared) list-item shape. `_updated` fires on any field-level change including `priority`, `priority_auto`, `xfer.session`, `xfer.total`, `requests.*`, `accepts.*` and `hashing_progress` — clients see live upload counters (and priority changes, and a running hash) without polling.

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
  "accepts":  { "session": 18,      "total": 921 },
  "upload_speed_bps": 51200,
  "uploading":        2,
  "last_upload":      1700000500,
  "shared_since":     1699000000,
  "hashing_progress": 0
}
```

`hashing_progress` counts the parts hashed so far by a [`POST /shared/{hash}/verify`](REFERENCE.md#post-apiv0sharedhashverify) run or an AICH hashset rebuild, and is `0` when nothing is hashing — each advance pushes a `shared_updated`, so a progress bar can be driven straight off the stream. A file that is both downloading and shared reports the same value on both channels.

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
  "soft_file_limit": 1000,
  "hard_file_limit": 5000,
  "priority":    "normal",
  "ping_ms":     42,
  "failed_count": 0,
  "static":      false,
  "tcp_flags":   { "bitmask": 1497, "compression": true, "new_tags": true, "unicode": true,
                   "related_search": true, "type_tag_integer": true, "large_files": true,
                   "tcp_obfuscation": true },
  "udp_flags":   { "bitmask": 1851, "get_sources": true, "get_files": true, "new_tags": true,
                   "unicode": true, "get_sources_v2": true, "large_files": true,
                   "udp_obfuscation": true, "tcp_obfuscation": true }
}
```

A server announces its capabilities and publishing limits only after it answers a UDP status request, which is usually a tick or two after it is added. Until then it reports `0` / all-`false`, and the reply produces one `server_updated`. Every bit is documented in [`GET /api/v0/servers`](REFERENCE.md#get-apiv0servers).

#### `server_removed`

```json
{ "ecid": 1 }
```

Servers are ECID-keyed (not hash-keyed) so the removed payload carries the integer ECID.

### `friends` channel

#### `friend_added` / `friend_updated`

Identical to the REST [`/api/v0/friends`](REFERENCE.md#get-apiv0friends) list-item shape.

```json
{
  "ecid":         12,
  "name":         "alice",
  "user_hash":    "a1b2c3d4e5060e708090a0b0c0d06f00",
  "ip":           "203.0.113.42",
  "port":         4662,
  "client_ecid":  4382,
  "online":       true,
  "friend_slot":  false
}
```

`friend_updated` fires on any observable change, including a friend coming online or going offline — that transition is `client_ecid` moving between a live peer's ECID and `0`, which is what drives the connected indicator in the desktop client.

One `PATCH /api/v0/friends/{ecid}` can produce **two** `friend_updated` events. Only one friend may hold the friend slot, so granting it to one clears it on whoever held it before, and both records change.

#### `friend_removed`

```json
{ "ecid": 12 }
```

### `chats` channel

Backed by the chat session store in `amuled`, which is shared by every client — so these events carry messages sent from the desktop GUI and amulegui too, not only ones this API sent.

#### `chat_message`

One event per message, **inbound and outbound alike**. An outbound one is how a message sent from amulegui, or from another browser tab, reaches every other viewer.

```json
{
  "peer":        "203.0.113.42:4662",
  "ip":          "203.0.113.42",
  "port":        4662,
  "name":        "alice",
  "client_ecid": 4382,
  "friend_ecid": 12,
  "message":     { "id": 91, "direction": "in", "text": "thanks!", "timestamp": 1786652714 }
}
```

`message` is identical to a `messages[]` entry on [`GET /api/v0/chats/{peer}/messages`](REFERENCE.md#get-apiv0chatspeermessages), and `name` uses the same `"IP: <ip> Port: <port>"` fallback the REST list does.

There is no separate "conversation started" event: a conversation that did not exist yet is implied by the first message carrying its `peer`.

#### `chat_session_closed`

```json
{ "peer": "203.0.113.42:4662" }
```

Closing is global — see [`DELETE /api/v0/chats/{peer}`](REFERENCE.md#delete-apiv0chatspeer). This fires whichever client closed it, including the desktop GUI, so a viewer should drop the conversation rather than assume it still exists.

### `clients` channel

#### `client_added` / `client_updated`

Identical to the REST [`/api/v0/clients`](REFERENCE.md#get-apiv0clients) list-item shape. Speed fields move on every tick during active transfers, so the `clients` channel can be the loudest one on a busy node.

```json
{
  "ecid":                   4382,
  "name":                   "AnonymousPeer",
  "user_hash":              "1f2e3a...",
  "ip":                     "203.0.113.42",
  "country_code":           "de",
  "port":                   4662,
  "software":               "emule",
  "software_version":       "0.50a",
  "os_info":                "Linux",
  "upload_state":           "uploading",
  "download_state":         "idle",
  "ident_state":            "identified",
  "upload_file_hash":       "8b54a3c20fae9e4b9f7e0c2c8c01b6b1",
  "download_file_hash":     "",
  "download_file_name":     "",
  "upload_file_name":       "example-distribution.iso",
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
  "obfuscation_status":     "enabled",
  "friend_slot":            false,
  "part_progress_percent":  75.0
}
```
Carries the same field set as the [`/clients`](REFERENCE.md#get-apiv0clients) list row, including `source_origin`, `available_parts`, `mod_version` and `view_shared_disabled`.

`part_progress_percent` follows the same rule as on the REST row: it is how much of the file we are downloading **from** this peer the peer already holds, and the key is **omitted entirely** when there is no such file, rather than sent as a negative sentinel. It is derived from `available_parts` and the linked download's part count, so it moves when `available_parts` does, and drops out if that download goes away.

It never carries a `parts` bitmap — those are opt-in on the per-file client routes only, being one boolean per chunk per peer.


`upload_file_hash` (file we're uploading TO this peer) and `download_file_hash` (file we're downloading FROM this peer) are 32-char MD4 hex hashes — directly resolvable against [`/api/v0/downloads/{hash}`](REFERENCE.md#get-apiv0downloadshash) (in-progress) or the corresponding entry in [`/api/v0/shared`](REFERENCE.md#get-apiv0shared) by `.hash`. Either field can be empty when the peer is queued / idle in that direction. `download_file_name` is the filename the peer advertised while we download from them; `upload_file_name` is the partfile they're downloading from us, resolved locally — see [`GET /clients`](REFERENCE.md#get-apiv0clients) for details.

#### `client_removed`

```json
{ "ecid": 4382 }
```

### `status` channel

#### `status_changed`

Identical to the REST [`/api/v0/status`](REFERENCE.md#get-apiv0status) envelope, including the `null` rule on the two disk figures. The payload is the post-change snapshot, not a diff. Fires when any field anywhere in the envelope changes — ed2k state and identity, Kad state, Kad network counters, headline speeds, the overhead rates, the free-space figures, the queue counters, or `ec_connected`.

Rate impact is small: the overhead rates move about as often as the speeds already in that comparison, so they add no wakeups, and the disk figures are resampled only every 10 s by the daemon, so at worst they add one event per 10 s and only when the number actually moved. An idle daemon still emits nothing.

```json
{
  "ec_connected": true,
  "ed2k": {
    "state":       "connected",
    "high_id":     true,
    "id":          1234567890,
    "public_ip":   "210.2.150.73",
    "connected_since": 1751000000,
    "server_name": "eMule Server",
    "server_ip":   "203.0.113.5",
    "server_port": 4242,
    "network":     { "users": 312000, "files": 75000000 }
  },
  "kad": {
    "state":      "connected",
    "firewalled": false,
    "connected_since": 1751000000,
    "network":    { "users": 5400000, "files": 1400000000, "nodes": 2400 }
  },
  "speeds": {
    "download_bps": 4500000, "upload_bps": 50000,
    "download_overhead_bps": 8700, "upload_overhead_bps": 1100
  },
  "disk":   { "temp_free_bytes": 48318382080, "incoming_free_bytes": 48318382080 },
  "queue":  { "upload_clients_waiting": 12, "download_sources_total": 1843 }
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

Driven by the refresher state machine that owns the `POST /search` → completion lifecycle (see [REFERENCE.md](REFERENCE.md#search)). Result and progress events only fire while a search is active; the channel is otherwise silent apart from `search_closed`. The [Bootstrap example](#bootstrap-snapshot--stream) omits the search endpoints because searches are normally client-initiated post-boot; if your UI persists a "search-in-progress" state across reloads, list them with `GET /search` and fetch `GET /search/{id}/results` in step 2 too.

#### `search_result_added`

Emitted per new result that appears in the results map between refresher ticks.

```json
{
  "search_id": 42,
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

`search_id` routes the result to the search that produced it — amuleapi runs several searches at once (see [REFERENCE.md](REFERENCE.md#post-apiv0search)), so demux on it. Key results by `(search_id, hash)`. Aside from the leading `search_id`, the payload is byte-for-byte identical to a `/search/{id}/results` array entry — the two are emitted by the same writer, so the promise holds by construction. That includes `status`, `type`, `directory` (the folder inside a browsed peer's share, `""` on ordinary hits), `kad_comment_search_running`, `comments[]` and the `children[]` grouping array — see [REFERENCE.md](REFERENCE.md#get-apiv0searchidresults); `sources` is the nested `{total, complete}` object, `media` — the audio/video metadata object — is present only for locally-known/probed hits and omitted otherwise, and `children` holds the same-hash/different-name alternatives (empty for a single-name hit), same as the REST endpoint. Only parent results fire this event — children are folded into their parent's `children[]`, never emitted as their own `search_result_added`. Each `search_id` is an independent result space — a new `POST /search` starts a fresh one without disturbing the others.

#### `search_progress`

Emitted whenever a search's completion advances and once more on its completion; every frame carries the `search_id` it refers to. Two triggers, both off the daemon's unambiguous `EC_TAG_SEARCH_LIFECYCLE_*` tags (see [REFERENCE.md](REFERENCE.md#get-apiv0searchidresults)): the `percent` changing between refresher ticks while the search runs, and the lifecycle flipping to finished (the `state` `running` → `finished` edge). A newly-started search also emits its initial `running` frame. The completion frame is just the terminal `search_progress` with `"state": "finished"` — there is **no** separate `search_finished` event.

```json
{ "search_id": 42, "state": "running", "percent": 47, "results": 88, "kind": "kad" }
```

```json
{ "search_id": 42, "state": "finished", "percent": 100, "results": 153, "kind": "local" }
```

- `search_id` — which search this frame is about.
- `state` — `"running"` while the search is in flight, `"finished"` on the terminal frame.
- `percent` — `0..100`, daemon-computed for every search kind. For **global** it is the real server-queue progress. For **Kad**, which has no measurable progress, it is a cosmetic time-ramp derived from the fixed 45 s keyword-search lifetime (capped at 99 until the daemon authoritatively reports completion, then 100); see [REFERENCE.md](REFERENCE.md#get-apiv0searchidresults). Treat the Kad value as a liveliness indicator, not an accurate completion estimate.
- `kind` — the originally-requested search type (`"local"` | `"global"` | `"kad"` | `"browse"`).
- `results` — the current results-map size; subscribers can reconcile against any `search_result_added` they may have missed via `GET /search/{id}/results`.

A Kad search hitting its result cap (`SEARCHKEYWORD_TOTAL`, 300) before the 45 s deadline finishes early — the lifecycle flips to `finished` and `percent` jumps straight to 100 ahead of the ramp.

A **browse** started via [`POST /clients/{ecid}/shared_files`](REFERENCE.md#post-apiv0clientsecidshared_files) rides this same channel: its `search_id` fires `search_result_added` per file the peer returns and `search_progress` frames with `"kind": "browse"`, where `percent` tracks the directories received so far. A denied / unreachable / lost browse flips to `finished` with the results it managed to collect (often zero) — same terminal frame as a completed one, no distinct failure event.

#### `search_closed`

The search is **gone**: its slot has been freed and its results are no longer readable. A subscriber holding one view per search should drop that view — the next `GET /api/v0/search/{id}/results` for this id is a `404`.

```json
{ "search_id": 42 }
```

Three things produce it: [`DELETE /api/v0/search/{id}`](REFERENCE.md#delete-apiv0searchid) from any client, amuleapi evicting an old finished search to stay under its slot cap, and an EC reconnect (which invalidates every cached `search_id` at once).

**What it is not:** a search that amuled evicted from its own 20-entry ring is *retired*, not freed — amuleapi keeps its last-known results for late reads, so that case arrives as a terminal `search_progress` with `"state": "finished"`. Only a vanished slot produces `search_closed`.

### Filter-bypass: `resync`

`resync` has no underscore prefix — it doesn't belong to any of the channel buckets above and is always delivered regardless of `?channels=`, whether it was synthesised for one subscriber or published on the bus. Documented under [Reconnect and Last-Event-ID](#reconnect-and-last-event-id).

## Single-publisher invariant

Only the wxApp refresher tick publishes diffs onto the bus. A future inline-refresh-then-publish path from an HTTP-thread mutation would silently race the refresher's diff walk; the daemon's debug build asserts this and the release build hard-aborts. End-user impact: events are strictly ordered by `id`, monotonically, with no interleavings between distinct publishers.

## Shutdown behaviour

When the daemon receives `SIGINT` / `SIGTERM`, the event bus is latched into a shutdown state, every in-flight `Drain()` wakes immediately and returns no events, and every live SSE socket is closed from the I/O thread. A subscriber loop sees the underlying stream go dead, exits the read loop, and reconnects on its normal backoff. EventSource handles this with no application code on the client side.
