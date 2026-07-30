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

#include <algorithm> // Needed for std::max
#include <vector>    // Needed for std::vector

#include <wx/clipbrd.h>  // Needed for wxTheClipboard
#include <wx/dataobj.h>  // Needed for wxTextDataObject
#include <wx/menu.h>     // Needed for wxMenu (context-menu)
#include <wx/notebook.h> // Needed for wxNotebook (log-tab insertion, CLIENT_GUI)

#include "muuli_wdr.h"      // Needed for ID_ADDTOLIST
#include "ServerWnd.h"      // Interface declarations.
#include "Server.h"         // Needed for CServer
#include "ServerList.h"     // Needed for CServerList
#include "ServerListCtrl.h" // Needed for CServerListCtrl
#include "Preferences.h"    // Needed for CPreferences
#include "ServerConnect.h"
#include "amuleDlg.h" // Needed for CamuleDlg
#include "amule.h"    // Needed for theApp
#include "Logger.h"
#include "IPFilter.h" // Needed for CIPFilter::IsReady
#include "kademlia/utils/UInt128.h"

#include "ClientList.h"

wxBEGIN_EVENT_TABLE(CServerWnd, wxPanel)
	EVT_BUTTON(ID_ADDTOLIST, CServerWnd::OnBnClickedAddserver)
	EVT_BUTTON(IDC_ED2KDISCONNECT, CServerWnd::OnBnClickedED2KDisconnect)
	EVT_BUTTON(ID_UPDATELIST, CServerWnd::OnBnClickedUpdateservermetfromurl)
	EVT_TEXT_ENTER(IDC_SERVERLISTURL, CServerWnd::OnBnClickedUpdateservermetfromurl)
	EVT_BUTTON(ID_BTN_RESET, CServerWnd::OnBnClickedResetLog)
	EVT_BUTTON(ID_BTN_RESET_SERVER, CServerWnd::OnBnClickedResetServerLog)
#ifdef CLIENT_GUI
	EVT_BUTTON(ID_BTN_RESET_GUILOG, CServerWnd::OnBnClickedResetGuiLog)
#endif
	EVT_SPLITTER_SASH_POS_CHANGING(ID_SRV_SPLITTER, CServerWnd::OnSashPositionChanging)
	EVT_SPLITTER_SASH_POS_CHANGED(ID_SRV_SPLITTER, CServerWnd::OnSashPositionChanged)
wxEND_EVENT_TABLE()

// Anonymous enum so the "Copy" context-menu item has a stable ID
// scoped to this translation unit (it never escapes to the rest of
// the main dialog's ID space).
namespace
{
enum
{
	kInfoListMenuCopy = wxID_HIGHEST + 1
};

// Column layout of both info lists. Column 0 is an empty spacer that exists
// only to inset the labels: wxListCtrl has no counterpart to Scintilla's
// SetMarginLeft, and the left cell padding comes from the platform theme, so
// on a theme that sets none the text is drawn hard against the frame (issue
// #702). A fixed narrow column is the one way to get that inset identically
// on every port. Its width matches CMuleLogCtrl's text margin so the info
// panes line up with the log panes beside them in the same notebook.
enum
{
	kInfoSpacerCol = 0,
	kInfoLabelCol = 1,
	kInfoValueCol = 2
};

// Matches CMuleLogCtrl's FromDIP(5) text margin. Not DIP-scaled here: a
// wxListCtrl column width is set from a plain int and this control has no
// FromDIP of its own in scope at the call site, so the value is the same
// physical inset the log panes use at 100% scaling.
constexpr int kInfoSpacerWidth = 5;

// Row helpers. InsertItem() writes column 0, which is now the spacer, so a
// row is created empty and the label goes in explicitly -- keeping that in
// one place rather than at each of the ~20 call sites.
void InfoInsertRow(wxListCtrl *list, long row, const wxString &label)
{
	list->InsertItem(row, wxEmptyString);
	list->SetItem(row, kInfoLabelCol, label);
}

void InfoSetValue(wxListCtrl *list, long row, const wxString &value)
{
	list->SetItem(row, kInfoValueCol, value);
}
} // namespace

