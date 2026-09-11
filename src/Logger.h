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

#ifndef LOGGER_H
#define LOGGER_H

#include <wx/log.h>
#include <wx/event.h>
#include <iosfwd>

enum DebugType
{
	//! Standard warning, not debug
	logStandard = -1,
	//! General warnings/errors.
	logGeneral = 0,
	//! Warnings/Errors for the main hashing thread.
	logHasher,
	//! Warnings/Errors for client-objects.
	logClient,
	//! Warnings/Errors for the local client protocol.
	logLocalClient,
	//! Warnings/Errors for the remote client protocol.
	logRemoteClient,
	//! Warnings/Errors when parsing packets.
	logPacketErrors,
	//! Warnings/Errors for the CFile class.
	logCFile,
	//! Warnings/Errors related to reading/writing files.
	logFileIO,
	//! Warnings/Errors when using the zLib library.
	logZLib,
	//! Warnings/Errors for the AICH-syncronization thread.
	logAICHThread,
	//! Warnings/Errors for transferring AICH hash-sets.
	logAICHTransfer,
	//! Warnings/Errors when recovering with AICH.
	logAICHRecovery,
	//! Warnings/Errors for the CListenSocket class.
	logListenSocket,
	//! Warnings/Errors for Client-Credits.
	logCredits,
	//! Warnings/Errors for the client UDP socket.
	logClientUDP,
	//! Warnings/Errors for the download-queue.
	logDownloadQueue,
	//! Warnings/Errors for the IP-Filter.
	logIPFilter,
	//! Warnings/Errors for known-files.
	logKnownFiles,
	//! Warnings/Errors for part-files.
	logPartFile,
	//! Warnings/Errors for SHA-hashset creation.
	logSHAHashSet,
	//! Warnings/Errors for servers, server connections.
	logServer,
	//! Warnings/Errors for proxy.
	logProxy,
	//! Warnings/Errors related to searching.
	logSearch,
	//! Warnings/Errors related to the server UDP socket.
	logServerUDP,
	//! Warning/Errors related to Kademlia UDP communication on client
	logClientKadUDP,
	//! Warning/Errors related to Kademlia Search
	logKadSearch,
	//! Warning/Errors related to Kademlia Routing
	logKadRouting,
	//! Warning/Errors related to Kademlia Indexing
	logKadIndex,
	//! Warning/Errors related to Kademlia Main Thread
	logKadMain,
	//! Warning/Errors related to Kademlia Preferences
	logKadPrefs,
	//! Warnings/Errors related to partfile importer
	logPfConvert,
	//! Warnings/Errors related to the basic UDP socket-class.
	logMuleUDP,
	//! Warnings/Errors related to the thread-scheduler.
	logThreads,
	//! Warnings/Errors related to the Universal Plug and Play subsystem.
	logUPnP,
	//! Warnings/Errors related to the UDP Firewall Tester
	logKadUdpFwTester,
	//! Warnings/Errors related to Kad packet tracking.
	logKadPacketTracking,
	//! Warnings/Errors related to Kad entry tracking.
	logKadEntryTracking,
	//! Kad node tracking: identity rotation, problematic nodes and bans.
	logKadNodeTracking,
	//! Full log of external connection packets
	logEC,
	//! Warnings/Errors related to HTTP traffic
	logHTTP,
	//! Warnings/Errors related to Boost Asio networking
	logAsio,
	//! Media-metadata probing (ffprobe) subsystem.
	logMediaProbe,
	//! Warnings/Errors related to Verify Local Data subsystem
	logVerifyLocalData
	// IMPORTANT NOTE: when you add values to this enum, update the g_debugcats
	// array in Logger.cpp!
};

/// Container class for the debugging categories.
class CDebugCategory
{
public:
	/// @param type The debug-category type.
	/// @param name The user-readable name.
	CDebugCategory(DebugType type, const wxString &name)
	: m_name(name)
	, m_type(type)
	, m_enabled(false)
	{
	}

	/// True if the category is enabled.
	bool IsEnabled() const { return m_enabled; }

	/// Enables or disables the category.
	void SetEnabled(bool enabled) { m_enabled = enabled; }

	/// The user-readable name.
	const wxString &GetName() const { return m_name; }

