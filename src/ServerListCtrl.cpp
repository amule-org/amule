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

#include <algorithm> // Needed for std::max
#include <vector>    // Needed for std::vector

#include <common/MenuIDs.h>

#include <wx/menu.h>
#include <wx/stattext.h>
#include <wx/msgdlg.h>

#include "amule.h"         // Needed for theApp
#include "DownloadQueue.h" // Needed for CDownloadQueue
#ifdef GEOIP_GUI
#include "CountryFlags.h"   // Needed for CCountryFlags (flag bitmaps)
#include "CountryDisplay.h" // Needed for GetDisplayCountryCode
#endif
#include "ServerList.h"    // Needed for CServerList
#include "ServerConnect.h" // Needed for CServerConnect
#include "Server.h"        // Needed for CServer and SRV_PR_*
#include "Logger.h"
#include <common/Format.h> // Needed for CFormat

#include <wx/dcclient.h> // Needed for wxClientDC

// One fixed size for everything in the control's small image list. Set by the
// 16x16 header sort arrows; the bundled country flags are 16x11 and get padded
// onto a transparent cell of this size (see FlagImage).
static const int LIST_IMAGE_SIZE = 16;

wxBEGIN_EVENT_TABLE(CServerListCtrl, CMuleVirtualDataViewCtrl)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CServerListCtrl::OnItemRightClicked)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CServerListCtrl::OnItemActivated)

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

CServerListCtrl::CServerListCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: CMuleVirtualDataViewCtrl(parent, winid, pos, size, style, name)
{
	m_connected = 0;

	// The name column carries the country flag, so it is icon+text; the rest
	// are plain text. Sortable throughout, or wx draws no header caret.
	const int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
	AddIconTextColumn(_("Server Name"), COLUMN_SERVER_NAME, "N", 150, wxALIGN_LEFT, flags);
	AddTextColumn(_("Address"), COLUMN_SERVER_ADDR, "A", 140, wxALIGN_LEFT, flags);
	AddTextColumn(_("Port"), COLUMN_SERVER_PORT, "P", 25, wxALIGN_LEFT, flags);
	AddTextColumn(_("Description"), COLUMN_SERVER_DESC, "D", 150, wxALIGN_LEFT, flags);
	AddTextColumn(_("Ping"), COLUMN_SERVER_PING, "p", 25, wxALIGN_LEFT, flags);
	AddTextColumn(_("Users"), COLUMN_SERVER_USERS, "U", 40, wxALIGN_LEFT, flags);
	// Beside Users, since the pair is read together: how many are on now, and
	// how many the server will take.
	AddTextColumn(_("Max Users"), COLUMN_SERVER_MAXUSERS, "m", 85, wxALIGN_LEFT, flags);
	AddTextColumn(_("Files"), COLUMN_SERVER_FILES, "F", 45, wxALIGN_LEFT, flags);
	AddTextColumn(_("Priority"), COLUMN_SERVER_PRIO, "r", 60, wxALIGN_LEFT, flags);
	AddTextColumn(_("Failed"), COLUMN_SERVER_FAILS, "f", 40, wxALIGN_LEFT, flags);
	AddTextColumn(_("Static"), COLUMN_SERVER_STATIC, "S", 40, wxALIGN_LEFT, flags);
	AddTextColumn(_("Version"), COLUMN_SERVER_VERSION, "V", 80, wxALIGN_LEFT, flags);

	AddTextColumn(_("Soft Files"), COLUMN_SERVER_SOFTFILES, "s", 85, wxALIGN_LEFT, flags);
	AddTextColumn(_("Hard Files"), COLUMN_SERVER_HARDFILES, "h", 85, wxALIGN_LEFT, flags);

	// Same columns in both binaries: diagnostics, hidden by default in release
	// builds (below), and the flags are streamed over EC so the remote GUI has
	// the data behind them.
	AddTextColumn(_("TCP Flags"), COLUMN_SERVER_TCPFLAGS, "t", 80, wxALIGN_LEFT, flags);
	AddTextColumn(_("UDP Flags"), COLUMN_SERVER_UDPFLAGS, "u", 80, wxALIGN_LEFT, flags);
	// Per-user publishing limits the server advertises: how many of a user's
	// shared files it will index. Both arrive with the periodic UDP status
	// reply, the same one that fills Users and Files.

	// Absorbs the macOS trailing-column sizing; the model answers any column
	// past the real ones with an empty value.
	AppendSpacerColumn(COLUMN_SERVER_SPACER);
	AssociateVirtualModel();

	// Default sort is by name, ascending; LoadColumnSettings() replaces it
	// when the config has something saved.
	ApplySorting(COLUMN_SERVER_NAME, 0);

#ifndef __DEBUG__
	// Wire-flag columns are diagnostics: listed in the header menu so they
	// can be switched on, hidden by default. Set before the settings are
	// loaded, so anything the user saved wins -- the same ordering the old
	// InsertColumn()/SetColumnWidth(0)/LoadSettings() sequence had.
	SetColumnHidden(COLUMN_SERVER_TCPFLAGS, true, 0);
	SetColumnHidden(COLUMN_SERVER_UDPFLAGS, true, 0);
#endif

	m_columnStore.SetTableName("Server");
	LoadColumnSettings();
	InitColumnState();
}

