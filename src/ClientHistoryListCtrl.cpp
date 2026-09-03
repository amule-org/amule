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

#include "ClientHistoryListCtrl.h" // Interface declarations

#include <wx/datetime.h>

#include <common/Format.h> // Needed for CFormat

#include "amule.h"           // Needed for theApp
#include "ClientList.h"      // Needed for CClientList::GetClientsByHash
#include "DataToText.h"      // Needed for GetSoftName, OriginToText
#include "MuleBarRenderer.h" // Needed for CMuleBarRenderer
#include "OtherFunctions.h"  // Needed for CastItoXBytes, Uint32toStringIP
#ifdef CLIENT_GUI
#include "UpDownClientEC.h"
#else
#include "updownclient.h"
#endif

namespace
{
//! Same renderer the active list uses; see CClientRowListCtrl::GetItemBarFill.
class CHistoryNameRenderer : public CMuleBarRenderer
{
public:
	bool Render(wxRect cell, wxDC *dc, int WXUNUSED(state)) override
	{
		const ClientNameCell *data =
			reinterpret_cast<const ClientNameCell *>(GetSpec().GetIdentity());
		if (data != nullptr) {
			DrawClientNameCell(*data, cell, dc);
		}
		return true;
	}
};
} // namespace

CClientHistoryListCtrl::CClientHistoryListCtrl(
	wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags)
: CClientRowListCtrl(parent, id, pos, size, flags)
, m_loaded(false)
{
	const int colFlags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
	AddBarColumn(_("Name"), COLUMN_HISTORY_NAME, "N", 200, colFlags, new CHistoryNameRenderer());
	AddTextColumn(_("Software"), COLUMN_HISTORY_SOFTWARE, "S", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Version"), COLUMN_HISTORY_VERSION, "V", 90, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("IP Address"), COLUMN_HISTORY_ADDRESS, "I", 140, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Origin"), COLUMN_HISTORY_ORIGIN, "O", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("First seen"), COLUMN_HISTORY_FIRST_SEEN, "F", 130, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Last seen"), COLUMN_HISTORY_LAST_SEEN, "L", 130, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Sessions"), COLUMN_HISTORY_SESSIONS, "n", 80, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Upload Speed"), COLUMN_HISTORY_UP_SPEED, "U", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Download Speed"), COLUMN_HISTORY_DOWN_SPEED, "D", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Total Uploaded"), COLUMN_HISTORY_TOTAL_UP, "T", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Total Downloaded"), COLUMN_HISTORY_TOTAL_DOWN, "t", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Ratio"), COLUMN_HISTORY_RATIO, "R", 70, wxALIGN_LEFT, colFlags);

	AppendSpacerColumn(COLUMN_HISTORY_SPACER);

	AssociateVirtualModel();

	// Most-recently-seen first: on a store holding tens of thousands of
	// records, the ones worth looking at are the ones from today.
	ApplySorting(COLUMN_HISTORY_LAST_SEEN, 1);

	m_columnStore.SetTableName("ClientHistory");
	LoadColumnSettings();
	InitColumnState();
}

CClientHistoryListCtrl::~CClientHistoryListCtrl() = default;

const ClientHistoryRow *CClientHistoryListCtrl::RowFor(wxUIntPtr item) const
{
	// Stored as index+1 so that 0 stays available as "no item", which is what
	// the base returns for an unknown row.
	if (item == 0 || item > m_rows.size()) {
		return nullptr;
	}
	return &m_rows[item - 1];
}

void CClientHistoryListCtrl::SetRows(std::vector<ClientHistoryRow> &&rows)
{
	ClearItemData();
	m_rows = std::move(rows);
	m_loaded = true;

	m_rowOfHash.clear();
	m_rowOfHash.reserve(m_rows.size());
	m_onlineRows.clear();
	for (size_t i = 0; i < m_rows.size(); ++i) {
		m_rowOfHash[m_rows[i].hash] = i;
		if (m_rows[i].online) {
			m_onlineRows.insert(i);
		}
	}

	// One bulk load rather than an insert per row: the store can hold tens of
	// thousands of records and AddItemData() sorts on every insert.
	for (size_t i = 0; i < m_rows.size(); ++i) {
		AppendItemData(static_cast<wxUIntPtr>(i + 1));
	}
	FinishBulkLoad();
}

