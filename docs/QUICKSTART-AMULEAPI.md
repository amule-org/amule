# amuleapi — quick start

amuleapi is a standalone HTTP daemon that serves a versioned JSON REST API
and a long-lived Server-Sent Events stream backed by amuled. It connects
to amuled as an EC client (same protocol amuleweb and amulecmd use) and
exposes its own HTTP surface on a separate port. amuleapi is the first
shipping REST API for aMule — there is no prior on-the-wire surface to
migrate from.

The protocol contract for `/api/v0/*` and the SSE event types is
documented under the source tree at `docs/api/` (per-endpoint OpenAPI-
style fragments).

## Requirements

- A running `amuled` (or a monolithic `aMule` with EC enabled) that
  amuleapi can connect to over the EC protocol.
- The EC password from `amule.conf[ExternalConnect]/Password` (set via
  `amuled --ec-config` if you've never run it).

## First-run setup

amuleapi keeps its config in the same per-platform aMule data directory
that `amuled` uses:

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
| `amuleapi.conf`           | INI-style runtime config (bind address, port, CORS allowlist, EC overrides, auth rate-limit knobs).      |
| `amuleapi-jwt-secret`     | 32-byte HMAC signing key for issued tokens. Auto-generated on first launch if absent.                    |
| `amuleapi-passwords`      | MD5-hashed admin and guest passwords. Plaintext is never persisted.                                      |

Set passwords via the dedicated CLI flags (these write the file and exit
without starting the HTTP server):

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

- `--host` / `--port` / `--password` specify the EC connection to
  `amuled` (default port `4712`).
- HTTP serves on `amuleapi.conf[Server]/Port` (default `4713`).
- amuleweb can run concurrently on its own port (default `4711`); the
  two daemons talk to amuled independently as separate EC clients.

aMule does not ship init-system units (systemd, launchd, Windows
service) for any of its daemons. If you want one, write a downstream
unit that wraps the command above.

## Verifying

```sh
# Public — no auth.
curl -s http://127.0.0.1:4713/api/v0/version

# Login → token.
TOKEN=$(curl -s -X POST http://127.0.0.1:4713/api/v0/auth/login \
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

## CORS

By default amuleapi serves no CORS headers (same-origin only). To allow
cross-origin browser clients, set in `amuleapi.conf`:

```ini
[Server]
AllowCORS=1
CorsOriginAllowlist=https://your-app.example.com,https://staging.example.com
```

Leave `CorsOriginAllowlist` empty to echo any caller's `Origin` header
(wildcard equivalent that stays cookie-compatible).

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
- Optional CORS allowlist via `amuleapi.conf[Server]/CorsOriginAllowlist`

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
