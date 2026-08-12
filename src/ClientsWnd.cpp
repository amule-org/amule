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

#ifndef CLIENT_GUI
#include <common/Format.h>     // Needed for CFormat
#include "amule.h"             // Needed for theApp
#include "ClientCredits.h"     // Needed for CClientCredits, ClientMetaStruct
#include "ClientCreditsList.h" // Needed for CClientCreditsList
#endif

CClientsWnd::CClientsWnd(wxWindow *parent)
: wxPanel(parent, -1)
, m_historyRequested(false)
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

	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(book, 1, wxEXPAND | wxALL, 0);
	SetSizer(sizer);
	sizer->SetSizeHints(this);
	sizer->Fit(this);
}

void CClientsWnd::LoadHistoryOnce()
{
	if (m_historyRequested) {
		return;
	}
	m_historyRequested = true;

#ifndef CLIENT_GUI
	// Monolithic: the credit store is right here, so there is nothing to
	// request and nothing to wait for.
	if (theApp->clientcredits == NULL) {
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
		rows.push_back(row);
	}
	historylistctrl->SetRows(std::move(rows));
#endif
}

CClientsWnd::~CClientsWnd() {}

void CClientsWnd::UpdateAll()
{
	// A repaint, not a per-row notification: a virtual list pulls each cell's
	// value as it draws, so this re-reads exactly the rows on screen and
	// nothing else -- the same reasoning as CSharedFilesCtrl::EndBulkUpdate().
	clientslistctrl->Refresh();
}
