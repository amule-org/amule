# amuleapi v0 — REST reference

This document is the contract for every REST endpoint exposed by the `amuleapi` daemon under the `/api/v0/` prefix. For the SSE stream see [EVENTS.md](EVENTS.md). For first-run setup see [../QUICKSTART-AMULEAPI.md](../QUICKSTART-AMULEAPI.md).

The API is versioned in the path. Breaking changes ship under `/api/v1/`; `/api/v0/` is frozen against any backwards-incompatible change for the lifetime of the v0 surface.

## Base URL and transport

`amuleapi` serves HTTP on the address declared in `amuleapi.conf[Server]/Port` (default `4713`). The server is HTTP-only by design — terminate TLS in a reverse proxy (nginx, Caddy, etc.) for any non-loopback deployment. The cookie is deliberately NOT marked `Secure` so the same Set-Cookie works whether the operator runs amuleapi behind TLS or directly. See QUICKSTART for the full bind-vs-listen story.

JSON in, JSON out. Every request body that carries a payload is `Content-Type: application/json`. Every response that carries a payload is `application/json` unless explicitly noted (the SSE endpoint emits `text/event-stream`).

## Authentication

Two carriers, one token. amuleapi mints HS256 JWTs at `/auth/login` and accepts them either as:

- An `Authorization: Bearer <jwt>` header (SDK / curl / server-to-server clients).
- An HttpOnly session cookie named `amuleapi_token` (browser clients).

If both arrive on the same request, the bearer header wins. The cookie attributes are `HttpOnly; SameSite=Strict; Path=/api/v0`. Cookie lifetime tracks the JWT's `exp` claim (`Max-Age = expires_at - now`).

### Login response shape

The JSON body of `POST /auth/login` deliberately omits the token by default — XSS that can `fetch('/auth/login', ...)` and read the body would defeat the HttpOnly protection. Browser clients work entirely off the Set-Cookie attached to the response. SDK clients that need the token in the body opt in via either:

- `?type=bearer` query string, or
- `Accept: application/jwt` request header.

| Mode | Body keys | Set-Cookie |
|------|-----------|------------|
| Default (cookie) | `role`, `expires_at`, `expires_at_unix` | yes |
| Bearer opt-in | `token`, `role`, `expires_at`, `expires_at_unix`, `jti` | yes (cookie also goes out so a hybrid client can use either) |

### Role model

Two roles, both gated by separate passwords configured via the `--set-admin-pass` / `--set-guest-pass` CLI commands:

- `admin` — full surface, including every mutation (`POST`, `PATCH`, `DELETE`).
- `guest` — read-only surface. Any `admin`-only endpoint returns `403 forbidden`.

A role is implicitly assigned at login based on which password matched; the verified role is encoded in the JWT and surfaced on `/auth/session`.

### Rate limiting

Two per-IP failure counters, both with sliding-window semantics:

- **Login limiter** — drives `/auth/login`. Defaults are `[Auth]/LoginFailureWindowSeconds=60`, `LoginFailureThreshold=5`, `LoginLockoutSeconds=300`. Configurable per-deployment.
- **Generic 401 limiter** — drives every other auth-protected endpoint. Fixed at 30 failures in 60 s → 5-minute lockout. Catches credential-stuffing across the non-login surface.

When the bucket fills, the next request from that IP returns `429 rate_limited` with a `Retry-After: <seconds>` header. The bucket clears on success or when the lockout expires.

### JWT structure

Header: `{"alg":"HS256","typ":"JWT"}`. Payload: `{"role":"admin"|"guest","iat":<unix>,"exp":<unix>,"jti":"<base64url>"}`. The signing secret is auto-generated as 32 random bytes into `${config_dir}/amuleapi-jwt-secret` on first launch (mode 0600). Delete that file and restart to invalidate every issued token. The `jti` claim drives the server-side revocation list (`/auth/logout`).

## Response model

### Success envelope

