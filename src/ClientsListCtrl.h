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

#ifndef CLIENTSLISTCTRL_H
#define CLIENTSLISTCTRL_H

#include "MuleVirtualDataViewCtrl.h"

class CUpDownClient;

#define COLUMN_CLIENTS_NAME 0
#define COLUMN_CLIENTS_SOFTWARE 1
#define COLUMN_CLIENTS_VERSION 2
#define COLUMN_CLIENTS_ADDRESS 3
#define COLUMN_CLIENTS_ORIGIN 4
#define COLUMN_CLIENTS_FILES 5
#define COLUMN_CLIENTS_UP_SPEED 6
#define COLUMN_CLIENTS_DOWN_SPEED 7
#define COLUMN_CLIENTS_SESSION_UP 8
#define COLUMN_CLIENTS_SESSION_DOWN 9
#define COLUMN_CLIENTS_TOTAL_UP 10
#define COLUMN_CLIENTS_TOTAL_DOWN 11
#define COLUMN_CLIENTS_RATIO 12
//! Always empty. Absorbs the macOS trailing-column sizing; see
//! CMuleDataViewCtrl::AppendSpacerColumn().
#define COLUMN_CLIENTS_SPACER 13

/**
 * Every peer we are currently talking to, once each.
 *
 * The per-file lists (CSourceListCtrl, CSharedFilePeersListCtrl) answer "who is
 * on this file". This answers "who am I talking to", which is a different
 * question: the same peer can be a source for one download and a requester for
 * another, and only a per-client view can say so. Hence the file *count*
 * column, which the per-file lists have no way to express.
 *
 * Rows are keyed on the CUpDownClient pointer, and the pointer is treated as a
 * key rather than something to hold: the client is added when CClientList
 * accepts it, refreshed as it transfers, and dropped on the
 * ClientBeingDestroyed broadcast. A CClientRef would be the obvious
 * alternative and is the wrong tool -- those are owning (Unlink() deletes at
 * the last release), so the list would keep every peer it ever saw alive and
 * would never be told to let go, because the signal it is waiting for comes
 * from the destructor its own reference is preventing.
 */
class CClientsListCtrl : public CMuleVirtualDataViewCtrl
{
public:
	CClientsListCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags);
	~CClientsListCtrl();

	//! A peer we are now talking to, if it isn't already listed.
	void AddClient(CUpDownClient *client);
	/**
	 * Drop a peer that is going away.
	 *
	 * Pointer-value comparison only: this runs from ~CUpDownClient, so the
	 * object is already being torn down and nothing about it may be read.
	 */
	void RemoveClient(CUpDownClient *client);

protected:
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;

	/**
	 * Speeds and transfer totals move on every poll, so a row sorted by one
	 * of them has to be able to move with it.
	 */
	bool IsLiveSortColumn() const override { return true; }

	wxDECLARE_EVENT_TABLE();

private:
	//! How many of our files this peer is exchanging, for COLUMN_CLIENTS_FILES.
	static unsigned CountRelatedFiles(const CUpDownClient *client);
};

#endif // CLIENTSLISTCTRL_H
// File_checked_for_headers
