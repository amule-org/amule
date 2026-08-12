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

#ifdef CLIENT_GUI
#include <ec/cpp/RemoteConnect.h> // Needed for CECPacketHandlerBase
#endif

class CClientsListCtrl;
class CClientHistoryListCtrl;

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
	CClientHistoryListCtrl *historylistctrl;

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

	/**
	 * Rebuild the history from the credit store.
	 *
	 * Called whenever the Known tab is shown, not once at startup. Two
	 * reasons it cannot be a one-off: a peer already in the history that
	 * reconnects has new totals, a new last-seen and one more session, and a
	 * peer met for the first time since the page was opened is missing
	 * entirely. A stale snapshot would show "last seen" months ago for
	 * someone visibly transferring in the Active tab next door.
	 *
	 * Rebuilding rather than patching individual rows: the credit store is
	 * the truth, a rebuild is a vector fill and one sort, and it happens only
	 * when a person actually looks at the tab.
	 */
	void LoadHistory();

#ifdef CLIENT_GUI
	/**
	 * Receives the EC_OP_CLIENT_HISTORY reply.
	 *
	 * Nested so the panel keeps ownership of the rows the reply becomes, and
	 * so a reply that arrives after the page is gone has nowhere to land
	 * rather than somewhere stale.
	 */
	class CHistoryHandler : public CECPacketHandlerBase
	{
	public:
		explicit CHistoryHandler(CClientsWnd *owner)
		: m_owner(owner)
		{
		}
		void HandlePacket(const CECPacket *packet) override;

	private:
		CClientsWnd *m_owner;
	};
	CHistoryHandler m_historyHandler;
#endif
};

#endif // CLIENTSWND_H
// File_checked_for_headers
