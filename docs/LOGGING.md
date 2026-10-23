# Logging format

Core logs and the console logger use UTF-8 text with an explicit severity:

```text
2026-10-18T17:42:31.123+08:00 INFO  Disconnected from eD2k
2026-10-18T17:42:31.124+08:00 WARN  [wx] A warning
```

The timestamp uses the emitting process's local time with milliseconds and a
numeric UTC offset (`+08:00` in this example). This keeps desktop times familiar
and makes server logs unambiguous, including across daylight-saving transitions.
Millisecond precision distinguishes bursts of messages within the same second.
The instant is captured before the core logger queues a worker-thread message.
Remote clients display the server's time and offset. Each physical message line,
including interior blank lines, receives a prefix. An entirely blank record keeps
the legacy blank-line behavior. Severity names occupy five columns; ordinary core
messages have no subsystem tag. Named debug categories and explicit subsystems
retain their tags.

The levels are TRACE, DEBUG, INFO, WARN and ERROR. This change does not add a
runtime level threshold or enable debug traces in release builds.

Severity, category and status-bar visibility are independent. Existing
`AddLogLineC` and `AddDebugLogLineC` calls remain INFO until their severity is
explicitly classified: their historical C flag requests a status-bar update,
not a warning. Noncritical debug messages use DEBUG and retain their existing
category checks. New code can use `CLogger::Log` with an explicit category and
severity, or `AddLogLineWarning` / `AddLogLineError`. These macros do not request
a status-bar update. wxWidgets records retain their native severity and warnings
and errors continue to update the status bar.

The core logfile and GUI/EC delivery retain their historical first byte:
space for ordinary messages, `!` for status-bar messages and `.` for logfile-only
messages. Logfile-only messages remain excluded from EC replay. Human stdout
omits this byte. Existing remote GUIs can continue stripping the marker.

The standalone console logger shares the formatter. Its category identifies the
program (`amulecmd`, `amuleweb` or `amuleapi`). The connector initializes it once
from its declared program name before starting workers, so renaming the executable
or invoking it through a symlink does not change the tag;
explicit `CLogger::Log` calls can supply a subsystem. Debug builds retain source
file/line details in legacy messages.

## Follow-up work

The amuleapi descriptor tee still timestamps arbitrary stdout/stderr, including
startup messages and crash output. Direct diagnostics use the shared formatter
with explicit WARN/ERROR severity,
and the wx log target preserves native severity and emission time. Raw descriptor
output receives the shared INFO prefix and the `amuleapi` tag. The tee recognizes
the shared local-time prefix with its numeric offset across pipe-read boundaries
to avoid double timestamps on console logger records, while preserving
raw-output capture and the crash descriptor. Rotation, the EC protocol and crash output remain unchanged.

Runtime severity/category controls, classifying legacy status messages, optional
JSON Lines output, and core logfile rotation can be introduced separately.

## Review sequence for 3.2.0

The format and sequence are proposed in #1441 and accepted by one reviewer. It
changes GUI, status-bar, Web UI, EC/API and file output, and belongs to 3.2.0.
The proposed sequence is to review severity/format independently, then add
runtime debug categories in Release with a benchmark against the current Release
build with every category disabled. That follow-up must keep message construction
behind the category check, preserve existing debug-only diagnostic blocks, and
measure the disabled path before enabling it in production. It should include
thread-safe category controls and their preferences/EC integration. This PR makes
no claim about the performance of runtime categories. Final maintainer agreement
remains pending.