CServerWnd::CServerWnd(wxWindow *pParent /*=NULL*/, int splitter_pos)
: wxPanel(pParent, -1)
{
	wxSizer *sizer = serverListDlg(this, TRUE);

#ifdef CLIENT_GUI
	// amulegui only: "aMule Log" carries the daemon/core log forwarded over EC,
	// so give the GUI client's own messages a second tab. Inserted here rather
	// than in serverListDlg() because muuli_wdr is compiled into a shared
	// library without CLIENT_GUI and cannot tell the two builds apart.
	wxNotebook *srvLogNotebook = CastChild(ID_SRVLOG_NOTEBOOK, wxNotebook);
	if (srvLogNotebook) {
		wxPanel *guiLogPanel = new wxPanel(srvLogNotebook, -1);
		aMuleGuiLog(guiLogPanel, FALSE);
		srvLogNotebook->InsertPage(1, guiLogPanel, _("aMuleGUI Log"));
	}
#endif

	// init serverlist
	// no use now. too early.

	serverlistctrl = CastChild(ID_SERVERLIST, CServerListCtrl);

	CastChild(ID_SRV_SPLITTER, wxSplitterWindow)->SetSashPosition(splitter_pos, true);
	// Default gravity (0.0) anchors the sash to the top: when the
	// main window resizes, the server list keeps its height and the
	// log pane absorbs the extra space. The other amule splitters
	// (Shared/Transfer/Messages) use the same default and don't
	// suffer the layout-recalc storm during minimize/restore that
	// gravity 0.5 produced on Mac and Windows (#334 reproductions).
	CastChild(IDC_NODESLISTURL, wxTextCtrl)->SetValue(thePrefs::GetKadNodesUrl());
	CastChild(IDC_SERVERLISTURL, wxTextCtrl)->SetValue(thePrefs::GetEd2kServersUrl());

	// Three columns, no header: an empty spacer that insets the labels (see
	// kInfoSpacerCol), then label and value. Columns must exist before any row
	// is inserted -- adding one to a populated wxListCtrl does not shift the
	// existing per-row data across ports.
	wxListCtrl *ED2KInfoList = CastChild(ID_ED2KINFO, wxListCtrl);
	wxASSERT(ED2KInfoList);
	ED2KInfoList->InsertColumn(kInfoSpacerCol, "");
	ED2KInfoList->InsertColumn(kInfoLabelCol, "");
	ED2KInfoList->InsertColumn(kInfoValueCol, "");

	wxListCtrl *KadInfoList = CastChild(ID_KADINFO, wxListCtrl);
	wxASSERT(KadInfoList);
	KadInfoList->InsertColumn(kInfoSpacerCol, "");
	KadInfoList->InsertColumn(kInfoLabelCol, "");
	KadInfoList->InsertColumn(kInfoValueCol, "");

	// Wire Ctrl+C and right-click-to-copy on both info notebook
	// list controls (#814). Bound dynamically so the same handler
	// instance covers both ED2K Info and Kad Info.
	for (wxListCtrl *list : { ED2KInfoList, KadInfoList }) {
		list->Bind(wxEVT_KEY_DOWN, &CServerWnd::OnInfoListKeyDown, this);
		list->Bind(wxEVT_CONTEXT_MENU, &CServerWnd::OnInfoListContextMenu, this);
	}

	sizer->Show(this, TRUE);

	UpdateED2KConnectButton();
}

CServerWnd::~CServerWnd()
{
	thePrefs::SetEd2kServersUrl(CastChild(IDC_SERVERLISTURL, wxTextCtrl)->GetValue());
	thePrefs::SetKadNodesUrl(CastChild(IDC_NODESLISTURL, wxTextCtrl)->GetValue());
}

void CServerWnd::UpdateServerMetFromURL(const wxString &strURL)
{
	thePrefs::SetEd2kServersUrl(strURL);
	theApp->serverlist->UpdateServerMetFromURL(strURL);
}

