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
> own files (`amuleapi.conf`, `amuleapi-jwt-secret`,
> `amuleapi-passwords`, and by default the `amuleapi.log` log file)
> sit alongside amuled's without colliding, and operators reading
> both sets of configs together don't have to context-switch
> directories. amuleapi never *writes* amuled's files; the one point
> of contact is read-only — launched with `--amule-config-file` (as
> aMule's [auto-start](#auto-starting-from-amule) does), it reads
> connection and admin-password settings out of `amule.conf`. amuled
> never touches amuleapi's files.

The default location:

| Platform | Default config dir                                  |
| -------- | --------------------------------------------------- |
| Linux    | `~/.aMule/`                                         |
| macOS    | `~/Library/Application Support/aMule/`              |
| Windows  | `%APPDATA%\aMule\`                                  |

Override with `amuleapi --config-dir=/path/to/dir`.

The directory holds three amuleapi-specific config/secret files, each
written with mode `0600`:

| File                      | Purpose                                                                                                  |
| ------------------------- | -------------------------------------------------------------------------------------------------------- |
| `amuleapi.conf`           | INI-style runtime config (HTTP bind/port/CORS, outbound EC connection to amuled, login rate-limit knobs, SSE event-bus ring size). Full reference below. |
| `amuleapi-jwt-secret`     | 32-byte HMAC signing key for issued tokens. Auto-generated on first launch if absent.                    |
| `amuleapi-passwords`      | MD5-hashed admin and guest passwords. Plaintext is never persisted.                                      |

By default amuleapi also writes an `amuleapi.log` file here (a copy of
its console output); see [Logging](#logging) to relocate or disable it.

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

> **Keep the EC password out of the command line.** `--password` is
> only an override — omit it and amuleapi reads the EC password from
> `amuleapi.conf[EC]/Password` (a `0600` file), which is the way to
> avoid exposing the secret in the process arguments. Passing
> `--password=…` puts the value in `argv`, where it is visible to any
> local user via `ps` / `/proc/<pid>/cmdline`; `--password=$EC_PASSWORD`
> does not help, since the shell expands the value into `argv` before
> exec. There is no environment-variable or stdin path for the EC
> password — the config file is the non-`argv` option. (In the
> auto-start flow, `--amule-config-file` reads the already-hashed EC
> password from `amule.conf`, also without touching `argv`.)

aMule does not ship init-system units (systemd, launchd, Windows
service) for any of its daemons. If you want one, write a downstream
unit that wraps the command above.

### Logging

By default amuleapi tees a copy of everything it prints to the console
into `amuleapi.log` in its config dir, installed as early as possible so
config-load errors, EC warnings, and a crash backtrace are all captured.
The output is low-volume (startup plus warnings/errors — there is no
per-request access log), and the file is rotated at 10 MiB as a runaway
guard.

- `--log-file=/path/to/amuleapi.log` — write the log somewhere other
  than the default `<config-dir>/amuleapi.log`.
- `--no-log-file` — don't write a log file; print to the console only.

The daemon prints `amuleapi: logging to <path>` on startup so you can see
where it landed.

### Auto-starting from aMule

aMule can launch amuleapi for you when it starts, the same way it can
launch amuleweb. Everything is configured under *Preferences → Remote
Controls* (**aMule API server parameters**): tick **Run amuleapi (REST
API) on startup**, then set the **listening interface**, **HTTP port**,
and **admin password**. These map to `/AmuleApi/Enabled`,
`/AmuleApi/BindAddress`, `/AmuleApi/HttpPort` and `/AmuleApi/Password`
(MD5-hashed) in `amule.conf`, and are also editable from a remote
amulegui over EC. aMule then spawns:

```sh
amuleapi --amule-config-file=<amule.conf> --config-dir=<amule data dir> --bind=<AmuleApi/BindAddress> --http-port=<AmuleApi/HttpPort>
```

`--amule-config-file` points amuleapi at aMule's own `amule.conf` so it
reads the EC host/port/(hashed) password **and** the admin password hash
(`/AmuleApi/Password`) straight from there — exactly as amuleapi's sibling
amuleweb reads `/WebServer/Password`. Nothing sensitive is passed on the
command line, and the admin hash is applied in memory only: a standalone
`amuleapi-passwords` file is never touched. Because the admin password is
supplied, you can bind a non-loopback interface directly from the prefs
panel to expose the API to other hosts. Changing any of these settings
prompts you to restart aMule, which relaunches amuleapi with the new
parameters. amuleapi is stopped when aMule exits.

When amuleapi is started **standalone** (no `--amule-config-file`), nothing
here applies: it reads its bind/port from `amuleapi.conf` and its admin
password from the `amuleapi-passwords` file (set via `--set-admin-pass`),
exactly as before.

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

The daemon serves a versioned REST surface under `/api/v0/`. This is the
map of what's there; the full per-endpoint contracts — methods, query
params, request/response bodies, error codes — live in
[`docs/api/REFERENCE.md`](api/REFERENCE.md), which stays authoritative and
current.

- **Auth** — `auth/login`, `auth/logout`, `auth/session` (JWT and
  session-cookie).
- **System** — `version`, `version/check`, `status`, `preferences`.
- **Downloads** — the transfer queue: list and per-file detail; add,
  pause/resume, cancel, `clear_completed`; per-file comments,
  source-reported filenames, and A4AF (alternate sources) listing and
  swap.
- **Shared files** — list and detail, `reload`, `verify` (re-hash local
  data), and the shared-directory roots (`shared/directories`, with
  GET/PUT/POST/DELETE).
- **Clients (peers)** — the per-peer view (optional
  `?filter=uploads|downloads|active` for the legacy "Uploads" subset),
  per-client detail, and browsing a peer's shared files ("View Files").
- **Servers** — the ed2k server list: add, connect, remove, and
  `servers/update` (refresh the list from a URL).
- **Network control** — `networks/connect` / `disconnect`,
  `kad/bootstrap`, and the `kad` status view.
- **Categories** — list plus create / edit / delete.
- **Search** — `search` (start), `search/results`, `search/stop`,
  download a result, and per-result comments/ratings.
- **Logs & stats** — `logs/{amule,serverinfo}`, `stats/tree`,
  `stats/graphs/{graph}`.

Conventions that apply across the surface (all detailed in REFERENCE):

- **List windowing.** `downloads`, `clients`, `shared`, `servers`, and
  `search/results` take `limit` / `offset` / `sort` / `order` and return
  `total` / `offset` / `limit` alongside the array. Omitting them all
  yields the full set.
- **Bulk mutations** report one entry per input item under a unified
  `results` array — `200`/`202` when every item succeeded, `207
  Multi-Status` for a mix (inspect each `results[].ok`), `503` when the
  whole batch failed on an EC disconnect. So a client submitting N items
  learns the fate of each rather than an aggregate counter.
- **ETag-on-GET** conditional caching (`304 Not Modified` on
  `If-None-Match`).
- Every runtime tunable lives in `amuleapi.conf`; see the `amuleapi.conf`
  reference above for sections, keys, and defaults.

### Events

`GET /api/v0/events` is a long-lived Server-Sent Events stream with
`Last-Event-ID` replay and typed `resync` frames for cache invalidation.
The channel catalog, frame format, snapshot-then-stream bootstrap, and
reconnect semantics are documented in
[`docs/api/EVENTS.md`](api/EVENTS.md).

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
