# amuleapi v0 — REST reference

This document is the contract for every REST endpoint exposed by the `amuleapi` daemon under the `/api/v0/` prefix. For the SSE stream see [EVENTS.md](EVENTS.md). For first-run setup see [../QUICKSTART-AMULEAPI.md](../QUICKSTART-AMULEAPI.md).

The API is versioned in the path. Breaking changes ship under `/api/v1/`; `/api/v0/` is frozen against any backwards-incompatible change for the lifetime of the v0 surface.

## Index

**Cross-cutting concerns**
- [Base URL and transport](#base-url-and-transport)
- [Authentication](#authentication) — [Login response shape](#login-response-shape), [Role model](#role-model), [Rate limiting](#rate-limiting), [JWT structure](#jwt-structure)
- [Response model](#response-model) — [Success envelope](#success-envelope), [List pagination and sorting](#list-pagination-and-sorting), [Bulk mutations and the `results` envelope](#bulk-mutations-and-the-results-envelope), [Error envelope](#error-envelope), [ETag and conditional GET](#etag-and-conditional-get), [CORS](#cors), [Path validation](#path-validation), [Request size limits](#request-size-limits)
- [Error code catalog](#error-code-catalog)
- [Backward compatibility](#backward-compatibility)

**System**
- [`GET /api/v0/version`](#get-apiv0version) — public version probe (+ daemon update-availability)
- [`POST /api/v0/version/check`](#post-apiv0versioncheck) — trigger a daemon-side version check
- [`GET /api/v0/status`](#get-apiv0status) — connection state, network state, headline counters

**Authentication**
- [`POST /api/v0/auth/login`](#post-apiv0authlogin) — mint a JWT, optionally return it in the body
- [`POST /api/v0/auth/logout`](#post-apiv0authlogout) — revoke the bearer's `jti`
- [`GET /api/v0/auth/session`](#get-apiv0authsession) — verified bearer's role and expiry

**Downloads**
- [`GET /api/v0/downloads`](#get-apiv0downloads) — list active queue
- [`GET /api/v0/downloads/{hash}`](#get-apiv0downloadshash) — detail view; `{hash}` is the 32-char MD4 hex hash
- [`GET /api/v0/downloads/{hash}/comments`](#get-apiv0downloadshashcomments) — per-source comments/ratings list (incl. retrieved Kad notes)
- [`POST /api/v0/downloads/{hash}/comments`](#post-apiv0downloadshashcomments) — trigger an on-demand Kad notes lookup
- [`GET /api/v0/downloads/{hash}/filenames`](#get-apiv0downloadshashfilenames) — source-reported filenames + counts
- [`GET /api/v0/downloads/{hash}/a4af`](#get-apiv0downloadshasha4af) — A4AF source list + auto flag
- [`POST /api/v0/downloads/{hash}/a4af`](#post-apiv0downloadshasha4af) — force A4AF source-swapping
- [`POST /api/v0/downloads`](#post-apiv0downloads) — add ed2k link(s)
- [`PATCH /api/v0/downloads`](#patch-apiv0downloads) — bulk pause / resume / priority / category
- [`DELETE /api/v0/downloads`](#delete-apiv0downloads) — bulk cancel + remove
- [`PATCH /api/v0/downloads/{hash}`](#patch-apiv0downloadshash) — pause / resume / priority / category
- [`DELETE /api/v0/downloads/{hash}`](#delete-apiv0downloadshash) — cancel + remove
- [`POST /api/v0/downloads/clear_completed`](#post-apiv0downloadsclear_completed) — bulk-clear completed staging buffer

**Clients (peers)**
- [`GET /api/v0/clients`](#get-apiv0clients) — list peers, optional filter
- [`GET /api/v0/clients/{ecid}`](#get-apiv0clientsecid) — full detail for one peer

**Shared files**
- [`GET /api/v0/shared`](#get-apiv0shared) — list shared files
- [`GET /api/v0/shared/{hash}`](#get-apiv0sharedhash) — detail view; every list field plus shared-detail fields
- [`POST /api/v0/shared/reload`](#post-apiv0sharedreload) — re-walk shared directories
- [`PATCH /api/v0/shared`](#patch-apiv0shared) — bulk change upload priority
- [`PATCH /api/v0/shared/{hash}`](#patch-apiv0sharedhash) — change upload priority

**Servers**
- [`GET /api/v0/servers`](#get-apiv0servers) — list known ed2k servers
- [`POST /api/v0/servers`](#post-apiv0servers) — add server
- [`POST /api/v0/servers/{ecid}/connect`](#post-apiv0serversecidconnect--post-apiv0serversipportconnect) — connect to specific server (ECID or `ip:port`)
- [`DELETE /api/v0/servers/{ecid}`](#delete-apiv0serversecid--delete-apiv0serversipport) — remove server (ECID or `ip:port`)
- [`POST /api/v0/servers/update`](#post-apiv0serversupdate) — refresh from `server.met` URL

**Categories**
- [`GET /api/v0/categories`](#get-apiv0categories) — list categories
- [`POST /api/v0/categories`](#post-apiv0categories) — create
- [`PATCH /api/v0/categories/{index}`](#patch-apiv0categoriesindex) — modify
- [`DELETE /api/v0/categories/{index}`](#delete-apiv0categoriesindex) — remove

**Preferences**
- [`GET /api/v0/preferences`](#get-apiv0preferences) — read all EC-carried preference categories
- [`PATCH /api/v0/preferences`](#patch-apiv0preferences) — update any subset of prefs

**Network control**
- [`POST /api/v0/networks/connect`](#post-apiv0networksconnect) — connect ed2k / kad / both
- [`POST /api/v0/networks/disconnect`](#post-apiv0networksdisconnect) — disconnect ed2k / kad / both
- [`POST /api/v0/kad/bootstrap`](#post-apiv0kadbootstrap) — single-contact Kad bootstrap
- [`GET /api/v0/kad`](#get-apiv0kad) — Kad-only status subtree

**Logs**
- [`GET /api/v0/logs/amule`](#get-apiv0logsamule) — amule log buffer
- [`DELETE /api/v0/logs/amule`](#delete-apiv0logsamule) — clear amule buffer
- [`GET /api/v0/logs/serverinfo`](#get-apiv0logsserverinfo--delete-apiv0logsserverinfo) — server-info log buffer
- [`DELETE /api/v0/logs/serverinfo`](#get-apiv0logsserverinfo--delete-apiv0logsserverinfo) — clear server-info buffer

**Statistics**
- [`GET /api/v0/stats/tree`](#get-apiv0statstree) — full statistics tree
- [`GET /api/v0/stats/graphs/{graph}`](#get-apiv0statsgraphsgraph) — time-series points (`download`, `upload`, `connections`, `kad`)

**Search**
- [`POST /api/v0/search`](#post-apiv0search) — start a search (global / local / kad)
- [`GET /api/v0/search/results`](#get-apiv0searchresults) — current results + progress envelope
- [`POST /api/v0/search/stop`](#post-apiv0searchstop) — cancel in-flight search
- [`POST /api/v0/search/results/{hash}/download`](#post-apiv0searchresultshashdownload) — promote a result into the download queue

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

### List pagination and sorting

The list endpoints — `GET /downloads`, `/clients`, `/shared`, `/servers`, and `/search/results` — accept optional query parameters for server-side windowing and ordering, and always return pagination metadata beside the array:

| Param    | Default          | Notes |
|----------|------------------|-------|
| `limit`  | *(all items)*    | Maximum items to return, capped at `500`. Omitted → the full set (pre-pagination behaviour). Non-integer or negative → `400 bad_request`. |
| `offset` | `0`              | Items to skip before the window. Non-integer or negative → `400 bad_request`. |
| `sort`   | *(native order)* | Field to sort by; endpoint-specific (table below). Unknown field → `400 bad_request`. |
| `order`  | `asc`            | `asc` or `desc`; anything else → `400 bad_request`. |

Sorting is applied to the full filtered set **before** slicing, so pagination is stable across requests (a stable sort — equal keys keep native order). The response adds three sibling keys to the array:

```json
{ "shared": [ ... ], "total": 8431, "offset": 100, "limit": 50 }
```

- `total` — item count after any endpoint-specific filter (e.g. `/clients?filter=`), before slicing.
- `offset` — the offset applied.
- `limit` — the effective page size (equals the number of items returned when `limit` was omitted).

Omitting all four parameters preserves the previous response exactly, plus the additive `total` / `offset` / `limit` keys.

**Sortable fields per endpoint:**

| Endpoint              | `sort` values |
|-----------------------|---------------|
| `GET /downloads`      | `name`, `size`, `progress`, `speed`, `status` |
| `GET /clients`        | `name`, `software` |
| `GET /shared`         | `name`, `size` |
| `GET /servers`        | `name`, `users`, `ping`, `files` |
| `GET /search/results` | `name`, `size`, `sources`, `rating` |

### Bulk mutations and the `results` envelope

Every mutation that operates on more than one item — `POST /downloads`, `PATCH /downloads`, `DELETE /downloads`, `PATCH /shared` — reports one entry per input item under a unified `results` array, so a client submitting N items learns the fate of each rather than an aggregate counter or a first-error-only summary:

```json
{
  "results": [
    { "id": "8b54a3c2…", "ok": true },
    { "id": "0011…",     "ok": false, "error": { "code": "not_found", "message": "no download with that hash" } }
  ]
}
```

- `id` — the item key: the MD4 hash for `/downloads` and `/shared`, or the submitted ed2k link for `POST /downloads`.
- `ok` — whether that single item's mutation succeeded.
- `error` — present only when `ok` is false; same `{code,message}` shape as the top-level [error envelope](#error-envelope).

Processing is **best-effort per item** — each item is an independent EC roundtrip, so a mid-batch failure does not abort the rest. The **HTTP status aggregates** the batch:

| Status | Meaning |
|--------|---------|
| `200 OK` (`202 Accepted` for the async `POST /downloads` add) | every item succeeded |
| `207 Multi-Status` | a mix — inspect each `results[].ok` |
| `503 ec_unavailable` | *every* item failed because the daemon was unreachable |

A malformed **request** (missing/empty `hashes`, an invalid patch field) is still a top-level `400 bad_request` and returns the plain error envelope, not `results`. The `hashes` array is capped at 500 entries.

### Localization and number formatting

The API is a machine contract: **all API text is English and all numbers use the C locale** (a `.` decimal separator, no digit grouping), independent of the `amuleapi`/`amuled` locale or the `--locale` option. Localization is a client concern.

- **Text** — enum-like fields (download status, priorities, upload/connection states) and the `/stats/tree` node label templates cross the wire in English. Strings relayed from amuled (for example `error.message` on an `amuled_rejected` failure, or connect/disconnect `message` fields) are passed through verbatim and are never translated by amuleapi.
- **Numbers** — every JSON number is C-locale. `/stats/tree` values are raw and typed (seconds, bytes, bytes/second, …) so the client does its own formatting and localization; nothing arrives pre-formatted with a locale's decimal separator.

Explicitly **out of scope** (not English-normalized): `GET /api/v0/logs/amule` content — daemon log lines are gettext-translated at the daemon's locale by nature — and user/external data such as file names, category names and comments, and server names and descriptions.

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
  "amule_version": "2.4.0-29-g...",
  "daemon_version": "2.4.0-29-g...",
  "update": {
    "check_enabled": true,
    "checked": true,
    "latest_version": "3.0.1",
    "update_available": false,
    "last_checked": 1783675590
  }
}
```

| Field | Meaning |
| --- | --- |
| `name` | Always `"amuleapi"`. |
| `api_version` | REST contract version served on this path (`"v0"`). |
| `amule_version` | amuleapi's **own** build version. |
| `daemon_version` | Version of the **connected amuled**, from the EC handshake. Empty string when EC is not (yet) connected, or when the daemon is old enough not to advertise it. Normally equal to `amule_version` (both are built from the same source tree), but they can differ if a mismatched amuleapi is pointed at a different amuled. |
| `update` | Update-availability, **relayed from the connected daemon** — amuleapi never contacts GitHub itself. See the sub-table. |

The `update` object:

| Field | Meaning |
| --- | --- |
| `check_enabled` | `true` only when the daemon can check **and** is configured to: built with `ENABLE_VERSION_CHECK` **and** its `NewVersionCheck` preference on. `false` for OS-package builds, the preference off, or a pre-3.1 daemon. When `false`, a client should show nothing. |
| `checked` | `true` once the daemon has completed at least one check this session (so `latest_version` is known). The daemon checks at startup; use `POST /api/v0/version/check` to trigger a fresh one. |
| `latest_version` | Latest release string (e.g. `"3.0.1"`); empty string when not yet checked or unavailable. |
| `update_available` | `true` when a newer release exists, `false` when up to date, `null` when unknown (not yet checked or disabled). |
| `last_checked` | Unix time (seconds) the last check completed; `null` when never checked. Useful because checks are startup-only unless re-triggered. |

#### `POST /api/v0/version/check`

**Auth:** `ADMIN`

Triggers an on-demand version check **on the daemon** (amuleapi does not fetch GitHub itself). Fire-and-forget: the request returns as soon as the daemon accepts it, and the result appears on a subsequent `GET /api/v0/version` once the async check completes. Throttled by the daemon to respect GitHub's rate limit.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" http://$HOST/api/v0/version/check
```

**Response:** `202 Accepted`

```json
{ "status": "started" }
```

**Errors:**

| Status | `error.code` | When |
| --- | --- | --- |
| `409` | `update_check_unavailable` | The daemon can't check (`update.check_enabled` is `false`). |
| `429` | `update_check_throttled` | A check ran too recently; retry shortly. |
| `503` | `ec_unavailable` | The EC round-trip to amuled failed. |

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
    "server_ip": "203.0.113.5",
    "server_port": 4242
  },
  "kad": {
    "state": "connected",
    "firewalled": false,
    "network": { "users": 5400000, "files": 1400000000, "nodes": 2400 }
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
      "kad_search_running": false
    }
  ]
}
```

`status` is one of `"downloading"`, `"waiting"`, `"hashing"`, `"allocating"`, `"paused"`, `"stopped"`, `"completing"` or `"completed"`. `"stopped"` is a paused file that has also dropped all its sources and reset its Kad source search (set via `PATCH` `status:"stopped"`); it is distinct from `"paused"`, which retains its sources.

`priority` is the download priority — one of `"low"`, `"normal"` or `"high"` — and `priority_auto` is `true` when amuled is deriving that level automatically. Downloads never report `very_low` or `release`; those are shared/upload-side levels only. A file that is simultaneously downloading and shared carries two independent priorities: this download priority, and the upload priority reported by [`GET /api/v0/shared`](#get-apiv0shared). Changing one does not affect the other.

The list shape omits `progress.parts` to keep large libraries compact. Use the detail endpoint for per-part state.

`kad_search_running` is `true` while an on-demand Kad notes lookup is in flight for the file (started by [`POST /downloads/{hash}/comments`](#post-apiv0downloadshashcomments)); it flips back to `false` when the lookup finishes. Because it lives on the download object, a client can watch the `download_updated` SSE event for the start → finish transition instead of polling.

The SSE `download_added` / `download_updated` event payload matches this object byte-for-byte.

**Errors:** `503 ec_unavailable`.

#### `GET /api/v0/downloads/{hash}`

**Auth:** `GUEST`

Detail view for a single partfile. `{hash}` is the 32-char MD4 hex hash (case-insensitive).

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c20fae9e4b9f7e0c2c8c01b6b1"
```

Same envelope as the list item, plus the detail-only fields below (all omitted from the `GET /downloads` list to keep it lean):

| Field | Type | Meaning |
|---|---|---|
| `progress.parts` | array | One entry per ~9.28 MiB chunk: `{ "state": string, "sources": int }` — `state` is `transferring`/`complete`/`empty`/`corrupt`/…, `sources` counts peers offering that chunk. |
| `last_seen_complete` | int | Unix ts a complete copy was last seen across sources; `0` = never/unknown. |
| `last_changed` | int | Unix ts the partfile last received data. |
| `download_active_time` | int | Seconds spent actively downloading. |
| `available_part_count` | int | Number of parts available across the current sources. |
| `part_count` | int | Total parts, `ceil(size / 9.28 MiB)`. |
| `remaining_time` | int | ETA in seconds; `-1` when stalled or paused (speed ≈ 0). |
| `hashing_progress` | int | Index of the part currently being hashed (0 when idle). |
| `lost_to_corruption` | int | Bytes discarded to corruption. |
| `gained_by_compression` | int | Bytes saved by on-the-wire compression. |
| `saved_by_ich` | int | Packets recovered by Intelligent Corruption Handling. |
| `aich_hash` | string | AICH master hash (hex); `""` if not yet computed. |
| `met_file` | string | The partfile's on-disk basename (e.g. `001.part`). |
| `partmet_id` | int | Numeric partfile id. |
| `queued_count` | int | Clients waiting on this file's upload queue. |
| `comment` | string | The user's own comment on this file (`""` if none). |
| `rating` | int | The user's own rating, `0`–`5` (`0` = unrated). See the [rating scale](#get-apiv0downloadshashcomments). |
| `a4af_auto` | bool | Whether automatic A4AF source-swapping is on for this file. See [A4AF](#get-apiv0downloadshasha4af). |
| `media` | object | Audio/video metadata — see [Media metadata](#media-metadata). **Omitted entirely** when the file has no probed metadata. |

**Errors:** `404 not_found` (no partfile with that hash), `503 ec_unavailable`.

##### Media metadata

The `media` object (on both `GET /downloads/{hash}` and `GET /shared/{hash}`) carries the audio/video metadata amuled probed for the file. It is **omitted entirely** when the file has not been probed (a non-media file, or one probing hasn't reached yet).

```json
"media": {
  "length_s": 5400,
  "bitrate": 1500,
  "codec": "h264",
  "artist": "…",
  "album": "…",
  "title": "…"
}
```

| Field | Type | Meaning |
|---|---|---|
| `length_s` | int | Duration in seconds. |
| `bitrate` | int | Bitrate (kbps). |
| `codec` | string | Codec identifier (e.g. `"h264"`). |
| `artist` / `album` / `title` | string | Tag metadata; `""` when the file carries none. |

#### `GET /api/v0/downloads/{hash}/comments`

**Auth:** `GUEST`

The comments and ratings this download's **sources** report for the file (the desktop "Show all comments" list). Downloads-only — a completed/shared file has no live source list.

The list also includes any **Kad community notes** retrieved on demand via `POST` on this same path (see below). A Kad note's `username` is the responding node's IP address when the note carries one, otherwise the placeholder `Kad user`.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2…/comments"
```

```json
{
  "count": 2,
  "kad_search_running": false,
  "comments": [
    { "username": "alice",    "filename": "Some.Movie.mkv", "rating": 5, "comment": "great quality" },
    { "username": "Kad user", "filename": "some_movie.avi", "rating": 4, "comment": "" }
  ]
}
```

`kad_search_running` is `true` while an on-demand Kad notes lookup (triggered by the `POST` below) is in flight; poll until it returns to `false` to know the lookup finished. Kad notes appear as ordinary entries whose `username` is the responding node's IP (or `Kad user` when the note carries no IP).

A per-source `rating` of `-1` means the source left a comment but no rating. Rating scale (from the desktop `GetRateString()`):

| value | meaning |
|---|---|
| 0 | Not rated |
| 1 | Invalid / Corrupt / Fake |
| 2 | Poor |
| 3 | Fair |
| 4 | Good |
| 5 | Excellent |

**Errors:** `404 not_found` (no download with that hash), `503 ec_unavailable`.

#### `POST /api/v0/downloads/{hash}/comments`

**Auth:** `USER`

Trigger an on-demand **Kad notes** lookup for this download (the desktop "Get from Kad" button). aMule asks the Kad network for community ratings/comments keyed on the file hash. The lookup is **asynchronous** on the daemon (it can take up to ~45 s); this call returns immediately with `202 Accepted`, and the retrieved notes then show up in the `GET` list above. Poll the `GET` endpoint to observe them arrive.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2…/comments"
```

```json
{ "status": "kad_search_started" }
```

**Errors:** `404 not_found` (no download with that hash), `503 ec_unavailable`, `400 amuled_rejected` (daemon refused, e.g. Kad not connected).

#### `GET /api/v0/downloads/{hash}/filenames`

**Auth:** `GUEST`

The distinct filenames this download's **sources** report for it, each with how many sources use that name (the desktop "File Names" list). Downloads-only — needs a live source list. Pair it with `PATCH /downloads/{hash}` `{ "name": … }` to implement the desktop "Takeover" flow (pick a source name, then rename).

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2…/filenames"
```

```json
{
  "filenames": [
    { "name": "Some.Movie.2024.mkv", "count": 7 },
    { "name": "some_movie.mkv", "count": 2 }
  ]
}
```

**Errors:** `404 not_found` (no download with that hash), `503 ec_unavailable`.

#### `GET /api/v0/downloads/{hash}/a4af`

**Auth:** `GUEST`

The download's **A4AF** (asked-for-another-file) sources — peers that hold this file but are currently serving another. Downloads-only.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2…/a4af"
```

```json
{ "a4af_auto": false, "sources": [ 1234, 5678 ] }
```

`sources` are client ECIDs, joinable against [`GET /clients`](#get-apiv0clients)'s `client_ecid`. The scalar `sources.a4af` count on the download object is unchanged.

**Errors:** `404 not_found` (no download with that hash), `503 ec_unavailable`.

#### `POST /api/v0/downloads/{hash}/a4af`

**Auth:** `ADMIN`

Force A4AF source-swapping for this download. Downloads-only.

**Body:** `{ "action": "<action>" }`

| action | Effect |
|---|---|
| `swap_this` | Make other files' A4AF sources take over **this** file. |
| `swap_this_auto` | Toggle automatic A4AF swapping for this file. |
| `swap_others` | Release this file's sources to the other files that want them. |

**Response:** `200 OK` — the post-action A4AF view (same shape as the `GET`).

**Errors:** `400 bad_request` (missing or unknown `action`), `400 amuled_rejected`, `404 not_found`, `503 ec_unavailable`.

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

**Response:** `202 Accepted` (all links accepted — the add is asynchronous: amuled allocates and hashes the partfile before it surfaces in `GET /downloads`, typically within one refresher tick), `207 Multi-Status` (partial), or `503 ec_unavailable` (every link blocked by an EC disconnect). Per-item outcomes use the shared [bulk `results` envelope](#bulk-mutations-and-the-results-envelope), keyed by the submitted link:

```json
{
  "results": [
    { "id": "ed2k://|file|a|...|/", "ok": true },
    { "id": "ed2k://|file|b|...|/", "ok": false,
      "error": { "code": "amuled_rejected", "message": "malformed ed2k link" } }
  ]
}
```

**Errors:** `400 bad_request` (malformed body, both forms used, non-string link, link not starting with `ed2k://`), `503 ec_unavailable`.

#### `PATCH /api/v0/downloads`

**Auth:** `ADMIN`

Bulk pause/resume, priority, or category change over multiple downloads — the same patch applied to every listed hash.

**Body:** `{ "hashes": ["<md4>", …], … }` — a non-empty `hashes` array (max 500) plus at least one of the single-item PATCH fields: `status` (`"paused"` | `"resumed"` | `"stopped"`), `priority` (`low` | `normal` | `high` | `auto`), `category` (integer 0–255).

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"hashes":["8b54a3c2…","0a1b2c3d…"],"priority":"high"}' "http://$HOST/api/v0/downloads"
```

**Response:** the [bulk `results` envelope](#bulk-mutations-and-the-results-envelope) (`200` all ok / `207` partial / `503`), keyed by hash. Per-item `error.code` is `not_found`, `amuled_rejected`, or `ec_unavailable`.

**Errors:** `400 bad_request` (missing/empty `hashes`, no patch field present, invalid field value), `503 ec_unavailable`.

#### `DELETE /api/v0/downloads`

**Auth:** `ADMIN`

Bulk cancel + remove of active downloads (deletes each `.part`/`.met` from disk). A completed entry is rejected per-item with `completed_use_clear_completed` — clear those via `POST /downloads/clear_completed`.

**Body:** `{ "hashes": ["<md4>", …] }` (non-empty, max 500).

**Response:** the [bulk `results` envelope](#bulk-mutations-and-the-results-envelope), keyed by hash. Per-item `error.code` is `not_found`, `completed_use_clear_completed`, `amuled_rejected`, or `ec_unavailable`.

**Errors:** `400 bad_request` (missing/empty `hashes`), `503 ec_unavailable`.

#### `PATCH /api/v0/downloads/{hash}`

**Auth:** `ADMIN`

Mutates one or more fields of a single partfile. `{hash}` is the 32-char MD4 hex hash (case-insensitive).

**Body:** at least one of:

- `status` — `"paused"`, `"resumed"` or `"stopped"`. `"paused"` halts transfer but keeps the file's sources; `"stopped"` additionally drops all known sources and resets the Kad source search (a stopped file must rediscover sources from scratch on resume); `"resumed"` clears either state. A stopped file reports `status: "stopped"` in the download object (see [`GET /downloads`](#get-apiv0downloads)).
- `priority` — `"low"` / `"normal"` / `"high"` / `"auto"`. Downloads support only these levels; the daemon clamps any other value to `normal`. (Shared files support the wider `very_low` … `release` set — see [`PATCH /shared/{hash}`](#patch-apiv0sharedhash).)
- `category` — uint8
- `comment` + `rating` — set the file's comment (string, ≤ 50 chars) and rating (integer `0`–`5`). Must be sent **together**; only settable when the partfile is also shared (≥ 1 complete chunk), else `409 not_shared`. Primarily a shared-file action — see [`PATCH /shared/{hash}`](#patch-apiv0sharedhash).
- `name` — rename the file (string). Must be non-empty and contain no path separators (`/` or `\`). See the [Takeover flow](#get-apiv0downloadshashfilenames).

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"status":"paused"}' \
  "http://$HOST/api/v0/downloads/8b54a3c2..."
```

**Response:** `200 OK` — the updated download object (full detail envelope including `progress.parts`).

**Errors:** `400 bad_request` (no recognised field, invalid enum, or `comment`/`rating` sent alone), `409 not_shared` (comment/rating on a non-shared file), `400 amuled_rejected`, `404 not_found`, `503 ec_unavailable`.

#### `DELETE /api/v0/downloads/{hash}`

**Auth:** `ADMIN`

Cancels an **active** partfile and deletes its on-disk data. `{hash}` is the 32-char MD4 hex hash (case-insensitive). amuled runs `EC_OP_PARTFILE_DELETE` → `CPartFile::Delete()`, which removes the `.part`, `.part.met`, and `.met.bak` files and adds the hash to its `canceledfiles` list (so re-adding the same ed2k link is silently refused until the operator clears that list out-of-band). Completed entries are out of scope; use [`POST /downloads/clear_completed`](#post-apiv0downloadsclear_completed) instead.

```sh
curl -s -X DELETE -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2..."
```

```json
{ "ok": true, "hash": "8b54a3c2..." }
```

**Errors:** `400 amuled_rejected`, `404 not_found`, `409 completed_use_clear_completed`, `503 ec_unavailable`.

#### `POST /api/v0/downloads/clear_completed`

**Auth:** `ADMIN`

Acks one or more entries in amuled's post-completion notification staging buffer. The on-disk file in the Incoming directory stays in place; this endpoint only clears amuled's "completed transfers awaiting acknowledgement" list. Active partfiles are out of scope; use [`DELETE /api/v0/downloads/{hash}`](#delete-apiv0downloadshash) instead.

Two request shapes share this endpoint:

```sh
# Bulk: no body. Clears every completed entry in one EC roundtrip.
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/clear_completed"

# Per-entry: clear a single completed entry by hash.
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"hash": "8b54a3c2..."}' \
  "http://$HOST/api/v0/downloads/clear_completed"
```

The response envelope is identical for both shapes:

```json
{ "ok": true, "cleared": 3, "cleared_hashes": ["...", "...", "..."] }
```

Bulk form returns `200 OK` with `cleared: 0` and no `cleared_hashes` field when nothing matches (no-op success, distinguishable from an amuled rejection). Per-entry form returns `404 not_found` if the hash doesn't exist and `409 not_completed` if it exists but isn't on the completed staging list (active partfile — caller probably wants `DELETE /downloads/{hash}` instead).

**Errors:** `400 amuled_rejected`, `400 bad_request` (malformed body or non-string `hash`), `404 not_found`, `409 not_completed`, `503 ec_unavailable`.

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
      "client_ecid": 4382,
      "client_name": "AnonymousPeer",
      "user_hash": "1f2e3a...",
      "ip": "203.0.113.42",
      "country_code": "de",
      "port": 4662,
      "software": "eMule",
      "software_version": "0.50a",
      "os_info": "Linux",
      "upload_state": "uploading",
      "download_state": "idle",
      "ident_state": "verified",
      "download_file_name": "",
      "upload_file_hash": "8b54a3c20fae9e4b9f7e0c2c8c01b6b1",
      "download_file_hash": "",
      "xfer": { "up_session": 22000000, "down_session": 0, "up_total": 452000000, "down_total": 189000000 },
      "upload_speed_bps": 22000,
      "download_speed_bps": 0,
      "queue_waiting_position": 0,
      "remote_queue_rank": 0,
      "score": 150,
      "obfuscation_status": "obfuscated",
      "friend_slot": false
    }
  ]
}
```

`client_ecid` identifies the remote *peer*, not a file — it's the URL key for [`GET /api/v0/clients/{ecid}`](#get-apiv0clientsecid) and the identity carried in `client_removed` SSE payloads. `user_hash` is the peer's stable identity *when published* (peers without SecIdent or in their first session don't have one), so `client_ecid` is the always-populated handle.

`upload_file_hash` / `download_file_hash` are the 32-char MD4 hex hashes of the partfile or shared file the peer is currently transferring with — directly resolvable against [`/api/v0/downloads/{hash}`](#get-apiv0downloadshash) (in-progress) or the corresponding entry in [`/api/v0/shared`](#get-apiv0shared) by `.hash`. Either field can be empty when the peer is queued / idle in that direction. `download_file_name` is the filename the peer advertised in `OP_REQFILENAMEANSWER` and is populated only while we're actively downloading from them.

`software` and `software_version` are locale-independent, per the API's English-only contract. A peer the daemon could not identify reports `"software": "unknown"` and `"software_version": "unknown"` — a lowercase sentinel, never a daemon-localized string (the daemon's own version formatting is gettext-translated and is deliberately not surfaced here). `os_info` is the peer's *own* self-reported OS string (raw external data, not normalized by amuled) and is frequently empty, since most clients don't send it.

`country_code` is the peer's ISO 3166-1 alpha-2 country code (lowercase, e.g. `"de"`), resolved server-side from the peer IP by the daemon's GeoIP database. It is an empty string when GeoIP is disabled or unsupported by the build, or when the IP does not resolve — render the flag and localized country name client-side from the code.

**Errors:** `400 bad_request` (unknown filter token), `503 ec_unavailable`.

---

#### `GET /api/v0/clients/{ecid}`

**Auth:** `GUEST`

Returns the full detail object for a single peer — every field [`GET /clients`](#get-apiv0clients) returns for that peer, **plus** the detail-only fields below. `{ecid}` is the peer's `client_ecid` (the EC connection id). Bare object, no list envelope.

`ecid`, not `user_hash`, is the resource key: not every peer has a hash (unidentified / some LowID / eDonkey peers expose an empty one), a hash is not unique among a peer's simultaneous connections, and it is unauthenticated unless the peer uses Secure Identification. `ecid` is always present and unique per live connection. Trade-off: `ecid` is reassigned when amuled restarts, so a detail URL is **not** stable across restarts — use the `user_hash` field for a durable reference.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/clients/4382"
```

```json
{
  "client_ecid": 4382,
  "client_name": "AnonymousPeer",
  "user_hash": "1f2e3a...",
  "ip": "203.0.113.42",
  "country_code": "de",
  "port": 4662,
  "software": "eMule",
  "software_version": "0.50a",
  "os_info": "Linux",
  "upload_state": "uploading",
  "download_state": "idle",
  "ident_state": "verified",
  "download_file_name": "",
  "upload_file_hash": "8b54a3c20fae9e4b9f7e0c2c8c01b6b1",
  "download_file_hash": "",
  "xfer": { "up_session": 22000000, "down_session": 0, "up_total": 452000000, "down_total": 189000000 },
  "upload_speed_bps": 22000,
  "download_speed_bps": 0,
  "queue_waiting_position": 0,
  "remote_queue_rank": 0,
  "score": 150,
  "obfuscation_status": "obfuscated",
  "friend_slot": false,
  "user_id_hybrid": 3232238090,
  "high_id": true,
  "server_ip": "203.0.113.9",
  "server_port": 4242,
  "server_name": "eD2K Test Server",
  "kad_port": 4672,
  "source_origin": "kad",
  "upload_file_name": "example-distribution.iso",
  "available_parts": 42,
  "mod_version": "",
  "view_shared_disabled": false,
  "is_friend": false,
  "dl_up_modifier": 1.0,
  "part_progress_percent": 87.5
}
```

The detail fields mirror the desktop "Client Details" modal. `user_id_hybrid` is the peer's hybrid eD2k id; `high_id` is `true` for a HighID peer (id ≥ `0x1000000`) and `false` for LowID. `server_ip` / `server_port` / `server_name` describe the eD2k server the peer connects through (`server_ip` is `""` when unknown). `kad_port` is non-zero when the peer is reachable on Kad. `source_origin` is how the peer was discovered — `"server"` / `"kad"` / `"source_exchange"` / `"passive"` / `"link"` / `"source_seeds"` / `"search_result"`. `upload_file_name` is the partfile the peer is downloading **from us** — present only while we're uploading to them. `available_parts` is the count of parts the peer holds of the linked file; `mod_version` is the peer's client-mod string (often `""`); `view_shared_disabled` is `true` when the peer forbids browsing its shared files. `is_friend` is `true` when the peer is in your friends list (`CUpDownClient::IsFriend()`) — **distinct** from `friend_slot`, which is a *reserved upload slot* granted to a peer and can be set for non-friends. `dl_up_modifier` is the upload score modifier the GUI labels "DL/UP modifier" (`GetScoreRatio()`). `part_progress_percent` is the peer's completeness of the file we are downloading **from** them (`available_parts` over that file's part count) and is **omitted** when there is no linked download or the part count is unknown.

> `is_friend` and `dl_up_modifier` ride two EC tags added for this endpoint. A webapi built against a newer core talking to an **older** amuled that doesn't send them degrades gracefully — `is_friend` reads `false` and `dl_up_modifier` reads `0`.

**Errors:** `400 bad_request` (`{ecid}` is not a non-negative integer), `404 not_found` (no peer with that ecid in the current snapshot), `405 method_not_allowed` (non-GET), `503 ec_unavailable`.

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
      "hash":             "1a2b3c4d...",
      "name":             "release-notes.txt",
      "ed2k_link":        "ed2k://|file|release-notes.txt|3217|1a2b...|/",
      "size":             3217,
      "priority":         "normal",
      "priority_auto":    false,
      "complete_sources": 12,
      "xfer":     { "session": 5242880,  "total": 314572800 },
      "requests": { "session": 42,       "total": 1837 },
      "accepts":  { "session": 18,       "total": 921 }
    }
  ]
}
```

`xfer.session` / `xfer.total` are bytes uploaded during the current amuled process vs over the file's lifetime. `requests` counts how many peers have asked for the file; `accepts` counts how many of those requests were granted an upload slot. The `session` counters reset on amuled restart; `total` is persisted in `known.met`.

`priority` is the upload priority — `"very_low"` / `"low"` / `"normal"` / `"high"` / `"release"` — and `priority_auto` is `true` when amuled is deriving that level automatically from the upload queue. This mirrors the `/downloads` shape (base `priority` + separate `priority_auto` flag); on an auto file `priority` reports the current derived level, not the literal string `"auto"`. For a file that is both downloading and shared this upload priority is independent of the download priority reported by [`GET /api/v0/downloads`](#get-apiv0downloads).

The SSE `shared_added` / `shared_updated` event payload matches this object byte-for-byte, so a subscriber that received `shared_updated` does not need to re-GET to see the moved counters.

**Errors:** `503 ec_unavailable`.

#### `GET /api/v0/shared/{hash}`

**Auth:** `GUEST`

Detail view for a single shared file. `{hash}` is the 32-char MD4 hex hash (case-insensitive). Returns every field of the [`GET /shared`](#get-apiv0shared) list item plus the detail-only fields below — one call for everything about a shared file. The list endpoint is unchanged.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/shared/8b54a3c20fae9e4b9f7e0c2c8c01b6b1"
```

| Field | Type | Meaning |
|---|---|---|
| `file_type` | string | Category token derived from the extension, lowercased: `"audio"`, `"videos"`, `"archives"`, `"cd-images"`, `"pictures"`, `"texts"`, `"programs"`, or `"any"` for unknown. |
| `share_ratio` | number | `xfer.total / size`; `0` when `size == 0`. |
| `path` | string | Directory path of the on-disk file, or `"[PartFile]"` when the shared file is still an incomplete partfile. |
| `complete_sources_range` | object | `{ "low": int, "high": int }` — the estimated full-copy source range behind the scalar `complete_sources`. |
| `aich_hash` | string | AICH master hash (hex); `""` if not yet computed. |
| `part_count` | int | Total parts, `ceil(size / 9.28 MiB)`. |
| `queued_count` | int | Clients waiting on this file's upload queue. |
| `comment` | string | The user's own comment on this file (`""` if none). |
| `rating` | int | The user's own rating, `0`–`5` (`0` = unrated). |
| `media` | object | Audio/video metadata — see [Media metadata](#media-metadata). **Omitted entirely** when the file has no probed metadata. |

**Errors:** `404 not_found` (no shared file with that hash), `503 ec_unavailable`.

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

#### `PATCH /api/v0/shared`

**Auth:** `ADMIN`

Bulk upload-priority change over multiple shared files — the same `priority` applied to every listed hash.

**Body:** `{ "hashes": ["<md4>", …], "priority": "<level>" }` — a non-empty `hashes` array (max 500) plus a required `priority` (`very_low` | `low` | `normal` | `high` | `release` | `auto`).

**Response:** the [bulk `results` envelope](#bulk-mutations-and-the-results-envelope) (`200` all ok / `207` partial / `503`), keyed by hash. Per-item `error.code` is `not_found`, `amuled_rejected`, or `ec_unavailable`.

**Errors:** `400 bad_request` (missing/empty `hashes`, missing/invalid `priority`), `503 ec_unavailable`.

#### `PATCH /api/v0/shared/{hash}`

**Auth:** `ADMIN`

Changes the upload priority and/or the comment+rating of a single shared file. `{hash}` is the 32-char MD4 hex hash (case-insensitive). The body must include at least one of `priority` or the `comment`+`rating` pair.

**Body:**

```json
{
  "priority": "very_low" | "low" | "normal" | "high" | "release" | "auto",
  "comment":  "<string, ≤ 50 chars>",
  "rating":   0
}
```

Send a bare priority level to pin it (the file's `priority_auto` becomes `false`). Send `"auto"` to hand level selection to amuled — it derives the level from the upload queue, and `GET /api/v0/shared` then reports the derived base `priority` with `priority_auto: true`. The combined `"*_auto"` strings are not accepted as input, since `"auto"` is the level the daemon computes rather than one the caller pins.

`comment` and `rating` must be sent **together** (both or neither) — the daemon writes them as one atomic operation. `comment` is capped at 50 characters; `rating` is an integer `0`–`5`. Setting them requires the file to be shared. The same fields are accepted on [`PATCH /downloads/{hash}`](#patch-apiv0downloadshash) for a downloading file that is also shared.

`name` renames the file — a non-empty string with no path separators (`/` or `\`, rejected to prevent the rename escaping the file's directory). Rename works on any known file, so it is accepted on both this endpoint and [`PATCH /downloads/{hash}`](#patch-apiv0downloadshash).

**Errors:** `400 bad_request` (missing/invalid fields, `comment`/`rating` sent alone, or a `name` that is empty or contains a path separator), `409 not_shared` (comment/rating on a non-shared file), `400 amuled_rejected`, `503 ec_unavailable`.

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
      "description": "Public server",
      "version": "17.15",
      "address": "203.0.113.5:4242",
      "country_code": "de",
      "port": 4242,
      "users": 312000,
      "max_users": 500000,
      "files": 75000000,
      "priority": "normal",
      "ping_ms": 42,
      "failed": 0,
      "static": false
    }
  ]
}
```

`country_code` is the ISO 3166-1 alpha-2 code (lowercase, e.g. `"de"`) of the server host, resolved server-side from the server IP by the daemon's GeoIP database — same semantics and empty-string fallback as the peer `country_code` on `/clients`.

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/servers`

**Auth:** `ADMIN`

Add a server to amuled's known-server list.

**Body:**

```json
{ "address": "203.0.113.5:4242", "name": "eMule Server" }
```

`name` optional; `address` required and must parse as `host:port`.

**Response:** `201 Created` → `{ "ok": true, "address": "..." }`.

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

#### `POST /api/v0/servers/{ecid}/connect` / `POST /api/v0/servers/{ip}:{port}/connect`

**Auth:** `ADMIN`

Tells amuled to disconnect from its current server and dial the specified one. Two route shapes are equivalent — the address form looks up the ECID by exact `(ip, port)` match against the server cache and delegates to the ECID handler. Hostname-form addresses do NOT resolve here — pass the literal IP.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/servers/203.0.113.5:4242/connect"
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

`name` required; others optional. `color` is a 24-bit RGB integer. `priority` is applied to the category's member files as a download priority, so it takes the same restricted set as [`PATCH /downloads`](#patch-apiv0downloads) — `"low"` / `"normal"` / `"high"` / `"auto"`. `very_low` and `release` are rejected (the daemon would clamp them to `normal` on the next restart).

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

Returns every preference category amuled carries over EC. The `general` and `connection` sub-objects are the original common-case set; the remaining categories (issue #437, plus `ip2country` from #440) map 1:1 to the daemon's own settings and mirror the desktop "Preferences" tabs.

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
    "max_upload_cap_kbps":   0,
    "max_download_cap_kbps": 0,
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
  },
  "directories": {
    "incoming": "/home/me/.aMule/Incoming",
    "temp":     "/home/me/.aMule/Temp",
    "shared":   ["/home/me/media"],
    "share_hidden":    false,
    "auto_rescan":     true,
    "follow_symlinks": false,
    "exclude_patterns": "",
    "exclude_regex":    false
  },
  "files": {
    "ich_enabled": true, "aich_trust": false,
    "new_paused": false, "new_auto_dl_prio": false, "new_auto_ul_prio": false,
    "preview_prio": false, "start_next_paused": false, "resume_same_cat": false,
    "save_sources": true, "extract_metadata": false, "alloc_full_size": false,
    "check_free_space": true, "min_free_space_mb": 1, "create_normal": false
  },
  "servers": {
    "remove_dead": true, "dead_server_retries": 3, "auto_update": false,
    "add_from_server": true, "add_from_client": true, "use_score_system": true,
    "smart_id_check": true, "safe_server_connect": false,
    "autoconn_static_only": false, "manual_high_prio": false,
    "update_url": "http://upd.emule-security.org/server.met"
  },
  "security": {
    "can_see_shares": false,
    "ipfilter_clients": true, "ipfilter_servers": true,
    "ipfilter_auto_update": false, "ipfilter_update_url": "",
    "ipfilter_level": 127, "ipfilter_filter_lan": true,
    "use_secident": true,
    "obfuscation_supported": true, "obfuscation_requested": true, "obfuscation_required": false
  },
  "message_filter": {
    "enabled": false, "all": false, "friends": false,
    "secure": false, "by_keyword": false, "keywords": ""
  },
  "remote_controls": {
    "webserver_enabled": false, "webserver_port": 4711, "webserver_use_gzip": true,
    "webserver_refresh": 120, "webserver_template": "",
    "webserver_guest_enabled": false,
    "amuleapi_enabled": true, "amuleapi_port": 4713, "amuleapi_bind": "0.0.0.0"
  },
  "online_signature": { "enabled": false },
  "core_tweaks": {
    "max_conn_per_five": 200, "verbose": false, "filebuffer": 240000,
    "ul_queue": 5000, "srv_keepalive_timeout": 0, "kad_max_searches": 50,
    "kad_reask_ms": 1800000, "source_reask_ms": 900000
  },
  "kademlia": { "update_url": "http://upd.emule-security.org/nodes.dat" },
  "ip2country": {
    "supported": true, "enabled": true, "source": "dbip",
    "custom_url": "", "maxmind_license": "", "auto_update": true,
    "loaded_source": "dbip", "db_path": "/home/me/.aMule/GeoIP/dbip.mmdb",
    "db_loaded": true, "downloading": false, "last_result": "ok"
  }
}
```

Booleans are plain JSON `true`/`false` regardless of how amuled encodes them on the wire. **Passwords are never returned** — the webserver admin/guest and amuleapi passwords are write-only (see PATCH). `general.user_hash` is the node's own identity hash, not a password.

`ip2country` is the GeoIP (IP-to-country) config category (issue #440). `supported` is a capability flag: `false` when the connected daemon is built without GeoIP — the config fields are then present but inert. `source` is one of `"dbip"` / `"maxmind"` / `"custom"` (the next-download database selector). `maxmind_license` is returned plainly (it is a config string the daemon already round-trips, not a masked password). `loaded_source`, `db_path`, `db_loaded`, `downloading`, and `last_result` are **read-only** live status (the currently loaded DB and any in-flight refresh); they are ignored if sent on PATCH.

**Errors:** `503 ec_unavailable`.

#### `PATCH /api/v0/preferences`

**Auth:** `ADMIN`

Body shape mirrors the GET; every sub-object and every field is optional, and fields not present are left unchanged. Subset example:

```json
{ "files": { "new_paused": true }, "servers": { "dead_server_retries": 5 } }
```

**Write-only passwords** (accepted here, never echoed on GET) live under `remote_controls`: `webserver_password`, `webserver_guest_password`, `amuleapi_password`. Send the plaintext — amuled stores the hash. `webserver_guest_password` requires that guest access be enabled (pass `webserver_guest_enabled: true` in the same request, or leave it already enabled).

**`ip2country`** accepts `enabled`, `source` (`"dbip"` / `"maxmind"` / `"custom"` — any other value is a `400`), `custom_url`, `maxmind_license`, and `auto_update`. It also accepts a **write-only** `update_now` boolean that triggers an immediate database download from the (just-applied) source; it is never echoed on GET. `supported` and the read-only status fields (`loaded_source`, `db_path`, `db_loaded`, `downloading`, `last_result`) are ignored if sent.

> **Note:** these are the daemon's live settings — the same ones the desktop GUI edits. Some are self-affecting: changing `remote_controls.amuleapi_port` / `amuleapi_bind`, or `directories.incoming` / `temp`, alters the very daemon you are talking to. A port/bind change only takes effect on the next amuled restart, so it will not drop your current connection mid-request.

**Response:** `200 OK` — full preferences object (post-mutation), so a read-modify-write client can confirm what landed without a follow-up GET.

**Errors:** `400 bad_request` (unknown/mis-typed field, or a body with no recognized fields), `400 amuled_rejected`, `503 ec_unavailable`.

---

### Network control

These endpoints drive amuled's connect/disconnect to the ed2k network, the Kad network, or both.

#### `POST /api/v0/networks/connect`

**Auth:** `ADMIN`

**Body:** `{ "network": "ed2k" | "kad" | "both" }` (optional; defaults to `"both"`). Same shape as `/networks/disconnect` — `"ed2k"` fires `EC_OP_SERVER_CONNECT`, `"kad"` fires `EC_OP_KAD_START`, omitted/`"both"` fires `EC_OP_CONNECT`.

**Response:** `202 Accepted`.

**Errors:** `400 bad_request` (unknown selector), `503 ec_unavailable`.

#### `POST /api/v0/networks/disconnect`

**Auth:** `ADMIN`

**Body:** `{ "network": "ed2k" | "kad" | "both" }` (optional; defaults to `"both"`).

**Response:** `200 OK`.

**Errors:** `400 bad_request`, `503 ec_unavailable`.

> Dedicated `POST /api/v0/kad/connect` and `POST /api/v0/kad/disconnect` shortcuts existed in an earlier draft of v0 but were dropped in favour of the `/networks/{connect,disconnect}` body selector — `{"network":"kad"}` does exactly what they did. The `/kad/bootstrap` endpoint below is genuinely distinct and stays.

#### `POST /api/v0/kad/bootstrap`

**Auth:** `ADMIN`

Manual Kad bootstrap against a single known-good Kad node. Fires `EC_OP_KAD_BOOTSTRAP_FROM_IP` against amuled. This is the only Kad bootstrap surface the EC protocol exposes — `nodes.dat` is read by amuled at startup from its own data directory and is NOT manageable via REST.

**Body:** `{ "ip": "203.0.113.5" | <uint32 host-order>, "port": <uint16> }`. `ip` accepts either the dotted-quad string form or the uint32 host-order integer form (amuled's wire-level shape). `port` is the contact's UDP port.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"ip":"203.0.113.5","port":4672}' \
  "http://$HOST/api/v0/kad/bootstrap"
```

**Response:** `202 Accepted` → `{ "ok": true, "ip": <uint32>, "port": <uint16> }`. The Kad probe itself is fire-and-forget UDP; the `202` confirms amuled accepted the request, not that the contact was reachable.

**Errors:** `400 bad_request` (missing/non-string-or-number `ip`, missing/non-numeric `port`, port outside `[0, 65535]`, malformed dotted-quad), `400 amuled_rejected`, `503 ec_unavailable`.

#### `GET /api/v0/kad`

**Auth:** `GUEST`

Standalone view of the Kad subtree from `/status`, plus the detail fields the status rollup omits (`firewalled_udp`, `in_lan_mode`, your external `ip`, the `indexed` Kad-store counters, and `buddy` contact info for low-ID peers).

```json
{
  "state": "connected",
  "firewalled": false,
  "firewalled_udp": false,
  "in_lan_mode": false,
  "ip": "203.0.113.5",
  "network": { "users": 5400000, "files": 1400000000, "nodes": 2400 },
  "indexed": { "sources": 12000, "keywords": 8500, "notes": 0, "load": 14 },
  "buddy": { "status": "connected", "ip": "203.0.113.9", "port": 4672 }
}
```

---

### Logs

#### `GET /api/v0/logs/amule`

**Auth:** `GUEST`

amuled's general log buffer.

**Query parameters:** `tail=N` — return only the last N lines (default: full buffer).

```json
{
  "lines": ["2026-06-19 11:00:00: line one", "...line two"],
  "total_cached": 1024,
  "returned": 2
}
```

`lines` is the array of log lines; `total_cached` is how many lines are held in the buffer and `returned` how many this response carried (≤ `tail`).

#### `DELETE /api/v0/logs/amule`

**Auth:** `ADMIN`

Clears the buffer.

```json
{ "ok": true }
```

#### `GET /api/v0/logs/serverinfo` / `DELETE /api/v0/logs/serverinfo`

**Auth:** `GUEST` / `ADMIN`

The ed2k server-info log buffer. Unlike `/logs/amule`, amuled ships this one as a single accumulated text blob, so the GET returns a `text` **string** rather than a `lines` array. `?tail=N` still selects by trailing lines (it walks back N newline boundaries), but the byte counts in the response describe the result.

```json
{
  "text": "Connecting to eMule Server (203.0.113.5:4242)\nConnection established\n",
  "total_bytes": 4096,
  "returned_bytes": 68
}
```

`DELETE /api/v0/logs/serverinfo` clears the buffer and returns `{ "ok": true }`.

---

### Statistics

#### `GET /api/v0/stats/tree`

**Auth:** `GUEST`

A tree mirroring amuled's "Statistics" tree (transfers, connections, clients, servers, downloads). Cached with a 1 s TTL.

The envelope is `{ "nodes": [...] }`. Each node is `{ "key": "<id>", "raw": "<value>", "label": "<template>", "values": [...], "children": [...] }`. A leaf is a node whose `children` array is empty. `key` and `raw` are optional (see below).

`label` is the **untranslated English template** (e.g. `"Total uploaded: %s"`), and `values` are the **typed, raw** values that fill its `%s` placeholders in order — the client formats and localizes them. This keeps the response identical regardless of the amuleapi/amuled `--locale` (see [Response model](#response-model)). A container node (one that only groups children) has an empty `values` array.

`key` is a **stable, machine-readable identifier** for the node (e.g. `"upload_data"`, `"ul_dl_ratio"`, `"servers_working"`). Unlike `label`, it does not change when the label is reworded, and it is never translated — use it to locate a specific field instead of matching on the label string. The `key` is optional: it is present only for nodes the daemon assigns one to, and it is omitted entirely when absent (older daemons that predate this field emit no `key` at all). Keys are unique among the fixed skeleton nodes; the dynamic per-client-software rows share a key by kind (see below).

`raw` is the **raw, untranslated machine value** for nodes whose `label` is itself data — the per-client-software breakdown rows, where the label is a version or OS string (`"v0.70b: %s"`, `"Linux: %s"`). It carries just that value (`"v0.70b"`, `"Linux"`) so a client reads it directly instead of parsing it out of the `label`. Present only on those rows; omitted otherwise and on daemons that predate the field. These rows are grouped under the `client_versions` / `client_operating_system` container nodes and each row carries `key: "client_version"` / `key: "client_os"` — so the `key` tells you the kind and `raw` gives the value.

Each value is `{ "type": "<type>", "value": <raw> }`:

| `type` | `value` JSON | meaning |
| --- | --- | --- |
| `integer`, `istring`, `ishort` | number | plain count |
| `bytes` | number | raw bytes |
| `speed` | number | raw bytes/second |
| `time` | number | raw seconds |
| `double` | number | raw double |
| `string` | string | opaque English string (e.g. a ratio, or `"Not available"`) |

A `string` value that is a **well-known sentinel** additionally carries an `enum` field with a stable, locale-independent token — currently `"never"` (a "Never" timestamp, e.g. max-connection-limit-reached) and `"not_available"` (a ratio with no data yet). The English `value` is left in place unchanged, so this is purely additive: prefer `enum` when present and fall back to `value` otherwise. It is absent on non-sentinel values and on daemons that predate the field.

A value may carry a nested `extra` value of the same shape — the parenthetical "(total …)" some nodes append (e.g. session vs. total transfer).

The UL:DL ratio node (`key: "ul_dl_ratio"`) additionally carries a `ratio` object with numeric `session` and `total` fields, so clients don't have to parse its composite string value. Both are **download-per-upload** doubles (received ÷ sent bytes): `session` for the current session, `total` for all time. Each field appears only when the daemon can compute it (both sides greater than zero); the whole `ratio` object is omitted when neither is available and on daemons that predate this field. No other node type carries `ratio`.

```json
{
  "nodes": [
    {
      "key": "transfer",
      "label": "Transfers",
      "values": [],
      "children": [
        {
          "key": "uploads",
          "label": "Uploads",
          "values": [],
          "children": [
            {
              "key": "upload_data",
              "label": "Total uploaded: %s",
              "values": [ { "type": "bytes", "value": 13314398208 } ],
              "children": []
            },
            {
              "key": "ul_dl_ratio",
              "label": "Session UL:DL Ratio (Total): %s",
              "values": [ { "type": "string", "value": "1 : 769.34 (1 : 1125.54)" } ],
              "ratio": { "session": 769.34, "total": 1125.54 },
              "children": []
            }
          ]
        }
      ]
    }
  ]
}
```

A per-client-software version row (note `raw`), and a sentinel value (note the additive `enum`):

```json
{
  "key": "client_version",
  "raw": "v0.70b",
  "label": "v0.70b: %s",
  "values": [ { "type": "istring", "value": 42, "extra": { "type": "double", "value": 12.5 } } ],
  "children": []
}
```

```json
{
  "label": "Max Connection Limit Reached: %s",
  "values": [ { "type": "string", "value": "Never", "enum": "never" } ],
  "children": []
}
```

**Errors:** `503 ec_unavailable`.

#### `GET /api/v0/stats/graphs/{graph}`

**Auth:** `GUEST`

Time-series points behind the desktop Statistics graphs.

`{graph}` is one of `download`, `upload`, `connections`, `kad`.

**Query parameters:** `width=N` — clamp the response to the last `N` samples (default/`0` returns the full ~1800-sample window).

```json
{
  "graph": "download",
  "unit": "bps",
  "interval_seconds": 1,
  "points": [
    { "t": "2026-06-19T11:00:00Z", "t_unix": 1781430000, "value": 4500000 },
    { "t": "2026-06-19T11:00:10Z", "t_unix": 1781430010, "value": 4800000 }
  ],
  "session": { "download_bytes": 12400000000, "upload_bytes": 980000000, "kad_bytes": 5400000 }
}
```

Each point is an object with `t` (ISO-8601 UTC), `t_unix` (unix seconds), and `value`. `unit` is `"bps"` for download/upload and `"count"` for connections/kad. `session` carries this-session byte totals so a client doesn't need a separate roundtrip.

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

Returns the current search-results buffer at the moment of the call PLUS a progress envelope so an empty `results` array isn't ambiguous between "no search running", "search in flight with no hits yet", and "search finished with zero hits".

This endpoint does NOT busy-wait — it returns whatever amuled has in its result buffer right now. A client that wants to wait for completion should poll while `progress.state == "running"`. There is no per-GET TTL cache: `POST /search` marks the search active and the refresher polls amuled (`EC_OP_SEARCH_RESULTS` + `EC_OP_SEARCH_PROGRESS`) every tick while it stays active, so this GET reads straight from that refresher-maintained snapshot — successive polls see the growing result set with no extra EC roundtrip, and `POST /search/stop` simply clears the active flag.

```json
{
  "results": [
    {
      "hash":         "8b54a3c2...",
      "name":         "example-distribution-26.04-amd64.iso",
      "size":         3825205248,
      "sources":      { "total": 217, "complete": 142 },
      "already_have": false,
      "rating":       0,
      "status":       "new",
      "type":         "videos",
      "media":        { "length_s": 5400, "bitrate": 1500, "codec": "h264", "artist": "", "album": "", "title": "" },
      "children": [
        { "ecid": 621, "name": "example-distribution-26.04.iso", "hash": "8b54a3c2...", "sources": { "total": 40, "complete": 22 } },
        { "ecid": 622, "name": "example_distro_2604_amd64.iso",  "hash": "8b54a3c2...", "sources": { "total": 10, "complete":  3 } }
      ]
    }
  ],
  "progress": {
    "state":    "running",
    "kind":     "kad",
    "percent":  67
  }
}
```

Each result carries `sources` as a nested `{total, complete}` object — `total` is the swarm size amuled reports and `complete` is how many of those hold the file complete. `already_have` is `true` when you are currently downloading the file or already have it completed/shared; it is `false` for a fresh result and for one you have canceled/removed (a canceled result is re-downloadable, so it does not read as held). `rating` is amuled's aggregated quality rating (`0` when unrated). `status` is this result's download status on your node — `"new"` / `"downloaded"` / `"queued"` / `"canceled"` / `"queued_canceled"`. `type` is the file-type token derived from the filename extension (same tokens as the shared-detail [`file_type`](#get-apiv0sharedhash), e.g. `"videos"` / `"audio"`; `""` when the name has no extension). `media` is the audio/video [media metadata](#media-metadata) object (same shape as the file-detail endpoints) — **present only** for a hit that is already known/probed locally, and **omitted entirely** for remote hits with no metadata (most global/Kad results), matching the blank Length/Bitrate/Codec columns in the desktop search list.

`children` (issue #431) is the result-grouping tree: amuled collapses hits that are the **same file** (same ed2k hash **and** size) but advertised under **different filenames** into one parent row, and `children[]` holds the alternative names. Each child carries the parent's `hash` (that's why they group), its own `sources`, and a distinct `ecid` — pass that `ecid` to [`POST /search/results/{hash}/download`](#post-apiv0searchresultshashdownload) to download the file **under that chosen filename**. `children` is always present and is an empty array for a hit seen under a single name. The top-level `results[]` contains parents only — a child never appears as its own top-level entry.

The `progress` object carries the same `state` / `kind` / `percent` fields as the [`search_progress`](EVENTS.md#search_progress) SSE event, so REST pollers and stream consumers interpret progress identically. (The event additionally carries a `results` count, since — unlike this response — it has no `results` array beside it.)

- `state` — `"running"` while the search is in flight, `"finished"` once amuled reports completion, `"idle"` when no search has run this session. This single field is canonical and replaces the older `complete` / `active` booleans (derive them as `complete = state == "finished"`, `active = state == "running"`).
- `kind` — the originally-requested search type (`"local"` | `"global"` | `"kad"`).
- `percent` — `[0, 100]`, computed by amuled for every search kind from its `EC_TAG_SEARCH_LIFECYCLE_PERCENT` tag. For **global** it is the real server-queue progress. For **Kad** — which has no measurable mid-flight progress — it is a cosmetic time-ramp off the fixed 45 s keyword-search lifetime, capped at 99 until amuled authoritatively reports completion (`EC_TAG_SEARCH_LIFECYCLE_STATE` = finished), at which point it snaps to 100. Treat the Kad value as a liveliness indicator, not an accurate estimate.

A client that wants to wait for completion polls while `state == "running"`. Because amuled now reports the lifecycle state directly (no sentinel decode), `state == "running"` unambiguously means in-flight even for Kad — there is no longer any "is `percent: 0` a stalled Kad search or no search at all?" ambiguity; check `state` instead. A Kad search that hits its result cap (`SEARCHKEYWORD_TOTAL`, 300) before the 45 s deadline finishes early — `state` flips to `finished` and `percent` jumps to 100 ahead of the ramp.

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

**Body:** `{ "category": 0, "ecid": 621 }` — both optional. `category` is the download category (default `0`). `ecid` (issue #431) selects one grouped **child** by its `results[].children[].ecid`, so the file downloads **under that child's filename**; omit it to download the parent (the aggregated/highest-source name). Since grouped children share the parent's hash, `{hash}` alone can't disambiguate them — `ecid` is how you pick a specific advertised name.

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

`/api/v0/` is frozen against any breaking change once released. Within a version only **additive** changes are made, and a conformant client must tolerate them:

- **New endpoints** may be added at any time.
- **New optional query parameters** may be added to existing endpoints, always with a backward-compatible default — omitting the parameter preserves the prior behaviour (as the list-pagination `limit`/`offset`/`sort`/`order` params did).
- **New fields** may be added to response bodies, and new optional fields to request bodies. Clients **MUST ignore unknown fields**, and must not depend on field order or on a field's absence.

Anything that could break a conformant client — renaming, removing, or retyping a field; changing a field's semantics or an endpoint's default behaviour; making an optional input required; or removing an endpoint — is deferred to the next version (`/api/v1/`) rather than applied in place.

`POST /api/v0/auth/login`'s default body shape (no token unless `?type=bearer`) IS a change from the very first amuleapi cuts; the legacy "token always in body" behaviour is reachable only via the opt-in. This is documented and committed.
