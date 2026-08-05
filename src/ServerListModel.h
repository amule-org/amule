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

#ifndef SERVERLISTMODEL_H
#define SERVERLISTMODEL_H

#include <wx/dataview.h>

#include <unordered_set>
#include <vector>

class CServer;
class CServerListCtrl;

/**
 * wxDataViewModel backing a CServerListCtrl. Flat (no grouping, unlike
 * CSearchListModel's parent/child forest): every server is a top-level row,
 * keyed by CServer* identity so the control's existing pointer-based API
 * (AddServer(CServer*), RemoveServer(CServer*), HighlightServer(const
 * CServer*, bool)) carries over without a row-index translation layer.
 *
 * Unlike CSearchListModel, this model owns its row set directly rather than
 * querying a live core-side collection: CServerListCtrl has always kept its
 * own displayed snapshot independent of CServerList's storage (see
 * RemoveAllServers()'s "drop the row before asking for removal" comment),
 * and that decoupling is preserved here.
 *
 * All mutations (AddServer/RemoveServer/RefreshServer/ClearAll) just mark
 * the model dirty; the control flushes one Cleared() per idle
 * (CServerListCtrl::OnIdle), same as CSearchListModel -- mixing incremental
 * Item* notifications with a full reset left wxGTK/wxMSW's tree
 * inconsistent for CSearchListCtrl (got3nks, PR #796 review), and a flat
 * list is exposed to the exact same backends.
 */
class CServerListModel : public wxDataViewModel
{
public:
	explicit CServerListModel(CServerListCtrl *owner);

	//! Adds `server` if not already present, else a no-op (AddServer() on
	//! the control has always tolerated re-adding a listed server).
	void AddServer(CServer *server);
	void RemoveServer(CServer *server);
	//! Value-only change (no shape change) -- still funnels through the
	//! same dirty-flag/idle-flush path as Add/Remove, see class comment.
	void RefreshServer(CServer *server);
	void ClearAll();
	bool HasServer(CServer *server) const;
	size_t GetCount() const { return m_servers.size(); }
	const std::vector<CServer *> &GetServers() const { return m_servers; }

	void MarkDirty() { m_pendingReset = true; }
	bool FlushPending();
	bool HasPending() const { return m_pendingReset; }

	static CServer *ToServer(const wxDataViewItem &item) { return static_cast<CServer *>(item.GetID()); }
	static wxDataViewItem ToItem(const CServer *server)
	{
		return wxDataViewItem(const_cast<CServer *>(server));
	}

	//! Column indices, matching the wxDataViewColumn order set up by
	//! CServerListCtrl -- shared with CServerListCtrl::CompareServers().
	enum Column
	{
		COL_NAME = 0,
		COL_ADDR,
		COL_PORT,
		COL_DESC,
		COL_PING,
		COL_USERS,
		COL_FILES,
		COL_PRIO,
		COL_FAILS,
		COL_STATIC,
		COL_VERSION,
		COL_TCPFLAGS,
		COL_UDPFLAGS,
		COL_COUNT
	};

	// wxDataViewModel interface
	unsigned int GetColumnCount() const wxOVERRIDE;
	wxString GetColumnType(unsigned int col) const wxOVERRIDE;
	void GetValue(wxVariant &variant, const wxDataViewItem &item, unsigned int col) const wxOVERRIDE;
	bool SetValue(const wxVariant &variant, const wxDataViewItem &item, unsigned int col) wxOVERRIDE;
	bool GetAttr(const wxDataViewItem &item, unsigned int col, wxDataViewItemAttr &attr) const wxOVERRIDE;
	wxDataViewItem GetParent(const wxDataViewItem &item) const wxOVERRIDE;
	bool IsContainer(const wxDataViewItem &item) const wxOVERRIDE;
	unsigned int GetChildren(const wxDataViewItem &item, wxDataViewItemArray &children) const wxOVERRIDE;
	int Compare(const wxDataViewItem &item1,
		const wxDataViewItem &item2,
		unsigned int column,
		bool ascending) const wxOVERRIDE;

private:
	CServerListCtrl *m_owner;
	std::vector<CServer *> m_servers;
	std::unordered_set<CServer *> m_index;
	bool m_pendingReset = false;
};

#endif // SERVERLISTMODEL_H
// File_checked_for_headers
