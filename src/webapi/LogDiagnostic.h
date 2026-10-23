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

#ifndef WEBAPI_LOG_DIAGNOSTIC_H
#define WEBAPI_LOG_DIAGNOSTIC_H

#include "../LogRecord.h"
#include <wx/log.h>
#include <cstdio>
#include <mutex>
#include <sstream>

namespace webapi
{

inline void WriteDiagnostic(const CLogRecord &record)
{
	const wxString text = FormatLogLines(record);
	const wxScopedCharBuffer bytes = text.utf8_str();
	static std::mutex mutex;
	const std::lock_guard<std::mutex> lock(mutex);
	std::fwrite(bytes.data(), 1, bytes.length(), stderr);
	std::fflush(stderr);
}

// Keep wx severity and emission time rather than parsing localized prefixes.
class CApiLogTarget : public wxLog
{
protected:
	void DoLogRecord(wxLogLevel level, const wxString &message, const wxLogRecordInfo &info) override
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
		WriteDiagnostic(CLogRecord(severity, "amuleapi", message, timestamp));
	}
};

// Collect one diagnostic before writing it, so concurrent API workers cannot
// interleave prefixes and payloads. The descriptor tee recognizes this format.
class CLogDiagnostic
{
public:
	explicit CLogDiagnostic(LogLevel level)
	: m_level(level)
	{
	}
	~CLogDiagnostic()
	{
		WriteDiagnostic(CLogRecord(
			m_level, "amuleapi", wxString::FromUTF8(m_message.str().c_str()), m_timestamp));
	}

	template <typename T> CLogDiagnostic &operator<<(const T &value)
	{
		m_message << value;
		return *this;
	}

private:
	LogLevel m_level;
	wxDateTime m_timestamp = wxDateTime::UNow();
	std::ostringstream m_message;
};

} // namespace webapi
#endif
