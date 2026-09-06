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

#include "MuleVirtualDataViewCtrl.h" // Interface declarations

#include <algorithm> // Needed for std::sort, std::lower_bound, std::remove

#include <wx/utils.h> // Needed for wxGetMouseState()

#include "Preferences.h" // Needed for thePrefs::LiveListSort()

namespace
{
//! Retry cadence while the user is interacting, in ms.
const int kResortRetryMs = 250;
const int kResortTimerId = wxID_HIGHEST + 1201;
} // namespace

wxBEGIN_EVENT_TABLE(CMuleVirtualDataViewCtrl, CMuleDataViewCtrl)
	EVT_TIMER(kResortTimerId, CMuleVirtualDataViewCtrl::OnResortTimer)
	EVT_KEY_DOWN(CMuleVirtualDataViewCtrl::OnArrowKey)
wxEND_EVENT_TABLE()

/**
 * The row-addressed view onto the control's item vector.
 *
 * Deliberately minimal: it holds no data of its own and answers every
 * question by indexing into the owner's m_items. Nothing here is virtual for
 * subclasses to touch -- they see the wxUIntPtr-based hooks instead.
 */
// wxDataViewVirtualListModel, not wxDataViewIndexListModel: despite the name,
// only the former is virtual in the generic backend (MSW). An index-list model
// gets a real wxDataViewTreeNode per row hanging off a root branch, and each
// branch caches the sort order it was last sorted under. Marking our columns
// sortable to get the header caret sets the window's sort order without wx
// ever sorting anything -- the list owns its own order -- so the branch cache
// and the window disagree, and the next RowChanged() trips
// "m_branchData->sortOrder == window->GetSortOrder()" in PutChildInSortOrder()
// (datavgen.cpp, present in 3.2 and 3.3 alike). A virtual list model has no
// tree nodes at all, so the code that asserts is never reached.
//
// On macOS this is a no-op: wx typedefs wxDataViewVirtualListModel to
// wxDataViewIndexListModel there ("better than nothing", dataview.h), which is
// also why the assert has only ever been seen on Windows.
class CMuleVirtualDataViewCtrl::VirtualModel : public wxDataViewVirtualListModel
{
public:
	explicit VirtualModel(CMuleVirtualDataViewCtrl *owner)
	: m_owner(owner)
	{
	}

	unsigned int GetColumnCount() const override { return m_owner->GetColumnCount(); }

	//! Taken from the column's own renderer, so a list can mix plain text and
	//! icon+text columns without telling the model about it separately.
	wxString GetColumnType(unsigned int col) const override
	{
		if (col >= m_owner->GetColumnCount()) {
			return "string";
		}
		return m_owner->GetColumn(col)->GetRenderer()->GetVariantType();
	}

	void GetValueByRow(wxVariant &variant, unsigned row, unsigned col) const override
	{
		const wxString type = GetColumnType(col);
		const bool iconText = (type == "wxDataViewIconText");
		const bool barFill = (type == "CBarFillSpec");
		if (row >= m_owner->m_items.size() || col >= m_owner->RealColumnCount()) {
			// Out of range, or the trailing spacer column, which is always
			// empty by construction.
			if (iconText) {
				variant << wxDataViewIconText(wxEmptyString, wxIcon());
			} else if (barFill) {
				variant << CBarFillSpec();
			} else {
				variant = wxString();
			}
			return;
		}

		const wxUIntPtr data = m_owner->m_items[row];
		if (barFill) {
			CBarFillSpec spec;
			m_owner->GetItemBarFill(data, col, spec);
			variant << spec;
			return;
		}

		const wxString text = m_owner->GetItemColumnText(data, col);
		if (!iconText) {
			variant = text;
			return;
		}
		wxIcon icon;
		m_owner->GetItemIcon(data, col, icon);
		variant << wxDataViewIconText(text, icon);
	}

	bool SetValueByRow(const wxVariant &, unsigned, unsigned) override { return false; }