Each endpoint documents its own response shape under the endpoint section. List endpoints wrap their array under the resource plural name (`{"downloads": [...]}`, `{"shared": [...]}`) so clients can extend the envelope with sibling metadata without breaking JSON-parser pipelines.

### Error envelope

Every non-2xx response carries the same shape:

```json
{
  "error": {
    "code": "machine_readable_token",
    "message": "human-readable explanation"
  }
}
```

`code` is stable across releases; alert on `code`, not on `message`. The catalog at the bottom of this file lists every code emitted by the dispatcher.

### ETag and conditional GET

Every `GET` or `HEAD` that returns `200` carries an `ETag: "<md5-hex>"` header. Clients that re-fetch should send `If-None-Match: "<etag>"` and accept `304 Not Modified` (no body, ETag preserved). The ETag is keyed on `(request target, last refresher snapshot timestamp)` and memoized — repeated GETs against the same path between refresher ticks skip the body hash entirely. `HEAD` returns the same headers (including ETag) with an empty body.

Mutations (`POST`/`PATCH`/`DELETE`) and error responses are never ETag-stamped; the body always ships.

### CORS

If `amuleapi.conf[Server]/AllowCORS=1`:

- Every response carries `Vary: Origin`.
- The origin is echoed in `Access-Control-Allow-Origin` if either the allowlist is empty (any-origin echo) or the request's `Origin` header matches a configured entry.
- Allowed responses also carry `Access-Control-Allow-Credentials: true` and `Access-Control-Expose-Headers: ETag` so cookie-auth clients can read the validator from JS.
- Preflight (`OPTIONS` with `Access-Control-Request-Method`) returns `204` with `Access-Control-Allow-Methods: GET, HEAD, POST, PATCH, DELETE, OPTIONS`, `Access-Control-Allow-Headers: Authorization, Content-Type, If-None-Match, Last-Event-ID`, and `Access-Control-Max-Age: 86400`.

### Path validation

The dispatcher rejects paths containing NUL, encoded NUL (`%00`), encoded `..` (any case of `%2e%2e`), or a literal `..` segment with `400 bad_request` before routing. Defence-in-depth against a future endpoint that admits path captures.

### Request size limits

- HTTP header section: hard cap 16 KiB.
- Request body: hard cap 1 MiB.
- JSON nesting: `>32` opening `{` or `[` tokens → `400 bad_request`. Applies to every body parser and to the JWT header/payload sections of bearer tokens.

Above any of these, the connection is rejected before the handler runs.

## Endpoint catalog

The catalog below is grouped by resource. Each entry documents:

- **Method + path**
- **Auth** — `NONE`, `GUEST` (any authenticated role), or `ADMIN`
- **Query parameters** if any
- **Request body schema** for endpoints that consume one
- **Response status + body**
- **Error codes the endpoint can emit** beyond the universal `unauthorized` / `forbidden` / `rate_limited` (those are documented in §Response model above and are not repeated per endpoint)

Curl examples use `$HOST` for `127.0.0.1:4713` and `$TOKEN` for a previously-issued bearer.

---

### System

#### `GET /api/v0/version`

**Auth:** `NONE` — always accessible, useful for health probes and version negotiation by SDK clients.

```sh
curl -s http://$HOST/api/v0/version
```

**Response:** `200 OK`

```json
{
  "name": "amuleapi",
  "api_version": "v0",
  "amule_version": "2.4.0-29-g..."
}
```

#### `GET /api/v0/status`

**Auth:** `GUEST`

Returns the current connection state, network state, and headline throughput counters.

```sh
curl -s -H "Authorization: Bearer $TOKEN" http://$HOST/api/v0/status
```

**Response:** `200 OK`

```json
{
  "ec_connected": true,
  "ed2k": {
    "state": "connected",
    "low_id": false,
    "server_name": "eMule Server",
    "server_ip": "176.123.5.89",
    "server_port": 4725
  },
  "kad": {
    "state": "connected",
    "firewalled": false
  },
  "speeds": { "download_bps": 4500000, "upload_bps": 50000 },
  "queue": { "upload_queue_length": 12, "total_source_count": 1843 }
}
```

