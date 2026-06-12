# RFC: amuleweb REST API v0.1

| | |
|---|---|
| **Status** | Draft |
| **Version** | 0.1 |
| **Date** | 2026-06-12 |
| **Scope** | Client-visible interface contract only |

## 1. Status & scope

This RFC defines a JSON REST API for the aMule web server (amuleweb).
It is the first step of the WebUI modernization effort:
the API must provide **functional parity with the current `default`
template** — everything the existing PHP pages can display or do, a client
must be able to display or do through this API.

This document is the contract as seen **from the client**. Implementation
details (C++, EC protocol) are out of scope. The API is
versioned `v0` in the URL because it is unstable; this document is revision
0.1 of that surface. When the surface stabilizes it will be promoted to
`/api/v1` and a v1.0 RFC.

Out of scope for v0.1 (recorded in §8):

- HTTPS/TLS (delegated to a reverse proxy, as today).
- Push/WebSocket notifications — clients poll.
- Pagination — every collection is bounded by the daemon (download queue,
  server list, search results) and is returned whole.
- Server-side sorting and filtering — collections are returned whole and
  clients sort/filter locally (see §2). No `?sort=`/`?filter=` query
  parameters exist in v0.1; they are planned for a future revision (see the
  scalability note on `GET /downloads`).
- Multi-user accounts — the daemon has exactly two credentials (admin and
  guest), as today.

## 2. Conventions

- Base path: **`/api/v0`**. All paths below are relative to it.
- All requests and responses are `application/json; charset=utf-8`.
- **Sizes** are integers in **bytes**. **Speeds** are integers in
  **bytes/second**. Clients format units.
- **File hashes** are ED2K hashes: 32-character lowercase hexadecimal strings.
  They are the canonical identifier for downloads, shared files and search
  results.
- **Booleans** are JSON `true`/`false` (not `0`/`1`).
- **Enums** are lowercase snake_case strings.
- **Timestamps** are ISO 8601 UTC strings (`"2026-06-12T10:30:00Z"`).
- Field names are snake_case, matching the names the daemon already exposes
  (`size_done`, `src_count`, `xfer_all`, …) wherever a current field exists.
- **No server-side sorting or filtering.** The current template keeps sort
  state and status/category filters in the server session; the API returns
  full collections and clients sort/filter locally. This removes a whole
  class of session state and reflected-parameter handling.
- Mutating endpoints use `POST`/`PATCH`/`DELETE` and return either `200` with
  a body or `204` with none. `GET` never mutates (the current template's
  "GET with side effects" pattern is not carried over).

### 2.1 Errors

Errors use standard HTTP status codes with a uniform envelope:

```json
{
  "error": {
    "code": "not_found",
    "message": "No download with hash 31d6cfe0d16ae931b73c59d7e0c089c0"
  }
}
```

| HTTP | `code` | Meaning |
|---|---|---|
| 400 | `bad_request` | Malformed JSON, missing/invalid field |
| 401 | `unauthorized` | Missing, invalid or expired credentials |
| 403 | `forbidden` | Authenticated as guest, endpoint requires admin |
| 404 | `not_found` | Unknown path or unknown resource id (hash, ip:port) |
| 409 | `conflict` | Action not applicable in current state (e.g. resume a non-paused file) |
| 502 | `core_unavailable` | Web server cannot reach the aMule core |

### 2.2 Roles

Two roles, mapping to the two existing passwords:

- **`admin`** — full access.
- **`guest`** — read-only: every `GET` works, every mutating endpoint
  returns `403 forbidden`. This matches the current template, where guest
  sessions render the pages but all commands are disabled.

Each endpoint below is tagged **[read]** (guest allowed) or **[admin]**.

## 3. Authentication

Authentication is based on a single credential: a **signed JWT with a fixed
expiry**. There is no refresh token and no server-side session state — when
the token expires, clients log in again. Expiry is absolute (the token's
`exp` claim); there is **no inactivity/sliding window**, unlike the current
template's session timeout.

