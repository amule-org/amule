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

#include "ServerListCtrl.h" // Interface declarations

#include <algorithm> // Needed for std::find, std::max, std::sort
#include <vector>    // Needed for std::vector

#include <common/MenuIDs.h>

#include <wx/dcclient.h> // Needed for wxClientDC
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/stattext.h>

#include "amule.h"         // Needed for theApp
#include "DownloadQueue.h" // Needed for CDownloadQueue
#ifdef GEOIP_GUI
#include "CountryFlags.h" // Needed for CCountryFlags (flag bitmaps)
#endif
#include "GetTickCount.h"    // Needed for GetTickCount64()
#include "Server.h"          // Needed for CServer and SRV_PR_*
#include "ServerConnect.h"   // Needed for CServerConnect
#include "ServerList.h"      // Needed for CServerList
#include "ServerListModel.h" // Needed for CServerListModel
#include <common/Format.h>   // Needed for CFormat

// One fixed size for the country flags, padded onto a transparent cell of
// this size (see FlagIcon).
static const int LIST_IMAGE_SIZE = 16;

// MLOrder-compatible bit value, reused from CMuleListCtrl so the persisted
// "TableOrderingServer" config entries stay wire-compatible (see
// ListColumnStore.cpp, which already hardcodes this same value).
namespace
{
const unsigned SORT_DES = 0x1000;
} // namespace

wxBEGIN_EVENT_TABLE(CServerListCtrl, wxDataViewCtrl)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CServerListCtrl::OnRightClick)
	EVT_DATAVIEW_COLUMN_HEADER_CLICK(wxID_ANY, CServerListCtrl::OnColumnHeaderClick)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CServerListCtrl::OnItemActivated)
	EVT_IDLE(CServerListCtrl::OnIdle)
	EVT_CHAR(CServerListCtrl::OnChar)

	EVT_MENU(MP_PRIOLOW, CServerListCtrl::OnPriorityChange)
	EVT_MENU(MP_PRIONORMAL, CServerListCtrl::OnPriorityChange)
	EVT_MENU(MP_PRIOHIGH, CServerListCtrl::OnPriorityChange)

	EVT_MENU(MP_ADDTOSTATIC, CServerListCtrl::OnStaticChange)
	EVT_MENU(MP_REMOVEFROMSTATIC, CServerListCtrl::OnStaticChange)

	EVT_MENU(MP_CONNECTTO, CServerListCtrl::OnConnectToServer)

	EVT_MENU(MP_REMOVE, CServerListCtrl::OnRemoveServers)
	EVT_MENU(MP_REMOVEALL, CServerListCtrl::OnRemoveServers)

	EVT_MENU(MP_GETED2KLINK, CServerListCtrl::OnGetED2kURL)
wxEND_EVENT_TABLE()

CServerListCtrl::CServerListCtrl(
	wxWindow *parent, wxWindowID winid, const wxPoint &pos, const wxSize &size, const wxString &name)