size_t CClientHistoryListCtrl::AppendLiveRow(const CMD4Hash &hash, const LiveClient &live)
{
	ClientHistoryRow row;
	row.hash = hash;
	row.uploaded = live.uploaded;
	row.downloaded = live.downloaded;
	row.upSpeed = live.upSpeed;
	row.downSpeed = live.downSpeed;
	row.name = live.name;
	row.version = live.version;
	row.ip = live.ip;
	row.port = live.port;
	row.clientSoft = live.clientSoft;
	row.sourceFrom = live.sourceFrom;
	row.nameCell = live.nameCell;
	// The core wrote this peer's metadata when it said hello -- CClientCredits
	// ::UpdateMeta() from ProcessHelloTypePacket(), which stamps first-seen the
	// first time a peer gets a record. The history snapshot predates that, so
	// the value is not in our rows; it is simply "now", which is what the core
	// recorded a moment ago. hasMeta stays false because the *store* did not
	// hold this peer when we loaded, and the columns key on the values.
	row.firstSeen = static_cast<uint32>(wxDateTime::GetTimeNow());
	row.sessions = 1;
	row.hasMeta = false;
	row.identityKnown = !live.name.IsEmpty();
	row.online = true;
	if (row.nameCell.name.IsEmpty()) {
		// Same fallback the stored rows use: the hash is all we know it by
		// until the peer says otherwise.
		row.nameCell.name = hash.Encode();
	}

	m_rows.push_back(row);
	const size_t index = m_rows.size() - 1;
	m_rowOfHash[hash] = index;
	AppendItemData(static_cast<wxUIntPtr>(index + 1));
	// Deliberately not added to m_onlineRows here: the caller owns that set for
	// the duration of a reconcile and swaps it in at the end.
	return index;
}

void CClientHistoryListCtrl::ReconcileLive(const std::unordered_map<CMD4Hash, LiveClient> &live)
{
	if (!m_loaded) {
		return;
	}

	std::unordered_set<size_t> stillOnline;
	stillOnline.reserve(live.size());
	bool appended = false;

	for (const auto &entry : live) {
		const auto found = m_rowOfHash.find(entry.first);
		if (found == m_rowOfHash.end()) {
			// A peer we had never met when the tab was loaded.
			stillOnline.insert(AppendLiveRow(entry.first, entry.second));
			appended = true;
			continue;
		}

		const size_t index = found->second;
		ClientHistoryRow &row = m_rows[index];
		stillOnline.insert(index);

		bool changed = !row.online || row.uploaded != entry.second.uploaded ||
			       row.downloaded != entry.second.downloaded ||
			       row.upSpeed != entry.second.upSpeed || row.downSpeed != entry.second.downSpeed;
		row.online = true;
		row.uploaded = entry.second.uploaded;
		row.downloaded = entry.second.downloaded;
		row.upSpeed = entry.second.upSpeed;
		row.downSpeed = entry.second.downSpeed;

		// Identity, when the peer in front of us knows more than the record
		// does. A record only gains a name when the core writes its metadata
		// at disconnect, so a peer we have never finished a session with shows
		// as its hash -- which used to resolve on the next load and now would
		// never resolve at all, since the tab loads once. A connected peer can
		// simply say who it is.
		//
		// Guarded on the live name being known: a peer whose handshake has not
		// completed yet has none, and an empty one must not overwrite a stored
		// name we already have. Beyond that the test covers every field the
		// body copies -- the badges in particular move while the name and
		// address stay put, so a narrower test would freeze them for the life
		// of the session.
		if (!entry.second.name.IsEmpty() &&
			(row.name != entry.second.name || row.version != entry.second.version ||
				row.ip != entry.second.ip || row.port != entry.second.port ||
				row.clientSoft != entry.second.clientSoft ||
				row.sourceFrom != entry.second.sourceFrom ||
				row.nameCell != entry.second.nameCell)) {
			row.name = entry.second.name;
			row.version = entry.second.version;
			row.ip = entry.second.ip;
			row.port = entry.second.port;
			row.clientSoft = entry.second.clientSoft;
			row.sourceFrom = entry.second.sourceFrom;
			row.nameCell = entry.second.nameCell;
			row.identityKnown = true;
			changed = true;
		}

		// A record we loaded before this peer had any metadata -- everything
		// written before #902 existed, which on a real store is nearly all of
		// it. The core stamped first-seen at this peer's hello, the same as for
		// a peer we had never met; only a record that already carries one is
		// left alone, since for that the stored value is the truth and ours
		// would just be the current session.
		if (row.firstSeen == 0) {
			row.firstSeen = static_cast<uint32>(wxDateTime::GetTimeNow());
			if (row.sessions == 0) {
				row.sessions = 1;
			}
			changed = true;
		}

		if (changed) {
			RefreshItemData(static_cast<wxUIntPtr>(index + 1));
		}
	}

	// Whoever was online last tick and is not in this one has gone. Found
	// through the online set, so this costs the number of departures rather
	// than a walk of the store.
	for (const size_t index : m_onlineRows) {
		if (stillOnline.count(index) == 0) {
			m_rows[index].online = false;
			// Seen until this moment, which is what the core will write to
			// the record at its own disconnect handling. Leaving the stored
			// value would show the previous disconnect as the last contact,
			// months ago for a peer that was here a second before.
			m_rows[index].lastSeen = static_cast<uint32>(wxDateTime::GetTimeNow());
			// Nothing is moving for a peer that is gone.
			m_rows[index].upSpeed = 0;
			m_rows[index].downSpeed = 0.0;
			RefreshItemData(static_cast<wxUIntPtr>(index + 1));
		}
	}
	m_onlineRows.swap(stillOnline);

	if (appended) {
		// New rows have to enter the sort order; the patches above do not.
		FinishBulkLoad();
	}
}

