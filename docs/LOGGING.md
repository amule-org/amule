# Logging format

Core logs and the console logger use UTF-8 text with an explicit severity:

```text
2026-10-18T17:42:31.123+08:00 INFO  Disconnected from eD2k
2026-10-18T17:42:31.124+08:00 WARN  [wx] A warning
```

The timestamp uses the local time of the program that writes the record, with
milliseconds and a numeric UTC offset (`+08:00` in this example). Desktop users see
familiar times, and server logs stay unambiguous, also across daylight-saving
changes. Milliseconds tell apart messages written within the same second. The time
is taken when the message is logged, before the core queues it from a worker thread.
Remote clients show the server's time and offset. Every physical line gets a prefix,
including blank lines inside a message and records that are entirely blank.

The GUI status bar has less room. It shows the first line of a status message
with a compact `2026-10-18 17:42:31` timestamp and no severity. Its tooltip keeps
the full line.

The severity takes five columns. The levels are TRACE, DEBUG, INFO, WARN and ERROR.
Ordinary core messages have no subsystem tag; named debug categories and explicit
subsystems keep their tags. There is no runtime level threshold; debug categories are
turned on as described in [Debug logging](README.md#debug-logging).

Severity, category and status-bar visibility are independent. `AddLogLineC` and
`AddDebugLogLineC` calls stay INFO until each one gets an explicit severity: their
C flag asks for a status-bar update, it does not mark a warning. Before this format,
stdout marked these lines with a leading `!`. They are now plain INFO lines on
stdout, so a server operator cannot pick them out there; the logfile still marks
them with `status=1`. Non-critical debug messages use DEBUG and keep their category
checks. New code can call `CLogger::Log` with an explicit category and severity.
wxWidgets records keep their own severity, and their warnings and errors still
update the status bar.

## Logfile

Each logfile record starts with the timestamp. After the severity, a reserved field
holds the status-bar and EC replay flags:

```text
2026-10-18T17:42:31.123+08:00 INFO  [log status=1 replay=1] Disconnected from eD2k
2026-10-18T17:42:31.124+08:00 DEBUG [log status=0 replay=0] [FileIO] File detail
```

`status=1` asks for a status-bar update. `replay=0` keeps the line out of the
incremental GUI replay; full-log and debug snapshots still include it with the
historical `.` marker. Every record has the field before its category and message,
so text inside a message cannot pose as flags. The flags stay with the record
through append, backup and reset, without a separate file. Stdout and GUI text do
not show the field.

For GUI and EC delivery only, the logger puts back the historical first byte: a
space for ordinary messages and `!` for status-bar messages. Older remote GUIs keep
stripping that byte, so the EC protocol does not change. Replay also reads logfiles
from before this format, including `.` logfile-only lines, because a failed backup
can leave old and new records in the same file. The `amulecmd` log command, the Web
UI log output and the API log strings remove the EC marker, so their records also
start with the timestamp.

EmergencyLog records use ERROR and the `crash` category, with the timestamp first in
the logfile and on stderr. Low-level fatal signal reports still write directly to
their reserved descriptors, because a signal handler cannot safely use wxWidgets
formatting. Their raw backtrace text stays readable over EC without losing its first
byte.

## Console programs

`amulecmd` and `amuleapi` share the formatter. The category names the
program. It is set once, from the name each program declares, before any worker
starts, so renaming the executable or starting it through a symlink does not change
the tag. Explicit `CLogger::Log` calls can supply a subsystem. Debug builds keep the
source file and line in legacy messages.

amuleapi passes its stdout and stderr through a tee that also writes its logfile.
Its own diagnostics use the shared formatter with an explicit severity, and its wx
log target keeps wx's severity and emission time. Any other raw output, such as
startup messages or crash output, gets an INFO prefix with the `amuleapi` tag. The
tee recognizes lines that already carry the shared prefix, also when a read splits
them, so they do not get a second timestamp.
