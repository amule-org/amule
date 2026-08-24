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

#include "ClientsWnd.h"

#include "ClientVersionString.h" // Interface declarations

#include <wx/sizer.h>

#include <wx/notebook.h>
#include <wx/splitter.h>
#include <wx/stattext.h>

#include "ClientsListCtrl.h"       // Needed for CClientsListCtrl
#include "ClientHistoryListCtrl.h" // Needed for CClientHistoryListCtrl
#include "muuli_wdr.h"             // Needed for ID_CLIENTSLIST

#include <set>
#include <unordered_map>

#include <common/Format.h> // Needed for CFormat

#include "amule.h" // Needed for theApp
#ifdef GEOIP_GUI
#include "CountryDisplay.h" // Needed for GetDisplayCountryCode
#endif
#include "PartFile.h" // Needed for CPartFile (CKnownFile::GetFileName)
// CUpDownClient. MUST match the build's client class: the reduced EC client for
// amulegui, the full one for monolithic. The two have different layouts, so the
// wrong header here reads every member of a live peer at the wrong offset --
// blank names, a zero IP and nonsense totals, then a crash once a wrong offset
// lands on something that is not a string. Same trap as GenericClientListCtrl.
#ifdef CLIENT_GUI
#include "UpDownClientEC.h"
#else
#include "updownclient.h"
#endif

#ifndef CLIENT_GUI
#include "ClientCredits.h"     // Needed for CClientCredits, ClientMetaStruct
#include "ClientCreditsList.h" // Needed for CClientCreditsList
#include "ClientList.h"        // Needed for CClientList::GetClientsByHash
#endif

CClientsWnd::CClientsWnd(wxWindow *parent)
: wxPanel(parent, -1)
#ifdef CLIENT_GUI
, m_historyHandler(this)
#endif
{
	// Two tabs rather than a split: the lists answer different questions --
	// "who am I talking to now" and "who have I ever talked to" -- and share
	// most of their columns, so showing both at once would mostly duplicate
	// the same headers down the page.
	wxNotebook *book = new wxNotebook(this, -1);
	const long listStyle = wxDV_MULTIPLE | wxDV_ROW_LINES | wxDV_VERT_RULES;

	// Split rather than one list: a peer is either giving us a file or taking
	// one, and often both at once, so a single list has to render each row's
	// direction into a column and leaves the reader to sort it out. Two panes
	// state it structurally -- and a peer swapping with us simply appears in
	// both, once as a source and once as a destination, which is exactly what
	// is happening.
	wxSplitterWindow *split = new wxSplitterWindow(
		book, ID_CLIENTSSPLITTER, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3DSASH);
	split->SetMinimumPaneSize(60);

	wxPanel *downPanel = new wxPanel(split, -1);
	downclientsctrl = new CClientsListCtrl(
		downPanel, ID_CLIENTSLIST, wxDefaultPosition, wxDefaultSize, listStyle, "ClientsDown");
	wxBoxSizer *downSizer = new wxBoxSizer(wxVERTICAL);
	downSizer->Add(new wxStaticText(downPanel, -1, _("Downloading from")), 0, wxALL, 3);
	downSizer->Add(downclientsctrl, 1, wxEXPAND);
	downPanel->SetSizer(downSizer);

	wxPanel *upPanel = new wxPanel(split, -1);
	upclientsctrl = new CClientsListCtrl(
		upPanel, ID_CLIENTSUPLIST, wxDefaultPosition, wxDefaultSize, listStyle, "ClientsUp");
	wxBoxSizer *upSizer = new wxBoxSizer(wxVERTICAL);
	upSizer->Add(new wxStaticText(upPanel, -1, _("Uploading to")), 0, wxALL, 3);
	upSizer->Add(upclientsctrl, 1, wxEXPAND);
	upPanel->SetSizer(upSizer);

	split->SplitHorizontally(downPanel, upPanel);
	// Even halves. The two sides carry comparable numbers of peers and neither
	// is the subordinate detail pane the Transfers splitter's ratio assumes.
	split->SetSashGravity(0.5);
	book->AddPage(split, _("Active"), true);

	// Only offered when there is a history to show. A daemon that does not
	// advertise EC_TAG_CAN_CLIENT_HISTORY cannot answer the request, so the tab
	// would sit there permanently empty with nothing to say why -- better not
	// to promise it. Decided once here because amulegui rebuilds this dialog on
	// every (re)connect, so a later connection to a newer daemon gets the tab.
#ifdef CLIENT_GUI
	const bool historyAvailable =
		theApp->m_connect != nullptr && theApp->m_connect->ServerSupportsClientHistory();
#else
	const bool historyAvailable = true;
#endif
	if (historyAvailable) {
		historylistctrl = new CClientHistoryListCtrl(
			book, ID_CLIENTHISTORYLIST, wxDefaultPosition, wxDefaultSize, listStyle);
		book->AddPage(historylistctrl, _("Known"), false);
	}

	// Rebuilt on every switch to the Known tab rather than once, so a peer
	// that has reconnected since you last looked shows its new totals and
	// last-seen instead of the values it had months ago.
	book->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent &event) {
		if (event.GetSelection() == 1 && historylistctrl != nullptr) {
			EnsureHistoryLoaded();
		}
		event.Skip();
	});

	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(book, 1, wxEXPAND);
	SetSizer(sizer);
	sizer->SetSizeHints(this);
	sizer->Fit(this);
}