void CServerWnd::OnBnClickedAddserver(wxCommandEvent &WXUNUSED(evt))
{
	wxString servername = CastChild(IDC_SERVERNAME, wxTextCtrl)->GetValue();
	wxString serveraddr = CastChild(IDC_IPADDRESS, wxTextCtrl)->GetValue();
	long port = StrToULong(CastChild(IDC_SPORT, wxTextCtrl)->GetValue());

	if (serveraddr.IsEmpty()) {
		AddLogLineC(_("Server not added: No IP or hostname specified."));
		return;
	}

	if (port <= 0 || port > 65535) {
		AddLogLineC(_("Server not added: Invalid server-port specified."));
		return;
	}

	CServer *toadd = new CServer(port, serveraddr);
	toadd->SetListName(servername.IsEmpty() ? serveraddr : servername);

	if (theApp->AddServer(toadd, true)) {
		CastChild(IDC_SERVERNAME, wxTextCtrl)->Clear();
		CastChild(IDC_IPADDRESS, wxTextCtrl)->Clear();
		CastChild(IDC_SPORT, wxTextCtrl)->Clear();
	} else {
		CServer *update =
			theApp->serverlist->GetServerByAddress(toadd->GetAddress(), toadd->GetPort());
		// See note on CServerList::AddServer
		if (update == NULL && toadd->GetIP() != 0) {
			update = theApp->serverlist->GetServerByIPTCP(toadd->GetIP(), toadd->GetPort());
		}

		if (update) {
			update->SetListName(toadd->GetListName());
			serverlistctrl->RefreshServer(update);
		}
		delete toadd;
	}

	theApp->serverlist->SaveServerMet();
}

void CServerWnd::OnBnClickedUpdateservermetfromurl(wxCommandEvent &WXUNUSED(evt))
{
	wxString strURL = CastChild(IDC_SERVERLISTURL, wxTextCtrl)->GetValue();
	UpdateServerMetFromURL(strURL);
}

void CServerWnd::OnBnClickedResetLog(wxCommandEvent &WXUNUSED(evt))
{
	theApp->GetLog(true); // Reset it.
}

void CServerWnd::OnBnClickedResetServerLog(wxCommandEvent &WXUNUSED(evt))
{
	theApp->GetServerLog(true); // Reset it
}

#ifdef CLIENT_GUI
void CServerWnd::OnBnClickedResetGuiLog(wxCommandEvent &WXUNUSED(evt))
{
	// Local-only clear of the "aMuleGUI Log" tab. Unlike the "aMule Log" reset,
	// there is nothing to reset on the daemon -- this log is generated here.
	theApp->amuledlg->ResetLog(ID_GUILOGVIEW);
}
#endif

void CServerWnd::UpdateED2KInfo()
{
	wxListCtrl *ED2KInfoList = CastChild(ID_ED2KINFO, wxListCtrl);

	ED2KInfoList->DeleteAllItems();
	InfoInsertRow(ED2KInfoList, 0, _("eD2k Status:"));

	if (theApp->IsConnectedED2K()) {
		InfoSetValue(ED2KInfoList, 0, _("Connected"));

		// Connection data
		InfoInsertRow(ED2KInfoList, 1, _("IP:Port"));
		InfoSetValue(ED2KInfoList,
			1,
			theApp->serverconnect->IsLowID()
				? wxString(_("Server"))
				: Uint32_16toStringIP_Port(theApp->GetED2KID(), thePrefs::GetPort()));

		InfoInsertRow(ED2KInfoList, 2, _("ID"));
		// No need to test the server connect, it's already true
		InfoSetValue(ED2KInfoList, 2, CFormat("%u") % theApp->GetED2KID());

		// Previously this row was inserted with an empty label and just
		// "LowID"/"HighID" in column 1, leaving a value with no key.
		// Give it an explicit label so the row is self-explanatory.
		InfoInsertRow(ED2KInfoList, 3, _("Connection Type:"));
		InfoSetValue(ED2KInfoList, 3, theApp->serverconnect->IsLowID() ? _("LowID") : _("HighID"));

		// Carried over EC as EC_TAG_CONNSTATE's optional ED2K_CONNECTED_SINCE
		// sub-tag (amule-org/amule#174), so this reads the same on amulegui
		// as it does locally -- no CLIENT_GUI gate needed.
		if (theApp->GetED2KConnectedSince().IsValid()) {
			InfoInsertRow(ED2KInfoList, 4, _("Connected since:"));
			InfoSetValue(ED2KInfoList, 4, theApp->GetED2KConnectedSince().Format("%x %X"));
		}
	} else {
		// No data
		InfoSetValue(ED2KInfoList, 0, _("Not Connected"));
	}

	FitInfoListColumns(ED2KInfoList);
}

