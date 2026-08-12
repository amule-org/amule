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

#include "ClientsWnd.h" // Interface declarations

#include <wx/sizer.h>

#include <wx/notebook.h>

#include "ClientsListCtrl.h"       // Needed for CClientsListCtrl
#include "ClientHistoryListCtrl.h" // Needed for CClientHistoryListCtrl
#include "muuli_wdr.h"             // Needed for ID_CLIENTSLIST

#include <set>

#include <common/Format.h> // Needed for CFormat

#include "amule.h"        // Needed for theApp
#include "updownclient.h" // Needed for CUpDownClient::GetUserHash

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

	clientslistctrl =
		new CClientsListCtrl(book, ID_CLIENTSLIST, wxDefaultPosition, wxDefaultSize, listStyle);
	book->AddPage(clientslistctrl, _("Active"), true);

	historylistctrl = new CClientHistoryListCtrl(
		book, ID_CLIENTHISTORYLIST, wxDefaultPosition, wxDefaultSize, listStyle);
	book->AddPage(historylistctrl, _("Known"), false);

	// Rebuilt on every switch to the Known tab rather than once, so a peer
	// that has reconnected since you last looked shows its new totals and
	// last-seen instead of the values it had months ago.
	book->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent &event) {
		if (event.GetSelection() == 1) {
			LoadHistory();
		}
		event.Skip();
	});

	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(book, 1, wxEXPAND | wxALL, 0);
	SetSizer(sizer);
	sizer->SetSizeHints(this);
	sizer->Fit(this);
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
			row.version = CFormat(wxT("v%u.%u.%u")) % (meta.version / 100000) %
				      ((meta.version % 100000) / 1000) % ((meta.version % 1000) / 100);
		}
		// Correlate with the live list by hash. Not by ECID: those mean
		// nothing outside one daemon process, whereas this is the same
		// identity the credit store itself is keyed on.
		row.online = !theApp->clientlist->GetClientsByHash(row.hash).empty();
		rows.push_back(row);
	}
	historylistctrl->SetRows(std::move(rows));
#else
	// amulegui: the credit store lives on the other side of the link, so ask
	// for it. Against a daemon too old to know the request this comes back
	// EC_OP_FAILED and the tab simply stays empty -- there is nothing to
	// negotiate in advance, the failure says it.
	if (theApp->m_connect != nullptr) {
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
		row.online = onlineHashes.count(row.hash) != 0;
		rows.push_back(row);
	}
	m_owner->historylistctrl->SetRows(std::move(rows));
}
#endif

CClientsWnd::~CClientsWnd() = default;

void CClientsWnd::UpdateAll()
{
	// A repaint, not a per-row notification: a virtual list pulls each cell's
	// value as it draws, so this re-reads exactly the rows on screen and
	// nothing else -- the same reasoning as CSharedFilesCtrl::EndBulkUpdate().
	clientslistctrl->Refresh();
}