namespace
{
//! The Name cell for a history row. One helper because the monolithic and EC
//! paths build the same rows from different sources and must agree on this.
void FillHistoryNameCell(ClientHistoryRow &row)
{
	row.nameCell.name = row.name.IsEmpty() ? row.hash.Encode() : row.name;
	// No live peer behind this row, so no state badge and no friend/credit
	// marks -- those describe a conversation in progress.
	row.nameCell.showState = false;
	row.identityKnown = row.hasMeta;
	row.nameCell.knownSoftware = row.hasMeta;
	row.nameCell.clientSoft = row.clientSoft;
	row.nameCell.obfuscation = row.obfuscation;
#ifdef GEOIP_GUI
	// Same call the live lists make: amulegui takes the code the core resolved
	// from the last address we saw the peer at, monolithic resolves it itself.
	// Records with no metadata carry no address, and get no flag.
	wxString code;
	if (GetDisplayCountryCode(row.countryFromCore, row.country, row.ip, code)) {
		row.nameCell.countryCode = code;
	}
#endif
}
} // namespace

void CClientsWnd::EnsureHistoryLoaded()
{
#ifdef CLIENT_GUI
	// Session 0 means the daemon never told us which process it is, so we
	// cannot claim what we hold is still its store.
	const uint64 session = theApp->m_connect != nullptr ? theApp->m_connect->GetServerSessionId() : 0;
	if (m_historyLoaded && session != 0 && session == m_historySessionId) {
		return;
	}
	m_historySessionId = session;
#else
	// Monolithic reads the store directly, and it is the same store for as
	// long as the program runs.
	if (m_historyLoaded) {
		return;
	}
#endif
	m_historyLoaded = true;
	LoadHistory();
}

void CClientsWnd::LoadHistory()
{
#ifndef CLIENT_GUI
	// Monolithic: the credit store is right here, so there is nothing to
	// request and nothing to wait for.
	if (theApp->clientcredits == nullptr) {
		return;
	}
	std::vector<CClientCredits *> credits;
	theApp->clientcredits->GetAllCredits(credits);

	std::vector<ClientHistoryRow> rows;
	rows.reserve(credits.size());
	for (const CClientCredits *cur : credits) {
		const CreditStruct *data = cur->GetDataStruct();
		ClientHistoryRow row;
		row.hash = data->key;
		row.uploaded = cur->GetUploadedTotal();
		row.downloaded = cur->GetDownloadedTotal();
		row.lastSeen = data->nLastSeen;
		if (cur->HasMeta()) {
			const ClientMetaStruct &meta = cur->GetMeta();
			row.hasMeta = true;
			row.name = meta.name;
			row.firstSeen = meta.firstSeen;
			row.sessions = meta.sessions;
			row.ip = meta.lastIP;
			row.port = meta.lastPort;
			row.clientSoft = meta.clientSoft;
			row.sourceFrom = meta.sourceFrom;
			row.obfuscation = meta.obfuscation;
			// Rendered the same way the live list renders it, so one peer is
			// not listed under two different versions depending on whether it
			// happens to be online.
			row.version = FormatPackedClientVersion(meta.clientSoft, meta.version);
		}
		// Correlate with the live list by hash. Not by ECID: those mean
		// nothing outside one daemon process, whereas this is the same
		// identity the credit store itself is keyed on.
		row.online = !theApp->clientlist->GetClientsByHash(row.hash).empty();
		FillHistoryNameCell(row);
		rows.push_back(row);
	}
	historylistctrl->SetRows(std::move(rows));
#else
	// amulegui: the credit store lives on the other side of the link, so ask
	// for it. Against a daemon too old to know the request this comes back
	// EC_OP_FAILED and the tab simply stays empty -- there is nothing to
	// negotiate in advance, the failure says it.
	if (theApp->m_connect != nullptr && theApp->m_connect->ServerSupportsClientHistory()) {
		CECPacket request(EC_OP_GET_CLIENT_HISTORY);
		theApp->m_connect->SendRequest(&m_historyHandler, &request);
	}
#endif
}