	/// The category type.
	DebugType GetType() const { return m_type; }

private:
	//! The user-readable name.
	wxString m_name;
	//! The actual type.
	DebugType m_type;
	//! Whenever or not the category is enabled.
	bool m_enabled;
};

/// Functions for logging operations.
class CLogger : public wxEvtHandler
{
public:
	/// True if debug messages should be generated for this category.
#ifdef __DEBUG__
	bool IsEnabled(DebugType) const;
#else
	bool IsEnabled(DebugType) const { return false; }
#endif

	/// Enables or disables debug messages for a category.
	void SetEnabled(DebugType type, bool enabled);

	/// Sets the global verbose-debug flag that gates IsEnabled() per category. The amuled /
	/// monolithic build stores this in thePrefs; the console binaries (amuleweb, amulecmd) keep
	/// their own static, not linking CPreferences. ExternalConnector calls this with
	/// /eMule/VerboseDebug after loading amule.conf and again after parsing --verbose, so both
	/// paths drive the same gate.
	void SetVerbose(bool verbose);

	/// True if logging to stdout is enabled.
	bool IsEnabledStdoutLog() const { return m_StdoutLog; }

	/// Enables or disables logging to stdout.
	void SetEnabledStdoutLog(bool enabled) { m_StdoutLog = enabled; }

	/// Logs @a str prefixed with the name of @a type (except for logStandard). @a critical
	/// makes the message visible directly to the user. Thread-safe: from the main thread the
	/// event is sent straight to the application, otherwise it is queued in the event loop.
	void AddLogLine(const wxString &file,
		int line,
		bool critical,
		DebugType type,
		const wxString &str,
		bool toStdout = false,
		bool toGUI = true);

	// for UPnP
	void AddLogLine(
		const wxString &file, int line, bool critical, DebugType type, const std::ostringstream &msg);

	void AddLogLine(const wxString &file, int line, bool critical, const std::ostringstream &msg);

	/// Emergency log for crashes.
	void EmergencyLog(const wxString &message, bool closeLog = true);

	/// The category at @a index.
	const CDebugCategory &GetDebugCategory(int index);

	/// Number of debug categories.
	unsigned int GetDebugCategoryCount();

	/// Opens the logfile; true on success.
	bool OpenLogfile(const wxString &name);

	/// Closes the logfile.
	void CloseLogfile();

	/// Name of the logfile.
	const wxString &GetLogfileName() const { return m_LogfileName; }

	/// Event handler.
	void OnLoggingEvent(class CLoggingEvent &evt);

	CLogger()
	{
		applog = NULL;
		m_StdoutLog = false;
		m_count = 0;
	}

private:
	class wxFFileOutputStream *applog; // the logfile
	wxString m_LogfileName;
	wxString m_ApplogBuf;
	bool m_StdoutLog;
	int m_count; // output line counter
	wxMutex m_lineLock;

	/// Writes all waiting log info to the logfile.
	void FlushApplog();

	/// Outputs a single line.
	void DoLine(const wxString &line, bool toStdout, bool toGUI);

	/// Outputs several lines.
	void DoLines(const wxString &lines, bool critical, bool toStdout, bool toGUI);

	wxDECLARE_EVENT_TABLE();
};

extern CLogger theLogger;

/// Forwards log lines from wxWidgets to CLogger.
class CLoggerTarget : public wxLog
{
public:
	CLoggerTarget();

	/// @see wxLog::DoLogText
	void DoLogText(const wxString &msg);
};

wxDECLARE_EVENT(MULE_EVT_LOGLINE, wxEvent);

/** This event is sent when a log-line is queued. */
class CLoggingEvent : public wxEvent
{
public:
	CLoggingEvent(bool critical, bool toStdout, bool toGUI, const wxString &msg)
	: wxEvent(-1, MULE_EVT_LOGLINE)
	, m_critical(critical)
	, m_stdout(toStdout)
	, m_GUI(toGUI)
	// Deep copy, to avoid thread-unsafe reference counting. */
	, m_msg(msg.c_str(), msg.Length())
	{
	}

	const wxString &Message() const { return m_msg; }

	bool IsCritical() const { return m_critical; }

	bool ToStdout() const { return m_stdout; }

	bool ToGUI() const { return m_GUI; }

