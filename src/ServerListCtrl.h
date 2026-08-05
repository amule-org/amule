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

#include <wx/dataview.h> // Needed for wxDataViewCtrl

#include "ListColumnStore.h" // Needed for CListColumnStore, IColumnWidthProvider
#include "Types.h"           // Needed for uint64

#include <list>
#include <map>
#include <utility>

class CServer;
class CServerListModel;

/**
 * The CServerListCtrl is used to display the list of servers which the user
 * can connect to and which we request sources from. It is a permanently
 * sorted list in that it always ensures that the items are sorted in the
 * correct order.
 *
 * Backed by a wxDataViewCtrl (native control) rather than a wxListCtrl, for
 * screen-reader accessibility (#180/#801). The list is flat -- every server
 * is a top-level row, no grouping -- so CServerListModel is a plain
 * pointer-keyed table rather than a tree, unlike CSearchListModel.
 */
class CServerListCtrl : public wxDataViewCtrl
{
public:
	/**
	 * Constructor.
	 */
	CServerListCtrl(wxWindow *parent,
		wxWindowID winid = -1,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		const wxString &name = "serverlistctrl");

	/**
	 * Destructor.
	 */
	virtual ~CServerListCtrl();

	/**
	 * Adds a server to the list.
	 *
	 * @param toadd A pointer to the new server.
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
	 * Removes all servers, or only the currently selected ones.
	 *
	 * @param selectedOnly If true, only the selected servers are removed.
	 */
	void RemoveAllServers(bool selectedOnly = false);

	/**
	 * Removes every row without the confirmation/static-server checks
	 * RemoveAllServers() does -- used when the core's own list is reset out
	 * from under this control (e.g. a full server.met reload).
	 */
	void DeleteAllItems();

	/**
	 * Updates the displayed information on a server.
	 *
	 * @param server The server to be updated.
	 *
	 * This function will not only update the displayed information, it will
	 * also reposition the item should it be necessary to enforce the current
	 * sorting. Also note that this function does not require that the server
	 * actually is on the list already, since AddServer makes use of it, but
	 * this should generally be avoided, since it will result in the
	 * server-count getting skewed until the next AddServer call.
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

	//! True if `server` is the one currently highlighted via HighlightServer().
	bool IsConnected(const CServer *server) const { return server && server == m_connected; }

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

	/**
	 * Full comparison of two servers according to this list's current sort
	 * column/direction. Used by CServerListModel::Compare().
	 */
	int CompareServers(const CServer *s1, const CServer *s2) const;

	/**
	 * Index of `code`'s flag, loading the bitmap on first use. Returns an
	 * invalid wxIcon for a code with no bundled flag.
	 */
	wxIcon FlagIcon(const wxString &code) const;

protected:
	/// Return old column order.
	wxString GetOldColumnOrder() const;

	/**
	 * Adapts this control to IColumnWidthProvider for CListColumnStore. See
	 * CSearchListCtrl::ColumnWidthAdapter for why this is a separate object
	 * rather than multiple inheritance.
	 */
	class ColumnWidthAdapter : public IColumnWidthProvider
	{
	public:
		explicit ColumnWidthAdapter(wxDataViewCtrl *ctrl)
		: m_ctrl(ctrl)
		{
		}
		int GetColumnCount() const override { return static_cast<int>(m_ctrl->GetColumnCount()); }
		int GetColumnWidth(int col) const override { return m_ctrl->GetColumn(col)->GetWidth(); }
		bool SetColumnWidth(int col, int width) override
		{
			m_ctrl->GetColumn(col)->SetWidth(width);
			return true;
		}

	private:
		wxDataViewCtrl *m_ctrl;
	};
	ColumnWidthAdapter m_widthAdapter;

	typedef std::pair<unsigned, unsigned> CColPair;
	typedef std::list<CColPair> CSortingList;

	//! Sort chain: front() is the primary sort column/order. Order values
	//! reuse CMuleListCtrl::SORT_DES's bit value so the persisted config
	//! stays wire-compatible (see ListColumnStore.cpp).
	CSortingList m_sort_orders;

	//! Single-column comparison, ported from the old CServerListCtrl::SortProc.
	//! No column offers an alternate tie-break criterion here (unlike
	//! Search's Sources column) -- the old SortProc never consulted a
	//! SORT_ALT bit for any Servers column, so there is no `alt`/AltSortAllowed
	//! parameter to thread through.
	int CompareByColumn(const CServer *s1, const CServer *s2, unsigned column, int modifier) const;

	CListColumnStore m_columnStore;
	void LoadColumnSettings();
	void SaveColumnSettings();
	//! Applies `order` to `column`, moving it to the front of the sort
	//! chain, sets the native column header sort indicator, and re-sorts.
	void ApplySorting(unsigned column, unsigned order);

	CServerListModel *m_model;

	void OnIdle(wxIdleEvent &event);
	void OnColumnHeaderClick(wxDataViewEvent &event);
	void OnRightClick(wxDataViewEvent &event);
	void OnItemActivated(wxDataViewEvent &event);

	void OnPriorityChange(wxCommandEvent &event);
	void OnStaticChange(wxCommandEvent &event);
	void OnConnectToServer(wxCommandEvent &event);
	void OnGetED2kURL(wxCommandEvent &event);
	void OnRemoveServers(wxCommandEvent &event);

	/**
	 * Type-to-select plus Delete-to-remove, reimplemented for
	 * wxDataViewCtrl -- see CSearchListCtrl::OnChar for the full rationale
	 * (none of the backends provide type-ahead-jump the way CMuleListCtrl's
	 * did). The Delete-key handling that used to be CServerListCtrl's own
	 * separate EVT_CHAR handler (OnKeyPressed) is folded in here rather than
	 * kept as a second binding.
	 */
	void OnChar(wxKeyEvent &evt);

	//! Keystrokes accumulated so far, lowercased; reset after kTypeAheadResetMs.
	wxString m_ttsText;
	//! GetTickCount64() of the last accepted keystroke.
	uint64 m_ttsTime = 0;
	//! Result the last match landed on, so repeats cycle to the next one.
	CServer *m_ttsItem = nullptr;

	//! Used to keep track of the last high-lighted item.
	const CServer *m_connected;

	//! ISO code -> flag icon, filled in lazily from GetValue()'s COL_NAME
	//! case in CServerListModel (which is otherwise const, hence mutable).
	//! Loading all ~250 flags up front instead would decode a PNG for every
	//! country nobody is connected to.
	mutable std::map<wxString, wxIcon> m_flagIcons;

	wxDECLARE_EVENT_TABLE();
};

#endif // SERVERLISTCTRL_H
// File_checked_for_headers
