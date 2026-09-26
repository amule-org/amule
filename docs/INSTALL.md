# Installing aMule

## Requirements

aMule is built with [CMake](https://cmake.org). You'll need at least the
following packages:

| Package   | Minimum version | Notes                             |
| --------- | --------------- | --------------------------------- |
| CMake     | 3.10            | 3.12 with `-DENABLE_UTP=YES`      |
| zlib      | 1.2.3           |                                   |
| wxWidgets | 3.2.0           | 3.2 branch or newer               |
| Crypto++  | 8.1             | classic or cryptopp-modern        |
| Boost     | 1.70            | headers only; only `asio` is used |

The Crypto++ row accepts either the classic
[weidai11/cryptopp](https://github.com/weidai11/cryptopp) library (minimum
8.1, for ChaCha20-Poly1305) or the
[cryptopp-modern](https://github.com/cryptopp-modern/cryptopp-modern) fork (any
release — it is based on 8.9.0, so every release clears the minimum). The cmake
check disambiguates the two by `CRYPTOPP_VERSION`, which the fork encodes as a
calendar version.

For `amuleweb` you'll also need a POSIX-compliant regex library — part of
the standard C library on most GNU systems.

### Optional dependencies

| Package                    | Minimum | What it enables                                                          |
| -------------------------- | ------- | ------------------------------------------------------------------------ |
| `libgd`                    | 2.0.0   | statistics images in `cas`                                               |
| `libupnp`                  | 1.6.6   | UPnP port forwarding                                                     |
| `libmaxminddb`             | 1.0     | country flags + IP→country mapping ([docs/IP2Country.md](IP2Country.md)) |
| `gettext`                  | 0.11.5  | native-language support (NLS)                                            |
| `libayatana-appindicator3` | —       | **Linux only.** StatusNotifierItem tray icon                             |

Without `libayatana-appindicator3` the tray falls back to the legacy
`GtkStatusIcon` API, which GNOME Shell dropped in 3.26 and wlroots
compositors don't implement (silently invisible on Fedora / vanilla GNOME /
Sway). Apt: `libayatana-appindicator3-dev`; RPM:
`libayatana-appindicator-gtk3-devel`.

### Linux-only note: glib-2.0 dev headers

The GUI build calls `g_set_prgname()` to bind the Wayland `wl_app_id` to
the `.desktop` filename, so glib-2.0 dev headers must be visible to
pkg-config. wxGTK transitively depends on glib but distro packaging
doesn't always pull the dev headers as a hard dep — install
`libglib2.0-dev` (Debian/Ubuntu) or `glib2-devel` (Fedora/RPM) explicitly
if pkg-config can't find `glib-2.0`.

### Unicode

aMule is unicode-only; wxWidgets must be built with unicode support
(this is the default since wx 3.0).

## Compiling aMule

The typical build is:

