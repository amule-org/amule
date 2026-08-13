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
 * Built in code rather than through a muuli_wdr layout function -- it is lists
 * and a splitter with no surrounding controls, so a generated layout would be
 * sizers and nothing else.
 */
class CClientsWnd : public wxPanel
{
public:
	CClientsWnd(wxWindow *parent);
	~CClientsWnd();

	//! Peers holding a file we are downloading.
	CClientsListCtrl *downclientsctrl;
	//! Peers we are sending a file to.
	CClientsListCtrl *upclientsctrl;
	//! Null when the core cannot answer for the history, in which case the
	//! Known tab is not offered at all.
	CClientHistoryListCtrl *historylistctrl = nullptr;

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

	/**
	 * Load the history if what we hold is not already current.
	 *
	 * The reply is the most expensive thing this page asks for -- a full walk
	 * of the credit store, which on a real node is tens of thousands of
	 * records -- and the sweep keeps the rows current once they are here, so
	 * re-entering the tab does not need to ask again.
	 *
	 * "Current" is decided by daemon session, not by a plain once-flag: this
	 * dialog survives a reconnect (CamuleRemoteGuiApp::FinishReconnect resets
	 * the containers, not the frame), so a core that restarted underneath us
	 * leaves rows whose live state belongs to a process that no longer exists.
	 * A session we cannot identify is treated as a new one, which costs a
	 * reload rather than showing something stale.
	 */
	void EnsureHistoryLoaded();

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
	//! Daemon session the history was loaded for; 0 until it has been.
	uint64 m_historySessionId = 0;
#endif
	//! Whether the history has been loaded at all in this dialog's lifetime.
	bool m_historyLoaded = false;
};

#endif // CLIENTSWND_H
// File_checked_for_headers
