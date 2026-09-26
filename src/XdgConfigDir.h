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

#ifndef XDGCONFIGDIR_H
#define XDGCONFIGDIR_H

#include <wx/string.h>
#include <wx/utils.h>

// The user's XDG config directory as the desktop session reads it. A Flatpak's own
// $XDG_CONFIG_HOME points into its sandbox, which the host never reads; Flatpak passes the host's
// value on as $HOST_XDG_CONFIG_HOME.
inline wxString XdgConfigDir()
{
	const bool flatpak = wxGetEnv(wxT("FLATPAK_ID"), nullptr);
	wxString dir;
	if (wxGetEnv(flatpak ? wxT("HOST_XDG_CONFIG_HOME") : wxT("XDG_CONFIG_HOME"), &dir) && !dir.empty()) {
		return dir;
	}
	return wxGetUserHome() + wxT("/.config");
}

#endif // XDGCONFIGDIR_H