```sh
cmake -B build -DBUILD_MONOLITHIC=YES -DBUILD_REMOTEGUI=YES
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

By default files are installed under `/usr/local`:

| What                 | Where                                                 |
| -------------------- | ----------------------------------------------------- |
| Binaries             | `/usr/local/bin/`                                     |
| Translation catalogs | `/usr/local/share/locale/<lang>/LC_MESSAGES/amule.mo` |
| Data files, docs     | `/usr/local/share/amule/`                             |
| Desktop entries      | `/usr/local/share/applications/`                      |
| Icon                 | `/usr/local/share/icons/hicolor/128x128/apps/`        |

Pass `--prefix=<dir>` to `cmake -B build` (or to `cmake --install`) to
install somewhere other than `/usr/local`. Installing under `$HOME/.local`
is useful during development — no `sudo` required and easy to clean up:

```sh
cmake --install build --prefix=$HOME/.local
```

Platform-specific notes (Homebrew paths on macOS, MSYS2 shells on
Windows, etc.) live in
[`.github/workflows/ccpp.yml`](../.github/workflows/ccpp.yml), which is
the authoritative reference used by CI.

## Uninstalling

CMake records every installed file in `build/install_manifest.txt`. Use
it to remove them:

```sh
sudo xargs rm -f < build/install_manifest.txt
```

If you installed under `$HOME/.local` no `sudo` is needed:

```sh
xargs rm -f < build/install_manifest.txt
```

### Linux-only icon-cache step

`cmake --install` ships `org.amule.aMule.png` into
`<prefix>/share/icons/hicolor/128x128/apps/`. Distro packages (`.deb` /
`.rpm`) refresh the GTK icon-theme cache automatically via post-install
scriptlets; a raw `cmake --install` does not. If the launcher / dock /
tray shows a generic placeholder ("three dots", question mark, etc.)
instead of the aMule mule icon immediately after install, refresh the
cache manually:

```sh
gtk-update-icon-cache -f -t <prefix>/share/icons/hicolor/
```

GNOME Shell's inotify watcher does pick up the new icons on its own
within a few seconds, so this is best-effort — the icon usually resolves
without the manual command.

## Build options

Common `-D` options (`YES` / `NO` unless noted otherwise):

| Option                   | Default | Effect                                                                   |
| ------------------------ | ------- | ------------------------------------------------------------------------ |
| `BUILD_MONOLITHIC`       | YES     | aMule GUI                                                                |
| `BUILD_REMOTEGUI`        | NO      | `amulegui` — remote control GUI                                          |
| `BUILD_DAEMON`           | NO      | `amuled` — headless daemon                                               |
| `BUILD_AMULECMD`         | NO      | `amulecmd` — CLI client for the daemon                                   |
| `BUILD_WEBSERVER`        | NO      | `amuleweb` — HTTP interface for the daemon                               |
| `BUILD_AMULEAPI`         | NO      | `amuleapi` — REST API + SSE daemon ([docs/QUICKSTART-AMULEAPI.md](QUICKSTART-AMULEAPI.md)) |
| `BUILD_ED2K`             | YES     | `ed2k` — queue `ed2k://` and magnet links from the command line          |
| `BUILD_CAS`              | NO      | `cas` — C statistics tool                                                |
| `BUILD_WXCAS`            | NO      | `wxCas` — GUI statistics tool                                            |
| `BUILD_ALC`              | NO      | aMuleLinkCreator GUI                                                     |
| `BUILD_ALCC`             | NO      | aMuleLinkCreator console                                                 |
| `BUILD_FILEVIEW`         | NO      | console file viewer (experimental)                                       |
| `BUILD_EVERYTHING`       | NO      | every program above (`cas` only on Unix)                                 |
| `BUILD_TESTING`          | NO      | unit tests, run with `ctest` ([unittests/README](../unittests/README))    |
| `ENABLE_NLS`             | YES     | native-language support (gettext)                                        |
| `ENABLE_UPNP`            | YES     | UPnP port forwarding                                                     |
| `ENABLE_IP2COUNTRY`      | YES     | libmaxminddb country flags ([docs/IP2Country.md](IP2Country.md))         |
| `ENABLE_MMAP`            | YES     | compile the mmap file-I/O path where the platform has it. Its use is a runtime preference, off by default. Set `NO` to leave the code out, for example in sanitizer builds |
| `ENABLE_BFD`             | YES     | resolve backtrace symbols in-process with libbfd. With `NO`, crash backtraces use `backtrace_symbols()` and an external `addr2line` |
| `TRANSLATED_MANPAGES`    | YES     | render and install translated manpages with `po4a` at build time. Needs `ENABLE_NLS`; skipped with a notice when `po4a` is not found |
| `ENABLE_CCACHE`          | AUTO    | use ccache as compiler launcher when found (`AUTO`/`ON`/`OFF`); set `OFF` for distro builds that manage ccache themselves, `ON` to hard-fail if ccache is missing |
| `ENABLE_VERSION_CHECK`   | ON      | compile in the in-app new-version check (startup notification, the "Check for new version at startup" preference, and the About dialog's "Check for updates" button). Packagers shipping aMule via an OS package manager want `OFF`, so nothing contacts GitHub and the distro's package manager owns updates |
| `USE_SYSTEM_PICOJSON`    | OFF     | use a system-installed `picojson.h` instead of the bundled copy           |
| `DOWNLOAD_AND_BUILD_DEPS` | OFF    | download and build missing dependencies. Needs Git                       |

### Experimental options

These switches are `OFF` by default. Each one compiles in unfinished
work; with the switch off, that code is left out of the build.

| Option                            | Effect |
| --------------------------------- | ------ |
| `ENABLE_UTP`                      | IPv4 uTP in `amule` and `amuled`: datagram framing, inbound streams, and dialing a peer that advertises uTP. Needs CMake 3.12 |
| `ENABLE_IPV6`                     | native IPv6 TCP admission. The IPv6 identity work is not complete |
| `ENABLE_NATT_SERVER_COORDINATION` | the server-coordinated NAT-T wire codecs. No login advertisement or network traffic yet |
| `ENABLE_KAD_PROTOCOL_10`          | advertise Kademlia protocol `0x0a`, with the AICH hashes on keyword storage that `0x09` added |
| `ENABLE_KAD_NODE_PROTECTION`      | local Kad node-protection heuristics: adaptive request timeouts and Kad identity checks. No wire-protocol change |
| `ENABLE_ALL_EXPERIMENTAL`         | all of the switches above |

For the full list:

```sh
cmake -LAH -B build | less
```

## Refreshing translated manpages (maintainers / translators)

The translated manpages are not tracked in git. The build renders them
from the English masters (`docs/man/*.1.in` and
`src/utils/*/docs/*.1.in`) and `docs/man/po/manpages-LANG.po`, with
`po4a` (see `TRANSLATED_MANPAGES` above).

After you edit an English master, regenerate the manpage catalogs from
the top of the source tree, and commit the result:

```sh
./scripts/update-manpages-po.sh
```

This rewrites `docs/man/po/manpages.pot` and merges it into every
`docs/man/po/manpages-LANG.po`.

## Links

* Detailed build and usage information: <https://amule-org.github.io/docs>
* Forum for questions, bug reports, etc: <https://github.com/amule-org/amule/discussions>
* Upstream issue tracker: <https://github.com/amule-org/amule/issues>
