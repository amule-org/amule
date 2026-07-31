# amuleapi — quick start

amuleapi is a standalone HTTP daemon that serves a versioned JSON REST API
and a Server-Sent Events stream backed by amuled. It connects to amuled as
an EC client — the same protocol amuleweb and amulecmd use — and serves its
own HTTP surface on a separate port.

> aMule's older web frontend, **amuleweb**, is **deprecated** — it may be removed in aMule 3.2 or later (it is not being removed yet). amuleapi is its intended replacement.

Per-endpoint contracts live in [`docs/api/REFERENCE.md`](api/REFERENCE.md);
the SSE event catalog and reconnect semantics in
[`docs/api/EVENTS.md`](api/EVENTS.md). The source of truth for routing is
[`src/webapi/Api.cpp`](../src/webapi/Api.cpp). A map of the surface is under
[What ships](#what-ships) below.

## Let aMule start it — the recommended setup

**This is the most secure way to run amuleapi, and the one to prefer unless
you have a reason not to.** A standalone amuleapi needs a long-lived EC
password of its own; an auto-started one never gets a durable EC credential
at all.

Configure it under *Preferences → Remote Controls* (**aMule API server
parameters**): tick **Run amuleapi (REST API) on startup**, set the
**listening interface**, **HTTP port** and **admin password**, and
optionally tick **Enable guest access** with a **guest password**. All of it
is equally editable from a remote amulegui over EC. aMule then spawns:

```sh
amuleapi --config-dir=<amule data dir> --bind=<BindAddress> --http-port=<HttpPort>
```

Note what is *not* there: no EC password, and no path to `amule.conf`.

### Why it is more secure

In the EC protocol the stored password value **is** the credential — the
challenge hashes it directly, so anything holding it can authenticate. A
network-facing daemon holding that value holds the equivalent of your EC
password for its whole run.

So aMule doesn't give it one. At startup it generates a random 128-bit
token, writes it `0600` into the config directory, and accepts it as a
second valid EC credential for that run only. amuleapi reads the token and
deletes the file immediately; aMule deletes it regardless after ten
seconds, so a child that dies before reading cannot leave a secret at rest.
The token is never passed on the command line, because `argv` is readable
by any local user through `ps`.

The result: nothing durable to steal from the API daemon, and nothing to
rotate if it is compromised — restarting aMule invalidates the token.

The admin and guest passwords are unaffected by this and travel their own
route; see [Passwords](#passwords).

Setting an admin password is what allows binding a non-loopback interface —
amuleapi refuses to start otherwise. Changing the interface or port prompts
you to restart aMule; password changes need no restart. amuleapi stops when
aMule exits.

### Headless — no GUI at all

A server running `amuled` alone does not need amulegui for any of this. Stop
amuled first (it rewrites `amule.conf` on exit, so edits made while it runs
are lost), then add to `amule.conf`:

```ini
[AmuleApi]
Enabled=1
BindAddress=127.0.0.1
HttpPort=4713
Path=amuleapi
```

`Path` is the amuleapi executable — a bare name is looked up on `PATH`, so
leave it alone unless yours lives somewhere unusual. Note the key is
`HttpPort`, not `Port`.

Then set the admin password with amuleapi itself:

```sh
amuleapi --set-admin-pass=mySecret123
```

No path needed: amuleapi defaults to the same per-platform directory amuled
uses — see [Files and the config directory](#files-and-the-config-directory).
Add `--config-dir=<path>` only if you start amuled with `-c <path>`.

That writes `amuleapi-passwords` and exits without starting anything. Start
amuled and it brings amuleapi up with the token, exactly as the GUI flow
does — the GUI only ever edited these same keys and called the same
credential store.

Passwords deliberately have no `amule.conf` key: `/AmuleApi/Password` is
transient and is deleted from the file if it ever appears there.

## Requirements

- A running `amuled` (or monolithic `aMule`) with EC enabled.
- For a **standalone** amuleapi only: the EC password from
  `amule.conf[ExternalConnect]/Password` (set it with `amuled --ec-config`
  if you never have).

## Files and the config directory

amuleapi keeps its files in the same per-platform aMule data directory
amuled uses, deliberately: operators reading both sets of config together
don't have to switch directories. amuleapi never writes amuled's files, and
amuled never writes amuleapi's.

| Platform | Default config dir                     |
| -------- | -------------------------------------- |
| Linux    | `~/.aMule/`                            |
| macOS    | `~/Library/Application Support/aMule/` |
| Windows  | `%APPDATA%\aMule\`                     |

Override with `--config-dir=/path/to/dir`.

Each amuleapi file is written mode `0600`:

| File                  | Purpose                                                                                                                     |
| --------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `amuleapi.conf`       | Runtime config: HTTP bind/port/CORS, outbound EC connection, login rate limits, SSE ring size. [Reference below](#amuleapiconf-reference). |
| `amuleapi-jwt-secret` | 32-byte HMAC signing key for issued tokens. Generated on first launch.                                                       |
| `amuleapi-passwords`  | Admin and guest passwords as salted PBKDF2-HMAC-SHA256 records. Never stored in plaintext and never readable back — only replaceable. Also written by aMule and amuled. |
| `amuleapi-ec-token`   | The ephemeral EC token, when aMule started amuleapi. Exists for seconds at most; see [above](#why-it-is-more-secure).        |

amuleapi also writes `amuleapi.log` here by default; see [Logging](#logging).

## Passwords

Admin and guest passwords live only in `amuleapi-passwords` — no copy goes
into `amule.conf`, because two copies can disagree. Set them from the
preferences panel (aMule writes the file directly; a remote amulegui sends
the change over EC and amuled writes it), or from the CLI:

```sh
amuleapi --set-admin-pass=mySecret123
amuleapi --set-guest-pass=readOnlyPass
```

Each invocation writes the file and exits — no HTTP server, no EC
connection — and the exit code reflects success, so
`amuleapi --set-admin-pass=… && systemctl restart amuleapi` short-circuits
on failure. Changes are picked up at the next login, with no restart.

An empty password means the role is disabled: `POST /api/v0/auth/login`
returns `login_disabled` for it. Unticking **Enable guest access** clears
the guest password, which is what turns guest access off.

Because the stored form cannot be reversed, the preference fields are
write-only: they open empty, and leaving one empty keeps the current
password. The panel says whether an admin password is currently set.

Running amuleapi on a *different* host from aMule? The preferences panel
writes the credential file on aMule's host, while that amuleapi reads its
own. Configure it with `--set-admin-pass` or `PATCH /auth/passwords`.

## Running it standalone

```sh
amuleapi --host=127.0.0.1 --port=4712
```

**Two ports.** `--port=4712` is the EC port amuleapi *uses* to reach amuled.
Its own HTTP listener is `amuleapi.conf[Server]/Port` (default `4713`) —
that is the port REST clients hit. amuleweb can run concurrently on its own
port; the two are independent EC clients.

**Keep the EC password out of `argv`.** Omit `--password` and let amuleapi
read it from `amuleapi.conf[EC]/Password`, a `0600` file. `--password=…` puts
the secret in `argv`, visible to any local user via `ps` —
`--password=$EC_PASSWORD` does not help, since the shell expands it before
exec. There is no environment or stdin path; the config file is the
non-`argv` option. Better still, use the [auto-start](#let-amule-start-it--the-recommended-setup)
flow, which needs no durable EC credential at all.

aMule ships no init-system units (systemd, launchd, Windows service) for any
of its daemons. Write a downstream unit wrapping the command above if you
want one.

### Logging

amuleapi tees its console output into `amuleapi.log` in the config dir,
installed early enough to capture config-load errors, EC warnings and a
crash backtrace. Output is low volume — startup plus warnings and errors,
no per-request access log — and rotates at 10 MiB.

- `--log-file=/path/to/file` — write it elsewhere.
- `--no-log-file` — console only.

The daemon prints `amuleapi: logging to <path>` on startup.

## Verifying

```sh
# Public — no auth.
curl -s http://127.0.0.1:4713/api/v0/version

# Login → token. `?type=bearer` opts into the SDK-client shape, putting
# the JWT in the body so a script can extract it. Browser clients omit it
# and use the HttpOnly session cookie set on the response, which keeps the
# token off any XSS-readable surface.
TOKEN=$(curl -s -X POST "http://127.0.0.1:4713/api/v0/auth/login?type=bearer" \
    -H 'Content-Type: application/json' \
    -d '{"password":"mySecret123"}' | jq -r .token)

# Authenticated GET.
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4713/api/v0/status

# Live event stream — run in a second terminal and trigger mutations
# elsewhere to watch events flow.
curl -s -N -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4713/api/v0/events
```

## `amuleapi.conf` reference

INI-style, mode `0600`, created with defaults on first launch. Edits
roundtrip through `wxFileConfig`, so quotes and comments survive restarts.

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
Encryption=1

[Auth]
LoginFailureWindowSeconds=60
LoginFailureThreshold=5
LoginLockoutSeconds=300

[Streaming]
EventBusRingCapacity=16384
```

### `[Server]` — HTTP listener

- `BindAddress` / `Port` — where amuleapi listens. A non-loopback
  `BindAddress` requires an admin password.
- `AllowCORS` / `CorsOriginAllowlist` — see [CORS](#cors).
- `StaticRoot` — directory to serve a frontend bundle from. Empty keeps the
  daemon API-only.

### `[EC]` — outbound connection to amuled

- `Host` / `Port` — where amuled's EC listener is.
- `Password` — the EC password. Used only when no token was issued; left
  empty in the auto-start flow.
- `Encryption` — negotiate an encrypted EC session (default on).

### `[Auth]` — login rate limiter

`LoginFailureThreshold` failures within `LoginFailureWindowSeconds` lock
that address out for `LoginLockoutSeconds`.

### `[Streaming]` — SSE event bus

`EventBusRingCapacity` bounds the replay ring backing `Last-Event-ID`.

CLI `--bind`, `--http-port`, `--host`, `--port`, `--password` and
`--config-dir` override the matching keys without rewriting the file.

## CORS

amuleapi serves no CORS headers by default (same-origin only). To allow
cross-origin browser clients:

```ini
[Server]
AllowCORS=1
CorsOriginAllowlist=https://your-app.example.com,https://staging.example.com
```

An empty `CorsOriginAllowlist` echoes the caller's `Origin`. That is *not*
literally `Access-Control-Allow-Origin: *` — echoing the exact origin is the
only form browsers accept together with `Access-Control-Allow-Credentials:
true`, and a literal `*` would break cookie auth cross-origin.

## What ships

A versioned REST surface under `/api/v0/`. Full contracts — methods, query
params, bodies, error codes — are in
[`docs/api/REFERENCE.md`](api/REFERENCE.md).

- **Auth** — `auth/login`, `auth/logout`, `auth/session` (JWT and session
  cookie).
- **System** — `version`, `version/check`, `status`, `preferences`.
- **Downloads** — queue list and per-file detail; add, pause/resume, cancel,
  `clear_completed`; per-file comments, source-reported filenames, and A4AF
  listing and swap.
- **Shared files** — list and detail, `reload`, `verify`, and the share
  roots (`shared/directories`).
- **Clients (peers)** — per-peer view (`?filter=uploads|downloads|active`),
  per-client detail, and browsing a peer's shared files.
- **Servers** — the ed2k server list: add, connect, remove, `servers/update`.
- **Network control** — `networks/connect` / `disconnect`, `kad/bootstrap`,
  `kad` status.
- **Categories** — list, create, edit, delete.
- **Search** — `search`, `search/results`, `search/stop`, download a result,
  per-result comments.
- **Logs & stats** — `logs/{amule,serverinfo}`, `stats/tree`,
  `stats/graphs/{graph}`.

Conventions across the surface:

- **List windowing.** `downloads`, `clients`, `shared`, `servers` and
  `search/results` take `limit` / `offset` / `sort` / `order` and return
  `total` / `offset` / `limit` beside the array. Omit them all for the full
  set.
- **Bulk mutations** return one entry per input item under `results` —
  `200`/`202` when all succeeded, `207 Multi-Status` for a mix, `503` when
  the batch failed on an EC disconnect. Callers learn the fate of each item,
  not an aggregate count.
- **ETag on GET** with `304 Not Modified` on `If-None-Match`.

### Events

`GET /api/v0/events` is a long-lived SSE stream with `Last-Event-ID` replay
and typed `resync` frames for cache invalidation. Channels, frame format and
reconnect semantics are in [`docs/api/EVENTS.md`](api/EVENTS.md).

## Security notes

- **Prefer the auto-start flow.** It is the only configuration in which the
  API daemon never holds a durable EC credential. See
  [above](#why-it-is-more-secure).
- **The admin role grants full control of the daemon's network surface.**
  That includes `POST /api/v0/servers/update {"servers_url": "..."}`, which
  makes amuled fetch the supplied URL. This is long-standing amuled
  behaviour, but it widens what an admin token *grants*: whoever holds one
  can have amuled issue an HTTP GET against arbitrary reachable URLs (a
  classic SSRF surface) and pull the response into amuled's process. The
  `http://` / `https://` pre-check is input hygiene, not a boundary. Protect
  the admin password and the JWT secret accordingly.
- **`BindAddress=127.0.0.1` is load-bearing.** The HTTP server spawns one OS
  thread per SSE subscriber, so a non-loopback bind exposes the
  thread-per-connection model to unauthenticated peers. For remote access,
  put a reverse proxy in front and keep the bind on loopback.
