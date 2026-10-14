# aMule Web frontend

A from-scratch web frontend for aMule that talks to the
[amuleapi](../../docs/QUICKSTART-AMULEAPI.md) REST + SSE surface. Views are
[preact](https://preactjs.com) components written with
[htm](https://github.com/developit/htm) (JSX-like tagged templates, no
transpilation). **No build step, no bundler, no npm/CDN at runtime** — every
asset (including the libraries) is vendored under `static/`, resolved by a
native browser [import map](static/index.html), and served as-is.

## Layout

`static/` holds **only** servable static content, so it is copied verbatim on
install. The dev-only `tools/` and this README live one level up.

```
src/webapi/
  README.md               this file
  tools/run-dev.sh        one-command dev launcher (amuled/amule + amuleapi)
  tools/update-vendor.sh  re-download / upgrade the vendored libraries
  static/                 servable static bundle (installed as amuleapi-static)
    index.html            single-page app entry point (+ import map)
    css/                  styles (app.css)
    js/                   ES modules: app, api, store, events, dom, format,
                          components, charts, icons, theme, i18n, views/
    js/vendor/            vendored third-party libs (+ their LICENSE)
    img/                  icons
    i18n/                 UI translations (flat JSON, en.json is the source)
```

## Translations (frontend)

Every user-facing string goes through `t()` / `tn()` in `static/js/i18n.js`;
the dictionaries are flat `key -> string` JSON files under `static/i18n/`,
translated on Weblate (component "Application WebUI", format "i18next JSON
file v4", `en.json` as the source). Plurals are `<key>_one` / `<key>_other`,
plus `_few` / `_many`… where a language needs them; keys must not contain `.`
or `[]`. The language is picked from localStorage (`amule.lang`, set in
Preferences) or the browser language, falling back to English per missing or
blank key. Languages are normally added on Weblate, which creates
`static/i18n/<code>.json` named with a canonical BCP 47 code (`pt-BR`, not
`pt_BR`); then add the code to `LANGS` in `static/js/i18n.js`. Run
`node src/webapi/tools/check-i18n.mjs` to verify every locale key exists in
`en.json` with matching `{placeholders}`, and that every code is canonical and
every `LANGS` entry has its file.

## Vendored libraries

Third-party libraries are vendored locally (no CDN, no npm at runtime). The
bare specifiers are wired to these files by the import map in `static/index.html`.

| Library | Specifier | Version | File | License |
|---------|-----------|---------|------|---------|
| [preact](https://preactjs.com) | `preact` | 10.29.2 | `static/js/vendor/preact.module.js` | MIT |
| preact hooks | `preact/hooks` | 10.29.2 | `static/js/vendor/hooks.module.js` | MIT |
| [htm](https://github.com/developit/htm) | `htm` | 3.1.1 | `static/js/vendor/htm.module.js` | MIT |

Each vendored file carries its name and version as a comment on the first line,
and ships next to its `*.LICENSE`.

### Updating vendored libraries

Versions are pinned as constants at the top of `tools/update-vendor.sh`. To
upgrade (or re-download), edit the relevant `*_VERSION` and run the script:

```sh
src/webapi/tools/update-vendor.sh
```

It downloads the minified ES modules into `static/js/vendor/`, prepends the
name + version comment, and refreshes the `*.LICENSE` files.

## Running

`amuleapi` serves this frontend itself (via its `StaticRoot` config key) on the
same origin as the REST + SSE API, so there is no separate static server: you
just run a backend + `amuleapi` and open the amuleapi URL.

### Option A — dev launcher (recommended while iterating)

`tools/run-dev.sh` brings the whole stack up in one command, in an **isolated**
config dir (default `~/.aMule-dev`, never touches your real `~/.aMule`): it
enables EC, sets the web admin/guest passwords, points amuleapi's `StaticRoot`
at `static/`, starts the backend, and runs amuleapi in the foreground. Edit the
HTML/JS/CSS under `static/` and just reload the browser.

```sh
src/webapi/tools/run-dev.sh          # amuled backend (default)
src/webapi/tools/run-dev.sh amule    # amule GUI backend (needs a display)
src/webapi/tools/run-dev.sh stop     # stop the dev processes
```

Then open `http://127.0.0.1:4713/` and log in (default `admin` / `guest`).

Requires the binaries to be built — `amuleapi` (`cmake -DBUILD_AMULEAPI=ON`) and
the chosen backend (`-DBUILD_DAEMON=ON` for `amuled`, or the default
`-DBUILD_MONOLITHIC=ON` for `amule`). The script prints the exact CMake flag if a
binary is missing.

Key environment overrides: `HTTP_PORT` (default 4713), `EC_PORT` (4712),
`CONFIG_DIR` (`~/.aMule-dev`), `ADMIN_PASS`/`GUEST_PASS`, `EC_PASS`, `BUILD_DIR`,
`STATIC_DIR`.

### Option B — manual / installed deployment

After `make install`, the bundle ships to
`${CMAKE_INSTALL_DATADIR}/amule/amuleapi-static` (e.g.
`/usr/share/amule/amuleapi-static`). An empty `StaticRoot` (the default) makes
the daemon auto-discover that installed location, so an installed amuleapi
serves the frontend out of the box — no config needed.

To serve a bundle from elsewhere (e.g. a source tree without installing), point
`StaticRoot` at it explicitly and restart amuleapi:

```ini
[Server]
StaticRoot=/path/to/aMule/src/webapi/static
```

Then open `http://127.0.0.1:4713/`.

## Constraints

- preact + htm via native ES modules and an import map — no build step, no
  bundler. Responsive (phone / tablet / desktop).
- Zero runtime downloads; third-party libs are vendored under
  `static/js/vendor/` with their license.
- Authenticates via the amuleapi session cookie (never stores the JWT).

Chat CAPTCHA challenges and results are included in the ordinary message history
and `chat_message` events. A challenge has `type: "captcha_challenge"` and
`captcha_image` containing a base64-encoded BMP. A result has
`type: "captcha_result"` and `captcha_status`: `0` for success, `1` for failure
with another attempt allowed, or `2` when attempts are exhausted. Ordinary text
messages omit these fields. Send the CAPTCHA text through the existing
`POST /api/v1/chats/{address}/messages` endpoint. After a retryable failure,
send another message to request a new challenge. Clients that present a dedicated
answer dialog can include `captcha_id` (the challenge's message `id`) in the POST
body. The daemon rejects an expired, replaced, or already answered challenge with
HTTP 409 `captcha_expired`, without sending the answer or reopening a conversation.
The session list exposes `pending_captcha_id` and `pending_captcha_image` (both
`null` when no answer is pending). The image survives transcript eviction.
`chat_session_updated` reports pending-id changes, including disconnect expiry.
The WebUI captures the id when typing starts and submits it with the answer.
An unknown hash-addressed conversation still returns the usual HTTP 404. The daemon only accepts
challenges after an outgoing message in an open conversation, and only accepts
results while awaiting a solution response. It validates bitmap size, dimensions,
layout and pixel data before storing the image; retained history is bounded by
the existing per-session message limit.

An offline peer test covers this flow with isolated daemons and a simulated
local eD2k peer:

```sh
python3 unittests/curl-tests/amuleapi/chat-captcha-peer.py --build-dir build
```