bool CClientHistoryListCtrl::IsLiveSortColumn() const
{
	if (m_sort_orders.empty()) {
		return false;
	}
	switch (m_sort_orders.front().first) {
	case COLUMN_HISTORY_LAST_SEEN:
	case COLUMN_HISTORY_UP_SPEED:
	case COLUMN_HISTORY_DOWN_SPEED:
	case COLUMN_HISTORY_TOTAL_UP:
	case COLUMN_HISTORY_TOTAL_DOWN:
	case COLUMN_HISTORY_RATIO:
		return true;
	default:
		return false;
	}
}

const ClientNameCell *CClientHistoryListCtrl::NameCellFor(wxUIntPtr item) const
{
	const ClientHistoryRow *row = RowFor(item);
	return row != nullptr ? &row->nameCell : nullptr;
}

namespace
{

// Without metadata the hash is all we know a peer by, which is still more
// useful than an empty cell.
wxString DisplayNameFor(const ClientHistoryRow &row)
{
	return row.name.IsEmpty() ? row.hash.Encode() : row.name;
}

} // namespace

std::vector<PeerIdentity> CClientHistoryListCtrl::SelectedPeers() const
{
	// Identity comes from the row, so a peer we are not connected to is still
	// named: the hash, name, address and port the store kept are enough to
	// friend it, and enough to open a connection if the user asks for one.
	//
	// The live client is attached when there is one, matched by user hash
	// rather than ECID: a history row outlives the daemon process whose ECIDs
	// would have named the peer, and the hash is what the credit store is
	// keyed on.
	std::vector<PeerIdentity> peers;
	for (wxUIntPtr data : GetSelectedItemData()) {
		const ClientHistoryRow *row = RowFor(data);
		if (row == nullptr || row->hash.IsEmpty()) {
			continue;
		}
		PeerIdentity peer;
		peer.hash = row->hash;
		// The record's own name, not the Name column's fallback. This one
		// is written to disk by AddFriend() and set on the live client by
		// CreateForAddress(), so a placeholder here would persist a hex
		// hash as somebody's name. Empty is meaningful: CFriend renders it
		// as "?" until the peer tells us what it is called.
		peer.name = row->name;
		peer.ip = row->ip;
		peer.port = row->port;

		// The half of the details dialog a stored record can answer. The
		// session half stays absent, which is what hasSession says.
		peer.detail.userName = row->name;
		peer.detail.userHash = row->hash;
		peer.detail.softStr = row->identityKnown ? GetSoftName(row->clientSoft) : wxString();
		peer.detail.softVerStr = row->version;
		// Left empty when unknown, so the dialog says so rather than
		// rendering a literal 0.0.0.0.
		peer.detail.fullIp = row->ip ? Uint32toStringIP(row->ip) : wxString();
		peer.detail.userPort = row->port;
		peer.detail.obfuscationStatus = row->obfuscation;
		// Same direction the list's own Total Up / Total Down columns use.
		peer.detail.uploadedTotal = row->uploaded;
		peer.detail.downloadedTotal = row->downloaded;
		peer.detail.hasSession = false;
		peer.hasDetail = true;
#ifdef CLIENT_GUI
		if (theApp->clientlist != nullptr) {
			for (const auto &entry : *theApp->clientlist) {
				CUpDownClient *client = entry->GetClient();
				if (client != nullptr && client->GetUserHash() == row->hash) {
					peer.client = *entry;
					break;
				}
			}
		}
#else
		for (const CClientRef &ref : theApp->clientlist->GetClientsByHash(row->hash)) {
			peer.client = ref;
			break;
		}
#endif
		peers.push_back(std::move(peer));
	}
	return peers;
}

