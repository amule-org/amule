# amuleapi — quick start

amuleapi gives aMule an HTTP interface: a JSON REST API and a live event
stream, so other programs can drive a running `amuled`. It connects to
amuled the same way `amulecmd` does and listens on its own port, so it can
run alongside `amuleweb`.

> aMule's older web frontend, **amuleweb**, is **deprecated** — it may be removed in aMule 3.2 or later (it is not being removed yet). amuleapi is its intended replacement.

Endpoint details are in [`docs/api/REFERENCE.md`](api/REFERENCE.md), and the
event stream in [`docs/api/EVENTS.md`](api/EVENTS.md).

## Let aMule start it

The simplest setup, and the safest: aMule launches amuleapi for you and
hands it a one-off login token, so amuleapi never needs a copy of your EC
password.

In the GUI, under *Preferences → Remote Controls*, tick **Run amuleapi
(REST API) on startup** and set the listening interface, HTTP port and admin
password. A remote amulegui can do the same over EC. That is all — amuleapi
then starts and stops with aMule.

Headless, with no GUI: stop amuled (it rewrites `amule.conf` when it exits),
add this to `amule.conf`, and set a password:

```ini
[AmuleApi]
Enabled=1
BindAddress=127.0.0.1
HttpPort=4713
```

```sh
amuleapi --set-admin-pass=mySecret123
```

The password command writes its file and exits. It needs no path, because
amuleapi looks in the same place amuled does. Start amuled and amuleapi
comes up with it.

An admin password is required before you can bind anything other than
`127.0.0.1` — amuleapi refuses to start otherwise.

## Running it yourself

If you start amuleapi by hand, put the EC password in its own config file,
`amuleapi.conf`:

```ini
[EC]
Password=your-ec-password
```

then just:

```sh
amuleapi
```

There is no `--password` option. A password on the command line is visible
to every user on the machine through `ps`, and amuleapi has two safer
routes: the token when aMule starts it, and this file when you don't.

`--host` and `--port` point at amuled (default `127.0.0.1:4712`). amuleapi's
own HTTP port is separate — `4713` by default, and that is the one REST
clients use.

aMule ships no systemd or launchd units. Wrap the command in your own if you
want one.

## Passwords

Two roles: **admin** (full control) and **guest** (read-only, off unless you
give it a password). Set them from the preferences panel, or:

```sh
amuleapi --set-admin-pass=mySecret123
amuleapi --set-guest-pass=readOnlyPass
```

An empty guest password turns guest access off. Passwords are stored so they
cannot be read back, only replaced — which is why the preference fields open
blank, and why leaving one empty keeps the current password. Changes apply at
the next login, with no restart.

If amuleapi runs on a different machine from aMule, set its password there:
the preferences panel only writes the file on aMule's own host.

## Files

amuleapi keeps its files beside amuled's, in the same folder:

| Platform | Folder                                 |
| -------- | -------------------------------------- |
| Linux    | `~/.aMule/`                            |
| macOS    | `~/Library/Application Support/aMule/` |
| Windows  | `%APPDATA%\aMule\`                     |

Use `--config-dir` to point somewhere else.

| File                  | What it is                                                          |
| --------------------- | ------------------------------------------------------------------- |
| `amuleapi.conf`       | Settings — see [below](#amuleapiconf).                              |
| `amuleapi-passwords`  | The admin and guest passwords.                                      |
| `amuleapi-jwt-secret` | Signs login tokens. Delete it to sign everyone out.                 |
| `amuleapi-ec-token`   | The one-off login token, when aMule starts amuleapi. Lasts seconds. |
| `amuleapi.log`        | A copy of the console output. `--no-log-file` turns it off.         |

All are readable only by you.

## Checking it works

```sh
# No login needed.
curl -s http://127.0.0.1:4713/api/v0/version

# Log in, then use the token.
TOKEN=$(curl -s -X POST "http://127.0.0.1:4713/api/v0/auth/login?type=bearer" \
    -H 'Content-Type: application/json' \
    -d '{"password":"mySecret123"}' | jq -r .token)

curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4713/api/v0/status
```

Browsers should call `/auth/login` *without* `?type=bearer` — they get a
cookie instead, which keeps the token out of reach of page scripts.

## Reaching it from another machine

Keep `BindAddress` on `127.0.0.1` and put a reverse proxy in front. The proxy
holds the certificate and speaks HTTPS to the outside world; amuleapi stays on
loopback, where nothing else can reach it.

With [Caddy](https://caddyserver.com) that is the entire config file, and the
certificate is obtained and renewed for you:

```caddy
api.example.com {
    reverse_proxy 127.0.0.1:4713
}
```

nginx needs a little more, and `proxy_http_version 1.1` is not optional — the
default breaks the event stream:

```nginx
server {
    listen 443 ssl;
    server_name api.example.com;

    ssl_certificate     /etc/letsencrypt/live/api.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/api.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:4713;
        proxy_http_version 1.1;
    }
}
```

`certbot --nginx` will get you those two certificate files.

Either way you need a domain name pointing at the machine, with ports 80 and
443 reachable — that is how the certificate authority checks you own the name.
Event streams need no extra buffering or timeout settings: amuleapi already
sends the header that turns nginx buffering off, and its 15-second heartbeat
keeps idle connections from being dropped.

On a home network, with no domain name, an SSH tunnel does the same job with
no certificate at all:

```sh
ssh -N -L 4713:127.0.0.1:4713 you@your-amule-box
```

The API is then at `http://127.0.0.1:4713` on your own machine.

## `amuleapi.conf`

Created with sensible defaults on first run. Your edits and comments survive
restarts.

```ini
[Server]
BindAddress=127.0.0.1     ; who can reach the API
Port=4713                 ; amuleapi's own HTTP port
AllowCORS=0               ; see CORS below
CorsOriginAllowlist=
StaticRoot=               ; folder to serve a web frontend from

[EC]
Host=127.0.0.1            ; where amuled is
Port=4712
Password=                 ; only needed when you start amuleapi yourself
Encryption=1              ; encrypt the link to amuled

[Auth]
LoginFailureWindowSeconds=60
LoginFailureThreshold=5   ; this many bad logins...
LoginLockoutSeconds=300   ; ...locks that address out for this long

[Streaming]
EventBusRingCapacity=16384
```

`--bind`, `--http-port`, `--host`, `--port` and `--config-dir` override the
matching settings for one run.

## CORS

Off by default, so only pages served from the same origin can call the API.
To allow others:

```ini
[Server]
AllowCORS=1
CorsOriginAllowlist=https://your-app.example.com
```

Leaving the allowlist empty accepts any origin.

## What you get

Everything under `/api/v0/`, with full details in
[`docs/api/REFERENCE.md`](api/REFERENCE.md):

- **Downloads** — the queue: add, pause, cancel, clear completed, plus
  comments, filenames and alternate sources.
- **Shared files** — list, verify, and the shared folders.
- **Peers** — who you are connected to, and browsing their shared files.
- **Servers** — the ed2k server list, connecting, and refreshing it.
- **Network** — connect and disconnect ed2k and Kad.
- **Search** — start a search, read results, download one.
- **Categories**, **preferences**, **logs** and **statistics**.

Lists take `limit`, `offset`, `sort` and `order`. Bulk actions report each
item's outcome separately rather than one overall result.

`GET /api/v0/events` streams changes as they happen and can resume where it
left off after a dropped connection — see
[`docs/api/EVENTS.md`](api/EVENTS.md).

## Worth knowing

- **Keep `BindAddress` on `127.0.0.1` unless you need otherwise.** amuleapi
  starts a thread per event-stream listener, so exposing it directly to a
  network is not something to do casually. For remote access, put a reverse
  proxy in front — see [above](#reaching-it-from-another-machine).
- **An admin token can do anything aMule can**, including asking amuled to
  fetch a server list from a URL you supply. Treat the admin password like
  the password to the machine.
