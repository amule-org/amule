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

#include <muleunit/test.h>
#include "config.h"
// Exercise the production macros rather than the unit-test no-op replacements.
#undef MULEUNIT
#include "Logger.h"
#include "LogText.h"
#include <cstdlib>
#include <ctime>
#include <memory>

using namespace muleunit;
DECLARE_SIMPLE(LogRecord)

namespace
{
wxDateTime FixedTime()
{
	wxDateTime time;
	time.Set((time_t)0);
	time.SetMillisecond(123);
	return time;
}
std::string Text(const wxString &text)
{
	return std::string(text.utf8_str());
}
std::string Stamp()
{
	return Text(FixedTime().Format("%Y-%m-%dT%H:%M:%S")) + ".123" + Text(FormatUtcOffset(FixedTime())) +
	       " ";
}

// POSIX TZ strings carry their own rules, so the test needs no time zone database.
class TimeZoneGuard
{
public:
	explicit TimeZoneGuard(const char *zone)
	{
		if (const char *previous = std::getenv("TZ")) {
			m_previous = previous;
			m_hadPrevious = true;
		}
		Set(zone);
	}
	~TimeZoneGuard() { Set(m_hadPrevious ? m_previous.c_str() : nullptr); }

private:
	static void Set(const char *zone)
	{
#ifdef _WIN32
		_putenv_s("TZ", zone ? zone : "");
		_tzset();
#else
		if (zone) {
			setenv("TZ", zone, 1);
		} else {
			unsetenv("TZ");
		}
		tzset();
#endif
	}
	std::string m_previous;
	bool m_hadPrevious = false;
};
} // namespace

TEST(LogRecord, UtcOffsetUsesRfc3339FormInEveryZone)
{
	const struct
	{
		const char *zone;
		const char *offset;
	} cases[] = { { "UTC0", "+00:00" },
		{ "IST-5:30", "+05:30" },
		{ "NST3:30", "-03:30" },
		{ "LHI-10:30", "+10:30" } };
	for (const auto &c : cases) {
		const TimeZoneGuard guard(c.zone);
		ASSERT_EQUALS(std::string(c.offset), Text(FormatUtcOffset(FixedTime())));
		const wxString line = FormatLogLines(CLogRecord(LogLevel::Info, "", "message", FixedTime()));
		ASSERT_TRUE(HasLogRecordHeader(line));
	}
}

TEST(LogRecord, FormatsLocalMillisecondsAndUtf8)
{
	const CLogRecord record(LogLevel::Warning, "FileIO", wxString::FromUTF8("café"), FixedTime());
	ASSERT_EQUALS(Stamp() + "WARN  [FileIO] café\n", Text(FormatLogLines(record)));
}

TEST(LogRecord, EverySeverityHasAnExplicitName)
{
	const LogLevel levels[] = {
		LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warning, LogLevel::Error
	};
	const char *names[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };
	for (unsigned i = 0; i < 5; ++i) {
		const CLogRecord record(levels[i], "", "message", FixedTime());
		ASSERT_EQUALS(Stamp() + names[i] + " message\n", Text(FormatLogLines(record)));
	}
}

TEST(LogRecord, PrefixesEveryPhysicalLine)
{
	const CLogRecord record(LogLevel::Info, "", "first\nsecond\n", FixedTime());
	ASSERT_EQUALS(Stamp() + "INFO  first\n" + Stamp() + "INFO  second\n", Text(FormatLogLines(record)));
	ASSERT_EQUALS(Stamp() + "INFO  \n",
		Text(FormatLogLines(CLogRecord(LogLevel::Info, "", " \n\t", FixedTime()))));
}

TEST(LogRecord, PreservesInteriorBlankLines)
{
	const CLogRecord record(LogLevel::Info, "", "first\n\nthird", FixedTime());
	ASSERT_EQUALS(Stamp() + "INFO  first\n" + Stamp() + "INFO  \n" + Stamp() + "INFO  third\n",
		Text(FormatLogLines(record)));
}

TEST(LogRecord, DebugMacrosAreSingleStatements)
{
	bool alternate = false;
	if (false)
		AddDebugLogLineN(logGeneral, "unreachable");
	else
		alternate = true;
	ASSERT_TRUE(alternate);
	alternate = false;
	if (false)
		AddDebugLogLineF(logGeneral, "unreachable");
	else
		alternate = true;
	ASSERT_TRUE(alternate);
}

TEST(LogRecord, DisabledDebugMacrosDoNotEvaluateMessages)
{
	theLogger.SetVerbose(false);
	int evaluations = 0;
	AddDebugLogLineN(logGeneral, (++evaluations, wxString("unused")));
	AddDebugLogLineF(logGeneral, (++evaluations, wxString("unused")));
	ASSERT_EQUALS(0, evaluations);
}

TEST(LogRecord, MarkersEncodeVisibilityRatherThanSeverity)
{
	ASSERT_EQUALS(' ', LegacyLogMarker(false, true));
	ASSERT_EQUALS('!', LegacyLogMarker(true, true));
	ASSERT_EQUALS('.', LegacyLogMarker(false, false));
	ASSERT_EQUALS('.', LegacyLogMarker(true, false));
}

TEST(LogRecord, EventCloneRetainsEmissionTimeAndIndependentStorage)
{
	CLogRecord record(LogLevel::Error, "FileIO", "original", FixedTime());
	CLoggingEvent event(false, true, true, record);
	std::unique_ptr<wxEvent> clone(event.Clone());
	const auto &copy = static_cast<const CLoggingEvent &>(*clone);
	record.message = "changed after queueing";
	record.category = "changed";
	record.timestamp = wxDateTime::UNow();
	ASSERT_EQUALS(Stamp() + "ERROR [FileIO] original\n", Text(FormatLogLines(copy.Record())));
	ASSERT_TRUE(!copy.IsCritical());
	ASSERT_TRUE(copy.ToStdout());
	ASSERT_TRUE(copy.ToGUI());
	ASSERT_TRUE(static_cast<const wxChar *>(copy.Record().message.c_str()) !=
		    static_cast<const wxChar *>(event.Record().message.c_str()));
}