`ec_connected` is `false` while amuleapi can't reach the underlying amuled. Most other endpoints return `503 ec_unavailable` in that state.

**Errors:** `503 ec_unavailable` if amuleapi hasn't received its first EC snapshot yet.

---

### Authentication

#### `POST /api/v0/auth/login`

**Auth:** `NONE`

Mints a JWT for the role that matched the supplied password.

**Query parameters:** `?type=bearer` (optional) — opt into the bearer body response shape. Equivalent to sending `Accept: application/jwt`.

**Body:**

```json
{ "password": "string" }
```

**Default (cookie) request:**

```sh
curl -i -X POST http://$HOST/api/v0/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"password":"adminpass"}'
```

```
HTTP/1.1 200 OK
Set-Cookie: amuleapi_token=eyJhbGciOi...; HttpOnly; SameSite=Strict; Path=/api/v0; Max-Age=86400
Content-Type: application/json

{"role":"admin","expires_at":"2026-06-20T11:00:00Z","expires_at_unix":1781434800}
```

**Bearer opt-in request:**

```sh
curl -s -X POST "http://$HOST/api/v0/auth/login?type=bearer" \
  -H 'Content-Type: application/json' \
  -d '{"password":"adminpass"}'
```

```json
{
  "token": "eyJhbGciOi...",
  "role": "admin",
  "expires_at": "2026-06-20T11:00:00Z",
  "expires_at_unix": 1781434800,
  "jti": "b3iY9oA1tUW2pK..."
}
```

**Errors:**

- `400 bad_request` — body missing/non-object/missing `password`/non-string `password`.
- `401 invalid_credentials` — password didn't match any configured role.
- `429 rate_limited` — login limiter armed; `Retry-After` set.
- `503 login_disabled` — no admin and no guest password configured.

#### `POST /api/v0/auth/logout`

**Auth:** `GUEST`

Adds the bearer's `jti` to the server-side revocation list (TTL = JWT's `exp`) and emits a clear-cookie. Idempotent: a token that is already revoked still gets `200 OK` so a double-tap on a logout button doesn't surface a confusing "session expired" toast.

```sh
curl -i -X POST -H "Authorization: Bearer $TOKEN" http://$HOST/api/v0/auth/logout
```

```json
{ "ok": true }
```

**Response headers:** `Set-Cookie: amuleapi_token=; HttpOnly; SameSite=Strict; Path=/api/v0; Max-Age=0`.

#### `GET /api/v0/auth/session`

**Auth:** `GUEST`

Returns the verified bearer's role and expiry. Useful for SPA bootstrap.

```sh
curl -s -H "Authorization: Bearer $TOKEN" http://$HOST/api/v0/auth/session
```

```json
{
  "role": "admin",
  "exp": "2026-06-20T11:00:00Z",
  "exp_unix": 1781434800,
  "jti": "b3iY9oA1tUW2pK..."
}
```

---

### Downloads

#### `GET /api/v0/downloads`

**Auth:** `GUEST`

Lists the current transfer queue. Completed entries (status `completed`) are excluded by default — they live in amuled's separate "awaiting clear" list and surfacing them inline confuses queue dashboards.

**Query parameters:**

- `include_completed=1|true|yes` — opt completed entries back in.

```sh
curl -s -H "Authorization: Bearer $TOKEN" "http://$HOST/api/v0/downloads"
```

```json
{
  "downloads": [
    {
      "ecid": 17,
      "hash": "8b54a3c2...",
      "name": "ubuntu-26.04-desktop-amd64.iso",
      "ed2k_link": "ed2k://|file|ubuntu...|3825..|8b54...|/",
      "size": 3825205248,
      "size_done": 1142000000,
      "size_xfer": 1102450000,
      "speed_bps": 4500000,
      "status": "downloading",
      "priority": "normal",
      "priority_auto": true,
      "category": 0,
      "sources": {
        "total": 217,
        "not_current": 23,
        "transferring": 8,
        "a4af": 4
      },
      "percent": 29.85
    }
  ]
}
```

