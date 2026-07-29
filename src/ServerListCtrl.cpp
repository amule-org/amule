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

wxBEGIN_EVENT_TABLE(CServerListCtrl, CMuleVirtualListCtrl)
	EVT_LIST_ITEM_RIGHT_CLICK(-1, CServerListCtrl::OnItemRightClicked)
	EVT_LIST_ITEM_ACTIVATED(-1, CServerListCtrl::OnItemActivated)

	EVT_MENU(MP_PRIOLOW, CServerListCtrl::OnPriorityChange)
	EVT_MENU(MP_PRIONORMAL, CServerListCtrl::OnPriorityChange)
	EVT_MENU(MP_PRIOHIGH, CServerListCtrl::OnPriorityChange)

	EVT_MENU(MP_ADDTOSTATIC, CServerListCtrl::OnStaticChange)
	EVT_MENU(MP_REMOVEFROMSTATIC, CServerListCtrl::OnStaticChange)

	EVT_MENU(MP_CONNECTTO, CServerListCtrl::OnConnectToServer)

	EVT_MENU(MP_REMOVE, CServerListCtrl::OnRemoveServers)
	EVT_MENU(MP_REMOVEALL, CServerListCtrl::OnRemoveServers)

	EVT_MENU(MP_GETED2KLINK, CServerListCtrl::OnGetED2kURL)

	EVT_CHAR(CServerListCtrl::OnKeyPressed)
wxEND_EVENT_TABLE()

CServerListCtrl::CServerListCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxValidator &validator,
	const wxString &name)
