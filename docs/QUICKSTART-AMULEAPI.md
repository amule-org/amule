# amuleapi — quick start

amuleapi is a standalone HTTP daemon that serves a versioned JSON REST API
and a long-lived Server-Sent Events stream backed by amuled. It connects
to amuled as an EC client (same protocol amuleweb and amulecmd use) and
exposes its own HTTP surface on a separate port. amuleapi is the first
shipping REST API for aMule — there is no prior on-the-wire surface to
migrate from.

> aMule's older web frontend, **amuleweb**, is **deprecated** — it may be removed in aMule 3.2 or later (it is not being removed yet). amuleapi is its intended replacement.

For the endpoint list see the [What ships](#what-ships) section below. Full per-endpoint contracts (methods, query params, request bodies, response shapes, error codes) live in [`docs/api/REFERENCE.md`](api/REFERENCE.md); the SSE event catalog and Last-Event-ID reconnect semantics live in [`docs/api/EVENTS.md`](api/EVENTS.md). The source of truth for routing is [`src/webapi/Api.cpp`](../src/webapi/Api.cpp).

## Requirements

- A running `amuled` (or a monolithic `aMule` with EC enabled) that
  amuleapi can connect to over the EC protocol.
- The EC password from `amule.conf[ExternalConnect]/Password` (set via
  `amuled --ec-config` if you've never run it).

## First-run setup

amuleapi keeps its config in the same per-platform aMule data directory
that `amuled` uses.

> **Cohabitation with amuled.** This is the same directory amuled
> keeps `amule.conf` and `*.met` in — intentionally so. amuleapi's
> three files (`amuleapi.conf`, `amuleapi-jwt-secret`,
> `amuleapi-passwords`) sit alongside amuled's without colliding,
> and operators reading both sets of configs together don't have
> to context-switch directories. amuleapi never touches amuled's
> files; amuled never touches amuleapi's.

The default location:

| Platform | Default config dir                                  |
| -------- | --------------------------------------------------- |
| Linux    | `~/.aMule/`                                         |
| macOS    | `~/Library/Application Support/aMule/`              |
| Windows  | `%APPDATA%\aMule\`                                  |

Override with `amuleapi --config-dir=/path/to/dir`.

The directory holds three amuleapi-specific files, all written with mode
`0600`:

| File                      | Purpose                                                                                                  |
| ------------------------- | -------------------------------------------------------------------------------------------------------- |
| `amuleapi.conf`           | INI-style runtime config (HTTP bind/port/CORS, outbound EC connection to amuled, login rate-limit knobs, SSE event-bus ring size). Full reference below. |
| `amuleapi-jwt-secret`     | 32-byte HMAC signing key for issued tokens. Auto-generated on first launch if absent.                    |
| `amuleapi-passwords`      | MD5-hashed admin and guest passwords. Plaintext is never persisted.                                      |

Set passwords via the dedicated CLI flags. Each invocation writes the
file and exits — the HTTP server is NOT brought up, no EC connection
is attempted, and the exit code reflects success / failure (so
`amuleapi --set-admin-pass=... && systemctl restart amuleapi` actually
short-circuits if the write fails):

```sh
amuleapi --set-admin-pass=mySecret123
amuleapi --set-guest-pass=readOnlyPass
```

An empty password row means "this role is disabled" and
`POST /api/v0/auth/login` returns `login_disabled` for that role.

## Running

```sh
amuleapi --host=127.0.0.1 --port=4712 --password=$EC_PASSWORD
```

> **Two ports.** `--port=4712` is the EC port amuleapi USES to talk
> to amuled (i.e. it's a client of amuled on 4712). amuleapi's OWN
> HTTP listener is on `amuleapi.conf[Server]/Port` (default 4713)
> — that's the port REST clients hit. The example above starts a
> daemon that consumes 4712 (outbound to amuled) and serves 4713
> (inbound from REST clients).

- `--host` / `--port` / `--password` specify the EC connection to
  `amuled` (default port `4712`).
- HTTP serves on `amuleapi.conf[Server]/Port` (default `4713`).
- amuleweb can run concurrently on its own port (default `4711`); the
  two daemons talk to amuled independently as separate EC clients.

aMule does not ship init-system units (systemd, launchd, Windows
service) for any of its daemons. If you want one, write a downstream
unit that wraps the command above.

### Auto-starting from aMule

aMule can launch amuleapi for you when it starts, the same way it can
launch amuleweb. Enable **Run amuleapi (REST API) on startup** under
*Preferences → Remote Controls*, or set `/AmuleApi/Enabled=1` in
`amule.conf`. aMule then spawns:

```sh
amuleapi --amule-config-file=<amule.conf> --config-dir=<amule data dir> --http-port=<AmuleApi/HttpPort>
```

`--amule-config-file` points amuleapi at aMule's own `amule.conf` so it
reads the EC host, port and (hashed) password straight from there — no
plaintext EC password on the command line. The auto-started instance
binds `127.0.0.1` only, so it needs no admin password; set one in
`amuleapi.conf` (or via `--set-admin-pass`) if you later expose it to
other hosts. amuleapi is stopped when aMule exits.

The HTTP port is configurable in the same preferences panel (or
`/AmuleApi/HttpPort`, default `4713`). When aMule runs as `amuled`, the
amulegui remote client can toggle the setting over EC.

## Verifying

```sh
# Public — no auth.
curl -s http://127.0.0.1:4713/api/v0/version

# Login → token.
# `?type=bearer` opts into the SDK-client response shape: the JWT
# lands in the JSON body so a shell script can extract it. Browser
# clients call /auth/login WITHOUT ?type=bearer and authenticate via
# the HttpOnly session cookie set on the response — that's the
# default to keep the token out of any XSS-readable surface.
TOKEN=$(curl -s -X POST "http://127.0.0.1:4713/api/v0/auth/login?type=bearer" \
    -H 'Content-Type: application/json' \
    -d '{"password":"mySecret123"}' | jq -r .token)

# Authenticated GETs.
curl -s -H "Authorization: Bearer $TOKEN" \
    http://127.0.0.1:4713/api/v0/status

# Live event stream — open in a separate terminal and trigger
# mutations elsewhere to watch events flow.
curl -s -N -H "Authorization: Bearer $TOKEN" \
    http://127.0.0.1:4713/api/v0/events
```

## `amuleapi.conf` reference

INI-style file written with mode `0600`. The defaults file is created on first launch if absent — edits roundtrip through `wxFileConfig`, so quotes and comments are preserved across daemon restarts. The full surface:

```ini
[Server]
BindAddress=127.0.0.1
Port=4713
AllowCORS=0
CorsOriginAllowlist=
StaticRoot=

[EC]
Host=127.0.0.1
Port=4712
Password=

[Auth]
LoginFailureWindowSeconds=60
LoginFailureThreshold=5
LoginLockoutSeconds=300

[Streaming]
EventBusRingCapacity=16384
```

### `[Server]` — HTTP listener

| Key | Default | Meaning |
| --- | --- | --- |
| `BindAddress` | `127.0.0.1` | Interface the HTTP listener binds to. Non-loopback binds are rejected at startup unless at least one of admin/guest passwords is set (the "publicly listening with no password" footgun gate in `App.cpp`). Overridable with `--bind=…` on the CLI. |
| `Port` | `4713` | TCP port for inbound REST traffic. Distinct from amuled's EC port (`[EC]/Port`, default 4712). Overridable with `--http-port=…`. |
| `AllowCORS` | `0` | `1` enables CORS headers (`Access-Control-Allow-Origin`, `Access-Control-Allow-Credentials: true`, `Vary: Origin`, preflight OPTIONS). Required for browser clients hosted on a different origin. See §CORS below. |
| `CorsOriginAllowlist` | *(empty)* | Comma-separated list of origins that may set credentialed CORS requests. Empty + `AllowCORS=1` echoes the caller's `Origin` verbatim (wildcard-equivalent that remains cookie-compatible). |
| `StaticRoot` | *(empty)* | Absolute filesystem path of a bundled web frontend. Empty (default) auto-discovers the bundled placeholder via the install-path chain (`make install` target on Linux/Windows, `aMule.app/Contents/Resources/amuleapi-static` on macOS) — same pattern amuleweb uses for templates, see [`WebInterface.cpp:146`](../src/webserver/src/WebInterface.cpp#L146). If no install is found, the daemon stays API-only and non-`/api/` paths return `404`. A non-empty `StaticRoot` overrides discovery and serves `GET`/`HEAD` requests outside `/api/` from that directory, with `index.html` SPA fallback for extension-less misses. Reads are containment-checked (symlinks pointing outside the root are rejected on POSIX; lexical `..`-rejection on Windows where symlinks require elevation), capped at 16 MiB per asset, and emit a mtime-size `ETag` so subsequent loads short-circuit to `304` via `If-None-Match`. |

### `[EC]` — outbound connection to amuled

| Key | Default | Meaning |
| --- | --- | --- |
| `Host` | `127.0.0.1` | Hostname or IP of the running amuled daemon. amuleapi is a long-lived EC client; CLI `--host=…` overrides. |
| `Port` | `4712` | amuled's EC listener port (matches amuled's `[ExternalConn]/ECPort`). CLI `--port=…` overrides. |
| `Password` | *(empty)* | Plaintext EC password matching amuled's `[ExternalConn]/ECPassword`. Stored cleartext because the base class wants a hashable plaintext — the `0600` file mode matches `amuleapi-jwt-secret` and `amuleapi-passwords`. CLI `--password=…` overrides. |

### `[Auth]` — login rate limiter

Drives the `/auth/login` per-IP throttle (`CRateLimiter` in `Auth.cpp`). Failures inside the sliding window count toward the threshold; tripping it locks the offending IP out for `LoginLockoutSeconds`. Successful logins reset the bucket immediately.

| Key | Default | Meaning |
| --- | --- | --- |
| `LoginFailureWindowSeconds` | `60` | Sliding window in seconds. Failures older than this fall off the count. |
| `LoginFailureThreshold` | `5` | Failures within the window before the IP is locked out. |
| `LoginLockoutSeconds` | `300` | Duration of the IP lockout once tripped. While locked, `/auth/login` returns `429 rate_limited` with a `Retry-After` header. |

### `[Streaming]` — SSE event bus

| Key | Default | Meaning |
| --- | --- | --- |
| `EventBusRingCapacity` | `16384` | Number of events the in-memory SSE bus retains for `Last-Event-ID` replay. Sized to absorb a cold-start tick on a busy node (5 K downloads + 5 K shared can publish ~10 K `*_added` events in a single tick). Worst-case memory ≈ capacity × ~1 KB JSON payload. Values below the bus's compile-time floor (16) are clamped up. Raise this on operator-heavy nodes where reconnecting clients are hitting `resync` events from natural traffic; lower it (e.g. `32`) only for the smoke-test gap-path scenario. |

CLI `--bind`, `--http-port`, `--host`, `--port`, `--password`, and `--config-dir` override the matching keys at runtime without rewriting the file.

## CORS

By default amuleapi serves no CORS headers (same-origin only). To allow
cross-origin browser clients, set in `amuleapi.conf`:

```ini
[Server]
AllowCORS=1
CorsOriginAllowlist=https://your-app.example.com,https://staging.example.com
```

Leave `CorsOriginAllowlist` empty to echo any caller's `Origin` header
(wildcard-equivalent that stays cookie-compatible).

> **CORS note.** The empty-allowlist form is *not* literally
> `Access-Control-Allow-Origin: *`. amuleapi echoes the caller's
> exact `Origin` value, which is the only shape browsers accept
> together with `Access-Control-Allow-Credentials: true` (RFC 6454
> + Fetch spec). A literal `*` would refuse cookie auth on cross-
> origin requests — which is what every browser session relies on.

## What ships

- `/api/v0/auth/login` / `logout` / `session` — JWT and session-cookie auth
- `/api/v0/version`, `/status`, `/preferences`
- `/api/v0/downloads`, `/shared`, `/servers`, `/kad`,
  `/clients` (the per-peer view, with optional
  `?filter=uploads|downloads|active` for the legacy "Uploads" page
  subset), `/categories`, `/logs/{amule,serverinfo}`,
  `/stats/{tree,graphs/{graph}}`, `/search`, `/search/results`
- POST / PATCH / DELETE mutations on each resource (admin role)
- ETag-on-GET conditional caching (304 Not Modified on `If-None-Match`)
- `/api/v0/events` — long-lived Server-Sent Events stream with
  `Last-Event-ID` replay and typed `resync` events for cache invalidation
- Every runtime tunable lives in `amuleapi.conf`; see the §`amuleapi.conf` reference above for sections, keys, and defaults.

## Notes on a few responses

- **`POST /api/v0/downloads` partial success.** The endpoint accepts
  a single `ed2k_link` or an array of `links`. When some links land
  cleanly and others fail (already on queue, malformed magnet,
  category out of range, or EC disconnect mid-batch) the response is
  `207 Multi-Status` with `ok: false` and four parallel arrays —
  `accepted_links`, `failed_links`, `disconnected_links`, plus
  counters and a `first_error`. `207` is borrowed from WebDAV (RFC
  4918 §13) for "the request was answered in pieces"; clients should
  treat it as success-with-details, not as a 4xx. `503` is reserved
  for "every link blocked by an EC disconnect" — nothing landed and
  the caller can retry once `GET /status` reports `ec_connected:
  true`.

## Security notes

- The admin role grants the holder full control of the daemon's
  network surface — that includes `POST /api/v0/servers/update
  {"servers_url": "..."}`, which makes amuled fetch the supplied URL
  to refresh the server list. This is the same behaviour amuled has
  exposed via the desktop GUI and amuleweb for years, but it widens
  what an admin token *grants* — anyone who steals one can ask
  amuled to perform an HTTP GET against arbitrary network-reachable
  URLs (a classic SSRF surface) and bring the response back into
  amuled's process. The `http://` / `https://` pre-check in the API
  is hygienic input validation, not a security boundary; protect
  the admin password and the JWT signing secret accordingly.
- The default `BindAddress=127.0.0.1` is load-bearing. The HTTP
  server spawns one OS thread per Server-Sent Events subscriber, so
  binding amuleapi to a non-loopback interface exposes the
  thread-per-connection model to unauthenticated peers. If you need
  remote access, put a reverse proxy in front and keep the bind on
  loopback.
