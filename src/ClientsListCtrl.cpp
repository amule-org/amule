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

#include "ClientsListCtrl.h" // Interface declarations

#include <common/Format.h> // Needed for CFormat

#include "DataToText.h"     // Needed for GetSoftName, OriginToText
#include "OtherFunctions.h" // Needed for CastItoXBytes, CastItoSpeed
#include "PartFile.h"       // Needed for CPartFile
#include "updownclient.h"   // Needed for CUpDownClient
#include "muuli_wdr.h"      // Needed for ID_CLIENTSLIST

wxBEGIN_EVENT_TABLE(CClientsListCtrl, CMuleVirtualDataViewCtrl)
wxEND_EVENT_TABLE()

CClientsListCtrl::CClientsListCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags)
: CMuleVirtualDataViewCtrl(parent, id, pos, size, flags)
{
	const int colFlags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
	AddTextColumn(_("Name"), COLUMN_CLIENTS_NAME, "N", 200, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Software"), COLUMN_CLIENTS_SOFTWARE, "S", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Version"), COLUMN_CLIENTS_VERSION, "V", 90, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("IP Address"), COLUMN_CLIENTS_ADDRESS, "I", 140, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Origin"), COLUMN_CLIENTS_ORIGIN, "O", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Files"), COLUMN_CLIENTS_FILES, "F", 60, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Upload Speed"), COLUMN_CLIENTS_UP_SPEED, "U", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Download Speed"), COLUMN_CLIENTS_DOWN_SPEED, "D", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Uploaded"), COLUMN_CLIENTS_SESSION_UP, "u", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Downloaded"), COLUMN_CLIENTS_SESSION_DOWN, "d", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Total Uploaded"), COLUMN_CLIENTS_TOTAL_UP, "T", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Total Downloaded"), COLUMN_CLIENTS_TOTAL_DOWN, "t", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Ratio"), COLUMN_CLIENTS_RATIO, "R", 70, wxALIGN_LEFT, colFlags);

	AppendSpacerColumn(COLUMN_CLIENTS_SPACER);

	AssociateVirtualModel();

	ApplySorting(COLUMN_CLIENTS_NAME, 0);

	m_columnStore.SetTableName("Clients");
	LoadColumnSettings();
	InitColumnState();
}

CClientsListCtrl::~CClientsListCtrl() = default;

unsigned CClientsListCtrl::CountRelatedFiles(const CUpDownClient *client)
{
	// The two files a peer can be working on with us at once: the one it is
	// uploading to us and the one it is downloading from us. A4AF entries are
	// deliberately not counted -- those are files the peer *could* serve, not
	// ones it is exchanging, and counting them would make the column mean
	// something else.
	unsigned files = 0;
	if (client->GetRequestFile() != nullptr) {
		files++;
	}
	const CKnownFile *upload = client->GetUploadFile();
	if (upload != nullptr &&
		static_cast<const void *>(upload) != static_cast<const void *>(client->GetRequestFile())) {
		files++;
	}
	return files;
}

void CClientsListCtrl::AddClient(CUpDownClient *client)
{
	const wxUIntPtr data = reinterpret_cast<wxUIntPtr>(client);
	if (HasItemData(data)) {
		return;
	}
	AddItemData(data);
}

void CClientsListCtrl::RemoveClient(CUpDownClient *client)
{
	// Pointer value only -- see the header. The client is mid-destruction.
	const wxUIntPtr data = reinterpret_cast<wxUIntPtr>(client);
	if (HasItemData(data)) {
		RemoveItemData(data);
	}
}