	/**
	 * Keeps wx's own sorting in step with ours instead of letting it invent
	 * an order.
	 *
	 * The columns are sortable so the header caret is drawn, which also means
	 * wx sorts on a header click -- and wxDataViewModel::Compare() defaults
	 * to comparing the rendered strings, so "10" would land before "9" and
	 * the list's own comparator would never be consulted. Answering in terms
	 * of the row order the control already holds makes wx's sort reproduce
	 * m_items exactly, which the type-ahead and page keys rely on.
	 *
	 * The ascending flag is ignored on purpose. SortList() has already put
	 * m_items in the requested direction, so honouring it here would invert
	 * a second time and the list would snap back to where it started -- the
	 * order only appearing to change on the very first click.
	 */
	int Compare(const wxDataViewItem &item1,
		const wxDataViewItem &item2,
		unsigned int WXUNUSED(column),
		bool WXUNUSED(ascending)) const override
	{
		return static_cast<int>(GetRow(item1)) - static_cast<int>(GetRow(item2));
	}

	bool HasDefaultCompare() const override { return true; }

	bool GetAttrByRow(unsigned row, unsigned col, wxDataViewItemAttr &attr) const override
	{
		if (row >= m_owner->m_items.size()) {
			return false;
		}
		return m_owner->GetItemAttr(m_owner->m_items[row], col, attr);
	}

private:
	CMuleVirtualDataViewCtrl *m_owner;
};

CMuleVirtualDataViewCtrl::CMuleVirtualDataViewCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: CMuleDataViewCtrl(parent, winid, pos, size, style, name)
, m_virtualModel(new VirtualModel(this))
{
	m_resortTimer.SetOwner(this, kResortTimerId);
}

CMuleVirtualDataViewCtrl::~CMuleVirtualDataViewCtrl()
{
	m_virtualModel->DecRef();
}

void CMuleVirtualDataViewCtrl::AssociateVirtualModel()
{
	AssociateModel(m_virtualModel);
}

long CMuleVirtualDataViewCtrl::RowOfData(wxUIntPtr data) const
{
	const auto it = m_rowOf.find(data);
	return (it == m_rowOf.end()) ? -1 : it->second;
}

bool CMuleVirtualDataViewCtrl::GetVisibleRowRange(long &firstRow, long &lastRow) const
{
	const wxDataViewItem top = GetTopItem();
	const int perPage = GetCountPerPage();
	if (!top.IsOk() || perPage <= 0) {
		return false;
	}

	firstRow = static_cast<long>(m_virtualModel->GetRow(top));
	lastRow = firstRow + perPage - 1;
	return true;
}

bool CMuleVirtualDataViewCtrl::IsItemDataSelected(wxUIntPtr data) const
{
	const long row = RowOfData(data);
	if (row < 0) {
		return false;
	}
	return IsSelected(m_virtualModel->GetItem(static_cast<unsigned>(row)));
}

wxUIntPtr CMuleVirtualDataViewCtrl::ItemAt(long row) const
{
	if (row < 0 || static_cast<size_t>(row) >= m_items.size()) {
		return 0;
	}
	return m_items[static_cast<size_t>(row)];
}

void CMuleVirtualDataViewCtrl::RebuildRowIndex()
{
	m_rowOf.clear();
	m_rowOf.reserve(m_items.size());
	for (size_t i = 0; i < m_items.size(); ++i) {
		m_rowOf[m_items[i]] = static_cast<long>(i);
	}
}