wxString CServerListCtrl::GetOldColumnOrder() const
{
	return "N,A,P,D,p,U,F,r,f,S,V,t,u";
}

CServerListCtrl::~CServerListCtrl() {}

void CServerListCtrl::AddServer(CServer *toadd)
{
	// RefreshServer will add the server.
	// This also means that we have simple duplicity checking. ;)
	RefreshServer(toadd);

	ShowServerCount();
}

void CServerListCtrl::RemoveServer(CServer *server)
{
	const wxUIntPtr ptr = reinterpret_cast<wxUIntPtr>(server);
	if (!HasItemData(ptr)) {
		return;
	}
	if (server == m_connected) {
		m_connected = nullptr;
	}
	RemoveItemData(ptr);
	ShowServerCount();
}

void CServerListCtrl::RemoveAllServers(bool selectedOnly)
{
	// Collect first, delete second: the rows are a view onto the model, so
	// removing one renumbers every row below it -- and the confirmations below
	// run a nested event loop, during which the list can be updated underneath
	// a row index but never underneath a server pointer.
	std::vector<CServer *> candidates;
	if (selectedOnly) {
		for (wxUIntPtr data : GetSelectedItemData()) {
			candidates.push_back(reinterpret_cast<CServer *>(data));
		}
	} else {
		for (long row = 0; row < ItemDataCount(); ++row) {
			candidates.push_back(reinterpret_cast<CServer *>(ItemAt(row)));
		}
	}

	const bool connected = theApp->IsConnectedED2K() || theApp->serverconnect->IsConnecting();

	for (CServer *server : candidates) {
		// May have gone away while a message box was up.
		if (!HasItemData(reinterpret_cast<wxUIntPtr>(server))) {
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

void CServerListCtrl::RefreshServer(CServer *server)
{
	// Can't really refresh a NULL server
	if (!server) {
		return;
	}

	const wxUIntPtr ptr = reinterpret_cast<wxUIntPtr>(server);
	if (HasItemData(ptr)) {
		// The cells are rendered from the server on demand, so a refresh is
		// just a repaint -- plus, when sorted by a column whose value just
		// changed, the re-sort the base class coalesces for us.
		RefreshItemData(ptr);
	} else {
		// We are not sure that the server isn't in the list, so we can re-add
		AddItemData(ptr);
	}
}

wxString CServerListCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	const CServer *server = reinterpret_cast<const CServer *>(item);

	switch (column) {
	case COLUMN_SERVER_NAME:
		// The host country is the flag icon on this column, not a text
		// prefix -- see OnGetItemColumnImage().
		return server->GetListName();

	case COLUMN_SERVER_ADDR:
		return server->GetAddress();

	case COLUMN_SERVER_PORT:
		if (server->GetAuxPortsList().IsEmpty()) {
			return CFormat("%u") % server->GetPort();
		}
		return CFormat("%u (%s)") % server->GetPort() % server->GetAuxPortsList();

	case COLUMN_SERVER_DESC:
		return server->GetDescription();

	case COLUMN_SERVER_PING:
		if (!server->GetPing()) {
			return wxEmptyString;
		}
		// GetPing() is already milliseconds (a GetTickCount64() delta), and
		// milliseconds is how latency is written everywhere else -- including
		// our own REST API, which publishes it as "ping_ms". It used to go
		// through CastSecondsToHM(), a general duration helper whose
		// sub-minute branch prints "0.203 secs" (issue #823).
		// The unit is translated: it is not written "ms" everywhere -- ru and
		// uk use "мс", and it is localised in zh and ja too -- and the number
		// is kept out of the catalog entry the way CastSecondsToHM() keeps it
		// out of _("secs") and _("mins").
		return CFormat("%u %s") % server->GetPing() % _("ms");

	case COLUMN_SERVER_USERS:
		if (!server->GetUsers()) {
			return wxEmptyString;
		}
		return CFormat("%u") % server->GetUsers();

	// Zero means "the server never told us", not "the limit is zero" -- these
	// only arrive once a UDP status reply has come back, so a freshly added or
	// UDP-silent server has nothing to show. Rendering that as 0 would read as
	// a real limit of zero, so it stays blank, as Users and Files do.
	case COLUMN_SERVER_SOFTFILES:
		if (!server->GetSoftFiles()) {
			return wxEmptyString;
		}
		return CFormat("%u") % server->GetSoftFiles();

	case COLUMN_SERVER_HARDFILES:
		if (!server->GetHardFiles()) {
			return wxEmptyString;
		}
		return CFormat("%u") % server->GetHardFiles();

	case COLUMN_SERVER_MAXUSERS:
		if (!server->GetMaxUsers()) {
			return wxEmptyString;
		}
		return CFormat("%u") % server->GetMaxUsers();

	case COLUMN_SERVER_FILES:
		if (!server->GetFiles()) {
			return wxEmptyString;
		}
		return CFormat("%u") % server->GetFiles();

	case COLUMN_SERVER_PRIO:
		switch (server->GetPreferences()) {
		case SRV_PR_LOW:
			return _("Low");
		case SRV_PR_NORMAL:
			return _("Normal");
		case SRV_PR_HIGH:
			return _("High");
		default:
			return "---"; // this should never happen
		}

	case COLUMN_SERVER_FAILS:
		return CFormat("%u") % server->GetFailedCount();

	case COLUMN_SERVER_STATIC:
		return server->IsStaticMember() ? _("Yes") : _("No");

	case COLUMN_SERVER_VERSION:
		return server->GetVersion();

	// Rendered in both binaries: the flags are streamed over EC, so the remote
	// GUI has the same data behind these columns.
	case COLUMN_SERVER_TCPFLAGS: {
		wxString flags;
		if (server->GetTCPFlags() & SRV_TCPFLG_COMPRESSION) {
			flags += "c";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_NEWTAGS) {
			flags += "n";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_UNICODE) {
			flags += "u";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_RELATEDSEARCH) {
			flags += "r";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_TYPETAGINTEGER) {
			flags += "t";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_LARGEFILES) {
			flags += "l";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_TCPOBFUSCATION) {
			flags += "o";
		}
		return flags;
	}

	case COLUMN_SERVER_UDPFLAGS: {
		wxString flags;
		if (server->GetUDPFlags() & SRV_UDPFLG_EXT_GETSOURCES) {
			flags += "g";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_EXT_GETFILES) {
			flags += "f";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_NEWTAGS) {
			flags += "n";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_UNICODE) {
			flags += "u";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_EXT_GETSOURCES2) {
			flags += "G";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_LARGEFILES) {
			flags += "l";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_UDPOBFUSCATION) {
			flags += "o";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_TCPOBFUSCATION) {
			flags += "O";
		}
		return flags;
	}

	default:
		return wxEmptyString;
	}
}

// Gated with its only caller (OnGetItemColumnImage below): this reaches
// theApp->GetCountryFlags(), which only exists under GEOIP_GUI, so compiling it
// unconditionally broke the monolithic build at -DENABLE_IP2COUNTRY=NO.
//
// Only the definition is gated, not the declaration: GEOIP_GUI is #defined in
// amule.h, which this file includes but ServerListCtrl.h does not -- so the
// header cannot see the macro and gating it there would compile the
// declaration out from under this definition. A member function that is
// declared, never called and never defined is fine.
#ifdef GEOIP_GUI
const wxIcon &CServerListCtrl::FlagIcon(const wxString &code) const
{
	static const wxIcon nullIcon;

	const auto it = m_flagIcons.find(code);
	if (it != m_flagIcons.end()) {
		return it->second;
	}

	const wxImage &flag = theApp->GetCountryFlags()->GetFlag(code);
	if (!flag.IsOk()) {
		// Cached as an invalid icon so an unknown code isn't looked up again.
		return m_flagIcons.emplace(code, wxIcon()).first->second;
	}

	wxIcon icon;
	icon.CopyFromBitmap(wxBitmap(flag));
	return m_flagIcons.emplace(code, icon).first->second;
}
#endif // GEOIP_GUI

bool CServerListCtrl::GetItemIcon(wxUIntPtr item, unsigned column, wxIcon &icon) const
{
	if (column != COLUMN_SERVER_NAME) {
		return false;
	}
#ifdef GEOIP_GUI
	// Host country as a flag icon, matching the peer list: no icon at all for
	// an unresolved server, rather than the "? - " prefix this used to show.
	const CServer *server = reinterpret_cast<const CServer *>(item);
	wxString code;
	if (GetDisplayCountryCode(
		    server->IsCountryFromCore(), server->GetCountryCode(), server->GetIP(), code) &&
		!code.IsEmpty()) {
		const wxIcon &flag = FlagIcon(code);
		if (flag.IsOk()) {
			icon = flag;
			return true;
		}
	}
#else
	wxUnusedVar(item);
#endif // GEOIP_GUI
	return false;
}

bool CServerListCtrl::GetItemAttr(wxUIntPtr item, unsigned WXUNUSED(column), wxDataViewItemAttr &attr) const
{
	if (m_connected && reinterpret_cast<const CServer *>(item) == m_connected) {
		attr.SetBold(true);
		return true;
	}
	return false;
}

bool CServerListCtrl::IsLiveSortColumn() const
{
	if (m_sort_orders.empty()) {
		return false;
	}
	switch (static_cast<int>(m_sort_orders.front().first)) {
	case COLUMN_SERVER_PING:
	case COLUMN_SERVER_USERS:
	case COLUMN_SERVER_FILES:
		return true;
	default:
		return false;
	}
}

void CServerListCtrl::HighlightServer(const CServer *server, bool highlight)
{
	// The bold font is handed out by OnGetItemAttr(), so all this has to do is
	// move m_connected and repaint the rows either side of the change.
	const CServer *previous = m_connected;

	if (highlight) {
		m_connected = server;
	} else if (m_connected == server) {
		m_connected = nullptr;
	}

	if (previous == m_connected) {
		return;
	}
	if (previous) {
		RefreshItemData(reinterpret_cast<wxUIntPtr>(previous));
	}
	if (m_connected) {
		RefreshItemData(reinterpret_cast<wxUIntPtr>(m_connected));
	}
}

void CServerListCtrl::ShowServerCount()
{
	wxStaticText *label = CastByName("serverListLabel", GetParent(), wxStaticText);

	if (label) {
		label->SetLabel(CFormat(_("Servers (%i)")) % ItemDataCount());
		label->GetParent()->Layout();
	}
}

void CServerListCtrl::FitColumnsToContent()
{
	// Only size a profile that has no widths of its own. Both callers -- the
	// core's bulk-load notification and the remote GUI's first populated update
	// -- run after LoadColumnSettings() has restored whatever was saved, so
	// fitting unconditionally would overwrite a width the user had dragged.
	// Harmless while drags were never persisted; not any more.
	if (HasPersistedColumnWidths()) {
		return;
	}

	// Upper bound for the Description column: descriptions can be very
	// long (full forum URLs etc.), so cap it rather than let one row blow
	// the column out. The other columns hold short, bounded values.
	const int descMaxWidth = 300;

	// Content is measured against the model rather than asked of the control:
	// wxDataViewCtrl has no portable "size this column to its contents", and
	// on a list that renders cells on demand there is nothing native to
	// measure anyway. The margins reproduce what the old wxLIST_AUTOSIZE
	// produced, so columns land where users are used to seeing them.
	const int autosizeMargin = 10;
	const int imageMargin = 5;
	const int flagWidth = 16;

	wxClientDC dc(this);
	dc.SetFont(GetFont());

	const long rows = ItemDataCount();

	Freeze();
	for (unsigned col = 0; col < RealColumnCount(); ++col) {
		// Leave hidden columns (the TCP/UDP flag columns, or ones the user
		// hid from the header menu) hidden.
		if (IsColumnHidden(static_cast<int>(col))) {
			continue;
		}

		int contentWidth = autosizeMargin;
		for (long row = 0; row < rows; ++row) {
			const wxUIntPtr data = ItemAt(row);
			wxCoord textWidth = 0;
			dc.GetTextExtent(GetItemColumnText(data, col), &textWidth, nullptr);
			int cellWidth = textWidth;
			wxIcon icon;
			if (GetItemIcon(data, col, icon)) {
				cellWidth += flagWidth + imageMargin;
			}
			if (cellWidth > contentWidth) {
				contentWidth = cellWidth;
			}
		}
		contentWidth += autosizeMargin;

		wxCoord headerWidth = 0;
		dc.GetTextExtent(GetColumn(col)->GetTitle(), &headerWidth, nullptr);
		headerWidth += 2 * autosizeMargin;

		int width = std::max(contentWidth, static_cast<int>(headerWidth));
		if (col == COLUMN_SERVER_DESC && width > descMaxWidth) {
			width = descMaxWidth;
		}
		GetColumn(col)->SetWidth(width);
	}
	Thaw();
}

void CServerListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	// Connect to the activated row alone, whatever else was selected.
	if (event.GetItem().IsOk()) {
		UnselectAll();
		Select(event.GetItem());
	}

	wxCommandEvent nulEvt;
	OnConnectToServer(nulEvt);
}

void CServerListCtrl::OnItemRightClicked(wxDataViewEvent &event)
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

	bool enable_reconnect = false;
	bool enable_static_on = false;
	bool enable_static_off = false;

	// Gather information on the selected items
	for (wxUIntPtr data : GetSelectedItemData()) {
		CServer *server = reinterpret_cast<CServer *>(data);

		// The current server is selected, so we might display the reconnect option
		if (server == m_connected) {
			enable_reconnect = true;
		}

		// We want to know which options should be enabled, either one or both
		enable_static_on |= !server->IsStaticMember();
		enable_static_off |= server->IsStaticMember();
	}

	wxMenu *serverMenu = new wxMenu(_("Server"));
	wxMenu *serverPrioMenu = new wxMenu();
	serverPrioMenu->Append(MP_PRIOLOW, _("Low"));
	serverPrioMenu->Append(MP_PRIONORMAL, _("Normal"));
	serverPrioMenu->Append(MP_PRIOHIGH, _("High"));
	serverMenu->Append(MP_CONNECTTO, _("Connect to server"));
	serverMenu->Append(12345, _("Priority"), serverPrioMenu);

	serverMenu->AppendSeparator();

	if (static_cast<int>(GetSelectedItemsCount()) == 1) {
		serverMenu->Append(MP_ADDTOSTATIC, _("Mark server as static"));
		serverMenu->Append(MP_REMOVEFROMSTATIC, _("Mark server as non-static"));
	} else {
		serverMenu->Append(MP_ADDTOSTATIC, _("Mark servers as static"));
		serverMenu->Append(MP_REMOVEFROMSTATIC, _("Mark servers as non-static"));
	}

	serverMenu->AppendSeparator();

	if (static_cast<int>(GetSelectedItemsCount()) == 1) {
		serverMenu->Append(MP_REMOVE, _("Remove server"));
	} else {
		serverMenu->Append(MP_REMOVE, _("Remove servers"));
	}
	serverMenu->Append(MP_REMOVEALL, _("Remove all servers"));

	serverMenu->AppendSeparator();

	if (static_cast<int>(GetSelectedItemsCount()) == 1) {
		serverMenu->Append(MP_GETED2KLINK, _("Copy eD2k link to clipboard"));
	} else {
		serverMenu->Append(MP_GETED2KLINK, _("Copy eD2k links to clipboard"));
	}

	serverMenu->Enable(MP_REMOVEFROMSTATIC, enable_static_off);
	serverMenu->Enable(MP_ADDTOSTATIC, enable_static_on);

	if (static_cast<int>(GetSelectedItemsCount()) == 1) {
		if (enable_reconnect)
			serverMenu->SetLabel(MP_CONNECTTO, _("Reconnect to server"));
	} else {
		serverMenu->Enable(MP_CONNECTTO, false);
	}

	PopupMenu(serverMenu);
	delete serverMenu;
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

	const std::vector<wxUIntPtr> items = GetSelectedItemData();

	for (wxUIntPtr data : items) {
		CServer *server = reinterpret_cast<CServer *>(data);
		theApp->serverlist->SetServerPrio(server, priority);
	}
}