The list shape omits `progress.parts` to keep large libraries compact. Use the detail endpoint for per-part state.

**Errors:** `503 ec_unavailable`.

#### `GET /api/v0/downloads/{hash}`

**Auth:** `GUEST`

Detail view for a single partfile. `{hash}` is the 32-char MD4 hex; the dispatcher lower-cases the input so callers can pass either case.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c20fae9e4b9f7e0c2c8c01b6b1"
```

Same envelope as the list item, plus a `progress.parts` array — one entry per ~9.28 MiB chunk with `state` (transferring / complete / empty / corrupt / etc.) and `sources` (count of peers offering that chunk).

**Errors:** `400 bad_request` (hash not 32-char hex), `404 not_found`, `503 ec_unavailable`.

#### `POST /api/v0/downloads`

**Auth:** `ADMIN`

Adds one or more ed2k links to the transfer queue.

**Body** (one of two forms, mutually exclusive):

```json
{ "ed2k_link": "ed2k://|file|...|/", "category": 0 }
```

```json
{ "links": ["ed2k://|file|a|...|/", "ed2k://|file|b|...|/"], "category": 0 }
```

`category` is optional (defaults to 0). Mixing `ed2k_link` and `links` in the same body is rejected `400`.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"links":["ed2k://|file|a|...|/", "ed2k://|file|b|...|/"]}' \
  "http://$HOST/api/v0/downloads"
```

**Response:** `202 Accepted` (all links accepted), `207 Multi-Status` (partial), or `503 ec_unavailable` (every link rejected by EC).

```json
{
  "ok": true,
  "accepted": 1,
  "failed": 1,
  "disconnected": 0,
  "accepted_links": ["ed2k://|file|a|...|/"],
  "failed_links":   ["ed2k://|file|b|...|/"],
  "first_error":    "malformed ed2k link"
}
```

**Errors:** `400 bad_request` (malformed body, both forms used, non-string link), `503 ec_unavailable`.

#### `PATCH /api/v0/downloads/{hash}`

**Auth:** `ADMIN`

Mutates one or more fields of a single partfile.

**Body:** at least one of:

- `status` — `"paused"` or `"resumed"`
- `priority` — `"very_low"` / `"low"` / `"normal"` / `"high"` / `"release"` / `"auto"`
- `category` — uint8

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"status":"paused"}' \
  "http://$HOST/api/v0/downloads/8b54a3c2..."
```

**Response:** `200 OK` — the updated download object (full detail envelope including `progress.parts`).

**Errors:** `400 bad_request` (no recognised field, invalid enum), `400 amuled_rejected`, `404 not_found`, `503 ec_unavailable`.

#### `DELETE /api/v0/downloads/{hash}`

**Auth:** `ADMIN`

Cancels and removes the partfile. The on-disk files are deleted by amuled.

```sh
curl -s -X DELETE -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2..."
```

```json
{ "ok": true, "hash": "8b54a3c2..." }
```

**Errors:** `400 amuled_rejected`, `404 not_found`, `503 ec_unavailable`.

#### `POST /api/v0/downloads/clear_completed`

**Auth:** `ADMIN`

Drops every completed transfer from amuled's "awaiting clear" list in one call.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/clear_completed"
```

```json
{ "ok": true, "cleared": 3, "cleared_hashes": ["...", "...", "..."] }
```

**Errors:** `400 amuled_rejected`, `503 ec_unavailable`.

---

### Clients (peers)

#### `GET /api/v0/clients`

**Auth:** `GUEST`

Lists the peers amuled is currently exchanging with.

**Query parameters:**

