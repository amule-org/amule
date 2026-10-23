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

#include <wx/datetime.h>
#include <wx/string.h>
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

inline char LegacyLogMarker(bool toStatusBar, bool toGUI)
{
	return !toGUI ? '.' : (toStatusBar ? '!' : ' ');
}

inline wxString FormatLogPrefix(const CLogRecord &record)
{
	// wx returns the basic numeric offset; RFC 3339 uses a colon between hours and minutes.
	wxString offset = record.timestamp.Format("%z");
	offset.insert(3, ":");
	return record.timestamp.Format("%Y-%m-%dT%H:%M:%S") +
	       wxString::Format(".%03u", static_cast<unsigned>(record.timestamp.GetMillisecond())) + offset +
	       " " + LogLevelName(record.level) + " " +
	       (record.category.IsEmpty() ? wxString() : "[" + record.category + "] ");
}

// Render at the sink, using the emission time retained in the record. Every physical
// line has its own prefix; a blank record remains a blank line, as in the old logger.
inline wxString FormatLogLines(const CLogRecord &record)
{
	const wxString text = record.message.Strip(wxString::trailing);
	if (text.IsEmpty()) {
		return "\n";
	}
	const wxString prefix = FormatLogPrefix(record);
	wxString result;
	wxStringTokenizer tokens(text, "\n", wxTOKEN_RET_EMPTY);
	while (tokens.HasMoreTokens()) {
		result += prefix + tokens.GetNextToken() + "\n";
	}
	return result;
}

#endif
