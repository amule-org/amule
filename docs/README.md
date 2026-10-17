# aMule — User Guide

This document covers the end-user side of aMule: how to launch each
tool, what to configure on first run, and where the safety footguns
are. For an overview of the project see the
[top-level README](../README.md). For installing the pre-built
binaries see [INSTALL_BINARIES.md](INSTALL_BINARIES.md); for building
from source see [INSTALL.md](INSTALL.md).


## What you got

aMule is shipped as several binaries that share the same on-disk state
under `~/.aMule/`:

| Binary      | What it is | When to use it |
| ----------- | ---------- | -------------- |
| `amule`     | The all-in-one GUI client. Daemon and UI in one process. | Day-to-day use on a desktop. |
| `amuled`    | The headless daemon — same engine as `amule` minus the UI. | When you want aMule running on a NAS / VPS / always-on box and connect to it remotely. |
| `amulegui`  | A remote GUI that talks to a running `amuled` over the [EC protocol](EC_Protocol.md). | Drive a remote `amuled` from your desktop. |
| `amuleweb`  | A small HTTP server that exposes a running `amuled` to a browser. | Drive a remote `amuled` from a phone or another machine without installing anything. |
| `amulecmd`  | An interactive CLI that talks to a running `amuled`. | Scripts, headless administration, troubleshooting. |
| `ed2k`      | A tiny helper that hands `ed2k://` and magnet links to a running aMule. | Queue links from a script or a terminal. Browsers hand clicked links to `amule` / `amulegui` themselves. |

Pick `amule` if you're not sure which to use — it's the all-in-one.


## Log file location

By default, `amule` and `amuled` write `logfile` in the configuration directory.
To store logs separately, stop aMule and set a full filename in `amule.conf`:

```ini
[eMule]
LogFilePath=/var/log/amule/logfile
```

Create the parent directory first and make it writable by the user running aMule.
You can use a directory on a RAM-backed filesystem while keeping the configuration
and other persistent data in their usual location.

For a single run, use `amuled --log-file=/var/log/amule/logfile` (also supported
by `amule` and `amulegui`). The command-line value overrides the saved setting
without changing it. Relative paths are resolved against the configuration
directory, including when using `--config-dir`.

An empty or absent `LogFilePath` keeps the default location. Changes take effect
after restart. On startup, the previous log is backed up beside the selected file
with `.bak` appended. If that backup fails, aMule logs a warning and appends
to the existing log instead of truncating it or stopping startup. Remote log viewing
and reset use the selected file too.
For `amulegui`, the setting is in `remote.conf` and controls its local log, whose
default filename is `remotelogfile`.


## First-run checklist

aMule ships with reasonable defaults and is usable as-is. Three
configuration steps are still worth doing on day one:

### 1. Open the ports — get a HighID

aMule needs **TCP 4662** and **UDP 4665** + **UDP 4672** reachable from
the internet to be a full peer on the network. If they're not, you'll
get a *LowID*, which makes you reachable from far fewer sources.

* **Behind a router**: forward the three ports to the machine running
  aMule (or use UPnP — `Preferences → Connection → UPnP enabled`).
* **Behind a firewall**: allow inbound on those ports.

The [network connectivity guide][network] has detailed walkthroughs for
getting a HighID and setting up firewall rules.

[network]: https://amule-org.github.io/docs/manual/configuration/network-connectivity

### 2. Set realistic upload / download limits

Under `Preferences → Connection`, set the limits to roughly **80 % of
your actual line speed** to avoid saturating the upstream and starving
your own traffic.

The values are in **kibibytes per second** (KiB/s, units of 1024 bytes).
ISP advertised speed is usually in **megabits per second** (Mbps); to
convert, multiply Mbps by **122**.

> Example: a 100 Mbps / 20 Mbps fibre line → roughly 12200 KiB/s
> downstream and 2440 KiB/s upstream. Set the *limits* to about 9800
> down / 1950 up to stay below the line cap.