void CServerWnd::UpdateKadInfo()
{
	wxListCtrl *KadInfoList = CastChild(ID_KADINFO, wxListCtrl);

	int next_row = 0;

	KadInfoList->DeleteAllItems();

	InfoInsertRow(KadInfoList, next_row, _("Kademlia Status:"));

	if (theApp->IsKadRunning()) {
		InfoSetValue(KadInfoList,
			next_row++,
			(theApp->IsKadRunningInLanMode() ? _("Running in LAN mode") : _("Running")));

		// Connection data
		InfoInsertRow(KadInfoList, next_row, _("Kademlia client ID:"));
		InfoSetValue(KadInfoList, next_row++, theApp->GetKadID().ToHexString());
		InfoInsertRow(KadInfoList, next_row, _("Status:"));
		InfoSetValue(KadInfoList,
			next_row++,
			theApp->IsConnectedKad() ? _("Connected") : _("Disconnected"));
		if (theApp->IsConnectedKad()) {
			// Carried over EC as EC_TAG_CONNSTATE's optional
			// KAD_CONNECTED_SINCE sub-tag (amule-org/amule#174).
			if (theApp->GetKadConnectedSince().IsValid()) {
				InfoInsertRow(KadInfoList, next_row, _("Connected since:"));
				InfoSetValue(KadInfoList,
					next_row++,
					theApp->GetKadConnectedSince().Format("%x %X"));
			}
			InfoInsertRow(KadInfoList, next_row, _("Connection State:"));
			InfoSetValue(KadInfoList,
				next_row++,
				theApp->IsFirewalledKad()
					? wxString(CFormat(_("Firewalled - open TCP port %d in your router "
							     "or firewall")) %
						   thePrefs::GetPort())
					: wxString(_("OK")));
			InfoInsertRow(KadInfoList, next_row, _("UDP Connection State:"));
			bool UDPFirewalled = theApp->IsFirewalledKadUDP();
			InfoSetValue(KadInfoList,
				next_row++,
				UDPFirewalled ? wxString(CFormat(_("Firewalled - open UDP port %d in your "
								   "router or firewall")) %
							 thePrefs::GetUDPPort())
					      : wxString(_("OK")));

			if (theApp->IsFirewalledKad() || UDPFirewalled) {
				InfoInsertRow(KadInfoList, next_row, _("Firewalled state: "));
				wxString BuddyState;
				switch (theApp->GetBuddyStatus()) {
				case Disconnected:
					if (!theApp->IsFirewalledKad()) {
						BuddyState = _("No buddy required - TCP port open");
					} else if (!UDPFirewalled) {
						BuddyState = _("No buddy required - UDP port open");
					} else {
						BuddyState = _("No buddy");
					}
					break;
				case Connecting:
					BuddyState = _("Connecting to buddy");
					break;
				case Connected:
					BuddyState = CFormat(_("Connected to buddy at %s")) %
						     Uint32_16toStringIP_Port(
							     theApp->GetBuddyIP(), theApp->GetBuddyPort());
					break;
				}
				InfoSetValue(KadInfoList, next_row++, BuddyState);
			}

			InfoInsertRow(KadInfoList, next_row, _("IP address:"));
			InfoSetValue(KadInfoList, next_row++, Uint32toStringIP(theApp->GetKadIPAddress()));

			// Index info
			InfoInsertRow(KadInfoList, next_row, _("Indexed sources:"));
			InfoSetValue(KadInfoList, next_row++, CFormat("%d") % theApp->GetKadIndexedSources());
			InfoInsertRow(KadInfoList, next_row, _("Indexed keywords:"));
			InfoSetValue(
				KadInfoList, next_row++, CFormat("%d") % theApp->GetKadIndexedKeywords());
			InfoInsertRow(KadInfoList, next_row, _("Indexed notes:"));
			InfoSetValue(KadInfoList, next_row++, CFormat("%d") % theApp->GetKadIndexedNotes());
			InfoInsertRow(KadInfoList, next_row, _("Indexed load:"));
			InfoSetValue(KadInfoList, next_row++, CFormat("%d") % theApp->GetKadIndexedLoad());

			InfoInsertRow(KadInfoList, next_row, _("Average Users:"));
			InfoSetValue(KadInfoList, next_row, CastItoIShort(theApp->GetKadUsers()));
			++next_row;
			InfoInsertRow(KadInfoList, next_row, _("Average Files:"));
			InfoSetValue(KadInfoList, next_row, CastItoIShort(theApp->GetKadFiles()));
		}
	} else {
		// No data
		InfoSetValue(KadInfoList, next_row, _("Not running"));
	}

	FitInfoListColumns(KadInfoList);
}