void CServerListCtrl::OnStaticChange(wxCommandEvent &event)
{
	bool isStatic = (event.GetId() == MP_ADDTOSTATIC);

	const std::vector<wxUIntPtr> items = GetSelectedItemData();

	for (wxUIntPtr data : items) {
		CServer *server = reinterpret_cast<CServer *>(data);

		// Only update items that have the wrong setting
		if (server->IsStaticMember() != isStatic) {
			theApp->serverlist->SetStaticServer(server, isStatic);
		}
	}
}

void CServerListCtrl::OnConnectToServer(wxCommandEvent &WXUNUSED(event))
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();

	if (!selected.empty()) {
		if (theApp->IsConnectedED2K()) {
			theApp->serverconnect->Disconnect();
		}

		theApp->serverconnect->ConnectToServer(reinterpret_cast<CServer *>(selected.front()));
	}
}

void CServerListCtrl::OnGetED2kURL(wxCommandEvent &WXUNUSED(event))
{
	wxString URL;

	for (wxUIntPtr data : GetSelectedItemData()) {
		CServer *server = reinterpret_cast<CServer *>(data);

		URL += CFormat("ed2k://|server|%s|%d|/\n") % server->GetFullIP() % server->GetPort();
	}

	URL.RemoveLast();

	theApp->CopyTextToClipboard(URL);
}