Every endpoint (except `POST /auth/login`) requires the token, and every
endpoint accepts it through **both** transports:

1. **Cookie** — `amule_token`, set by the login endpoint
   (`HttpOnly; SameSite=Strict; Max-Age=<seconds until expiry>`). The cookie
   value **is the JWT itself**. Intended for the bundled web frontend, where
   `HttpOnly` keeps the token out of reach of page scripts.
2. **`Authorization: Bearer <token>`** header. Intended for external clients
   and scripts.

Both transports carry the same token and are validated identically. If both
are present, the `Authorization` header wins.

> The legacy template's `amuleweb_session_id` cookie (opaque id, inactivity
> timeout) is unrelated to this API and untouched by it.

### 3.1 `POST /auth/login` — [public]

Request:

```json
{ "password": "secret" }
```

The password is matched against the admin password first, then the guest
password (same rule as the current login form).

Response `200` — also sets the `amule_token` cookie to the same token:

```json
{
  "token": "eyJhbGciOiJIUzI1NiIs...",
  "role": "admin",
  "expires_at": "2026-06-13T10:30:00Z"
}
```

Errors: `401 unauthorized` with a generic "wrong password" message when the
password matches neither credential, `401` with `code: "login_disabled"` when
no password is configured on the daemon (today's "no password specified"
case).

### 3.2 `POST /auth/logout` — [read]

