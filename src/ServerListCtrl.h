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

#ifndef SERVERLISTCTRL_H
#define SERVERLISTCTRL_H

#include "MuleVirtualDataViewCtrl.h" // Needed for CMuleVirtualDataViewCtrl

#include <map>

#define COLUMN_SERVER_NAME 0
#define COLUMN_SERVER_ADDR 1
#define COLUMN_SERVER_PORT 2
#define COLUMN_SERVER_DESC 3
#define COLUMN_SERVER_PING 4
#define COLUMN_SERVER_USERS 5
#define COLUMN_SERVER_MAXUSERS 6
#define COLUMN_SERVER_FILES 7
#define COLUMN_SERVER_PRIO 8
#define COLUMN_SERVER_FAILS 9
#define COLUMN_SERVER_STATIC 10
#define COLUMN_SERVER_VERSION 11
// These come before the wire-flag columns on purpose: the flag columns are not
// appended at all in the remote GUI (see the CLIENT_GUI gate in
// ServerListCtrl.cpp), and the model treats any id at or above
// RealColumnCount() -- the count of *appended* columns -- as out of range and
// renders it blank.
//
// The ids must also stay in the same order the columns are appended in.
// FitColumnsToContent() walks one index and uses it as both a model column and
// a view position, so a mismatch sizes each column against another one's
// content -- which reads as columns collapsing to nothing.
#define COLUMN_SERVER_SOFTFILES 12
#define COLUMN_SERVER_HARDFILES 13
#define COLUMN_SERVER_TCPFLAGS 14
#define COLUMN_SERVER_UDPFLAGS 15

class CServer;
class CServerList;
class wxListEvent;
class wxCommandEvent;

/**
 * The CServerListCtrl is used to display the list of servers which the user
 * can connect to and which we request sources from. It is a permanently sorted
 * list in that it always ensure that the items are sorted in the correct order.
 *
 * The rows are text-rendered from the model (GetItemColumnText), not
 * owner-drawn: the only graphic is the country flag in the Server Name column,
 * supplied through GetItemIcon().
 */
class CServerListCtrl : public CMuleVirtualDataViewCtrl
{
public:
	/**
	 * Constructor.
	 *
	 * @see CMuleListCtrl::CMuleListCtrl
	 */
	CServerListCtrl(wxWindow *parent,
		wxWindowID winid = wxID_ANY,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxString &name = "serverlistctrl");

	/**
	 * Destructor.
	 */
	virtual ~CServerListCtrl();

	/**
	 * Adds a server to the list.
	 *
	 * @param A pointer to the new server.
	 *
	 * Internally this function calls RefreshServer and ShowServerCount, with
	 * the result that it is legal to add servers already in the list, though
	 * not recommended.
	 */
	void AddServer(CServer *toadd);

	/**
	 * Removes a server from the displayed list.
	 */
	void RemoveServer(CServer *server);

	/**
	 * Removes servers from the list and from the core.
	 *
	 * @param selectedOnly Only the selected rows, rather than every server.
	 */
	void RemoveAllServers(bool selectedOnly = false);

	/**
	 * Updates the displayed information on a server.
	 *
	 * @param server The server to be updated.
	 *
	 * This function will not only update the displayed information, it will also
	 * reposition the item should it be nescecarry to enforce the current sorting.
	 * Also note that this function does not require that the server actually is
	 * on the list already, since AddServer makes use of it, but this should
	 * generally be avoided, since it will result in the server-count getting
	 * skewed until the next AddServer call.
	 */
	void RefreshServer(CServer *server);

	/**
	 * Sets the highlighting of the specified server.
	 *
	 * @param server The server to have its highlighting set.
	 * @param highlight The new highlighting state.
	 *
	 * Please note that only _one_ item is allowed to be highlighted at any
	 * one time, so calling this function while another item is already
	 * highlighted will result in the old item not being highlighted any more.
	 */
	void HighlightServer(const CServer *server, bool highlight);

	/**
	 * This function updates the server-count in the server-wnd.
	 */
	void ShowServerCount();

	/**
	 * Resize every visible column to fit its content (and at least its
	 * header). The Description column is capped so a very long description
	 * cannot dominate the list. Meant to be called once after a bulk
	 * (re)load, not on every per-server refresh.
	 */
	void FitColumnsToContent();

protected:
	/// Return old column order.
	wxString GetOldColumnOrder() const override;

	/// Text of one cell, pulled on demand for the cells being drawn.
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;

	/// Host-country flag on the Server Name column, nothing elsewhere.
	bool GetItemIcon(wxUIntPtr item, unsigned column, wxIcon &icon) const override;

	/// Bold for the server we are connected to, default for the rest.
	bool GetItemAttr(wxUIntPtr item, unsigned column, wxDataViewItemAttr &attr) const override;

	/**
	 * Ping, Users and Files change while the list is up, so sorting by one of
	 * them enables the inherited live auto-sort.
	 */
	bool IsLiveSortColumn() const override;

	/// Single-column comparison for the base's sort chain.
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;

private:
	/**
	 * Event-handler for handling item activation (connect).
	 */
	void OnItemActivated(wxDataViewEvent &event);

	/**
	 * Event-handler for displaying the popup-menu.
	 */
	void OnItemRightClicked(wxDataViewEvent &event);

	/**
	 * Event-handler for priority changes.
	 */
	void OnPriorityChange(wxCommandEvent &event);

	/**
	 * Event-handler for static changes.
	 */
	void OnStaticChange(wxCommandEvent &event);

	/**
	 * Event-handler for server connections.
	 */
	void OnConnectToServer(wxCommandEvent &event);

	/**
	 * Event-handler for copying server-urls to the clipboard.
	 */
	void OnGetED2kURL(wxCommandEvent &event);

	/**
	 * Event-handler for server removal.
	 */
	void OnRemoveServers(wxCommandEvent &event);

	/**
	 * Delete key removes the selected servers; see CMuleDataViewCtrl::OnListKey.
	 */
	bool OnListKey(wxKeyEvent &event) override;

	/**
	 * @a code's flag, decoded on first use. An unknown code yields an
	 * invalid icon, which draws as no icon at all.
	 */
	const wxIcon &FlagIcon(const wxString &code) const;

	//! Used to keep track of the last high-lighted item.
	const CServer *m_connected;

	/**
	 * ISO code -> flag icon, filled in lazily.
	 *
	 * Mutable because the flags are decoded from GetItemIcon(), which the
	 * control calls to paint a row and is therefore const. Loading all ~250
	 * up front instead would decode a PNG for every country nobody is
	 * connected to.
	 */
	mutable std::map<wxString, wxIcon> m_flagIcons;

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