// Both info notebooks (ED2K Info, Kad Info) are two-column wxListCtrls
// where column 0 holds short labels ("eD2k Status:", "Status:", ...) and
// column 1 holds values that can grow wide (IP:port strings, hex client
// IDs, firewall-state sentences). The previous code called
// `SetColumnWidth(col, wxLIST_AUTOSIZE)` on both columns, which on
// wxGTK ends up sizing column 1 to the *current* longest item in the
// list -- and that was sometimes narrower than the actual content, so
// the IP:Port value got truncated (#813) even though there was free
// horizontal space in the panel.
//
// Pin column 0 to autosize (its content is short and predictable) and
// let column 1 absorb whatever client width remains. Floored at a
// reasonable minimum so the column stays usable while the panel is
// being resized or before the first layout pass.
/* static */
void CServerWnd::FitInfoListColumns(wxListCtrl *list)
{
	if (!list) {
		return;
	}
	// Fixed narrow spacer; wxLIST_AUTOSIZE would collapse it to zero, since
	// every cell in it is empty.
	list->SetColumnWidth(kInfoSpacerCol, kInfoSpacerWidth);
	list->SetColumnWidth(kInfoLabelCol, wxLIST_AUTOSIZE);
	const int clientWidth = list->GetClientSize().GetWidth();
	const int used = list->GetColumnWidth(kInfoSpacerCol) + list->GetColumnWidth(kInfoLabelCol);
	// Small breathing-room pad so the rightmost glyph isn't flush
	// against the column border in GTK themes that draw cell padding
	// asymmetrically.
	constexpr int kPad = 8;
	constexpr int kValueMin = 200;
	list->SetColumnWidth(kInfoValueCol, std::max(clientWidth - used - kPad, kValueMin));
}