### 3. Pick what you share

`Preferences → Shared Folders` controls what you offer to the network.
Defaults are conservative; **don't share blanket filesystem trees**.

> **Never share** your entire home directory or a system root.
> `/etc`, `/var`, `/lib`, `/boot`, `/usr` and similar must stay
> private. Don't share folders containing private files (SSH keys,
> tax documents, password files, etc.).

A focused share — the Incoming dir, plus one or two media folders —
is the safe default. If you share more than ~200 files, some servers
may drop you because of a per-client file limit; lots of small files
beats one giant index.


## Running aMule headless (`amuled`)

If you want aMule running on a NAS, VPS, or always-on home server, use
`amuled`. First-time setup:

```sh
# Generate a config dir and EC password (the daemon needs a password
# for amulegui / amuleweb / amulecmd to connect)
amuled                                # interactive once; Ctrl-C
$EDITOR ~/.aMule/amule.conf           # set [ExternalConn]
                                      # AcceptExternalConnections=1
                                      # ECPassword=<md5 of your password>

# Then run it daemonised
amuled --full-daemon
```

Connect to it from another machine with `amulegui`, `amuleweb`, or
`amulecmd` using the same EC password.

The documentation has detailed walkthroughs for
[amuled setup](https://amule-org.github.io/docs/manual/interfaces/amuled),
[amuleweb](https://amule-org.github.io/docs/manual/interfaces/amuleweb), and
[amulecmd](https://amule-org.github.io/docs/manual/interfaces/amulecmd).


## Reading the transfers window

aMule's per-file progress bar uses colour to communicate availability:

| Colour              | Meaning |
| ------------------- | ------- |
| **Black**           | Parts you already have |
| **Red**             | Parts missing in *all* known sources — nobody on the network can give them to you right now |
| **Blue (shades)**   | Parts available in known sources; darker = higher availability |
| **Yellow**          | Part being downloaded *right now* |
| **Green** (top bar) | Total file completion |

Per-source bars (when you expand a download):

| Colour      | Meaning |
| ----------- | ------- |
| **Black**   | Parts you're still missing |
| **Silver**  | Parts this source is also missing |
| **Green**   | Parts you already have |
| **Yellow**  | Part being uploaded *to you* right now |

A red part isn't necessarily lost — it just means none of your *current*
sources have it. Switching servers, joining Kad, or simply waiting often
turns red into blue/black over hours.


## Common file types

| Group     | Extensions |
| --------- | ---------- |
| Audio     | `mp3` `m4a` `aac` `flac` `ogg` `opus` `wav` `wma` `ape` |
| Video     | `mkv` `mp4` `webm` `mov` `avi` `mpg` `mpeg` `m4v` `vob` `wmv` |
| Archive   | `zip` `7z` `rar` `tar.gz` `tar.bz2` `tar.xz` `tar.zst` `cbz` `cbr` |
| Disc image| `iso` `img` `bin/cue` `nrg` `mds/mdf` `ccd/sub` |
| Documents | `pdf` `epub` `mobi` `djvu` `chm` `azw3` |
| Images    | `jpg` `jpeg` `png` `webp` `avif` `gif` `tif` `heic` |
| Software  | `exe` `msi` `dmg` `pkg` `deb` `rpm` `AppImage` `flatpak` |

aMule's search also supports filtering by these categories — click the
type dropdown in the search panel.


## Passing event values to commands

Event command templates are split into arguments before `%FILE`, `%NAME`,
`%HASH`, `%SIZE`, `%DLACTIVETIME`, `%SENDER`, or `%PARTITION` is substituted.
Each value stays inside its original argument, including spaces and shell
punctuation. Quote fixed paths in the template using the native command syntax;
placeholders do not need extra quoting to keep substituted spaces intact.
Existing commands that relied on a placeholder expanding into multiple arguments
must be rewritten with those arguments explicitly in the template.

For example, pass several values as separate arguments to a script, or combine
several placeholders inside one quoted argument:

```sh
/home/me/bin/on-complete.sh %FILE %HASH %SIZE
notify-send "aMule" "Finished %NAME (%SIZE bytes)"
```

aMule rejects placeholders in the executable name and embedded NUL characters.
Keep the executable and option names fixed. If the receiving program supports
`--` to end option parsing, use it before untrusted values so a value starting
with `-` cannot become an option.

aMule rejects substitution into recognized interpreter code or script filenames.
For POSIX shells, the supported inline form is exactly `sh -c` (or another
recognized POSIX shell with `-c`) followed by fixed code. Put values in positional
arguments after that code. Other recognized interpreters should run a fixed script
file followed by data arguments; interpreter options combined with placeholders
are rejected. For example:

```sh
sh -c 'mv -- "$1" "/archive/$2-$3"' _ %FILE %HASH %NAME
```

Here `_` supplies the shell's `$0`, and `%FILE`, `%HASH`, and `%NAME` become `$1`,
`$2`, and `$3`. Always quote positional arguments in the script.

On Windows, native programs receive literal arguments using Windows C runtime
escaping. Characters such as `%`, `!`, and quotes remain data for these programs;
filenames such as `100% Hits.mp3` and `Help!.avi` work for event commands and previews.

For `cmd.exe` and `.bat`/`.cmd` targets, aMule rejects substituted values containing
`"`, `%`, `!`, CR, or LF. Those characters can change quoting, expand environment
variables, or introduce another command. Other punctuation, including `&`, `|`,
`<`, `>`, and `^`, stays inside quoted data arguments. The filter also checks
literal prefixes and suffixes surrounding placeholders.

aMule explicitly invokes the OS command processor for batch files and adds an
outer quote pair with `/d /s` so `cmd` removes only that pair, preserving each
argument's quotes. Backslashes are kept literal for batch files and builtins;
native children retain CRT quoting.

Use a fixed command token with separate values, for example `cmd /d /c echo %SENDER`.
External targets after `cmd /c` or `/k` must include `.exe`, `.com`, `.bat`, or
`.cmd`; extensionless names are refused when inserting data because `PATHEXT`
can select a batch file with different argument quoting. Quote fixed paths
containing spaces, for example:

```bat
cmd /d /c "C:\Program Files\hooks\on-chat.cmd" %SENDER
```

Do not put a placeholder inside a combined command string such as
`cmd /c "echo %SENDER"`; it is code, and aMule refuses it. Compound command
strings, user-specified `/s`, dispatch builtins (`call`, `start`, `for`, `if`),
and nested interpreters with event data are unsupported; put that logic in a
fixed batch file instead.
PowerShell and the other prohibited launchers still refuse substituted values.
Existing Windows short-path aliases are expanded before interpreter checks.

Batch authors must quote positional values when using them, just as POSIX scripts
quote `"$1"`. For example, a template `C:\scripts\on-complete.cmd %FILE %HASH %SIZE`
passes three arguments to a batch file that can use:

```bat
@echo off
native-tool.exe "%~1" "%~2" "%~3"
```

Do not re-evaluate event values using `call`, another `cmd /c`, or an interpreter.
[`cmd.exe`](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/cmd)
expands `%VAR%` and delayed `!VAR!` references, which is why those values are refused.

Fixed event templates with no substituted values run unchanged on Windows,
including redirection and compound commands. Media previews always insert a
filename and therefore use the argument validation and quoting described above.

Validation refusals log that the command was not run and give the reason;
a missing executable or another spawn failure retains the launch-failure message.

These checks recognize common interpreters and wrappers; they cannot establish
how an arbitrary executable, renamed interpreter, or custom script uses its
arguments. Use programs and fixed scripts that treat event values as data.
Never evaluate those values as code inside the receiving program.

### Configuring event commands

In the monolithic aMule application, Preferences → Events configures Core and
GUI commands. With a remote `amulegui`, Core command controls are disabled:
Core commands must be configured in the daemon's `amule.conf`. Restart `amuled`
after editing its event command settings. Remote GUI preferences are not sent
to the daemon.

The "New chat session" event is raised locally by `amulegui`, so its GUI command
and `%SENDER` variable remain available with a remote core. "Download completed"
also runs its GUI command locally when `amulegui` observes a known download change
to complete. Downloads already complete when first seen, including the initial
sync and the first sync after a reconnect, do not trigger commands. Each GUI sees
its own transitions; a completion cleared by another client before the next poll
can be missed.

Remote completion commands support `%NAME`, `%HASH`, `%SIZE`, and `%DLACTIVETIME`.
`%FILE` expands to an empty string in remote GUI mode: EC does not supply a reliable
full path for the local download proxy, and the file lives on the daemon's host.
For example, a local desktop notification can use:

```sh
notify-send "aMule" "Finished %NAME (%SIZE bytes)"
```

"Error on completion" and "Out of space" remain daemon-only with a remote core,
and their GUI controls are disabled. EC's error status also covers failures
unrelated to completion, so it cannot reliably identify the completion error
event; daemon free-space checks are not visible to the GUI.


## Troubleshooting

* **"LowID"** — your ports aren't reachable. See the
  [HighID guide][highid].
* **Generic icon in the launcher** — the GTK icon-theme cache hasn't
  refreshed since install. See the [icon-cache section in
  INSTALL.md](INSTALL.md#linux-only-icon-cache-step).
* **AppImage doesn't appear in the application menu** — on first
  launch the AppImage will prompt to integrate itself; if you declined
  ("Don't ask again"), the documentation describes the manual install of
  the `.desktop` file.
* **Tray icon invisible on GNOME** — you need an SNI host. On vanilla
  GNOME install
  [`gnome-shell-extension-appindicator`](https://extensions.gnome.org/extension/615/appindicator-support/);
  Ubuntu enables this by default.

For anything else, the documentation and forum are the best places to look:

* Documentation: <https://amule-org.github.io/docs>
* Forum: <https://github.com/amule-org/amule/discussions>
* GitHub Issues: <https://github.com/amule-org/amule/issues>


## Safety / legal

aMule is an interface to the eD2k and Kad networks. The aMule
developers have no control over what other peers transfer through this
medium and cannot be held liable for non-personal copyright
infringement or other illegal activity by third parties. Share
responsibly.

### Server-list sources

`addresses.dat` in the aMule configuration directory holds an ordered list of
HTTP or HTTPS URLs for `server.met` files, one per line. Blank lines and lines
beginning with `#` are ignored, as are unsupported URLs and duplicate entries.
aMule reads the file when an update sequence begins, so edits apply to the next
sequence without restarting. It does not create or rewrite this file or install
a default backup source.

The existing automatic server-list update option downloads and merges all valid
sources in order. Local users can edit the file through the Servers preferences
editor. For a daemon, edit the file in the daemon's configuration directory;
remote clients do not edit that file through this setting.

### Server-list download fallback

A manual or first-run download tries the requested URL first. If the URL is
invalid, the request fails, or the downloaded list is invalid or truncated,
aMule tries the valid
`addresses.dat` entries in order until a complete list is imported. This also
applies to explicitly entered URLs and requests from remote clients. Each
source is tried at most once; the requested URL is skipped if it also appears
in the file. Existing server entries are preserved when a download is invalid.

A successful download, a not-modified response, or cancellation ends the
sequence. An empty or absent `addresses.dat` disables additional attempts.
Edits made during a download apply to the next sequence; overlapping manual
requests do not replace the active sequence. Startup automatic updates continue
to process their own source list independently.

The former proposed `Ed2kServersFallbackUrl` setting is no longer used. If you
used an earlier development build with that setting, put its intended backup
URL in `addresses.dat` instead. No new backup URL is installed by default.
