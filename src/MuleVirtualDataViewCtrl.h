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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
//

#ifndef MULEVIRTUALDATAVIEWCTRL_H
#define MULEVIRTUALDATAVIEWCTRL_H

#include "MuleBarRenderer.h"  // Needed for CBarFillSpec
#include "MuleDataViewCtrl.h" // Needed for CMuleDataViewCtrl

#include <wx/timer.h> // Needed for wxTimer (deferred live re-sort)

#include <unordered_map>
#include <vector>

/**
 * A CMuleDataViewCtrl holding a flat, model-owned list of items.
 *
 * The wxDataViewCtrl counterpart of CMuleVirtualListCtrl, and virtual in the
 * same sense: the control materialises nothing per row. Rows are addressed by
 * index through a wxDataViewVirtualListModel, and cell text is pulled from the
 * subclass on demand for the cells actually drawn.
 *
 * Two consequences are worth knowing before deriving from this.
 *
 * The list owns the order. Sorting is done here, over m_items, rather than by
 * wx calling wxDataViewModel::Compare() -- the columns are marked sortable so
 * the header caret appears, but wx is never asked to reorder anything.
 *
 * Item identity is not row identity. A wxDataViewItem from a row-addressed
 * model encodes the row number, so removing a row silently moves what any
 * previously-held item refers to. Everything below therefore speaks in
 * wxUIntPtr item data, and selection is preserved across mutations by
 * re-resolving that data to rows -- never by holding wxDataViewItems.
 */
class CMuleVirtualDataViewCtrl : public CMuleDataViewCtrl
{
public:
	CMuleVirtualDataViewCtrl(wxWindow *parent,
		wxWindowID winid = wxID_ANY,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxString &name = "mulevirtualdataviewctrl");

	~CMuleVirtualDataViewCtrl() override;

	//! Inserts in sorted position and tells the control about that row only.
	void AddItemData(wxUIntPtr data);
	//! Appends without sorting; pair with FinishBulkLoad().
	void AppendItemData(wxUIntPtr data);
	/**
	 * Appends one item and shows it immediately, without sorting.
	 *
	 * For the single add that has to appear while a popup menu is up: the
	 * batch path defers its work, and idle events don't run inside a modal
	 * loop, so the row would otherwise not appear until the menu closed.
	 */
	void AppendItemDataNow(wxUIntPtr data);
	//! Sorts and refreshes once, after a run of AppendItemData(). Keeps the
	//! row the user was looking at at the top of the viewport.
	void FinishBulkLoad();
	//! Drops the row, keeping the selection on the items that remain.
	void RemoveItemData(wxUIntPtr data);
	/**
	 * Drops many rows as one operation.
	 *
	 * A loop of RemoveItemData() costs an index rebuild and a selection
	 * round-trip per item; this pays each once, and tells the control about
	 * the whole set in a single notification.
	 */
	void RemoveItemDataBatch(const std::vector<wxUIntPtr> &data);
	//! Repaints just this row, re-sorting only if it moved under the sort.
	void RefreshItemData(wxUIntPtr data);
	void ClearItemData();
	//! Name-compatible with the wxListCtrl API the ported lists were written
	//! against, so their callers don't have to change.
	bool DeleteAllItems()
	{
		ClearItemData();
		return true;
	}

	bool HasItemData(wxUIntPtr data) const { return m_rowOf.count(data) != 0; }
	long RowOfData(wxUIntPtr data) const;
	/**
	 * The rows the control currently has on screen, as the inclusive range
	 * [firstRow, lastRow]. lastRow is the last *fully* visible row, so a
	 * caller that must not miss a drawn pixel has to allow for one more
	 * below it.
	 *
	 * Returns false when the backend cannot report its viewport -- the
	 * wxDataViewCtrl base answers GetTopItem()/GetCountPerPage() with
	 * "don't know" and only the ports override them. The range is then
	 * meaningless and the caller has to fall back to whatever is safe for
	 * it. Which way that falls is not symmetric: treating a visible row as
	 * off screen leaves a stale cell in front of the user, whereas treating
	 * an off-screen row as visible only costs the work it saved.
	 */
	bool GetVisibleRowRange(long &firstRow, long &lastRow) const;
	/**
	 * Whether this item is part of the selection. Answers the one question
	 * without materialising the whole selection the way
	 * GetSelectedItemData() has to -- worth the separate entry point on the
	 * per-item update paths, which ask it once per changed row.
	 */
	bool IsItemDataSelected(wxUIntPtr data) const;
	wxUIntPtr ItemAt(long row) const;
	/**
	 * Scroll so that @p data sits at the top of the viewport again.
	 *
	 * Anchored to the row's data rather than to a wxDataViewItem: in a virtual
	 * list an item is a row number, which names a different row once a sort has
	 * moved things, so it cannot survive the rebuild. Does nothing for 0 or for
	 * data no longer in the list, which is what a row that has since gone away
	 * should do.
	 */
	void ScrollDataToTop(wxUIntPtr data);
	long ItemDataCount() const { return static_cast<long>(m_items.size()); }

	/**
	 * Resolves a wxDataViewItem from an event (e.g. an activation or
	 * context-menu event) back to its row, through the
	 * wxDataViewVirtualListModel AssociateVirtualModel() installed.
	 *
	 * A wxDataViewItem's ID *is* the row number for this model, but that
	 * detail is model-internal; going through GetRow() rather than casting
	 * the ID directly is what stays correct if that ever changes.
	 */
	long GetModelRow(const wxDataViewItem &item) const
	{
		return static_cast<long>(static_cast<const wxDataViewListModel *>(GetModel())->GetRow(item));
	}

