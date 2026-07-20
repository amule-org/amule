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

#ifndef MULEVIRTUALLISTCTRL_H
#define MULEVIRTUALLISTCTRL_H

#include "MuleListCtrl.h" // Needed for CMuleListCtrl

#include <wx/timer.h>

#include <unordered_map>
#include <vector>

/**
 * A CMuleListCtrl running in wxLC_VIRTUAL mode with a model-owned, sorted item
 * vector.
 *
 * The control stores no per-row native items — only a count — and asks the
 * subclass to render each visible row on demand. This scales to very large
 * lists (100k+): adding/removing/updating never touches native items, and a
 * sort is a plain std::sort on the model vector instead of moving native rows.
 *
 * Items are held as wxUIntPtr (the same type CMuleListCtrl used for per-item
 * data); subclasses reinterpret_cast to their element type. A subclass:
 *   - supplies its comparator via SetSortFunc() (unchanged from before),
 *   - drives the model with AddItemData()/RemoveItemData()/RefreshItemData()/
 *     AppendItemData()+FinishBulkLoad()/ClearItemData() instead of
 *     InsertItem()/DeleteItem()/SetItemPtrData()/FindItem(),
 *   - reads the row's item via ItemAt() (owner-drawn lists, in OnDrawItem) or
 *     overrides GetItemColumnText() (text-rendered lists).
 *
 * Optional live auto-sort: when the list is sorted by a column whose values
 * change during operation (override IsLiveSortColumn()), RefreshItemData()
 * schedules a re-sort that runs right after the current update batch drains
 * (via CallAfter — no fixed delay, no polling), deferred while the user is
 * interacting so rows don't slide under the cursor.
 */
class CMuleVirtualListCtrl : public CMuleListCtrl
{
public:
	CMuleVirtualListCtrl(wxWindow *parent,
		wxWindowID winid = -1,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = wxLC_ICON,
		const wxValidator &validator = wxDefaultValidator,
		const wxString &name = "mulevirtuallistctrl");
	virtual ~CMuleVirtualListCtrl();

	/** Map a (virtual) row index to its item data. The virtual control only
	 *  ever draws valid rows, and callers using selection/event indices are
	 *  likewise in range; a bounds-checked ternary here would return a literal
	 *  0 that trips the static analyzer at every reinterpret_cast<T*>(ItemAt())
	 *  deref, so index directly and assert in debug. */
	wxUIntPtr ItemAt(long row) const
	{
		wxASSERT(row >= 0 && row < static_cast<long>(m_items.size()));
		return m_items[row];
	}

	/** Number of model items (mirrors GetItemCount()). */
	long ItemDataCount() const { return static_cast<long>(m_items.size()); }

	/** Re-sort the model using the current column sort sequence and repaint,
	 *  preserving selection + focus by item identity. */
	virtual void SortList();

protected:
	// --- model mutation -------------------------------------------------
	/** Insert one item at its sorted position (current sort sequence). */
	void AddItemData(wxUIntPtr data);
	/** Append one item unsorted; pair a batch of these with FinishBulkLoad(). */
	void AppendItemData(wxUIntPtr data);
	/** Append one item unsorted and make it visible now (O(1), no re-sort).
	 *  For lists that add at the end and sort later (or not at all). */
	void AppendItemDataNow(wxUIntPtr data);
	/** Sort + index + repaint after a batch of AppendItemData() calls. */
	void FinishBulkLoad();
	/** Remove one item (no-op if absent). */
	void RemoveItemData(wxUIntPtr data);
	/** Empty the model. */
	void ClearItemData();
	/** True if the item is in the model. */
	bool HasItemData(wxUIntPtr data) const { return m_rowOf.count(data) != 0; }
	/** Row of an item, or -1 if absent. */
	long RowOfData(wxUIntPtr data) const;
	/** Repaint one item's row; if sorted by a live column, schedule a re-sort. */
	void RefreshItemData(wxUIntPtr data);

	// --- subclass hooks -------------------------------------------------
	/** Text for a cell, used by the virtual control (text-rendered lists and
	 *  keyboard type-ahead). Owner-drawn lists may leave the default (empty)
	 *  or return the primary column for type-ahead. */
	virtual wxString GetItemColumnText(wxUIntPtr WXUNUSED(item), long WXUNUSED(column)) const
	{
		return wxString();
	}

	/** Whether the current primary sort column changes value during operation
	 *  (so live auto-sort applies). Default: no live sort. */
	virtual bool IsLiveSortColumn() const { return false; }

	/** Whether a context menu is currently open (defer live re-sort). Default
	 *  no; a subclass with a tracked menu returns menu != NULL. */
	virtual bool IsMenuOpen() const { return false; }

	/** Virtual-list text callback -> GetItemColumnText(). */
	virtual wxString OnGetItemText(long item, long column) const;

private:
	// The sorted view (row i -> m_items[i]) and a data->row index kept in
	// lockstep for O(1) update/remove (rebuilt after any insert/erase/sort).
	std::vector<wxUIntPtr> m_items;
	std::unordered_map<wxUIntPtr, long> m_rowOf;

	// Live auto-sort state.
	bool m_resortPending;
	bool m_resortScheduled;
	wxTimer m_resortTimer;

	void RebuildRowIndex();
	void SyncItemCount();
	void RefreshVisible();
	void RefreshFromRow(long fromRow);
	long InsertPos(wxUIntPtr data);
	bool IsInteracting() const;
	void ScheduleResort();
	void MaybeResortNow();
	void OnResortTimer(wxTimerEvent &evt);

	wxDECLARE_EVENT_TABLE();
};

#endif // MULEVIRTUALLISTCTRL_H