int CMuleVirtualDataViewCtrl::CompareItemsFull(wxUIntPtr data1, wxUIntPtr data2) const
{
	// Walks the sort chain the base maintains: primary column first, then
	// the tie-breakers left by earlier header clicks.
	for (const CColPair &entry : m_sort_orders) {
		const int modifier = (entry.second & SORT_DES) ? -1 : 1;
		const bool alt = (entry.second & SORT_ALT) != 0;
		const int result = CompareItemData(data1, data2, entry.first, alt, modifier);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

long CMuleVirtualDataViewCtrl::InsertPos(wxUIntPtr data) const
{
	// With no sort chain every comparison is equal, and lower_bound would
	// answer begin() -- putting every arrival at the front, so the list fills
	// in reverse. CMuleListCtrl::GetInsertPos() appends when it has no sorter;
	// match it, so an unsorted list is at least in arrival order.
	if (m_sort_orders.empty()) {
		return static_cast<long>(m_items.size());
	}

	const auto at =
		std::lower_bound(m_items.begin(), m_items.end(), data, [this](wxUIntPtr lhs, wxUIntPtr rhs) {
			return CompareItemsFull(lhs, rhs) < 0;
		});
	return static_cast<long>(std::distance(m_items.begin(), at));
}

std::vector<wxUIntPtr> CMuleVirtualDataViewCtrl::GetSelectedItemData() const
{
	wxDataViewItemArray selection;
	const_cast<CMuleVirtualDataViewCtrl *>(this)->GetSelections(selection);

	std::vector<wxUIntPtr> data;
	data.reserve(selection.GetCount());
	for (size_t i = 0; i < selection.GetCount(); ++i) {
		const unsigned row = m_virtualModel->GetRow(selection[i]);
		if (row < m_items.size()) {
			data.push_back(m_items[row]);
		}
	}
	return data;
}

void CMuleVirtualDataViewCtrl::SetSelectedItemData(const std::vector<wxUIntPtr> &data)
{
	wxDataViewItemArray selection;
	for (wxUIntPtr item : data) {
		const long row = RowOfData(item);
		if (row >= 0) {
			selection.Add(m_virtualModel->GetItem(static_cast<unsigned>(row)));
		}
	}
	SetSelections(selection);
}

void CMuleVirtualDataViewCtrl::AddItemData(wxUIntPtr data)
{
	if (!data || HasItemData(data)) {
		return;
	}
	const long pos = InsertPos(data);

	// Selection is re-resolved rather than kept: a wxDataViewItem from a
	// row-addressed model would silently point at a different row once
	// everything below the insertion shifts down. Rows above the insertion
	// don't move, and an empty selection has nothing to preserve, so both
	// cases skip the round-trip -- it is O(selection) and this path runs per
	// arriving item.
	const bool shifts = (static_cast<size_t>(pos) < m_items.size());
	const std::vector<wxUIntPtr> selected =
		(shifts && HasSelection()) ? GetSelectedItemData() : std::vector<wxUIntPtr>();

	m_items.insert(m_items.begin() + pos, data);
	RebuildRowIndex();
	m_virtualModel->RowInserted(static_cast<unsigned>(pos));

	if (!selected.empty()) {
		SetSelectedItemData(selected);
	}
}

void CMuleVirtualDataViewCtrl::AppendItemData(wxUIntPtr data)
{
	if (!data || HasItemData(data)) {
		return;
	}
	m_items.push_back(data);
	m_rowOf[data] = static_cast<long>(m_items.size()) - 1;
}

void CMuleVirtualDataViewCtrl::AppendItemDataNow(wxUIntPtr data)
{
	if (!data || HasItemData(data)) {
		return;
	}
	m_items.push_back(data);
	m_rowOf[data] = static_cast<long>(m_items.size()) - 1;
	// Appending at the end leaves every other row where it was, so no
	// selection round-trip is needed here.
	m_virtualModel->RowInserted(static_cast<unsigned>(m_items.size()) - 1);
}

void CMuleVirtualDataViewCtrl::RemoveItemDataBatch(const std::vector<wxUIntPtr> &data)
{
	wxArrayInt rows;
	for (wxUIntPtr item : data) {
		const long row = RowOfData(item);
		if (row >= 0) {
			rows.Add(static_cast<int>(row));
		}
	}
	if (rows.IsEmpty()) {
		return;
	}

	std::vector<wxUIntPtr> selected = GetSelectedItemData();
	for (wxUIntPtr item : data) {
		selected.erase(std::remove(selected.begin(), selected.end(), item), selected.end());
	}

	// Erase from the highest row down so the earlier indices stay valid.
	wxArrayInt ordered(rows);
	ordered.Sort([](int *a, int *b) { return *b - *a; });
	for (size_t i = 0; i < ordered.GetCount(); ++i) {
		m_items.erase(m_items.begin() + ordered[i]);
	}
	RebuildRowIndex();
	// One notification for the whole set, and sent before the caller frees
	// anything these point at.
	m_virtualModel->RowsDeleted(rows);

	SetSelectedItemData(selected);
}

void CMuleVirtualDataViewCtrl::FinishBulkLoad()
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	// The row the user is looking at, remembered by its data. Reset() below
	// says the model was replaced, and every backend answers that by dropping
	// the view to the top -- so a list that rebuilds whenever a row joins it
	// scrolled itself home under anyone reading further down (the clients
	// lists do this on each arrival or departure).
	long firstRow = 0;
	long lastRow = 0;
	wxUIntPtr anchor = GetVisibleRowRange(firstRow, lastRow) ? ItemAt(firstRow) : 0;
	if (!anchor) {
		// The rebuild cleared the list first, so the live query has nothing to
		// answer with and ClearItemData()'s note is the only record of where
		// the user was.
		anchor = m_bulkAnchor;
	}
	m_bulkAnchor = 0;

	SortItems();

	// Reset() here, unlike SortList(): AppendItemData() adds rows without
	// telling the control -- that is what makes a bulk load cheap -- so this
	// is the only notification that the rows exist at all. A repaint would
	// draw a list the control still believes is the length it was before.
	m_virtualModel->Reset(static_cast<unsigned>(m_items.size()));

	SetSelectedItemData(selected);
	ScrollDataToTop(anchor);
}

void CMuleVirtualDataViewCtrl::ScrollDataToTop(wxUIntPtr data)
{
	if (!data) {
		return;
	}
	const long row = RowOfData(data);
	if (row < 0) {
		return;
	}
	// Reset() left the view at the top, so a row that sorted to the top is
	// already where it belongs. This is the common case on a list nobody has
	// scrolled, and it costs no scrolling at all.
	if (row == 0) {
		return;
	}

	// Two calls, because EnsureVisible() scrolls the shortest distance that
	// makes its target visible. The view is at the top after Reset(), so
	// asking for the anchor alone would stop as soon as it appeared at the
	// BOTTOM edge. Bringing the last row of the anchor's page into view first
	// scrolls past it, and the second call then comes back up to it, which
	// leaves the anchor on the top line where it was.
	const int perPage = GetCountPerPage();
	if (perPage > 1 && !m_items.empty()) {
		const long last = static_cast<long>(m_items.size()) - 1;
		const long bottom = std::min(row + perPage - 1, last);
		if (bottom > row) {
			EnsureVisible(m_virtualModel->GetItem(static_cast<unsigned>(bottom)));
		}
	}
	EnsureVisible(m_virtualModel->GetItem(static_cast<unsigned>(row)));
}

void CMuleVirtualDataViewCtrl::RemoveItemData(wxUIntPtr data)
{
	const long pos = RowOfData(data);
	if (pos < 0) {
		return;
	}
	std::vector<wxUIntPtr> selected = HasSelection() ? GetSelectedItemData() : std::vector<wxUIntPtr>();
	selected.erase(std::remove(selected.begin(), selected.end(), data), selected.end());

	m_items.erase(m_items.begin() + pos);
	RebuildRowIndex();
	// Told before the caller frees whatever `data` points at: the control
	// must not be left holding a row for an object that no longer exists.
	m_virtualModel->RowDeleted(static_cast<unsigned>(pos));

	if (!selected.empty()) {
		SetSelectedItemData(selected);
	}
}

void CMuleVirtualDataViewCtrl::RefreshItemData(wxUIntPtr data)
{
	const long row = RowOfData(data);
	if (row < 0) {
		return;
	}

	// Only rows the user can actually see are worth telling the control
	// about. Values are pulled per cell as they are drawn, so a row that is
	// off screen -- or in a list on a panel that isn't showing -- renders
	// current data the moment it is scrolled or switched into view, with no
	// notification needed. wxDataViewCtrl reaches the same conclusion, but
	// only at the very end of wxDataViewMainWindow::DoItemChanged(): before
	// it intersects the row against the client rect it invalidates every
	// column's cached best width (a clear + resize of a per-column vector)
	// and constructs and dispatches a wxEVT_DATAVIEW_ITEM_VALUE_CHANGED,
	// which no aMule handler is listening for. Measured on a reporter's
	// 11,000-row download list, that unconditional prologue cost ~20us per
	// row and ~300ms of every one-second poll -- and it was charged for the
	// list on all four panels he wasn't looking at as well as the one he
	// was (issue #867).
	//
	// The pre-wxDataViewCtrl lists did not have this problem because the
	// vendored generic wxListCtrl ordered it the other way round:
	// wxListMainWindow::RefreshLine() tested the visible range first and
	// returned, so an off-screen row cost two integer comparisons. This
	// restores that ordering.
	//
	// Skipping the notification for a row that really is visible would
	// leave a stale cell on screen, so both unknowns resolve towards
	// refreshing: a backend that cannot report its viewport, and the
	// partially visible row below the last fully visible one.
	long firstRow = 0;
	long lastRow = 0;
	const bool offScreen = !IsShownOnScreen() || (GetVisibleRowRange(firstRow, lastRow) &&
							     (row < firstRow || row > lastRow + 1));
	if (!offScreen) {
		m_virtualModel->RowChanged(static_cast<unsigned>(row));
	}

	// The value that just changed may be the one the list is sorted by, in
	// which case the row has to move. Scheduled rather than done here: an
	// update burst refreshes many rows, and re-sorting on each would be
	// quadratic. Gated by the same preference CMuleVirtualListCtrl checks.
	if (thePrefs::LiveListSort() && IsLiveSortColumn()) {
		m_resortPending = true;
		ScheduleResort();
	}
}

void CMuleVirtualDataViewCtrl::ClearItemData()
{
	// Remembered before the rows go, for the clear-then-refill rebuild the
	// clients list does on every arrival and departure: by the time
	// FinishBulkLoad() runs, the control has been told it holds nothing and
	// cannot say what was on top. Harmless for a clear that is really a clear
	// -- the data will not be in the list afterwards, and restoring skips
	// anything it cannot find.
	long firstRow = 0;
	long lastRow = 0;
	if (GetVisibleRowRange(firstRow, lastRow)) {
		m_bulkAnchor = ItemAt(firstRow);
	}

	m_items.clear();
	m_rowOf.clear();
	m_virtualModel->Reset(0);
}

void CMuleVirtualDataViewCtrl::SortItems()
{
	std::sort(m_items.begin(), m_items.end(), [this](wxUIntPtr lhs, wxUIntPtr rhs) {
		return CompareItemsFull(lhs, rhs) < 0;
	});
	RebuildRowIndex();
}

void CMuleVirtualDataViewCtrl::SortList()
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();

	// The cursor is addressed by row, like the selection, and unlike the
	// selection nothing else puts it back. Reset() used to invalidate it
	// along with the rest of the view; a repaint leaves it pointing at
	// whatever item has since moved into that row -- so the next arrow key
	// steps from the wrong place, a shifted page key extends from the wrong
	// anchor, and the focus ring can come to rest on an unselected row.
	const wxDataViewItem currentItem = GetCurrentItem();
	const wxUIntPtr currentData =
		currentItem.IsOk() ? ItemAt(static_cast<long>(m_virtualModel->GetRow(currentItem))) : 0;

	SortItems();

	// Repaint, rather than Reset(). A sort reorders rows; it does not replace
	// them, and the count is the same on both sides of the std::sort above.
	// Reset() says the model was replaced, which makes every backend rebuild
	// its view from scratch -- and a rebuilt view starts at the top, so an
	// auto-sort while the user was reading further down threw them back to
	// row 0, horizontally as well. Nothing needs telling: rows are addressed
	// by index and their values are pulled per cell as they are drawn, so the
	// cells simply render the new order.
	Refresh();

	// The control's selection is a set of rows, and the rows now mean
	// different items, so it is re-applied from the item data either way.
	SetSelectedItemData(selected);

	// Only while the row it lands on is already on screen. Measured on GTK
	// with wx 3.3.3: SetCurrentItem() to a visible row leaves the viewport
	// alone, but to one off screen it clamps the view onto the cursor -- and
	// with no row ever clicked the cursor sits at row 0, so restoring it
	// after a sort dragged the list back to the top, which is the very thing
	// this function exists to stop. A cursor the user cannot see is one they
	// are not about to arrow from; a cursor they can see is the one that has
	// to be right.
	if (currentData != 0) {
		const long row = RowOfData(currentData);
		long firstRow = 0;
		long lastRow = 0;
		if (row >= 0 && GetVisibleRowRange(firstRow, lastRow) && row >= firstRow && row <= lastRow) {
			SetCurrentItem(m_virtualModel->GetItem(static_cast<unsigned>(row)));
		}
	}
}

