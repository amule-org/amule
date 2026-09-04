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

#ifndef FRIENDLISTCTRL_H
#define FRIENDLISTCTRL_H

#include "MuleVirtualDataViewCtrl.h" // Needed for CMuleVirtualDataViewCtrl
#include "MD4Hash.h"

#define COLUMN_FRIEND_NAME 0
//! Always empty. Absorbs the macOS trailing-column sizing; see
//! CMuleDataViewCtrl::AppendSpacerColumn().
#define COLUMN_FRIEND_SPACER 1

class wxString;
class CFriend;

class CFriendListCtrl : public CMuleVirtualDataViewCtrl
{
public:
	CFriendListCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize siz, int flags);
	~CFriendListCtrl();

	void UpdateFriend(CFriend *toupdate);
	void RemoveFriend(CFriend *todel);

protected:
	/// Text of the one real column, pulled on demand for the rows being drawn.
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;

	/// Blue for a linked friend, default text colour for the rest.
	bool GetItemAttr(wxUIntPtr item, unsigned column, wxDataViewItemAttr &attr) const override;

	/// Case-insensitive name comparison for the base's sort chain.
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;

	/// The name is the only column, and it can change after insertion (a
	/// friend arrives before its name does), so a refresh may need to move
	/// the row under a name sort.
	bool IsLiveSortColumn() const override { return true; }

	/**
	 * Delete removes the selected friends; see CMuleDataViewCtrl::OnListKey.
	 */
	bool OnListKey(wxKeyEvent &event) override;

	wxDECLARE_EVENT_TABLE();

	void OnItemRightClicked(wxDataViewEvent &event);

private:
	void OnItemActivated(wxDataViewEvent &event);

	//! Open a chat with a friend, reporting the reason when there is no
	//! address to reach them on yet. Shared by double-click and the menu.
	void MessageFriend(CFriend *cur_friend);

	// Menu Items
	void OnShowDetails(wxCommandEvent &event);
	void OnSendMessage(wxCommandEvent &event);
	void OnRemoveFriend(wxCommandEvent &event);
	void OnSetFriendslot(wxCommandEvent &event);
	void OnAddFriend(wxCommandEvent &event);
	void OnViewFiles(wxCommandEvent &event);
};

#endif
// File_checked_for_headers
