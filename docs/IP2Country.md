# IP2Country setup

aMule's *"Show country flags for clients"* feature uses a GeoIP database
to look up a country code from a peer's IP address and render the
matching flag in the client list, search results, transfers pane, and
shared-files-peers list.

The backing library is `libmaxminddb` and the on-disk database file is
`geoip.mmdb`. As of aMule 3.0.x the feature has a dedicated `Preferences
→ IP2Country` tab that handles source selection, download, and
auto-update from inside aMule — no `amule.conf` editing required.


## Build prerequisites

`libmaxminddb` must be installed and discoverable by CMake's
`find_path(maxminddb.h)` and `find_library(maxminddb)` for the
IP2Country feature to be compiled in.

| Platform               | Install                                                  |
|------------------------|----------------------------------------------------------|
| Debian / Ubuntu        | `apt install libmaxminddb-dev`                           |
| Fedora / RHEL          | `dnf install libmaxminddb-devel`                         |
| Arch Linux             | `pacman -S libmaxminddb`                                 |
| macOS (Homebrew)       | `brew install libmaxminddb`                              |
| MSYS2 (Windows clang)  | `pacman -S mingw-w64-clang-aarch64-libmaxminddb` (ARM64) |
| MSYS2 (Windows MinGW)  | `pacman -S mingw-w64-x86_64-libmaxminddb` (x64)          |

If the library is not present at configure time, `ENABLE_IP2COUNTRY` is
set to `OFF`, the feature is omitted from the build with a status
message, and the `IP2Country` preferences tab is hidden.


## The IP2Country preferences tab

Open `Preferences → IP2Country`. The tab looks like this:

```
☑ Show country flags for clients

┌─ Database ──────────────────────────────────────────┐
│ Status: ●  Loaded                                    │
│            ~/.aMule/geoip.mmdb (9.0 MB) — <source>   │
│                                                      │
│ Source: [DB-IP (free, no account)            ▼]     │
│                                                      │
│ ┌── source-specific info, credentials, attribution ──│
│ │                                                    │
│ └────────────────────────────────────────────────────│
│                                                      │
│ [Update now]    ☑ Auto-update on startup            │
└──────────────────────────────────────────────────────┘
```

The master "Show country flags for clients" toggle is the same one that
used to live in the `Interface` tab — it has moved here so all GeoIP
controls are in one place.

### Three sources

| Source             | Account required? | Default? |
|--------------------|-------------------|----------|
| **DB-IP**          | No                | Yes      |
| **MaxMind GeoLite2** | Yes (free)      | No       |
| **Custom URL**     | n/a               | No       |

#### DB-IP (default — recommended for most users)

Zero-configuration. Click `Update now` and aMule downloads
`dbip-country-lite-YYYY-MM.mmdb.gz` from DB-IP's free distribution. The
database refreshes monthly; with `Auto-update on startup` enabled
(default) aMule re-downloads on every launch so you always have current
data.

Early-of-month note: DB-IP usually publishes the new month's file by the
second or third day of the month. If you launch aMule on day 1 and the
new file isn't up yet, aMule retries once with the previous month's URL
and uses that until the new file appears. This happens transparently —
you don't need to do anything.

