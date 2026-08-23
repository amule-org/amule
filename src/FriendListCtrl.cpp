//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "FriendListCtrl.h" // Interface declarations

#include <vector> // Needed for std::vector (snapshot of selected friends)

#include <common/MenuIDs.h>
#include <common/MacrosProgramSpecific.h>

#include "amule.h"              // Needed for theApp
#include "amuleDlg.h"           // Needed for CamuleDlg
#include "ClientDetailDialog.h" // Needed for CClientDetailDialog
#include "AddFriend.h"          // Needed for CAddFriend
#include "ChatWnd.h"            // Needed for CChatWnd
#include "Friend.h"             // Needed for CFriend
#include "SearchDlg.h"          // Needed for CSearchDlg (View Files browse tab)
#include "muuli_wdr.h"
#include "SafeFile.h"
#include "FriendList.h" // Needed for the friends list

wxBEGIN_EVENT_TABLE(CFriendListCtrl, CMuleVirtualDataViewCtrl)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CFriendListCtrl::OnItemRightClicked)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CFriendListCtrl::OnItemActivated)

	EVT_MENU(MP_MESSAGE, CFriendListCtrl::OnSendMessage)
	EVT_MENU(MP_REMOVEFRIEND, CFriendListCtrl::OnRemoveFriend)
	EVT_MENU(MP_ADDFRIEND, CFriendListCtrl::OnAddFriend)
	EVT_MENU(MP_DETAIL, CFriendListCtrl::OnShowDetails)
	EVT_MENU(MP_SHOWLIST, CFriendListCtrl::OnViewFiles)
	EVT_MENU(MP_FRIENDSLOT, CFriendListCtrl::OnSetFriendslot)
wxEND_EVENT_TABLE()

CFriendListCtrl::CFriendListCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize siz, int flags)
: CMuleVirtualDataViewCtrl(parent, id, pos, siz, flags)
{
	AddTextColumn(_("Username"),
		COLUMN_FRIEND_NAME,
		"N",
		siz.GetWidth() - 4,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);

	// Absorbs the macOS trailing-column sizing; the model answers any column
	// past the real one with an empty value.
	AppendSpacerColumn(COLUMN_FRIEND_SPACER);
	AssociateVirtualModel();

	// One-letter name so CListColumnStore has something to write under
	// /eMule/TableWidthsFriend.

	ApplySorting(COLUMN_FRIEND_NAME, 0);

	m_columnStore.SetTableName("Friend");
	LoadColumnSettings();
	InitColumnState();
}

CFriendListCtrl::~CFriendListCtrl() {}

void CFriendListCtrl::RemoveFriend(CFriend *toremove)
{
	if (!toremove) {
		return;
	}

	const wxUIntPtr ptr = reinterpret_cast<wxUIntPtr>(toremove);
	if (HasItemData(ptr)) {
		RemoveItemData(ptr);
	}
}

void CFriendListCtrl::UpdateFriend(CFriend *toupdate)
{
	if (!toupdate) {
		return;
	}

	const wxUIntPtr ptr = reinterpret_cast<wxUIntPtr>(toupdate);
	if (HasItemData(ptr)) {
		// The cell is rendered from the friend on demand, so a refresh is
		// just a repaint (plus a re-sort if the name changed under a
		// name-sorted list).
		RefreshItemData(ptr);
	} else {
		AddItemData(ptr);
	}
}

wxString CFriendListCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	if (column != COLUMN_FRIEND_NAME) {
		return wxEmptyString;
	}
	return reinterpret_cast<const CFriend *>(item)->GetName();
}

bool CFriendListCtrl::GetItemAttr(wxUIntPtr item, unsigned WXUNUSED(column), wxDataViewItemAttr &attr) const
{
	// Linked friends stay visually distinguished in blue; unlinked ones fall
	// back to the default (system) text colour so they don't render
	// invisible on dark themes (#640).
	const CFriend *cur_friend = reinterpret_cast<const CFriend *>(item);
	if (cur_friend->GetLinkedClient().IsLinked()) {
		attr.SetColour(*wxBLUE);
		return true;
	}
	return false;
}

int CFriendListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	if (column != COLUMN_FRIEND_NAME) {
		return 0;
	}
	const CFriend *f1 = reinterpret_cast<const CFriend *>(data1);
	const CFriend *f2 = reinterpret_cast<const CFriend *>(data2);
	return modifier * f1->GetName().CmpNoCase(f2->GetName());
}

void CFriendListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	// Open a session with the activated row alone, whatever else was selected.
	// event.GetItem()'s ID is the row-addressed model's row index, not the
	// item data -- see the "item identity is not row identity" note in
	// MuleVirtualDataViewCtrl.h -- so the friend has to be resolved through
	// the selection rather than cast directly from the item.
	if (event.GetItem().IsOk()) {
		UnselectAll();
		Select(event.GetItem());
	}

	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (selected.empty()) {
		return;
	}

	theApp->amuledlg->m_chatwnd->StartSession(reinterpret_cast<CFriend *>(selected.front()));
}