void CServerListCtrl::OnRemoveServers(wxCommandEvent &event)
{
	if (event.GetId() == MP_REMOVEALL) {
		if (ItemDataCount()) {
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
		if (static_cast<int>(GetSelectedItemsCount())) {
			wxString question;
			if (static_cast<int>(GetSelectedItemsCount()) == 1) {
				question = _("Are you sure that you wish to delete the selected server?");
			} else {
				question = _("Are you sure that you wish to delete the selected servers?");
			}

			if (wxMessageBox(
				    question, _("Cancel"), wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT, this) ==
				wxYES) {
				RemoveAllServers(true);
			}
		}
	}
}

bool CServerListCtrl::OnListKey(wxKeyEvent &event)
{
	// Delete removes the selected servers; everything else belongs to the
	// control's own navigation.
	if ((event.GetKeyCode() == WXK_DELETE) || (event.GetKeyCode() == WXK_NUMPAD_DELETE)) {
		wxCommandEvent evt;
		evt.SetId(MP_REMOVE);
		OnRemoveServers(evt);
		return true;
	}
	return false;
}

int CServerListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	const CServer *server1 = reinterpret_cast<const CServer *>(data1);
	const CServer *server2 = reinterpret_cast<const CServer *>(data2);

	const int mode = modifier;

	switch (column) {
	// Sort by server-name
	case COLUMN_SERVER_NAME:
		return mode * server1->GetListName().CmpNoCase(server2->GetListName());

	// Sort by address
	case COLUMN_SERVER_ADDR: {
		if (server1->HasDynIP() && server2->HasDynIP()) {
			return mode * server1->GetDynIP().CmpNoCase(server2->GetDynIP());
		} else if (server1->HasDynIP()) {
			return mode * -1;
		} else if (server2->HasDynIP()) {
			return mode * 1;
		} else {
			uint32 a = wxUINT32_SWAP_ALWAYS(server1->GetIP());
			uint32 b = wxUINT32_SWAP_ALWAYS(server2->GetIP());
			return mode * CmpAny(a, b);
		}
	}
	// Sort by port
	case COLUMN_SERVER_PORT:
		return mode * CmpAny(server1->GetPort(), server2->GetPort());
	// Sort by description
	case COLUMN_SERVER_DESC:
		return mode * server1->GetDescription().CmpNoCase(server2->GetDescription());
	// Sort by Ping
	// The -1 ensures that a value of zero (no ping known) is sorted last.
	case COLUMN_SERVER_PING:
		return mode * CmpAny(server1->GetPing() - 1, server2->GetPing() - 1);
	// Sort by user-count
	case COLUMN_SERVER_USERS:
		return mode * CmpAny(server1->GetUsers(), server2->GetUsers());
	case COLUMN_SERVER_SOFTFILES:
		return mode * CmpAny(server1->GetSoftFiles(), server2->GetSoftFiles());
	case COLUMN_SERVER_HARDFILES:
		return mode * CmpAny(server1->GetHardFiles(), server2->GetHardFiles());
	case COLUMN_SERVER_MAXUSERS:
		return mode * CmpAny(server1->GetMaxUsers(), server2->GetMaxUsers());
	// Sort by file-count
	case COLUMN_SERVER_FILES:
		return mode * CmpAny(server1->GetFiles(), server2->GetFiles());
	// Sort by priority
	case COLUMN_SERVER_PRIO: {
		uint32 srv_pr1 = server1->GetPreferences();
		uint32 srv_pr2 = server2->GetPreferences();
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
		return mode * CmpAny(srv_pr1, srv_pr2);
	}
	// Sort by failure-count
	case COLUMN_SERVER_FAILS:
		return mode * CmpAny(server1->GetFailedCount(), server2->GetFailedCount());
	// Sort by static servers
	case COLUMN_SERVER_STATIC: {
		return mode * CmpAny(server2->IsStaticMember(), server1->IsStaticMember());
	}
	// Sort by version
	case COLUMN_SERVER_VERSION:
		return mode * FuzzyStrCmp(server1->GetVersion(), server2->GetVersion());

	default:
		return 0;
	}
}
// File_checked_for_headers
