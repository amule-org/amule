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

#ifndef GUI_CONFIRM_H
#define GUI_CONFIRM_H

// NOTE: amuled builds with wxUSE_GUI=0, where no wx GUI header (not even
// <wx/msgdlg.h>) may be included. GuiEvents.cpp is compiled for both the
// monolithic GUI and the daemon, so this header must stay includable there.
#ifdef AMULE_DAEMON
// No message boxes in the daemon: keep the base style untouched and do not
// touch wx at all.
inline int GuiConfirmStyle(int baseStyle)
{
	return baseStyle;
}
#else
// Must be included after "amule.h" (which sets up config/wx): including
// <wx/msgdlg.h> first breaks builds with incomplete wx types.
#include <wx/msgdlg.h> // Needed for wxYES_NO / wxNO_DEFAULT

#include "Preferences.h" // Needed for thePrefs::ConfirmDefaultYes

// Central helper for Yes/No confirmations: by default (preference off) No stays the
// highlighted safe choice via wxNO_DEFAULT; when the user opts into
// "Default confirm dialogs to Yes", the flag is omitted so Yes is highlighted.
inline int GuiConfirmStyle(int baseStyle)
{
	if (thePrefs::ConfirmDefaultYes()) {
		return baseStyle;
	}
	return baseStyle | wxNO_DEFAULT;
}
#endif                   // AMULE_DAEMON

#endif // GUI_CONFIRM_H
