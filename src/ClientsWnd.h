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

#ifndef CLIENTSWND_H
#define CLIENTSWND_H

#include <wx/panel.h>

class CClientsListCtrl;

/**
 * The Clients page: every peer we are currently talking to, once each.
 *
 * Built in code rather than through a muuli_wdr layout function -- it is one
 * full-bleed list with no surrounding controls, so a generated layout would be
 * a sizer and nothing else.
 */
class CClientsWnd : public wxPanel
{
public:
	CClientsWnd(wxWindow *parent);
	~CClientsWnd();

	CClientsListCtrl *clientslistctrl;

	/**
	 * Re-read every listed peer's values.
	 *
	 * Driven from the GUI timer rather than from the per-file refresh
	 * signals: those carry an ECID and fire from a dozen sites, so a global
	 * list hooked into them would be one missed call away from a row that
	 * stops moving. A sweep costs only the rows on screen -- the control
	 * skips anything outside the viewport -- and is called only while this
	 * page is the active one.
	 */
	void UpdateAll();
};

#endif // CLIENTSWND_H
// File_checked_for_headers