void CMuleVirtualDataViewCtrl::GetDisplayOrder(wxDataViewItemArray &ordered) const
{
	// m_items is already the display order, so this is a straight copy.
	ordered.Clear();
	ordered.Alloc(m_items.size());
	for (size_t i = 0; i < m_items.size(); ++i) {
		ordered.Add(m_virtualModel->GetItem(static_cast<unsigned>(i)));
	}
}

wxString CMuleVirtualDataViewCtrl::GetRowLabel(const wxDataViewItem &item) const
{
	const unsigned row = m_virtualModel->GetRow(item);
	if (row >= m_items.size()) {
		return wxEmptyString;
	}
	return GetItemColumnText(m_items[row], 0);
}

int CMuleVirtualDataViewCtrl::CompareByColumn(const wxDataViewItem &item1,
	const wxDataViewItem &item2,
	unsigned column,
	bool alt,
	int modifier) const
{
	const unsigned row1 = m_virtualModel->GetRow(item1);
	const unsigned row2 = m_virtualModel->GetRow(item2);
	if (row1 >= m_items.size() || row2 >= m_items.size()) {
		return 0;
	}
	return CompareItemData(m_items[row1], m_items[row2], column, alt, modifier);
}

void CMuleVirtualDataViewCtrl::OnSortingChanged()
{
	SortList();
}

