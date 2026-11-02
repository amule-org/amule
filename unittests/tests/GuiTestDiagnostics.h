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

#ifndef AMULE_GUI_TEST_DIAGNOSTICS_H
#define AMULE_GUI_TEST_DIAGNOSTICS_H

#include <wx/app.h>
#include <wx/debug.h>
#include <wx/evtloop.h>
#include <wx/init.h>
#include <wx/log.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace GuiTestDiagnostics
{
// wxEntryStart deletes the startup log target, so retain its result separately.
inline bool &StartupLogFailed()
{
	static bool failed = false;
	return failed;
}

class FailingLogStderr : public wxLogStderr
{
public:
	explicit FailingLogStderr(bool failImmediately)
	: m_failImmediately(failImmediately)
	{
	}

protected:
	void DoLogRecord(wxLogLevel level, const wxString &message, const wxLogRecordInfo &info) override
	{
		wxLogStderr::DoLogRecord(level, message, info);
		if (level == wxLOG_Error || level == wxLOG_Warning) {
			StartupLogFailed() = true;
			if (m_failImmediately) {
				Flush();
				std::exit(EXIT_FAILURE);
			}
		}
	}

private:
	bool m_failImmediately;
};

inline void AssertHandler(
	const wxString &file, int line, const wxString &func, const wxString &cond, const wxString &msg)
{
	std::cerr << file.ToUTF8().data() << ":" << line << ": " << func.ToUTF8().data() << ": assertion "
		  << cond.ToUTF8().data() << " failed: " << msg.ToUTF8().data() << '\n';
	std::exit(EXIT_FAILURE);
}
} // namespace GuiTestDiagnostics

// Before wxEntryStart: assertions fail visibly, and startup logs cannot open
// dialogs. Optional GUI startup may still skip if initialization fails.
inline void InstallGuiTestDiagnostics(bool requireGui = false)
{
	GuiTestDiagnostics::StartupLogFailed() = false;
	delete wxLog::SetActiveTarget(new GuiTestDiagnostics::FailingLogStderr(requireGui));
	wxSetAssertHandler(GuiTestDiagnostics::AssertHandler);
}

// After successful wxEntryStart, before CallOnInit: wx deletes the startup
// target. Restore fatal stderr logging and reject any recorded startup warning.
inline void RestoreGuiTestLog()
{
	delete wxLog::SetActiveTarget(new GuiTestDiagnostics::FailingLogStderr(true));
	if (GuiTestDiagnostics::StartupLogFailed()) {
		std::exit(EXIT_FAILURE);
	}
}

inline void Check(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

enum class GuiTestEventLoop
{
	None,
	Activate
};

// Runs body between CallOnInit and OnExit. Returns 77 (skip) when no display
// is available, 1 when init fails or body throws, else body's result or 0.
template <typename Body>
int RunGuiTest(int argc,
	char **argv,
	const char *skipMessage,
	Body body,
	GuiTestEventLoop eventLoop = GuiTestEventLoop::None)
{
	InstallGuiTestDiagnostics();
	if (!wxEntryStart(argc, argv)) {
		if (skipMessage) {
			std::cerr << skipMessage << '\n';
		}
		return 77;
	}
	RestoreGuiTestLog();
	int result = 0;
	if (!wxTheApp->CallOnInit()) {
		result = 1;
	} else {
		auto run = [&] {
			if constexpr (std::is_void_v<decltype(body())>) {
				body();
			} else {
				result = body();
			}
		};
		try {
			if (eventLoop == GuiTestEventLoop::Activate) {
				wxEventLoop loop;
				wxEventLoopActivator active(&loop);
				run();
			} else {
				run();
			}
		} catch (const std::exception &error) {
			std::cerr << error.what() << '\n';
			result = 1;
		}
		wxTheApp->OnExit();
	}
	wxEntryCleanup();
	return result;
}

#endif // AMULE_GUI_TEST_DIAGNOSTICS_H