#ifdef CLIENT_GUI
void CClientsWnd::CHistoryHandler::HandlePacket(const CECPacket *packet)
{
	if (packet->GetOpCode() != EC_OP_CLIENT_HISTORY) {
		// EC_OP_FAILED from a core that predates the request. Leave the tab
		// as it is rather than blanking it -- an older core is not a reason
		// to throw away what is already on screen.
		return;
	}

	// The live peers, by hash. Same reasoning as the monolithic path: an
	// ECID says nothing across daemon processes, the user hash is the
	// identity the credit store itself is keyed on.
	std::set<CMD4Hash> onlineHashes;
	if (theApp->clientlist != nullptr) {
		for (const auto &entry : *theApp->clientlist) {
			onlineHashes.insert(entry->GetClient()->GetUserHash());
		}
	}

	std::vector<ClientHistoryRow> rows;
	rows.reserve(packet->GetTagCount());
	for (const CECTag &entry : *packet) {
		const CECTag *tag = &entry;
		if (tag->GetTagName() != EC_TAG_CLIENT) {
			continue;
		}
		ClientHistoryRow row;
		row.hash = tag->GetMD4Data();
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_UPLOAD_TOTAL)) {
			row.uploaded = t->GetInt();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_DOWNLOAD_TOTAL)) {
			row.downloaded = t->GetInt();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_LAST_SEEN)) {
			row.lastSeen = t->GetInt();
		}
		// Everything below is absent for a peer the core has no metadata
		// for -- an older record, or a core that never kept any. The row
		// still carries a hash, totals and a date; the rest renders blank.
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_FIRST_SEEN)) {
			row.firstSeen = t->GetInt();
			row.hasMeta = true;
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_SESSIONS)) {
			row.sessions = t->GetInt();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_NAME)) {
			row.name = t->GetStringData();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_USER_IP)) {
			row.ip = t->GetInt();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_USER_PORT)) {
			row.port = t->GetInt();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_SOFTWARE)) {
			row.clientSoft = t->GetInt();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_SOFT_VER_STR)) {
			row.version = t->GetStringData();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_FROM)) {
			row.sourceFrom = t->GetInt();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_OBFUSCATION_STATUS)) {
			row.obfuscation = t->GetInt();
		}
		if (const CECTag *t = tag->GetTagByName(EC_TAG_CLIENT_COUNTRY)) {
			row.country = t->GetStringData();
			row.countryFromCore = true;
		}
		row.online = onlineHashes.count(row.hash) != 0;
		FillHistoryNameCell(row);
		rows.push_back(row);
	}
	if (m_owner->historylistctrl != nullptr) {
		m_owner->historylistctrl->SetRows(std::move(rows));
	}
}
#endif

CClientsWnd::~CClientsWnd() = default;

