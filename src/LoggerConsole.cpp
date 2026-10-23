//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Marcelo Roberto Jimenez ( phoenix@amule.org )
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

#include <sstream>
#include <mutex>
#include <common/StringFunctions.h> // for char2unicode()

#include "Logger.h" // for Add(Debug)LogLineM()

wxDEFINE_EVENT(MULE_EVT_LOGLINE, wxEvent);

#ifdef __DEBUG__

// Console-binary verbose-debug gate. Driven by /eMule/VerboseDebug from amule.conf (read by
// CaMuleExternalConnector::LoadAmuleConfig) and by the --verbose CLI flag, both of which call
// CLogger::SetVerbose below. Default off, matching amuled when VerboseDebug is unset.
static bool s_consoleVerbose = false;

bool CLogger::IsEnabled(DebugType /*type*/) const
{
	return s_consoleVerbose;
}

// Dummy functions for EC logging
#include "ec/cpp/ECLog.h"

bool ECLogIsEnabled()
{
	return false;
}
void DoECLogLine(const wxString &) {}

#endif /* __DEBUG__ */

void CLogger::SetVerbose(bool verbose)
{
#ifdef __DEBUG__
	s_consoleVerbose = verbose;
#else
	(void)verbose;
#endif
}

void CLogger::AddLogLine(const wxString &file, int line, bool critical, const std::ostringstream &msg)
{
	AddLogLine(file, line, critical, logStandard, msg);
}

void CLogger::AddLogLine(const wxString &file,
	int line,
	bool critical,
	DebugType type,
	const wxString &str,
	bool toStdout,
	bool toGUI)
{
#ifdef __DEBUG__
	wxString message(str);
	message = file + wxString::Format("(%d): ", line) + message;
#else
	const wxString &message = str;
	(void)file;
	(void)line;
#endif
	Log(type != logStandard && !critical ? LogLevel::Debug : LogLevel::Info,
		m_programName,
		message,
		critical,
		toStdout,
		toGUI);
}

void CLogger::Log(LogLevel level,
	const wxString &category,
	const wxString &message,
	bool toStatusBar,
	bool toStdout,
	bool toGUI)
{
	Log(CLogRecord(level, category, message), toStatusBar, toStdout, toGUI);
}

void CLogger::Log(const CLogRecord &record, bool, bool, bool)
{
	const wxString rendered = FormatLogLines(record);
	const wxScopedCharBuffer text = rendered.utf8_str();
	// Write each complete record under one lock, including multiline messages.
	static std::mutex mutex;
	const std::lock_guard<std::mutex> lock(mutex);
	std::fwrite(text.data(), 1, text.length(), stdout);
	std::fflush(stdout);
}

void CLogger::AddLogLine(
	const wxString &file, int line, bool critical, DebugType type, const std::ostringstream &msg)
{
	AddLogLine(file, line, critical, type, wxString(char2unicode(msg.str().c_str())));
}

CLogger theLogger;

wxBEGIN_EVENT_TABLE(CLogger, wxEvtHandler)
wxEND_EVENT_TABLE()