- `filter=uploads` — peers we are currently uploading to (`upload_state == "uploading"`).
- `filter=downloads` — peers we are currently downloading from (`download_state == "downloading"`).
- `filter=active` — peers that are either uploading or downloading right now.
- Default (no filter) — every known peer, including queued.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/clients?filter=active"
```

```json
{
  "clients": [
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
  ]
}
```

**Errors:** `400 bad_request` (unknown filter token), `503 ec_unavailable`.

---

### Shared files

#### `GET /api/v0/shared`

**Auth:** `GUEST`

Lists every file the local node is sharing. The `complete_sources` counter is amuled's estimate of how many peers in the swarm hold the file complete.

```sh
curl -s -H "Authorization: Bearer $TOKEN" "http://$HOST/api/v0/shared"
```

```json
{
  "shared": [
    {
      "ecid": 91,
      "hash": "1a2b3c4d...",
      "name": "release-notes.txt",
      "ed2k_link": "ed2k://|file|release-notes.txt|3217|1a2b...|/",
      "size": 3217,
      "priority": "normal",
      "complete_sources": 12
    }
  ]
}
```

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/shared/reload`

**Auth:** `ADMIN`

Equivalent to the desktop client's "Reload" button — amuled re-walks its shared directories and updates the file list.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/shared/reload"
```

```json
{ "ok": true }
```

Returns `202 Accepted`.

**Errors:** `503 ec_unavailable`.

#### `PATCH /api/v0/shared/{hash}`

**Auth:** `ADMIN`

Changes the upload priority of a single shared file.

**Body:**

```json
{ "priority": "very_low" | "low" | "normal" | "high" | "release" }
```

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

---

### Servers (ed2k server list)

#### `GET /api/v0/servers`

**Auth:** `GUEST`

```json
{
  "servers": [
    {
      "ecid": 1,
      "name": "eMule Server",
      "address": "176.123.5.89:4725",
      "port": 4725,
      "users": 312000,
      "files": 75000000,
      "priority": "normal",
      "static": false
    }
  ]
}
```

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/servers`

**Auth:** `ADMIN`

Add a server to amuled's known-server list.

**Body:**

```json
{ "address": "176.123.5.89:4725", "name": "eMule Server" }
```

`name` optional; `address` required and must parse as `host:port`.

**Response:** `201 Created` → `{ "ok": true, "address": "..." }`.

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

#### `POST /api/v0/servers/{ecid}/connect` / `POST /api/v0/servers/{ip}:{port}/connect`

**Auth:** `ADMIN`

Tells amuled to disconnect from its current server and dial the specified one. Two route shapes are equivalent — the address form looks up the ECID by exact `(ip, port)` match against the server cache and delegates to the ECID handler. Hostname-form addresses do NOT resolve here — pass the literal IP.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/servers/176.123.5.89:4725/connect"
```

**Response:** `202 Accepted` → `{ "ok": true, "ecid": 1 }`.

**Errors:** `400 bad_request` (unparseable address/ECID), `404 not_found`, `503 ec_unavailable`.

#### `DELETE /api/v0/servers/{ecid}` / `DELETE /api/v0/servers/{ip}:{port}`

**Auth:** `ADMIN`

Removes the server from amuled's list.

**Response:** `200 OK` → `{ "ok": true, "ecid": 1 }`.

**Errors:** `400 amuled_rejected`, `404 not_found`, `503 ec_unavailable`.

#### `POST /api/v0/servers/update`

**Auth:** `ADMIN`

Tells amuled to fetch the `server.met` from the supplied URL and refresh its list. Same operation the desktop GUI's "Update server list from URL" button drives.

**Body:**

```json
{ "servers_url": "http://example.com/server.met" }
```

The URL must start with `http://` or `https://`; anything else is rejected `400 bad_request`.

**Response:** `202 Accepted` → `{ "ok": true, "servers_url": "..." }`.

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

---

### Categories

amuled's category system lets users tag downloads with one of N user-defined buckets (separate save directory, separate priority, separate color). Category 0 is the default "Uncategorized" and cannot be deleted.

#### `GET /api/v0/categories`

**Auth:** `GUEST`