	wxEvent *Clone() const { return new CLoggingEvent(m_critical, m_stdout, m_GUI, m_msg); }

private:
	bool m_critical;
	bool m_stdout;
	bool m_GUI;
	wxString m_msg;
};

typedef void (wxEvtHandler::*MuleLogEventFunction)(CLoggingEvent &);

//! Event-handler for log-line events dispatched to GUI / stdout sinks.
#define EVT_MULE_LOGGING(func) \
	wx__DECLARE_EVT0(MULE_EVT_LOGLINE, wxEVENT_HANDLER_CAST(MuleLogEventFunction, func))

// access the logfile for EC
class CLoggerAccess
{
private:
	class wxFFileInputStream *m_logfile;
	class wxCharBuffer *m_buffer;
	size_t m_bufferlen;
	size_t m_pos;

	bool m_ready;

public:
	// construct/destruct
	CLoggerAccess();
	~CLoggerAccess();
	// Reset (used when the logfile is cleared)
	void Reset();
	// get a string (if there is one)
	bool GetString(wxString &s);
	// is a string available?
	bool HasString();
};

/**
 * Marks a log string that is deliberately not wrapped in _().
 *
 * Both expand to the string unchanged, so neither reaches xgettext and neither costs anything at
 * runtime; they exist so the reason is greppable and an untranslated literal is not mistaken for an
 * oversight (issue #866).
 *
 * LOG_DIAGNOSTIC: an internal fault or trace whose value is being readable in a bug report someone
 * pastes in. Translating it would make those reports harder to search.
 *
 * LOG_PRELOCALE: emitted before Localize_mule() has run, so no catalog is loaded and gettext would
 * return the original text anyway. See the ordering note in CamuleApp::OnInit().
 */
#define LOG_DIAGNOSTIC(str) str
#define LOG_PRELOCALE(str) str

/**
 * Logging macros. AddLogLineM calls one of the two CLogger::AddLogLine overloads depending on
 * parameters; AddDebugLogLine* logs only if the message is critical or the debug type is enabled in
 * the preferences. AddLogLineMS also always prints to stdout.
 */
#ifdef MULEUNIT
#define AddDebugLogLineN(...) \
	do { \
	} while (false)
#define AddLogLineN(...) \
	do { \
	} while (false)
#define AddLogLineNS(...) \
	do { \
	} while (false)
#define AddDebugLogLineC(...) \
	do { \
	} while (false)
#define AddLogLineC(...) \
	do { \
	} while (false)
#define AddLogLineCS(...) \
	do { \
	} while (false)
#define AddDebugLogLineF(...) \
	do { \
	} while (false)
#define AddLogLineF(...) \
	do { \
	} while (false)
#else
// Macros for 'N'on critical logging
#ifdef __DEBUG__
#define AddDebugLogLineN(type, string) \
	if (theLogger.IsEnabled(type)) \
	theLogger.AddLogLine(__TFILE__, __LINE__, false, type, string)
#else
#define AddDebugLogLineN(type, string) \
	do { \
	} while (false)
#endif
#define AddLogLineN(string) theLogger.AddLogLine(__TFILE__, __LINE__, false, logStandard, string)
#define AddLogLineNS(string) theLogger.AddLogLine(__TFILE__, __LINE__, false, logStandard, string, true)
// Macros for 'C'ritical logging
#define AddDebugLogLineC(type, string) theLogger.AddLogLine(__TFILE__, __LINE__, true, type, string)
#define AddLogLineC(string) theLogger.AddLogLine(__TFILE__, __LINE__, true, logStandard, string)
#define AddLogLineCS(string) theLogger.AddLogLine(__TFILE__, __LINE__, true, logStandard, string, true)
// Macros for logging to logfile only
#ifdef __DEBUG__
#define AddDebugLogLineF(type, string) \
	if (theLogger.IsEnabled(type)) \
	theLogger.AddLogLine(__TFILE__, __LINE__, false, type, string, false, false)
#else
#define AddDebugLogLineF(type, string) \
	do { \
	} while (false)
#endif
#define AddLogLineF(string) \
	theLogger.AddLogLine(__TFILE__, __LINE__, false, logStandard, string, false, false)
#endif

#endif
// File_checked_for_headers