: wxDataViewCtrl(parent, winid, pos, size, wxDV_ROW_LINES | wxDV_MULTIPLE, wxDefaultValidator, name)
, m_widthAdapter(this)
, m_connected(nullptr)
{
	// See CSearchListCtrl's ctor for why this is required for OnIdle to fire.
	SetExtraStyle(GetExtraStyle() | wxWS_EX_PROCESS_IDLE);

	m_model = new CServerListModel(this);
	AssociateModel(m_model);
	m_model->DecRef(); // the control now holds the only reference

	AppendIconTextColumn(_("Server Name"),
		CServerListModel::COL_NAME,
		wxDATAVIEW_CELL_INERT,
		150,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Address"),
		CServerListModel::COL_ADDR,
		wxDATAVIEW_CELL_INERT,
		140,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Port"),
		CServerListModel::COL_PORT,
		wxDATAVIEW_CELL_INERT,
		25,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Description"),
		CServerListModel::COL_DESC,
		wxDATAVIEW_CELL_INERT,
		150,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Ping"),
		CServerListModel::COL_PING,
		wxDATAVIEW_CELL_INERT,
		25,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Users"),
		CServerListModel::COL_USERS,
		wxDATAVIEW_CELL_INERT,
		40,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Files"),
		CServerListModel::COL_FILES,
		wxDATAVIEW_CELL_INERT,
		45,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Priority"),
		CServerListModel::COL_PRIO,
		wxDATAVIEW_CELL_INERT,
		60,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Failed"),
		CServerListModel::COL_FAILS,
		wxDATAVIEW_CELL_INERT,
		40,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Static"),
		CServerListModel::COL_STATIC,
		wxDATAVIEW_CELL_INERT,
		40,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Version"),
		CServerListModel::COL_VERSION,
		wxDATAVIEW_CELL_INERT,
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);

	m_columnStore.RegisterColumn(CServerListModel::COL_NAME, 150, "N");
	m_columnStore.RegisterColumn(CServerListModel::COL_ADDR, 140, "A");
	m_columnStore.RegisterColumn(CServerListModel::COL_PORT, 25, "P");
	m_columnStore.RegisterColumn(CServerListModel::COL_DESC, 150, "D");
	m_columnStore.RegisterColumn(CServerListModel::COL_PING, 25, "p");
	m_columnStore.RegisterColumn(CServerListModel::COL_USERS, 40, "U");
	m_columnStore.RegisterColumn(CServerListModel::COL_FILES, 45, "F");
	m_columnStore.RegisterColumn(CServerListModel::COL_PRIO, 60, "r");
	m_columnStore.RegisterColumn(CServerListModel::COL_FAILS, 40, "f");
	m_columnStore.RegisterColumn(CServerListModel::COL_STATIC, 40, "S");
	m_columnStore.RegisterColumn(CServerListModel::COL_VERSION, 80, "V");

#if !defined(CLIENT_GUI)
	AppendTextColumn(_("TCP Flags"),
		CServerListModel::COL_TCPFLAGS,
		wxDATAVIEW_CELL_INERT,
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("UDP Flags"),
		CServerListModel::COL_UDPFLAGS,
		wxDATAVIEW_CELL_INERT,
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	m_columnStore.RegisterColumn(CServerListModel::COL_TCPFLAGS, 80, "t");
	m_columnStore.RegisterColumn(CServerListModel::COL_UDPFLAGS, 80, "u");
#if !defined(__DEBUG__)
	// Created with their default 80 width above, which is required to unhide
	// them on the context menu. Now, for Release builds, hide them by default.
	GetColumn(CServerListModel::COL_TCPFLAGS)->SetHidden(true);
	GetColumn(CServerListModel::COL_UDPFLAGS)->SetHidden(true);
#endif
#endif

	// Default sort is by name, ascending.
	m_sort_orders.emplace_back(CServerListModel::COL_NAME, 0);
	GetColumn(CServerListModel::COL_NAME)->SetSortOrder(true);

	m_columnStore.SetTableName("Server");
	LoadColumnSettings();
}

CServerListCtrl::~CServerListCtrl()
{
	SaveColumnSettings();
}

wxString CServerListCtrl::GetOldColumnOrder() const
{
	return "N,A,P,D,p,U,F,r,f,S,V,t,u";
}

void CServerListCtrl::LoadColumnSettings()
{
	if (!m_columnStore.HasTableName()) {
		return;
	}

	CListColumnStore::CSortingList decoded;
	m_columnStore.LoadSettings(m_widthAdapter, GetOldColumnOrder(), decoded);

	// LoadSettings() returns the orders primary-LAST -- see
	// CSearchListCtrl::LoadColumnSettings for the full explanation.
	m_sort_orders.clear();
	for (const CListColumnStore::CColPair &pair : decoded) {
		ApplySorting(pair.first, pair.second);
	}
	if (m_sort_orders.empty()) {
		ApplySorting(CServerListModel::COL_NAME, 0);
	}
}

void CServerListCtrl::SaveColumnSettings()
{
	if (!m_columnStore.HasTableName()) {
		return;
	}
	m_columnStore.SaveSettings(m_widthAdapter, m_sort_orders);
}

void CServerListCtrl::AddServer(CServer *toadd)
{
	// RefreshServer will add the server.
	// This also means that we have simple duplicity checking. ;)
	RefreshServer(toadd);

	ShowServerCount();
}

void CServerListCtrl::RemoveServer(CServer *server)
{
	if (!m_model->HasServer(server)) {
		return;
	}
	if (server == m_connected) {
		m_connected = nullptr;
	}
	m_model->RemoveServer(server);
	ShowServerCount();
}

void CServerListCtrl::RemoveAllServers(bool selectedOnly)
{
	// Collect first, delete second: the confirmations below run a nested
	// event loop, during which the list can be updated underneath a server
	// pointer but the pointer itself stays a stable identity to check.
	std::vector<CServer *> candidates;
	if (selectedOnly) {
		wxDataViewItemArray selections;
		GetSelections(selections);
		for (const wxDataViewItem &item : selections) {
			candidates.push_back(CServerListModel::ToServer(item));
		}
	} else {
		candidates = m_model->GetServers();
	}

	const bool connected = theApp->IsConnectedED2K() || theApp->serverconnect->IsConnecting();

	for (CServer *server : candidates) {
		// May have gone away while a message box was up.
		if (!m_model->HasServer(server)) {
			continue;
		}

		if (server == m_connected && connected) {
			wxMessageBox(_("You are connected to a server you are trying to delete. Please "
				       "disconnect first. The server was NOT deleted."),
				_("Info"),
				wxOK,
				this);
			continue;
		}

		if (server->IsStaticMember()) {
			const wxString name = (!server->GetListName() ? wxString(_("(Unknown name)"))
								      : server->GetListName());

			if (wxMessageBox(
				    CFormat(_("Are you sure you want to delete the static server %s")) % name,
				    _("Cancel"),
				    wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT,
				    this) != wxYES) {
				continue;
			}
			theApp->serverlist->SetStaticServer(server, false);
		}

		// Drop the row before asking for the removal, not as a result of it:
		// amulegui's CServerListRem::RemoveServer() only sends an EC command,
		// so the row would otherwise linger until the core's next update. In
		// the monolithic build the resulting Notify_ServerRemove() comes back
		// here and finds the row already gone.
		RemoveServer(server);
		theApp->serverlist->RemoveServer(server);
	}

	ShowServerCount();
}

void CServerListCtrl::DeleteAllItems()
{
	m_connected = nullptr;
	m_model->ClearAll();
	ShowServerCount();
}

void CServerListCtrl::RefreshServer(CServer *server)
{
	// Can't really refresh a NULL server
	if (!server) {
		return;
	}
	m_model->RefreshServer(server);
}

wxIcon CServerListCtrl::FlagIcon(const wxString &code) const
{
	const auto it = m_flagIcons.find(code);
	if (it != m_flagIcons.end()) {
		return it->second;
	}

#ifdef GEOIP_GUI
	const wxImage &flag = theApp->GetCountryFlags()->GetFlag(code);
	if (!flag.IsOk()) {
		m_flagIcons[code] = wxIcon();
		return wxIcon();
	}

	// Centre the 16x11 flag on a square cell. Adding it at its own size
	// instead would leave it non-uniform against the other rows' icons.
	wxImage cell = flag;
	if (!cell.HasAlpha()) {
		// Size() only pads with transparent pixels when there is an alpha
		// channel to be transparent in; without one it would pad with black.
		cell.InitAlpha();
	}
	cell = cell.Size(wxSize(LIST_IMAGE_SIZE, LIST_IMAGE_SIZE),
		wxPoint((LIST_IMAGE_SIZE - cell.GetWidth()) / 2, (LIST_IMAGE_SIZE - cell.GetHeight()) / 2));

	wxIcon icon;
	icon.CopyFromBitmap(wxBitmap(cell));
	m_flagIcons[code] = icon;
	return icon;
#else
	m_flagIcons[code] = wxIcon();
	return wxIcon();
#endif // GEOIP_GUI
}

void CServerListCtrl::HighlightServer(const CServer *server, bool highlight)
{
	// The bold attribute is handed out by CServerListModel::GetAttr(), so
	// all this has to do is move m_connected and repaint the rows either
	// side of the change.
	const CServer *previous = m_connected;

	if (highlight) {
		m_connected = server;
	} else if (m_connected == server) {
		m_connected = nullptr;
	}

	if (previous == m_connected) {
		return;
	}
	if (previous && m_model->HasServer(const_cast<CServer *>(previous))) {
		m_model->RefreshServer(const_cast<CServer *>(previous));
	}
	if (m_connected && m_model->HasServer(const_cast<CServer *>(m_connected))) {
		m_model->RefreshServer(const_cast<CServer *>(m_connected));
	}
}

void CServerListCtrl::ShowServerCount()
{
	wxStaticText *label = CastByName("serverListLabel", GetParent(), wxStaticText);

	if (label) {
		label->SetLabel(CFormat(_("Servers (%i)")) % m_model->GetCount());
		label->GetParent()->Layout();
	}
}

void CServerListCtrl::FitColumnsToContent()
{
	// Upper bound for the Description column: descriptions can be very
	// long (full forum URLs etc.), so cap it rather than let one row blow
	// the column out. The other columns hold short, bounded values.
	const int descMaxWidth = 300;
	const int autosizeMargin = 10;
	const int imageMargin = 5;

	wxClientDC dc(this);
	dc.SetFont(GetFont());

	// m_model->GetServers() is measured directly rather than waiting for a
	// pending Cleared() to flush: the vector itself is mutated eagerly by
	// AddServer/RemoveServer/RefreshServer, only the control's own
	// notification is idle-deferred, so this is already up to date even
	// right after a Freeze()/bulk-AddServer()/Thaw() reload.
	const std::vector<CServer *> &servers = m_model->GetServers();

	Freeze();
	for (int col = 0; col < GetColumnCount(); ++col) {
		wxDataViewColumn *column = GetColumn(col);
		if (column->IsHidden()) {
			continue;
		}

		int contentWidth = autosizeMargin;
		for (CServer *server : servers) {
			wxVariant value;
			m_model->GetValue(value, CServerListModel::ToItem(server), col);

			wxString text;
			int extraWidth = 0;
			if (col == CServerListModel::COL_NAME) {
				wxDataViewIconText iconText;
				iconText << value;
				text = iconText.GetText();
				if (iconText.GetIcon().IsOk()) {
					extraWidth = LIST_IMAGE_SIZE + imageMargin;
				}
			} else {
				text = value.GetString();
			}

			wxCoord textWidth = 0;
			dc.GetTextExtent(text, &textWidth, nullptr);
			int cellWidth = textWidth + extraWidth;
			if (cellWidth > contentWidth) {
				contentWidth = cellWidth;
			}
		}
		contentWidth += autosizeMargin;

		wxCoord headerTextWidth = 0;
		dc.GetTextExtent(column->GetTitle(), &headerTextWidth, nullptr);
		const int headerWidth = headerTextWidth + 2 * autosizeMargin; // padding + sort-arrow room

		int width = std::max(contentWidth, headerWidth);
		if (col == CServerListModel::COL_DESC && width > descMaxWidth) {
			width = descMaxWidth;
		}
		column->SetWidth(width);
	}
	Thaw();
}

void CServerListCtrl::OnIdle(wxIdleEvent &event)
{
	event.Skip();

	// One coalesced rebuild per idle -- see CSearchListCtrl::OnIdle for the
	// full rationale. There is no expand state to preserve here (flat list),
	// only selection.
	if (m_model->HasPending()) {
		wxDataViewItemArray selected;
		GetSelections(selected);

		m_model->FlushPending();

		wxDataViewItemArray live;
		m_model->GetChildren(wxDataViewItem(), live);
		wxDataViewItemArray restore;
		for (size_t i = 0; i < selected.GetCount(); ++i) {
			if (live.Index(selected[i]) != wxNOT_FOUND) {
				restore.Add(selected[i]);
			}
		}
		if (!restore.IsEmpty()) {
			SetSelections(restore);
		}
	}
}

int CServerListCtrl::CompareByColumn(
	const CServer *s1, const CServer *s2, unsigned column, int modifier) const
{
	switch (column) {
	case CServerListModel::COL_NAME:
		return modifier * s1->GetListName().CmpNoCase(s2->GetListName());

	case CServerListModel::COL_ADDR: {
		if (s1->HasDynIP() && s2->HasDynIP()) {
			return modifier * s1->GetDynIP().CmpNoCase(s2->GetDynIP());
		} else if (s1->HasDynIP()) {
			return modifier * -1;
		} else if (s2->HasDynIP()) {
			return modifier * 1;
		} else {
			uint32 a = wxUINT32_SWAP_ALWAYS(s1->GetIP());
			uint32 b = wxUINT32_SWAP_ALWAYS(s2->GetIP());
			return modifier * CmpAny(a, b);
		}
	}

	case CServerListModel::COL_PORT:
		return modifier * CmpAny(s1->GetPort(), s2->GetPort());

	case CServerListModel::COL_DESC:
		return modifier * s1->GetDescription().CmpNoCase(s2->GetDescription());

	// The -1 ensures that a value of zero (no ping known) is sorted last.
	case CServerListModel::COL_PING:
		return modifier * CmpAny(s1->GetPing() - 1, s2->GetPing() - 1);

	case CServerListModel::COL_USERS:
		return modifier * CmpAny(s1->GetUsers(), s2->GetUsers());

	case CServerListModel::COL_FILES:
		return modifier * CmpAny(s1->GetFiles(), s2->GetFiles());

	case CServerListModel::COL_PRIO: {
		uint32 srv_pr1 = s1->GetPreferences();
		uint32 srv_pr2 = s2->GetPreferences();
		switch (srv_pr1) {
		case SRV_PR_HIGH:
			srv_pr1 = SRV_PR_MAX;
			break;
		case SRV_PR_NORMAL:
			srv_pr1 = SRV_PR_MID;
			break;
		case SRV_PR_LOW:
			srv_pr1 = SRV_PR_MIN;
			break;
		default:
			return 0;
		}
		switch (srv_pr2) {
		case SRV_PR_HIGH:
			srv_pr2 = SRV_PR_MAX;
			break;
		case SRV_PR_NORMAL:
			srv_pr2 = SRV_PR_MID;
			break;
		case SRV_PR_LOW:
			srv_pr2 = SRV_PR_MIN;
			break;
		default:
			return 0;
		}
		return modifier * CmpAny(srv_pr1, srv_pr2);
	}

	case CServerListModel::COL_FAILS:
		return modifier * CmpAny(s1->GetFailedCount(), s2->GetFailedCount());

	case CServerListModel::COL_STATIC:
		return modifier * CmpAny(s2->IsStaticMember(), s1->IsStaticMember());

	case CServerListModel::COL_VERSION:
		return modifier * FuzzyStrCmp(s1->GetVersion(), s2->GetVersion());

	default:
		return 0;
	}
}

int CServerListCtrl::CompareServers(const CServer *s1, const CServer *s2) const
{
	for (const CColPair &entry : m_sort_orders) {
		const unsigned column = entry.first;
		const unsigned order = entry.second;
		const int modifier = (order & SORT_DES) ? -1 : 1;

		int result = CompareByColumn(s1, s2, column, modifier);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

void CServerListCtrl::ApplySorting(unsigned column, unsigned order)
{
	CSortingList::iterator it = m_sort_orders.begin();
	for (; it != m_sort_orders.end(); ++it) {
		if (it->first == column) {
			m_sort_orders.erase(it);
			break;
		}
	}
	m_sort_orders.emplace_front(column, order);

	// Unmark the previous sort column (only one wxDataViewColumn can be the
	// active sort key at a time; SetSortOrder() below moves the mark).
	for (int i = 0; i < GetColumnCount(); ++i) {
		if ((unsigned)i != column && GetColumn(i)->IsSortKey()) {
			GetColumn(i)->UnsetAsSortKey();
		}
	}
	GetColumn(column)->SetSortOrder(!(order & SORT_DES));
	GetModel()->Resort();
}

void CServerListCtrl::OnColumnHeaderClick(wxDataViewEvent &event)
{
	wxDataViewColumn *col = event.GetDataViewColumn();
	if (!col) {
		event.Skip();
		return;
	}
	const unsigned column = static_cast<unsigned>(col->GetModelColumn());

	// Same column clicked again flips ascending<->descending; there is no
	// alt-criterion cycle for any Servers column (AltSortAllowed() is always
	// false here), so a third click just restarts ascending.
	unsigned sort_order = 0;
	if (!m_sort_orders.empty() && m_sort_orders.front().first == column) {
		sort_order = m_sort_orders.front().second;
		sort_order = (sort_order & SORT_DES) ? 0 : SORT_DES;
	} else {
		for (CSortingList::const_iterator it = m_sort_orders.begin(); it != m_sort_orders.end();
			++it) {
			if (it->first == column) {
				sort_order = it->second;
				break;
			}
		}
	}

	ApplySorting(column, sort_order);
}

void CServerListCtrl::OnRightClick(wxDataViewEvent &event)
{
	wxDataViewItemArray selections;
	GetSelections(selections);
	if (selections.empty()) {
		event.Skip();
		return;
	}

	bool enable_reconnect = false;
	bool enable_static_on = false;
	bool enable_static_off = false;

	for (const wxDataViewItem &item : selections) {
		CServer *server = CServerListModel::ToServer(item);
		if (server == m_connected) {
			enable_reconnect = true;
		}
		enable_static_on |= !server->IsStaticMember();
		enable_static_off |= server->IsStaticMember();
	}

	// No title -- see CSearchListCtrl::OnRightClick's identical rationale
	// (wxMenu's title parameter renders inconsistently across platforms).
	wxMenu serverMenu;
	wxMenu *serverPrioMenu = new wxMenu();
	serverPrioMenu->Append(MP_PRIOLOW, _("Low"));
	serverPrioMenu->Append(MP_PRIONORMAL, _("Normal"));
	serverPrioMenu->Append(MP_PRIOHIGH, _("High"));
	serverMenu.Append(MP_CONNECTTO, _("Connect to server"));
	serverMenu.Append(12345, _("Priority"), serverPrioMenu);

	serverMenu.AppendSeparator();

	const bool multi = (selections.size() != 1);
	serverMenu.Append(MP_ADDTOSTATIC, multi ? _("Mark servers as static") : _("Mark server as static"));
	serverMenu.Append(MP_REMOVEFROMSTATIC,
		multi ? _("Mark servers as non-static") : _("Mark server as non-static"));

	serverMenu.AppendSeparator();

	serverMenu.Append(MP_REMOVE, multi ? _("Remove servers") : _("Remove server"));
	serverMenu.Append(MP_REMOVEALL, _("Remove all servers"));

	serverMenu.AppendSeparator();

	serverMenu.Append(
		MP_GETED2KLINK, multi ? _("Copy eD2k links to clipboard") : _("Copy eD2k link to clipboard"));

	serverMenu.Enable(MP_REMOVEFROMSTATIC, enable_static_off);
	serverMenu.Enable(MP_ADDTOSTATIC, enable_static_on);

	if (!multi) {
		if (enable_reconnect) {
			serverMenu.SetLabel(MP_CONNECTTO, _("Reconnect to server"));
		}
	} else {
		serverMenu.Enable(MP_CONNECTTO, false);
	}

	PopupMenu(&serverMenu);
}

void CServerListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	CServer *server = CServerListModel::ToServer(event.GetItem());
	if (!server) {
		return;
	}

	if (theApp->IsConnectedED2K()) {
		theApp->serverconnect->Disconnect();
	}
	theApp->serverconnect->ConnectToServer(server);
}

void CServerListCtrl::OnPriorityChange(wxCommandEvent &event)
{
	uint32 priority = 0;

	switch (event.GetId()) {
	case MP_PRIOLOW:
		priority = SRV_PR_LOW;
		break;
	case MP_PRIONORMAL:
		priority = SRV_PR_NORMAL;
		break;
	case MP_PRIOHIGH:
		priority = SRV_PR_HIGH;
		break;

	default:
		return;
	}

	wxDataViewItemArray selections;
	GetSelections(selections);
	for (const wxDataViewItem &item : selections) {
		theApp->serverlist->SetServerPrio(CServerListModel::ToServer(item), priority);
	}
}

void CServerListCtrl::OnStaticChange(wxCommandEvent &event)
{
	bool isStatic = (event.GetId() == MP_ADDTOSTATIC);

	wxDataViewItemArray selections;
	GetSelections(selections);
	for (const wxDataViewItem &item : selections) {
		CServer *server = CServerListModel::ToServer(item);
		// Only update items that have the wrong setting
		if (server->IsStaticMember() != isStatic) {
			theApp->serverlist->SetStaticServer(server, isStatic);
		}
	}
}

void CServerListCtrl::OnConnectToServer(wxCommandEvent &WXUNUSED(event))
{
	wxDataViewItemArray selections;
	GetSelections(selections);
	if (selections.empty()) {
		return;
	}

	if (theApp->IsConnectedED2K()) {
		theApp->serverconnect->Disconnect();
	}
	theApp->serverconnect->ConnectToServer(CServerListModel::ToServer(selections[0]));
}

void CServerListCtrl::OnGetED2kURL(wxCommandEvent &WXUNUSED(event))
{
	wxDataViewItemArray selections;
	GetSelections(selections);
	if (selections.empty()) {
		return;
	}

	wxString URL;
	for (const wxDataViewItem &item : selections) {
		CServer *server = CServerListModel::ToServer(item);
		URL += CFormat("ed2k://|server|%s|%d|/\n") % server->GetFullIP() % server->GetPort();
	}
	URL.RemoveLast();

	theApp->CopyTextToClipboard(URL);
}

void CServerListCtrl::OnRemoveServers(wxCommandEvent &event)
{
	if (event.GetId() == MP_REMOVEALL) {
		if (m_model->GetCount()) {
			wxString question = _("Are you sure that you wish to delete all servers?");

			if (wxMessageBox(
				    question, _("Cancel"), wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT, this) ==
				wxYES) {
				if (theApp->serverconnect->IsConnecting()) {
					theApp->downloadqueue->StopUDPRequests();
					theApp->serverconnect->StopConnectionTry();
					theApp->serverconnect->Disconnect();
				}

				RemoveAllServers(false);
			}
		}
	} else if (event.GetId() == MP_REMOVE) {
		wxDataViewItemArray selections;
		GetSelections(selections);
		if (!selections.empty()) {
			wxString question = (selections.size() == 1)
						    ? wxString(_("Are you sure that you wish to delete the "
								 "selected server?"))
						    : wxString(_("Are you sure that you wish to delete the "
								 "selected servers?"));

			if (wxMessageBox(
				    question, _("Cancel"), wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT, this) ==
				wxYES) {
				RemoveAllServers(true);
			}
		}
	}
}

namespace
{
//! How long a pause resets the accumulated type-ahead string, in ms.
const uint64 kTypeAheadResetMs = 1500;
} // namespace

void CServerListCtrl::OnChar(wxKeyEvent &evt)
{
	if ((evt.GetKeyCode() == WXK_DELETE) || (evt.GetKeyCode() == WXK_NUMPAD_DELETE)) {
		wxCommandEvent removeEvt;
		removeEvt.SetId(MP_REMOVE);
		OnRemoveServers(removeEvt);
		return;
	}

	int key = evt.GetKeyCode();
	if (key == 0) {
		// GetUnicodeKey() returns wxChar -- a signed char in wx's UTF-8 build
		// but wchar_t in the wide build, so an unsigned-char cast would
		// truncate the wide case; the widening to int is deliberate.
		// NOLINTNEXTLINE(bugprone-signed-char-misuse,bugprone-narrowing-conversions)
		key = evt.GetUnicodeKey();
	} else if (key >= WXK_START) {
		evt.Skip();
		return;
	}

	if (evt.AltDown() || evt.ControlDown() || evt.MetaDown()) {
		const int plain = wxTolower(evt.GetKeyCode());
		if (evt.CmdDown() && (evt.GetKeyCode() == 0x01 || plain == 'a')) {
			SelectAll();
			return;
		}
		evt.Skip();
		return;
	}

	const uint64 now = GetTickCount64();
	if (m_ttsTime + kTypeAheadResetMs < now) {
		m_ttsText.Clear();
	}
	m_ttsTime = now;
	m_ttsText.Append(wxTolower(static_cast<wxChar>(key)));

	std::vector<CServer *> ordered = m_model->GetServers();
	if (ordered.empty()) {
		return;
	}
	std::sort(ordered.begin(), ordered.end(), [this](const CServer *s1, const CServer *s2) {
		return CompareServers(s1, s2) < 0;
	});

	size_t start = 0;
	const auto current = std::find(ordered.begin(), ordered.end(), m_ttsItem);
	if (current != ordered.end()) {
		start = static_cast<size_t>(std::distance(ordered.begin(), current));
		if (m_ttsText.length() == 1) {
			++start;
		}
	}

	const size_t count = ordered.size();
	for (size_t i = 0; i < count; ++i) {
		CServer *server = ordered[(start + i) % count];
		if (server->GetListName().Lower().StartsWith(m_ttsText)) {
			const wxDataViewItem item = CServerListModel::ToItem(server);
			m_ttsItem = server;
			UnselectAll();
			Select(item);
			SetCurrentItem(item);
			EnsureVisible(item);
			return;
		}
	}
}
// File_checked_for_headers
