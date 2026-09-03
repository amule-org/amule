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

#ifndef CLIENTSLISTCTRL_H
#define CLIENTSLISTCTRL_H

#include <unordered_map>
#include <vector>

#include "ClientRowListCtrl.h" // Needed for CClientRowListCtrl

#define COLUMN_CLIENTS_NAME 0
#define COLUMN_CLIENTS_SOFTWARE 1
#define COLUMN_CLIENTS_VERSION 2
#define COLUMN_CLIENTS_ADDRESS 3
#define COLUMN_CLIENTS_ORIGIN 4
#define COLUMN_CLIENTS_FILES 5
#define COLUMN_CLIENTS_UP_SPEED 6
#define COLUMN_CLIENTS_DOWN_SPEED 7
#define COLUMN_CLIENTS_SESSION_UP 8
#define COLUMN_CLIENTS_SESSION_DOWN 9
#define COLUMN_CLIENTS_TOTAL_UP 10
#define COLUMN_CLIENTS_TOTAL_DOWN 11
#define COLUMN_CLIENTS_RATIO 12
//! Always empty. Absorbs the macOS trailing-column sizing; see
//! CMuleDataViewCtrl::AppendSpacerColumn().
#define COLUMN_CLIENTS_SPACER 13

/**
 * Every peer we are currently talking to, once each.
 *
 * The per-file lists (CSourceListCtrl, CSharedFilePeersListCtrl) answer "who is
 * on this file". This answers "who am I talking to", which is a different
 * question: the same peer can be a source for one download and a requester for
 * another, and only a per-client view can say so. Hence the file *count*
 * column, which the per-file lists have no way to express.
 *
 * Rows hold values rather than clients. A CClientRef would be the obvious
 * alternative and is the wrong tool -- those are owning (Unlink() deletes at
 * the last release), so the list would keep every peer it ever saw alive and
 * would never be told to let go, because the signal it is waiting for comes
 * from the destructor its own reference is preventing. The one thing a row
 * keeps of the peer itself is its ECID, which is a name to look it up by on
 * demand rather than a claim that it still exists.
 */
class CClientsListCtrl : public CClientRowListCtrl
{
public:
	/**
	 * @param tableName Names this list's saved column widths. The Active page
	 *                  shows two of these side by side and they are sized for
	 *                  different content, so they must not share one entry.
	 */
	CClientsListCtrl(wxWindow *parent,
		int id,
		const wxPoint &pos,
		wxSize size,
		int flags,
		const wxString &tableName);
	~CClientsListCtrl();

	/**
	 * One row's worth of a peer, copied at sweep time.
	 *
	 * Values, not a pointer. The list is painted asynchronously, after the
	 * sweep that produced it has returned, and a peer can be freed in
	 * between -- which read back as a valid ECID beside a garbage name, a
	 * zero address and a 2^48 byte count, and as values that never changed
	 * because dead objects do not update. Copying is affordable: the set is
	 * bounded by MaxConnections and this runs once a second only while the
	 * page is on screen.
	 */
	struct Row
	{
		//! Looks the peer up again when a row is acted on. Meaningless
		//! across daemon restarts, which is fine: so is the row.
		uint32 ecid = 0;
		//! Everything the Name cell draws, snapshotted with the rest.
		ClientNameCell nameCell;
		wxString name;
		wxString software;
		wxString version;
		uint32 ip = 0;
		uint16 port = 0;
		uint8 sourceFrom = 0;
		//! The file(s) this peer is on, by name. A peer holds at most one
		//! download and one upload, so this is one name or two -- which is
		//! what the column used to render as the digits 0, 1 and 2.
		wxString files;
		uint32 upSpeed = 0;
		double downSpeed = 0.0;
		uint64 sessionUp = 0;
		uint64 sessionDown = 0;
		uint64 totalUp = 0;
		uint64 totalDown = 0;

		/**
		 * Whole-row comparison, so a caller asking "did this row change"
		 * cannot test a subset by accident.
		 *
		 * ecid is excluded: rows are matched on it, so two rows being compared
		 * always share one. Everything else is displayed, and anything that
		 * moves has to reach the screen.
		 */
		bool operator==(const Row &other) const
		{
			return nameCell == other.nameCell && name == other.name &&
			       software == other.software && version == other.version && ip == other.ip &&
			       port == other.port && sourceFrom == other.sourceFrom && files == other.files &&
			       upSpeed == other.upSpeed && downSpeed == other.downSpeed &&
			       sessionUp == other.sessionUp && sessionDown == other.sessionDown &&
			       totalUp == other.totalUp && totalDown == other.totalDown;
		}
		bool operator!=(const Row &other) const { return !(*this == other); }
	};

	/**
	 * Replace the rows with exactly the peers that are live right now.
	 *
	 * Takes the whole set rather than individual adds and removes: the
	 * notifications that would drive those are queued when raised off the
	 * main thread, so by delivery an added client may be a reused allocation
	 * and a removed one already freed. Nothing is held between calls.
	 */
	void SetClients(std::vector<Row> &&rows);

protected:
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;
	const ClientNameCell *NameCellFor(wxUIntPtr item) const override;
	unsigned NameColumn() const override { return COLUMN_CLIENTS_NAME; }
	std::vector<PeerIdentity> SelectedPeers() const override;

	/**
	 * Speeds and transfer totals move on every poll, so a row sorted by one
	 * of them has to be able to move with it.
	 *
	 * Only those columns: answering yes unconditionally re-sorts on every
	 * refresh, which with no sort column at all -- or a static one like the
	 * name -- reshuffles equal rows once a second and reads as flicker.
	 */
	bool IsLiveSortColumn() const override;

private:
	const Row *RowFor(wxUIntPtr item) const;
	//! Rebuild m_rowOfEcid from m_rows. Call after any change to the vector.
	void ReindexRows();

	std::vector<Row> m_rows;
	/**
	 * ECID to position in m_rows.
	 *
	 * Item data is the ECID, not the row position, so a selection survives a
	 * rebuild: the sweep's order is not stable, so restoring a saved *index*
	 * would hand the selection to whichever peer now sits there -- and the
	 * context menu acts on the selection, so "Add to friends" could land on
	 * someone the user never picked. This map is what keeps the identity
	 * lookup O(1) on the paint path, which asks per cell.
	 */
	std::unordered_map<uint32, size_t> m_rowOfEcid;
};

#endif // CLIENTSLISTCTRL_H
// File_checked_for_headers
