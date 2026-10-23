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
	return Text(FixedTime().Format("%Y-%m-%dT%H:%M:%S")) + ".123" +
	       Text(FixedTime().Format("%z")).insert(3, ":") + " ";
}
} // namespace

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
	ASSERT_EQUALS(std::string("\n"),
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
