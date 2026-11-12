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

#include "config.h"
#undef MULEUNIT
#include "Logger.h"
#include "Preferences.h"
#include <muleunit/test.h>
#include <stdexcept>
#include <wx/ffile.h>
#include <wx/filename.h>

// The production logger only needs these two preference values in this fixture.
bool CPreferences::s_bVerbose = false;
bool CPreferences::s_bVerboseLogfile = false;

using namespace muleunit;
DECLARE_SIMPLE(LogFileReplay)

namespace
{
struct LogFile
{
	LogFile()
	: name(wxFileName::CreateTempFileName("amule-log-replay-"))
	{
		if (!theLogger.OpenLogfile(name)) {
			throw std::runtime_error("could not open temporary logfile");
		}
	}
	~LogFile()
	{
		theLogger.CloseLogfile();
		wxRemoveFile(name);
	}
	wxString Read() const
	{
		wxFFile file(name, "rb");
		wxString text;
		file.ReadAll(&text, wxConvUTF8);
		return text;
	}
	void Append(const wxString &text) const
	{
		wxFFile file(name, "ab");
		file.Write(text, wxConvUTF8);
	}
	void AppendBytes(const char *bytes, size_t count) const
	{
		wxFFile file(name, "ab");
		file.Write(bytes, count);
	}
	wxString name;
};
} // namespace

TEST(LogFileReplay, PersistentFlagsSurviveReopenAndReaderEof)
{
	LogFile file;
	theLogger.Log(LogLevel::Error, "test", wxString::FromUTF8("ordinary café"));
	theLogger.Log(LogLevel::Info, "test", "status", true);
	theLogger.Log(LogLevel::Warning, "test", "private\n\nprivate last", true, false, false);
	const wxString disk = file.Read();
	ASSERT_TRUE(HasLogRecordHeader(disk));
	ASSERT_TRUE(disk.Contains("[log status=1 replay=0] [test] private"));
	// A new writer and reader must recover policy from disk, not process memory.
	theLogger.CloseLogfile();
	ASSERT_TRUE(theLogger.OpenLogfile(file.name, true));
	CLoggerAccess reader;
	wxString line;
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line.StartsWith(" ") && line.Contains(wxString::FromUTF8("ERROR [test] ordinary café")));
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line.StartsWith("!") && line.Contains("INFO  [test] status"));
	ASSERT_TRUE(!reader.HasString());
	theLogger.Log(LogLevel::Info, "test", "after eof");
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line.Contains("after eof"));
	ASSERT_TRUE(!reader.HasString());
	reader.Reset();
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line.Contains(wxString::FromUTF8("ordinary café")));
}

TEST(LogFileReplay, MixedLegacyFileAndPartialUtf8Record)
{
	LogFile file;
	file.Append("!legacy status\n.legacy hidden\n legacy ordinary\nraw crash text\n");
	CLoggerAccess reader;
	wxString line;
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line == "!legacy status\n");
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line == " legacy ordinary\n");
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line == " raw crash text\n");
	const wxString disk = EncodeLogFileLine(
		FormatLogLines(CLogRecord(LogLevel::Info, "test", wxString::FromUTF8("café"))), true, true);
	const wxScopedCharBuffer bytes = disk.utf8_str();
	file.AppendBytes(bytes.data(), bytes.length() - 2);
	ASSERT_TRUE(!reader.HasString());
	file.AppendBytes(bytes.data() + bytes.length() - 2, 2);
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line.StartsWith("!") && line.Contains(wxString::FromUTF8("café")));
	ASSERT_TRUE(!line.Contains("[log status="));
	ASSERT_TRUE(!reader.HasString());
}

TEST(LogFileReplay, EmptyAndEmergencyRecordsAreTimestamped)
{
	LogFile file;
	theLogger.Log(LogLevel::Info, "", "");
	theLogger.EmergencyLog(wxString::FromUTF8("fatal café\n\nlast frame\n"), false);
	CLoggerAccess reader;
	wxString line;
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line.StartsWith(" ") && HasLogRecordHeader(line.Mid(1)));
	for (int i = 0; i < 3; ++i) {
		ASSERT_TRUE(reader.GetString(line));
		ASSERT_TRUE(line.StartsWith("!") && HasLogRecordHeader(line.Mid(1)));
		ASSERT_TRUE(line.Contains("ERROR [crash] "));
	}
	ASSERT_TRUE(!reader.HasString());
	ASSERT_TRUE(!file.Read().Contains("\n!"));
}

TEST(LogFileReplay, LongUtf8RecordsGrowTheReaderBuffer)
{
	LogFile file;
	const wxString message = wxString('x', 12000) + wxString::FromUTF8("café");
	theLogger.Log(LogLevel::Info, "", message, true);
	CLoggerAccess reader;
	wxString line;
	ASSERT_TRUE(reader.GetString(line));
	ASSERT_TRUE(line.StartsWith("!") && line.EndsWith(message + "\n"));
	ASSERT_TRUE(!line.Contains("[log status="));
	ASSERT_TRUE(!reader.HasString());
}
