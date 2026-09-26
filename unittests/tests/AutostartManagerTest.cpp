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

#include <AutostartManager.h>

#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>

using namespace muleunit;

// Linux only: the Windows and macOS backends write the user's own registry and LaunchAgents,
// while this one follows $XDG_CONFIG_HOME into a scratch directory.
DECLARE_SIMPLE(AutostartManager)

namespace
{
wxString g_configHome;

void UseScratchConfigHome()
{
	g_configHome = wxFileName::CreateTempFileName(wxT("amule-autostart"));
	wxRemoveFile(g_configHome);
	wxFileName::Mkdir(g_configHome, 0700, wxPATH_MKDIR_FULL);
	wxSetEnv(wxT("XDG_CONFIG_HOME"), g_configHome);
}

wxString EntryPath()
{
	return g_configHome + wxT("/autostart/amule.desktop");
}

wxString ReadEntry()
{
	wxString content;
	wxFile f(EntryPath());
	f.ReadAll(&content);
	return content;
}

void WriteEntry(const wxString &content)
{
	wxFile f;
	f.Create(EntryPath(), true);
	f.Write(content);
}

void Rewrite(const wxString &from, const wxString &to)
{
	wxString content = ReadEntry();
	content.Replace(from, to);
	WriteEntry(content);
}
} // namespace

TEST(AutostartManager, OffWhenTheDesktopSwitchesItOff)
{
	UseScratchConfigHome();
	ASSERT_TRUE(AutostartManager::Enable());
	ASSERT_TRUE(AutostartManager::IsEnabled());

	// GNOME's switch.
	Rewrite(wxT("X-GNOME-Autostart-enabled=true"), wxT("X-GNOME-Autostart-enabled=false"));
	ASSERT_TRUE(!AutostartManager::IsEnabled());

	// KDE's.
	Rewrite(wxT("X-GNOME-Autostart-enabled=false"), wxT("X-GNOME-Autostart-enabled=true"));
	Rewrite(wxT("Hidden=false"), wxT("Hidden=true"));
	ASSERT_TRUE(!AutostartManager::IsEnabled());
}

TEST(AutostartManager, EnableSwitchesItBackOn)
{
	UseScratchConfigHome();
	ASSERT_TRUE(AutostartManager::Enable());
	Rewrite(wxT("Hidden=false"), wxT("Hidden=true"));

	ASSERT_TRUE(AutostartManager::Enable());
	ASSERT_TRUE(AutostartManager::IsEnabled());
	ASSERT_TRUE(ReadEntry().Contains(wxT("Hidden=false")));
}

TEST(AutostartManager, SelfHealKeepsItSwitchedOff)
{
	UseScratchConfigHome();
	ASSERT_TRUE(AutostartManager::Enable());
	const wxString target = AutostartManager::GetTarget();
	Rewrite(target, wxT("/moved/away/") + wxFileName(target).GetFullName());
	Rewrite(wxT("X-GNOME-Autostart-enabled=true"), wxT("X-GNOME-Autostart-enabled=false"));

	AutostartManager::SelfHealOnStartup();

	const wxString entry = ReadEntry();
	ASSERT_TRUE(entry.Contains(wxT("Exec=\"") + target + wxT("\"")));
	ASSERT_TRUE(entry.Contains(wxT("X-GNOME-Autostart-enabled=false")));
	ASSERT_TRUE(!AutostartManager::IsEnabled());
}
