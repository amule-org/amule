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

#include "ClientsWnd.h" // Interface declarations

#include <wx/sizer.h>

#include "ClientsListCtrl.h" // Needed for CClientsListCtrl
#include "muuli_wdr.h"       // Needed for ID_CLIENTSLIST

CClientsWnd::CClientsWnd(wxWindow *parent)
: wxPanel(parent, -1)
{
	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
	clientslistctrl = new CClientsListCtrl(this,
		ID_CLIENTSLIST,
		wxDefaultPosition,
		wxDefaultSize,
		wxDV_MULTIPLE | wxDV_ROW_LINES | wxDV_VERT_RULES);
	sizer->Add(clientslistctrl, 1, wxEXPAND | wxALL, 0);
	SetSizer(sizer);
	sizer->SetSizeHints(this);
	sizer->Fit(this);
}

CClientsWnd::~CClientsWnd() {}

void CClientsWnd::UpdateAll()
{
	// A repaint, not a per-row notification: a virtual list pulls each cell's
	// value as it draws, so this re-reads exactly the rows on screen and
	// nothing else -- the same reasoning as CSharedFilesCtrl::EndBulkUpdate().
	clientslistctrl->Refresh();
}