: CMuleVirtualListCtrl(parent, winid, pos, size, style, validator, name)
, m_images(LIST_IMAGE_SIZE, LIST_IMAGE_SIZE, true, 0)
{
	// Setting the sorter function.
	SetSortFunc(SortProc);

	// Set the table-name (for loading and saving preferences).
	SetTableName("Server");

	m_connected = 0;

	// Take over the small image list from the one every CMuleListCtrl shares:
	// the country flags are added to it on demand and have no business showing
	// up in the other lists' indices. The sort arrows have to come first, at
	// the indices SetSorting() uses.
	AddSortArrows(m_images);
	SetImageList(&m_images, wxIMAGE_LIST_SMALL);

	wxFont bold = GetFont();
	bold.SetWeight(wxFONTWEIGHT_BOLD);
	m_boldAttr.SetFont(bold);

	InsertColumn(COLUMN_SERVER_NAME, _("Server Name"), wxLIST_FORMAT_LEFT, 150, "N");
	InsertColumn(COLUMN_SERVER_ADDR, _("Address"), wxLIST_FORMAT_LEFT, 140, "A");
	InsertColumn(COLUMN_SERVER_PORT, _("Port"), wxLIST_FORMAT_LEFT, 25, "P");
	InsertColumn(COLUMN_SERVER_DESC, _("Description"), wxLIST_FORMAT_LEFT, 150, "D");
	InsertColumn(COLUMN_SERVER_PING, _("Ping"), wxLIST_FORMAT_LEFT, 25, "p");
	InsertColumn(COLUMN_SERVER_USERS, _("Users"), wxLIST_FORMAT_LEFT, 40, "U");
	InsertColumn(COLUMN_SERVER_FILES, _("Files"), wxLIST_FORMAT_LEFT, 45, "F");
	InsertColumn(COLUMN_SERVER_PRIO, _("Priority"), wxLIST_FORMAT_LEFT, 60, "r");
	InsertColumn(COLUMN_SERVER_FAILS, _("Failed"), wxLIST_FORMAT_LEFT, 40, "f");
	InsertColumn(COLUMN_SERVER_STATIC, _("Static"), wxLIST_FORMAT_LEFT, 40, "S");
	InsertColumn(COLUMN_SERVER_VERSION, _("Version"), wxLIST_FORMAT_LEFT, 80, "V");

#if !defined(CLIENT_GUI)
	InsertColumn(COLUMN_SERVER_TCPFLAGS, _("TCP Flags"), wxLIST_FORMAT_LEFT, 80, "t");
	InsertColumn(COLUMN_SERVER_UDPFLAGS, _("UDP Flags"), wxLIST_FORMAT_LEFT, 80, "u");
#if !defined(__DEBUG__)
	// InsertColumn created the columns with their default 80 width, which is required to unhide
	// them on the context menu. Now, for Release builds, we will hide them by default
	SetColumnWidth(GetColumnIndex("t"), 0);
	SetColumnWidth(GetColumnIndex("u"), 0);
#endif
#endif

	LoadSettings();
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

void CServerListCtrl::RemoveAllServers(int state)
{
	// Collect first, delete second: the rows are a view onto the model, so
	// removing one renumbers every row below it -- and the confirmations below
	// run a nested event loop, during which the list can be updated underneath
	// a row index but never underneath a server pointer.
	std::vector<CServer *> candidates;
	for (long pos = GetNextItem(-1, wxLIST_NEXT_ALL, state); pos != -1;
		pos = GetNextItem(pos, wxLIST_NEXT_ALL, state)) {
		candidates.push_back(reinterpret_cast<CServer *>(ItemAt(pos)));
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

wxString CServerListCtrl::GetItemColumnText(wxUIntPtr item, long column) const
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
		return CastSecondsToHM(server->GetPing() / 1000, server->GetPing() % 1000);

	case COLUMN_SERVER_USERS:
		if (!server->GetUsers()) {
			return wxEmptyString;
		}
		return CFormat("%u") % server->GetUsers();

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

#if !defined(CLIENT_GUI)
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
#endif

	default:
		return wxEmptyString;
	}
}

int CServerListCtrl::FlagImage(const wxString &code) const
{
	const auto it = m_flagImages.find(code);
	if (it != m_flagImages.end()) {
		return it->second;
	}

	const wxImage &flag = theApp->GetCountryFlags()->GetFlag(code);
	if (!flag.IsOk()) {
		m_flagImages[code] = -1;
		return -1;
	}

	// Centre the 16x11 flag on the image list's square cell. Adding it at its
	// own size instead would leave wxImageList to stretch it to the cell.
	wxImage cell = flag;
	if (!cell.HasAlpha()) {
		// Size() only pads with transparent pixels when there is an alpha
		// channel to be transparent in; without one it would pad with black.
		cell.InitAlpha();
	}
	cell = cell.Size(wxSize(LIST_IMAGE_SIZE, LIST_IMAGE_SIZE),
		wxPoint((LIST_IMAGE_SIZE - cell.GetWidth()) / 2, (LIST_IMAGE_SIZE - cell.GetHeight()) / 2));

	const int index = m_images.Add(wxBitmap(cell));
	m_flagImages[code] = index;
	return index;
}

int CServerListCtrl::OnGetItemColumnImage(long item, long column) const
{
	if (column != COLUMN_SERVER_NAME) {
		return -1;
	}
#ifdef GEOIP_GUI
	// Host country as a flag icon, matching the peer list: no icon at all for
	// an unresolved server, rather than the "? - " prefix this used to show.
	const CServer *server = reinterpret_cast<const CServer *>(ItemAt(item));
	wxString code;
	if (GetDisplayCountryCode(
		    server->IsCountryFromCore(), server->GetCountryCode(), server->GetFullIP(), code) &&
		!code.IsEmpty()) {
		return FlagImage(code);
	}
#else
	wxUnusedVar(item);
#endif // GEOIP_GUI
	return -1;
}

wxListItemAttr *CServerListCtrl::OnGetItemAttr(long item) const
{
	if (m_connected && reinterpret_cast<const CServer *>(ItemAt(item)) == m_connected) {
		return &m_boldAttr;
	}
	return nullptr;
}

bool CServerListCtrl::IsLiveSortColumn() const
{
	switch (static_cast<int>(GetSortColumn())) {
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
		label->SetLabel(CFormat(_("Servers (%i)")) % GetItemCount());
		label->GetParent()->Layout();
	}
}

void CServerListCtrl::FitColumnsToContent()
{
	// Upper bound for the Description column: descriptions can be very
	// long (full forum URLs etc.), so cap it rather than let one row blow
	// the column out. The other columns hold short, bounded values.
	const int descMaxWidth = 300;

	// wxLIST_AUTOSIZE is a no-op on a virtual control -- it has no native items
	// to measure and just hands back a default width -- so the content side is
	// measured here against the model. wxLIST_AUTOSIZE_USEHEADER measures the
	// header text and works either way, so that half is left to it.
	//
	// The two constants and the arithmetic below reproduce what the non-virtual
	// wxLIST_AUTOSIZE did, so column widths come out where they used to: the
	// margin is the starting floor and is added once to the finished column
	// (NOT per cell), and a cell carrying an image counts the image plus the
	// narrower in-row image margin. Both live in listctrl.cpp as file-statics,
	// hence the copies.
	const int autosizeMargin = 10; // AUTOSIZE_COL_MARGIN
	const int imageMargin = 5;     // IMAGE_MARGIN_IN_REPORT_MODE

	wxClientDC dc(this);
	dc.SetFont(GetFont());

	const long rows = ItemDataCount();

	Freeze();
	for (int col = 0; col < GetColumnCount(); ++col) {
		// Leave hidden columns (width 0, e.g. the TCP/UDP flag columns or
		// ones the user hid via the header menu) hidden.
		if (GetColumnWidth(col) == 0) {
			continue;
		}

		int contentWidth = autosizeMargin;
		for (long row = 0; row < rows; ++row) {
			wxCoord textWidth = 0;
			dc.GetTextExtent(GetItemColumnText(ItemAt(row), col), &textWidth, nullptr);
			int cellWidth = textWidth;
			if (OnGetItemColumnImage(row, col) != -1) {
				cellWidth += LIST_IMAGE_SIZE + imageMargin;
			}
			if (cellWidth > contentWidth) {
				contentWidth = cellWidth;
			}
		}
		contentWidth += autosizeMargin;

		SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
		const int headerWidth = GetColumnWidth(col);

		int width = std::max(contentWidth, headerWidth);
		if (col == COLUMN_SERVER_DESC && width > descMaxWidth) {
			width = descMaxWidth;
		}
		SetColumnWidth(col, width);
	}
	Thaw();
}

void CServerListCtrl::OnItemActivated(wxListEvent &event)
{
	// Unselect all items but the activated one
	long item = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	while (item > -1) {
		SetItemState(item, 0, wxLIST_STATE_SELECTED);

		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	}

	SetItemState(event.GetIndex(), wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);

	wxCommandEvent nulEvt;
	OnConnectToServer(nulEvt);
}

void CServerListCtrl::OnItemRightClicked(wxListEvent &event)
{
	// Check if clicked item is selected. If not, unselect all and select it.
	long index = CheckSelection(event);

	bool enable_reconnect = false;
	bool enable_static_on = false;
	bool enable_static_off = false;

	// Gather information on the selected items
	while (index > -1) {
		CServer *server = reinterpret_cast<CServer *>(ItemAt(index));

		// The current server is selected, so we might display the reconnect option
		if (server == m_connected) {
			enable_reconnect = true;
		}

		// We want to know which options should be enabled, either one or both
		enable_static_on |= !server->IsStaticMember();
		enable_static_off |= server->IsStaticMember();

		index = GetNextItem(index, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	}

	wxMenu *serverMenu = new wxMenu(_("Server"));
	wxMenu *serverPrioMenu = new wxMenu();
	serverPrioMenu->Append(MP_PRIOLOW, _("Low"));
	serverPrioMenu->Append(MP_PRIONORMAL, _("Normal"));
	serverPrioMenu->Append(MP_PRIOHIGH, _("High"));
	serverMenu->Append(MP_CONNECTTO, _("Connect to server"));
	serverMenu->Append(12345, _("Priority"), serverPrioMenu);

	serverMenu->AppendSeparator();

	if (GetSelectedItemCount() == 1) {
		serverMenu->Append(MP_ADDTOSTATIC, _("Mark server as static"));
		serverMenu->Append(MP_REMOVEFROMSTATIC, _("Mark server as non-static"));
	} else {
		serverMenu->Append(MP_ADDTOSTATIC, _("Mark servers as static"));
		serverMenu->Append(MP_REMOVEFROMSTATIC, _("Mark servers as non-static"));
	}

	serverMenu->AppendSeparator();

	if (GetSelectedItemCount() == 1) {
		serverMenu->Append(MP_REMOVE, _("Remove server"));
	} else {
		serverMenu->Append(MP_REMOVE, _("Remove servers"));
	}
	serverMenu->Append(MP_REMOVEALL, _("Remove all servers"));

	serverMenu->AppendSeparator();

	if (GetSelectedItemCount() == 1) {
		serverMenu->Append(MP_GETED2KLINK, _("Copy eD2k link to clipboard"));
	} else {
		serverMenu->Append(MP_GETED2KLINK, _("Copy eD2k links to clipboard"));
	}

	serverMenu->Enable(MP_REMOVEFROMSTATIC, enable_static_off);
	serverMenu->Enable(MP_ADDTOSTATIC, enable_static_on);

	if (GetSelectedItemCount() == 1) {
		if (enable_reconnect)
			serverMenu->SetLabel(MP_CONNECTTO, _("Reconnect to server"));
	} else {
		serverMenu->Enable(MP_CONNECTTO, false);
	}

	PopupMenu(serverMenu, event.GetPoint());
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

	ItemDataList items = GetSelectedItems();

	for (unsigned int i = 0; i < items.size(); ++i) {
		CServer *server = reinterpret_cast<CServer *>(items[i]);
		theApp->serverlist->SetServerPrio(server, priority);
	}
}

void CServerListCtrl::OnStaticChange(wxCommandEvent &event)
{
	bool isStatic = (event.GetId() == MP_ADDTOSTATIC);

	ItemDataList items = GetSelectedItems();

	for (unsigned int i = 0; i < items.size(); ++i) {
		CServer *server = reinterpret_cast<CServer *>(items[i]);

		// Only update items that have the wrong setting
		if (server->IsStaticMember() != isStatic) {
			theApp->serverlist->SetStaticServer(server, isStatic);
		}
	}
}

void CServerListCtrl::OnConnectToServer(wxCommandEvent &WXUNUSED(event))
{
	int item = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

	if (item > -1) {
		if (theApp->IsConnectedED2K()) {
			theApp->serverconnect->Disconnect();
		}

		theApp->serverconnect->ConnectToServer(reinterpret_cast<CServer *>(ItemAt(item)));
	}
}

void CServerListCtrl::OnGetED2kURL(wxCommandEvent &WXUNUSED(event))
{
	int pos = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

	wxString URL;

	while (pos != -1) {
		CServer *server = reinterpret_cast<CServer *>(ItemAt(pos));

		URL += CFormat("ed2k://|server|%s|%d|/\n") % server->GetFullIP() % server->GetPort();

		pos = GetNextItem(pos, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	}

	URL.RemoveLast();

	theApp->CopyTextToClipboard(URL);
}

void CServerListCtrl::OnRemoveServers(wxCommandEvent &event)
{
	if (event.GetId() == MP_REMOVEALL) {
		if (GetItemCount()) {
			wxString question = _("Are you sure that you wish to delete all servers?");

			if (wxMessageBox(
				    question, _("Cancel"), wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT, this) ==
				wxYES) {
				if (theApp->serverconnect->IsConnecting()) {
					theApp->downloadqueue->StopUDPRequests();
					theApp->serverconnect->StopConnectionTry();
					theApp->serverconnect->Disconnect();
				}

				RemoveAllServers(wxLIST_STATE_DONTCARE);
			}
		}
	} else if (event.GetId() == MP_REMOVE) {
		if (GetSelectedItemCount()) {
			wxString question;
			if (GetSelectedItemCount() == 1) {
				question = _("Are you sure that you wish to delete the selected server?");
			} else {
				question = _("Are you sure that you wish to delete the selected servers?");
			}

			if (wxMessageBox(
				    question, _("Cancel"), wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT, this) ==
				wxYES) {
				RemoveAllServers(wxLIST_STATE_SELECTED);
			}
		}
	}
}

void CServerListCtrl::OnKeyPressed(wxKeyEvent &event)
{
	// Check if delete was pressed
	if ((event.GetKeyCode() == WXK_DELETE) || (event.GetKeyCode() == WXK_NUMPAD_DELETE)) {
		wxCommandEvent evt;
		evt.SetId(MP_REMOVE);
		OnRemoveServers(evt);
	} else {
		event.Skip();
	}
}

int CServerListCtrl::SortProc(wxUIntPtr item1, wxUIntPtr item2, wxIntPtr sortData)
{
	CServer *server1 = reinterpret_cast<CServer *>(item1);
	CServer *server2 = reinterpret_cast<CServer *>(item2);

	int mode = (sortData & CMuleListCtrl::SORT_DES) ? -1 : 1;

	switch (sortData & CMuleListCtrl::COLUMN_MASK) {
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
