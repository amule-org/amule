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

#ifndef AUTOSTARTMANAGER_H
#define AUTOSTARTMANAGER_H

#include <wx/string.h>

// Cross-platform "start aMule when the user logs in" toggle. The per-OS store of record is:
//
//   Windows : HKCU\Software\Microsoft\Windows\CurrentVersion\Run\aMule
//   Linux   : $XDG_CONFIG_HOME/autostart/amule.desktop (per XDG spec)
//   macOS   : ~/Library/LaunchAgents/org.amule.amule.plist
//
// All three are per-user -- no elevation required. Toggling reads and writes the OS directly, never
// aMule.conf, so the OS is always the source of truth, matching what the user sees in Task Manager
// / Login Items / `systemctl --user list-unit-files`.
//
// amule, amuled and amulegui share that one slot. Each binary only reports, removes or heals an
// entry that starts itself; enabling one replaces whichever another had set.
class AutostartManager
{
public:
	// True if the per-user entry starts this binary. Does not validate the registered path --
	// use SelfHealOnStartup() for that.
	static bool IsEnabled();

	// Writes/overwrites the autostart entry to start this binary, at GetTarget().
	// Idempotent; returns true on success.
	static bool Enable();

	// Removes the entry if it starts this binary. Idempotent; returns true on success.
	static bool Disable();

	// Called once from CamuleApp::OnInit. If the entry starts this binary from another path,
	// rewrites it to GetTarget() so the next login launches the right one. Handles a moved
	// AppImage, .app or install dir without making the user re-toggle the checkbox.
	//
	// Does nothing if no entry exists -- disabling autostart is always a deliberate user
	// choice.
	static void SelfHealOnStartup();

	// Resolves argv[0] to its canonical absolute path (realpath() on POSIX,
	// GetModuleFileNameW() on Windows).
	static wxString GetCanonicalExecutablePath();

	// What the entry registers for this binary: its executable, its .app for the macOS GUIs, or
	// the path an AppImage was started as. Empty when nothing on disk would start it.
	static wxString GetTarget();
};

#endif // AUTOSTARTMANAGER_H
// File_checked_for_headers