void CClientsWnd::UpdateAll()
{
	// Rebuild the row set from the live container rather than maintaining it
	// from add/remove notifications.
	//
	// Those notifications are queued whenever they are raised off the main
	// thread, which is exactly what CUpDownClientListRem does -- so an add
	// could arrive after its client's allocation had been reused (rows full
	// of blank peers whose values never moved), and a removal could arrive
	// after the object was freed, leaving the list to repaint a dangling
	// pointer once a second until it crashed inside drawing.
	//
	// Holding CClientRefs instead would fix the lifetime and create a worse
	// problem: those are owning, so the list would keep every peer it ever
	// saw alive. Enumerating what is live, when we draw, has neither failure
	// mode, and the set is bounded by MaxConnections.
	// Copy each peer's values here, while we know they are alive. The list
	// is painted later, and a peer freed in between would otherwise be read
	// through a dangling pointer at draw time.
	std::vector<CClientsListCtrl::Row> downRows;
	std::vector<CClientsListCtrl::Row> upRows;
	// Every connected peer, for the history reconcile below -- not just the
	// ones that pass the pane filter. A peer holding no file is still online,
	// and the Known tab says so.
	std::unordered_map<CMD4Hash, CClientHistoryListCtrl::LiveClient> live;
	const bool wantLive = historylistctrl != nullptr && historylistctrl->IsLoaded();

	auto snapshot = [&downRows, &upRows, &live, wantLive](const CUpDownClient *c) {
		if (c == nullptr) {
			return;
		}
		// Built at most once per peer, and only if something asks for it. It
		// is the most expensive thing here -- a country lookup and several
		// string copies -- and it used to be built twice for every peer, once
		// for the history entry and again for the pane row. Lazily, because a
		// peer that neither pane shows and that the history does not want
		// should not pay for one at all (issue #920).
		ClientNameCell cell;
		bool haveCell = false;
		auto nameCell = [&]() -> const ClientNameCell & {
			if (!haveCell) {
				cell = MakeClientNameCell(c);
				haveCell = true;
			}
			return cell;
		};

		if (wantLive && !c->GetUserHash().IsEmpty()) {
			CClientHistoryListCtrl::LiveClient entry;
			entry.uploaded = c->GetUploadedTotal();
			entry.downloaded = c->GetDownloadedTotal();
			entry.upSpeed = c->GetUploadDatarate();
			entry.downSpeed = c->GetKBpsDown();
			entry.name = c->GetUserName();
			entry.version = c->GetSoftVerStr();
			entry.ip = c->GetIP();
			entry.port = c->GetUserPort();
			entry.clientSoft = static_cast<uint8>(c->GetClientSoft());
			entry.sourceFrom = static_cast<uint8>(c->GetSourceFrom());
			entry.nameCell = nameCell();
			// A history row describes a peer we may not be talking to, so it
			// carries no live download-state badge even when we are.
			entry.nameCell.showState = false;
			live[c->GetUserHash()] = entry;
		}
		// Which pane(s) this peer belongs in. A peer holds at most one file in
		// each direction, and a peer swapping with us holds one of each -- so
		// it is listed twice, once as a source and once as a destination,
		// rather than being forced into a single row that has to explain
		// itself. Membership is the relationship, not whether bytes are moving
		// this second: a queued source is still someone we are downloading
		// from, and the speed columns already say whether it is live.
		const CKnownFile *requested = c->GetRequestFile();
		const CKnownFile *uploading = c->GetUploadFile();
		if (requested == nullptr && uploading == nullptr) {
			return;
		}
		CClientsListCtrl::Row row;
		row.ecid = c->ECID();
		row.nameCell = nameCell();
		row.name = c->GetUserName();
		row.software = c->GetSoftStr();
		row.version = c->GetSoftVerStr();
		row.ip = c->GetIP();
		row.port = c->GetUserPort();
		row.sourceFrom = static_cast<uint8>(c->GetSourceFrom());
		row.upSpeed = c->GetUploadDatarate();
		row.downSpeed = c->GetKBpsDown();
		row.sessionUp = c->GetTransferredUp();
		row.sessionDown = c->GetTransferredDown();
		row.totalUp = c->GetUploadedTotal();
		row.totalDown = c->GetDownloadedTotal();

		// The Files column names one file, not a list, because which file it is
		// follows from which pane the row is in.
		if (requested != nullptr) {
			row.files = requested->GetFileName().GetPrintable();
			downRows.push_back(row);
		}
		if (uploading != nullptr) {
			row.files = uploading->GetFileName().GetPrintable();
			upRows.push_back(row);
		}
	};

#ifdef CLIENT_GUI
	if (theApp->clientlist != nullptr) {
		for (const auto &entry : *theApp->clientlist) {
			snapshot(entry->GetClient());
		}
	}
#else
	if (theApp->clientlist != nullptr) {
		for (const auto &entry : theApp->clientlist->GetClientList()) {
			snapshot(entry.second.GetClient());
		}
	}
#endif
	downclientsctrl->SetClients(std::move(downRows));
	upclientsctrl->SetClients(std::move(upRows));

	// Fold this tick's peers into the history, so the rows for peers that are
	// connected keep up instead of standing at whatever they were when the tab
	// was opened. Costs one lookup per connected peer; the stored records for
	// everyone else cannot change while their peer is away.
	if (wantLive) {
		historylistctrl->ReconcileLive(live);
	}

	// Only ever a re-check: EnsureHistoryLoaded() returns immediately unless
	// the daemon session changed, and the m_historyLoaded guard means sitting
	// on the Active tab never triggers the first, expensive load. Without it a
	// core that restarted while the Known tab was open would keep showing rows
	// belonging to a process that no longer exists.
	if (m_historyLoaded) {
		EnsureHistoryLoaded();
	}
}