void CMuleVirtualDataViewCtrl::SetFilterText(const wxString &text)
{
	const wxString lower = text.Lower();
	if (lower == m_filterText) {
		return;
	}
	m_filterText = lower;
	RebuildFilteredView();
}

bool CMuleVirtualDataViewCtrl::MatchesFilter(const wxString &name) const
{
	if (m_filterText.IsEmpty()) {
		return true;
	}
	// m_filterText is already lower-cased by SetFilterText().
	return name.Lower().Contains(m_filterText);
}

void CMuleVirtualDataViewCtrl::ScheduleResort()
{
	if (m_resortScheduled) {
		return; // one CallAfter coalesces the whole update burst
	}
	m_resortScheduled = true;
	CallAfter(&CMuleVirtualDataViewCtrl::MaybeResortNow);
}

void CMuleVirtualDataViewCtrl::MaybeResortNow()
{
	m_resortScheduled = false;
	if (!m_resortPending) {
		return;
	}
	// The user may have switched to a static column since this was scheduled.
	if (!IsLiveSortColumn()) {
		m_resortPending = false;
		return;
	}
	// Nobody is looking at this list, so sorting it now buys nothing. The
	// remote GUI keeps every list fed from the same poll whether or not its
	// panel is on screen, so without this a single reply re-sorts the
	// downloads, shared-files, sources and peers lists in full, once or twice
	// a second, of which at most one is visible. Measured on a 1228-row
	// shared list: 156 sorts in a 150-poll session, 143 of them while hidden.
	//
	// The work is deferred rather than dropped -- m_resortPending stays set,
	// so the next update after the panel comes back sorts it. The poll that
	// marks rows dirty runs continuously while connected, so that is within a
	// poll interval of becoming visible, and a list only has a sort pending
	// at all because something changed in it.
	if (!IsShownOnScreen()) {
		return;
	}

	// Hold off while interacting; the retry timer polls (only during
	// interaction) until idle.
	if (IsInteracting()) {
		if (!m_resortTimer.IsRunning()) {
			m_resortTimer.Start(kResortRetryMs, wxTIMER_ONE_SHOT);
		}
		return;
	}
	m_resortPending = false;
	SortList();
}