void CFriendListCtrl::OnItemRightClicked(wxDataViewEvent &event)
{
	// Right-clicking a row outside the selection acts on that row alone.
	if (event.GetItem().IsOk()) {
		wxDataViewItemArray selection;
		GetSelections(selection);
		if (selection.Index(event.GetItem()) == wxNOT_FOUND) {
			UnselectAll();
			Select(event.GetItem());
		}
	}

	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	CFriend *cur_friend = selected.empty() ? nullptr : reinterpret_cast<CFriend *>(selected.front());

	wxMenu *menu = new wxMenu(_("Friends"));

	if (cur_friend) {
		menu->Append(MP_DETAIL, _("Show &Details"));
		menu->Enable(MP_DETAIL, cur_friend->GetLinkedClient().IsLinked());
	}

	menu->Append(MP_ADDFRIEND, _("Add a friend"));

	if (cur_friend) {
		menu->Append(MP_REMOVEFRIEND, _("Remove Friend"));
		menu->Append(MP_MESSAGE, _("Send &Message"));
		menu->Append(MP_SHOWLIST, _("View Files"));
		menu->AppendCheckItem(MP_FRIENDSLOT, _("Establish Friend Slot"));
		if (cur_friend->GetLinkedClient().IsLinked()) {
			menu->Enable(MP_FRIENDSLOT, true);
			menu->Check(MP_FRIENDSLOT, cur_friend->HasFriendSlot());
		} else {
			menu->Enable(MP_FRIENDSLOT, false);
		}
	}

	PopupMenu(menu, event.GetPosition());
	delete menu;
}

void CFriendListCtrl::OnSendMessage(wxCommandEvent &WXUNUSED(event))
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		CFriend *cur_friend = reinterpret_cast<CFriend *>(data);
		theApp->amuledlg->m_chatwnd->StartSession(cur_friend);
// #warning CORE/GUI!
#ifndef CLIENT_GUI
		theApp->friendlist->StartChatSession(cur_friend);
#endif
	}
}

void CFriendListCtrl::OnRemoveFriend(wxCommandEvent &WXUNUSED(event))
{
	wxString question;
	if (GetSelectedItemsCount() == 1) {
		question = _("Are you sure that you wish to delete the selected friend?");
	} else {
		question = _("Are you sure that you wish to delete the selected friends?");
	}

	if (wxMessageBox(question, _("Cancel"), wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT, this) == wxYES) {
		// Collect the selected friends first, then remove them. On amuleGUI
		// RemoveFriend() is asynchronous (it only sends an EC request; the row is
		// dropped later, when the daemon pushes the updated friend list), so the
		// removed friend stays selected in the list when RemoveFriend() returns.
		// Re-querying the selection in the loop would then keep finding the same
		// friend and resend the request forever, pegging the CPU (the tight loop
		// never yields to process the daemon's update). Snapshot the selection up
		// front so removal is correct whether it is synchronous (monolithic) or
		// asynchronous (remote GUI).
		const std::vector<wxUIntPtr> selected = GetSelectedItemData();

		for (wxUIntPtr data : selected) {
			theApp->friendlist->RemoveFriend(reinterpret_cast<CFriend *>(data));
		}
	}
}

void CFriendListCtrl::OnAddFriend(wxCommandEvent &WXUNUSED(event))
{
	CAddFriend(this).ShowModal();
}

void CFriendListCtrl::OnShowDetails(wxCommandEvent &WXUNUSED(event))
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		CFriend *cur_friend = reinterpret_cast<CFriend *>(data);
		if (cur_friend->GetLinkedClient().IsLinked()) {
			CClientDetailDialog(this, cur_friend->GetLinkedClient()).ShowModal();
		}
	}
}

void CFriendListCtrl::OnViewFiles(wxCommandEvent &WXUNUSED(event))
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		CFriend *cur_friend = reinterpret_cast<CFriend *>(data);
		// If this friend's listing is already open in the Search panel, switch
		// to that tab instead of re-requesting -- a second request would
		// duplicate the results in the existing tab.
		//
		// Which ECID keys that tab depends on who opened it, so both are
		// tried. The monolithic opens it from Notify_Browse_Started, which
		// carries the browsed CLIENT's ECID; amulegui opens it from
		// SendBrowseRequest, which has only the FRIEND's -- a friend need not
		// be linked to a client at the moment the browse is asked for. They
		// are different numbers from one counter, so matching on the client
		// alone never found amulegui's tab, and every click re-asked the peer.
		CSearchDlg *const searchwnd = theApp->amuledlg ? theApp->amuledlg->m_searchwnd : nullptr;
		const CClientRef &linked = cur_friend->GetLinkedClient();
		const uint32 clientEcid = linked.IsLinked() ? linked.ECID() : 0;
		if (!(searchwnd && (searchwnd->ActivateBrowseTabIfOpen(clientEcid) ||
					   searchwnd->ActivateBrowseTabIfOpen(cur_friend->ECID())))) {
			theApp->friendlist->RequestSharedFileList(cur_friend);
		}
	}
}

void CFriendListCtrl::OnSetFriendslot(wxCommandEvent &event)
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (selected.empty()) {
		return;
	}

	theApp->friendlist->SetFriendSlot(reinterpret_cast<CFriend *>(selected.front()), event.IsChecked());

	if (selected.size() > 1) {
		wxMessageBox(_("You are not allowed to set more than one friendslot.\n Only one slot was "
			       "assigned."),
			_("Multiple selection"),
			wxOK | wxICON_ERROR,
			this);
	}
}

bool CFriendListCtrl::OnListKey(wxKeyEvent &event)
{
	// Delete removes the selected friends; everything else belongs to the
	// control's own navigation.
	if ((event.GetKeyCode() == WXK_DELETE) || (event.GetKeyCode() == WXK_NUMPAD_DELETE)) {
		if (ItemDataCount()) {
			wxCommandEvent evt;
			evt.SetId(MP_REMOVEFRIEND);
			OnRemoveFriend(evt);
		}
		return true;
	}
	return false;
}
// File_checked_for_headers