wxString CClientHistoryListCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	const ClientHistoryRow *row = RowFor(item);
	if (row == nullptr) {
		return wxEmptyString;
	}

	switch (column) {
	case COLUMN_HISTORY_NAME:
		return DisplayNameFor(*row);

	case COLUMN_HISTORY_SOFTWARE:
		return row->identityKnown ? GetSoftName(row->clientSoft) : wxString();

	case COLUMN_HISTORY_VERSION:
		return row->version;

	case COLUMN_HISTORY_ADDRESS:
		if (row->ip == 0) {
			return wxEmptyString;
		}
		return CFormat(wxT("%s:%u")) % Uint32toStringIP(row->ip) % row->port;

	case COLUMN_HISTORY_ORIGIN:
		// Translated here, not by OriginToText(): see CClientsListCtrl, which
		// shares this column and had the same gap.
		return row->identityKnown ? wxGetTranslation(OriginToText(row->sourceFrom)) : wxString();

	case COLUMN_HISTORY_FIRST_SEEN:
		return row->firstSeen == 0
			       ? wxString()
			       : FormatLocalDateTime(wxDateTime(static_cast<time_t>(row->firstSeen)));

	case COLUMN_HISTORY_LAST_SEEN:
		// A date is the wrong answer for a peer that is here now -- and the
		// stored last-seen for a connected peer is whenever it previously
		// disconnected, which reads as though it were long gone.
		if (row->online) {
			return _("Online now");
		}
		if (row->lastSeen == 0) {
			return wxEmptyString;
		}
		return FormatLocalDateTime(wxDateTime(static_cast<time_t>(row->lastSeen)));

	case COLUMN_HISTORY_SESSIONS:
		if (row->sessions == 0) {
			return wxEmptyString;
		}
		return CFormat(wxT("%u")) % row->sessions;

	case COLUMN_HISTORY_UP_SPEED:
		return row->upSpeed ? CastItoSpeed(row->upSpeed) : wxString();

	case COLUMN_HISTORY_DOWN_SPEED:
		return row->downSpeed > 0.001 ? CastItoSpeed(static_cast<uint32>(row->downSpeed * 1024))
					      : wxString();

	case COLUMN_HISTORY_TOTAL_UP:
		return CastItoXBytes(row->uploaded);

	case COLUMN_HISTORY_TOTAL_DOWN:
		return CastItoXBytes(row->downloaded);

	case COLUMN_HISTORY_RATIO:
		// Blank unless both directions moved -- see the same reasoning in
		// CClientsListCtrl.
		if (row->uploaded == 0 || row->downloaded == 0) {
			return wxEmptyString;
		}
		return CFormat(wxT("%.2f")) %
		       (static_cast<double>(row->downloaded) / static_cast<double>(row->uploaded));

	default:
		return wxEmptyString;
	}
}

int CClientHistoryListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	const ClientHistoryRow *r1 = RowFor(data1);
	const ClientHistoryRow *r2 = RowFor(data2);
	if (r1 == nullptr || r2 == nullptr) {
		return 0;
	}

	switch (column) {
	case COLUMN_HISTORY_FIRST_SEEN:
		return modifier * CmpAny(r1->firstSeen, r2->firstSeen);
	case COLUMN_HISTORY_LAST_SEEN:
		// Peers that are here now sort as the most recent thing there is, so
		// the column reads as "when was this peer last around" throughout
		// instead of stranding the live ones at their stale timestamps.
		if (r1->online != r2->online) {
			return modifier * (r1->online ? 1 : -1);
		}
		return modifier * CmpAny(r1->lastSeen, r2->lastSeen);
	case COLUMN_HISTORY_SESSIONS:
		return modifier * CmpAny(r1->sessions, r2->sessions);
	case COLUMN_HISTORY_UP_SPEED:
		return modifier * CmpAny(r1->upSpeed, r2->upSpeed);
	case COLUMN_HISTORY_DOWN_SPEED:
		return modifier * CmpAny(r1->downSpeed, r2->downSpeed);
	case COLUMN_HISTORY_TOTAL_UP:
		return modifier * CmpAny(r1->uploaded, r2->uploaded);
	case COLUMN_HISTORY_TOTAL_DOWN:
		return modifier * CmpAny(r1->downloaded, r2->downloaded);
	default:
		return modifier *
		       GetItemColumnText(data1, column).CmpNoCase(GetItemColumnText(data2, column));
	}
}
