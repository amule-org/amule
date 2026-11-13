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

#include "Logger.h"

#include <common/MuleDebug.h>       // Needed for ReserveCrashFd
#include <common/StringFunctions.h> // Needed for char2unicode
#include "amule.h"
#include <common/Macros.h>
#include <common/MacrosProgramSpecific.h>
#include <sstream>
#include <wx/tokenzr.h>
#include <wx/wfstream.h>
#include <wx/sstream.h>
#include <wx/filename.h>

wxDEFINE_EVENT(MULE_EVT_LOGLINE, wxEvent);

CDebugCategory g_debugcats[] = { CDebugCategory(logGeneral, "General"),
	CDebugCategory(logHasher, "Hasher"),
	CDebugCategory(logClient, "ED2k Client"),
	CDebugCategory(logLocalClient, "Local Client Protocol"),
	CDebugCategory(logRemoteClient, "Remote Client Protocol"),
	CDebugCategory(logPacketErrors, "Packet Parsing Errors"),
	CDebugCategory(logCFile, "CFile"),
	CDebugCategory(logFileIO, "FileIO"),
	CDebugCategory(logZLib, "ZLib"),
	CDebugCategory(logAICHThread, "AICH-Hasher"),
	CDebugCategory(logAICHTransfer, "AICH-Transfer"),
	CDebugCategory(logAICHRecovery, "AICH-Recovery"),
	CDebugCategory(logListenSocket, "ListenSocket"),
	CDebugCategory(logCredits, "Credits"),
	CDebugCategory(logClientUDP, "ClientUDPSocket"),
	CDebugCategory(logDownloadQueue, "DownloadQueue"),
	CDebugCategory(logIPFilter, "IPFilter"),
	CDebugCategory(logKnownFiles, "KnownFileList"),
	CDebugCategory(logPartFile, "PartFiles"),
	CDebugCategory(logSHAHashSet, "SHAHashSet"),
	CDebugCategory(logServer, "Servers"),
	CDebugCategory(logProxy, "Proxy"),
	CDebugCategory(logSearch, "Searching"),
	CDebugCategory(logServerUDP, "ServerUDP"),
	CDebugCategory(logClientKadUDP, "Client Kademlia UDP"),
	CDebugCategory(logKadSearch, "Kademlia Search"),
	CDebugCategory(logKadRouting, "Kademlia Routing"),
	CDebugCategory(logKadIndex, "Kademlia Indexing"),
	CDebugCategory(logKadMain, "Kademlia Main Thread"),
	CDebugCategory(logKadPrefs, "Kademlia Preferences"),
	CDebugCategory(logPfConvert, "PartFileConvert"),
	CDebugCategory(logMuleUDP, "MuleUDPSocket"),
	CDebugCategory(logThreads, "ThreadScheduler"),
	CDebugCategory(logUPnP, "Universal Plug and Play"),
	CDebugCategory(logKadUdpFwTester, "Kademlia UDP Firewall Tester"),
	CDebugCategory(logKadPacketTracking, "Kademlia Packet Tracking"),
	CDebugCategory(logKadEntryTracking, "Kademlia Entry Tracking"),
	CDebugCategory(logKadNodeTracking, "Kademlia Node Tracking"),
	CDebugCategory(logKadBuddy, "Kademlia Buddy"),
	CDebugCategory(logEC, "External Connect"),
	CDebugCategory(logHTTP, "HTTP"),
	CDebugCategory(logAsio, "Asio Sockets"),
	CDebugCategory(logMediaProbe, "Media Probe"),
	CDebugCategory(logVerifyLocalData, "Verify Local Data"),
	CDebugCategory(logGUI, "GUI"),
	CDebugCategory(logKadPublish, "Kademlia Publish"),
	CDebugCategory(logSourceRoutes, "Source Routes") };

const int categoryCount = itemsof(g_debugcats);
static_assert(categoryCount == logCategoryCount, "Update the debug category table");

void CLogger::SetEnabled(DebugType type, bool enabled)
{
	int index = (int)type;

	if (index >= 0 && index < categoryCount) {
		CDebugCategory &cat = g_debugcats[index];
		wxASSERT(type == cat.GetType());

		cat.SetEnabled(enabled);
		m_debugEnabled[index].store(enabled && m_verbose, std::memory_order_relaxed);
	} else {
		wxFAIL;
	}
}

void CLogger::SetVerbose(bool verbose)
{
	m_verbose = verbose;
	for (int i = 0; i < categoryCount; ++i) {
		m_debugEnabled[i].store(verbose && g_debugcats[i].IsEnabled(), std::memory_order_relaxed);
	}
}