void CMuleVirtualDataViewCtrl::OnResortTimer(wxTimerEvent &WXUNUSED(evt))
{
	MaybeResortNow();
}

bool CMuleVirtualDataViewCtrl::IsInteracting() const
{
	// A context menu is open, or the user is holding the mouse (click /
	// drag-select): defer the reorder so rows don't slide under the pointer.
	if (IsMenuOpen()) {
		return true;
	}
	return wxGetMouseState().LeftIsDown();
}

void CMuleVirtualDataViewCtrl::OnArrowKey(wxKeyEvent &evt)
{
	const int key = evt.GetKeyCode();
	if (key != WXK_LEFT && key != WXK_RIGHT) {
		// Skip() so the base class still gets PageUp/PageDown/Home/End.
		evt.Skip();
		return;
	}

	// Swallowed. Left and right belong to the tree control: they expand and
	// collapse, and a flat list has nothing to expand. Handed to the native
	// control they do something worse than nothing here -- on macOS the name
	// column repaints without each row's value being re-supplied, so every
	// row draws the last row's text until something forces a full repaint.
	//
	// Scrolling sideways would be the useful alternative and is not
	// available: the only portable lever is EnsureVisible(item, column),
	// which needs to know which columns are off screen, and GetItemRect()
	// reports a column's position in the full layout rather than in the
	// viewport -- the same coordinates before and after a scroll. So the
	// target cannot be worked out, and a first attempt behaved accordingly:
	// right moved once and then stopped, left never fired at all because the
	// x it compares against never goes negative. Doing it properly means
	// reading the native scroll position per platform, which belongs in its
	// own change rather than in a repaint fix.
	//
	// Deliberately not on CMuleDataViewCtrl: the search list is a real tree
	// where these keys expand and collapse result variants.
}