/* static */
void CServerWnd::CopyInfoListToClipboard(wxListCtrl *list)
{
	if (!list || list->GetItemCount() == 0) {
		return;
	}

	// If the user has rows selected, copy only those; otherwise copy
	// the whole list. Matches the affordance most users expect from
	// list-style read-only data panels.
	std::vector<long> rows;
	long sel = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	while (sel != -1) {
		rows.push_back(sel);
		sel = list->GetNextItem(sel, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	}
	if (rows.empty()) {
		for (long i = 0; i < list->GetItemCount(); ++i) {
			rows.push_back(i);
		}
	}

	wxString out;
	for (long row : rows) {
		// Columns by name: column 0 is the spacer and always empty, so
		// copying it would prefix every line with a stray tab.
		const wxString label = list->GetItemText(row, kInfoLabelCol);
		const wxString value = list->GetItemText(row, kInfoValueCol);
		out << label << '\t' << value << '\n';
	}

	// wxClipboard isn't always open by default; explicitly Open/Close.
	// SetData takes ownership of the wxTextDataObject*.
	if (wxTheClipboard->Open()) {
		wxTheClipboard->SetData(new wxTextDataObject(out));
		wxTheClipboard->Close();
	}
}

void CServerWnd::OnInfoListKeyDown(wxKeyEvent &evt)
{
	// Ctrl+C (or Cmd+C on macOS, which wx maps to ControlDown for
	// wxKeyEvent on standard menus). Anything else falls through to
	// the default key handler so arrow navigation etc. still works.
	if ((evt.GetKeyCode() == 'C' || evt.GetKeyCode() == 'c') && evt.ControlDown()) {
		wxListCtrl *list = wxDynamicCast(evt.GetEventObject(), wxListCtrl);
		CopyInfoListToClipboard(list);
		return;
	}
	evt.Skip();
}

void CServerWnd::OnInfoListContextMenu(wxContextMenuEvent &evt)
{
	wxListCtrl *list = wxDynamicCast(evt.GetEventObject(), wxListCtrl);
	if (!list) {
		evt.Skip();
		return;
	}

	wxMenu menu;
	menu.Append(kInfoListMenuCopy, _("Copy"));

	// Stash the target list on the menu's client-data slot so the
	// EVT_MENU handler knows which list control fired the event
	// without an extra member variable.
	menu.SetClientData(list);
	menu.Bind(wxEVT_MENU, &CServerWnd::OnInfoListCopy, this, kInfoListMenuCopy);

	// Pop up at the event position (already in screen coords for
	// wxContextMenuEvent); fall back to the list's centre if the
	// event came from a keyboard menu key with no position.
	const wxPoint pos = evt.GetPosition() != wxDefaultPosition
				    ? list->ScreenToClient(evt.GetPosition())
				    : wxPoint(list->GetClientSize().GetWidth() / 2,
					      list->GetClientSize().GetHeight() / 2);
	list->PopupMenu(&menu, pos);
}

void CServerWnd::OnInfoListCopy(wxCommandEvent &evt)
{
	wxMenu *menu = wxDynamicCast(evt.GetEventObject(), wxMenu);
	if (!menu) {
		return;
	}
	wxListCtrl *list = static_cast<wxListCtrl *>(menu->GetClientData());
	CopyInfoListToClipboard(list);
}

void CServerWnd::OnSashPositionChanging(wxSplitterEvent &evt)
{
	// CHANGING fires only while the user is actively dragging the
	// sash; mark the drag in flight so OnSashPositionChanged knows
	// the next CHANGED event came from a real user gesture (and not
	// a layout reflow during minimize/restore).
	m_userDraggingSash = true;
	evt.Skip();
}

void CServerWnd::OnSashPositionChanged(wxSplitterEvent &WXUNUSED(evt))
{
	wxSplitterWindow *split = CastChild("SrvSplitterWnd", wxSplitterWindow);
	if (!m_userDraggingSash) {
		// Layout-induced sash move — don't persist. With the default
		// sash gravity, these should be rare; previously gravity 0.5
		// produced a storm of CHANGED events during minimize/restore
		// reflows that pushed the sash out of the visible range.
		return;
	}
	m_userDraggingSash = false;
	if (theApp->amuledlg && split) {
		theApp->amuledlg->m_srv_split_pos = split->GetSashPosition();
	}
}

void CServerWnd::OnBnClickedED2KDisconnect(wxCommandEvent &WXUNUSED(evt))
{
	// Doubles as Connect/Cancel/Disconnect depending on the button's
	// current state (see UpdateED2KConnectButton()).
	if (theApp->serverconnect->IsConnecting()) {
		theApp->serverconnect->StopConnectionTry();
	} else if (theApp->IsConnectedED2K()) {
		theApp->serverconnect->Disconnect();
	} else {
		AddLogLineC(_("Connecting"));
		theApp->serverconnect->ConnectToAnyServer();
	}
}

void CServerWnd::UpdateED2KConnectButton()
{
	wxButton *button = CastChild(IDC_ED2KDISCONNECT, wxButton);
	wxCHECK_RET(button, "'IDC_ED2KDISCONNECT' widget not found");

	EConnButtonState state;
	if (theApp->IsConnectedED2K()) {
		state = ConnButtonConnected;
	} else if (theApp->serverconnect->IsConnecting()) {
		state = ConnButtonConnecting;
	} else {
		state = ConnButtonOff;
	}

	// _("ED2K") matches the translatable tab label (muuli_wdr.cpp's
	// NetDialog), so a translation that localizes the network name stays
	// consistent between the tab and this button.
	SetConnectButtonState(
		button, state, thePrefs::GetNetworkED2K() && theApp->ipfilter->IsReady(), _("ED2K"));
}
// File_checked_for_headers
