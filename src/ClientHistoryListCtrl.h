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

#ifndef CLIENTHISTORYLISTCTRL_H
#define CLIENTHISTORYLISTCTRL_H

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ClientRowListCtrl.h" // Needed for CClientRowListCtrl
#include "MD4Hash.h"

#define COLUMN_HISTORY_NAME 0
#define COLUMN_HISTORY_SOFTWARE 1
#define COLUMN_HISTORY_VERSION 2
#define COLUMN_HISTORY_ADDRESS 3
#define COLUMN_HISTORY_ORIGIN 4
#define COLUMN_HISTORY_FIRST_SEEN 5
#define COLUMN_HISTORY_LAST_SEEN 6
#define COLUMN_HISTORY_SESSIONS 7
#define COLUMN_HISTORY_UP_SPEED 8
#define COLUMN_HISTORY_DOWN_SPEED 9
#define COLUMN_HISTORY_TOTAL_UP 10
#define COLUMN_HISTORY_TOTAL_DOWN 11
#define COLUMN_HISTORY_RATIO 12
//! Always empty. Absorbs the macOS trailing-column sizing.
#define COLUMN_HISTORY_SPACER 13

/**
 * One row of the clients history.
 *
 * Deliberately a plain value rather than a pointer into the credit store: the
 * monolithic build fills these from CClientCreditsList and amulegui from an
 * EC reply, and a value type is the only thing both can produce. It also means
 * the list holds nothing that can be freed underneath it -- the history is a
 * snapshot of something that only changes when a peer connects or leaves.
 *
 * Every field except the hash and the totals is optional. A record written
 * before aMule kept per-peer metadata has no name, address or software, and a
 * daemon too old to send it leaves them empty too; the list renders the gaps
 * as blanks rather than pretending to know.
 */
struct ClientHistoryRow
{
	CMD4Hash hash;
	wxString name;
	wxString version;
	uint64 uploaded = 0;
	uint64 downloaded = 0;
	uint32 lastSeen = 0;
	uint32 firstSeen = 0;
	uint32 sessions = 0;
	//! Live transfer rates, blank for a peer that is not connected.
	uint32 upSpeed = 0;
	double downSpeed = 0.0;
	uint32 ip = 0;
	uint16 port = 0;
	uint8 clientSoft = 0;
	uint8 sourceFrom = 0;
	uint8 obfuscation = 0;
	bool hasMeta = false;
	/**
	 * Name, software and origin are known from somewhere real.
	 *
	 * Not the same as hasMeta, which says the *store* holds metadata: a peer
	 * connected right now tells us who it is whether or not the core has ever
	 * written a record for it. Columns describing identity key on this;
	 * first-seen and session count still key on hasMeta, since only the store
	 * has those.
	 */
	bool identityKnown = false;
	//! Country as the core resolved it, and whether it said anything at all.
	//! Same contract as the live lists: told-by-core is authoritative even
	//! when empty, and only a build with its own resolver falls back locally.
	wxString country;
	bool countryFromCore = false;
	/**
	 * This peer is connected right now.
	 *
	 * Established by user hash, never by ECID: an ECID identifies a peer only
	 * within one daemon process, while the hash is what the credit store is
	 * keyed on and is the same peer's identity across restarts. It is the
	 * only thing that can connect a history row to the live client that is
	 * the same peer.
	 */
	bool online = false;
	//! Everything the Name cell draws, built from the stored metadata. No
	//! download-state badge: this row describes a peer we may not be talking
	//! to, and there is no live state to report.
	ClientNameCell nameCell;
};

/**
 * Every peer we have ever exchanged data with.
 *
 * Rows are addressed by index into m_rows rather than by pointer, so sorting
 * the control never invalidates them.
 */
class CClientHistoryListCtrl : public CClientRowListCtrl
{
public:
	CClientHistoryListCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags);
	~CClientHistoryListCtrl();

	/**
	 * What a currently-connected peer contributes to its history row.
	 *
	 * A record for a peer that is *not* connected cannot change -- the credit
	 * totals only move while a transfer is running, and last-seen only at
	 * disconnect -- so the connected peers are the whole of what can go stale
	 * between loads, and the sweep already walks exactly those.
	 */
	struct LiveClient
	{
		uint64 uploaded = 0;
		uint64 downloaded = 0;
		uint32 upSpeed = 0;
		double downSpeed = 0.0;
		ClientNameCell nameCell;
		wxString name;
		wxString version;
		uint32 ip = 0;
		uint16 port = 0;
		uint8 clientSoft = 0;
		uint8 sourceFrom = 0;
	};

	//! Replace everything on show with a fresh snapshot.
	void SetRows(std::vector<ClientHistoryRow> &&rows);

	/**
	 * Fold this tick's live peers into the rows.
	 *
	 * Costs one hash lookup per connected peer -- bounded by MaxConnections --
	 * never a walk of the store, which on a real node is tens of thousands of
	 * records. Peers that went away are found through the set of rows
	 * currently marked online rather than by scanning for them.
	 *
	 * Does three things a load-on-switch list cannot: keeps the totals of a
	 * transferring peer moving, clears "Online now" for one that left, and
	 * adds a row for a peer met since the tab was opened.
	 */
	void ReconcileLive(const std::unordered_map<CMD4Hash, LiveClient> &live);
	//! True once a snapshot has been supplied, so the page can tell "empty
	//! history" from "not asked yet".
	bool IsLoaded() const { return m_loaded; }

protected:
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;
	/**
	 * Last-seen, the transfer rates and the totals all move while the tab is
	 * open, so a row sorted by one of them has to be able to move with it --
	 * "Online now" sorts as the most recent thing there is, so a peer that
	 * connects or leaves changes its own place in the default order.
	 */
	bool IsLiveSortColumn() const override;
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;
	const ClientNameCell *NameCellFor(wxUIntPtr item) const override;
	unsigned NameColumn() const override { return COLUMN_HISTORY_NAME; }
	std::vector<CClientRef> SelectedClients() const override;

private:
	const ClientHistoryRow *RowFor(wxUIntPtr item) const;
	//! Append a row for a peer we have no record of yet, index it, and return
	//! its position. Does not touch m_onlineRows -- ReconcileLive() owns that.
	size_t AppendLiveRow(const CMD4Hash &hash, const LiveClient &live);

	std::vector<ClientHistoryRow> m_rows;
	//! Hash to position, so ReconcileLive() stays O(1) per peer.
	std::unordered_map<CMD4Hash, size_t> m_rowOfHash;
	//! Rows currently showing "Online now", so a peer that leaves is found
	//! without scanning every row for one that is no longer connected.
	std::unordered_set<size_t> m_onlineRows;
	bool m_loaded;
};

#endif // CLIENTHISTORYLISTCTRL_H
// File_checked_for_headers