	//! Cheap "is anything selected" test, avoiding a full selection copy.
	bool HasSelection() const
	{
		return const_cast<CMuleVirtualDataViewCtrl *>(this)->GetSelectedItemsCount() > 0;
	}

	//! Item data of every selected row, in display order.
	std::vector<wxUIntPtr> GetSelectedItemData() const;
	//! Selects exactly these items, ignoring any that are no longer present.
	void SetSelectedItemData(const std::vector<wxUIntPtr> &data);

	//! Re-sorts the whole list, preserving the selection.
	void SortList();

	/**
	 * Live text filter. Setting it calls RebuildFilteredView(), which the
	 * list implements by re-populating itself from its own data source --
	 * the filter is purely a GUI-side view, exactly as on
	 * CMuleVirtualListCtrl.
	 */
	void SetFilterText(const wxString &text);

protected:
	// --- what a subclass must provide -------------------------------------

	//! Text for a cell. Called only for cells being drawn.
	virtual wxString GetItemColumnText(wxUIntPtr data, unsigned column) const = 0;

	/**
	 * Icon for a cell in a column appended as an icon+text column.
	 * Return false to draw text alone.
	 */
	virtual bool GetItemIcon(
		wxUIntPtr WXUNUSED(data), unsigned WXUNUSED(column), wxIcon &WXUNUSED(icon)) const
	{
		return false;
	}

	//! Per-row display attributes (bold, colour). Return false for default.
	virtual bool GetItemAttr(
		wxUIntPtr WXUNUSED(data), unsigned WXUNUSED(column), wxDataViewItemAttr &WXUNUSED(attr)) const
	{
		return false;
	}

	/**
	 * Fill data for a cell in a column appended with
	 * CMuleDataViewCtrl::AppendBarColumn(). Default leaves `out` empty,
	 * which CMuleBarRenderer draws as an empty bar -- fine for any column
	 * that never asks for one.
	 */
	virtual void GetItemBarFill(
		wxUIntPtr WXUNUSED(data), unsigned WXUNUSED(column), CBarFillSpec &WXUNUSED(out)) const
	{
	}

	//! Single-column comparison of two items; the caller applies `modifier`.
	virtual int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const = 0;

	/**
	 * Whether the active sort column's values change while the list is up,
	 * so a refreshed row may have to move. Gated by thePrefs::LiveListSort()
	 * exactly as CMuleVirtualListCtrl gates it.
	 */
	virtual bool IsLiveSortColumn() const { return false; }

	//! True while a context menu is up, so a reorder can be deferred.
	virtual bool IsMenuOpen() const { return false; }

	//! Re-populate the list for the current filter text.
	virtual void RebuildFilteredView() {}

	//! Whether `name` passes the current filter (case-insensitive contains).
	bool MatchesFilter(const wxString &name) const;

	// --- CMuleDataViewCtrl hooks ------------------------------------------

	void GetDisplayOrder(wxDataViewItemArray &ordered) const override;
	wxString GetRowLabel(const wxDataViewItem &item) const override;
	int CompareByColumn(const wxDataViewItem &item1,
		const wxDataViewItem &item2,
		unsigned column,
		bool alt,
		int modifier) const override;
	void OnSortingChanged() override;

	//! Associates the internal model; call once the columns are appended.
	void AssociateVirtualModel();

private:
	class VirtualModel;
	friend class VirtualModel;

	//! Full sort-chain comparison of two items.
	int CompareItemsFull(wxUIntPtr data1, wxUIntPtr data2) const;
	//! Where `data` belongs under the current sort.
	long InsertPos(wxUIntPtr data) const;
	//! Orders m_items and refreshes the row index, telling the control
	//! nothing -- the caller decides what notification its situation needs.
	void SortItems();

	//! m_rowOf is a cache of m_items; every structural change rebuilds it.
	void RebuildRowIndex();

	/**
	 * Coalesces live re-sorts. A burst of RefreshItemData() calls schedules
	 * exactly one reorder, and it is held off while the user is interacting
	 * so rows don't slide out from under the pointer -- the same treatment
	 * CMuleVirtualListCtrl gives it.
	 */
	void ScheduleResort();
	void MaybeResortNow();
	void OnResortTimer(wxTimerEvent &evt);
	//! Swallows left/right in a flat list, which has no expand/collapse for
	//! them to do and misrepaints when the native control handles them.
	void OnArrowKey(wxKeyEvent &evt);

	bool IsInteracting() const;

	VirtualModel *m_virtualModel;

	wxString m_filterText;
	bool m_resortPending = false;
	bool m_resortScheduled = false;
	wxTimer m_resortTimer;

	//! The rows, in display order. The single source of truth for ordering.
	std::vector<wxUIntPtr> m_items;
	std::unordered_map<wxUIntPtr, long> m_rowOf;
	//! Top row remembered across a ClearItemData() + refill + FinishBulkLoad()
	//! rebuild, which is the only sequence where the control cannot be asked.
	wxUIntPtr m_bulkAnchor = 0;

	wxDECLARE_EVENT_TABLE();
};

#endif // MULEVIRTUALDATAVIEWCTRL_H
