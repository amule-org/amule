# amuleapi v0 — REST reference

This document is the contract for every REST endpoint exposed by the `amuleapi` daemon under the `/api/v0/` prefix. For the SSE stream see [EVENTS.md](EVENTS.md). For first-run setup see [../QUICKSTART-AMULEAPI.md](../QUICKSTART-AMULEAPI.md).

The API is versioned in the path. **`/api/v0/` is the pre-release surface and is not frozen**: it has no consumers outside this repository, so a name or a shape that is wrong gets corrected in place rather than carried forward. Renames land as they are agreed, and this document is the contract as of the current commit. `/api/v1/` is what gets frozen, and it is cut once the surface has settled and been exercised end to end -- not before.

## Index

**Cross-cutting concerns**
- [Base URL and transport](#base-url-and-transport)
- [Authentication](#authentication) — [Login response shape](#login-response-shape), [Role model](#role-model), [Rate limiting](#rate-limiting), [JWT structure](#jwt-structure)
- [Response model](#response-model) — [Success envelope](#success-envelope), [Mutation responses](#mutation-responses), [Idempotency](#idempotency), [IP addresses](#ip-addresses), [List pagination and sorting](#list-pagination-and-sorting), [Bulk mutations and the `results` envelope](#bulk-mutations-and-the-results-envelope), [Error envelope](#error-envelope), [ETag and conditional GET](#etag-and-conditional-get), [CORS](#cors), [Path validation](#path-validation), [Request size limits](#request-size-limits)
- [Naming rules](#naming-rules) — the conventions every path, parameter and key follows, and why rates are spelled out
- [Error code catalog](#error-code-catalog)
- [Backward compatibility](#backward-compatibility)

**System**
- [`GET /api/v0/health`](#get-apiv0health) — liveness probe; readiness flags in the body
- [`GET /api/v0/version`](#get-apiv0version) — version negotiation; identity fields are public, the `update` block needs a token
- [`POST /api/v0/version/check`](#post-apiv0versioncheck) — trigger a daemon-side version check
- [`GET /api/v0/status`](#get-apiv0status) — connection state, network state, headline counters

**Authentication**
- [`POST /api/v0/auth/login`](#post-apiv0authlogin) — mint a JWT, optionally return it in the body
- [`POST /api/v0/auth/logout`](#post-apiv0authlogout) — revoke the bearer's `jti`
- [`GET /api/v0/auth/session`](#get-apiv0authsession) — verified bearer's role and expiry
- [`GET /api/v0/auth/passwords`](#get-apiv0authpasswords) — which roles have a password configured
- [`PATCH /api/v0/auth/passwords`](#patch-apiv0authpasswords) — change the admin password, enable/disable guest

**Downloads**
- [`GET /api/v0/downloads`](#get-apiv0downloads) — list active queue
- [`GET /api/v0/downloads/{hash}`](#get-apiv0downloadshash) — detail view; `{hash}` is the 32-char MD4 hex hash
- [`GET /api/v0/downloads/{hash}/comments`](#get-apiv0downloadshashcomments) — per-source comments/ratings list (incl. retrieved Kad notes)
- [`POST /api/v0/downloads/{hash}/comments`](#post-apiv0downloadshashcomments) — trigger an on-demand Kad notes lookup
- [`GET /api/v0/downloads/{hash}/filenames`](#get-apiv0downloadshashfilenames) — source-reported filenames + counts
- [`GET /api/v0/downloads/{hash}/clients`](#get-apiv0downloadshashclients--get-apiv0sharedhashclients) — sources and A4AF rows of one partfile
- [`POST /api/v0/downloads/{hash}/a4af`](#post-apiv0downloadshasha4af) — force A4AF source-swapping
- [`POST /api/v0/downloads`](#post-apiv0downloads) — add ed2k link(s)
- [`PATCH /api/v0/downloads`](#patch-apiv0downloads) — bulk pause / resume / priority / category
- [`DELETE /api/v0/downloads`](#delete-apiv0downloads) — bulk cancel + remove
- [`PATCH /api/v0/downloads/{hash}`](#patch-apiv0downloadshash) — pause / resume / priority / category
- [`DELETE /api/v0/downloads/{hash}`](#delete-apiv0downloadshash) — cancel + remove
- [`POST /api/v0/downloads_clear_completed`](#post-apiv0downloads_clear_completed) — bulk-clear completed staging buffer

**Clients**
- [`GET /api/v0/clients`](#get-apiv0clients) — list clients, optional filter
- [`GET /api/v0/clients/{ecid}`](#get-apiv0clientsecid) — full detail for one client
- [`POST /api/v0/clients/{ecid}/shared_files`](#post-apiv0clientsecidshared_files) — browse a client's shared files ("View Files"), returns a `search_id`

**Shared files**
- [`GET /api/v0/shared`](#get-apiv0shared) — list shared files
- [`GET /api/v0/shared/{hash}`](#get-apiv0sharedhash) — detail view; every list field plus shared-detail fields
- [`GET /api/v0/shared/{hash}/clients`](#get-apiv0downloadshashclients--get-apiv0sharedhashclients) — clients of one shared file
- [`GET /api/v0/shared/{hash}/content`](#get-apiv0sharedhashcontent) — download the shared file's bytes, with `Range` support
- [`POST /api/v0/shared_reload`](#post-apiv0shared_reload) — re-walk shared directories
- [`POST /api/v0/shared/media/refresh`](#post-apiv0sharedmediarefresh) — re-extract media metadata for the whole share
- [`POST /api/v0/shared/{hash}/media/refresh`](#post-apiv0sharedhashmediarefresh) — re-extract it for one file
- [`GET /api/v0/share_directories`](#get-apiv0share_directories) — the configured share roots
- [`PUT /api/v0/share_directories`](#put-apiv0share_directories) — replace the configured share roots
- [`POST /api/v0/share_directories`](#post-apiv0share_directories) — add one share root
- [`DELETE /api/v0/share_directories`](#delete-apiv0share_directories) — remove one share root
- [`POST /api/v0/shared/{hash}/verify`](#post-apiv0sharedhashverify) — re-hash a shared file against its on-disk data
- [`PATCH /api/v0/shared`](#patch-apiv0shared) — bulk change upload priority
- [`PATCH /api/v0/shared/{hash}`](#patch-apiv0sharedhash) — change upload priority

**Servers**
- [`GET /api/v0/servers`](#get-apiv0servers) — list known ed2k servers
- [`POST /api/v0/servers`](#post-apiv0servers) — add server
- [`POST /api/v0/servers/{ecid}/connect`](#post-apiv0serversecidconnect--post-apiv0serversby-addressaddressconnect) — connect to specific server (by ECID, or by `ip:port` under `by-address`)
- [`DELETE /api/v0/servers/{ecid}`](#delete-apiv0serversecid--delete-apiv0serversby-addressaddress) — remove server (by ECID, or by `ip:port` under `by-address`)
- [`PATCH /api/v0/servers/{ecid}`](#patch-apiv0serversecid--patch-apiv0serversby-addressaddress) — set server priority / static flag (by ECID, or by `ip:port` under `by-address`)
- [`POST /api/v0/servers_update`](#post-apiv0servers_update) — refresh from `server.met` URL
- [`GET /api/v0/friends`](#get-apiv0friends) — list the friends list
- [`POST /api/v0/friends`](#post-apiv0friends) — add a friend, by connected client or by address
- [`DELETE /api/v0/friends/{ecid}`](#delete-apiv0friendsecid) — remove a friend
- [`PATCH /api/v0/friends/{ecid}`](#patch-apiv0friendsecid) — grant or clear the friend slot
- [`POST /api/v0/friends/{ecid}/shared_files`](#post-apiv0friendsecidshared_files) — browse a friend's shared files
- [`POST /api/v0/friends/{ecid}/messages`](#post-apiv0friendsecidmessages) — message a friend, online or offline
- [`GET /api/v0/chats`](#get-apiv0chats) — list chat conversations
- [`GET /api/v0/chats/{address}/messages`](#get-apiv0chatsaddressmessages) — read a conversation's history
- [`POST /api/v0/chats/{address}/messages`](#post-apiv0chatsaddressmessages) — send a message to a client address
- [`DELETE /api/v0/chats/{address}`](#delete-apiv0chatsaddress) — close a conversation
- [`POST /api/v0/clients/{ecid}/messages`](#post-apiv0clientsecidmessages) — message a connected client

**Categories**
- [`GET /api/v0/categories`](#get-apiv0categories) — list categories
- [`POST /api/v0/categories`](#post-apiv0categories) — create
- [`GET /api/v0/categories/{index}`](#get-apiv0categoriesindex) - read one category
- [`PATCH /api/v0/categories/{index}`](#patch-apiv0categoriesindex) — modify
- [`DELETE /api/v0/categories/{index}`](#delete-apiv0categoriesindex) — remove

**Preferences**
- [`GET /api/v0/preferences`](#get-apiv0preferences) — read all EC-carried preference categories
- [`PATCH /api/v0/preferences`](#patch-apiv0preferences) — update any subset of prefs

**Network control**
- [`POST /api/v0/networks/connect`](#post-apiv0networksconnect) — connect ed2k / kad / both
- [`POST /api/v0/networks/disconnect`](#post-apiv0networksdisconnect) — disconnect ed2k / kad / both
- [`POST /api/v0/kad/bootstrap`](#post-apiv0kadbootstrap) — single-contact Kad bootstrap
- [`POST /api/v0/kad/update`](#post-apiv0kadupdate) — refresh the Kad node list from a `nodes.dat` URL
- [`GET /api/v0/kad`](#get-apiv0kad) — Kad-only status subtree

**IP filter**
- [`POST /api/v0/ipfilter/reload`](#post-apiv0ipfilterreload) — re-read the on-disk IP filter files
- [`POST /api/v0/ipfilter/update`](#post-apiv0ipfilterupdate) — download a fresh `ipfilter.dat` from a URL
- [`POST /api/v0/geoip/update`](#post-apiv0geoipupdate) — download a fresh GeoIP database

**Logs**
- [`GET /api/v0/logs/amule`](#get-apiv0logsamule) — amule log buffer
- [`DELETE /api/v0/logs/amule`](#delete-apiv0logsamule) — clear amule buffer
- [`GET /api/v0/logs/server_info`](#get-apiv0logsserver_info--delete-apiv0logsserver_info) — server-info log buffer
- [`DELETE /api/v0/logs/server_info`](#get-apiv0logsserver_info--delete-apiv0logsserver_info) — clear server-info buffer

**Statistics**
- [`GET /api/v0/stats/tree`](#get-apiv0statstree) — full statistics tree
- [`GET /api/v0/stats/graphs/{graph}`](#get-apiv0statsgraphsgraph) — time-series points (`download_speed`, `upload_speed`, `connections`, `kad_nodes`)

**Search**
- [`GET /api/v0/search`](#get-apiv0search) — enumerate every search amuled currently holds, including ones this session never started
- [`POST /api/v0/search`](#post-apiv0search) — start a search (global / local / kad), returns its `search_id`
- [`GET /api/v0/search/{id}/results`](#get-apiv0searchidresults) — one search's results + progress envelope
- [`POST /api/v0/search/{id}/stop`](#post-apiv0searchidstop) — stop a search, keeping its results
- [`POST /api/v0/search/{id}/more`](#post-apiv0searchidmore) — widen a running Kad search
- [`DELETE /api/v0/search/{id}`](#delete-apiv0searchid) — stop a search and free it
- [`POST /api/v0/search/results/{hash}/download`](#post-apiv0searchresultshashdownload) — promote a result into the download queue
- [`GET /api/v0/search/results/{hash}/comments`](#get-apiv0searchresultshashcomments) — Kad ratings/comments for a result
- [`POST /api/v0/search/results/{hash}/comments`](#post-apiv0searchresultshashcomments) — trigger a Kad notes lookup for a result

**Assets**
- [`GET /flags/{code}.png`](#get-flagscodepng) — country-flag artwork for a `country_code`

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

- `?include_token=true` query string, or
- `Accept: application/jwt` request header.

The two are equivalent.

| Mode | Body keys | Set-Cookie |
|------|-----------|------------|
| Default (cookie) | `role`, `expires_at`, `session_id` | yes |
| Token opt-in | `token`, `role`, `expires_at`, `session_id` | yes (cookie also goes out so a hybrid client can use either) |

`include_token` adds exactly the one key it names: `session_id` and `expires_at` are unconditional, so a cookie client can identify and expire its own session without asking for a token it will not use.

### Role model

Two roles, each gated by its own password:

- `admin` — full surface, including every mutation (`POST`, `PATCH`, `DELETE`).
- `guest` — read-only surface. Any `admin`-only endpoint returns `403 forbidden`.

A role is implicitly assigned at login based on which password matched; the verified role is encoded in the JWT and surfaced on `/auth/session`.

Guest access is on exactly when a guest password is configured — there is no separate switch. Clearing the guest password is how guest access is turned off.

**What `guest` is, and is not.** It is a read-only role for someone you already trust on the network, not a sandbox for a hostile party. A guest reads the whole read surface, and that surface includes things an operator may not think of as public: the daemon's incoming, temp and shared **filesystem paths**; the node's `user_hash`; the configured `proxy_user`; and the raw daemon **log**, which carries peer addresses and file names as they are logged. None of that is a defect - a read-only API that hid the paths it is reading from would be hard to use - but it means guest credentials should be handed out on the same basis as read access to the machine, and a guest password is not a way to expose amuleapi to the open internet safely. If a caller should not see those, do not give them a credential at all.

### Where the passwords live

Both are stored in `${config_dir}/amuleapi-passwords` (mode 0600), each as a salted PBKDF2-HMAC-SHA256 record rather than a reversible digest. That file is the only store; nothing is kept in `amule.conf`.

Four things write it, and all of them mean the same thing by it:

| Entry point | Used by |
|-------------|---------|
| `amuleapi --set-admin-pass=` / `--set-guest-pass=` | standalone operator |
| `PATCH /auth/passwords` | REST clients and the web frontend |
| aMule → *Preferences → Remote Controls* | monolithic aMule |
| the same panel in amulegui, pushed to amuled over EC | remote GUI |

A change made through any of them takes effect on the next login without restarting amuleapi.

Because the stored form is not reversible, a configured password can never be read back — only replaced. Every interface therefore treats its password field as write-only: leaving it empty means "keep the current password", and `GET /auth/passwords` reports only *whether* each role is configured.

### Changing a password ends other sessions

Any credential change invalidates every token issued before it, whichever entry point made the change. A token whose `iat` predates the credential file's modification time is rejected with `401 unauthorized` (`credentials changed; please sign in again`).

This is what makes rotating a leaked password effective: without it, whoever held the old password would stay signed in for up to the full token lifetime. `PATCH /auth/passwords` issues the caller a replacement token in the same response, so the operator making the change is not signed out by their own request.

### Rate limiting

Two per-IP failure counters, both with sliding-window semantics:

- **Login limiter** — drives `/auth/login`. Defaults are `[Auth]/LoginFailureWindowSeconds=60`, `LoginFailureThreshold=5`, `LoginLockoutSeconds=300`. Configurable per-deployment.
- **Generic 401 limiter** counts every rejected token (bad, missing, expired or revoked) on any other auth-protected endpoint. [`GET /api/v0/version`](#get-apiv0version) is the one exception: authentication there is optional, so a request that presents no credential at all is not counted — otherwise an anonymous poller of a public endpoint could lock real sessions out. A credential that *is* presented and rejected still counts. Defaults are `[Auth]/TokenFailureWindowSeconds=60`, `TokenFailureThreshold=30`, `TokenLockoutSeconds=300`. Configurable per-deployment, like the login limiter: this is the one a browser tab left open overnight actually trips, so an operator serving long-lived clients may want it looser.

When the bucket fills, the next request from that IP returns `429 rate_limited` with a `Retry-After: <seconds>` header. The bucket clears when the lockout expires, and individual failures age out of the window on their own.

A successful login does **not** clear the login limiter's bucket. One bucket, keyed by IP, guards both passwords, so clearing it on any success would let a holder of the guest password erase the admin-password failure streak at will and brute-force it without ever arming the lockout. The practical effect is that failed attempts from an IP keep counting for `LoginFailureWindowSeconds` even after a correct login. The generic 401 limiter does clear on success: a valid token is not a guess at another credential.

A polling client must treat its first `401` as terminal for that session and stop sending until it has re-authenticated. Restarting amuleapi invalidates every issued token, so a client that keeps a poll loop and an SSE reconnect running against the old cookie spends the generic limiter's whole budget within seconds and locks its own IP out for five minutes. Retrying a `401` never succeeds — only `POST /auth/login` clears it.

### JWT structure

Header: `{"alg":"HS256","typ":"JWT"}`. Payload: `{"role":"admin"|"guest","iat":<unix>,"exp":<unix>,"jti":"<base64url>"}`. The signing secret is auto-generated as 32 random bytes into `${config_dir}/amuleapi-jwt-secret` on first launch (mode 0600). Delete that file and restart to invalidate every issued token. The `jti` claim drives the server-side revocation list (`/auth/logout`), and the `iat` claim drives the credential-change cutoff described above.

## Response model

### Success envelope

Each endpoint documents its own response shape under the endpoint section. List endpoints wrap their array under the resource plural name (`{"downloads": [...]}`, `{"shared": [...]}`) so clients can extend the envelope with sibling metadata without breaking JSON-parser pipelines.

### Mutation responses

One rule for what a mutation answers with: **a mutation carries a body only where the body says something the caller could not otherwise know.** A field echoed back out of the request URL or body is not that, and neither is a constant `ok: true` restating the status code.

That resolves to four shapes:

- **`204 No Content`** - a completed action with nothing to report. Deletes, `POST /auth/logout`, `POST /search/{id}/stop`.
- **`202 Accepted`, no body** - the daemon took the request and the outcome arrives elsewhere: on a later read, on the log channel, or on the SSE stream. Connects, URL fetches, re-hash and reload requests, and the creations whose EC op answers success or failure and nothing more (`POST /servers`, `POST /categories`, `POST /friends`) - a body there could only be reconstructed by scanning the snapshot after an inline refresh and hoping the new record had already landed.
- **`202 Accepted` with the created resource, plus a `Location` header** - the creations where the daemon really does hand one back: [`POST /search`](#post-apiv0search) and the two browse routes, which get an `EC_TAG_SEARCH_ID`. The body is the same row [`GET /search`](#get-apiv0search) lists.
- **`200 OK` with the resource** - a `PATCH`, which answers with the state the caller just produced so no re-read is needed to see it.

Three bodies are deliberate exceptions, because each reports something no later read recovers:

- the per-item [`results` envelope](#bulk-mutations-and-the-results-envelope), which carries a real outcome per input item;
- `message` on the connection-control routes, which is the daemon's own explanation of what it did with the request - and only when the daemon actually said something: with nothing to report those routes answer `202` with no body, like the URL fetches beside them, rather than an empty object;
- `ip` / `port` on [`POST /kad/bootstrap`](#post-apiv0kadbootstrap), which reports **which** address the daemon parsed.

### Idempotency

**Every endpoint is idempotent: sending the same request twice leaves the daemon in the state one request would have left it in.** A client library, a proxy or a browser can repeat a request without the caller knowing, so a surface that toggles rather than sets turns an invisible retry into a silent, wrong state change.

In practice that means a mutation names the value it wants rather than the change it wants. `PATCH /downloads/{hash}` with `{"a4af_auto": true}` reaches `true` from either starting point and stays there however many times it arrives; there is deliberately no "flip it" spelling. Where the daemon's own operation was a flip, amuleapi does not paper over it by reading the current value and deciding -- the value goes down to the core, which stores it.

Two things are idempotent without looking it: a `DELETE` of something already gone is a `404` rather than a second delete, and `POST /downloads` with a link already queued is refused per-item in the [`results` envelope](#bulk-mutations-and-the-results-envelope). Both report that the request had no effect, which is the point; neither does the work twice.

The exceptions are the endpoints whose whole purpose is to do something again: `POST /shared_reload`, `POST /shared/{hash}/verify`, `POST /shared/media/refresh`, `POST /shared/{hash}/media/refresh`, the `*/update` fetches, and `POST /search/{id}/more`. Asking twice asks for the work twice, which is what the caller meant.

### IP addresses

**Every IP address on this surface, in either direction, is a dotted-quad string.** `"203.0.113.5"`, never `3405803781`, and never a byte-order the caller has to know about. That covers request bodies, query parameters and response fields alike.

The conversion to and from the host-order integers EC carries happens inside amuleapi. `POST /kad/bootstrap` used to accept a `uint32` alongside the quad, and the two disagreed: the quad parser packed `a.b.c.d` least-significant byte first while the integer was taken verbatim, so `2130706433` (`0x7F000001`, which is what a client computing an IPv4 integer the conventional way writes for `127.0.0.1`) reached the daemon as `1.0.0.127`. Only the quad is accepted now, and the question does not arise.

### Query parameter validation

One rule, everywhere: **a query parameter the server does not understand is a `400 bad_request`, never a silent default.** That covers both halves of "does not understand": a value that will not parse, and a value outside the parameter's documented range.

- Booleans (`include_parts`) accept `1`/`0`, `true`/`false` and `yes`/`no`. Anything else is a `400`.
- Counts (`limit`, `offset`, `tail`, `width`, `max_client_versions`, `since_message_id`) and the one duration (`interval_seconds`) accept decimal digits within the range documented for that parameter. A non-numeric value, a negative one, or one outside the range is a `400` naming the bound.
- An omitted parameter takes the documented default. Only omission does that; an empty value (`?limit=`) is a `400`, not an omission.

Nothing clamps. A count above its cap used to be quietly reduced on some endpoints, so a count over the cap returned a full page with nothing in the response saying the request had been altered; it is now a rejection, which is the same answer the other endpoints already gave.

### List pagination and sorting

The list endpoints (`GET /downloads`, `/clients`, `/known_clients`, `/shared`, `/servers`, `/friends`, `/chats`, `/categories`, `/search`, the two per-file client routes, and `/search/{id}/results` — the same set as the sortable-fields table below) accept optional query parameters for server-side windowing and ordering, and always return pagination metadata beside the array:

| Param    | Default          | Notes |
|----------|------------------|-------|
| `limit`  | `100`            | Maximum items to return, `0`–`1000000000`. **Omitting it selects the first 100 items, not the whole collection.** Ask for the whole collection explicitly (`limit=1000000000`). Non-integer, negative or above the ceiling → `400 bad_request`. |
| `offset` | `0`              | Items to skip before the window, counted from the `after` anchor when one is given. Non-integer, negative or above `1000000000` → `400 bad_request`. |
| `sort`   | *(native order)* | Field to sort by; endpoint-specific (table below). Unknown field → `400 bad_request`. |
| `order`  | `asc`            | `asc` or `desc`; anything else → `400 bad_request`. |
| `after`  | *(none)*         | Keyset anchor: return the items ordered **after** this value. Requires a `sort` on that endpoint's identity column (marked in the table below) and `order=asc`; anything else → `400 bad_request`. |

**Why `limit` defaults rather than meaning "everything".** The old rule was not monotonic — 500 rows, an error at 501, and the whole collection at zero — so a response size could not be predicted from the parameter, and the cap bounded the explicit caller while leaving the naive one unbounded. A caller that wants everything now says so, which is also a thing an operator can see in an access log.

**Why the ceiling is `1000000000` and not the largest integer available.** `offset + limit` has to stay inside a 32-bit `size_t` for the 32-bit builds, and `1e9 + 1e9` is the largest round pair that does. It is also inside JavaScript's exact-integer range (`2^53 - 1`), so a browser client gets back the number it sent.

Sorting is applied to the full filtered set **before** slicing, so pagination is stable across requests (a stable sort — equal keys keep native order). The response adds three sibling keys to the array:

```json
{ "shared": [ ... ], "total": 8431, "offset": 100, "limit": 50 }
```

- `total` — item count after any endpoint-specific filter (e.g. `/clients?activity=`), before slicing.
- `offset` — the offset applied.
- `limit` — the page size applied. Never `null`: every response has a real page size, so `{total, offset, limit}` round-trips straight back into the next request. It is not the row count — that is `total`.

### Paging a whole collection safely

`offset` is a **position**, and a position moves when the collection changes size below it. Delete one row from a page already fetched and every later index slides down by one, so the next request starts a row late and that row is never returned — by anything. It was not added, updated or removed, so no [SSE event](EVENTS.md) mentions it either, which is what makes the loss last for the session rather than being repaired a tick later.

`after` removes the arithmetic that fails. It anchors on a **value** instead of a count, so nothing that happens to the rows before it can move the window:

```
GET /api/v0/shared?sort=hash&limit=500
GET /api/v0/shared?sort=hash&limit=500&after=<hash of the last row returned>
...repeat until a page comes back shorter than `limit`
```

Two rules make this correct:

- **Sort on the endpoint's identity column** — the ones marked **id** below. Anchoring on a mutable column is meaningless because the anchor's own value moves between two requests, so the API rejects it rather than silently skipping rows.
- **Ascending only.** `order=desc` with `after` is a `400`.

Rows added or removed *during* a sweep are not missed: both emit an SSE event, so a client that opened the stream before starting the sweep (which is the [documented bootstrap order](EVENTS.md#bootstrap-snapshot--stream)) learns about them from the stream even when the sweep itself does not return them.

**Sortable fields per endpoint:**

| Endpoint              | `sort` values |
|-----------------------|---------------|
| `GET /downloads`      | **`hash`** (id), `name`, `size_bytes`, `progress.percent`, `speed_bytes_per_second`, `status` |
| `GET /clients`        | **`ecid`** (id), `name`, `software`, `uploaded_bytes_total`, `downloaded_bytes_total`, `upload_speed_bytes_per_second`, `download_speed_bytes_per_second` |
| `GET /downloads/{hash}/clients`<br>`GET /shared/{hash}/clients` | same keys as `/clients`, `after` included |
| `GET /known_clients`  | **`user_hash`** (id), `name`, `software`, `first_seen_at`, `last_seen_at`, `session_count`, `uploaded_bytes_total`, `downloaded_bytes_total` |
| `GET /shared`         | **`hash`** (id), `name`, `size_bytes`, `uploaded_bytes_total`, `upload_speed_bytes_per_second` |
| `GET /servers`        | **`ecid`** (id), `name`, `user_count`, `ping_ms`, `file_count` |
| `GET /friends`        | **`ecid`** (id), `name`, `online` |
| `GET /chats`          | **`client_ecid`** (id), `last_message_at`, `name` |
| `GET /search/{id}/results` | **`hash`** (id), `name`, `size_bytes`, `sources.total`, `rating`, `directory` |
| `GET /search`         | **`search_id`** (id), `query`, `started_at`, `result_count` |
| `GET /categories`     | **`index`** (id), `name` |

The column marked **id** is the endpoint's identity: immutable for the life of the row, and the only one `after` accepts. Note that `ecid` is stable **within a daemon session** only — `CECID` hands ids out from a counter that restarts with the process — which is enough for a bootstrap sweep, since a reconnect invalidates the sweep anyway and triggers a `resync`.

### Bulk mutations and the `results` envelope

Every mutation that operates on more than one item (`POST /downloads`, `PATCH /downloads`, `DELETE /downloads`, `PATCH /shared`, `POST /downloads_clear_completed`, `PUT /share_directories`) reports one entry per input item under a unified `results` array, so a client submitting N items learns the fate of each rather than an aggregate counter or a first-error-only summary:

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

### Unknown values

A field whose value is not known is `null`, not a sentinel. `remaining_seconds` is `null` rather than `-1` when there is no ETA to compute; `last_upload_at`, `shared_since_at` and `last_seen_complete_at` are `null` rather than `0` when a file has never uploaded, has never been seen complete, or its `known.met` entry predates the field. On a client row, `parts_offered_count` is `null` when that client has not reported its part map -- distinct from `0`, which is a real answer and what a fresh source looks like -- and `remote_queue_position` is `null` when the client's queue is full, which the daemon signals with a `65535` sentinel rather than a position.

A key is **omitted** only where absence itself is the meaning: something the daemon never reported, rather than something known to be absent. `started_at` on [`GET /search`](#get-apiv0search) is the example, missing for a search this process did not start, and `result_count` is missing when the daemon is too old to send it, which has to stay distinguishable from a search that found nothing.

So: `null` means "no value", an absent key means "not reported", and neither is ever spelled `0` or `-1`.

The rule now reaches the whole surface rather than just the download and shared objects. Keys that used to disappear and are `null` instead: `name`, `ip`, `port`, `kad_port`, `country_code`, `software`, `software_version`, `source_origin`, `obfuscation_state`, `first_seen_at` and `session_count` on [`GET /known_clients`](#get-apiv0known_clients); `part_progress_percent` and `parts_offered_count` on the client rows and the `client_*` events; `client_ecid` on [`GET /search`](#get-apiv0search), [`GET /friends`](#get-apiv0friends) and the `friend_*` events; `last_message` on [`GET /chats`](#get-apiv0chats); `token`, `label_value` and `extra` on the statistics tree; and `media` everywhere it appears. The same pass reached the remaining address fields: `port` on [`GET /friends`](#get-apiv0friends) and the `friend_*` events, `ed2k.public_ip` and `ed2k.server_ip` on [`GET /status`](#get-apiv0status), and `public_ip` on [`GET /kad`](#get-apiv0kad); and `server_ip` / `server_port` on [`GET /clients/{ecid}`](#get-apiv0clientsecid), which used `""` for the same "unknown" its own snapshot field documents. Completing that sweep: `ip` and `port` on [`GET /clients`](#get-apiv0clients) and the client detail row, which the `client_*` events already nulled, and `ed2k.server_port` on [`GET /status`](#get-apiv0status), which stayed a bare `0` beside its own nulled `server_ip`. The `status_changed` event nulls `ed2k.public_ip`, `ed2k.server_ip` and `ed2k.server_port` to match the REST row. Continuing it: `kad_port` on [`GET /api/v0/clients/{ecid}`](#get-apiv0clientsecid), which stayed a raw `0` beside the `ip`/`port` it is nulled with everywhere else; `last_received_at` on [`GET /api/v0/downloads/{hash}`](#get-apiv0downloadshash), which read as 1970 for a partfile that had received nothing; and `node_id` on [`GET /api/v0/kad`](#get-apiv0kad), the last `""` sentinel in an object whose every other field already answered `null`. And closing the connected-server triple: `server_name` on [`GET /status`](#get-apiv0status) and [`GET /clients/{ecid}`](#get-apiv0clientsecid), which stayed a raw `""` beside the `server_ip` and `server_port` it is nulled with, so one object spelled "not on a server" two ways. `status_changed` nulls it too. Finishing the client objects themselves: `name`, `software`, `software_version`, `reported_os`, `download_file_name`, `upload_file_name`, `upload_file_hash`, `download_file_hash`, `obfuscation_state`, `source_origin` and `client_mod_name` on [`GET /clients`](#get-apiv0clients), the client detail row and the `client_*` events, which spelled "unknown" as a raw `""` while [`GET /known_clients`](#get-apiv0known_clients) already nulled the same keys, so one peer described by both objects disagreed with itself. And the reachability fields: `online` on [`GET /friends`](#get-apiv0friends), [`GET /chats`](#get-apiv0chats) and [`GET /known_clients`](#get-apiv0known_clients), plus the new `connected` on the client objects, are `null` on a daemon that does not report peer connectivity - unknown, rather than a guessed "offline".

`media` is the one place this reaches an **object** rather than a scalar, so a client tests `media === null` before reaching into it -- which it had to do regardless, since the object's own fields can be absent.

Five keys stay omitted, because for them absence really is the meaning: `started_at` and `result_count` on [`GET /search`](#get-apiv0search) as described above, `key` on a statistics node (absent from a daemon too old to send it), `ratio_session` / `ratio_total` on the statistics ratio node (each absent when the daemon could not compute it), and `parts` under [`?include_parts=true`](#get-apiv0downloadshashclients--get-apiv0sharedhashclients) -- there the caller opted in, so the key's absence answers a question they did not ask.

Opting out is not one of them. `parts`, `next_requested_part_index` and `downloading_part_index` are all absent from a client row unless `?include_parts=true` was asked for, but that absence is a property of the request, not a fact about the client, so it is not what the null-versus-absent rule above is about. Under the flag the two index keys are *always* present — `null`, never absent, wherever the index does not apply — which is why only `parts` is in the list above.

### Priority levels

`priority` appears on four resources and accepts three different sets. The differences are deliberate rather than drift, and they reflect what the daemon can actually store:

| Resource | Accepted |
|---|---|
| Downloads (`PATCH /downloads`, `PATCH /downloads/{hash}`) | `low`, `normal`, `high`, `auto` |
| Shared files (`PATCH /shared`, `PATCH /shared/{hash}`) | `very_low`, `low`, `normal`, `high`, `release`, `auto` |
| Categories (`POST /categories`, `PATCH /categories/{index}`) | `very_low`, `low`, `normal`, `high`, `release`, `auto` |
| Servers (`PATCH /servers/{ecid}`) | `low`, `normal`, `high` |

`very_low` and `release` are upload-side levels only: the `.part.met` loader clamps anything outside low/normal/high back to normal on restart, so a download pinned to one of them would silently lose it. Categories take the full six-level set on both read and write (see below), even though they apply their priority to member files as a *download* priority. Servers have three levels and no `auto`.

A rejection names the set that endpoint accepts, so sending a wrong value tells you the right ones. Note that a file which is both downloading and shared carries two independent priorities from the two sets, and changing one does not affect the other.

**Categories read and write the same six levels.** A category's `priority` is formatted on read from the same file-priority set as downloads and shared files, so `GET /api/v0/categories` can report `very_low` or `release` -- and `POST` / `PATCH` accept them, so a read-modify-write round-trip cannot fail on a field the client never touched (R9). The narrower four-value set still applies to downloads, whose read side cannot produce the other two.

Reaching it takes a category whose stored priority was not set through this API or the desktop: the desktop's category priority control offers only Low / Normal / High / Auto, and `CDownloadQueue::SetCatPrio` applies whatever it is given as a *download* priority, which is the same four. A hand-edited `amule.conf`, or another client, is what it would take.

The asymmetry is left in place deliberately. Clamping on read would have the API report `high` for a category the daemon holds as `release`, and the client's write-back would then persist that lie; widening the write domain would let the value survive one session and be clamped to normal by the partfile loader on the next restart, which is the silent rewrite the download set refuses those two levels to avoid. Between two ways of quietly changing a user's setting and one documented sentence, the sentence wins.

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

Every `GET` or `HEAD` that returns `200` carries an `ETag` header. Clients that re-fetch should send `If-None-Match: "<etag>"` and accept `304 Not Modified` (no content, ETag preserved). `If-None-Match` accepts `*`, a comma-separated list, and weak `W/"..."` validators.

The validator is memoized for two collections only, `/downloads` and `/shared`, keyed on the target plus a revision that every writer of those bodies advances. Repeated GETs between changes skip the body hash; everything else on the surface is hashed per request. Eligibility is opt-in rather than exclusion-based, because a resource qualifies only if its body moves solely when the state moves AND is identical for every caller. Anything with its own cache, an append-only mirror, a refresh-on-read, a live daemon roundtrip per request, or a per-caller body is simply not in the set. `/auth/session` is per-caller and is additionally marked `Cache-Control: private, no-store` (see below).

`HEAD` returns the same headers as the equivalent `GET`, including `ETag` and a `Content-Length` describing the body the `GET` would return, and no content -- on every status, not only `200`. The one exception is [`GET /api/v0/events`](EVENTS.md): its body is an unbounded chunked stream, so a `HEAD` there reports the stream's headers and no length. Note that this is why `curl -X HEAD` appears to hang or fail: `-X` only changes the method string, leaving curl waiting for a body that a HEAD response correctly never sends. Use `curl --head` (or `-I`).

A validator names one representation, not one URL. When a response is compressed the `ETag` carries a `-gzip` suffix, so the gzipped and identity forms of the same resource never share a validator, and an `If-None-Match` is only a hit against the form the current request would actually receive. A client that stored `"abc123-gzip"` and then re-fetches with `Accept-Encoding: identity` gets a `200` with the identity body, which is the correct answer -- the stored entry does not describe it. Store the validator alongside the encoding you received, which is what an HTTP cache does anyway.

**Caching policy.** Any request that presented credentials -- a bearer token or the session cookie -- is answered `Cache-Control: private` with `Cookie` added to `Vary`. `private` keeps the response out of shared caches, while still letting the client's own cache hold it and revalidate with `If-None-Match`; `no-store` would forbid that too and there would be no stored entry left for the conditional GET above to match. [`GET /api/v0/auth/session`](#get-apiv0authsession) is the deliberate exception and is `private, no-store`: it carries the credential itself, which should not be written down anywhere. Requests without credentials are left cacheable. Static assets are the other case. The WebUI shell and its bundles are byte-identical for every caller, so an authenticated request for one is answered exactly as an anonymous request is, and they carry `public, no-cache`. Both tokens do work. `no-cache` means revalidate every time -- not "do not store": the copy stays cached and an unchanged bundle costs one conditional request answered `304`. `public` is what lets a shared cache in front of the daemon keep one copy rather than one per caller: RFC 9111 §3.5 bars a shared cache from reusing a response to a request that carried an `Authorization` header unless the response is marked `public`, `must-revalidate` or `s-maxage`, and `no-cache` is not on that list. The browser WebUI is not the case that engages this -- it authenticates by cookie, which is why the authenticated stamp above adds `Cookie` to `Vary` rather than relying on §3.5 at all. A bearer-token client fetching the same shell is the case that engages it.

There is no freshness lifetime on them because these filenames carry no content hash: `index.html`, `app.js` and `app.css` keep their names across a rebuild, so a `max-age` would let an upgraded daemon keep serving the old shell until the entry expired, and -- since each asset expires on its own clock -- pair a new shell with an old bundle. `must-revalidate` would not prevent that: it governs what a cache may do once an entry is *already* stale (RFC 9111 §5.2.2.2), not whether it may be used while fresh.

The [country flags](#get-flagscodepng) take the opposite trade and keep `public, max-age=86400`. That artwork is compiled into the daemon, so it changes only with a new build, and a client list is a page full of `<img>` tags: a day of freshness turns those into cache hits instead of one conditional request per distinct country per reload. The `max-age` is what bounds how long an upgraded daemon can keep serving the old art -- a day, not forever.

Mutations (`POST`/`PATCH`/`DELETE`) and error responses are never ETag-stamped; the body always ships.

### CORS

If `amuleapi.conf[Server]/AllowCORS=1`:

- Every response carries `Vary: Origin`.
- The origin is echoed in `Access-Control-Allow-Origin` if either the allowlist is empty (any-origin echo) or the request's `Origin` header matches a configured entry.
- **`Access-Control-Allow-Credentials: true` is sent only for an origin the allowlist names.** With an empty allowlist the echo is `*`-equivalent, and granting credentials there would let any site the user happens to visit call this API with their session cookie and read the replies, which is what the same-origin policy exists to prevent. Anonymous cross-origin reads still work with an empty allowlist; a **credentialed** cross-origin client needs its origin listed in `amuleapi.conf[Server]/CorsOriginAllowlist`: a comma-separated list, entries trimmed, matched by exact string equality against the request's `Origin` header. An origin is a scheme, host and port (`https://app.example.com`, or `http://localhost:5173` in development) with no path and no wildcards, so `https://example.com` does not cover `https://www.example.com` and the two ports of the same host are two entries.
- Allowed responses carry `Access-Control-Expose-Headers: ETag, Allow, Retry-After` so clients can read the validator, the supported-method list from a `405`, and the back-off hint from a `429` or `503`, from JS.
- Preflight (`OPTIONS` with `Access-Control-Request-Method`) returns `204` with `Access-Control-Allow-Methods: GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS`, `Access-Control-Allow-Headers: Authorization, Content-Type, If-None-Match, Last-Event-ID`, and `Access-Control-Max-Age: 86400`.

### Path validation

The dispatcher rejects paths containing NUL, encoded NUL (`%00`), encoded `..` (any case of `%2e%2e`), or a literal `..` segment with `400 bad_request` before routing. Defence-in-depth against a future endpoint that admits path captures.

**Trailing slash.** Under `/api/`, one trailing `/` is stripped before routing, so `/api/v0/status/` and `/api/v0/status` are the same request. Exactly one is stripped: `//` is a malformed path rather than a synonym, so `/api/v0/downloads//` does not reach `/api/v0/downloads`. The rule stops at the API prefix: a static asset path is a filesystem path, where a trailing slash means a directory.

**Empty path captures.** A segment standing in for a `{capture}` cannot be empty. Every capture names a resource (a hash, an ECID, an index, an address), so a path that binds one to the empty string matches no route and is `404 not_found`, rather than reaching a handler and being rejected there with whatever status that endpoint happens to use.

### Request size limits

- HTTP header section: hard cap 16 KiB. Exceeding it is `431 headers_too_large`.
- Request body: hard cap 1 MiB. Exceeding it is `413 payload_too_large`.
- A request that is not fully sent within 10 s is `408 request_timeout`.
- JSON nesting: `>32` opening `{` or `[` tokens → `400 bad_request`. Applies to every body parser and to the JWT header/payload sections of bearer tokens.

The first three are answered by the transport before any handler runs, and each closes the connection after answering, with `Connection: close` on the response.

## Naming rules

Every path segment, query parameter and JSON key on this surface follows the rules below. They exist so a client can predict a name instead of looking it up, and so a reviewer has something to point at other than taste. Where the surface does not yet follow them, that is a defect being worked through resource by resource, not an exception.

| # | Rule |
|---|---|
| **R1** | `snake_case` for every path segment, query parameter and JSON key. |
| **R2** | **Units live in the name, spelled out.** `_bytes`, `_bytes_per_second`, `_seconds`, `_minutes`, `_ms`, `_percent` (always 0-100). A bare number is only acceptable for a dimensionless count. See the note below on why rates are spelled out rather than abbreviated. |
| **R3** | **Timestamps are integer unix seconds and end in `_at`.** One representation, not two: no parallel ISO-8601 twin. Formatting is a client concern. |
| **R4** | **Booleans carry no `is_`/`has_`/`can_` prefix.** A boolean already reads as a predicate: an adjective or past participle (`online`, `incomplete`, `recursive`), or an `_enabled`/`_supported` suffix on a stateful noun. A boolean is never a field holding a count, a bare command-verb that reads as an imperative, or a reserved word. |
| **R5** | **Counts end in `_count`** — except inside an object that already names the thing being counted, where the parent carries it (`sources.total`, `sources.transferring`). The pagination envelope keeps `total` for "matching items before the window". |
| **R6** | **One concept, one key, everywhere.** The same quantity does not get one name on one resource and another on the next. |
| **R7** | **A `sort` value *is* the response key it orders by**, spelled identically, dotted for nested keys (`sort=sources.total`). No stripped suffixes and no per-endpoint mapping table. |
| **R8** | **No implementation vocabulary in public names.** EC tag names, C++ member names, aMule config-file section names and on-disk format details stay internal. Protocol terms aMule's own UI shows the user — `ed2k`, `kad`, `a4af`, `ich`, `aich`, `ecid` — stay, and are expanded once here. |
| **R9** | **A writable field accepts the values the same field returns.** Where a write is really a command it belongs in a differently-named key (`action`), not in the read field's name. |
| **R10** | **Always emit the key.** An unknown value is `null`, never an omitted key and never a `0`/`-1` sentinel. The few deliberate exceptions are enumerated under [Response model](#response-model). |
| **R11** | **Group by quantity, not by scope.** A sub-object earns its place when it groups *different* quantities (`sources`, `progress`, `media`). One quantity split by time window does not — the window belongs in the key, not in a wrapper. |
| **R12** | **One word for the remote party: `client`.** `address` is not used in a key, a path or an enum value. `source` is not a synonym: a source is a client *in a role with respect to one file*, so it survives only where that relation is what is being described (`sources.total`, `source_origin`). |

### Why rates are spelled out

A byte rate is `_bytes_per_second`, not `_bps`. The abbreviation was considered and rejected: in networking `bps` means **bits** per second, and every rate on this surface is bytes. The neighbouring fields make that trap concrete rather than theoretical — [`GET /downloads/{hash}`](#get-apiv0downloadshash) returns `size_bytes` and `completed_bytes` beside `speed_bytes_per_second`, and `(size_bytes - completed_bytes) / speed` is the most natural thing to compute from the object. Two adjacent, identical-looking fields differing by 8x with nothing on screen to say so is a worse failure than a long name, because a name is visible and a factor of 8 is not.

The same reasoning gives the media bitrate its full spelling. `media.bitrate_kilobits_per_second` really is kilo**bits** — `ParseBitrateKbps` divides ffprobe's bits/second by 1000 — so it is the one quantity on the surface that is not bytes, and the one place `kilo` means 1000 rather than 1024. Abbreviating it would put the surface's only bit-valued rate one character away from its byte-valued ones.

The three bandwidth caps under `connection` are the one place a rate is not per-byte: `max_download_kibibytes_per_second`, `max_upload_kibibytes_per_second` and `upload_slot_min_kibibytes_per_second` carry the KiB/s figure the user types, unconverted, because the core stores and enforces it in those units (`GetMaxUpload() * 1024`). `kibibytes` is spelled out for the same reason every other unit is, and it says 1024 without the reader having to guess which `k` this is.

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

#### `GET /api/v0/health`

**Auth:** `NONE`. A probe has to work before anyone holds a token.

```json
{ "status": "ok", "ec_connected": true, "snapshot": true }
```

**Liveness, not readiness.** The status is `200` whenever the HTTP server is answering, so a container, systemd or load-balancer probe never restarts a healthy process just because `amuled` went away. Readiness is in the body instead: `ec_connected` is the live EC link and `snapshot` is whether the first refresher tick has landed. A caller that wants readiness keys on those two fields; a caller that wants liveness keys on the status code.

The handler touches no EC. amuleapi serialises EC through one worker, so a probe that waited on the daemon could block behind an unrelated slow mutation and time out, reporting the service as down when it is merely busy.

`HEAD` is supported. Any other method is `405` with `Allow: GET, HEAD`.

**Conditional requests.** The body is small and changes only when those two flags change, so the usual `ETag` applies and a probe that sends `If-None-Match` will get `304 Not Modified`. That is a healthy answer, not an outage. A checker that treats anything other than `200` as failure should either not send the header or accept `304`.

#### `GET /api/v0/version`

**Auth:** `NONE` for the identity fields, so an unauthenticated caller can negotiate versions. Use [`GET /api/v0/health`](#get-apiv0health) for liveness probing rather than this endpoint.

The `update` object is **omitted unless the request is authenticated**: it reports whether this daemon is running an outdated build, which is not something an unauthenticated caller on a deliberately reachable interface should learn. A client showing an "update available" banner is already authenticated when it does. Sending no credential is not an error here — the response is `200` with the identity fields, and it does not count against the [generic 401 limiter](#rate-limiting). Sending a *bad* one is also `200` without `update`, but does count.

```sh
curl -s http://$HOST/api/v0/version
```

**Response:** `200 OK`

```json
{
  "service": "amuleapi",
  "api_version": "v0",
  "amuleapi_version": "GIT rev. 3.0.1-773-g500293ba3",
  "daemon_version": "GIT rev. 3.0.1-773-g500293ba3",
  "update": {
    "check_enabled": true,
    "checked": true,
    "latest_version": "3.0.1",
    "available": false,
    "last_checked_at": 1783675590
  }
}
```

| Field | Meaning |
| --- | --- |
| `service` | Always `"amuleapi"`. |
| `api_version` | REST contract version served on this path (`"v0"`). |
| `amuleapi_version` | amuleapi's **own** build version. On a tagged release this is the release number (`"3.0.1"`); on a development build it is `"GIT"` followed by the revision (`"GIT rev. 3.0.1-773-g500293ba3"`), because `"GIT"` alone is the same string for every snapshot and identifies nothing. |
| `daemon_version` | Version of the **connected amuled**, from the EC handshake, in the same spelling as `amuleapi_version` above. Empty string when EC is not (yet) connected, or when the daemon is old enough not to advertise it. Normally equal to `amuleapi_version` (both are built from the same source tree), but they can differ if a mismatched amuleapi is pointed at a different amuled. On development builds the revision makes that comparison meaningful, which bare `"GIT"` could not. |
| `update` | Update-availability, **relayed from the connected daemon** — amuleapi never contacts GitHub itself. See the sub-table. |

The `update` object:

| Field | Meaning |
| --- | --- |
| `check_enabled` | `true` only when the daemon can check **and** is configured to: built with `ENABLE_VERSION_CHECK` **and** its `NewVersionCheck` preference on. `false` for OS-package builds, the preference off, or a pre-3.1 daemon. When `false`, a client should show nothing. |
| `checked` | `true` once the daemon has completed at least one check this session (so `latest_version` is known). The daemon checks at startup; use `POST /api/v0/version/check` to trigger a fresh one. |
| `latest_version` | Latest release string (e.g. `"3.0.1"`); `null` when not yet checked or unavailable. |
| `available` | `true` when a newer release exists, `false` when up to date, `null` when unknown (not yet checked or disabled). |
| `last_checked_at` | Unix time (seconds) the last check completed; `null` when never checked. Useful because checks are startup-only unless re-triggered. |

#### `POST /api/v0/version/check`

**Auth:** `ADMIN`

Triggers an on-demand version check **on the daemon** (amuleapi does not fetch GitHub itself). Fire-and-forget: the request returns as soon as the daemon accepts it, and the result appears on a subsequent `GET /api/v0/version` once the async check completes. Throttled by the daemon to respect GitHub's rate limit.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" http://$HOST/api/v0/version/check
```

**Response:** `202 Accepted`, with no body. The check runs asynchronously; the outcome arrives on the `logs` channel and in [`GET /api/v0/version`](#get-apiv0version).

**Errors:**

| Status | `error.code` | When |
| --- | --- | --- |
| `409` | `version_check_unavailable` | The daemon can't check (`update.check_enabled` is `false`). |
| `429` | `version_check_throttled` | A check ran too recently; retry shortly. Distinct from the auth limiter's `rate_limited`, which also answers `429` but ends the session. |
| `503` | `ec_unavailable` | The EC round-trip to amuled failed, or the first snapshot has not landed yet. |

The snapshot gate matters at startup: `version_check_available` defaults to false, so before the first EC tick this route used to answer `409 version_check_unavailable`, blaming the daemon's configuration for amuleapi not having read it yet. It answers `503` there now, which is the condition a client can retry.

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
    "high_id": true,
    "user_id": 1234567890,
    "public_ip": "210.2.150.73",
    "connected_since_at": 1751000000,
    "server_name": "eMule Server",
    "server_ip": "203.0.113.5",
    "server_port": 4242,
    "network": { "user_count": 312000, "file_count": 75000000 }
  },
  "kad": {
    "state": "connected",
    "firewalled_tcp": false,
    "connected_since_at": 1751000000,
    "network": { "user_count": 5400000, "file_count": 1400000000, "node_count": 2400 }
  },
  "speeds": {
    "download_speed_bytes_per_second": 4500000,
    "upload_speed_bytes_per_second": 50000,
    "download_overhead_bytes_per_second": 8700,
    "upload_overhead_bytes_per_second": 1100
  },
  "disk": { "temp_free_bytes": 48318382080, "incoming_free_bytes": 48318382080 },
  "queue": { "waiting_upload_client_count": 12, "download_source_count": 1843 }
}
```

`ec_connected` is `false` while amuleapi can't reach the underlying amuled. Most other endpoints return `503 ec_unavailable` in that state.

`ed2k.connected_since_at` / `kad.connected_since_at` are unix timestamps of the most recent connect, `0` while not connected — gate on `ed2k.state` / `kad.state` rather than trust a `0` timestamp alone.

**Network figures are `null` while their network is down.** `ed2k.network.user_count` / `.file_count`, `kad.network.user_count` / `.file_count` / `.node_count`, and `kad.firewalled_tcp` are all `null` unless the corresponding `state` is `connected`. They are measurements of a network, and there is no measurement when not attached to one -- see [Unknown values](#unknown-values). Before this they reported the last figures they had: a disconnected daemon repeated its connected eD2k counts verbatim and indefinitely, and `kad.network.node_count` (this node's own routing-table size, not a network estimate) stayed non-zero even after a full stop. Sample values above are the connected case.

**`kad.firewalled_tcp` is named for its transport.** It is the TCP half of a pair; [`GET /api/v0/kad`](#get-apiv0kad) reports `firewalled_udp` alongside it. The two are independent measurements taken by different mechanisms, not a verdict and a refinement of it. See the `/kad` field table for what each one measures and how their defaults differ.

**Our eD2k identity.** `ed2k.user_id` is the id the connected server assigned us, and `ed2k.high_id` is `true` when it is a HighID — an id `>= 16777216`, the same threshold the client-side `high_id` on [`GET /clients/{ecid}`](#get-apiv0clientsecid) uses. A HighID **is** our public IPv4 packed into that integer, which is where `ed2k.public_ip` comes from; a LowID is a small number the server picked for a firewalled client and carries no address, so `public_ip` is `null` there.

While disconnected `user_id` is `0`, `public_ip` is `null` and `high_id` is `false` — so read `high_id` **together with `state`**: `false` means "LowID" only once `state` is `"connected"`, and means "no id yet" otherwise. The transient `0xffffffff` the daemon sends mid-connect is normalized to `0` and never appears.

**`ed2k.user_id` is not the same encoding as a client's `ed2k_user_id`.** [`GET /api/v0/clients/{ecid}`](#get-apiv0clientsecid) reports `ed2k_user_id` for a remote client, and the similar name invites the assumption that the two are interchangeable. They are not. Ours is stored exactly as the server sent it and is read least-significant-byte-first to produce `public_ip`; a client's HighID is **byte-swapped** on the way in. A consumer that compares the two values, or feeds one through the other's IP decoder, gets a reversed address. The `>= 16777216` HighID threshold *is* common to both; the byte order is not.

**Overhead is additive.** `speeds.download_overhead_bytes_per_second` / `upload_overhead_bytes_per_second` are protocol and control traffic, counted **separately** from `download_speed_bytes_per_second` / `upload_speed_bytes_per_second` rather than being part of them — the desktop shows them as a second figure in parentheses. Both are `0` when the daemon reports nothing.

**Disk figures may be `null`.** `disk.temp_free_bytes` is free space on the filesystem holding the part files, `disk.incoming_free_bytes` where finished downloads land. Either is `null` when the daemon has no figure — the first seconds after startup, or a directory it cannot stat, such as an unreachable network mount. `null` rather than `0`, because `0` would read as a full disk.

The two are **equal whenever Temp and Incoming share a filesystem**, which is the default layout — that is correct, not a bug. `incoming_free_bytes` describes the **default category's** incoming directory; a category pointed at another filesystem is not covered, because the daemon publishes no per-category figure.

To reproduce the desktop's low-space warning, compare `temp_free_bytes` against the bytes still to write across the queue (`size - completed_bytes` summed over [`GET /downloads`](#get-apiv0downloads)), or against the `files.min_free_space_mebibytes` preference when `files.stop_on_low_disk_space` is on. Note that preference is in **MiB** while these fields are bytes.

**Errors:** `503 ec_unavailable` if amuleapi hasn't received its first EC snapshot yet.

---

### Authentication

#### `POST /api/v0/auth/login`

**Auth:** `NONE`

Mints a JWT for the role that matched the supplied password.

**Query parameters:** `?include_token=true` (optional) — also return the token in the body. Equivalent to sending `Accept: application/jwt`.

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

{"role":"admin","expires_at":1781434800,"session_id":"b3iY9oA1tUW2pK..."}
```

**Token opt-in request:**

```sh
curl -s -X POST "http://$HOST/api/v0/auth/login?include_token=true" \
  -H 'Content-Type: application/json' \
  -d '{"password":"adminpass"}'
```

```json
{
  "token": "eyJhbGciOi...",
  "role": "admin",
  "expires_at": 1781434800,
  "session_id": "b3iY9oA1tUW2pK..."
}
```

`expires_at` is unix seconds, like every other `_at` key on this surface.

**Errors:**

- `400 bad_request` — body missing/non-object/missing `password`/non-string `password`.
- `401 invalid_credentials` — password didn't match any configured role.
- `429 rate_limited` — login limiter armed; `Retry-After` set.
- `503 login_disabled` — no admin and no guest password configured.

#### `POST /api/v0/auth/logout`

**Auth:** `GUEST`

Adds the bearer's `jti` to the server-side revocation list (TTL = JWT's `exp`) and emits a clear-cookie. Idempotent: a token that is already revoked still gets `204` so a double-tap on a logout button doesn't surface a confusing "session expired" toast.

`204 No Content`, no body: the clear-cookie header is the whole result, and the `{"ok": true}` this used to send only restated the status code.

```sh
curl -i -X POST -H "Authorization: Bearer $TOKEN" http://$HOST/api/v0/auth/logout
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
  "session_id": "b3iY9oA1tUW2pK...",
  "expires_at": 1781434800
}
```

---

#### `GET /api/v0/auth/passwords`

**Auth:** `ADMIN`

Reports which roles have a password configured. The passwords themselves are stored irreversibly and are never returned.

```sh
curl -s -H "Authorization: Bearer $TOKEN" http://$HOST/api/v0/auth/passwords
```

```json
{
  "admin_password_set": true,
  "guest_access_enabled": false
}
```

`guest_access_enabled` is simply whether a guest password exists.

---

#### `PATCH /api/v0/auth/passwords`

**Auth:** `ADMIN`

Changes the admin password, and turns guest access on or off or changes its password.

| Field | Type | Meaning |
|-------|------|---------|
| `current_password` | string | **Required.** The admin password as it is now. |
| `admin_password` | string | New admin password. Omit to leave it unchanged. |
| `guest_password` | string | New guest password. Implies `guest_access_enabled: true` unless that field says otherwise. Omit to leave it unchanged. |
| `guest_access_enabled` | bool | `false` clears the guest password, turning guest access off. Omit to leave the current state. |

Omitting a field means "leave it alone" — the same rule every other interface follows, and a necessary one, because a client cannot read a stored password back in order to resend it.

**Errors:** `400 bad_request` (body is not an object, a password field is not a string, `current_password` missing or wrong, or nothing to change), `429 rate_limited` (too many failed attempts on this IP), `500 internal_error` (the new credentials could not be written to disk).

`current_password` is required even though the caller already holds an admin token: a stolen token alone should not be enough to lock the real operator out. It is checked against the same per-IP limiter as `/auth/login`, so this is not a softer place to guess passwords.

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' \
    -d '{"current_password":"old-secret","admin_password":"new-secret"}' \
    "http://$HOST/api/v0/auth/passwords?include_token=true"
```

```json
{
  "admin_password_set": true,
  "guest_access_enabled": false,
  "token": "eyJhbGciOiJIUzI1NiIs...",
  "role": "admin",
  "expires_at": 1781521200,
  "session_id": "9pQ2xR7mLk4vTn..."
}
```

The response re-issues the caller's session — same shape and same `?include_token=true` / `Accept: application/jwt` opt-in as `/auth/login`, cookie included. Every *other* session is now invalid — unconditionally, which is why the body carries no key for it. Clients that ignore the new token will get `401 unauthorized` on their next request.

**Errors**

| Status | Code | Cause |
|--------|------|-------|
| `400` | `bad_request` | no changeable field given; a password field is not a string; `admin_password` empty (the admin role cannot be removed); `guest_password` sent together with `guest_access_enabled: false` |
| `403` | `invalid_credentials` | `current_password` is not the admin password |
| `403` | `forbidden` | the token is a guest token |
| `429` | `rate_limited` | too many failed `current_password` attempts from this IP |
| `500` | `internal_error` | the credential file could not be written (permissions, full disk); the passwords are unchanged |

---

### Downloads

#### `GET /api/v0/downloads`

**Auth:** `GUEST`

Lists the current transfer queue. Completed entries (status `completed`) are excluded by default — they live in amuled's separate "awaiting clear" list and surfacing them inline confuses queue dashboards.

**Query parameters:**

- `status=active|all|completed` -- which part of the queue to list. Defaults to `active`, which is what is currently transferring; `completed` selects only finished downloads awaiting a clear, and `all` is both. Anything else is a `400`. This replaced `include_completed`, which could not express completed-only; sending it now is a `400` naming the replacement.

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
      "size_bytes":    3825205248,
      "completed_bytes":     1142000000,
      "transferred_bytes":     1102450000,
      "speed_bytes_per_second":     4500000,
      "status":        "downloading",
      "priority":      "normal",
      "priority_auto": true,
      "category_index": 0,
      "sources":  { "total": 217, "unavailable": 23, "transferring": 8, "a4af": 4 },
      "progress": { "percent": 29.85 },
      "kad_comment_lookup_running": false,
      "hashed_part_count": 0,
      "total_part_count": 394,
      "source_ecids": [ 1234, 5678 ]
    }
  ]
}
```

`progress.percent` is a float in the range **0-100** (not 0-1), per [R2](#naming-rules).

`status` is one of `"downloading"`, `"waiting"`, `"hashing"`, `"allocating"`, `"paused"`, `"stopped"`, `"completing"`, `"completed"`, `"erroneous"`, `"insufficient_disk"` or `"unknown"`. The last three are terminal-ish conditions a client must handle rather than fall through: `"erroneous"` is a failed partfile, `"insufficient_disk"` is one that ran the volume out of space, and `"unknown"` is a status code this version does not recognise. `"stopped"` is a paused file that has also dropped all its sources and reset its Kad source search (set via `PATCH` `action:"stop"`); it is distinct from `"paused"`, which retains its sources.

`priority` is the download priority — one of `"low"`, `"normal"` or `"high"` — and `priority_auto` is `true` when amuled is deriving that level automatically. Downloads never report `very_low` or `release`; those are shared/upload-side levels only. A file that is simultaneously downloading and shared carries two independent priorities: this download priority, and the upload priority reported by [`GET /api/v0/shared`](#get-apiv0shared). Changing one does not affect the other.

The list shape omits `progress.parts` to keep large libraries compact. Use the detail endpoint for per-part state.

`kad_comment_lookup_running` is `true` while an on-demand Kad notes lookup is in flight for the file (started by [`POST /downloads/{hash}/comments`](#post-apiv0downloadshashcomments)); it flips back to `false` when the lookup finishes. Because it lives on the download object, a client can watch the `download_updated` SSE event for the start → finish transition instead of polling.

`hashed_part_count` is the number of parts hashed so far by a pass running over the file — a `hashing` status, an [`AICH`](#post-apiv0sharedhashverify) hashset rebuild — and `0` when nothing is hashing. It is a count of completed parts, not the index of the part in flight, so it runs `0` → `total_part_count`; divide by `total_part_count`, which is on this row, for a percentage. No detail roundtrip is needed for it.

`source_ecids` are the ECIDs of the clients holding this file as an A4AF source — the same array, under the same name, that [`POST /downloads/{hash}/a4af`](#post-apiv0downloadshasha4af) returns, and `[]` when there are none. It is the one thing a per-file client list needs that [`GET /api/v0/clients`](#get-apiv0clients) and the `clients` SSE channel cannot say: A4AF is a relation between a client and a *file*, so it does not live on the client object. With it on the download event, a Clients panel driven by the `downloads` and `clients` channels can shade its A4AF rows from the stream instead of polling [`GET /downloads/{hash}/clients`](#get-apiv0downloadshashclients--get-apiv0sharedhashclients). Note the name is scoped to A4AF, not to sources at large — the count of *all* sources is `sources.total`.

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
| `progress.parts` | array | One entry per ~9.28 MiB chunk: `{ "state": string, "sources": int }` — `state` is `complete`/`pending`/`unavailable` -- `complete` when the chunk is done, `pending` when a gap remains and at least one client offers it, `unavailable` when a gap remains and none does -- and `sources` counts clients offering that chunk. |
| `last_seen_complete_at` | int \| null | Unix ts a complete copy was last seen across sources; `null` when no complete copy has ever been seen, or the daemon does not report it. |
| `last_received_at` | int \| null | Unix ts the partfile last changed on disk (the daemon's own last-received stamp), `null` when the daemon reports none — it is not a "no byte has arrived yet" flag, since a partfile that has transferred nothing still carries a stamp (see [Unknown values](#unknown-values)). |
| `active_seconds` | int | Seconds spent actively downloading. |
| `available_part_count` | int | Number of parts available across the current sources. |
| `remaining_seconds` | int \| null | ETA in seconds, or `null` when stalled or paused (speed ≈ 0) and there is nothing to compute from. |
| `lost_to_corruption_bytes` | int | Bytes discarded to corruption. |
| `gained_by_compression_bytes` | int | Bytes saved by on-the-wire compression. |
| `ich_recovered_packet_count` | int | Packets recovered by **I.C.H.** (Intelligent Corruption Handling), the recovery pass aMule's own UI names. Packets, not bytes -- unlike its two byte-valued neighbours above. |
| `aich_hash` | string\|null | AICH master hash (hex), or `null` until the hashset exists. |
| `part_file_name` | string | The partfile's `.part` control-file basename (e.g. `001.part`). `""` once the download has completed (status `completed`, before `clear_completed`). |
| `directory` | string | Directory the file lives in on disk — the Temp directory while downloading, the destination directory once completed. |
| `upload_queue_count` | int | Clients waiting on this file's upload queue. |
| `my_comment` | string | The user's own comment on this file (`""` if none). Named apart from `comments[].comment`, which are *other clients'*. |
| `my_rating` | int | The user's own rating, `0`–`5` (`0` = unrated). See the [rating scale](#get-apiv0downloadshashcomments). |
| `a4af_auto` | bool | Whether automatic A4AF source-swapping is on for this file. See [A4AF](#post-apiv0downloadshasha4af). |
| `media` | object | Audio/video metadata — see [Media metadata](#media-metadata). **`null`** when the file has no probed metadata; the key is always present. |

**Errors:** `400 bad_request` (`{hash}` is not 32 hex characters), `404 not_found` (no partfile with that hash), `503 ec_unavailable`.

##### Media metadata

The `media` object (on both `GET /downloads/{hash}` and `GET /shared/{hash}`) carries the audio/video metadata amuled probed for the file. It is **`null`** when the file has not been probed (a non-media file, or one probing hasn't reached yet) -- the key is always present, so test `media === null` rather than for the key.

```json
"media": {
  "duration_seconds": 5400,
  "bitrate_kilobits_per_second": 1500,
  "codec": "h264",
  "artist": "…",
  "album": "…",
  "title": "…"
}
```

| Field | Type | Meaning |
|---|---|---|
| `duration_seconds` | int | Duration in seconds. |
| `bitrate_kilobits_per_second` | int | Bitrate (kbps). |
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
  "total": 2,
  "kad_comment_lookup_running": false,
  "comments": [
    { "username": "alice",    "filename": "Some.Movie.mkv", "rating": 5, "comment": "great quality" },
    { "username": "Kad user", "filename": "some_movie.avi", "rating": 4, "comment": "" }
  ]
}
```

`kad_comment_lookup_running` is `true` while an on-demand Kad notes lookup (triggered by the `POST` below) is in flight; poll until it returns to `false` to know the lookup finished. Kad notes appear as ordinary entries whose `username` is the responding node's IP (or `Kad user` when the note carries no IP).

A per-source `rating` of `-1` means the source left a comment but no rating. Rating scale (from the desktop `GetRateString()`):

| value | meaning |
|---|---|
| 0 | Not rated |
| 1 | Invalid / Corrupt / Fake |
| 2 | Poor |
| 3 | Fair |
| 4 | Good |
| 5 | Excellent |

**Errors:** `400 bad_request` (`{hash}` is not 32 hex characters), `404 not_found` (no download with that hash), `503 ec_unavailable`.

#### `POST /api/v0/downloads/{hash}/comments`

**Auth:** `ADMIN`

Trigger an on-demand **Kad notes** lookup for this download (the desktop "Get from Kad" button). aMule asks the Kad network for community ratings/comments keyed on the file hash. The lookup is **asynchronous** on the daemon (it can take up to ~45 s); this call returns immediately with `202 Accepted`, and the retrieved notes then show up in the `GET` list above. Poll the `GET` endpoint to observe them arrive.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2…/comments"
```

**Response:** `202 Accepted`, with no body. The lookup is asynchronous; results appear on the download's `comments` and via `kad_comment_lookup_running`.

**Errors:** `403 forbidden` (guest token — the lookup makes the daemon do network work, so it is `ADMIN`-only), `404 not_found` (no download with that hash), `503 ec_unavailable`, `400 amuled_rejected` (daemon refused, e.g. Kad not connected).

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
    { "filename": "Some.Movie.2024.mkv", "source_count": 7 },
    { "filename": "some_movie.mkv", "source_count": 2 }
  ]
}
```

**Errors:** `404 not_found` (no download with that hash), `503 ec_unavailable`.

#### `GET /api/v0/downloads/{hash}/clients` / `GET /api/v0/shared/{hash}/clients`

**Auth:** `GUEST`

The clients of one file: sources serving it to us, clients pulling it from us, and — on the downloads side — A4AF sources parked on another file. Replaces the client-side join of the global `/clients` list against `download_file_hash` / `upload_file_hash`, which could never produce the A4AF rows.

Each entry is the [`/clients`](#get-apiv0clients) list object plus five keys:

| Key | Meaning |
|---|---|
| `role` | which direction this row moves the file: `"downloading_from"` — we pull the file from it (including queued); `"uploading_to"` — it pulls the file from us; `"both"`; `"none"` |
| `a4af` | `true` for a source parked on another file — the desktop's A4AF row |
| `parts` | the client's per-part bitmap, **only** when `?include_parts=true` |
| `next_requested_part_index` | index of the chunk queued next from this client, or `null`; **only** when `?include_parts=true` |
| `downloading_part_index` | index of the chunk currently in flight from this client, or `null`; **only** when `?include_parts=true` |

`role` and `a4af` are orthogonal: a pure A4AF row is `role: "none"`, `a4af: true`, but a client can be parked on another file *and* be pulling this one from us (`role: "uploading_to"`, `a4af: true`).

`parts` is opt-in because it is one boolean per chunk per client — a multi-TiB file is 100k+ entries each. It is exactly `total_part_count` entries, and it describes the file this row is about: the download bitmap for a `"downloading_from"` row, the upload bitmap for an `"uploading_to"` one. A client the core reports as holding every part comes back all-`true`. A pure A4AF row has no bitmap for this file and omits the key. **`parts` never appears in SSE payloads.**

`next_requested_part_index` and `downloading_part_index` are the two extra states the desktop paints on top of that bitmap — the chunk in flight in amber, the one queued behind it in pale yellow — turning a three-state bar into the desktop's five-state one. Both are `0`-based indices into `parts`, and both ride `include_parts` for the same reason: an index is meaningless without the bitmap it indexes, and a caller that did not ask for `parts` does not know the file's `total_part_count`. Under the flag both keys are always present, `null` rather than omitted whenever the index does not apply, so one query returns one row shape. `null` covers every such case: the client never reported the value, it reported the `0xffff` "nothing pending" sentinel, the index does not address a chunk of this file, the row is not a source for this file at all (`role: "uploading_to"` or a pure A4AF row, whose indices belong to whatever else that client is downloading), or the row carries no `parts` bitmap for the index to point into. `downloading_part_index` carries one further rule: it is `null` unless the client's `download_state` is `"downloading"`, because the daemon reports a stale `0` for a source that is merely connected or queued — treat a number here as "this chunk is arriving right now", which is what makes it safe to paint, and which is why it is named for the present tense rather than for a previous part. Note that `0` is a real chunk index, never a stand-in for unknown — see [`Unknown values`](#unknown-values). Neither key ever appears in SSE payloads.

The file's own three-state part view (`complete` / `pending` / `unavailable`) is on [`GET /downloads/{hash}`](#get-apiv0downloadshash); combine it with this bitmap and the two indices above to render the desktop's five-state per-source bar.

Both routes accept `limit` / `offset` / `sort` / `order` exactly as `/clients` does. A partfile with at least one completed chunk is both a download and a shared file, and then **both routes return the same body** — they differ only in which collection the hash must belong to, which is what the `404` checks.

```sh
curl -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2…/clients?include_parts=true"
```

**Errors:** `404 not_found` (no download / no shared file with that hash), `400 bad_request` (bad list params, or an `include_parts` that is not a boolean), `503 ec_unavailable`.

#### `POST /api/v0/downloads/{hash}/a4af`

**Auth:** `ADMIN`

> `POST` only; a `GET` here answers `405`. A4AF sources are rows of [`GET /downloads/{hash}/clients`](#get-apiv0downloadshashclients--get-apiv0sharedhashclients), carrying the whole client object rather than a bare ECID, and `a4af_auto` is on the download detail object.

Force A4AF source-swapping for this download. Downloads-only.

**Body:** `{ "action": "<action>", "client_ecid": 1234 }`

| action | Effect |
|---|---|
| `swap_this` | Make other files' A4AF sources take over **this** file. |
| `swap_others` | Release this file's sources to the other files that want them. |

Both move sources one way. A third action, `swap_this_auto`, flipped the `a4af_auto` flag and is gone: a flip cannot be retried safely, and it set a field the download object already reports. Set it with [`PATCH /downloads/{hash}`](#patch-apiv0downloadshash) and `{"a4af_auto": true|false}` instead. Sending `swap_this_auto` here is a `400` naming the replacement.

`client_ecid` is optional and valid **only with `swap_this`**, where it narrows the action from every A4AF source of this file to the single named one — the per-client "Swap to this file" of the desktop client. It must name a client in the current snapshot that is an A4AF source of *this* download; pairing it with `swap_others` is a `400`, because the core has no per-source form of it.

The swap moves the client between two files' source lists, so an SSE subscriber sees `download_updated` for **both** the client's former download and this one, plus `client_updated` for the client itself.

**Response:** `200 OK` — the post-action A4AF view of this download:

```json
{ "a4af_auto": false, "source_ecids": [ 1234, 5678 ] }
```

`source_ecids` are the ECIDs of the clients holding this file as an A4AF source, joinable against [`GET /api/v0/clients`](#get-apiv0clients). The same array, under the same name, rides the download object and its `download_updated` SSE event, so a subscriber does not have to POST here to keep it current. The array is the post-action state, so a `swap_this` naming a single client shows up as that ECID having left it. The same clients appear as rows with `"a4af": true` on [`GET /api/v0/downloads/{hash}/clients`](#get-apiv0downloadshashclients--get-apiv0sharedhashclients), which carries the whole client object rather than a bare ECID.

**Errors:** `400 bad_request` (missing or unknown `action`; `swap_this_auto`, which moved to `PATCH`; a non-integer `client_ecid`; `client_ecid` with the wrong action), `400 amuled_rejected` (the daemon refused the swap — most commonly because the client is actively sending data, which it will not be swapped away from), `404 not_found` (no download with that hash, or no client with that ECID), `409 not_a4af_source` (that client is not an A4AF source of this download), `503 ec_unavailable`.

#### `POST /api/v0/downloads`

**Auth:** `ADMIN`

Adds one or more ed2k links to the transfer queue.

**Body:**

```json
{ "links": ["ed2k://|file|a|...|/", "ed2k://|file|b|...|/"], "category_index": 0 }
```

`links` is an array even for one link -- `{"links": ["ed2k://|file|a|...|/"]}` -- and the response is the per-item `results` envelope either way. `category_index` is optional (defaults to 0).

A singular `ed2k_link` was accepted here previously and is now refused with a `400` naming the replacement.

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

**Body:** `{ "hashes": ["<md4>", …], … }` — a non-empty `hashes` array (max 500) plus at least one of the single-item PATCH fields: `action` (`"pause"` | `"resume"` | `"stop"`), `priority` (`low` | `normal` | `high` | `auto`), `category_index` (integer 0–255).

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"hashes":["8b54a3c2…","0a1b2c3d…"],"priority":"high"}' "http://$HOST/api/v0/downloads"
```

**Response:** the [bulk `results` envelope](#bulk-mutations-and-the-results-envelope) (`200` all ok / `207` partial / `503`), keyed by hash. Per-item `error.code` is `not_found`, `amuled_rejected`, or `ec_unavailable`.

**Errors:** `400 bad_request` (missing/empty `hashes`, no patch field present, invalid field value), `503 ec_unavailable`.

#### `DELETE /api/v0/downloads`

**Auth:** `ADMIN`

Bulk cancel + remove of active downloads (deletes each `.part`/`.met` from disk). A completed entry is rejected per-item with `download_completed` — clear those via `POST /downloads_clear_completed`.

**Body:** `{ "hashes": ["<md4>", …] }` (non-empty, max 500).

**Response:** the [bulk `results` envelope](#bulk-mutations-and-the-results-envelope), keyed by hash. Per-item `error.code` is `not_found`, `download_completed`, `amuled_rejected`, or `ec_unavailable`.

**Errors:** `400 bad_request` (missing/empty `hashes`), `503 ec_unavailable`.

#### `PATCH /api/v0/downloads/{hash}`

**Auth:** `ADMIN`

Mutates one or more fields of a single partfile. `{hash}` is the 32-char MD4 hex hash (case-insensitive).

**Body:** at least one of:

- `action` — `"pause"`, `"resume"` or `"stop"`. A command, not a state, which is why it is not spelled `status`: the read-side `status` has eleven values and this accepts three of them, in a different tense. `"pause"` halts transfer but keeps the file's sources; `"stop"` additionally drops all known sources and resets the Kad source search (a stopped file must rediscover sources from scratch on resume); `"resume"` clears either state. A stopped file reports `status: "stopped"` in the download object (see [`GET /downloads`](#get-apiv0downloads)).
- `priority` — `"low"` / `"normal"` / `"high"` / `"auto"`. Downloads support only these levels and any other value is a `400`; the reason is that the daemon's `.part.met` loader would clamp it back to `normal` on the next restart. (Shared files support the wider `very_low` … `release` set — see [`PATCH /shared/{hash}`](#patch-apiv0sharedhash) and [Priority levels](#priority-levels).)
- `category_index` — uint8
- `a4af_auto` — bool. Turns automatic A4AF source-swapping on or off for this file. A named value, not a flip: sending `true` twice leaves it `true`. This is the only way to set the flag; the `swap_this_auto` action on [`POST /downloads/{hash}/a4af`](#post-apiv0downloadshasha4af) that used to toggle it is gone, because a toggle cannot survive a retry (see [Idempotency](#idempotency)).
- `my_comment` + `my_rating` — set the file's comment (string, ≤ 50 chars) and rating (integer `0`–`5`). Must be sent **together**; only settable when the partfile is also shared (≥ 1 complete chunk), else `409 not_shared`. Primarily a shared-file action — see [`PATCH /shared/{hash}`](#patch-apiv0sharedhash).
- `name` — rename the file (string). Must be non-empty and contain no path separators (`/` or `\`). See the [Takeover flow](#get-apiv0downloadshashfilenames).

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"action":"pause"}' \
  "http://$HOST/api/v0/downloads/8b54a3c2..."
```

**Response:** `200 OK` — the updated download object (full detail envelope including `progress.parts`).

**Errors:** `400 bad_request` (`{hash}` is not 32 hex characters, no recognised field, invalid enum, or `my_comment`/`my_rating` sent alone), `409 not_shared` (comment/rating on a non-shared file), `400 amuled_rejected`, `404 not_found`, `503 ec_unavailable`.

#### `DELETE /api/v0/downloads/{hash}`

**Auth:** `ADMIN`

Cancels an **active** partfile and deletes its on-disk data. `{hash}` is the 32-char MD4 hex hash (case-insensitive). amuled runs `EC_OP_PARTFILE_DELETE` → `CPartFile::Delete()`, which removes the `.part`, `.part.met`, and `.met.bak` files and adds the hash to its `canceledfiles` list (so re-adding the same ed2k link is silently refused until the operator clears that list out-of-band). Completed entries are out of scope; use [`POST /downloads_clear_completed`](#post-apiv0downloads_clear_completed) instead.

```sh
curl -s -X DELETE -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads/8b54a3c2..."
```

**Response:** `204 No Content`.

**Errors:** `400 bad_request` (`{hash}` is not 32 hex characters), `400 amuled_rejected`, `404 not_found`, `409 download_completed`, `503 ec_unavailable`.

#### `POST /api/v0/downloads_clear_completed`

**Auth:** `ADMIN`

Acks one or more entries in amuled's post-completion notification staging buffer. The on-disk file in the Incoming directory stays in place; this endpoint only clears amuled's "completed transfers awaiting acknowledgement" list. Active partfiles are out of scope; use [`DELETE /api/v0/downloads/{hash}`](#delete-apiv0downloadshash) instead.

Two request shapes share this endpoint:

```sh
# Bulk: no body. Clears every completed entry in one EC roundtrip.
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/downloads_clear_completed"

# Per-entry: clear a single completed entry by hash.
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"hash": "8b54a3c2..."}' \
  "http://$HOST/api/v0/downloads_clear_completed"
```

The response envelope is identical for both shapes:

```json
{ "results": [ { "id": "<md4>", "ok": true }, { "id": "<md4>", "ok": true } ] }
```

One entry per cleared hash, in the shared [`results` envelope](#bulk-mutations-and-the-results-envelope). Bulk form returns `200 OK` with an empty `results` array when nothing matches, which is the no-op and stays distinguishable from an amuled rejection (a 4xx with an error envelope). Per-entry form returns `404 not_found` if the hash does not exist and `409 not_completed` if it exists but is not on the completed staging list (an active partfile, where the caller probably wants `DELETE /downloads/{hash}` instead).

**Errors:** `400 amuled_rejected`, `400 bad_request` (malformed body or non-string `hash`), `404 not_found`, `409 not_completed`, `503 ec_unavailable`.

---

### Clients

#### `GET /api/v0/clients`

**Auth:** `GUEST`

Lists the clients amuled is currently exchanging with.

**Query parameters:**

- `activity=uploading` — clients we are currently uploading to (`upload_state == "uploading"`).
- `activity=downloading` — clients we are currently downloading from (`download_state == "downloading"`).
- `activity=active` — clients that are either uploading or downloading right now.
- Default (no `activity`) — every known client, including queued.

The values are spelled the way `upload_state` / `download_state` spell them, so the parameter and the field it selects on read alike. Any other value is a `400 bad_request`.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/clients?activity=active"
```

```json
{
  "clients": [
    {
      "ecid": 4382,
      "name": "AnonymousPeer",
      "user_hash": "1f2e3a...",
      "ip": "203.0.113.42",
      "country_code": "de",
      "port": 4662,
      "software": "emule",
      "software_version": "0.50a",
      "reported_os": "Linux",
      "upload_state": "uploading",
      "download_state": "idle",
      "ident_state": "identified",
      "download_file_name": null,
      "upload_file_name": "example-distribution.iso",
      "upload_file_hash": "8b54a3c20fae9e4b9f7e0c2c8c01b6b1",
      "download_file_hash": null,
      "uploaded_bytes_session": 22000000,
      "downloaded_bytes_session": 0,
      "uploaded_bytes_total": 452000000,
      "downloaded_bytes_total": 189000000,
      "upload_speed_bytes_per_second": 22000,
      "download_speed_bytes_per_second": 0,
      "upload_queue_position": 0,
      "remote_queue_position": 0,
      "upload_queue_score": 150,
      "obfuscation_state": "enabled",
      "connected": true,
      "protocol_extensions": 0,
      "friend_slot": false,
      "source_origin": "kad",
      "parts_offered_count": 42,
      "client_mod_name": null,
      "shared_files_browsable": false,
      "part_progress_percent": 87.5
    }
  ]
}
```

The last five were originally detail-only and were promoted onto this row (and onto the `client_added` / `client_updated` SSE payloads) so a client rendering a client list does not have to fan out a detail request per row. `part_progress_percent` is `null`, not a sentinel, when the client is not a source for anything we are downloading — see the detail section below for what all five mean.

`ecid` identifies the remote *client*, not a file — it's the URL key for [`GET /api/v0/clients/{ecid}`](#get-apiv0clientsecid) and the identity carried in `client_removed` SSE payloads. `user_hash` is the client's stable identity *when published* (clients without SecIdent or in their first session don't have one), so `ecid` is the always-populated handle.

`upload_file_hash` / `download_file_hash` are the 32-char MD4 hex hashes of the partfile or shared file the client is currently transferring with — directly resolvable against [`/api/v0/downloads/{hash}`](#get-apiv0downloadshash) (in-progress) or the corresponding entry in [`/api/v0/shared`](#get-apiv0shared) by `.hash`. Either field is `null` when the client is queued / idle in that direction. `download_file_name` is the filename the client advertised in `OP_REQFILENAMEANSWER` and is populated only while we're actively downloading from them. `upload_file_name` is the partfile the client is downloading **from us**, resolved locally against our own partfile list — present only while we're uploading to them.

`software` and `software_version` are locale-independent, per the API's English-only contract. `software` is one of the tokens in the enumerated-fields table below; `software_version` is a free-form string. A client the daemon could not identify reports `"software": "unknown"` and `"software_version": null` — never a daemon-localized string, since the daemon's own version formatting is gettext-translated and is deliberately not surfaced here. The two differ because `software` is an enum with an `unknown` member to name that case, while `software_version` is free text with no such member, so an unrecorded version is `null` like every other unknown value. `reported_os` is the client's *own* self-reported OS string (raw external data, not normalized by amuled) and is frequently `null`, since most clients don't send it.

`ident_state` is the client's secure-identification (SecIdent) state, one of `"not_available"` (the client does not support SecIdent, or this build has no crypto), `"id_needed"` (its public key is known but the signature exchange has not completed), `"identified"` (verified), `"id_failed"` (signature verification failed), `"bad_guy"` (verified earlier, but currently connecting from a *different* IP than the one it was verified on) or `"unknown"` (state not yet reported for a newly seen client). `"bad_guy"` is also briefly reported for a legitimate client that reconnected after an IP change and has not re-identified yet, so treat it as a hint rather than a verdict.

**Enumerated string fields.** Every client field below carries one of a fixed set of lowercase, locale-independent tokens — the daemon decodes amuled's internal enums server-side so consumers never need the lookup tables. The complete sets:

| Field | Values |
|---|---|
| `upload_state` | `uploading`, `queued`, `waiting_callback`, `connecting`, `pending`, `low_to_low_ip`, `banned`, `error`, `idle`, `unknown` |
| `download_state` | `downloading`, `queued`, `connected`, `connecting`, `waiting_callback`, `waiting_callback_kad`, `requesting_hashset`, `no_needed_parts`, `too_many_connections`, `too_many_connections_kad`, `low_to_low_ip`, `banned`, `error`, `idle`, `remote_queue_full`, `unknown` |
| `ident_state` | `not_available`, `id_needed`, `identified`, `id_failed`, `bad_guy`, `unknown` |
| `obfuscation_state` | `undefined`, `enabled`, `supported`, `not_supported`, `disabled`, `unknown` |
| `software` | `emule`, `cdonkey`, `lxmule`, `amule`, `shareaza`, `emule_plus`, `hydranode`, `mldonkey`, `lphant`, `edonkey_hybrid`, `edonkey`, `old_emule`, `compat`, `unknown` |
| `source_origin` | `server`, `kad`, `source_exchange`, `passive`, `link`, `source_seeds`, `search_result`, `unknown` |

Every one of them falls back to `"unknown"` for a code the daemon does not map, so a client can treat `"unknown"` as its default branch and never has to handle an unexpected token. Three of them are also nullable on the live client objects: `software`, `obfuscation_state` and `source_origin` are `null` when the daemon never reported the field at all, which is a different thing from reporting a code we could not map. `upload_state`, `download_state` and `ident_state` are never `null` — the daemon always answers those. Note the two distinct sentinels on `obfuscation_state`: `"undefined"` is *the client has not told us yet*, `"unknown"` is *the daemon received a code it does not recognise*. The authoritative mappings are the `Client*Name()` / `SourceOriginName()` functions in `src/webapi/Refresher.cpp`.

`connected` says whether a socket to this peer is up right now. A row existing in this list does not answer that: the daemon holds a client object from the first contact attempt, so a peer it is still trying to reach - or can never reach - appears here with `connected: false`. It is `null` on a daemon that does not report peer connectivity. The `online` fields on [`GET /friends`](#get-apiv0friends), [`GET /chats`](#get-apiv0chats) and [`GET /known_clients`](#get-apiv0known_clients) are the same fact reached by their own keys.

`protocol_extensions` is the peer's eMuleAI vendor capability word, taken verbatim from `EC_TAG_CLIENT_MOD_CAPABILITIES` — the same value the desktop GUI renders as its *Protocol extensions* row. It is a bitfield, not one of the enumerated tokens above, because a peer claims any combination of the bits rather than one state: `0x01` extended source exchange, `0x02` uTP NAT traversal, `0x04` IPv6, `0x08` serving-buddy pull, `0x10` QUIC NAT traversal. The daemon has already cleared every bit it does not define, so a bit set here is one aMule names; a bit aMule does not name reads as absent, not as unknown. `0` means the peer claimed nothing, which is what nearly every peer on the network does — a peer that sent no capability tag and one that sent an all-zero word are the same state on the wire and report the same here.

It says what the *peer* supports, never what this daemon does: aMule implements none of these extensions yet and advertises none of them back, so the field is recognition only. The bit meanings are wire format shared with eMuleAI and are defined in `src/PeerCapabilities.h`.

`country_code` is the client's ISO 3166-1 alpha-2 country code (lowercase, e.g. `"de"`), resolved server-side from the client IP by the daemon's GeoIP database. It is `null` when GeoIP is disabled or unsupported by the build, or when the IP does not resolve — render the flag and localized country name client-side from the code. The flag image is served by [`GET /flags/{code}.png`](#get-flagscodepng); the localized name has no endpoint because the browser already has it (`Intl.DisplayNames` with `{ type: "region" }`).

**Errors:** `400 bad_request` (unknown filter token), `503 ec_unavailable`.

---

#### `GET /api/v0/clients/{ecid}`

**Auth:** `GUEST`

Returns the full detail object for a single client — every field [`GET /clients`](#get-apiv0clients) returns for that client, **plus** the detail-only fields below. `{ecid}` is the client's `ecid` (the EC connection id). Bare object, no list envelope.

`ecid`, not `user_hash`, is the resource key: not every client has a hash (unidentified / some LowID / eDonkey clients expose an empty one), a hash is not unique among a client's simultaneous connections, and it is unauthenticated unless the client uses Secure Identification. `ecid` is always present and unique per live connection. Trade-off: `ecid` is reassigned when amuled restarts, so a detail URL is **not** stable across restarts — use the `user_hash` field for a durable reference.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/clients/4382"
```

```json
{
  "ecid": 4382,
  "name": "AnonymousPeer",
  "user_hash": "1f2e3a...",
  "ip": "203.0.113.42",
  "country_code": "de",
  "port": 4662,
  "software": "emule",
  "software_version": "0.50a",
  "reported_os": "Linux",
  "upload_state": "uploading",
  "download_state": "idle",
  "ident_state": "identified",
  "download_file_name": null,
  "upload_file_hash": "8b54a3c20fae9e4b9f7e0c2c8c01b6b1",
  "download_file_hash": null,
  "uploaded_bytes_session": 22000000,
      "downloaded_bytes_session": 0,
      "uploaded_bytes_total": 452000000,
      "downloaded_bytes_total": 189000000,
  "upload_speed_bytes_per_second": 22000,
  "download_speed_bytes_per_second": 0,
  "upload_queue_position": 0,
  "remote_queue_position": 0,
  "upload_queue_score": 150,
  "obfuscation_state": "enabled",
  "connected": true,
  "protocol_extensions": 0,
  "friend_slot": false,
  "ed2k_user_id": 3232238090,
  "high_id": true,
  "server_ip": "203.0.113.9",
  "server_port": 4242,
  "server_name": "eD2K Test Server",
  "kad_port": 4672,
  "source_origin": "kad",
  "upload_file_name": "example-distribution.iso",
  "parts_offered_count": 42,
  "client_mod_name": null,
  "shared_files_browsable": false,
  "friend": false,
  "credit_ratio": 1.0,
  "part_progress_percent": 87.5
}
```

The detail fields mirror the desktop "Client Details" modal. Five of the fields below — `source_origin`, `parts_offered_count`, `client_mod_name`, `shared_files_browsable` and `part_progress_percent` — are **not** detail-only: they are on the [`GET /clients`](#get-apiv0clients) row and the SSE payload too, and are described here because this is where the rest of their neighbours live. `ed2k_user_id` is the client's hybrid eD2k id; `high_id` is `true` for a HighID client (id ≥ `16777216`, i.e. `0x1000000`) and `false` for LowID — the same threshold and the same spelling as `ed2k.high_id` on [`GET /status`](#get-apiv0status), so the value means the same thing on both ends of the API. `server_ip` / `server_port` / `server_name` describe the eD2k server the client connects through, and all three are `null` together when the server is unknown. `kad_port` is non-zero when the client is reachable on Kad, and `null` when the client has no recorded address at all — it is nulled together with `ip` and `port`, the way [`GET /known_clients`](#get-apiv0known_clients) has always nulled the three. `source_origin` is how the client was discovered (values in the enumerated-fields table under [`GET /clients`](#get-apiv0clients)). (`upload_file_name` is part of the base field set — see [`GET /clients`](#get-apiv0clients) above.) `parts_offered_count` is the count of parts the client holds of the linked file, or `null` when the client has not reported a part map (see [Unknown values](#unknown-values)); `client_mod_name` is the client's client-mod string (often `""`); `shared_files_browsable` is `true` when the client allows browsing its shared files, and `false` when it forbids it. `friend` is `true` when the client is in your friends list (`CUpDownClient::IsFriend()`) — **distinct** from `friend_slot`, which is a *reserved upload slot* granted to a client and can be set for non-friends. `credit_ratio` is the upload score modifier the GUI labels "DL/UP modifier" (`GetScoreRatio()`). `part_progress_percent` is the client's completeness of the file we are downloading **from** them (`parts_offered_count` over that file's part count) and is `null` when there is no linked download or the part count is unknown (see [Unknown values](#unknown-values)).

> `friend` and `credit_ratio` ride two EC tags added for this endpoint. A webapi built against a newer core talking to an **older** amuled that doesn't send them degrades gracefully — `friend` reads `false` and `credit_ratio` reads `0`.

**Errors:** `400 bad_request` (`{ecid}` is not a non-negative integer), `404 not_found` (no client with that ecid in the current snapshot), `405 method_not_allowed` (non-GET), `503 ec_unavailable`.

---

#### `POST /api/v0/clients/{ecid}/shared_files`

**Auth:** `ADMIN`

Browse a client's shared file list — the API equivalent of "View Files" in the GUI. `{ecid}` is the client's `ecid`. amuled opens (or reuses) a direct client-to-client connection to that client and asks it to enumerate its shared directories and files.

The browse runs **asynchronously**: the client answers over the network, one directory at a time, and a HighID/reachable client may take seconds while a LowID client needs a server callback or Kad first. So this endpoint does **not** return the files — it returns a `search_id` and the results flow through the **search machinery**, exactly like a query search:

- `GET /api/v0/search/{id}/results` reads the accumulated files as they arrive (standard search-result fields, plus `directory` — the folder each file sits in inside the client's share). The browse also appears in [`GET /api/v0/search`](#get-apiv0search) with `type: "browse"` and the browsed client's `client_ecid`.
- The refresher advances `search_progress` for this `search_id` while the browse is live, and completion arrives as the terminal `search_progress` frame with `"state": "finished"` — when the client's list is complete or the browse fails (denied / client unreachable / connection lost). There is **no** `search_finished` event; a client waiting for one waits forever. A denied or failed browse finishes with zero results, so there is no distinct error event either.

Reusing the search id-space means one poll loop and one SSE stream cover both queries and browses; a client tells them apart by remembering which `search_id` it started with which verb.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/clients/4382/shared_files"
```

**Response:** `202 Accepted`, with a `Location: /api/v0/search/{search_id}` header and the browse as the body -- the same row [`GET /search`](#get-apiv0search) lists, with `type: "browse"` and `query` holding the client's name:

```json
{
  "search_id":   17,
  "query":       "some client",
  "type":        "browse",
  "state":       "running",
  "client_ecid": null,
  "started_at":  1750412400
}
```

`client_ecid` is `null` in this reply even though you addressed the client by ecid: the row is built locally before the daemon has reported anything about the browse, and `client_ecid` is one of the fields it fills in. [`GET /search`](#get-apiv0search) carries the client's ecid for the same browse from the next refresher tick onward.

A browse the daemon cannot even start — a LowID client it has no way to call back, for instance — is reported as `finished` immediately rather than left pending: no connection is attempted, so there is nothing to wait for. It carries no results, the same as a browse the client denied.

**Idempotent while a browse is running.** Asking again for a client that is already being browsed returns **the same `search_id`** rather than starting a second browse — amuled will not re-ask a client that is still answering, so a second id would name a browse that never happens. Two clicks therefore leave one browse, one id and one entry on [`GET /api/v0/search`](#get-apiv0search). Once that browse has settled, a fresh request starts a new one with a new id.

Status `202 Accepted` — the browse was started, not completed. Then poll:

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/search/17/results"
```

**Errors:** `400 bad_request` (`{ecid}` is not a non-negative integer), `403 forbidden` (guest token — browsing is `ADMIN`-only), `404 not_found` (no client with that ecid), `405 method_not_allowed` (non-POST), `502 amuled_rejected` (core accepted the request but returned no `search_id`), `503 ec_unavailable`.

---

### Known clients

#### `GET /api/v0/known_clients`

**Auth:** `GUEST`

Lists every client the daemon has ever exchanged data with, from its credit store.

Distinct from [`GET /clients`](#get-apiv0clients), which lists the clients connected **right now**. The two differ in identity as well as content: a live client is keyed by `ecid`, which is meaningful only within one daemon process, while a known client is keyed by `user_hash` and survives daemon restarts. Correlate the two on `user_hash`; the `online` field says whether we are actually connected to that peer at this moment.

Standard [list envelope](#list-pagination-and-sorting) under the `known_clients` key, with `limit` / `offset` / `sort` / `order`.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/known_clients?sort=last_seen_at&order=desc&limit=2"
```

```json
{
  "known_clients": [
    {
      "user_hash": "a1b2c3d4e5060e708090a0b0c0d06f00",
      "name": "example-client",
      "ip": "192.0.2.10",
      "port": 4665,
      "kad_port": 4675,
      "country_code": "de",
      "software": "amule",
      "software_version": "v2.3.1",
      "source_origin": "kad",
      "obfuscation_state": "supported",
      "uploaded_bytes_total": 0,
      "downloaded_bytes_total": 0,
      "last_seen_at": 1786652714,
      "first_seen_at": 1786652714,
      "session_count": 1,
      "online": false
    }
  ],
  "total": 392, "offset": 0, "limit": 2
}
```

| Field | Notes |
|---|---|
| `user_hash` | 32-char lowercase MD4. The identity; join with `/clients`. |
| `name` | Client-chosen name. `null` when never recorded. |
| `ip`, `port`, `kad_port` | Last address the client was seen at. `null` together. |
| `country_code` | ISO 3166-1 alpha-2, lowercase, resolved by the daemon's GeoIP. `null` when GeoIP is off or the address does not resolve. Artwork: [`GET /flags/{code}.png`](#get-flagscodepng). |
| `software`, `software_version` | Same tokens `GET /clients` uses. `null` together. |
| `source_origin` | How the client was first found — `server`, `kad`, `source_exchange`, `passive`, … |
| `obfuscation_state` | Protocol-obfuscation state as of the last session. |
| `uploaded_bytes_total`, `downloaded_bytes_total` | Lifetime bytes, from the credit record. Always present. |
| `last_seen_at` | Unix seconds. Always present. For a client that is connected this is *now* — it is being seen — so the connected records are the most recent in the store under `sort=last_seen_at&order=desc`. A client that left during the current tick carries the same timestamp and ties with them; ties keep a stable order across requests. |
| `first_seen_at`, `session_count` | `null` together, and non-null only for a record the daemon holds metadata for. |
| `online` | Whether a connection to this peer is up right now, correlated by `user_hash`. `null` when the daemon does not report peer connectivity. Not the same as having a row in [`GET /clients`](#get-apiv0clients): the daemon holds a client object from the first contact attempt, including for a peer it never reaches. |

**Optional fields are `null`, never omitted and never emitted empty.** Every key above is written through `Write*OrNull`, so the shape of a row does not change with what the daemon knows: a record written before the daemon kept per-client metadata carries the hash, the totals and `last_seen_at` as values and the rest as `null`, which is how a consumer tells "never recorded" from "recorded as empty". On a long-lived node most records are of that kind. This follows the surface-wide rule in [Unknown values](#unknown-values); `GET /search` is the documented exception where absence itself is the meaning.

The store is read from the daemon **once**, on the first request, and maintained from there: every refresher tick folds the connected clients back in, so later requests never touch EC at all. That is sound rather than a shortcut — a record whose client is not connected cannot change, since credit totals only move during a transfer and `last_seen_at` is written at disconnect. What the maintenance covers:

- a client that connects is added, with `first_seen_at` and `session_count` set to what the daemon recorded when it said hello;
- a connected client's `online`, `uploaded_bytes_total` and `downloaded_bytes_total` track the live client state;
- a bare record gains its name, address, software and origin once its client identifies;
- a connected client's `last_seen_at` is now, and a client that leaves has `online` cleared with `last_seen_at` stamped at the moment it went.

The cost is one EC roundtrip per amuleapi process, and the store stays resident from first use.

**Errors:** `405 method_not_allowed` (non-GET/HEAD), `502 amuled_response_invalid` (the daemon answered the history request with something that is not a client list), `503 ec_unavailable`, `503 ec_unsupported` (the connected amuled predates the client-history request — it is never sent to a daemon that does not advertise support).

---

### Shared files

#### `GET /api/v0/shared`

**Auth:** `GUEST`

Lists every file the local node is sharing. The `sources.complete` counter is amuled's estimate of how many clients in the swarm hold the file complete.

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
      "size_bytes":       3217,
      "priority":         "normal",
      "priority_auto":    false,
      "sources":          { "complete": 12 },
      "uploaded_bytes_session":         5242880,
      "uploaded_bytes_total":           314572800,
      "request_count_session":          42,
      "request_count_total":            1837,
      "accepted_request_count_session": 18,
      "accepted_request_count_total":   921,
      "upload_speed_bytes_per_second": 51200,
      "uploading_client_count": 2,
      "last_upload_at":      1700000500,
      "shared_since_at":     1699000000,
      "hashed_part_count": 0,
      "media": {
        "duration_seconds": 212,
        "bitrate_kilobits_per_second": 320,
        "codec":    "mp3",
        "artist":   "Some Artist",
        "album":    "Some Album",
        "title":    "Some Title"
      }
    }
  ]
}
```

`uploaded_bytes_session` / `uploaded_bytes_total` are bytes uploaded during the current amuled process vs over the file's lifetime. `requests` counts how many clients have asked for the file; `accepts` counts how many of those requests were granted an upload slot. The `session` counters reset on amuled restart; `total` is persisted in `known.met`.

`upload_speed_bytes_per_second` is the file's current combined upload rate in bytes/sec (summed over the clients it is uploading to), and `uploading_client_count` is how many clients it is actively uploading to right now — together the "is this file being seeded" signal, the upload-side analogue of the `/downloads` speed + transferring-source counts. Subtract `uploading_client_count` from the queued-client count (`upload_queue_count`, on the detail view) to show `uploading / queued`. Both are live and refresh every tick. `last_upload_at` is the unix timestamp of the last time data was sent for the file, and `shared_since_at` is when the file was completed or first shared; both are persisted in `known.met` and are `null` when unknown: a file that has never uploaded, or a `known.met` entry written before these fields existed.

`priority` is the upload priority — `"very_low"` / `"low"` / `"normal"` / `"high"` / `"release"` — and `priority_auto` is `true` when amuled is deriving that level automatically from the upload queue. This mirrors the `/downloads` shape (base `priority` + separate `priority_auto` flag); on an auto file `priority` reports the current derived level, not the literal string `"auto"`. For a file that is both downloading and shared this upload priority is independent of the download priority reported by [`GET /api/v0/downloads`](#get-apiv0downloads).

`hashed_part_count` is the number of parts hashed so far by a pass running over the file — a [`POST /shared/{hash}/verify`](#post-apiv0sharedhashverify) run, or an AICH hashset rebuild — and `0` when nothing is hashing. It is a count of completed parts, not the index of the part in flight, so it runs `0` → `total_part_count`, which is on this row.

A file that is both downloading and shared reports its progress here as well: amuled describes such a file as a partfile, so the value is read across from the download side and the two agree. That makes `hashed_part_count` usable from either list without checking which one owns the file.

`media` is an object on an audio or video file that has been probed and `null` otherwise — the key is always present, so test `media === null` rather than checking for the key. Its six fields are the same ones the detail endpoint reports, and a [media refresh](#post-apiv0sharedmediarefresh) replaces all of them, clearing any the new probe no longer finds.

The SSE `shared_added` / `shared_updated` event payload matches this object byte-for-byte, so a subscriber that received `shared_updated` does not need to re-GET to see the moved counters — including `media`, which is what makes a metadata refresh observable without polling.

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
| `file_type` | string | Category token derived from the extension: `"audio"`, `"video"`, `"archive"`, `"cd_image"`, `"picture"`, `"text"`, `"program"`, or `"unknown"`. |
| `upload_ratio` | number | `xfer.total / size`; `0` when `size == 0`. |
| `directory` | string | Directory path of the on-disk file — the temp directory while the file is still an incomplete partfile, the destination directory once it has completed. Identical to `directory` on `/downloads/{hash}` for the same file. |
| `incomplete` | bool | `true` while the file is still an incomplete partfile, `false` once complete. Always present. A download that has finished but has not been cleared yet reports `false`, since its data already sits in the destination directory. |
| `sources.complete_min` / `sources.complete_max` | int | The low and high ends of the estimated full-copy source range behind `sources.complete`. Two scalars, not a nested object. |
| `aich_hash` | string\|null | AICH master hash (hex), or `null` until the hashset exists. |
| `total_part_count` | int | Total parts, `ceil(size / 9.28 MiB)`. |
| `parts` | array | Per-part source availability, `[{ "sources": int }, ...]`, exactly `total_part_count` entries in file order. **`null`** until the first decode has landed, so "no data yet" and "no sources for any part" stay distinguishable. The key is always present. See below. |
| `upload_queue_count` | int | Clients waiting on this file's upload queue. |
| `my_comment` | string | The user's own comment on this file (`""` if none). |
| `my_rating` | int | The user's own rating, `0`–`5` (`0` = unrated). |
| `media` | object | Audio/video metadata — see [Media metadata](#media-metadata). **`null`** when the file has no probed metadata; the key is always present. |

`hashed_part_count` comes through from the list item; pair it with `total_part_count` here for a percentage.

`parts[].sources` is how many clients currently requesting this file hold that part — an **availability** measure, not a progress one. A shared file is fully local by definition, so a part with `"sources": 0` means no other client has it and you are its only source. Counts saturate at `255`.

This is deliberately detail-only. A 100 GB file has ~10 800 parts, so carrying the array on [`GET /shared`](#get-apiv0shared) or in `shared_updated` SSE events would multiply that across the whole share on every tick — the same reason `progress.parts` is absent from the downloads list. `shared_updated` events are unaffected by source-count changes.

For a shared file that is also still downloading, the same values are available as `progress.parts[].sources` on [`GET /downloads/{hash}`](#get-apiv0downloadshash); both come from one encoder in amuled, so they agree.

**Errors:** `400 bad_request` (`{hash}` is not 32 hex characters), `404 not_found` (no shared file with that hash), `503 ec_unavailable`.

#### `GET /api/v0/shared/{hash}/content`

**Auth:** `GUEST` — `HEAD` is accepted too; any other method is `405 method_not_allowed` with `Allow: GET, HEAD`.

Downloads the bytes of a completed shared file. `{hash}` is the 32-char MD4 hex hash (case-insensitive), the same one [`GET /shared`](#get-apiv0shared) reports. This is the only route that serves library content: everything else on the surface describes files, this one hands them over.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/shared/8b54a3c20fae9e4b9f7e0c2c8c01b6b1/content" -o file.bin
```

**Response headers** on a `200` or `206`:

| Header | Value |
|---|---|
| `Content-Type` | `application/octet-stream`, always |
| `Content-Disposition` | `attachment; filename="<name>"; filename*=UTF-8''<name>` |
| `X-Content-Type-Options` | `nosniff` |
| `Content-Security-Policy` | `default-src 'none'; sandbox` |
| `Accept-Ranges` | `bytes` |
| `ETag` | `"<mtime>-<size>"`, hex, set by this handler rather than hashed from the body |
| `Content-Length` | the exact number of bytes the response carries |
| `Content-Range` | on a `206` and a `416` only |
| `X-Accel-Buffering` | `no` on every streamed response. A zero-byte file is answered with a buffered empty `200` and does not carry it - there is nothing to stream and nothing for a proxy to spool. |
| `Vary` | `Accept-Encoding` on every file response. |

**Never `inline`, never the file's real media type.** A completed download lands in Incoming, which is itself shared, so both the bytes and the filename came from strangers on the ed2k network — and amuleapi serves the Web UI from this same origin. A shared `evil.html`, or a scripted `.svg`, returned under its "real" type would execute against the user's own session. `octet-stream` + `attachment` + `nosniff` + a sandbox CSP are four independent reasons the browser will not run it. The filename is sanitised for the quoted form and percent-encoded for the extended one, so a name carrying quotes or control characters cannot forge further parameters or split the header.

**Ranges.** A single byte range is served as `206 Partial Content` with a `Content-Range`; `bytes=0-9` and the suffix form `bytes=-100` both work. A range that starts at or past EOF is `416 range_not_satisfiable` with `Content-Range: bytes */<size>`, so the client learns the true length without a second request.

**A multi-range request gets a `200` carrying the whole file** — not a `206`, not a `416`, and never a `multipart/byteranges` body. `Range: bytes=0-99,500-599` is answered exactly as though no `Range` header had been sent, so a client that asks for two small windows of a 4 GB file will be handed 4 GB unless it checks the status. A client that needs several windows must issue one request per window and treat any status other than `206` as "the range was not honoured".

That is one case of a general rule: everything the handler does not recognise as a single satisfiable range is **ignored** rather than refused, and answered `200` with the full body — an unrecognised range unit (per RFC 9110 §14.2), a value too large for a 64-bit offset (rejected, never wrapped), and the multi-range form above. RFC 7233 §3.1 permits a server to ignore `Range`, and never emitting `multipart/byteranges` is what makes the CVE-2011-3192 "Apache Killer" amplification shape a no-op here: a request naming hundreds of overlapping windows costs exactly one linear read of the file, the same as no `Range` header at all.

**`If-Range`.** Honoured, and evaluated between `If-None-Match` and the `Range` itself (RFC 9110 §13.1.5). A resuming client sends the validator it already holds alongside `Range: bytes=N-`; if it still matches, the range is served as usual, and if it does not, the `Range` is **dropped** and the whole current representation is returned as `200`. That is what stops the client from appending bytes of the new file to its copy of the old one and reading the `206` as confirmation that nothing changed.

The comparison is **strong**, unlike the one `If-None-Match` uses on this same route: `W/"<mtime>-<size>"` matches for a `304` and does **not** license a byte range, because a weak validator says two representations are equivalent, not byte-identical. The HTTP-date form of `If-Range` is not implemented and is treated as non-matching — a second-resolution validator compares equal across a replacement that happened inside the same second, which is the race the header exists to close, so the answer is a `200` of the whole file rather than a window taken on a guess. `If-Range` without a `Range` is ignored entirely.

**Conditional GET.** The `ETag` is `mtime-size` rather than a hash of the body — hashing a multi-gigabyte file per request would not be a slow path but an unusable one. `If-None-Match` is answered here rather than by the generic stamping described in [ETag and conditional GET](#etag-and-conditional-get), and accepts `*`, a comma-separated list, and weak `W/"..."` validators. A match is `304 Not Modified` with no content and the `ETag` preserved. It is evaluated **before** `Range`, per RFC 9110 §13.2.2, so a conditional request that also carries a `Range` answers `304`, never `206`. The validator carries no `-gzip` suffix because a file response is never compressed: there is exactly one representation to name, and `Accept-Encoding` cannot produce a `Content-Encoding` on this route.

**Concurrency.** The transport caps concurrent file responses at 6 by default, because each one pins a file descriptor and a streaming buffer for as long as the client takes to drain it. Over the cap the answer is `503 file_responses_exhausted` with a `Retry-After`, the same shape the SSE session cap uses — an honest refusal rather than a queue that grows without bound. Within the cap the body streams through a fixed 64 KiB buffer and is never held in memory, so a multi-gigabyte transfer moves the daemon's RSS by kilobytes.

The cap is `amuleapi.conf[Streaming]/MaxConcurrentFileResponses`, accepted in `1..256` and falling back to 6 for anything else. It is a **global** budget, not a per-client one: behind a reverse proxy every request arrives from the proxy's address, so six slots are six slots for the whole household, and a `X-Forwarded-For`-aware per-user allowance does not exist on this surface. Six suits one mechanical disk that the hasher and the ed2k uploader are already using; an SSD-backed NAS serving several devices can afford more. A `HEAD` never claims a slot.

**Behind a reverse proxy.** The response carries `X-Accel-Buffering: no`, nginx's opt-out from response buffering, for the same reason the [event stream](EVENTS.md#response-headers) does — with the default `proxy_buffering on`, nginx spools the response to its own disk before the client sees the first byte, up to `proxy_max_temp_file_size` (1 GB out of the box). That both delays the transfer and copies the whole file onto the proxy host, which is precisely the I/O cost streaming off disk was meant to avoid. nginx honours the header, so no configuration is needed; if your proxy does not, set `proxy_buffering off;` (or its equivalent) for this location.

**Same-host only.** The bytes are read from the filesystem **amuleapi** is running on, not amuled's. The two are usually the same machine, but the EC endpoint is configurable, so amuleapi can be pointed at a remote amuled — and then the daemon's paths mean nothing here. The desktop remote GUI carries a whole path-mapping layer for exactly this split; amuleapi has no equivalent, and does not guess. A path that resolves to nothing is `503 ec_content_unreachable`; a path that resolves to a *different* file that merely shares a name is caught by the size invariant and is `503 ec_content_mismatch` rather than being served under the wrong hash. Both say the deployment is misconfigured, not that the request was wrong. On a split deployment, download through amuled instead.

**Errors:** `401 unauthorized` (no token), `404 not_found` (no shared file with that hash — and every path-resolution refusal, collapsed into the same reply so it cannot be used to probe the share layout), `405 method_not_allowed`, `409 partfile_unsupported`, `416 range_not_satisfiable`, `503 path_unavailable`, `503 ec_content_unreachable`, `503 ec_content_mismatch`, `503 file_responses_exhausted`, `503 ec_unavailable`.

Only completed files can be downloaded. A file still downloading is rejected with `409 partfile_unsupported`: a partfile's on-disk layout is gapped and its offsets do not correspond to the completed file's, so a byte range taken out of it would be silently **wrong** rather than merely unavailable.

`503 path_unavailable` carries `Retry-After: 5` and is transient rather than an error in the request. The file's directory rides an EC tag amuled emits only on the frames where it changed, so a snapshot taken before the first such frame knows the file but not where it lives. The resource exists; it just cannot be addressed yet.

#### `POST /api/v0/shared_reload`

**Auth:** `ADMIN`

Equivalent to the desktop client's "Reload" button — amuled re-walks its shared directories and updates the file list.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/shared_reload"
```

Returns `202 Accepted`. amuled schedules the re-walk and answers immediately, so the response confirms only that the reload was **scheduled** — it never carries the outcome. The walk begins on amuled's next processing tick, within about a second, and a large or network-mounted share tree can take minutes to finish.

Repeated calls coalesce: requesting a reload while one is already pending, or while a walk is in progress, results in a single further walk rather than one per call.

**Reading the result.** The walk brackets itself with two amule log lines, so read them back from [`GET /api/v0/logs/amule`](#get-apiv0logsamule) or the `logs` SSE channel. Both are localised and the second is pluralised, so a client matching on them should pin the daemon's locale:

- `Reloading shared files...` when the walk starts
- `Found 1234 known shared files` when it ends

The resulting changes also arrive as `shared_added` / `shared_removed` events on the `shared` SSE channel, which is the better signal if you only care about the delta.

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/shared/media/refresh`

**Auth:** `ADMIN`

Re-extract media metadata for every shared file, whether or not it already has some.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/shared/media/refresh"
```

```json
{ "scope": "all", "queued_file_count": 812 }
```

Returns `202 Accepted`. `queued_file_count` is how many files were **accepted for probing**, not how many produced metadata — files the scheduler drops (not audio/video by extension, an incomplete download, missing on disk) are not counted, and nothing has been extracted yet when the response returns.

This is the only way to correct metadata that is *wrong* rather than missing. The normal scheduler skips any file that already carries a media tag, so a value stored by an older build — a cover-art codec, or a preview inherited from a search result before the local probe could overwrite it — is otherwise permanent short of deleting `known.met`, which would also discard the ed2k part hashes and every per-file statistic.

A refresh also retries files a previous probe could not read. amuled records that ffprobe already ran on a file and found nothing usable, so a broken or truncated one is not re-probed on every share reload and every restart; that record is what a refresh clears. Repair the file, or install a working ffprobe, and ask for a refresh -- there is no need to touch `known.met`.

Each probe **replaces** every media field, including *clearing* one the new probe no longer finds, so a refresh corrects a value in both directions. Nothing else about a file is touched: statistics, comment, rating, upload priority, AICH hash set and share state all survive, the file is not re-hashed, its ed2k hash does not change, and it never leaves the share.

The work runs on amuled's media-probe worker, one file at a time, so downloads and uploads are unaffected and the daemon stays responsive. Shutting down mid-refresh is clean — files not yet reached keep their previous values. Progress is observable through [`GET /api/v0/logs/amule`](#get-apiv0logsamule) and, as each probe lands, `shared_updated` SSE events.

Cost is roughly 13 ms per file — a probe reads the container header, not the file — so a 10 000-file library is on the order of two minutes of background work.

**Errors:** `400 amuled_rejected` (media metadata extraction is disabled in amuled's preferences), `503 ec_unsupported` (the connected amuled predates this operation), `503 ec_unavailable`.

`queued: 0` means the share held no eligible file, which is a legitimate answer for a share with no audio or video in it. It is no longer how a disabled feature reports itself: that is a `400`, so the two cannot be confused.

#### `POST /api/v0/shared/{hash}/media/refresh`

**Auth:** `ADMIN`

The same operation for a single file, which is the quickest way to check a fix on one file rather than a whole library.

```json
{ "scope": "file", "queued_file_count": 1 }
```

**Errors:** `404 not_found` (no shared file with that hash), `409 partfile_unsupported` (an incomplete download has no complete file to read), `400 amuled_rejected` (media metadata extraction is disabled, or the file is not eligible: not audio/video, or an incomplete download), `503 ec_unsupported`, `503 ec_unavailable`.

#### `GET /api/v0/share_directories`

**Auth:** `GUEST`

The share roots amuled is configured with — as opposed to [`GET /shared`](#get-apiv0shared), which lists the files those roots produced. This is the *intent*: a recursive root is one entry here however many subdirectories it covers.

```sh
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/share_directories"
```

```json
{
  "directories": [
    { "path": "/home/user/media", "recursive": true },
    { "path": "/home/user/iso", "recursive": false }
  ]
}
```

`recursive` distinguishes the two lists amuled keeps (`shareddir-recursive.dat` vs `shareddir-explicit.dat`). The runtime union it derives from them — every expanded subdirectory — is deliberately not exposed: it is generated state, not configuration.

**Errors:** `502 amuled_rejected`, `503 ec_unavailable`.

#### `PUT /api/v0/share_directories`

**Auth:** `ADMIN`

Replace the whole set of roots. A full replace rather than a merge, because that is exactly what amuled's operation does — making it a `PUT` keeps the semantics honest instead of hiding a read-modify-write.

```sh
curl -s -X PUT -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"directories":[{"path":"/home/user/media","recursive":true}]}' \
  "http://$HOST/api/v0/share_directories"
```

```json
{ "results": [ { "id": "/srv/share", "ok": true } ] }
```

`recursive` is optional and defaults to `false`.

amuled validates every path: a REST client cannot stat the core's filesystem, so a typo would otherwise become a silently dead share. Paths that pass are applied and persisted before the response returns, and every submitted path gets an entry either way, so **one bad entry does not discard the edit** and a caller can tell an applied path from one the response simply did not mention. A response with any rejection is `207 Multi-Status`, as with every other multi-item mutation:

```json
{
  "results": [
    { "id": "/srv/share", "ok": true },
    { "id": "/typo", "ok": false, "error": { "code": "not_found", "message": "no such directory" } }
  ]
}
```

`error.code` is `not_found` (missing, or not a directory) or `not_readable` (`403`, amuled cannot read the path). amuled reports these as codes and the API renders them, so its locale never leaks into your response.

The **rescan is scheduled, not completed**, before the response returns: a successful reply means the new roots are validated and persisted, and that the re-walk will start on amuled's next processing tick. Until it finishes, [`GET /api/v0/shared`](#get-apiv0shared) still serves the previous file list. Observe completion the same way as [`POST /api/v0/shared_reload`](#post-apiv0shared_reload), whose notes on log lines and coalescing apply here too.

**Errors:** `400 bad_request` (`directories` not an array, an entry without a non-empty `path`, non-boolean `recursive`), `502 amuled_rejected`, `503 ec_unavailable`.

#### `POST /api/v0/share_directories`

**Auth:** `ADMIN`

Add a single root, leaving the others alone — the convenience path for scripts that would otherwise have to read the list, splice it and write it back.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"path":"/home/user/new","recursive":true}' \
  "http://$HOST/api/v0/share_directories"
```

Idempotent: adding a path that is already configured updates its `recursive` flag rather than failing, so "ensure this folder is shared" is safe to repeat. Same `{results: [...]}` envelope as `PUT`.

**Errors:** `400 bad_request` (missing/empty `path`, non-boolean `recursive`), `502 amuled_rejected`, `503 ec_unavailable`.

#### `DELETE /api/v0/share_directories`

**Auth:** `ADMIN`

Remove a single root. The path is a query parameter rather than a path segment because it is an absolute filesystem path.

Pass the **exact** `path` string returned by `GET /share_directories`, percent-encoded — the match is byte-for-byte, so a differing separator or drive-letter case will not match. This is what makes Windows roots work: percent-encoding carries the backslashes, the `C:` colon, and any spaces through unchanged.

```sh
# POSIX root: /home/user/new
curl -s -X DELETE -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/share_directories?path=%2Fhome%2Fuser%2Fnew"

# Windows root: C:\Users\bob\My Shares
curl -s -X DELETE -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/share_directories?path=C%3A%5CUsers%5Cbob%5CMy%20Shares"
```

Removing a path that is not configured is a `404` rather than a silent success, so a typo — or a path that does not byte-match what `GET` returned — is visible. Same `{results: [...]}` envelope as `PUT`.

**Errors:** `400 bad_request` (no `path` parameter), `404 not_found` (path not configured), `502 amuled_rejected`, `503 ec_unavailable`.

> Concurrency: `POST` and `DELETE` are read-modify-write against amuled's whole-list operation, serialised inside amuleapi so two API clients cannot lose each other's change. Nothing can make them atomic against a *simultaneous* edit from amuleGUI — the protocol has no compare-and-set — so that case is last-write-wins.

#### `POST /api/v0/shared/{hash}/verify`

**Auth:** `ADMIN`

Equivalent to the desktop client's "Verify Local Data" — amuled re-hashes the file's on-disk data against the MD4 (and, where a hashset is available, AICH) hashes it has stored, reporting any blocks that no longer match.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/shared/$HASH/verify"
```

Returns `202 Accepted`, with no body. amuled queues the hashing task and answers immediately, so the response confirms only that the re-hash was **scheduled** — it never carries the outcome, and a large file may take minutes to finish.

**Watching it run.** While the task is hashing, `hashed_part_count` on the file's [`GET /shared`](#get-apiv0shared) row counts the parts done so far, and each advance pushes a `shared_updated` SSE event — enough to drive a progress bar without polling. It returns to `0` when the task finishes or aborts, which is the signal that the log line below is available.

**Reading the result.** The verdict is emitted as an amule log line when the task completes, so read it back from [`GET /api/v0/logs/amule`](#get-apiv0logsamule) or the `logs` SSE channel:

- `Verify Local Data (MD4 & AICH): Result OK for <path>`
- `Verify Local Data (MD4 & AICH): ERRORS FOUND! <path> Failed blocks: MD4: 3,7 AICH: 5: (0,2)`

Like all daemon log output these lines are gettext-translated at the daemon's locale and carry no correlation id tying them to a specific request, so treat them as human-readable output rather than a machine-parseable contract.

**Errors:** `400 bad_request` (`{hash}` is not 32 hex characters), `400 amuled_rejected` (the daemon refused the verify), `404 not_found` (no shared file with that hash), `409 partfile_unsupported`, `500 internal_error`, `503 ec_unavailable`.

Only completed files can be verified. A file still downloading is rejected with `409 partfile_unsupported`: the hashing task skips partfiles outright, so accepting one would promise a report that never arrives. A download that has *finished* but is still listed under `/downloads` is a valid target.

#### `PATCH /api/v0/shared`

**Auth:** `ADMIN`

Bulk upload-priority change over multiple shared files — the same `priority` applied to every listed hash.

**Body:** `{ "hashes": ["<md4>", …], "priority": "<level>" }` — a non-empty `hashes` array (max 500) plus a required `priority` (`very_low` | `low` | `normal` | `high` | `release` | `auto`).

**Response:** the [bulk `results` envelope](#bulk-mutations-and-the-results-envelope) (`200` all ok / `207` partial / `503`), keyed by hash. Per-item `error.code` is `not_found`, `amuled_rejected`, or `ec_unavailable`.

**Errors:** `400 bad_request` (missing/empty `hashes`, missing/invalid `priority`), `503 ec_unavailable`.

#### `PATCH /api/v0/shared/{hash}`

**Auth:** `ADMIN`

Changes the upload priority and/or the comment+rating of a single shared file. `{hash}` is the 32-char MD4 hex hash (case-insensitive). The body must include at least one of `priority` or the `my_comment`+`my_rating` pair.

**Body:**

```json
{
  "priority": "very_low" | "low" | "normal" | "high" | "release" | "auto",
  "my_comment":  "<string, ≤ 50 chars>",
  "my_rating":   0
}
```

Send a bare priority level to pin it (the file's `priority_auto` becomes `false`). Send `"auto"` to hand level selection to amuled — it derives the level from the upload queue, and `GET /api/v0/shared` then reports the derived base `priority` with `priority_auto: true`. The combined `"*_auto"` strings are not accepted as input, since `"auto"` is the level the daemon computes rather than one the caller pins.

`my_comment` and `my_rating` must be sent **together** (both or neither) — the daemon writes them as one atomic operation. `my_comment` is capped at 50 characters; `my_rating` is an integer `0`–`5`. Setting them requires the file to be shared. The same fields are accepted on [`PATCH /downloads/{hash}`](#patch-apiv0downloadshash) for a downloading file that is also shared.

`name` renames the file — a non-empty string with no path separators (`/` or `\`, rejected to prevent the rename escaping the file's directory). Rename works on any known file, so it is accepted on both this endpoint and [`PATCH /downloads/{hash}`](#patch-apiv0downloadshash).

**Errors:** `400 bad_request` (`{hash}` is not 32 hex characters, missing/invalid fields, `my_comment`/`my_rating` sent alone, or a `name` that is empty or contains a path separator), `404 not_found` (no shared file with that hash), `409 not_shared` (comment/rating on a non-shared file), `400 amuled_rejected`, `503 ec_unavailable`.

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
      "software_version": "17.15",
      "address": "203.0.113.5:4242",
      "ip":      "203.0.113.5",
      "country_code": "de",
      "port": 4242,
      "user_count": 312000,
      "max_user_count": 500000,
      "file_count": 75000000,
      "soft_file_limit": 1000,
      "hard_file_limit": 5000,
      "priority": "normal",
      "ping_ms": 42,
      "failed_count": 0,
      "permanent": false,
      "tcp_flags": {
        "bitmask": 1497,
        "compression": true,
        "new_tags": true,
        "unicode": true,
        "related_search": true,
        "type_tag_integer": true,
        "large_files": true,
        "tcp_obfuscation": true
      },
      "udp_flags": {
        "bitmask": 1851,
        "get_sources": true,
        "get_files": true,
        "new_tags": true,
        "unicode": true,
        "get_sources_v2": true,
        "large_files": true,
        "udp_obfuscation": true,
        "tcp_obfuscation": true
      }
    }
  ]
}
```

`country_code` is the ISO 3166-1 alpha-2 code (lowercase, e.g. `"de"`) of the server host, resolved server-side from the server IP by the daemon's GeoIP database — same semantics as the client `country_code` on `/clients`, `null` when unresolved, and the same artwork route, [`GET /flags/{code}.png`](#get-flagscodepng).

`file_count` is how many files the server indexes. `soft_file_limit` and `hard_file_limit` are something else entirely: the per-user publishing limits the server advertises. Below the soft limit a client may publish every file it shares, between soft and hard only its rarest, above the hard limit nothing. Both arrive only once the server has answered a UDP status request, so **`0` means "not reported yet", not "the limit is zero"** — render it blank rather than as a number, the way the desktop's Soft Files / Hard Files columns do. `user_count`, `max_user_count` and `file_count` share that sentinel.

`failed_count` is the number of consecutive failed connection attempts, not a boolean.

`tcp_flags` and `udp_flags` are the eD2k wire capabilities the server announced, decoded server-side so a consumer never needs the protocol tables. Every key below is always present — `false` when the bit is clear — so there is no need to branch on key existence. `bitmask` carries the raw value alongside, both for diagnostics and so a bit a newer server announces that this build does not name yet is still visible. A server that has announced nothing yet reports `bitmask: 0` with every boolean `false`.

`tcp_flags`:

| Key | Bit | Meaning |
|---|---|---|
| `compression` | `0x0001` | zlib-compressed packets |
| `new_tags` | `0x0008` | compact tag encoding |
| `unicode` | `0x0010` | Unicode strings |
| `related_search` | `0x0040` | related-files search — whether a `related::<hash>` local search will work against this server |
| `type_tag_integer` | `0x0080` | integer file-type tags |
| `large_files` | `0x0100` | files larger than 4 GiB |
| `tcp_obfuscation` | `0x0400` | TCP protocol obfuscation |

`udp_flags`:

| Key | Bit | Meaning |
|---|---|---|
| `get_sources` | `0x0001` | extended GetSources request |
| `get_files` | `0x0002` | extended GetFiles request |
| `new_tags` | `0x0008` | compact tag encoding |
| `unicode` | `0x0010` | Unicode strings |
| `get_sources_v2` | `0x0020` | GetSources v2 |
| `large_files` | `0x0100` | files larger than 4 GiB |
| `udp_obfuscation` | `0x0200` | UDP obfuscation |
| `tcp_obfuscation` | `0x0400` | TCP obfuscation, announced over UDP |

Both objects spell out the transport on their obfuscation keys because `udp_flags` legitimately carries both bits; a key means exactly one wire capability in either object.

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/servers`

**Auth:** `ADMIN`

Add a server to amuled's known-server list.

**Body:**

```json
{ "address": "203.0.113.5:4242", "name": "eMule Server" }
```

`name` optional; `address` required and must parse as `host:port`.

**Response:** `202 Accepted`, no body. `EC_OP_SERVER_ADD` answers success or failure and never returns the server it made, so anything reported here would be a reconstruction from the snapshot after an inline refresh -- a guess that can silently come back short. Re-read [`GET /servers`](#get-apiv0servers) for the new entry.

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

#### `POST /api/v0/servers/{ecid}/connect` / `POST /api/v0/servers/by-address/{address}/connect`

**Auth:** `ADMIN`

Tells amuled to disconnect from its current server and dial the specified one. Two route shapes are equivalent — `{address}` is `<ip>:<port>`, and the address form looks up the ECID by exact `(ip, port)` match against the server cache before delegating to the ECID handler. It has its own `by-address` path rather than being sniffed out of `{ecid}`, so nothing about the URL is ambiguous. Hostname-form addresses do NOT resolve here — pass the literal IP.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/servers/by-address/203.0.113.5:4242/connect"
```

**Response:** `202 Accepted`, no body. The connect is asynchronous; its outcome shows up on [`GET /status`](#get-apiv0status)'s `ed2k.state` and on the SSE stream.

**Errors:** `400 bad_request` (unparseable address/ECID), `400 amuled_rejected` (the daemon refused the connect), `404 not_found`, `503 ec_unavailable`.

#### `DELETE /api/v0/servers/{ecid}` / `DELETE /api/v0/servers/by-address/{address}`

**Auth:** `ADMIN`

Removes the server from amuled's list.

**Response:** `204 No Content`.

**Errors:** `400 bad_request` (`{ecid}` is not a non-negative integer, or `{address}` is not a dotted quad with a port in 1–65535), `400 amuled_rejected`, `404 not_found` (well-formed but no such server), `503 ec_unavailable`.

#### `PATCH /api/v0/servers/{ecid}` / `PATCH /api/v0/servers/by-address/{address}`

**Auth:** `ADMIN`

Sets an ed2k server's priority, its static flag, or both — the same operation the desktop server list offers from its context menu.

**Body:** both fields are optional, and only the ones present are applied; a body with neither is a `400`.

```json
{ "priority": "high", "permanent": true }
```

`priority` is one of `"low"` / `"normal"` / `"high"` — the same values `GET /servers` reports. `permanent` marks the server as one amuled keeps across list updates.

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"priority":"high","permanent":true}' \
  "http://$HOST/api/v0/servers/1"
```

**Response:** `200 OK` → the full server object as it now stands, the same shape [`GET /servers`](#get-apiv0servers) lists. A `PATCH` answers with the state the caller just produced, so no re-read is needed to see it.

**Errors:** `400 bad_request` (unknown `priority`, non-bool `permanent`, or neither field present), `400 amuled_rejected`, `404 not_found`, `503 ec_unavailable`.

#### `POST /api/v0/servers_update`

**Auth:** `ADMIN`

Tells amuled to fetch the `server.met` from the supplied URL and refresh its list. Same operation the desktop GUI's "Update server list from URL" button drives.

**Body:**

```json
{ "url": "http://example.com/server.met" }
```

`url` is **required** here, and must start with `http://` or `https://`; omitting it, or sending anything else, is a `400 bad_request`. This differs from [`POST /ipfilter/update`](#post-apiv0ipfilterupdate), which does fall back to its configured preference when the field is absent.

All three `*/update` endpoints -- servers, Kad nodes and the IP filter -- name this field `url` (R6). Each used to repeat a noun the path already carries (`servers_url`, `nodes_url`, `ipfilter_url`).

The URL is **persisted** into the `servers.update_url` preference, so a subsequent `GET /preferences` reflects it — there is no need to PATCH it separately.

**Response:** `202 Accepted`, no body. The download runs asynchronously -- its outcome arrives on the log channel.

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

---

### Friends

The friends list amuled persists to `emfriends.met`. The daemon ships the whole list inside the update every client already receives, so `GET /friends` costs no roundtrip of its own.

`{ecid}` is the friend's own id, distinct from the client ECIDs on `/clients`. Like every ECID it does **not** survive an `amuled` restart — use `user_hash` as the durable reference where a friend has one. A friend added by address alone has no hash.

#### `GET /api/v0/friends`

**Auth:** `GUEST`

```json
{
  "friends": [
    {
      "ecid": 12,
      "name": "alice",
      "user_hash": "a1b2c3d4e5060e708090a0b0c0d06f00",
      "ip": "203.0.113.42",
      "port": 4662,
      "client_ecid": 4382,
      "online": true,
      "friend_slot": false
    }
  ],
  "total": 7,
  "offset": 0,
  "limit": 7
}
```

`client_ecid` is the live client this friend is currently linked to, joinable against [`GET /api/v0/clients`](#get-apiv0clients), and `null` when no client object is held for the friend. `online` is a different question and answers it directly: whether a connection to the peer is up. A friend can have a `client_ecid` and be `false` here, which is the ordinary state for one the daemon is trying, or failing, to reach. `null` means the daemon does not report peer connectivity. `user_hash` is `""` for a friend added by address only; `ip` and `port` are `null` for a zero address, and the `friend_*` events emit the same nulls.

`friend_slot` reads `false` against a daemon predating the tag that carries it, the same way `friend` and `credit_ratio` degrade on `/clients`.

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/friends`

**Auth:** `ADMIN`

Two mutually exclusive body forms.

Promote a connected client:

```json
{ "client_ecid": 4382 }
```

Or add by address, where `ip` and `port` are required and `port` is an integer in `1..65535` (the same contract [`POST /api/v0/kad/bootstrap`](#post-apiv0kadbootstrap) enforces), `user_hash` must be 32 hexadecimal characters when given, and `name` defaults to the address:

```json
{ "ip": "203.0.113.42", "port": 4662, "name": "alice", "user_hash": "a1b2c3d4e5060e708090a0b0c0d06f00" }
```

Sending `client_ecid` together with any address field is a `400`.

**Response:** `202 Accepted`, no body. EC's `FRIEND` op answers success or failure and never returns the record it created, so the only way to name the new friend here was to diff the snapshot against a pre-add copy and hope the inline refresh had already surfaced it. Re-read [`GET /friends`](#get-apiv0friends), keyed on the address or hash the request carried.

**Errors:** `400 bad_request`, `404 not_found` (no connected client with that `client_ecid`), `400 amuled_rejected`, `503 ec_unavailable`.

#### `DELETE /api/v0/friends/{ecid}`

**Auth:** `ADMIN`

**Response:** `204 No Content`.

Removing the friend that currently holds the friend slot clears it.

**Errors:** `400 bad_request` (`{ecid}` is not a non-negative integer), `404 not_found`, `400 amuled_rejected`, `503 ec_unavailable`.

#### `PATCH /api/v0/friends/{ecid}`

**Auth:** `ADMIN`

**Body:** `{ "friend_slot": true }` — the only mutable field.

**Response:** `200 OK` → the updated friend object.

Only one friend can hold the slot at a time, so granting it clears it on whoever held it before. A single call therefore changes two records and emits two `friend_updated` events; the response body describes only the friend named in the URL.

**Errors:** `400 bad_request`, `404 not_found`, `400 amuled_rejected`, `503 ec_unavailable`.

#### `POST /api/v0/friends/{ecid}/shared_files`

**Auth:** `ADMIN`

Browse a friend's shared files. The friend-addressed twin of [`POST /api/v0/clients/{ecid}/shared_files`](#post-apiv0clientsecidshared_files), and more capable: a friend record carries a stored address, so the daemon can browse a friend who is **not currently connected**, which the clients route cannot do.

**Response:** `202 Accepted`, with a `Location` header and the browse row as the body, exactly as on the clients route. Poll [`GET /api/v0/search/{id}/results`](#get-apiv0searchidresults) with the `search_id` it carries. Idempotent while a browse of that client is running, exactly as on the clients route.

**Errors:** `400 bad_request` (`{ecid}` is not a non-negative integer), `403 forbidden`, `404 not_found`, `502 amuled_rejected`, `503 ec_unavailable`.

### Categories

amuled's category system lets users tag downloads with one of N user-defined buckets (separate save directory, separate priority, separate color). Category 0 is the default "Uncategorized" and cannot be deleted.

#### `GET /api/v0/categories`

**Auth:** `GUEST`

```json
{
  "categories": [
    {
      "index": 0,
      "name": "Default",
      "save_path": "/home/user/aMule/Incoming",
      "comment": "",
      "color": "#1664c0",
      "priority": "low"
    }
  ],
  "total": 1,
  "offset": 0,
  "limit": 1
}
```

A list endpoint like the others: `?limit`, `?offset`, `?sort` and `?order` apply, and the `total` / `offset` / `limit` trio describes the window. See [List pagination and sorting](#list-pagination-and-sorting); the sort keys are `index` and `name`.

Category `0` is always present, so the list is never empty. amuled's EC omits the row entirely until the first custom category exists, so amuleapi synthesises it when missing:

```json
{ "index": 0, "name": "Default", "save_path": "/home/user/aMule/Incoming", "comment": "", "color": "#1664c0", "priority": "low" }
```

`name` and `save_path` are filled in for index `0` whether the row came from the daemon or was synthesised here. amuled holds neither -- its `defaultcat` is built with an empty title and path -- so a client rendering a category picker was left with a blank row it had to label itself, and nothing to show for where an uncategorised download lands. `save_path` is `directories.incoming_path` from [`GET /preferences`](#get-apiv0preferences), which is genuinely where such a file is saved. `priority` is `low`, amuled's own default for the row.

Filling both in unconditionally is deliberate: doing it only for the synthesised row would mean `/categories/0` answered `"Default"` on a daemon with no custom categories and `""` as soon as the operator added one, which is a response shape that depends on unrelated state.

**Errors:** `400 bad_request` (bad list params), `503 ec_unavailable`.

#### `POST /api/v0/categories`

**Auth:** `ADMIN`

**Body:**

```json
{
  "name": "Linux ISOs",
  "save_path": "/home/user/aMule/Incoming/Linux",
  "comment": "Distros only",
  "color": "#1664c0",
  "priority": "high"
}
```

`name` required; others optional. `color` is a `"#rrggbb"` string in both
directions -- it used to be the raw 24-bit integer amuled stores, which a
client had to unpack itself, and which is easy to get wrong: the core packs
it as `0x00BBGGRR` with **red in the low byte**, so a naive hex print of the
integer comes out reversed. Anything that is not `#` followed by six hex
digits is a `400 bad_request`. `priority` accepts the same six levels the category read side can return — `"very_low"` / `"low"` / `"normal"` / `"high"` / `"release"` / `"auto"` — so a read-modify-write round-trip always succeeds (R9). It is applied to the category's member files as a download priority.

**`save_path` is resolved on the daemon's filesystem, not yours,** so it is accepted without being checked here. Omit it and the category is created on the incoming directory, which is amuled's own default. Give a path amuled cannot use — it does not exist and cannot be created — and the category is still created, on the incoming directory again. Either way this is a success, and [`GET /categories/{index}`](#get-apiv0categoriesindex) reports the path that was actually stored. Compare it with what you sent if it matters to you; a `save_path` that comes back different is the daemon saying it could not use yours.

**Response:** `202 Accepted`, no body. `EC_OP_CREATE_CATEGORY` answers success or failure and never returns the index it assigned, so naming the new category here meant scanning the snapshot for one with a matching name and falling back to a bodiless `201` when the scan came up short. Re-read [`GET /categories`](#get-apiv0categories) for the assigned index.

**Errors:** `400 bad_request`, `400 amuled_rejected`, `503 ec_unavailable`.

#### `GET /api/v0/categories/{index}`

**Auth:** `GUEST`, matching the collection read.

Returns the single category object, the same shape [`PATCH`](#patch-apiv0categoriesindex) returns. Every other resource with a member path has a member `GET`; this one did not, so a client that had just created a category and wanted the stored result had to re-fetch the whole collection and search it by index.

`{index}` is a uint8. A non-numeric or out-of-range segment is `400 bad_request`; an index no category holds is `404 not_found`. Index `0` is always present, synthesised when amuled omits it and carrying the same `name` / `save_path` fill-in, exactly as on the collection: the two routes cannot disagree about which categories exist or about what they hold.

**Errors:** `400 bad_request`, `404 not_found`, `503 ec_unavailable`.

#### `PATCH /api/v0/categories/{index}`

**Auth:** `ADMIN`

Any subset of the POST body fields. `index 0` (the default category) can be patched but not deleted.

**Response:** `200 OK` with the category object as stored, the same shape [`GET /categories/{index}`](#get-apiv0categoriesindex) returns, so a client can see what landed without a follow-up read.

`save_path` follows the same rule as on create: amuled resolves it on its own filesystem, and when it cannot use the path it keeps the one the category already had. The rest of the request still applies — `name`, `comment`, `color` and `priority` all land — and the echoed object reports the path that was kept. That is the one case where a field you sent is not the field you get back, which is why the response echoes the object rather than answering `204`.

**Errors:** `400 bad_request` (non-numeric or out-of-range `{index}`, malformed `color`, unknown `priority`), `404 not_found` (no category at that index), `400 amuled_rejected`, `503 ec_unavailable`.

#### `DELETE /api/v0/categories/{index}`

**Auth:** `ADMIN`

**Response:** `204 No Content`.

**Errors:** `400 bad_request` (non-numeric or out-of-range `{index}`, or index `0`), `404 not_found` (no category at that index), `400 amuled_rejected`, `503 ec_unavailable`.

Deleting `index 0` is refused here, before any EC roundtrip: `400 bad_request` ("cannot delete the default (index=0) category"). amuled is never asked.

---

### Preferences

#### `GET /api/v0/preferences`

**Auth:** `GUEST`

Returns every preference category amuled carries over EC. The `general` and `connection` sub-objects are the original common-case set; the remaining categories map 1:1 to the daemon's own settings and mirror the desktop "Preferences" tabs.

```json
{
  "general": {
    "nickname": "MyNode",
    "user_hash": "abcd...",
    "daemon_host_name": "host.example.com",
    "version_check_enabled": true
  },
  "connection": {
    "max_upload_kibibytes_per_second": 50,
    "max_download_kibibytes_per_second": 0,
    "upload_slot_min_kibibytes_per_second": 3,
    "tcp_port": 4662,
    "udp_port": 4672,
    "extended_udp_port_enabled": true,
    "max_sources_per_file_count": 250,
    "max_connection_count": 400,
    "autoconnect": true,
    "reconnect_on_connection_loss": true,
    "ed2k_enabled": true,
    "kad_enabled": true,
    "bind_address": "",
    "bind_interface": "",
    "proxy_enabled": false,
    "proxy_type": "socks5",
    "proxy_host": "",
    "proxy_port": 1080,
    "proxy_auth_enabled": false,
    "proxy_user": "",
    "upnp_supported": true,
    "upnp_enabled": false,
    "upnp_control_point_port": 50000
  },
  "directories": {
    "incoming_path":   "/home/me/.aMule/Incoming",
    "temp_path":       "/home/me/.aMule/Temp",
    "shared_paths":    ["/home/me/media"],
    "share_hidden":    false,
    "rescan_on_startup": true,
    "follow_symlinks": false,
    "exclude_patterns": "",
    "exclude_patterns_use_regex": false
  },
  "files": {
    "ich_enabled": true, "trust_unverified_aich_hashes": false,
    "add_new_downloads_paused": false, "new_downloads_auto_priority": false,
    "new_shared_files_auto_priority": false,
    "prioritize_first_last_chunks": false, "on_finished_start_next_paused": false,
    "on_finished_start_next_in_same_category": false,
    "save_sources_for_rare_files": true, "preallocate_full_file_size": false,
    "mmap_supported": true, "mmap_enabled": false,
    "stop_on_low_disk_space": true, "min_free_space_mebibytes": 1, "create_sparse_files": true,
    "on_finished_start_next_alphabetically": false, "endgame_mode_enabled": false,
    "media_metadata_enabled": true, "ffprobe_path": ""
  },
  "servers": {
    "remove_dead_servers": true, "dead_server_retry_count": 3, "update_list_at_startup": false,
    "update_list_from_server": true, "update_list_from_client": true,
    "server_priority_system_enabled": true,
    "smart_lowid_check_enabled": true, "safe_server_connect_enabled": false,
    "autoconnect_static_servers_only": false, "manual_servers_high_priority": false,
    "update_url": "http://upd.emule-security.org/server.met"
  },
  "security": {
    "shared_files_visibility": "everybody",
    "ipfilter_clients_enabled": true, "ipfilter_servers_enabled": true,
    "ipfilter_auto_update": false, "ipfilter_update_url": "",
    "ipfilter_min_access_level": 127, "ipfilter_include_lan_ips": true,
    "secure_identification_enabled": true,
    "protocol_obfuscation_enabled": true, "obfuscation_requested": true, "obfuscation_required": false,
    "reject_spoofed_source_ips": true, "system_ipfilter_enabled": false
  },
  "message_filter": {
    "enabled": false, "filter_all_messages": false,
    "accept_from_friends_only": false, "accept_from_known_clients_only": false,
    "filter_by_keyword": false, "keywords": "",
    "log_filtered_messages": true, "filter_comments": false, "comment_keywords": ""
  },
  "remote_controls": {
    "webserver": {
      "enabled": false, "port": 4711, "gzip_enabled": true,
      "refresh_seconds": 120, "template_name": "", "guest_enabled": false
    },
    "amuleapi": { "enabled": true, "port": 4713, "bind_address": "0.0.0.0" }
  },
  "online_signature": { "enabled": false, "directory": "/home/me/.aMule", "update_frequency_seconds": 5 },
  "advanced": {
    "max_new_connections_per_5_seconds": 200, "verbose_logging": false,
    "file_buffer_bytes": 240000, "max_upload_queue_client_count": 5000,
    "server_keepalive_timeout_minutes": 0, "kad_max_concurrent_source_search_count": 50,
    "kad_source_reask_minutes": 30, "source_reask_minutes": 15
  },
  "kad": { "update_url": "http://upd.emule-security.org/nodes.dat" },
  "geoip": {
    "supported": true, "enabled": true, "source": "dbip",
    "custom_update_url": "", "maxmind_license": "", "auto_update": true,
    "loaded_source": "dbip", "db_path": "/home/me/.aMule/GeoIP/dbip.mmdb",
    "db_loaded": true, "download_in_progress": false, "last_update_status": "ok"
  }
}
```

Booleans are plain JSON `true`/`false` regardless of how amuled encodes them on the wire. **Passwords are never returned** — the webserver admin/guest and amuleapi passwords are write-only (see PATCH). `general.user_hash` is the node's own identity hash, not a password.

`connection.extended_udp_port_enabled` is positive-sense: `true` means the extended UDP port (Kad / global search) is on. `security.shared_files_visibility` is a 3-state enum string, not a bool: `"everybody"` / `"friends"` / `"nobody"`.

`geoip` is the GeoIP (IP-to-country) config category. `supported` is a capability flag: `false` when the connected daemon is built without GeoIP — the config fields are then present but inert. `source` is one of `"dbip"` / `"maxmind"` / `"custom"` (the next-download database selector). `maxmind_license` is returned plainly (it is a config string the daemon already round-trips, not a masked password). `loaded_source`, `db_path`, `db_loaded`, `download_in_progress`, and `last_update_status` are **read-only** live status (the currently loaded DB and any in-flight refresh); they are ignored if sent on PATCH.

`files.media_metadata_enabled` / `files.ffprobe_path` control media-metadata extraction: when enabled, the daemon probes shared audio/video with `ffprobe` to advertise length, bitrate, codec, artist, album and title. `ffprobe_path` is a **daemon-side** path — an empty string means the daemon auto-detects the binary, trying `ffprobe` on its `PATH` and then a per-platform list of well-known install locations. The detected path is deliberately **not** written back: it describes the daemon's machine rather than a choice you made, so `ffprobe_path` keeps reading as `""` while extraction works, and a daemon that moves to a host with ffmpeg somewhere else re-detects on its own. Set the field explicitly to pin one binary and override detection. When nothing is found the daemon logs one line saying so — visible on [`GET /api/v0/logs/amule`](#get-apiv0logsamule) — and extraction stays inert until ffmpeg is installed or the path is set. `connection.bind_address` (empty = bind to any local IP), `connection.bind_interface` (a daemon-side interface name such as `eth0` / `en0` / `tun0`; empty = any), and `online_signature.directory` are likewise daemon-side paths/addresses. These, together with `files.on_finished_start_next_alphabetically`, `security.reject_spoofed_source_ips`, `security.system_ipfilter_enabled`, and `online_signature.update_frequency_seconds`, are ordinary daemon settings; a `bind_address` change takes effect on the next amuled restart.

`files.mmap_supported` is **read-only** — the daemon advertises whether it was built with memory-mapped file I/O (`false` on a core without mmap support, e.g. Windows or a build with `-DENABLE_MMAP=OFF`); it is ignored if sent on PATCH. `files.mmap_enabled` is the runtime toggle for memory-mapped block I/O — download writes to part files, upload reads of both shared (completed) and partial files, and hashing (lower per-process memory use, at some write-path cost; best for upload-heavy or memory-constrained hosts). It is **capability-gated**: a PATCH that sets `files.mmap_enabled` is rejected with **409 `option_not_supported`** when `files.mmap_supported` is `false`, so the option is only writable against a daemon that can actually use it. Safe to toggle with active transfers.

`connection.upnp_enabled` toggles UPnP router forwarding of the daemon's P2P ports — the ports themselves are `connection.tcp_port` (ed2k TCP) and `connection.udp_port` (ed2k/Kad UDP). `connection.upnp_control_point_port` is a separate optional knob: the fixed local port the UPnP control point (libupnp) binds to for the router's callbacks, `0` meaning auto-assign — **not** a forwarded port. `connection.upnp_supported` is **read-only** — the daemon advertises whether it was built with UPnP (`false` on a core built `-DENABLE_UPNP=OFF`, where `upnp_enabled` has no effect); it is ignored if sent on PATCH. (Web-server and EC-port UPnP are intentionally not exposed — amuleweb is deprecated and the EC port is not a P2P port.)

The `connection.proxy_*` fields configure the proxy the **daemon** routes its P2P and HTTP traffic through. `proxy_type` is one of `"socks5"` / `"socks4"` / `"http"` / `"socks4a"` — any other value is a `400`. It is the empty string when the daemon has no proxy type configured at all (the core's `PROXY_NONE`), a state that cannot be set back through this API; use `proxy_enabled: false` to turn the proxy off. `proxy_auth_enabled` toggles username/password authentication. `proxy_password` is **write-only** — accepted on PATCH but never returned on GET (same as the `remote_controls` passwords); PATCH the other proxy fields without it to leave the stored password unchanged.

**Errors:** `503 ec_unavailable`.

#### `PATCH /api/v0/preferences`

**Auth:** `ADMIN`

**Not every field in the `GET` is writable.** The payload mixes four kinds, and the response does not mark them, so this is the list:

| Kind | What `PATCH` does | Examples |
| --- | --- | --- |
| Settable | applied | most of the 125 -- `files.mmap_enabled`, `connection.max_connection_count`, … |
| Read-only status | ignored, request still succeeds | `files.mmap_supported`, `connection.upnp_supported`, the six `geoip.*` |
| Write-only | applied, never echoed on `GET` | `remote_controls.webserver.password`, `.guest_password` |
| Refused | `400 bad_request` | `remote_controls.amuleapi.password`, `.guest_password`, `.guest_enabled` |

Read-only fields are **ignored rather than rejected** when they arrive alongside at least one writable field, so the obvious way to use this endpoint -- `GET` the object, change one value, `PATCH` the whole thing back -- keeps working. A body naming *only* read-only fields is a `400` that says which ones: `no writable fields in the request; files.mmap_supported is read-only`.

Body shape mirrors the GET; every sub-object and every field is optional, and fields not present are left unchanged. Subset example:

```json
{ "files": { "add_new_downloads_paused": true }, "servers": { "dead_server_retry_count": 5 } }
```

`remote_controls` nests its two independent subsystems as `remote_controls.webserver` and `remote_controls.amuleapi` rather than prefixing every field. It reports amuleapi's `enabled` / `port` / `bind_address`, but **not** whether its admin or guest password is set. Those live in `amuleapi-passwords`, which amuleapi owns and which may sit on a different host from amuled — so the daemon's view of that file can be the wrong one. Ask the API that actually reads it: [`GET /auth/passwords`](#get-apiv0authpasswords), which is admin-only, whereas this endpoint is readable by any authenticated role. `webserver.guest_enabled` is reported because it is a genuine amuled preference rather than a fact about another process's file.

**Write-only passwords** (accepted here, never echoed on GET) live under `remote_controls.webserver`: `password`, `guest_password`. Send the plaintext — amuled stores the hash. `guest_password` is accepted whether or not the webserver's guest access is enabled: amuled stores the hash either way, and it simply sits inert until `guest_enabled` is turned on. (amuleapi's own [`PATCH /auth/passwords`](#patch-apiv0authpasswords) does enforce the pairing, and answers `400`; these are different credentials on different daemons.)

amuleapi's own `admin` and `guest` passwords are **not** settable here; `remote_controls.amuleapi.password`, `.guest_password` and `.guest_enabled` are rejected with `400 bad_request`. Use [`PATCH /auth/passwords`](#patch-apiv0authpasswords), which writes the credential file this daemon actually reads, requires the current password, and is rate-limited. A field here would instead travel over EC to whichever aMule this amuleapi is attached to and land in that host's config directory.

**`geoip`** accepts `enabled`, `source` (`"dbip"` / `"maxmind"` / `"custom"` — any other value is a `400`), `custom_update_url`, `maxmind_license`, and `auto_update`. `supported` and the read-only status fields (`loaded_source`, `db_path`, `db_loaded`, `download_in_progress`, `last_update_status`) are ignored if sent.

Downloading a database **now** is [`POST /geoip/update`](#post-apiv0geoipupdate), not a field here: it is an action, not a setting. Sending `geoip.update_now` in this body is a `400` naming that endpoint.

> **Note:** these are the daemon's live settings — the same ones the desktop GUI edits. Some are self-affecting: changing `remote_controls.amuleapi.port` / `.bind_address`, or `directories.incoming_path` / `temp_path`, alters the very daemon you are talking to. A port/bind change only takes effect on the next amuled restart, so it will not drop your current connection mid-request.

**Response:** `200 OK` — full preferences object (post-mutation), so a read-modify-write client can confirm what landed without a follow-up GET.

**Numeric fields are bounded to what the daemon can actually hold**, not to the width of their JSON type. A value outside the range is a `400` naming the range; it is never accepted and quietly changed. Several of these bounds are much narrower than they look, because the core stores the setting differently from how the API spells it:

| Field | Range | Step | Why |
|---|---|---|---|
| `connection.tcp_port` | `1`–`65532` | | the server UDP socket is TCP+3, and the core substitutes the default port for anything higher |
| `connection.upload_slot_min_kibibytes_per_second` | `1`–`65535` | | the core clamps anything lower up to `1`, so a `0` would answer `200` and read back as `1` |
| `advanced.file_buffer_bytes` | `0`–`3825000` | `15000` | stored as a `uint8` count of 15000-byte blocks |
| `advanced.max_upload_queue_client_count` | `0`–`25500` | `100` | stored as a `uint8` count of hundreds |
| `advanced.max_new_connections_per_5_seconds` | `0`–`65535` | | stored as a `uint16` |
| `advanced.kad_max_concurrent_source_search_count` | `5`–`50` | | clamped to this range at daemon start |
| `advanced.kad_source_reask_minutes` | `30`–`60` | | clamped to this range at daemon start |
| `advanced.source_reask_minutes` | `15`–`60` | | clamped to this range at daemon start; below it the UDP reask gets you auto-banned for reask spam |
| `security.ipfilter_min_access_level` | `0`–`255` | | an ipfilter.dat access level, not a percentage: an entry is enforced when its level is **below** this value, so `0` disables the list and `255` enforces every entry |
| `online_signature.update_frequency_seconds` | `0`–`65535` | | stored as a `uint16` |
| `advanced.server_keepalive_timeout_minutes` | `0`–`71582` | | the core stores milliseconds in a `uint32`, and 71582 minutes is where that overflows |

The two fields with a **step** accept only whole multiples of it: `file_buffer_bytes: 20000` is a `400`, not a silent round down to `15000`. The names stay in the unit you think in rather than being renamed to the daemon's internal one, and the price is that the API is strict about the values in between.

The three **clamped at daemon start** rows are the reason this is enforced on the write rather than left to the client to check. Their setters assign the value raw, so a `GET` right after the `PATCH` reports it back faithfully and only the next restart reveals that the daemon never kept it.

**A low `connection.max_upload_kibibytes_per_second` caps `max_download_kibibytes_per_second`,** which is the one place a `PATCH` changes a field the request did not name. Below `4` KiB/s up the download limit is forced to 3× the upload; below `10` it is forced to 4×. So `PATCH {"connection": {"max_upload_kibibytes_per_second": 3}}` also sets `max_download_kibibytes_per_second` to `9`. This is a deliberate anti-leech rule in the core rather than a defect, it applies whichever of the two you write, and the `PATCH` response echoes the whole preferences object so the adjusted value is visible in the reply.

**Errors:** `400 bad_request` (unknown/mis-typed field, or a body with no recognized fields), `409 option_not_supported` (the daemon was built without support for the option being set), `400 amuled_rejected`, `503 ec_unavailable`.

---

### Network control

These endpoints drive amuled's connect/disconnect to the ed2k network, the Kad network, or both.

#### `POST /api/v0/networks/connect`

**Auth:** `ADMIN`

**Body:** `{ "network": "ed2k" | "kad" | "both" }` (optional; defaults to `"both"`). Same shape as `/networks/disconnect` — `"ed2k"` fires `EC_OP_SERVER_CONNECT`, `"kad"` fires `EC_OP_KAD_START`, omitted/`"both"` fires `EC_OP_CONNECT`.

**Response:** `202 Accepted`, with amuled's own account of what it did:

```json
{ "message": "Connecting to eD2k...; Connecting to Kad..." }
```

`message` is amuled's status text, verbatim; the two networks' lines are joined with `; ` when both act, and it is absent when the daemon said nothing. `202` rather than `200`: the request is handed to amuled over EC and returns before the effect is observable, which is as true of a disconnect as of a connect -- the two used to disagree on this for no reason a client could see.

**Errors:** `400 bad_request` (unknown selector), `400 amuled_rejected` (the daemon refused the operation), `503 ec_unavailable`.

#### `POST /api/v0/networks/disconnect`

**Auth:** `ADMIN`

**Body:** `{ "network": "ed2k" | "kad" | "both" }` (optional; defaults to `"both"`).

**Response:** `202 Accepted`, with amuled's own account of what it did:

```json
{ "message": "Disconnected from eD2k.; Disconnected from Kad." }
```

`message` is amuled's status text, verbatim; the two networks' lines are joined with `; ` when both act, and it is absent when the daemon said nothing. `202` rather than `200`: the request is handed to amuled over EC and returns before the effect is observable, which is as true of a disconnect as of a connect -- the two used to disagree on this for no reason a client could see.

**Errors:** `400 bad_request` (unknown selector), `400 amuled_rejected` (the daemon refused the operation), `503 ec_unavailable`.

> Dedicated `POST /api/v0/kad/connect` and `POST /api/v0/kad/disconnect` shortcuts existed in an earlier draft of v0 but were dropped in favour of the `/networks/{connect,disconnect}` body selector — `{"network":"kad"}` does exactly what they did. The `/kad/bootstrap` endpoint below is genuinely distinct and stays.

#### `POST /api/v0/kad/bootstrap`

**Auth:** `ADMIN`

Manual Kad bootstrap against a single known-good Kad node. Fires `EC_OP_KAD_BOOTSTRAP_FROM_IP` against amuled. This is the only Kad bootstrap surface the EC protocol exposes — `nodes.dat` is read by amuled at startup from its own data directory and is NOT manageable via REST.

**Body:** `{ "ip": "203.0.113.5", "port": <uint16> }`. `ip` is a dotted-quad string, per the [IP addresses](#ip-addresses) rule; the conversion to the integer EC carries happens inside amuleapi. `port` is the contact's UDP port. A `uint32` was accepted here too and is now a `400`: the two spellings disagreed about byte order, so the same address reached the daemon differently depending on how it was written.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"ip":"203.0.113.5","port":4672}' \
  "http://$HOST/api/v0/kad/bootstrap"
```

**Response:** `202 Accepted` → `{ "ip": "1.2.3.4", "port": 4672 }`. The Kad probe itself is fire-and-forget UDP; the `202` confirms amuled accepted the request, not that the contact was reachable.

The echo is the documented exception to the no-body rule for actions: it reports **which address the daemon parsed**, which the caller cannot read back anywhere else. `ip` comes back as a dotted quad whichever form the request used -- answering with the host-order integer meant a client that posted `"1.2.3.4"` and stored the reply held `16909060`, a value no other field on this surface produces and one it could not post back without converting.

**Errors:** `400 bad_request` (missing `ip`, or an `ip` that is not a string — a numeric one included, missing/non-integer `port`, port outside `1..65535`, malformed dotted-quad), `400 amuled_rejected`, `503 ec_unavailable`.

#### `POST /api/v0/kad/update`

**Auth:** `ADMIN`

Downloads a `nodes.dat` from the supplied URL and rebuilds the Kad node list from it — the Kad counterpart of [`POST /api/v0/servers_update`](#post-apiv0servers_update), and the same operation the desktop GUI's "Update node list from URL" button drives.

**Body:**

```json
{ "url": "https://upd.emule-security.org/nodes.dat" }
```

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"url":"https://upd.emule-security.org/nodes.dat"}' \
  "http://$HOST/api/v0/kad/update"
```

Two side effects are worth planning for. The URL is **persisted** into the `kad.update_url` preference, so a subsequent `GET /preferences` reflects it — there is no need to PATCH it separately. And once the download completes, amuled **stops Kad, swaps in the new `nodes.dat`, and starts Kad again**; expect a brief Kad outage and a `kad_state` transition on the SSE stream. The desktop GUI prompts before doing this; the API does not.

**Response:** `202 Accepted`, no body. The URL came from the request, and the download is asynchronous -- the `202` confirms amuled accepted the request, not that the node list was replaced, and its outcome arrives on the log channel.

**Errors:** `400 bad_request` (missing/non-string/empty `url`, or a scheme other than `http://` / `https://`), `400 amuled_rejected`, `503 ec_unavailable`.

#### `GET /api/v0/kad`

**Auth:** `GUEST`

Standalone view of the Kad subtree from `/status`, plus the detail fields the status rollup omits (`node_id`, `firewalled_udp`, `lan_mode`, your `public_ip`, the `indexed` Kad-store counters, and `buddy` contact info for low-ID clients). Together with `GET /api/v0/preferences` (for the TCP/UDP port numbers the firewalled messages quote) this covers every row of the desktop client's **Networks → Kad Info** panel.

```json
{
  "state": "connected",
  "node_id": "8f3a1c07d94b2e5a6018bb4c7f209d3e",
  "firewalled_tcp": false,
  "firewalled_udp": false,
  "lan_mode": false,
  "connected_since_at": 1751000000,
  "public_ip": "203.0.113.5",
  "network": { "user_count": 5400000, "file_count": 1400000000, "node_count": 2400 },
  "indexed": { "sources": 12000, "keywords": 8500, "notes": 0, "load_percent": 14 },
  "buddy": { "state": "connected", "ip": "203.0.113.9", "port": 4672 }
}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | string | `disabled` / `connecting` / `connected`. `disabled` means Kad is not running at all, which is the condition several fields below key their "no measurement" value on. The same value `GET /api/v0/status` reports as `kad.state`. |
| `node_id` | string \| null | This node's own 128-bit Kademlia id, 32 lowercase hex characters (the desktop panel shows the same value uppercase). `null` while Kad is not running, which is exactly when `state` is `disabled`. Persisted by the daemon, so unlike the session-scoped ECIDs and the server-assigned eD2k id it is stable across restarts — the one identifier for the local node a consumer can key on. It is a DHT routing key, not a credential: every Kad contact the daemon talks to learns it. |
| `connected_since_at` | int | Unix seconds of the most recent Kad connect, the same value `GET /api/v0/status` reports as `kad.connected_since_at`. `0` when not connected, so gate on `state` rather than trusting a `0`. |
| `public_ip` | string \| null | This node's externally-visible IPv4, as a remote Kad contact reported it back. Two "not known" cases, both matching what the desktop panel's *IP address* row shows: `null` while Kad is not connected (the daemon sends the field only then), and `0.0.0.0` while connected but not yet told its own address by any contact. **Two distinct "unknown" sentinels, one of them a syntactically valid address**: a consumer that only checks for `null` will treat `0.0.0.0` as a real IP. Distinct from `preferences.connection.bind_address`, which is the local interface the daemon binds to. Named `public_ip` rather than `ip` because `buddy.ip` in the same payload belongs to somebody else. |
| `firewalled_tcp` | bool \| null | Whether this node is firewalled for **TCP**. The verdict is a **vote**: two distinct clients must confirm reachability by opening an incoming TCP connection carrying `OP_KAD_FWTCPCHECK_ACK` before it clears to `false`. With no verdict yet it reads **`true`**, the conservative assumption. During an IP recheck it freezes at its previous value rather than momentarily reporting a false LowID. **`null` unless `state` is `connected`** - the underlying bit outlives a disconnect, so this used to report a reachability verdict for a network the daemon was not on. Named for the transport because it is one half of a pair, not an overall verdict that `firewalled_udp` refines. |
| `firewalled_udp` | bool \| null | Whether this node is firewalled for **UDP**, measured by an entirely different mechanism: a directed test with its own state, which can also declare firewalled **by timeout** after six minutes. **`null` unless `state` is `connected`.** It previously read `false` while Kad was down, which was the absence of a measurement dressed as "UDP is open" - the asymmetry with `firewalled_tcp`'s `true` made the pair actively misleading. Both are `null` now, so there is nothing to misread. |
| `lan_mode` | bool \| null | `true` when the daemon is running Kad in LAN mode. It **forces both firewalled fields to `false`** regardless of any measurement, which is why it belongs beside them: a `false` on either flag is only meaningful once you have checked this one. `null` unless `state` is `connected`. |
| `network.user_count` / `.file_count` | int \| null | Network-wide estimates for the whole Kad network, not counts belonging to this node. The same values `GET /api/v0/status` reports under `kad.network`. **`null` unless `state` is `connected`** - they used to keep their last estimate through a disconnect, indistinguishable from a live reading. |
| `network.node_count` | int \| null | **This node's own routing table size** - how many Kad contacts it currently holds - *not* a network-wide figure like the two above. Saturates at 65535 (the daemon counts it in a `uint16`). **`null` unless `state` is `connected`**: routing contacts outlive the disconnect, so this was measured at `2` on a fully stopped Kad. |
| `indexed.sources` / `.keywords` / `.notes` | int \| null | Kad-store counters: how many entries this node is holding for the network as a DHT participant. `null` unless `state` is `connected` - they previously read `0`, which is a real count and indistinguishable from an idle but connected node. |
| `indexed.load_percent` | int \| null | A **load figure, not a count**, despite sitting beside three counts: it is the Kad store's fill level. `null` unless `state` is `connected`, on the same gate as the three counters above. |
| `buddy.state` | string \| null | LowID-buddy state for NAT-traversal clients: `no_buddy` / `connecting` / `connected` (`unknown` only if the daemon ever ships a value outside its own enum). **`null` unless `state` is `connected`**, along with `buddy.ip` / `buddy.port`; it previously read `no_buddy`, which is a real state rather than the absence of one. While connected with no buddy, `ip` / `port` are `0.0.0.0` / `0`. |

---

### IP filter

The IP-filter *settings* are ordinary preferences (`security.ipfilter_*` on [`GET`/`PATCH /api/v0/preferences`](#get-apiv0preferences)). The two endpoints here are the standalone operations behind the desktop client's Security page buttons: reloading the filter files amuled already has on disk, and downloading a new one.

Neither reports its outcome in the response — amuled answers both immediately and does the work asynchronously. What actually happened shows up only in the amule log, readable through [`GET /api/v0/logs/amule`](#get-apiv0logsamule) or the `logs` SSE channel. The lines to watch for:

| Line | Meaning |
|---|---|
| `Loading IP filters 'ipfilter.dat' and 'ipfilter_static.dat'.` | a reload started |
| `IP filter is ready` | the new filter is live |
| `Successfully updated ipfilter.dat` | the download landed and a reload follows |
| `Failed to download ipfilter.dat from <url>` | the download failed; the old filter stays live |

Those lines are gettext-translated at the daemon's locale and carry no correlation id, so treat them as human-readable output, not a machine-parseable contract.

#### `POST /api/v0/ipfilter/reload`

**Auth:** `ADMIN`

Re-reads `ipfilter.dat` and `ipfilter_static.dat` from amuled's configuration directory into the live filter — the desktop client's "Reload List" button. Use it after dropping a filter file into that directory by hand. No body.

amuled keeps the current filter live until the new one has finished loading, so this is accepted, never completed.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/ipfilter/reload"
```

**Response:** `202 Accepted` with **no body** (not an empty object): amuled returns no status string for this opcode, and the route sends nothing rather than inventing one. A client that parses the body unconditionally has to tolerate a zero-length one -- the same shape [`POST /api/v0/geoip/update`](#post-apiv0geoipupdate) and the other fire-and-forget actions answer with.

**Errors:** `400 amuled_rejected`, `503 ec_unavailable`.

#### `POST /api/v0/ipfilter/update`

**Auth:** `ADMIN`

Downloads an `ipfilter.dat` from a URL, swaps it in and reloads — the desktop client's "Update now" button.

**Body (optional):**

```json
{ "url": "http://upd.emule-security.org/ipfilter.zip" }
```

`url` must start with `http://` or `https://` when given. Omit it and the configured `security.ipfilter_update_url` is used instead; if that is empty too the request is rejected `400 bad_request` rather than accepted and silently dropped. The configured value is read from amuleapi's preferences snapshot, which trails amuled by up to one refresh tick — a `PATCH /preferences` immediately followed by a bodyless update can still send the previous URL, so pass `url` explicitly when it matters which one runs.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"url":"http://upd.emule-security.org/ipfilter.zip"}' \
  "http://$HOST/api/v0/ipfilter/update"
```

An explicit URL is **persisted** into the `security.ipfilter_update_url` preference, so a subsequent `GET /preferences` reflects it and the next startup auto-update (`security.ipfilter_auto_update`) uses it — the same side effect [`POST /api/v0/servers_update`](#post-apiv0servers_update) and [`POST /api/v0/kad/update`](#post-apiv0kadupdate) have.

**Response:** `202 Accepted`, no body. Where the request named a URL it already knows which one ran; where it omitted one, `security.ipfilter_update_url` on [`GET /preferences`](#get-apiv0preferences) is the answer, and the paragraph above is why reading it there is the honest version -- the snapshot this handler resolves from is the same one that endpoint serves.

**Errors:** `400 bad_request` (non-string / empty / non-`http(s)` `url`, or no URL available at all), `400 amuled_rejected`, `503 ec_unavailable`.

---

### GeoIP

#### `POST /api/v0/geoip/update`

**Auth:** `ADMIN`

Downloads a fresh GeoIP database now — the desktop client's "Update database now" button. No body.

Unlike the three sibling fetch endpoints this one takes **no URL**: which database to download is `geoip.source` (`"dbip"` / `"maxmind"` / `"custom"`) and, for `"custom"`, `geoip.custom_update_url`. Both stay ordinary preferences, so set them with [`PATCH /preferences`](#patch-apiv0preferences) first and then call this. Nothing about the request is persisted.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  "http://$HOST/api/v0/geoip/update"
```

**Response:** `202 Accepted` with **no body**. The download runs in the daemon after the reply: watch `geoip.download_in_progress` on [`GET /preferences`](#get-apiv0preferences) while it runs, and `geoip.last_update_status` for the outcome.

`geoip.last_update_status` is a **human-readable message, localised to the daemon's language** (`"Successfully updated dbip"`, `"Failed to download dbip from …"`) — not a status enum. Do not parse it; use `db_loaded` and `loaded_source` for the machine-readable answer to "is a database in use, and which one".

A daemon built without GeoIP (`geoip.supported` is `false`) answers `400 amuled_rejected`.

**Errors:** `400 amuled_rejected` (no GeoIP support, or the daemon refused the trigger), `503 ec_unavailable`.

---

### Logs

#### `GET /api/v0/logs/amule`

**Auth:** `GUEST`

amuled's general log buffer.

**Query parameters:** `tail=N` — return only the last N lines (default: full buffer).

```json
{
  "lines": ["2026-06-19 11:00:00: line one", "...line two"],
  "total_lines": 1024,
  "returned_lines": 2
}
```

`lines` is the array of log lines; `total_lines` is how many lines are held in the buffer and `returned_lines` how many this response carried (≤ `tail`).

**Errors:** `400 bad_request` (`tail` is not an integer, or outside `0`-`100000`), `400 amuled_rejected` (the daemon refused the read), `503 ec_unavailable`.

#### `DELETE /api/v0/logs/amule`

**Auth:** `ADMIN`

Clears the buffer.

**Response:** `204 No Content`, with no body -- a pure action with nothing to report.

**Errors:** `400 amuled_rejected` (the daemon refused the reset), `503 ec_unavailable`.

#### `GET /api/v0/logs/server_info` / `DELETE /api/v0/logs/server_info`

**Auth:** `GUEST` / `ADMIN`

The ed2k server-info log buffer. Unlike `/logs/amule`, amuled ships this one as a single accumulated text blob, so the GET returns a `text` **string** rather than a `lines` array. `?tail=N` still selects by trailing lines (it walks back N newline boundaries), but the byte counts in the response describe the result.

```json
{
  "text": "Connecting to eMule Server (203.0.113.5:4242)\nConnection established\n",
  "total_bytes": 4096,
  "returned_bytes": 68
}
```

**Errors:** `400 bad_request` (`tail` is not an integer, or outside `0`-`100000`), `400 amuled_rejected` (the daemon refused the read), `503 ec_unavailable`.

`DELETE /api/v0/logs/server_info` clears the buffer and answers `204 No Content` with no body. Its own errors are `400 amuled_rejected` (the daemon refused the reset) and `503 ec_unavailable`.

---

### Statistics

#### `GET /api/v0/stats/tree`

**Auth:** `GUEST`

**Query parameters:** `max_client_versions=N` (`0`–`255`, default `0` = unlimited) — caps how many per-software **version** rows the daemon serializes. Only the version lists are affected; the OS breakdown and the fixed skeleton nodes are always complete. A long-lived node accumulates hundreds of version rows, so a dashboard showing a top-10 should pass this rather than discard the rest client-side.

A tree mirroring amuled's "Statistics" tree (transfers, connections, clients, servers, downloads). Cached with a 1 s TTL.

The envelope is `{ "nodes": [...] }`. Each node is `{ "key": "<id>", "label_value": "<value>", "label": "<template>", "values": [...], "children": [...] }`. A leaf is a node whose `children` array is empty. `key` is optional; `label_value` is always present, `null` unless the node's `label` is itself data (see below).

`label` is the **untranslated English template** (e.g. `"Total uploaded: %s"`), and `values` are the **typed, raw** values that fill its `%s` placeholders in order — the client formats and localizes them. This keeps the response identical regardless of the amuleapi/amuled `--locale` (see [Response model](#response-model)). A container node (one that only groups children) has an empty `values` array.

`key` is a **stable, machine-readable identifier** for the node (e.g. `"upload_data"`, `"ul_dl_ratio"`, `"servers_working"`). Unlike `label`, it does not change when the label is reworded, and it is never translated — use it to locate a specific field instead of matching on the label string. The `key` is optional: it is present only for nodes the daemon assigns one to, and it is omitted entirely when absent (older daemons that predate this field emit no `key` at all). Keys are unique among the fixed skeleton nodes; the dynamic per-client-software rows share a key by kind (see below).

`label_value` is the **raw, untranslated machine value** for nodes whose `label` is itself data — the per-client-software breakdown rows, where the label is a version or OS string (`"v0.70b: %s"`, `"Linux: %s"`). It carries just that value (`"v0.70b"`, `"Linux"`) so a client reads it directly instead of parsing it out of the `label`. Present as a string on those rows and `null` otherwise, including on daemons that predate the field; the key itself is always emitted. These rows are grouped under the `client_versions` / `client_operating_system` container nodes and each row carries `key: "client_version"` / `key: "client_os"` — so the `key` tells you the kind and `label_value` gives the value.

Each value is `{ "type": "<type>", "value": <raw> }`:

| `type` | `value` JSON | meaning |
| --- | --- | --- |
| `integer` | number | plain count |
| `bytes` | number | raw bytes |
| `speed` | number | raw bytes/second |
| `time` | number | raw seconds |
| `double` | number | raw double |
| `string` | string | opaque English string (e.g. a ratio, or `"Not available"`) |

A `string` value that is a **well-known sentinel** additionally carries a `token` field with a stable, locale-independent token — currently `"never"` (a "Never" timestamp, e.g. max-connection-limit-reached) and `"not_available"` (a ratio with no data yet). The English `value` is left in place unchanged, so this is purely additive: prefer `token` when present and fall back to `value` otherwise. It is `null` on non-sentinel values and on daemons that predate the field; the key itself is always emitted.

A value may carry a nested `extra` value of the same shape — whatever the desktop prints in parentheses. It is **three different quantities** depending on the node, so format it from its own `type` rather than assuming: a **percentage of the parent** on counter nodes that show one, a **packet count** beside a byte total on the packet nodes, and the **all-time total** beside a session figure on the transfer counters.

The UL:DL ratio node (`key: "ul_dl_ratio"`) additionally carries two flat numeric fields, `ratio_session` and `ratio_total`, so clients don't have to parse its composite string value. Both are **download-per-upload** doubles (received ÷ sent bytes): `ratio_session` for the current session, `ratio_total` for all time. Each appears only when the daemon can compute it (both sides greater than zero), so both are absent when neither is available and on daemons that predate the fields. No other node type carries them.

```json
{
  "nodes": [
    {
      "key": "transfer",
      "label": "Transfers",
      "label_value": null,
      "values": [],
      "children": [
        {
          "key": "uploads",
          "label": "Uploads",
          "label_value": null,
          "values": [],
          "children": [
            {
              "key": "upload_data",
              "label": "Total uploaded: %s",
              "label_value": null,
              "values": [ { "type": "bytes", "value": 13314398208, "token": null, "extra": null } ],
              "children": []
            },
            {
              "key": "ul_dl_ratio",
              "label": "Session UL:DL Ratio (Total): %s",
              "label_value": null,
              "values": [ { "type": "string", "value": "1 : 769.34 (1 : 1125.54)", "token": null, "extra": null } ],
              "ratio_session": 769.34,
              "ratio_total": 1125.54,
              "children": []
            }
          ]
        }
      ]
    }
  ]
}
```

A per-client-software version row (note `label_value`, and an `extra` that is a percentage of the parent here), and a sentinel value (note the additive `token`):

```json
{
  "key": "client_version",
  "label_value": "v0.70b",
  "label": "v0.70b: %s",
  "values": [ { "type": "integer", "value": 42, "token": null, "extra": { "type": "double", "value": 12.5 } } ],
  "children": []
}
```

```json
{
  "label": "Max Connection Limit Reached: %s",
  "values": [ { "type": "string", "value": "Never", "token": "never", "extra": null } ],
  "children": []
}
```

**Errors:** `400 bad_request` for a `max_client_versions` outside `0`-`255` or not an integer; `503 ec_unavailable`.

#### `GET /api/v0/stats/graphs/{graph}`

**Auth:** `GUEST`

Time-series points behind the desktop Statistics graphs.

`{graph}` is one of `download_speed`, `upload_speed`, `connections`, `kad_nodes`.

**Query parameters:**

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `interval_seconds` | int, `1`–`3600` | `1` | Seconds between samples, and the same key the response echoes. Changes what amuled is asked for, so it changes the reach of the window: `interval_seconds=10` covers ten times as long at a tenth the resolution. |
| `width` | int, `0`–`1800` | full window | Tails the response to the last `N` samples. Applied after the fetch, so it does not change what is requested, so it never changes the time span each sample covers. `0` means the full window, same as omitting it. |

```json
{
  "graph": "connections",
  "unit": "count",
  "interval_seconds": 1,
  "max_points": 1800,
  "points": [
    { "at": 1781430000, "value": 42, "active_download_count": 7, "active_upload_count": 4 },
    { "at": 1781430001, "value": 44, "active_download_count": 8, "active_upload_count": 4 }
  ],
  "session": {
    "downloaded_bytes": 12400000000,
    "uploaded_bytes": 980000000,
    "kad_node_seconds": 5400000,
    "duration_seconds": 86400
  }
}
```

Each point has `at` (unix seconds) and `value`, spaced by `interval_seconds`. `unit` is `"bytes_per_second"` for the two speed graphs and `"count"` for the other two.

`max_points` is how many points this daemon can answer with before it starts repeating records; `points` is never longer than it. It is not a constant across daemons, so a client that wants the deepest window available should read it rather than assume 1800.

**`connections` only:** each point also carries `active_download_count` (clients being pulled from) and `active_upload_count` (clients being pushed to), alongside `value`, which stays the total connection count. Against an amuled that does not report them the two keys are **omitted entirely** rather than sent as `0`, so "not reported" is distinguishable from "nothing was transferring". They are all-or-nothing: either every point in a response has them or none does.

**`session`** carries this-session figures so a client doesn't need a separate roundtrip:

| Field | Meaning |
|---|---|
| `download_bytes` / `upload_bytes` | Bytes transferred this session. Granularity is 1 KiB — amuled tracks these in kibibytes and truncates before sending. |
| `kad_node_seconds` | **Not a transfer figure.** The running per-second sum of the Kad node count, i.e. node·seconds. Divide by `duration_seconds` for the session-average node count, which is its only use. |
| `duration_seconds` | Daemon uptime at the newest point. `0` if the daemon does not report it. Divide any of the three figures above by it to get the session average the desktop plots. |

**Errors:** `400 bad_request` (`interval_seconds` or `width` non-numeric or out of range), `404 not_found` (unknown graph name), `503 ec_unavailable`.

---

### Search

The search surface is admin-only because firing a global ed2k search has real network cost.

#### `GET /api/v0/search`

**Auth:** `GUEST`

Lists every search amuled currently holds — including ones started by a **different** client (another amuleapi request, the monolithic GUI, an amulegui session). Each call is a direct round trip to amuled (`EC_OP_SEARCH_LIST`), independent of the Refresher-maintained `m_state` cache, so a search this process never saw a `POST /search` for still shows up here as soon as amuled is holding it. [`GET /search/{id}/results`](#get-apiv0searchidresults) can then fetch that same `search_id` — on a cache miss it does its own one-off `EC_OP_SEARCH_LIST` check before returning `404`, so a search this endpoint just listed is never a dead end there.

```json
{
  "searches": [
    { "search_id": 42, "query": "ubuntu desktop iso", "type": "global", "state": "finished", "client_ecid": null, "started_at": 1751000000, "result_count": 182 },
    { "search_id": 43, "query": "debian",             "type": "kad",    "state": "running",  "client_ecid": null, "started_at": 1751000042, "result_count": 57  },
    { "search_id": 44, "query": "SomePeerNick",       "type": "browse", "state": "running",  "client_ecid": 621,  "result_count": 237 }
  ],
  "total": 3,
  "offset": 0,
  "limit": 3
}
```

This is a list endpoint like the others: it takes `?limit`, `?offset`, `?sort` and `?order`, and carries the same `total` / `offset` / `limit` trio. See [List pagination and sorting](#list-pagination-and-sorting); the sort keys are `search_id`, `query`, `started_at` and `result_count`.

`search_id` is the value that fills `{id}` on every search-scoped path: [`GET /search/{id}/results`](#get-apiv0searchidresults) to read its hits, [`POST /search/{id}/stop`](#post-apiv0searchidstop) to stop it, [`DELETE /search/{id}`](#delete-apiv0searchid) to free it. `type` is `"local"` | `"global"` | `"kad"` | `"browse"`. The first three are the vocabulary `POST /search`'s `type` accepts; `"browse"` is reported only, for a "View Files" listing of one client's share, which is started through the client endpoints rather than by a query. `state` is `"running"` | `"finished"` | `"idle"`, same vocabulary and meaning as `GET /search/{id}/results`'s `progress.state`.

For a `"browse"` entry, `state` and the results endpoint's `progress.percent` come from the browse's own lifecycle rather than from a query's: the request being sent is `"running"`, and the client having answered, denied the request, or disconnected mid-list is `"finished"` — a browse is never reported as `"idle"`. `percent` is the share of the client's directory list received so far, so it climbs while the listing streams in rather than jumping straight from `0` to `100`.

Browsing a client that is **already being browsed** returns the id already in flight rather than starting a second one, so this list holds one entry per browsed client, not one per request.

`query` is the daemon's name for the search. For a `"browse"` that is **the client's nickname**, not a query string — a browse has no query. `client_ecid` is the browsed client's ecid on a `"browse"` entry and `null` on an ordinary search, so a consumer can tell whose share is being listed and cross-reference [`GET /clients`](#get-apiv0clients); it is always present, `null` rather than omitted.

`started_at` is the Unix second amuleapi started the search, and it is the **only recency signal on this list**: entries arrive ordered by `search_id`, and id order is not start order, because Kad search ids carry a high-bit mask and therefore always sort above eD2k ones. Ask for `?sort=started_at&order=desc` when you need "the newest search".

It is **omitted** for any search this `amuleapi` process did not start itself — one begun by another client, by the desktop GUI, or restored from the daemon's on-disk ring after a restart. The daemon ships no timestamp of its own, so there is nothing to report for those; treat a missing `started_at` as *unknown*, not as oldest.

`result_count` is how many results the daemon currently holds for that search. It matches the `total` that [`GET /search/{id}/results`](#get-apiv0searchidresults) reports for the same id once the search has finished; while one is still running the two can differ by a fetch, because this number comes straight off the daemon's live index and `total` counts what amuleapi last pulled into its cache. It counts top-level hits only: grouped alternative filenames ride their parent's `alternate_names[]` and are not counted separately. On a `"browse"` entry it is the files received from the client so far. Like every other field on this listing it is a snapshot at request time, so a running search's count climbs between calls.

It exists so a client that adopts the whole list and fetches each search's results lazily — on first activation of a tab, rather than all at once at load — has a number to label an unopened tab with. It is **omitted**, not zeroed, when the daemon does not report it: a daemon older than this field sends nothing, and "does not report counts" has to stay distinguishable from "this search found nothing". Same rule as `started_at` above.

amuled only tracks multiple concurrent searches for clients that advertise multi-search support; `amuleapi` does, so this always reflects the full live set. `searches` is an empty array when amuled holds no searches, never an error.

**Errors:** `503 ec_unavailable`.

#### `POST /api/v0/search`

**Auth:** `ADMIN`

Kicks off a new search. amuleapi supports **several concurrent searches** — a new search does NOT stop or wipe the others. amuled allocates a globally-unique `search_id` for each start and returns it; every subsequent results/stop/more call names that id in the path. There is no implicit "current search": keep the id you are given, or re-discover it through [`GET /search`](#get-apiv0search).

**Body:**

```json
{
  "query":     "ubuntu desktop iso",
  "type":      "global",
  "file_type": "iso",
  "extension": "iso",
  "min_size_bytes":  1000000000,
  "max_size_bytes":  5000000000,
  "min_source_count": 5
}
```

Only `query` is required. `type` defaults to `"global"`; valid values are `"local"`, `"global"`, `"kad"`. `min_size_bytes`, `max_size_bytes` and `min_source_count` are integers: a fractional value is a `400` rather than being truncated, so a client that computed a size cannot half-apply a filter it thinks it set. A `"global"`/`"local"` (ed2k) search and a `"kad"` search run independently and can be in flight at the same time; starting one never disturbs the other.

**Response:** `202 Accepted`, with a `Location: /api/v0/search/{search_id}` header and the created search as the body -- the same row [`GET /search`](#get-apiv0search) lists, so it can go straight into a collection the client already keeps:

```json
{
  "search_id":  42,
  "query":       "ubuntu desktop iso",
  "type":        "global",
  "state":       "running",
  "client_ecid": null,
  "started_at":  1750412400
}
```

Keep the `search_id` to read this search's results/progress or to stop it. This is one of the two creations that answer with the resource, because `EC_OP_SEARCH_START` really does hand one back; the ones whose EC op answers success or failure and nothing more are a bare `202` with no body.

**Errors:** `400 bad_request` for any body validation (missing or non-string `query`, an unknown `type`, a malformed `file_type`, out-of-range size or availability bounds, and the rest of the body rules above); `400 amuled_rejected` when the daemon refuses the search (its own message is passed through); `502 amuled_rejected` when the daemon accepts it but returns no search_id; `503 ec_unavailable`.

#### `GET /api/v0/search/{id}/results`

**Auth:** `GUEST`

**Path:** `{id}` — the `search_id` to read, from [`POST /search`](#post-apiv0search) or [`GET /search`](#get-apiv0search). Required. A non-numeric segment or `0` is `400 bad_request`, never a fallback to some other search. An id that names no live search (never started anywhere, freed, or evicted from amuled's ring — see below) returns `404 not_found`, distinct from a known-but-empty search which returns an idle/empty envelope.

Returns one search's results buffer at the moment of the call PLUS a progress envelope so an empty `results` array isn't ambiguous between "search not started", "search in flight with no hits yet", and "search finished with zero hits".

This endpoint does NOT busy-wait — it returns whatever amuled has in its result buffer right now. A client that wants to wait for completion should poll while `progress.state == "running"`. While a search is **running** the refresher polls amuled (`EC_OP_SEARCH_RESULTS` + `EC_OP_SEARCH_PROGRESS`, addressed by `search_id`) every tick, so this GET reads straight from that snapshot and successive polls see the growing result set with no extra EC roundtrip.

A finished search keeps being refreshed. The daemon is polled for every search it holds in one incremental request per tick, so a search that has completed costs nothing to keep current and is not dropped from the poll set: a hit you download from it starts reading `status: "downloaded"` / `already_downloaded: true` without anyone having to ask.

This endpoint additionally refreshes on read, coalesced by a ~1 s TTL, which covers the sub-tick window: a client that starts a Kad notes lookup and immediately re-reads sees the flag without waiting for the next tick. Repeated polling costs at most one EC roundtrip per second, not one per request.

`POST /search` is one way a search becomes readable; an unknown `search_id` (one this session never started) triggers a one-off `EC_OP_SEARCH_LIST` check before the `404`. Once confirmed, that same request reads the search in full, so the first response already carries its whole result set rather than an empty one that fills in over the following ticks; it is then polled every tick like any other. A search another client (or the monolithic GUI) started is therefore readable here too, not just listable via [`GET /search`](#get-apiv0search).

amuled keeps a bounded ring of recent searches (20). A search evicted from that ring (because 20 newer searches were started) is reported to amuleapi as expired: its slot is retired as `finished` and reads with its `search_id` then return `404`.

```json
{
  "results": [
    {
      "hash":         "8b54a3c2...",
      "name":         "example-distribution-26.04-amd64.iso",
      "size_bytes":   3825205248,
      "sources":      { "total": 217, "complete": 142 },
      "already_downloaded": false,
      "rating":       0,
      "status":       "new",
      "file_type":    "video",
      "directory":    "",
      "media":        { "duration_seconds": 5400, "bitrate_kilobits_per_second": 1500, "codec": "h264", "artist": "", "album": "", "title": "" },
      "alternate_names": [
        { "ecid": 621, "name": "example-distribution-26.04.iso", "sources": { "total": 40, "complete": 22 }, "directory": "" },
        { "ecid": 622, "name": "example_distro_2604_amd64.iso",  "sources": { "total": 10, "complete":  3 }, "directory": "" }
      ],
      "kad_comment_lookup_running": false,
      "comments": []
    }
  ],
  "search_id":  42,
  "query": "ubuntu desktop iso",
  "progress": {
    "state":    "running",
    "type":     "kad",
    "percent":  67
  }
}
```

Each result carries `sources` as a nested `{total, complete}` object — `total` is the swarm size amuled reports and `complete` is how many of those hold the file complete. `already_downloaded` is `true` when you are currently downloading the file or already have it completed/shared; it is `false` for a fresh result and for one you have canceled/removed (a canceled result is re-downloadable, so it does not read as held). `rating` is amuled's aggregated quality rating (`0` when unrated). `status` is this result's download status on your node — `"new"` / `"downloaded"` / `"queued"` / `"canceled"` / `"queued_canceled"`. `file_type` is the file-type token derived from the filename extension (same tokens as the shared-detail [`file_type`](#get-apiv0sharedhash), e.g. `"video"` / `"audio"`; `"unknown"` when the name has no recognised extension). `media` is the audio/video [media metadata](#media-metadata) object (same shape as the file-detail endpoints), and is **`null`** for a hit that carries no metadata (most global/Kad results), matching the blank Length/Bitrate/Codec columns in the desktop search list. The key is always present. **Unlike `media` on `GET /downloads/{hash}` and `GET /shared/{hash}`, which amuled probed locally, `media` on a search result is whatever the responding server advertised.** It is not verified against the file and can contradict it — a `.pdf` reporting a runtime and a video codec is a real observed result — so treat it as a hint, not as probed metadata.

`directory` is the folder this file sits in **inside a browsed client's share** — the desktop search list's *Directories* column. It is populated only for results filed from a client's shared-file listing (see [client browse](#post-apiv0clientsecidshared_files)) and is `""` on every ordinary server/Kad hit, which never carries it. It is per-result rather than per-search: two copies of one file in different folders of the same share group under a single parent and each keeps its own folder, exactly as the desktop shows them, which is why `alternate_names[]` entries carry it too.

`search_id` and `query` identify the search these results belong to. `search_id` echoes the path so clients can key a view on the response alone; `query` is what the search was started with, so a client that adopted an id from [`GET /search`](#get-apiv0search) can label it without a second call. For a **browse**, `query` is the client's nickname rather than a query string. `query` is `""` only for a search discovered before amuled reported a name for it.

`alternate_names` is the result-grouping tree: amuled collapses hits that are the **same file** (same ed2k hash **and** size) but advertised under **different filenames** into one parent row, and `alternate_names[]` holds the alternative names. Every entry shares the parent's `hash` by construction — that is what groups them, so the `hash` is not repeated on each entry — and carries its own `name`, `sources`, `directory` and a distinct `ecid`; pass that `ecid` to [`POST /search/results/{hash}/download`](#post-apiv0searchresultshashdownload) to download the file **under that chosen filename**. `alternate_names` is always present and is an empty array for a hit seen under a single name. The top-level `results[]` contains parents only — an alternative never appears as its own top-level entry.

`kad_comment_lookup_running` and `comments` carry the file's community ratings/comments fetched from Kad. Unlike a download — whose comments come from connected sources — a search result has no sources, so `comments` is populated purely by an on-demand Kad notes lookup you start with [`POST /search/results/{hash}/comments`](#post-apiv0searchresultshashcomments). `kad_comment_lookup_running` is `true` while that lookup is in flight and flips back to `false` when it finishes; `comments` is an empty array until notes arrive, each entry shaped like a download comment (`username` / `filename` / `rating` / `comment`, with `username` the responding node's IP or `Kad user`). Both fields are always present.

The `progress` object carries the same `state` / `type` / `percent` fields as the [`search_progress`](EVENTS.md#search_progress) SSE event, so REST pollers and stream consumers interpret progress identically. (The event additionally carries a `results` count, since — unlike this response — it has no `results` array beside it.)

- `state` — `"running"` while the search is in flight, `"finished"` once amuled reports completion, `"idle"` when no search has run this session. This single field is canonical and replaces the older `complete` / `active` booleans (derive them as `complete = state == "finished"`, `active = state == "running"`).
- `type` — the originally-requested search type (`"local"` | `"global"` | `"kad"` | `"browse"`), spelled like `POST /search`'s body field and the `searches[]` row.
- `percent` — `[0, 100]`, computed by amuled for every search kind from its `EC_TAG_SEARCH_LIFECYCLE_PERCENT` tag. For **global** it is the real server-queue progress. For **Kad** — which has no measurable mid-flight progress — it is a cosmetic time-ramp off the fixed 45 s keyword-search lifetime, capped at 99 until amuled authoritatively reports completion (`EC_TAG_SEARCH_LIFECYCLE_STATE` = finished), at which point it snaps to 100. Treat the Kad value as a liveliness indicator, not an accurate estimate.

A client that wants to wait for completion polls while `state == "running"`. Because amuled now reports the lifecycle state directly (no sentinel decode), `state == "running"` unambiguously means in-flight even for Kad — there is no longer any "is `percent: 0` a stalled Kad search or no search at all?" ambiguity; check `state` instead. A Kad search that hits its result cap (`SEARCHKEYWORD_TOTAL`, 300) before the 45 s deadline finishes early — `state` flips to `finished` and `percent` jumps to 100 ahead of the ramp.

**Errors:** `400 bad_request` (bad `{id}`), `404 not_found` (no such search). No `503`: this endpoint serves the refresher's cache, and a failed EC roundtrip while adopting an unknown `{id}` surfaces as the `404` rather than as an availability error.

#### `POST /api/v0/search/{id}/stop`

**Auth:** `ADMIN`

Stops one search. No request body. Its cached results stay readable until it is freed or evicted, so a consumer viewing the search keeps seeing the set it was just looking at. Sibling searches are untouched.

**Response:** `204 No Content`. The same status [`DELETE /search/{id}`](#delete-apiv0searchid) answers with: what separates the two is that the results survive this one.

**Errors:** `400 bad_request` (bad `{id}`), `404 not_found` (no such search), `405`, `400 amuled_rejected` (the daemon refused the operation), `503 ec_unavailable`.

#### `DELETE /api/v0/search/{id}`

**Auth:** `ADMIN`

Stops the search **and frees it**: amuled drops it from its result ring and amuleapi drops its slot, so [`GET /search/{id}/results`](#get-apiv0searchidresults) for it then returns `404`. Sibling searches are untouched. Use this when a consumer is done with a search rather than just pausing it; use `POST /search/{id}/stop` to halt the in-flight query but keep the results.

Freeing a search delivers a [`search_closed`](EVENTS.md#search_closed) event to every SSE subscriber, so other clients holding a view on it find out immediately.

**Response:** `204 No Content`.

**Errors:** `400 bad_request` (bad `{id}`), `404 not_found` (no such search), `405`, `400 amuled_rejected` (the daemon refused the operation), `503 ec_unavailable`.

#### `POST /api/v0/search/{id}/more`

**Auth:** `ADMIN`

Widens a running **Kad** search — the desktop's **"More"** button. It re-asks the Kad clients already queried for a wider result frontier. No body.

Kad-only and running-only, matching what the desktop button allows rather than what the core tolerates: amuled turns a `more` on a non-Kad or finished search into a silent no-op, so both are rejected here instead of being answered with a misleading `202`.

**Response:** `202 Accepted`, no body - the reask was performed, or could not be performed *yet* but may be on a later press. The terminal case is the `409` below, so the status code is the whole answer.

**When it can no longer be widened:** `409 Conflict`, code `kad_more_exhausted`. Kad allows at most **4** reasks per search, and stops accepting them entirely once the search enters its stopping window — which begins 20 s before a keyword search's 45 s life ends, or as soon as it has collected 300 answers. In practice that means `more` only does something during roughly the first half of a search; past that, this is the answer. Disable the control for that search when you see it: nothing about that search will make it widenable again, and re-running the query is the way to get more (a fresh `POST /search` succeeds immediately and returns a new `search_id`).

A `202` does **not** promise a reask went out. A press made while no already-responded client is left un-reasked also answers `202`, because that clears the moment another client answers and retrying is the right next action. The distinction the status carries is *terminal* versus *not terminal*, which is what a UI acts on; the daemon's log line distinguishes all the individual outcomes for anyone debugging.

Note that a successful reask changes neither `progress.percent` nor `progress.state` — the Kad percent is a wall-clock ramp off the search's own start time. The response is the only signal; do not try to infer the outcome from the progress envelope.

**Older daemons.** A daemon predating this reports nothing, and amuleapi keeps answering `202` for every press rather than guessing. Absent is *unknown*, never *exhausted*.

**Errors:** `400 bad_request` (bad `{id}`, a non-Kad search, or one that has already finished), `400 amuled_rejected`, `403 forbidden` (guest), `404 not_found` (no such search), `405`, `409 kad_more_exhausted`, `503 ec_unavailable`.

#### Related-files search

There is no endpoint for the desktop's **"Search related files (eD2k, local server)"** action, and none is needed: the GUI simply composes a magic keyword and starts an ordinary local search. Do the same.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"query":"related::8b54a3c2...::0a1b2c3d...","type":"local"}' \
  "http://$HOST/api/v0/search"
```

The query is the literal prefix `related` followed by one or more `::`-separated 32-char hex MD4 hashes — one per file you want related hits for (the desktop passes every selected result). `type` must be `"local"`: the request is answered by the ed2k server you are connected to, and there is no Kad or global equivalent.

Not every server implements related search. Check the connected server's capability first rather than reading an empty result set as "nothing related": [`GET /api/v0/servers`](#get-apiv0servers) reports `related_search` among each server's flags, and the desktop refuses the action outright when it is absent.

#### `POST /api/v0/search/results/{hash}/download`

**Auth:** `ADMIN`

Promote a search result into the transfer queue. Equivalent to clicking "Download" on a desktop search row.

**Body:** `{ "category_index": 0, "ecid": 621 }` — both optional. `category_index` is the download category (default `0`), spelled like the same field on [`PATCH /downloads/{hash}`](#patch-apiv0downloadshash). `ecid` selects one grouped **alternative** by its `results[].alternate_names[].ecid`, so the file downloads **under that alternative's filename**; omit it to download the parent (the aggregated/highest-source name). Since grouped alternatives share the parent's hash, `{hash}` alone can't disambiguate them — `ecid` is how you pick a specific advertised name.

**Errors:** `400 bad_request` (malformed `{hash}`, or a non-integer `category_index` / `ecid`), `400 amuled_rejected` (the daemon refused the download), `503 ec_unavailable`.

**Response:** `202 Accepted`, no body. `hash` came from the URL and `category_index` is the value the request supplied; the download itself reports the category it landed in.

#### `GET /api/v0/search/results/{hash}/comments`

**Auth:** `GUEST`

Community ratings/comments for a single search result — the Kad notes retrieved so far plus the running flag. The same data rides each result on [`GET /search/{id}/results`](#get-apiv0searchidresults); this per-hash endpoint mirrors [`GET /downloads/{hash}/comments`](#get-apiv0downloadshashcomments) for polling one result after starting a lookup.

The route is deliberately **not** nested under a search id: amuled runs one Kad notes lookup per hash and fans the notes out to every result carrying it, so the lookup is not scoped to one search. This endpoint refreshes whichever search owns the hit before answering, which is what makes the flag below observable on a search that has already finished.

```json
{
  "total": 2,
  "kad_comment_lookup_running": false,
  "comments": [
    { "username": "203.0.113.7", "filename": "example-distribution-26.04-amd64.iso", "rating": 5, "comment": "Verified good, fast sources." },
    { "username": "Kad user",    "filename": "example-distribution-26.04-amd64.iso", "rating": 4, "comment": "Works." }
  ]
}
```

`kad_comment_lookup_running` is `true` while an on-demand Kad notes lookup (triggered by the `POST` below) is in flight; poll until it returns to `false` to know the lookup finished. `username` is the responding Kad node's IP, or `Kad user` when the note carries no IP.

**Polling is the only mechanism here — there is no event to wait for.** [`comments_updated`](EVENTS.md#comments_updated) is emitted for downloads only and never fires for a search hit, so a client that starts a lookup polls this endpoint (or the results list) while `kad_comment_lookup_running` is `true`.

**Errors:** `400 bad_request` (`{hash}` is not 32 hex characters), `404 not_found` (no live search result with that hash), `503 ec_unavailable`.

#### `POST /api/v0/search/results/{hash}/comments`

**Auth:** `ADMIN`

Trigger an on-demand Kad notes lookup for a search result you have not downloaded. This is the search-side equivalent of [`POST /downloads/{hash}/comments`](#post-apiv0downloadshashcomments): the lookup is asynchronous on amuled (up to ~45 s), and retrieved ratings/comments then appear via the `GET` above and on the result's `comments` in the search list.

**Response:** `202 Accepted`, with no body.

**Errors:** `400 bad_request` (malformed hash), `403 forbidden` (guest token — the lookup makes the daemon do network work, so it is `ADMIN`-only), `404 not_found` (no live search result with that hash), `400 amuled_rejected` (Kad down, or a search is already using this hash — retry shortly), `503 ec_unavailable`.

---

### Assets

#### `GET /flags/{code}.png`

**Auth:** `NONE` — public artwork, no per-installation data. `HEAD` is accepted too; any other method is `405 method_not_allowed`.

The country-flag image for a `country_code`. `/clients`, `/servers` and their SSE diffs carry the ISO 3166-1 alpha-2 code (see [`GET /api/v0/clients`](#get-apiv0clients)); this is where the matching artwork comes from, so a frontend does not have to ship its own flag set.

Note the path is deliberately **outside** `/api/v0/` — it is an image an `<img src>` points at, not a JSON resource, and it is versioned by the daemon build rather than by the API contract.

`{code}` must be exactly two **lowercase** ASCII letters, or the literal `unknown` for the "??" placeholder the desktop GUI falls back to when a code is empty or unrecognised. The bytes are the 16×11 famfamfam PNGs compiled into the daemon binary — the same artwork the desktop draws — so the route behaves identically whether or not `[Server]/StaticRoot` is set, and never touches the file system.

```sh
curl -s http://$HOST/flags/de.png -o de.png
curl -s http://$HOST/flags/unknown.png -o unknown.png
```

**Response:** `200 OK`, `Content-Type: image/png`, `Cache-Control: public, max-age=86400`.

Responses carry an `ETag` and honour `If-None-Match` with `304 Not Modified` like every other `GET` (see [ETag and conditional GET](#etag-and-conditional-get)). The `max-age` is what keeps a client list full of `<img>` tags from issuing one conditional request per country on every reload; the artwork can only change with a new daemon build, and a day bounds how long an upgraded daemon keeps serving the old image.

The response is never `Content-Encoding: gzip` — a PNG is already entropy-coded, so the server skips compression for it.

**Errors:** `404 not_found` for anything that is neither two lowercase letters nor `unknown` (uppercase, wrong length, digits, another extension), and for a well-formed code the famfamfam set has no artwork for — it covers 248 of the assignable alpha-2 codes, so a resolvable country can legitimately have no flag. Render the code as text, or fall back to `unknown.png`, in that case. `400 bad_request` for paths carrying traversal tokens, per [Path validation](#path-validation).

Clients whose country could not be resolved — GeoIP disabled, unsupported by the build, or a private/unmatched address — come back with `country_code: null`. There is no per-country image to request in that case; draw nothing, or `unknown.png` for parity with the desktop list.

---

### Chat

Conversations with clients, backed by the chat session store in `amuled`. The store is shared: a message sent from the desktop GUI, from amulegui or through this API lands in the same transcript, and every client sees the same conversation.

A conversation is keyed on `{address}` = `"<ip>:<port>"` (for example `203.0.113.42:4662`). That is the readable form of the internal id the EC wire already uses, it is stable across client reconnects — unlike an ECID — and it needs no identifier of its own. A `{address}` that is not four dotted octets plus a port is a `400`.

The store is **in memory**: an `amuled` restart empties every conversation, exactly as the desktop's own transcript dies with its notebook tab. Retention is bounded at 200 messages per conversation and 50 conversations, evicting the least recently active first.

Message `id` is monotonic per `amuled` process and never reused, which makes it a safe polling cursor: `since_message_id` never returns a duplicate and never skips a message. It resets when the daemon restarts, which also empties the store.

Every endpoint here answers `503 ec_unsupported` when the connected `amuled` does not serve chat.

#### `GET /api/v0/chats`

**Auth:** `GUEST`

```sh
curl -s -H "Authorization: Bearer $TOKEN" "http://$HOST/api/v0/chats"
```

```json
{
  "chats": [
    {
      "address":            "203.0.113.42:4662",
      "ip":              "203.0.113.42",
      "port":            4662,
      "name":            "alice",
      "client_ecid":     4382,
      "friend_ecid":     12,
      "online":          true,
      "message_count":   14,
      "last_message_id":     91,
      "last_message_at": 1786652714,
      "last_message":    { "id": 91, "direction": "in", "text": "thanks!", "sent_at": 1786652714 }
    }
  ],
  "total": 1, "offset": 0, "limit": 1
}
```

`name` falls back to `"IP: <ip> Port: <port>"` when the core has no nickname for the client, matching what the desktop shows; the same string appears in the SSE payload. `client_ecid` is `null` when the client is offline and `friend_ecid` is `null` when the client is not a friend — join either against [`GET /clients`](#get-apiv0clients) and [`GET /friends`](#get-apiv0friends). `online` says whether a connection to the peer is actually up, which is not the same as `client_ecid` being non-null: the daemon holds a client object from the first contact attempt, so a conversation opened against an unreachable address has an ecid and is not online. `null` means the daemon does not report peer connectivity.

`last_message` is `null` for a conversation that holds none; the key is always present. The full transcript is deliberately **not** on the list: 50 conversations at 200 messages each would be 10 000 objects per read. Use the messages endpoint below.

Served from the refresher snapshot — no EC roundtrip per request. Standard [list envelope](#list-pagination-and-sorting); sortable on `last_message_at` and `name`.

**Errors:** `503 ec_unsupported`, `503 ec_unavailable`.

#### `GET /api/v0/chats/{address}/messages`

**Auth:** `GUEST`

**Query:** `since_message_id=N` (only messages with `id > N`), `tail=N` (the last N of that window, max `100000`). This selects a tail rather than a page, which is why it is not called `limit`: on the list endpoints `limit` is a window paired with `offset`, and one word meaning two things is a rule a client has to learn twice.

```json
{
  "address": "203.0.113.42:4662",
  "messages": [
    { "id": 90, "direction": "out", "text": "hi",      "sent_at": 1786652700 },
    { "id": 91, "direction": "in",  "text": "thanks!", "sent_at": 1786652714 }
  ],
  "total": 14,
  "last_message_id": 91
}
```

`direction` is `"in"` (from the client) or `"out"` (sent by us — from **any** client: this API, amulegui, or the desktop GUI). `sent_at` is stamped by the core when the message was received or sent, and is `null` only in the reply to a send: `EC_OP_CHAT_SEND` answers with the message id and no timestamp, and the core is the only thing entitled to stamp one. That reply is otherwise the same object shape this endpoint returns, emitted by the same writer, so a client can append it optimistically and reconcile on the next read. `total` counts everything the store holds for this conversation, not the returned window.

**Errors:** `404 not_found` (no such conversation), `400 bad_request` (malformed `{address}` or query), `503 ec_unsupported`, `503 ec_unavailable`.

#### `POST /api/v0/chats/{address}/messages`

**Auth:** `ADMIN`

**Body:** `{ "text": "hello" }` — non-empty, at most 1024 bytes.

```sh
curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"text":"hello"}' "http://$HOST/api/v0/chats/203.0.113.42:4662/messages"
```

```json
{ "address": "203.0.113.42:4662",
  "message": { "id": 92, "direction": "out", "text": "hello", "sent_at": null } }
```

The created message stays in the body because the store-assigned `id` is only readable here. It is the same object shape [`GET /chats/{address}/messages`](#get-apiv0chatsaddressmessages) returns, emitted by the same writer, with one difference: `sent_at` is **`null`**, because `EC_OP_CHAT_SEND` answers with the message id and no timestamp. Read the timestamp back from [`GET /chats`](#get-apiv0chats) (as `last_message`), from the per-conversation messages endpoint, or from the `chat_message` SSE event.

The core creates the conversation if it does not exist, so this doubles as "start a chat with this address" — an unknown `{address}` is not a `404` here.

Returns `202 Accepted`, not `200`: the core acknowledges that it queued the message on the client connection, not that the client received it. An unreachable client is not an error — the desktop behaves the same, optimistically showing `*** Connecting to Client ***`.

**Errors:** `400 bad_request`, `404 not_found` (no client at that address to send to), `503 ec_unsupported`, `503 ec_unavailable`.

#### `POST /api/v0/friends/{ecid}/messages`

**Auth:** `ADMIN`

Message a friend by friend ECID. This is the form that reaches an **offline** friend: the daemon resolves the ECID to the friend's stored address, so no live connection is needed.

**Body:** `{ "text": "hello" }`

**Response:** `202 Accepted` → `{ "address": "203.0.113.42:4662", "message": { … } }`, so the caller learns the conversation key to read back.

**Errors:** `404 not_found` (no friend with that ECID), plus the set above.

#### `POST /api/v0/clients/{ecid}/messages`

**Auth:** `ADMIN`

The client-addressed form, for a caller holding a client row that should not have to compose an `ip:port` key. Same body and response as above.

**Errors:** `404 not_found` (no live client with that ECID), plus the set above.

#### `DELETE /api/v0/chats/{address}`

**Auth:** `ADMIN`

Closes the conversation: drops it from the core store and resets the client's chat state.

**Response:** `204 No Content`.

Closing is **global**, the same way closing a search tab frees the search for every client. A connected amulegui drops its tab on the next poll, and the desktop GUI closes its own; no client is left showing a conversation the core no longer has.

**Errors:** `404 not_found`, `400 bad_request`, `503 ec_unsupported`, `503 ec_unavailable`.

## Error code catalog

Every error code emitted by `/api/v0/*`, sorted by what triggered it. Two codes are emitted with **two different statuses** depending on the cause, so a client that switches on `code` alone must also look at the status for those.

| Code | Status | Meaning |
|------|--------|---------|
| `bad_request` | 400 | Body, query, or path-segment validation failed. Body parse depth-cap rejects also surface here. |
| `amuled_rejected` | 400, 502 | amuled rejected the EC operation and the message carries its reason verbatim (400); or amuled answered but its reply was unusable, e.g. no `search_id` came back from a search or a browse (502). |
| `request_timeout` | 408 | The request was not completed within the 10 s read timeout. |
| `unauthorized` | 401 | Missing token, bad signature, expired, revoked, or `iat` invariants failed. |
| `invalid_credentials` | 401, 403 | `/auth/login` password didn't match any role (401); or `PATCH /auth/passwords` was given a `current_password` that does not match (403). |
| `forbidden` | 403 | Authenticated as `guest` but the endpoint requires `admin`. |
| `not_found` | 404 | Resource doesn't exist (unknown hash, ECID, graph name, or no such endpoint). |
| `not_readable` | 403 | Per-item code from the [`/share_directories`](#post-apiv0share_directories) bulk envelope: amuled cannot read that path. Only ever appears inside a `results[].error`, never as a whole-response error. |
| `method_not_allowed` | 405 | Wrong HTTP verb for the route. The response carries an `Allow` header listing the methods this resource does support. |
| `option_not_supported` | 409 | `PATCH /preferences` set an option the connected daemon was built without. |
| `not_a4af_source` | 409 | The client named on a `POST /downloads/{hash}/a4af` request is not an A4AF source of that download. |
| `partfile_unsupported` | 409 | Verify Local Data, or a content download, requested on a file that is still an incomplete partfile. |
| `not_shared` | 409 | A comment or rating was posted against a file that is not shared. |
| `not_completed` | 409 | `clear_completed` named a `hash` that is not a completed download. |
| `download_completed` | 409 | A `DELETE /downloads` matched a completed download; use `clear_completed` for those. |
| `kad_more_exhausted` | 409 | `POST /search/{id}/more` on a Kad search that can no longer be widened — its 4-reask budget is spent, or it has entered the stopping window Kad begins 20 s before a keyword search ends. Terminal for that search; re-run the query for more. |
| `version_check_unavailable` | 409 | `POST /version/check` cannot run - the daemon has no update-check capability. |
| `payload_too_large` | 413 | Request body exceeds the 1 MiB limit. The connection closes after the response. |
| `range_not_satisfiable` | 416 | A `Range` on [`GET /shared/{hash}/content`](#get-apiv0sharedhashcontent) starts at or past EOF. `Content-Range: bytes */<size>` accompanies the response. |
| `rate_limited` | 429 | The per-IP auth-failure bucket is full, so the address is locked out of every authenticated route. `Retry-After: <seconds>` accompanies it. A client should treat this as the session ending. |
| `version_check_throttled` | 429 | [`POST /version/check`](#post-apiv0versioncheck) arrived inside the daemon's 60 s cooldown. Affects that route only, so a client must not read it as a lost session. |
| `headers_too_large` | 431 | Request headers exceed the 16 KiB limit. The connection closes after the response. |
| `internal_error` | 500 | A server-side failure: a handler failed internally (hash decode, serialization) or threw and was caught by the HTTP layer. The body is generic; details land in the daemon's stderr. |
| `amuled_response_invalid` | 502 | amuled returned an EC payload this endpoint could not decode. |
| `ec_unavailable` | 503 | EC connection not ready yet (cold start, transient amuled restart). |
| `ec_unsupported` | 503 | The connected amuled is too old to serve this route — the chat endpoints and `/known_clients`. |
| `login_disabled` | 503 | `/auth/login` reached but no admin AND no guest password configured. |
| `too_many_streams` | 503 | Too many concurrent SSE streams. `Retry-After` accompanies the response. |
| `file_responses_exhausted` | 503 | Too many concurrent file responses (`[Streaming]/MaxConcurrentFileResponses`, default 6) on [`GET /shared/{hash}/content`](#get-apiv0sharedhashcontent). `Retry-After` accompanies the response. |
| `path_unavailable` | 503 | The shared file's on-disk directory has not yet arrived over EC. Transient; `Retry-After: 5` accompanies the response. |
| `ec_content_unreachable` | 503 | The shared file is not present on the filesystem running amuleapi — a deployment where amuleapi talks to a **remote** amuled. |
| `ec_content_mismatch` | 503 | The file at the resolved path disagrees in size with the shared file the hash names — the same remote-amuled split, resolving to an unrelated local file of the same name. |

`message` is human-readable and may change between releases. Pin on `code`.

## Backward compatibility

**`/api/v0/` is not frozen, and offers no compatibility guarantee.** It is the surface under construction: it has no consumers outside this repository, so a field whose name is wrong is renamed in place rather than aliased, deprecated or carried into the next version. A client written against v0 must expect to be updated alongside it, and the bundled Web UI is exactly that client.

This is a deliberate position, not an oversight. Freezing a surface still being corrected would mean shipping v1 with the mistakes intact and a compatibility shim for each one.

What that means in practice, until the freeze:

- **Fields get renamed**, and the old name stops existing the same commit the new one appears. There is no alias window.
- **Response shapes change** where the current one misleads — a key that reports a count but is named like a percentage, a write field that accepts values the matching read field never returns.
- **Enum values change** where the token is wrong for what it describes.
- **New endpoints, parameters and fields** may be added at any time. Clients **MUST ignore unknown fields**, and must not depend on field order or on a field's absence.

**`/api/v1/` is where the guarantee starts.** It is cut once the naming rules below hold across the whole surface and the result has been exercised end to end. From that point the additive-only discipline applies: anything that could break a conformant client is deferred to the next version rather than applied in place.

`POST /api/v0/auth/login`'s default body shape (no token unless `?include_token=true`) IS a change from the very first amuleapi cuts; the legacy "token always in body" behaviour is reachable only via the opt-in. This is documented and committed.