```json
{
  "categories": [
    {
      "index": 0,
      "name": "All",
      "path": "/home/user/aMule/Incoming",
      "comment": "",
      "color": 0,
      "priority": "normal"
    }
  ]
}
```

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/categories`

**Auth:** `ADMIN`

**Body:**

```json
{
  "name": "Linux ISOs",
  "path": "/home/user/aMule/Incoming/Linux",
  "comment": "Distros only",
  "color": 16711680,
  "priority": "high"
}
```

`name` required; others optional. `color` is a 24-bit RGB integer; `priority` is the same enum the shared-file PATCH accepts.

**Response:** `201 Created` → the new category object.

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

#### `PATCH /api/v0/categories/{index}`

**Auth:** `ADMIN`

Any subset of the POST body fields. `index 0` (the default category) can be patched but not deleted.

#### `DELETE /api/v0/categories/{index}`

**Auth:** `ADMIN`

```json
{ "ok": true, "index": 1 }
```

Deleting `index 0` is rejected by amuled (`400 amuled_rejected`).

---

### Preferences

#### `GET /api/v0/preferences`

**Auth:** `GUEST`

```json
{
  "general": {
    "nickname": "MyNode",
    "user_hash": "abcd...",
    "host_name": "host.example.com",
    "check_new_version": true
  },
  "connection": {
    "max_upload_kbps":   50,
    "max_download_kbps": 0,
    "slot_allocation":   3,
    "tcp_port":          4662,
    "udp_port":          4672,
    "udp_disabled":      false,
    "max_sources_per_file": 250,
    "max_connections":      400,
    "autoconnect": true,
    "reconnect":   true,
    "network_ed2k": true,
    "network_kad":  true
  }
}
```

**Errors:** `503 ec_unavailable`.

#### `PATCH /api/v0/preferences`

**Auth:** `ADMIN`

Body shape mirrors the GET; every field is optional. Fields not present are left unchanged. Subset example:

```json
{ "connection": { "max_upload_kbps": 100 } }
```

**Response:** `200 OK` — full preferences object (post-mutation).

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

---

### Network control

These endpoints drive amuled's connect/disconnect to the ed2k network, the Kad network, or both.

#### `POST /api/v0/networks/connect`

**Auth:** `ADMIN`

Connects both ed2k and Kad. No body.

**Response:** `202 Accepted`.

#### `POST /api/v0/networks/disconnect`

**Auth:** `ADMIN`

**Body:** `{ "network": "ed2k" | "kad" | "both" }` (optional; defaults to `"both"`).

**Response:** `200 OK`.

#### `POST /api/v0/kad/connect` / `POST /api/v0/kad/disconnect`

**Auth:** `ADMIN`

Kad-only equivalents of the network endpoints. No body.

#### `POST /api/v0/kad/bootstrap`

**Auth:** `ADMIN`

Manual Kad bootstrap against a single contact (UDP `nodes.dat`-equivalent without rewriting the file).

**Body:** `{ "server_address": "host:port" }`.

#### `GET /api/v0/kad`

**Auth:** `GUEST`

Standalone view of the Kad subtree from `/status`.

```json
{
  "state": "connected",
  "firewalled": false,
  "network": { "users": 5400000, "files": 1.4e9, "nodes": 2400 }
}
```

---

### Logs

#### `GET /api/v0/logs/amule`

**Auth:** `GUEST`

amuled's general log buffer.

**Query parameters:** `tail=N` — return only the last N lines (default: full buffer).

```json
{ "log": ["2026-06-19 11:00:00: line one", "...line two"] }
```

#### `DELETE /api/v0/logs/amule`

**Auth:** `ADMIN`

Clears the buffer.

```json
{ "ok": true }
```

#### `GET /api/v0/logs/serverinfo` / `DELETE /api/v0/logs/serverinfo`

**Auth:** `GUEST` / `ADMIN`

Same shape; the ed2k server-info log buffer instead of the general log.

---

### Statistics

#### `GET /api/v0/stats/tree`

**Auth:** `GUEST`

A nested object mirroring amuled's "Statistics" tree (transfers, connections, clients, servers, downloads). Cached with a 1 s TTL.

```json
{
  "tree": { "Transfers": { "Uploads": { "Total uploaded": "12.4 GB", "..." : "..." } } }
}
```

**Errors:** `503 ec_unavailable`.

#### `GET /api/v0/stats/graphs/{graph}`

**Auth:** `GUEST`

Time-series points behind the desktop Statistics graphs.

`{graph}` is one of `download`, `upload`, `connections`, `kad`.

```json
{ "points": [[1781430000, 4500000], [1781430010, 4800000]] }
```

Each entry is `[unix_timestamp, value]`.

**Errors:** `404 not_found` (unknown graph name), `503 ec_unavailable`.

---

### Search

The search surface is admin-only because firing a global ed2k search has real network cost.

#### `POST /api/v0/search`

**Auth:** `ADMIN`

Kicks off a new search; the prior search results are wiped.

**Body:**

```json
{
  "query":     "ubuntu desktop iso",
  "type":      "global",
  "file_type": "iso",
  "extension": "iso",
  "min_size":  1000000000,
  "max_size":  5000000000,
  "min_avail": 5
}
```

Only `query` is required. `type` defaults to `"global"`; valid values are `"local"`, `"global"`, `"kad"`.

**Response:** `202 Accepted` → `{ "ok": true, "query": "..." }`.

#### `GET /api/v0/search/results`

**Auth:** `GUEST`

```json
{
  "results": [
    {
      "hash": "8b54a3c2...",
      "name": "ubuntu-26.04-desktop-amd64.iso",
      "size": 3825205248,
      "sources_count": 217,
      "file_type": "iso"
    }
  ]
}
```

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/search/stop`