wxString CClientsListCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	const CUpDownClient *client = reinterpret_cast<const CUpDownClient *>(item);
	if (client == nullptr) {
		return wxEmptyString;
	}

	switch (column) {
	case COLUMN_CLIENTS_NAME:
		// A peer that has not finished its handshake has no name yet;
		// its address is the only thing that identifies it so far.
		if (!client->GetUserName().IsEmpty()) {
			return client->GetUserName();
		}
		return Uint32toStringIP(client->GetIP());

	case COLUMN_CLIENTS_SOFTWARE:
		return client->GetSoftStr();

	case COLUMN_CLIENTS_VERSION:
		return client->GetSoftVerStr();

	case COLUMN_CLIENTS_ADDRESS:
		// GetIP(), not GetFullIP(): the latter reads m_FullUserIP, which
		// only the core fills in. Over EC the address arrives as
		// EC_TAG_CLIENT_USER_IP and lands in m_dwUserIP, so amulegui showed
		// 0.0.0.0 for every peer until this used the field that is actually
		// populated in both builds.
		return CFormat(wxT("%s:%u")) % Uint32toStringIP(client->GetIP()) % client->GetUserPort();

	case COLUMN_CLIENTS_ORIGIN:
		return OriginToText(client->GetSourceFrom());

	case COLUMN_CLIENTS_FILES:
		return CFormat(wxT("%u")) % CountRelatedFiles(client);

	case COLUMN_CLIENTS_UP_SPEED:
		return client->GetUploadDatarate() ? CastItoSpeed(client->GetUploadDatarate()) : wxString();

	case COLUMN_CLIENTS_DOWN_SPEED:
		return client->GetKBpsDown() > 0.001
			       ? CastItoSpeed(static_cast<uint32>(client->GetKBpsDown() * 1024))
			       : wxString();

	case COLUMN_CLIENTS_SESSION_UP:
		// GetTransferredUp(), not GetSessionUp(). The latter subtracts
		// m_nCurSessionUp, which nothing populates in the CLIENT_GUI build,
		// so the unsigned subtraction underflowed and every row read
		// 16777216 TB.
		return CastItoXBytes(client->GetTransferredUp());

	case COLUMN_CLIENTS_SESSION_DOWN:
		return CastItoXBytes(client->GetTransferredDown());

	case COLUMN_CLIENTS_TOTAL_UP:
		return CastItoXBytes(client->GetUploadedTotal());

	case COLUMN_CLIENTS_TOTAL_DOWN:
		return CastItoXBytes(client->GetDownloadedTotal());

	case COLUMN_CLIENTS_RATIO: {
		// Only meaningful when both directions have moved. Blank rather
		// than a zero or an infinity: on a seeding node almost every peer
		// is upload-only, and a column full of "inf" says less than a
		// column full of nothing.
		const uint64 up = client->GetUploadedTotal();
		const uint64 down = client->GetDownloadedTotal();
		if (up == 0 || down == 0) {
			return wxEmptyString;
		}
		return CFormat(wxT("%.2f")) % (static_cast<double>(down) / static_cast<double>(up));
	}

	default:
		return wxEmptyString;
	}
}

int CClientsListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	const CUpDownClient *c1 = reinterpret_cast<const CUpDownClient *>(data1);
	const CUpDownClient *c2 = reinterpret_cast<const CUpDownClient *>(data2);
	if (c1 == nullptr || c2 == nullptr) {
		return 0;
	}

	// Numeric columns compare on their values, not on the rendered strings --
	// otherwise "10 MB" sorts before "9 MB" and the sizes lie.
	switch (column) {
	case COLUMN_CLIENTS_FILES:
		return modifier * CmpAny(CountRelatedFiles(c1), CountRelatedFiles(c2));
	case COLUMN_CLIENTS_UP_SPEED:
		return modifier * CmpAny(c1->GetUploadDatarate(), c2->GetUploadDatarate());
	case COLUMN_CLIENTS_DOWN_SPEED:
		return modifier * CmpAny(c1->GetKBpsDown(), c2->GetKBpsDown());
	case COLUMN_CLIENTS_SESSION_UP:
		return modifier * CmpAny(c1->GetTransferredUp(), c2->GetTransferredUp());
	case COLUMN_CLIENTS_SESSION_DOWN:
		return modifier * CmpAny(c1->GetTransferredDown(), c2->GetTransferredDown());
	case COLUMN_CLIENTS_TOTAL_UP:
		return modifier * CmpAny(c1->GetUploadedTotal(), c2->GetUploadedTotal());
	case COLUMN_CLIENTS_TOTAL_DOWN:
		return modifier * CmpAny(c1->GetDownloadedTotal(), c2->GetDownloadedTotal());
	default:
		// Everything else is genuinely textual, so the rendered form is
		// the right thing to compare and stays in step with the column.
		return modifier *
		       GetItemColumnText(data1, column).CmpNoCase(GetItemColumnText(data2, column));
	}
}
