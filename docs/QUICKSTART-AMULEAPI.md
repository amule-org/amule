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
- `/api/v0/downloads`, `/uploads`, `/shared`, `/servers`, `/kad`,
  `/clients`, `/categories`, `/logs/{amule,serverinfo}`,
  `/stats/{tree,graphs/{graph}}`, `/search`, `/search/results`
- POST / PATCH / DELETE mutations on each resource (admin role)
- ETag-on-GET conditional caching (304 Not Modified on `If-None-Match`)
- `/api/v0/events` — long-lived Server-Sent Events stream with
  `Last-Event-ID` replay and typed `resync` events for cache invalidation
- Optional CORS allowlist via `amuleapi.conf[Server]/CorsOriginAllowlist`
