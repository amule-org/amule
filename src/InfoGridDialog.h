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

#ifndef INFOGRIDDIALOG_H
#define INFOGRIDDIALOG_H

#include <functional>

#include <wx/colour.h>
#include <wx/string.h>

class wxWindow;
class wxSizer;

/**
 * Show a small modal dialog: an optional intro line, a two-column grid the
 * caller fills, and an OK button.
 *
 * Shared so the part-bar legends and the status-bar core-version details do
 * not each carry a copy of the same scaffolding. `fillGrid` receives the
 * dialog to parent widgets to, and the grid to add them to.
 *
 * `introArt` is an optional wxArtProvider id (wxART_WARNING, wxART_TICK_MARK,
 * ...) drawn left of the intro line. Stock art, so it matches the platform and
 * needs no bundled asset.
 */
void ShowInfoGridDialog(wxWindow *parent,
	const wxString &title,
	const wxString &intro,
	const std::function<void(wxWindow *, wxSizer *)> &fillGrid,
	const wxString &introArt = wxString(),
	const wxColour &introColour = wxColour());

#endif // INFOGRIDDIALOG_H
