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

#ifndef AMULE_PLAYEREXECUTABLE_H
#define AMULE_PLAYEREXECUTABLE_H

#include <wx/filename.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

// A preflight for the Unix player launch inside Flatpak. With PATH unset, leave
// resolution to execvp's platform default rather than rejecting a valid command.
inline bool IsPlayerAvailable(const wxString &program)
{
	if (program.IsEmpty()) {
		return false;
	}

	// execvp searches PATH only when the command has no slash.
	if (program.Find('/') != wxNOT_FOUND) {
		return wxFileName::FileExists(program) && wxFileName::IsFileExecutable(program);
	}

	wxString searchPath;
	if (!wxGetEnv("PATH", &searchPath)) {
		// With PATH unset, execvp uses a platform default. Let it resolve the
		// command rather than incorrectly rejecting an available player.
		return true;
	}
	if (searchPath.IsEmpty()) {
		searchPath = ".";
	}
	wxStringTokenizer directories(searchPath, ":", wxTOKEN_RET_EMPTY_ALL);
	while (directories.HasMoreTokens()) {
		const wxString directory = directories.GetNextToken();
		// An empty PATH entry means the current directory. Keep searching when a
		// matching file is not executable, just as execvp does.
		const wxString candidate = wxFileName(directory, program).GetFullPath();
		if (wxFileName::FileExists(candidate) && wxFileName::IsFileExecutable(candidate)) {
			return true;
		}
	}
	return false;
}

// A macOS application, such as /Applications/VLC.app: a folder, not an executable file.
inline bool IsAppBundle(const wxString &path)
{
	wxString dir = path;
	while (dir.length() > 1 && dir.EndsWith("/")) {
		dir.RemoveLast();
	}
	return dir.Lower().EndsWith(".app") && wxFileName::DirExists(dir);
}

#endif // AMULE_PLAYEREXECUTABLE_H
