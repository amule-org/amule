// This file is part of the aMule Project.
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <common/Macros.h>
#include "Logger.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <wx/init.h>
#include <wx/filename.h>

namespace
{
void Check(bool passed, const char *message)
{
	if (!passed) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}
} // namespace

int main()
{
	wxInitializer init;
	Check(init.IsOk(), "wx initialization failed");
	int constructed = 0;
	auto message = [&]() {
		++constructed;
		return wxString("runtime-category-test");
	};

	// Exercise the production macros, without MULEUNIT's no-op replacements.
	AddDebugLogLineN(logGeneral, message());
	AddDebugLogLineF(logGeneral, message());
	Check(constructed == 0, "disabled logging evaluated a message");

#ifndef LOGGER_CONSOLE_TEST
	theLogger.SetEnabled(logGeneral, true);
	Check(!theLogger.IsEnabled(logGeneral), "category bypassed the global verbose gate");
#endif
	theLogger.SetVerbose(true);
	Check(theLogger.IsEnabled(logGeneral), "verbose did not enable the selected category");
	AddDebugLogLineN(logGeneral, message());
	AddDebugLogLineF(logGeneral, message());
	Check(constructed == 2, "enabled logging lost a message");

#ifndef LOGGER_CONSOLE_TEST
	Check(!theLogger.IsEnabled(logHasher), "unselected category was enabled");
	theLogger.SetEnabled(logGeneral, false);
	AddDebugLogLineN(logGeneral, message());
	Check(constructed == 2, "disabled category evaluated a message");
	theLogger.SetEnabled(logGeneral, true);
#endif
	theLogger.SetVerbose(false);
	AddDebugLogLineN(logGeneral, message());
	AddDebugLogLineF(logGeneral, message());
	Check(constructed == 2, "verbose off evaluated a message");
	theLogger.SetVerbose(true);
	Check(theLogger.IsEnabled(logGeneral), "verbose toggle lost the category selection");

	// Each macro must be a single statement, including in Release.
	bool outerElse = false;
	if (false)
		AddDebugLogLineN(logGeneral, message());
	else
		outerElse = true;
	Check(outerElse, "normal macro captured an outer else");
	bool fileOuterElse = false;
	if (false)
		AddDebugLogLineF(logGeneral, message());
	else
		fileOuterElse = true;
	Check(fileOuterElse, "file-only macro captured an outer else");

	Check(!theLogger.IsEnabled(logStandard), "standard pseudo-category was enabled");
	Check(!theLogger.IsEnabled(logCategoryCount), "invalid category was enabled");
	theLogger.SetVerbose(false);
	AddDebugLogLineC(logGeneral, message());
	AddLogLineN(message());
	Check(constructed == 4, "critical or ordinary logging was suppressed");

#ifndef LOGGER_CONSOLE_TEST
	const wxString log = theLogger.GetUnwrittenLog();
	Check(log.Contains("General: runtime-category-test"), "debug category name was lost");
	Check(log.Contains(".20"), "file-only marker was lost");
	Check(log.Contains("!20"), "critical marker was lost");
	Check(log.Contains(" 20"), "normal marker was lost");
	Check(theLogger.GetDebugCategory(logGeneral).IsEnabled(), "verbose off cleared selection");

	const wxString path = wxFileName::CreateTempFileName("amule-runtime-log");
	Check(theLogger.OpenLogfile(path), "could not open a temporary logfile");
	theLogger.SetVerbose(true);
	AddDebugLogLineN(logGeneral, "runtime-visible");
	AddDebugLogLineF(logGeneral, "runtime-hidden");
	theLogger.SetVerboseLogfile(true);
	AddDebugLogLineN(logGeneral, "runtime-routed");
	{
		CLoggerAccess access;
		wxString replay, line;
		while (access.GetString(line)) {
			replay += line;
		}
		Check(replay.Contains("runtime-visible"), "EC replay lost a visible message");
		Check(!replay.Contains("runtime-hidden"), "EC replay included a file-only message");
		Check(!replay.Contains("runtime-routed"), "logfile preference did not route a message");
	}
	theLogger.CloseLogfile();
	wxRemoveFile(path);

	// Main-thread preference changes may race with workers emitting debug messages.
	theLogger.SetVerboseLogfile(false);
	theLogger.SetEnabled(logGeneral, true);
	theLogger.SetVerbose(true);
	std::atomic<bool> started{ false };
	std::thread reader([&]() {
		AddDebugLogLineF(logGeneral, "runtime-worker");
		started.store(true, std::memory_order_release);
		for (int i = 0; i < 1000; ++i) {
			AddDebugLogLineF(logGeneral, "runtime-worker");
		}
	});
	while (!started.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	for (int i = 0; i < 1000; ++i) {
		theLogger.SetEnabled(logGeneral, (i & 1) != 0);
		theLogger.SetVerbose((i & 2) != 0);
		theLogger.SetVerboseLogfile((i & 4) != 0);
	}
	reader.join();
	Check(theLogger.GetUnwrittenLog().Contains("runtime-worker"), "worker logging lost its message");
#endif
	std::puts("Runtime logging checks passed");
}