TEST(LogRecord, DiskFlagsRoundTripIndependentlyOfSeverity)
{
	for (const LogLevel level : { LogLevel::Info, LogLevel::Warning, LogLevel::Error }) {
		const wxString line =
			FormatLogLines(CLogRecord(level, "FileIO", wxString::FromUTF8("café"), FixedTime()));
		for (const bool status : { false, true }) {
			for (const bool replay : { false, true }) {
				const wxString disk = EncodeLogFileLine(line, status, replay);
				ASSERT_TRUE(HasLogRecordHeader(disk));
				wxString wire;
				ASSERT_EQUALS(replay, DecodeLogFileLine(disk, wire));
				ASSERT_EQUALS(
					replay ? Text(wxString(status ? '!' : ' ') + line) : std::string(),
					Text(wire));
			}
		}
	}
}

TEST(LogRecord, ReplayDoesNotInterpretMessageTextAsFlags)
{
	const wxString line = FormatLogLines(
		CLogRecord(LogLevel::Info, "", "[log status=1 replay=0] is message text", FixedTime()));
	wxString wire;
	ASSERT_TRUE(DecodeLogFileLine(EncodeLogFileLine(line, false, true), wire));
	ASSERT_EQUALS(Text(" " + line), Text(wire));
}

TEST(LogRecord, ReadsLegacyAndUnstructuredRecordsWithoutTruncation)
{
	wxString wire;
	for (const wxString line : { wxString("!old status\n"), wxString(" old ordinary\n") }) {
		ASSERT_TRUE(DecodeLogFileLine(line, wire));
		ASSERT_EQUALS(Text(line), Text(wire));
	}
	ASSERT_TRUE(!DecodeLogFileLine(".old logfile only\n", wire));
	ASSERT_TRUE(wire.empty());
	for (const wxString line : { wxString("fatal backtrace\n"),
		     wxString("\n"),
		     wxString("not-a-timestamp INFO  [log status=1 replay=0] text\n") }) {
		ASSERT_TRUE(DecodeLogFileLine(line, wire));
		ASSERT_EQUALS(Text(" " + line), Text(wire));
	}
}

TEST(LogRecord, PrefixesCrLfAndCarriageReturnLines)
{
	const CLogRecord record(LogLevel::Info, "", "first\r\nsecond\rthird", FixedTime());
	ASSERT_EQUALS(Stamp() + "INFO  first\n" + Stamp() + "INFO  second\n" + Stamp() + "INFO  third\n",
		Text(FormatLogLines(record)));
}

TEST(LogRecord, FullSnapshotsRetainLogfileOnlyMarkersAndPartialLastLine)
{
	const wxString ordinary = FormatLogLines(CLogRecord(LogLevel::Info, "", "ordinary", FixedTime()));
	const wxString status = FormatLogLines(CLogRecord(LogLevel::Error, "", "status", FixedTime()));
	const wxString hidden = FormatLogLines(CLogRecord(LogLevel::Debug, "", "hidden", FixedTime()));
	const wxString mixed = "!old status\n.old hidden\n" + EncodeLogFileLine(ordinary, false, true) +
			       EncodeLogFileLine(status, true, true) +
			       EncodeLogFileLine(hidden, true, false) + "raw tail";
	ASSERT_EQUALS(
		Text("!old status\n.old hidden\n " + ordinary + "!" + status + "." + hidden + " raw tail"),
		Text(LogFileToEC(mixed)));
	ASSERT_TRUE(LogFileToEC("").empty());
}

TEST(LogRecord, HumanExportsStripOnlyOneWireBytePerLine)
{
	const wxString line = FormatLogLines(CLogRecord(LogLevel::Info, "", "!message . text", FixedTime()));
	ASSERT_EQUALS(Text(line + line + line + line + "ERROR: cannot open logfile"),
		Text(ECLogToText(
			" " + line + "!" + line + "." + line + line + "ERROR: cannot open logfile")));
	ASSERT_EQUALS(std::string("\n\n"), Text(ECLogToText(" \n!\n")));
	ASSERT_TRUE(ECLogToText("").empty());
}

TEST(LogRecord, StatusBarUsesCompactTimestampForEverySeverity)
{
	const TimeZoneGuard zone("UTC0");
	for (const LogLevel level :
		{ LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warning, LogLevel::Error }) {
		const wxString line = FormatLogLines(
			CLogRecord(level, "FileIO", wxString::FromUTF8("café INFO .123+00:00"), FixedTime()));
		ASSERT_EQUALS(std::string("1970-01-01 00:00:00 [FileIO] café INFO .123+00:00\n"),
			Text(FormatStatusBarLogLine(line)));
		ASSERT_TRUE(HasLogRecordHeader(line));
	}
	ASSERT_EQUALS(std::string("1970-01-01 00:00:00 message\n"),
		Text(FormatStatusBarLogLine(
			FormatLogLines(CLogRecord(LogLevel::Info, "", "message", FixedTime())))));
	for (const wxString line : { wxString("2016-11-11 18:00:00: old message\n"),
		     wxString("raw crash text\n"),
		     wxString("\n") }) {
		ASSERT_EQUALS(Text(line), Text(FormatStatusBarLogLine(line)));
	}
}
