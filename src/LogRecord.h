//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef LOG_RECORD_H
#define LOG_RECORD_H

#include <cstddef>
#include <initializer_list>
#include <wx/datetime.h>
#include <wx/log.h>
#include <wx/string.h>
#include <wx/time.h>
#include <wx/tokenzr.h>

// Severity is independent of category, status-bar visibility and EC replay eligibility.
enum class LogLevel
{
	Trace,
	Debug,
	Info,
	Warning,
	Error
};

inline const char *LogLevelName(LogLevel level)
{
	switch (level) {
	case LogLevel::Trace:
		return "TRACE";
	case LogLevel::Debug:
		return "DEBUG";
	case LogLevel::Info:
		return "INFO ";
	case LogLevel::Warning:
		return "WARN ";
	case LogLevel::Error:
		return "ERROR";
	}
	return "INFO ";
}

struct CLogRecord
{
	CLogRecord(LogLevel severity,
		const wxString &subsystem,
		const wxString &text,
		const wxDateTime &time = wxDateTime::UNow())
	: level(severity)
	, category(subsystem.c_str(), subsystem.Length())
	, message(text.c_str(), text.Length())
	, timestamp(time)
	{
	}

	// wx event clones may cross threads: copy string storage, not reference counts.
	CLogRecord(const CLogRecord &other)
	: CLogRecord(other.level, other.category, other.message, other.timestamp)
	{
	}

	LogLevel level;
	wxString category;
	wxString message;
	wxDateTime timestamp;
};

// A wx log call keeps its native severity and the emission time wx captured.
inline CLogRecord RecordFromWx(
	wxLogLevel level, const wxString &category, const wxString &message, const wxLogRecordInfo &info)
{
	LogLevel severity = LogLevel::Info;
	if (level == wxLOG_Error || level == wxLOG_FatalError) {
		severity = LogLevel::Error;
	} else if (level == wxLOG_Warning) {
		severity = LogLevel::Warning;
	} else if (level == wxLOG_Debug) {
		severity = LogLevel::Debug;
	} else if (level == wxLOG_Trace) {
		severity = LogLevel::Trace;
	}
	wxDateTime timestamp;
	timestamp.Set(static_cast<time_t>(info.timestampMS / 1000));
	timestamp.SetMillisecond(static_cast<wxDateTime::wxDateTime_t>(info.timestampMS % 1000));
	return CLogRecord(severity, category, message, timestamp);
}

inline char LegacyLogMarker(bool toStatusBar, bool toGUI)
{
	return !toGUI ? '.' : (toStatusBar ? '!' : ' ');
}

// RFC 3339 offset such as "+05:30". Computed from the C runtime's broken-down times, not
// strftime's %z, whose output varies between runtimes and would break the fixed header width.
inline wxString FormatUtcOffset(const wxDateTime &time)
{
	const time_t ticks = time.GetTicks();
	struct tm local;
	struct tm utc;
	if (!wxLocaltime_r(&ticks, &local) || !wxGmtime_r(&ticks, &utc)) {
		return "+00:00";
	}
	// The two views are at most a day apart, including across a year boundary.
	const long days = local.tm_year != utc.tm_year ? (local.tm_year > utc.tm_year ? 1 : -1)
						       : local.tm_yday - utc.tm_yday;
	long minutes = days * 1440 + (local.tm_hour - utc.tm_hour) * 60L + (local.tm_min - utc.tm_min);
	const char sign = minutes < 0 ? '-' : '+';
	minutes = minutes < 0 ? -minutes : minutes;
	return wxString::Format("%c%02ld:%02ld", sign, minutes / 60, minutes % 60);
}

inline wxString FormatLogPrefix(const CLogRecord &record)
{
	return record.timestamp.Format("%Y-%m-%dT%H:%M:%S") +
	       wxString::Format(".%03u", static_cast<unsigned>(record.timestamp.GetMillisecond())) +
	       FormatUtcOffset(record.timestamp) + " " + LogLevelName(record.level) + " " +
	       (record.category.IsEmpty() ? wxString() : "[" + record.category + "] ");
}