**Attribution**: IP-to-Country data by DB-IP.com
([https://db-ip.com](https://db-ip.com))
**License**: Creative Commons BY 4.0
([terms](https://creativecommons.org/licenses/by/4.0/))

#### MaxMind GeoLite2

Requires a free MaxMind account. Once you have one:

1. Sign in to your MaxMind account, generate a *License Key* under
   `Manage License Keys`.
2. Paste the License Key into the `License key` field.
3. Click `Update now`.

aMule downloads `GeoLite2-Country` in `.tar.gz` form, extracts the
`.mmdb` file out of the date-stamped directory, and installs it at
`~/.aMule/geoip.mmdb`.

aMule uses MaxMind's License-Key download URL form
(`https://download.maxmind.com/app/geoip_download?edition_id=...&license_key=...&suffix=tar.gz`),
so no Account ID is required. Users who specifically need the
basic-auth URL form (`https://account:key@download.maxmind.com/...`)
can fall back to **Custom URL** with that URL pasted in.

**Attribution**: This product includes GeoLite2 data created by
MaxMind, available from
[https://www.maxmind.com](https://www.maxmind.com)
**License**: [MaxMind GeoLite2 EULA](https://www.maxmind.com/en/geolite2/eula)
— requires attribution and account registration; the database must be
refreshed at least every 30 days. The default `Auto-update on startup`
keeps you compliant automatically.

#### Custom URL

Escape hatch for self-hosted mirrors or new providers aMule doesn't yet
know about. Paste a URL that points to either:

* an `.mmdb` file directly, or
* a `.gz` containing the `.mmdb`, or
* a `.tar.gz` containing the `.mmdb` inside any directory layout.

The URL may include credentials in the userinfo portion
(`https://user:pass@host/...`). License terms and attribution
requirements are determined by the upstream URL — you are responsible
for compliance.


## Where the database lives on disk

| Platform              | Path                                                          |
|-----------------------|---------------------------------------------------------------|
| Linux                 | `~/.aMule/geoip.mmdb`                                         |
| macOS                 | `~/Library/Application Support/aMule/geoip.mmdb`              |
| Windows               | `%APPDATA%\aMule\geoip.mmdb`                                  |
| Windows (portable)    | `<install-dir>\.aMule\geoip.mmdb` if a `.aMule` folder is next to `amule.exe`, otherwise the same `%APPDATA%` path |

You can also drop your own `.mmdb` file at that path directly — aMule
will use it without any UI configuration. This is the recommended path
for air-gapped installations.

### Migration from earlier aMule versions

aMule 2.x stored the file as `GeoLite2-Country.mmdb`. On first launch
of 3.0.x or later, if `GeoLite2-Country.mmdb` exists and `geoip.mmdb`
does not, aMule renames it automatically and logs the migration. No
manual action required.

Any value previously set in `amule.conf` under
`/eMule/GeoLiteCountryUpdateUrl` is migrated to the new `Custom URL`
source on first launch, so users who hand-configured a download endpoint
don't lose it.


## Auto-update behaviour

`Auto-update on startup` (default ON for all sources) triggers a fresh
download from the selected source on every aMule launch when both:

* the master `Show country flags for clients` is ticked, AND
* a database is already loaded (the first-run / missing-file path
  always triggers a download regardless of this toggle).

For MaxMind this also satisfies their EULA's "refresh at least every
30 days" requirement.

If a download fails (network outage, expired credentials, URL change),
aMule logs the failure and continues using whatever database file is
already on disk — the feature degrades gracefully rather than failing
loudly.


## Diagnostics

If the feature is enabled but flags don't appear:

* Confirm the file exists at the expected path and is non-empty.
* Open `Preferences → IP2Country` — the status block reports the
  current state in plain text:

| Status                     | Meaning                                                   |
|----------------------------|-----------------------------------------------------------|
| `Loaded`                   | Database open, country lookups working.                   |
| `Failed to load — file present but rejected by libmaxminddb` | The file is there but isn't a valid `.mmdb`. Click `Update now` to re-download. |
| `Not found`                | No `geoip.mmdb` at the expected path. Click `Update now`. |

* Restart aMule after replacing the database file — it's opened with
  `MMDB_MODE_MMAP` and not re-checked on the fly.

A log line of the form

```
Failed to open MaxMindDB database '…': <error>
```

indicates the file is missing, unreadable, or not a valid `.mmdb`.


## Editing amule.conf directly (advanced)

The IP2Country panel covers every supported configuration; the keys
below are documented for completeness only. They live in `[eMule]`:

| Key                       | Values                                  |
|---------------------------|-----------------------------------------|
| `GeoIPEnabled`            | `1` / `0` — master enable               |
| `GeoIPSource`             | `dbip` / `maxmind` / `custom`           |
| `GeoIPMaxMindLicense`     | Free-form string (MaxMind License Key)  |
| `GeoIPCustomUrl`          | Any URL                                 |
| `GeoIPAutoUpdate`         | `1` / `0`                               |

The legacy `GeoLiteCountryUpdateUrl` key is read once on first 3.0.x
launch and migrated into `GeoIPCustomUrl` + `GeoIPSource=custom`, then
cleared. Subsequent writes to it are ignored.

Path to `amule.conf`:

| Platform | Path                                              |
|----------|---------------------------------------------------|
| Linux    | `~/.aMule/amule.conf`                             |
| macOS    | `~/Library/Application Support/aMule/amule.conf`  |
| Windows  | `%APPDATA%\aMule\amule.conf`                      |
