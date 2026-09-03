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

#ifndef CLIENTROWLISTCTRL_H
#define CLIENTROWLISTCTRL_H

#include <vector>

#include "ClientNameCell.h" // Needed for ClientNameCell
#include "PeerIdentity.h"   // Needed for PeerIdentity
#include "MuleVirtualDataViewCtrl.h"

/**
 * A list whose rows describe peers, with the behaviour that implies.
 *
 * The Clients page shows two such lists -- who we are talking to now, and
 * everyone we have ever talked to -- and they want the same things: the Name
 * cell drawn with its badge icons, the details dialog on double-click or Enter,
 * and the peer context menu on right-click. What differs is only how a row
 * names its peer: the active list keeps an ECID, the history keeps a user hash,
 * because an ECID means nothing once the daemon that issued it has restarted.
 *
 * So that difference is all a subclass supplies. Everything built on top of it
 * lives here once, which is also why the actions are reached through
 * ClientContextActions rather than reimplemented: the per-file client lists in
 * Downloads and Shared files run the same code from CGenericClientListCtrl.
 *
 * Rows hold values, never clients. A CClientRef would be owning, so a list
 * holding one would keep every peer it ever showed alive; the peer is resolved
 * only when something is actually done to it, and a row whose peer has since
 * gone still names it well enough to act on from the record alone.
 */
class CClientRowListCtrl : public CMuleVirtualDataViewCtrl
{
public:
	CClientRowListCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags);

protected:
	//! The Name cell for a row, or nullptr if the row has none.
	virtual const ClientNameCell *NameCellFor(wxUIntPtr item) const = 0;

	//! Which model column the Name occupies, so the bar fill goes to the right one.
	virtual unsigned NameColumn() const = 0;

	/**
	 * The peer behind one row, or false when the row names none usable.
	 *
	 * Identity comes from the row, so a peer we are not talking to is still
	 * named here -- `client` is simply unlinked for it. The two lists key on
	 * different things (ECID within a process, user hash across restarts),
	 * which is why resolving one row stays per-list while everything built
	 * on top of it is shared.
	 *
	 * Per row rather than per selection so the context menu, which describes
	 * a single row, can resolve just that one. Resolving the whole selection
	 * to read its first entry costs a full ClientDetailInfo and a client
	 * lookup for every other selected row, which is the entire credit store
	 * after a select-all.
	 */
	virtual bool PeerForItem(wxUIntPtr data, PeerIdentity &out) const = 0;

	/**
	 * The peer the context menu was built for, resolved afresh.
	 *
	 * False when no menu has been opened or the row has since gone.
	 */
	bool MenuPeer(PeerIdentity &out) const;

	//! The peers behind the current selection, in display order.
	std::vector<PeerIdentity> SelectedPeers() const;

	void GetItemBarFill(wxUIntPtr data, unsigned column, CBarFillSpec &out) const override;

	wxDECLARE_EVENT_TABLE();

private:
	void OnItemActivated(wxDataViewEvent &event);
	void ShowDetailsForSelection();
	/**
	 * Whether a bulk action over `count` rows should go ahead.
	 *
	 * True without asking for a small selection. Larger ones are confirmed,
	 * because the history list is the whole credit store and the menu was
	 * built for a single row.
	 */
	bool ConfirmBulkPeerAction(size_t count, const wxString &message);

	/**
	 * The row the context menu was last built for, as item data.
	 *
	 * Single-row entries resolve this rather than re-reading the selection,
	 * so the entry and the action can never describe different peers. Bulk
	 * entries still use the whole selection, which is what they are for.
	 *
	 * Deliberately the row and not a resolved PeerIdentity: that holds an
	 * owning CClientRef, and a member would keep a disconnected peer's
	 * CUpDownClient alive for the lifetime of the control, which is exactly
	 * what this class holds values rather than clients to avoid. Resolving
	 * again also cannot go stale: a row that has since gone simply fails to
	 * resolve and the entry does nothing.
	 */
	wxUIntPtr m_menuItem = 0;
	bool m_menuItemValid = false;

	void OnItemRightClicked(wxDataViewEvent &event);
	void OnViewFiles(wxCommandEvent &event);
	void OnAddFriend(wxCommandEvent &event);
	void OnSetFriendslot(wxCommandEvent &event);
	void OnSendMessage(wxCommandEvent &event);
	void OnViewClientInfo(wxCommandEvent &event);
};

#endif // CLIENTROWLISTCTRL_H
// File_checked_for_headers