// Render at the sink, using the emission time retained in the record. Every physical
// line has its own prefix, including a record containing only whitespace.
inline wxString FormatLogLines(const CLogRecord &record)
{
	wxString text = record.message;
	text.Replace("\r\n", "\n");
	text.Replace("\r", "\n");
	text = text.Strip(wxString::trailing);
	const wxString prefix = FormatLogPrefix(record);
	if (text.IsEmpty()) {
		return prefix + "\n";
	}
	wxString result;
	wxStringTokenizer tokens(text, "\n", wxTOKEN_RET_EMPTY);
	while (tokens.HasMoreTokens()) {
		result += prefix + tokens.GetNextToken() + "\n";
	}
	return result;
}

// On disk, persist display/replay policy after the timestamp and severity. Always
// write the field, so message text resembling metadata cannot change EC behavior.
// stdout and GUI/EC retain only the human record; no sidecar needs to follow rotation.
inline wxString LogFileFlags(bool toStatusBar, bool toGUI)
{
	return wxString::Format("[log status=%d replay=%d] ", toStatusBar ? 1 : 0, toGUI ? 1 : 0);
}

// RFC 3339 timestamp (29 chars), space, padded severity (5 chars), space.
inline constexpr size_t LogRecordHeaderLength = 36;

inline wxString EncodeLogFileLine(const wxString &line, bool toStatusBar, bool toGUI)
{
	return line.Left(LogRecordHeaderLength) + LogFileFlags(toStatusBar, toGUI) +
	       line.Mid(LogRecordHeaderLength);
}

inline bool HasLogRecordHeader(const wxString &line)
{
	static constexpr char pattern[] = "0000-00-00T00:00:00.000+00:00 ";
	if (line.length() < LogRecordHeaderLength) {
		return false;
	}
	for (size_t i = 0; i < sizeof(pattern) - 1; ++i) {
		const wxUniChar c = line[i];
		if (pattern[i] == '0' ? (c < '0' || c > '9')
				      : (pattern[i] == '+' ? (c != '+' && c != '-') : c != pattern[i])) {
			return false;
		}
	}
	const wxString level = line.Mid(sizeof(pattern) - 1, 6);
	return level == "TRACE " || level == "DEBUG " || level == "INFO  " || level == "WARN  " ||
	       level == "ERROR ";
}

// The status bar has no room for milliseconds, UTC offset and severity; the log
// view, logfile and EC keep them.
inline wxString FormatStatusBarLogLine(const wxString &line)
{
	if (!HasLogRecordHeader(line)) {
		return line;
	}
	return line.Left(10) + " " + line.Mid(11, 8) + " " + line.Mid(LogRecordHeaderLength);
}

// Accept pre-upgrade marker files too: backup failure intentionally appends to
// the existing logfile. Unstructured crash output must not lose its first byte.
inline bool DecodeLogFileLine(const wxString &line, wxString &ecLine, bool includeLogfileOnly = false)
{
	ecLine.clear();
	if (line.StartsWith(".") && !includeLogfileOnly) {
		return false;
	}
	if (line.StartsWith("!") || line.StartsWith(" ") || line.StartsWith(".")) {
		ecLine = line;
		return true;
	}
	if (HasLogRecordHeader(line)) {
		for (const bool status : { false, true }) {
			for (const bool replay : { false, true }) {
				const wxString flags = LogFileFlags(status, replay);
				if (line.Mid(LogRecordHeaderLength).StartsWith(flags)) {
					if (!replay && !includeLogfileOnly) {
						return false;
					}
					ecLine = wxString(static_cast<wxChar>(
							 LegacyLogMarker(status, replay))) +
						 line.Left(LogRecordHeaderLength) +
						 line.Mid(LogRecordHeaderLength + flags.length());
					return true;
				}
			}
		}
	}
	ecLine = " " + line;
	return true;
}

// Full-log requests historically include logfile-only records with a '.' marker,
// while incremental CLoggerAccess replay skips them. Preserve both contracts.
inline wxString LogFileToEC(const wxString &text)
{
	wxString result;
	size_t start = 0;
	while (start < text.length()) {
		const size_t end = text.find('\n', start);
		const size_t length = end == wxString::npos ? text.length() - start : end - start + 1;
		wxString line;
		DecodeLogFileLine(text.Mid(start, length), line, true);
		result += line;
		start += length;
	}
	return result;
}

#endif
