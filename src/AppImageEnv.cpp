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
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
//

#include "AppImageEnv.h"

#include <wx/string.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

#include <cstdlib>

namespace
{

// The AppImage runtime (and our AppRun) export APPDIR = the squashfs mount
// point. Its presence is what tells us we're running from inside a bundle.
wxString GetAppDir()
{
	const char *env = getenv("APPDIR");
	return env ? wxString::FromUTF8(env) : wxString();
}

} // namespace

namespace AppImageEnv
{

bool GetSanitizedExecEnv(wxExecuteEnv &env)
{
	const wxString appdir = GetAppDir();
	if (appdir.IsEmpty()) {
		// Not inside an AppImage: nothing to strip, inherit the environment.
		return false;
	}

	wxEnvVariableHashMap current;
	if (!wxGetEnvMap(&current)) {
		return false;
	}

	// A path component belongs to the bundle when it is $APPDIR itself or sits
	// beneath it. Strip such components from every variable; a variable whose
	// whole value was bundle-owned is dropped entirely. Deliberately generic
	// rather than a fixed LD_LIBRARY_PATH/GTK_PATH list, so the module- and
	// data-path variables the linuxdeploy GTK hooks inject are covered too.
	const wxString prefix = appdir + wxT("/");
	for (const auto &var : current) {
		const wxString &value = var.second;
		if (!value.Contains(appdir)) {
			env.env[var.first] = value;
			continue;
		}
		wxString kept;
		wxStringTokenizer parts(value, wxT(":"), wxTOKEN_RET_EMPTY_ALL);
		while (parts.HasMoreTokens()) {
			const wxString part = parts.GetNextToken();
			if (part == appdir || part.StartsWith(prefix)) {
				// Bundle-owned entry -- drop it so the child resolves the
				// host's system copy instead.
				continue;
			}
			if (!kept.IsEmpty()) {
				kept << wxT(":");
			}
			kept << part;
		}
		if (!kept.IsEmpty()) {
			env.env[var.first] = kept;
		}
		// else: every component was bundle-owned -> omit the variable.
	}
	return true;
}

} // namespace AppImageEnv