Clears the `amule_token` cookie (`Max-Age=0`). Response `204`. (Replaces
today's "request `login.php` while logged in" logout.)

Because the token is stateless, logout is best-effort: a copy of the JWT kept
elsewhere remains valid until its `exp`. Whether the server additionally
keeps a denylist of logged-out tokens is an implementation decision (§8).

### 3.3 `GET /auth/session` — [read]

Introspection of the presented credentials:

```json
{ "role": "guest", "method": "cookie", "expires_at": "2026-06-12T11:00:00Z" }
```

## 4. Endpoints

### 4.1 Status

#### `GET /status` — [read]

Connection summary — parity with the footer bar (`stats.php` /
`amule_get_stats`) plus the speed fields:

```json
{
  "version": "aMule 2.3.3",
  "ed2k": {
    "status": "connected",
    "id": 123456789,
    "high_id": true,
    "server": {
      "name": "eMule Security",
      "address": "176.103.48.36:4184",
      "users": 154233,
      "files": 15400233
    }
  },
  "kad": {
    "status": "connected",
    "firewalled": false,
    "network": {
      "users": 11233,
      "files": 110023
    }
  },
  "speed": {
    "download": 245760,
    "upload": 30720,
    "download_limit": 0,
    "upload_limit": 51200
  }
}
```

- `version` is the core version string exactly as the daemon reports it
  (today's `amule_get_version`). Display-only; no format is guaranteed.
- `ed2k.status`: `disconnected | connecting | connected` (the current footer's
  three states, derived from the raw id: `0` = disconnected, `0xffffffff` =
  connecting). `server` is `null` unless `status` is `connected`.
- `kad.status`: `disconnected | connected` (Kad has no intermediate state).
  The enum shape matches `ed2k.status` for consistency.
- `id` is the raw ED2K client id; `high_id` is the derived flag
  (`id >= 16777216`) provided so clients don't duplicate the rule. Both are
  only meaningful when `ed2k.status` is `connected` (`id` is otherwise the
  `0`/`0xffffffff` sentinel encoding the state).
- A limit of `0` means "unlimited".

### 4.2 Downloads

#### `GET /downloads` — [read]

```json
{
  "downloads": [
    {
      "hash": "31d6cfe0d16ae931b73c59d7e0c089c0",
      "name": "ubuntu-24.04.iso",
      "link": "ed2k://|file|ubuntu-24.04.iso|...|/",
      "size": 5368709120,
      "size_done": 2684354560,
      "size_xfer": 2690000000,
      "speed": 245760,
      "status": "downloading",
      "priority": "normal",
      "priority_auto": true,
      "category": 0,
      "sources": {
        "total": 25,
        "not_current": 5,
        "transferring": 3,
        "a4af": 2
      },
      "progress": {
        "percent": 50.0,
        "parts": [
          { "state": "done", "from": 0.0, "to": 0.5 },
          { "state": "available", "from": 0.5, "to": 0.8 },
          { "state": "missing", "from": 0.8, "to": 1.0 }
        ]
      }
    }
  ]
}
```

- `status`: `waiting | paused | downloading | error | completing | completed`.
  v0.1 clients only need the first three (the only states the current
  template distinguishes); the rest are reserved.
- `priority`: `low | normal | high | very_high | very_low | release`.
  `priority_auto` is a separate boolean, as in the daemon.
- `category` is the category id (index into `GET /categories`).
- `sources` unpacks today's `src_count*` fields: `total` = `src_count`,
  `not_current` = `src_count_not_curr`, `transferring` = `src_count_xfer`,
  `a4af` = `src_count_a4af`.
- `progress` replaces the server-rendered HTML bar: `percent` (0–100, one
  decimal) plus `parts`, an ordered list of fractional spans with
  `state: done | downloading | available | missing`, carrying the same
  information as the colored bar gif sequence.

> **Scalability note.** v0.1 returns the whole queue on every poll, with the
> client sorting and filtering locally. Future revisions will add
> server-side pagination, sorting and filtering to this endpoint. If polling
> the full objects still doesn't scale, parts of the response can be split
> out — e.g. moving `progress.parts` to a per-file endpoint
> (`GET /downloads/{hash}/parts`) fetched on demand, or adding an events
> endpoint that returns only the changes since the client's last poll.

#### `POST /downloads` — [admin]

Add download(s) from ed2k links (replaces the footer form):

```json
{
  "links": [
    "ed2k://|file|ubuntu-24.04.iso|5368709120|31D6CFE0D16AE931B73C59D7E0C089C0|/"
  ],
  "category": 0
}
```

Response `204`. `category` is optional (default `0`). Multiple links are
accepted in one call, like today's footer field, but as a proper array
instead of string splitting.

#### `POST /downloads/{hash}/pause` — [admin]
#### `POST /downloads/{hash}/resume` — [admin]

Response `204`. `409` if the state transition does not apply.

#### `DELETE /downloads/{hash}` — [admin]

Cancels (removes) the download. Response `204`. This is the current "cancel"
command; the confirmation dialog is a client concern.

#### `PATCH /downloads/{hash}` — [admin]

Partial update of mutable attributes:

```json
{ "priority": "high", "priority_auto": false, "category": 2 }
```

All fields optional; at least one required. Response `200` with the updated
download object. Covers today's `prioup`/`priodown` (clients compute the
target value) plus the `prio`/`setcat` commands the daemon already supports
but the template never exposed.

> **Bulk operations.** The current UI applies one command to N checked files
> by issuing N commands. Clients of this API do the same: one HTTP call per
> hash. A batch endpoint is deliberately deferred (§8).

### 4.3 Uploads

#### `GET /uploads` — [read]

Current upload slots (peers we are sending to):

```json
{
  "uploads": [
    {
      "hash": "31d6cfe0d16ae931b73c59d7e0c089c0",
      "name": "ubuntu-24.04.iso",
      "user_name": "some_peer",
      "xfer_up": 10485760,
      "xfer_down": 524288,
      "speed": 30720
    }
  ]
}
```

`hash` is the ED2K hash of the shared file being uploaded — it cross-links
the row to `GET /shared`, but it is **not** a unique key for this list (the
same file can be uploading to several peers at once). `xfer_up`/`xfer_down`
are session totals exchanged with that peer; `speed` is the current upload
speed to it (today's `xfer_speed`). Uploads have no id and no actions — the
list is informational, as in the current page.

### 4.4 Shared files

#### `GET /shared` — [read]

```json
{
  "shared": [
    {
      "hash": "31d6cfe0d16ae931b73c59d7e0c089c0",
      "name": "ubuntu-24.04.iso",
      "link": "ed2k://|file|ubuntu-24.04.iso|...|/",
      "size": 5368709120,
      "xfer": 10485760,
      "xfer_all": 53687091200,
      "requests": 12,
      "requests_all": 3456,
      "accepts": 4,
      "accepts_all": 789,
      "priority": "normal",
      "priority_auto": true
    }
  ]
}
```

`xfer`/`requests`/`accepts` are the current-session counters, `*_all` the
all-time ones (today's `xfer/xfer_all`, `req/req_all`, `accept/accept_all`).

> **Scalability note.** See the scalability note on `GET /downloads` (§4.2);
> it applies to this endpoint too.

#### `POST /shared/reload` — [admin]

Rescans the shared directories (today's reload button). Response `204`.

#### `PATCH /shared/{hash}` — [admin]

```json
{ "priority": "high", "priority_auto": false }
```

Response `200` with the updated shared-file object. Covers today's
`prioup`/`priodown`/`setprio`.

### 4.5 Servers

Servers are identified by `{ip}:{port}` in the path (they have no hash).

#### `GET /servers` — [read]

```json
{
  "servers": [
    {
      "name": "eMule Security",
      "description": "www.emule-security.org",
      "address": "176.103.48.36:4184",
      "ip": "176.103.48.36",
      "port": 4184,
      "users": 154233,
      "max_users": 200000,
      "files": 12345678,
      "ping": 32,
      "priority": "normal",
      "failed_count": 0,
      "static": false,
      "version": "17.15"
    }
  ]
}
```

- `address` is the display form the daemon provides — it may be a hostname —
  while `ip`/`port` are the canonical identifier used in URLs.
- `max_users` is the server's user capacity, `0` = unknown.
- `ping` is in milliseconds, `0` = not measured yet.
- `priority`: `low | normal | high` — the server's connect preference.
- `failed_count` counts failed connection attempts since the last success.
- `static` is true when the server is pinned in the static list.
- `version` is the server software version string, `""` if unknown.

This is every per-server field the daemon exposes; fields the daemon has no
value for arrive as the defaults given above, never omitted.

#### `POST /servers` — [admin]

Adds a server to the list:

```json
{ "host": "176.103.48.36", "port": 4184, "name": "eMule Security" }
```

`host` is an IP or hostname. `name` is optional and defaults to
`host:port`. Response `204`. Errors: `400` when `host` or `port` is
missing or invalid, `409 conflict` when the daemon refuses the entry
(e.g. already in the list).

#### `POST /servers/update` — [admin]

```json
{ "servers_url": "http://upd.emule-security.org/server.met" }
```

Updates the server list from a `server.met` URL — the ED2K counterpart of
`POST /kad/bootstrap`. Response `204`.

#### `POST /servers/{ip}:{port}/connect` — [admin]

Connects to that server. Response `204`.

#### `DELETE /servers/{ip}:{port}` — [admin]

Removes the server from the list. Response `204`.

#### `POST /servers/disconnect` — [admin]

Network-level disconnect from the current ED2K server (no target). Response
`204`.

### 4.6 Kad

#### `GET /kad` — [read]

```json
{ "status": "connected", "firewalled": false }
```

(Same data as `GET /status`'s `kad` object; provided as its own resource so
the Kad page doesn't over-fetch.)

#### `POST /kad/connect` — [admin]

Two forms, matching the two connect actions of the Kad page:

- Empty body (or `{}`): connect using known peers (today's
  "Connect from known peers").
- `{ "ip": "1.2.3.4", "port": 4672 }`: bootstrap from a specific node.
  `ip` is a dotted-quad string — the API does the packing, clients never see
  the integer encoding the template computes today.

Response `204`.

#### `POST /kad/bootstrap` — [admin]

```json
{ "nodes_url": "https://upd.emule-security.org/nodes.dat" }
```

Updates the nodes list from a `nodes.dat` URL. Response `204`.

#### `POST /kad/disconnect` — [admin]

Response `204`.

### 4.7 Search

The daemon holds **one** search at a time; starting a new search replaces the
previous result set. The API models that directly.

> **Future evolution: concurrent searches (tabs).** The current
> implementation does not allow several searches at once. A future revision
> could lift that with search ids
>
> - `POST /search` would return a tab id for the new search.
> - `GET /search/results` would return the list of open tabs with their ids.
> - `GET /search/results/{id}` would return the results of one tab.
> - `PATCH /search/results/{id}` would cancel a running search or extend it
>   (e.g. "search more" for additional results).
> - `DELETE /search/results/{id}` would close the tab and free the result
>   set on the server.

#### `POST /search` — [admin]

```json
{
  "query": "ubuntu iso",
  "type": "kad",
  "min_size": 1073741824,
  "max_size": 0,
  "availability": 0,
  "extension": "",
  "file_type": ""
}
```

- `type`: `local | global | kad`.
- `min_size`/`max_size` in bytes, `0` = no bound. (Unit multipliers are a
  client concern; the kilo/mega/giga selectors of the current form disappear.)
- `availability`: minimum source count, `0` = no bound.
- `extension`, `file_type`: accepted and forwarded to the daemon; the current
  template sends them empty. Optional, default `""`.

Only `query` is required. Response `202` (search started; results accumulate
asynchronously).

#### `GET /search/results` — [read]

```json
{
  "results": [
    {
      "hash": "31d6cfe0d16ae931b73c59d7e0c089c0",
      "name": "ubuntu-24.04.iso",
      "size": 5368709120,
      "sources": 42,
      "present": false
    }
  ]
}
```

`present` is true when the file is already in the download queue or shared
(the daemon's known-file flag). Clients poll this endpoint while results
arrive (today's "Click here to update the search results" link).

#### `POST /search/results/{hash}/download` — [admin]

```json
{ "category": 0 }
```

Starts downloading that result into the category (`0` default). Response
`204`.

> **Note.** This endpoint may turn out to be unnecessary: a search result
> carries `name`, `size` and `hash`, which is everything needed to build an
> ed2k link (`ed2k://|file|<name>|<size>|<hash>|/`) client-side and call
> `POST /downloads` instead. It is kept in v0.1 because it mirrors the
> daemon's native "download a search result" command; if `POST /downloads`
> proves equivalent in practice, a future revision may drop it.

### 4.8 Categories

#### `GET /categories` — [read]

```json
{
  "categories": [
    { "id": 0, "name": "all" },
    { "id": 1, "name": "Movies" }
  ]
}
```

Category ids are the daemon's category indexes; `0` is the default category.
v0.1 is read-only (the current frontend cannot create or edit categories
either; see §8).

### 4.9 Statistics

#### `GET /stats/tree` — [read]

The full statistics tree (today's `stats_tree.php`), as nested nodes:

```json
{
  "tree": [
    {
      "label": "Transfer",
      "children": [
        { "label": "Uploads: 3", "children": [] },
        {
          "label": "Sessions",
          "children": [ { "label": "Total: 42", "children": [] } ]
        }
      ]
    }
  ]
}
```

Nodes are `label` strings exactly as the daemon emits them (the tree is
free-form and locale-dependent); leaves have an empty `children` array.

> **Note.** It would be desirable to add an `id` field to every node: a
> stable, locale-independent identifier (e.g. `"transfer.sessions.total"`).
> Today the daemon only emits the formatted `label`, so clients have nothing
> stable to key on — ids would let them preserve expand/collapse state
> across polls, translate labels, or pick specific values out of the tree.
> Deferred until the daemon can provide them.

#### `GET /stats/graphs/{graph}` — [read]

`{graph}` is one of `download | upload | connections | kad`. Returns the time
series behind the four PNG graphs the server currently renders
(`amule_stats_download.png`, `_upload`, `_conncount`, `_kad`):

```json
{
  "graph": "download",
  "unit": "bytes_per_second",
  "interval": 5,
  "points": [
    { "t": "2026-06-12T10:29:30Z", "value": 245760 },
    { "t": "2026-06-12T10:29:35Z", "value": 251904 }
  ]
}
```

- `interval` is the sampling period in seconds.
- `unit`: `bytes_per_second` for `download`/`upload`, `count` for
  `connections` and `kad` (kad node count).
- Optional query param `?width=N` limits the response to the most recent `N`
  points (same role as the pixel width of the old PNGs). Default: the
  daemon's full history window.

Clients render the series (canvas/SVG). The dynamic PNG URLs remain available
outside this API for the legacy template but are not part of the contract.

### 4.10 Preferences

#### `GET /preferences` — [read]

Returns every preference the current page can show, in the daemon's groups:

```json
{
  "nick": "http://www.aMule.org",
  "connection": {
    "max_line_down_cap": 7168,
    "max_line_up_cap": 1024,
    "max_down_limit": 0,
    "max_up_limit": 50,
    "slot_alloc": 2,
    "tcp_port": 4662,
    "udp_port": 4672,
    "udp_dis": false,
    "max_file_src": 300,
    "max_conn_total": 500,
    "autoconn_en": true,
    "reconn_en": true,
    "network_ed2k": true,
    "network_kad": true
  },
  "files": {
    "check_free_space": true,
    "min_free_space": 1,
    "extract_metadata": false,
    "ich_en": true,
    "aich_trust": false,
    "preview_prio": false,
    "save_sources": true,
    "resume_same_cat": false,
    "new_files_paused": false,
    "alloc_full": false,
    "alloc_full_chunks": false,
    "new_files_auto_dl_prio": true,
    "new_files_auto_ul_prio": true
  },
  "webserver": {
    "use_gzip": true,
    "autorefresh_time": 60
  }
}
```

Notes:

- `*_en`/`*_dis` flags and the other on/off options are booleans.
- Rate fields (`max_*_limit`, `max_line_*_cap`) keep the daemon's unit
  (kB/s), `slot_alloc` in kB/s per slot, `min_free_space` in MB — these are
  configuration values, not transfer measurements, so they keep their native
  units; the field reference table in the RFC marks the unit of each.
- `preview_prio`, `save_sources`, `resume_same_cat` are settable today but
  have no form control; the API exposes them uniformly.

> **Note.** It may be simpler to flatten the object and use prefixes instead
> of groups (e.g. `connection_max_up_limit`, `files_min_free_space`,
> `webserver_use_gzip`). A flat shape makes `PATCH` trivial (plain key/value
> pairs, no deep merge) at the cost of losing the daemon's native grouping.
> To be decided before the surface is frozen as v1.

#### `PATCH /preferences` — [admin]

Deep-partial update — send only the fields to change:

```json
{ "nick": "myNick", "connection": { "max_up_limit": 80 } }
```

Response `200` with the full updated preferences object. (This fixes by
design the current page's failure mode where an incomplete form submit
zeroes unmentioned options.)

### 4.11 Logs

#### `GET /logs/amule` — [read]

```json
{ "text": "2026-06-12 10:30:01: Connected to eMule Security\n..." }
```

An ordinary JSON response like every other endpoint: `text` carries the log
as one string, exactly as the daemon formats it — JSON encoding is the only
transformation.

#### `DELETE /logs/amule` — [admin]

Clears the log (today's "Reset log"). Response `204`.

#### `GET /logs/serverinfo` — [read]
#### `DELETE /logs/serverinfo` — [admin]

Same contract for the server-info log (server MOTD/details text).

## 5. Endpoint summary

| Method | Path | Role | Purpose |
|---|---|---|---|
| POST | `/auth/login` | public | Get JWT (body + `amule_token` cookie) |
| POST | `/auth/logout` | read | Invalidate credentials |
| GET | `/auth/session` | read | Introspect credentials |
| GET | `/status` | read | Connection + speed summary (footer) |
| GET | `/downloads` | read | Download queue |
| POST | `/downloads` | admin | Add ed2k link(s) |
| POST | `/downloads/{hash}/pause` | admin | Pause |
| POST | `/downloads/{hash}/resume` | admin | Resume |
| DELETE | `/downloads/{hash}` | admin | Cancel |
| PATCH | `/downloads/{hash}` | admin | Priority / category |
| GET | `/uploads` | read | Active upload slots |
| GET | `/shared` | read | Shared files |
| POST | `/shared/reload` | admin | Rescan shared dirs |
| PATCH | `/shared/{hash}` | admin | Priority |
| GET | `/servers` | read | Server list |
| POST | `/servers` | admin | Add server (host:port) |
| POST | `/servers/update` | admin | Update server list from server.met URL |
| POST | `/servers/{ip}:{port}/connect` | admin | Connect to server |
| DELETE | `/servers/{ip}:{port}` | admin | Remove server |
| POST | `/servers/disconnect` | admin | Disconnect from ED2K |
| GET | `/kad` | read | Kad status |
| POST | `/kad/connect` | admin | Connect (known peers or ip:port) |
| POST | `/kad/bootstrap` | admin | Update nodes from URL |
| POST | `/kad/disconnect` | admin | Disconnect Kad |
| POST | `/search` | admin | Start search |
| GET | `/search/results` | read | Poll results |
| POST | `/search/results/{hash}/download` | admin | Download result |
| GET | `/categories` | read | Category list |
| GET | `/stats/tree` | read | Statistics tree |
| GET | `/stats/graphs/{graph}` | read | Graph time series |
| GET | `/preferences` | read | All preferences |
| PATCH | `/preferences` | admin | Update preferences |
| GET | `/logs/amule` | read | aMule log |
| DELETE | `/logs/amule` | admin | Reset log |
| GET | `/logs/serverinfo` | read | Server info log |
| DELETE | `/logs/serverinfo` | admin | Reset server info |

## 6. Mapping from the current template

| Current page / action | API counterpart |
|---|---|
| `login.php` POST `pass` | `POST /auth/login` |
| `login.php` while logged in (logout) | `POST /auth/logout` |
| `stats.php` (footer connection bar) | `GET /status` |
| `amuleweb-main-dload.php` table | `GET /downloads` |
| dload `command=pause/resume/cancel/prioup/priodown` + checkboxes | `POST .../pause`, `POST .../resume`, `DELETE`, `PATCH` per hash |
| dload status/category filter (`command=filter`) | client-side filtering |
| `?sort=` columns (all pages) | client-side sorting |
| footer form `ed2klink` + `selectcat` | `POST /downloads` |
| dload UPLOAD table | `GET /uploads` |
| `amuleweb-main-shared.php` table | `GET /shared` |
| shared `reload/prioup/priodown/setprio` | `POST /shared/reload`, `PATCH /shared/{hash}` |
| `amuleweb-main-servers.php` table | `GET /servers` |
| servers `cmd=connect/remove&ip&port` | `POST /servers/{ip}:{port}/connect`, `DELETE /servers/{ip}:{port}` |
| servers `server_action=disconnect` | `POST /servers/disconnect` |
| `amuleweb-main-search.php` form (`command=search`) | `POST /search` |
| search results table + update link | `GET /search/results` |
| search `command=download` + `targetcat` | `POST /search/results/{hash}/download` |
| `amuleweb-main-kad.php` `kad_action=connect_known/connect_ip/update_url/disconnect` | `POST /kad/connect`, `/kad/bootstrap`, `/kad/disconnect` |
| `amule_stats_*.png` graphs | `GET /stats/graphs/{graph}` |
| `stats_tree.php` | `GET /stats/tree` |
| `amuleweb-main-prefs.php` form (`Submit=Apply`) | `GET` / `PATCH /preferences` |
| `log.php` / `?show=srv` / `?rstlog=1` / `?rstsrv=1` | `GET`/`DELETE /logs/amule`, `/logs/serverinfo` |
| guest login (commands disabled) | `role: "guest"` → `403` on mutations |
| session auto-refresh (`auto_refresh`) | client polling policy |

## 7. Security considerations

- The `amule_token` cookie is `HttpOnly; SameSite=Strict` (the same
  attributes the legacy session cookie uses), which is the CSRF mitigation
  for cookie-authenticated browser clients. Header-authenticated clients are
  immune to CSRF by construction (the header is never attached
  automatically).
- Token expiry is absolute, never extended by activity. A stolen token is
  valid at most until its `exp`; shortening the lifetime bounds the damage
  window (see §8 on the expiry value and optional logout denylist).
- If needed, a global JWT revocation mechanism could be added: a button in
  preferences that rotates the token signing secret on the server,
  invalidating **all** issued JWTs at once (every client must log in again).
  Coarse but simple — it needs no per-token state, unlike a denylist.
- All strings in responses (file names, server names, user names, log text)
  are **raw, unescaped data**. JSON encoding is the only transformation.
  HTML-escaping is strictly a client responsibility — clients must never
  inject these values into the DOM as HTML. (This inverts the current
  template's server-side `htmlspecialchars` approach.)
- A failed login is simply a wrong password: the submitted password is
  checked against both credentials, so there is no "wrong admin password"
  vs "wrong guest password" distinction the server could reveal. The `401`
  response carries a single generic "wrong password" message.
- TLS is out of scope; deployments wanting HTTPS put a reverse proxy in
  front, as documented for the current web server.

## 8. Open questions / deferred

Flagged for the implementation phase or a later revision — not part of the
v0.1 contract:

1. **JWT signing & secret management** — algorithm (HS256 expected) and where
   the signing secret lives/rotates is an implementation decision.
2. **Token expiry value** — fixed lifetime TBD (proposal: 24 h); the contract
   only promises `expires_at` in the login response.
3. **Logout revocation** — whether `POST /auth/logout` also adds the token to
   a server-side denylist until its `exp` (true revocation) or only clears
   the cookie (stateless, the v0.1 baseline).
4. **Batch operations** — `POST /downloads/actions {hashes, action}` style
   endpoints, if per-hash calls prove too chatty.
5. **Category management** — create/edit/delete categories; current frontend
   lacks it.
6. **Rate limiting / login throttling** — desirable against password
   guessing; unspecified in v0.1.
7. **Push channel** — WebSocket or SSE for live updates instead of polling.
8. **Collection scalability** — server-side pagination, sorting and
   filtering for list endpoints (starting with `GET /downloads`); if full
   objects still prove too heavy to poll, split heavy fields into
   per-resource endpoints (e.g. `GET /downloads/{hash}/parts`) or add an
   events endpoint returning only changes since the last poll.
9. **Concurrent searches (tabs)** — search ids on `POST /search`, tab
   listing/results/cancel/close via `/search/results/{id}` (see the note
   in §4.7); blocked today by the daemon's single-search model.
10. **Download peers** — it would be desirable to see the peers a file is
    currently being downloaded from (e.g. `GET /downloads/{hash}/peers`:
    user name, client software, speed, progress), like the source list in
    the desktop GUI. `GET /downloads` only exposes aggregate source counts;
    the current web frontend has nothing like it either.
11. **Download details** — it would be desirable to expose the detailed
    per-file information the desktop GUI shows in its "File details" dialog
    (e.g. `GET /downloads/{hash}`: full path of the .part file, ed2k link,
    alternative file names seen on the network, transfer/session statistics,
    last seen complete, time remaining). `GET /downloads` carries only the
    list-view fields; the current web frontend has no equivalent.
12. **User details** — it would be desirable to expose the detailed per-user
    information the desktop GUI shows in its "User details" dialog (user
    name, user hash, client software and version, ip:port, server,
    obfuscation/secure-ident state, kad support, queue rank, totals
    uploaded/downloaded to that user). The API only shows user names in
    `GET /uploads`; the current web frontend has no equivalent.