**Auth:** `ADMIN`

Cancels the in-flight search; cached results stay.

```json
{ "ok": true }
```

#### `POST /api/v0/search/results/{hash}/download`

**Auth:** `ADMIN`

Promote a search result into the transfer queue. Equivalent to clicking "Download" on a desktop search row.

**Body:** `{ "category": 0 }` (optional).

**Response:** `202 Accepted` → `{ "ok": true, "hash": "...", "category": 0 }`.

---

## Error code catalog

Every error code emitted by `/api/v0/*`, sorted by what triggered it. The matching HTTP status is in parentheses.

| Code | Status | Meaning |
|------|--------|---------|
| `method_not_allowed` | 405 | Wrong HTTP verb for the route. |
| `bad_request` | 400 | Body, query, or path-segment validation failed. Body parse depth-cap rejects also surface here. |
| `unauthorized` | 401 | Missing token, bad signature, expired, revoked, or `iat` invariants failed. |
| `invalid_credentials` | 401 | `/auth/login` password didn't match any role. |
| `forbidden` | 403 | Authenticated as `guest` but the endpoint requires `admin`. |
| `not_found` | 404 | Resource doesn't exist (unknown hash, ECID, graph name). |
| `rate_limited` | 429 | Per-IP failure bucket full. `Retry-After: <seconds>` accompanies the response. |
| `login_disabled` | 503 | `/auth/login` reached but no admin AND no guest password configured. |
| `ec_unavailable` | 503 | EC connection not ready yet (cold start, transient amuled restart). |
| `amuled_rejected` | 400 | amuled rejected the EC operation; the message field carries amuled's reason verbatim. |
| `internal` | 500 | Handler threw. The body is generic; details land in the daemon's stderr. |

`message` is human-readable and may change between releases. Pin on `code`.

## Backward compatibility

`/api/v0/` is frozen against any breaking change. Endpoints may add new fields to response bodies and new optional fields to request bodies; clients SHOULD ignore unknown fields. Renaming, removing, or tightening a field's type is a v1 affair.

`POST /api/v0/auth/login`'s default body shape (no token unless `?type=bearer`) IS a change from the very first amuleapi cuts; the legacy "token always in body" behaviour is reachable only via the opt-in. This is documented and committed.