void CLogger::AddLogLine(const wxString &DEBUG_ONLY(file),
	int DEBUG_ONLY(line),
	bool critical,
	DebugType type,
	const wxString &str,
	bool toStdout,
	bool toGUI)
{
#ifdef __DEBUG__
	wxString msg(str);
#else
	const wxString &msg = str;
#endif
	wxString category =
#ifdef CLIENT_GUI
		"remote-GUI";
#else
		"";
#endif
	// handle Debug messages
	if (type != logStandard) {
		if (!critical && !IsEnabled(type)) {
			return;
		}
		if (!critical && m_verboseLogfile.load(std::memory_order_relaxed)) {
			// print non critical debug messages only to the logfile
			toGUI = false;
		}
		int index = (int)type;

		if (index >= 0 && index < categoryCount) {
			const CDebugCategory &cat = g_debugcats[index];
			wxASSERT(type == cat.GetType());

			category = cat.GetName();
		} else {
			wxFAIL;
		}
	}

#ifdef __DEBUG__
	if (line) {
		msg = file.AfterLast(wxFileName::GetPathSeparator()).AfterLast('/')
		      << "(" << line << "): " + msg;
	}
#endif

	// Legacy C means status-bar visibility, not severity. Preserve it as INFO
	// until call sites are explicitly classified; ordinary debug traces are DEBUG.
	Log(type != logStandard && !critical ? LogLevel::Debug : LogLevel::Info,
		category,
		msg,
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

void CLogger::Log(const CLogRecord &record, bool toStatusBar, bool toStdout, bool toGUI)
{
	if (toGUI && !wxThread::IsMain()) {
		CLoggingEvent event(toStatusBar, toStdout, toGUI, record);
		AddPendingEvent(event);
	} else {
		DoLines(record, toStatusBar, toStdout, toGUI);
	}
}

void CLogger::AddLogLine(
	const wxString &file, int line, bool critical, DebugType type, const std::ostringstream &msg)
{
	AddLogLine(file, line, critical, type, wxString(char2unicode(msg.str().c_str())));
}

const CDebugCategory &CLogger::GetDebugCategory(int index)
{
	// wxCHECK rather than wxASSERT so a release build returns a safe fallback on out-of-range
	// instead of reading past the array; the debug-build assert + abort is unchanged.
	wxCHECK_MSG(index >= 0 && index < categoryCount,
		g_debugcats[0],
		"CLogger::GetDebugCategory: index out of range");

	return g_debugcats[index];
}

unsigned int CLogger::GetDebugCategoryCount()
{
	return categoryCount;
}

bool CLogger::OpenLogfile(const wxString &name, bool append)
{
	applog = new wxFFileOutputStream(name, append ? "ab" : "wb");
	bool ret = applog->Ok();
	if (ret) {
		FlushApplog();
		m_LogfileName = name;
		// A daemon's stderr is /dev/null, so the abort handler needs a real file to write to.
		// Reserved rather than looked up on demand: it runs from a signal handler.
		m_crashFd = ReserveCrashFd(m_crashFd, applog->GetFile()->fp());
	} else {
		CloseLogfile();
	}
	return ret;
}

void CLogger::CloseLogfile()
{
	delete applog;
	applog = NULL;
	m_LogfileName.Clear();
}

void CLogger::OnLoggingEvent(class CLoggingEvent &evt)
{
	DoLines(evt.Record(), evt.IsCritical(), evt.ToStdout(), evt.ToGUI());
}

void CLogger::DoLines(const CLogRecord &record, bool critical, bool toStdout, bool toGUI)
{
	const wxString lines = FormatLogLines(record);
	wxStringTokenizer tokens(lines, "\n", wxTOKEN_RET_EMPTY);
	while (tokens.HasMoreTokens()) {
		DoLine(tokens.GetNextToken() + "\n", critical, toStdout, toGUI);
	}
}

void CLogger::DoLine(const wxString &line, bool toStatusBar, bool toStdout, bool toGUI)
{
	{
		wxMutexLocker lock(m_lineLock);
		++m_count;

		m_ApplogBuf += EncodeLogFileLine(line, toStatusBar, toGUI);
		FlushApplog();

		if (m_StdoutLog || toStdout) {
			// Use UTF-8 even in the C locale, as on disk.
			printf("%s", static_cast<const char *>(line.utf8_str()));
			fflush(stdout);
		}
	}
#ifndef AMULE_DAEMON
	if (toGUI) {
		theApp->AddGuiLogLine(
			wxString(static_cast<wxChar>(LegacyLogMarker(toStatusBar, toGUI))) + line);
	}
#endif
}

void CLogger::EmergencyLog(const wxString &message, bool closeLog)
{
	const wxString lines = FormatLogLines(CLogRecord(LogLevel::Error, "crash", message));
	fprintf(stderr, "%s", static_cast<const char *>(lines.utf8_str()));
	// Do not queue or reacquire the logging mutex on this emergency path.
	wxStringTokenizer tokens(lines, "\n", wxTOKEN_RET_EMPTY);
	while (tokens.HasMoreTokens()) {
		m_ApplogBuf += EncodeLogFileLine(tokens.GetNextToken() + "\n", true, true);
	}
	FlushApplog();
	if (closeLog && applog) {
		applog->Close();
		delete applog;
		applog = nullptr;
	}
}

void CLogger::FlushApplog()
{
	if (applog) { // Wait with output until logfile is actually opened
		wxStringInputStream stream(m_ApplogBuf);
		(*applog) << stream;
		applog->Sync();
		m_ApplogBuf.Clear();
	}
}

CLogger theLogger;

wxBEGIN_EVENT_TABLE(CLogger, wxEvtHandler)
	EVT_MULE_LOGGING(CLogger::OnLoggingEvent)
wxEND_EVENT_TABLE()

CLoggerTarget::CLoggerTarget() {}

void CLoggerTarget::DoLogRecord(wxLogLevel level, const wxString &msg, const wxLogRecordInfo &info)
{
	// A thread-local guard does not suppress unrelated messages on other threads.
	static thread_local bool recursion = false;
	if (recursion) {
		return;
	}
	recursion = true;
	theLogger.Log(RecordFromWx(level, "wx", msg, info),
		level == wxLOG_Error || level == wxLOG_FatalError || level == wxLOG_Warning);
	recursion = false;
}

CLoggerAccess::CLoggerAccess()
{
	m_bufferlen = 4096;
	m_buffer = new wxCharBuffer(m_bufferlen);
	m_logfile = NULL;
	Reset();
}

void CLoggerAccess::Reset()
{
	delete m_logfile;
	m_logfile = new wxFFileInputStream(theLogger.GetLogfileName());
	m_pos = 0;
	m_ready = false;
	m_replayLine.clear();
}

CLoggerAccess::~CLoggerAccess()
{
	delete m_buffer;
	delete m_logfile;
}

// Read a line of text from the logfile if available. (Can't believe there's no library function
// for this >:( )
bool CLoggerAccess::HasString()
{
	while (!m_ready) {
		int c = m_logfile->GetC();
		if (c == wxEOF) {
			// wxFFileInputStream wraps a stdio FILE* whose EOF flag is sticky: once
			// GetC() returns wxEOF, later reads keep returning it even after amuled has
			// appended more lines. The old fix re-seeked to the current position to
			// clear it, but wx 3.3 short-circuits a seek to the current offset, so EOF
			// is never cleared and EC clients stop seeing new lines after the batch
			// captured at connect time. Reopening gives a stream with no EOF set, and
			// seeking back resumes at the first newly appended byte.
			wxFileOffset pos = m_logfile->TellI();
			if (pos != wxInvalidOffset) {
				delete m_logfile;
				m_logfile = new wxFFileInputStream(theLogger.GetLogfileName());
				m_logfile->SeekI(pos, wxFromStart);
			}
			break;
		}
		// check for buffer overrun
		if (m_pos == m_bufferlen) {
			m_bufferlen += 1024;
			m_buffer->extend(m_bufferlen);
		}
		m_buffer->data()[m_pos++] = c;
		if (c == '\n') {
			m_ready = DecodeLogFileLine(
				wxString(m_buffer->data(), wxConvUTF8, m_pos), m_replayLine);
			m_pos = 0;
		}
	}
	return m_ready;
}

bool CLoggerAccess::GetString(wxString &s)
{
	if (!HasString()) {
		return false;
	}
	s = m_replayLine;
	m_replayLine.clear();
	m_ready = false;
	return true;
}

// Functions for EC logging

#ifdef __DEBUG__
#include "ec/cpp/ECLog.h"

bool ECLogIsEnabled()
{
	return theLogger.IsEnabled(logEC);
}

void DoECLogLine(const wxString &line)
{
	// without file/line
	theLogger.AddLogLine("", 0, false, logStandard, line, false, false);
}

#endif /* __DEBUG__ */
// File_checked_for_headers
