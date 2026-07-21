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

#include "MuleVirtualListCtrl.h"

#include "Preferences.h" // for thePrefs::LiveListSort()

#include <algorithm> // for std::sort / std::lower_bound

// Retry cadence for the live re-sort *while the user is interacting*. The idle
// path does not wait on this at all — it runs via CallAfter as soon as the
// update batch drains; the timer only polls during interaction until idle.
static const int MVLC_RESORT_RETRY_MS = 250;

// A private timer id, high enough not to collide with subclass menu ids.
static const int MVLC_RESORT_TIMER_ID = wxID_HIGHEST + 720;

wxBEGIN_EVENT_TABLE(CMuleVirtualListCtrl, CMuleListCtrl)
	EVT_TIMER(MVLC_RESORT_TIMER_ID, CMuleVirtualListCtrl::OnResortTimer)
wxEND_EVENT_TABLE()

CMuleVirtualListCtrl::CMuleVirtualListCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxValidator &validator,
	const wxString &name)
: CMuleListCtrl(parent, winid, pos, size, style | wxLC_VIRTUAL, validator, name)
, m_resortPending(false)
, m_resortScheduled(false)
{
	m_resortTimer.SetOwner(this, MVLC_RESORT_TIMER_ID);
}

CMuleVirtualListCtrl::~CMuleVirtualListCtrl() = default;

// --- model plumbing -------------------------------------------------------

void CMuleVirtualListCtrl::RebuildRowIndex()
{
	m_rowOf.clear();
	m_rowOf.reserve(m_items.size());
	for (long i = 0; i < static_cast<long>(m_items.size()); ++i) {
		m_rowOf[m_items[i]] = i;
	}
}

void CMuleVirtualListCtrl::RefreshVisible()
{
	// Repaint only the rows currently on screen. Invalidating the whole item
	// range (0..N) flashes the entire viewport every structural change, which
	// reads as blinking on a busy list; the off-screen rows are redrawn lazily
	// by the virtual control when scrolled into view anyway.
	if (m_items.empty()) {
		Refresh(false);
		return;
	}
	long top = GetTopItem();
	if (top < 0) {
		top = 0;
	}
	long last = top + GetCountPerPage() + 1;
	const long maxRow = static_cast<long>(m_items.size()) - 1;
	if (last > maxRow) {
		last = maxRow;
	}
	if (last >= top) {
		RefreshItems(top, last);
	}
}

void CMuleVirtualListCtrl::SyncItemCount()
{
	SetItemCount(static_cast<long>(m_items.size()));
	RefreshVisible();
}

void CMuleVirtualListCtrl::RefreshFromRow(long fromRow)
{
	// Rows at/after fromRow shifted (insert/erase); repaint from there to the
	// bottom of the viewport, leaving rows above untouched (no blink there).
	SetItemCount(static_cast<long>(m_items.size()));
	// Recompute the layout now (see AppendItemDataNow) so a single insert/erase
	// while a popup menu is open doesn't blank the list; no-op while frozen.
	EnsureLayout();
	if (m_items.empty()) {
		Refresh(false);
		return;
	}
	long top = GetTopItem();
	if (top < 0) {
		top = 0;
	}
	long first = fromRow < top ? top : fromRow;
	// Clamp into range: after an erase, fromRow can equal the old (larger) count
	// and land past the last row. RefreshAfter() bails out when its start row is
	// beyond the visible range, so an out-of-range first would skip the repaint.
	const long maxRow = static_cast<long>(m_items.size()) - 1;
	if (first > maxRow) {
		first = maxRow;
	}
	// Repaint from `first` down to the bottom of the client area, not just to the
	// last item: on an erase the rows freed below the shrunken count must be
	// cleared too, or stale, non-clickable rows linger at the bottom until a full
	// repaint (panel switch) forces one (aMule #399).
	RefreshAfter(first);
}

long CMuleVirtualListCtrl::InsertPos(wxUIntPtr data)
{
	auto it = std::lower_bound(m_items.begin(), m_items.end(), data, [this](wxUIntPtr a, wxUIntPtr b) {
		return CompareItems(a, b) < 0;
	});
	return static_cast<long>(it - m_items.begin());
}

long CMuleVirtualListCtrl::RowOfData(wxUIntPtr data) const
{
	auto it = m_rowOf.find(data);
	return it == m_rowOf.end() ? -1 : it->second;
}

void CMuleVirtualListCtrl::AppendItemData(wxUIntPtr data)
{
	// Unsorted append; caller pairs a batch with FinishBulkLoad().
	m_items.push_back(data);
}

void CMuleVirtualListCtrl::FinishBulkLoad()
{
	// AppendItemData() only grew the vector; push the new count, then sort.
	SetItemCount(static_cast<long>(m_items.size()));
	RebuildRowIndex();
	SortList();
	// SortList()'s clean-check may skip its repaint (freshly appended data can
	// happen to be in order); ensure the newly-shown rows paint at least once.
	RefreshVisible();
}

void CMuleVirtualListCtrl::AppendItemDataNow(wxUIntPtr data)
{
	m_items.push_back(data);
	m_rowOf[data] = static_cast<long>(m_items.size()) - 1;
	SetItemCount(static_cast<long>(m_items.size()));
	// Clear the just-set dirty layout now instead of waiting for idle: a single
	// add while a popup menu is open (AddSource) would otherwise leave the list
	// blank until the menu closes, since idle events don't run during a modal
	// loop. No-op inside a Freeze/Thaw batch (Thaw recomputes at its end).
	EnsureLayout();
	RefreshItem(static_cast<long>(m_items.size()) - 1);
}

void CMuleVirtualListCtrl::AddItemData(wxUIntPtr data)
{
	if (m_rowOf.find(data) != m_rowOf.end()) {
		return; // already present
	}
	const long pos = InsertPos(data);
	m_items.insert(m_items.begin() + pos, data);
	RebuildRowIndex(); // insert shifted every later row
	RefreshFromRow(pos);
}

void CMuleVirtualListCtrl::RemoveItemData(wxUIntPtr data)
{
	auto it = m_rowOf.find(data);
	if (it == m_rowOf.end()) {
		return;
	}
	const long pos = it->second;
	m_items.erase(m_items.begin() + pos);
	RebuildRowIndex(); // erase shifted every later row
	RefreshFromRow(pos);
}

void CMuleVirtualListCtrl::ClearItemData()
{
	m_items.clear();
	m_rowOf.clear();
	SetItemCount(0);
	Refresh();
}

void CMuleVirtualListCtrl::RefreshItemData(wxUIntPtr data)
{
	auto it = m_rowOf.find(data);
	if (it == m_rowOf.end()) {
		return;
	}
	RefreshItem(it->second);

	// Live auto-sort: if sorted by a column that just changed value, mark a
	// re-sort pending and schedule it. It runs once, right after the current
	// update batch (the handler that calls RefreshItemData for every changed
	// item) drains back to the event loop — so the reorder follows the data
	// with no fixed delay and no polling. Static columns never trigger this,
	// and the whole feature is gated by the Interface preference.
	if (thePrefs::LiveListSort() && IsLiveSortColumn()) {
		m_resortPending = true;
		ScheduleResort();
	}
}

wxString CMuleVirtualListCtrl::OnGetItemText(long item, long column) const
{
	return GetItemColumnText(ItemAt(item), column);
}

// --- sorting --------------------------------------------------------------

void CMuleVirtualListCtrl::SortList()
{
	// Note: don't early-out on an empty model — we still need to push a zero
	// count into the control (e.g. list cleared) so no stale rows linger.
	if (IsSorting() || !GetColumnCount() || m_items.empty()) {
		return;
	}

	// Clean check: if the model is already in order, do nothing. On a live
	// re-sort the values changed but the order very often did NOT, and
	// repainting the visible window every tick is what reads as blinking.
	if (std::is_sorted(m_items.begin(), m_items.end(), [this](wxUIntPtr a, wxUIntPtr b) {
		    return CompareItems(a, b) < 0;
	    })) {
		return;
	}

	m_isSorting = true;

	// Preserve selection + focus by item identity (in virtual mode the control
	// tracks selection by row index, so after the sort those indices point at
	// different items).
	std::vector<wxUIntPtr> selected;
	for (long p = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); p != -1;
		p = GetNextItem(p, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
		selected.push_back(ItemAt(p));
	}
	long focusPos = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED);
	wxUIntPtr focused = focusPos == -1 ? 0 : ItemAt(focusPos);

	const std::vector<wxUIntPtr> before = m_items; // snapshot for the moved-span diff
	std::sort(m_items.begin(), m_items.end(), [this](wxUIntPtr a, wxUIntPtr b) {
		return CompareItems(a, b) < 0;
	});
	RebuildRowIndex();
	// No SetItemCount: a sort does not change the item count, and calling it
	// triggers a scrollbar/position recalc that can flash the view.

	// Repaint only the contiguous span whose occupant actually changed, not
	// the whole viewport. Rows outside [first,last] hold the same item as
	// before and need no redraw.
	const long n = static_cast<long>(m_items.size());
	long first = 0;
	while (first < n && before[first] == m_items[first]) {
		++first;
	}
	long last = n - 1;
	while (last > first && before[last] == m_items[last]) {
		--last;
	}
	if (first <= last) {
		// Clear the stale by-index selection across the changed span before
		// re-applying by identity (else the selection set would accrete).
		for (long idx = first; idx <= last; ++idx) {
			SetItemState(idx, 0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
		}
		for (wxUIntPtr d : selected) {
			auto it = m_rowOf.find(d);
			if (it != m_rowOf.end()) {
				SetItemState(it->second, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
			}
		}
		if (focused) {
			auto it = m_rowOf.find(focused);
			if (it != m_rowOf.end()) {
				SetItemState(it->second, wxLIST_STATE_FOCUSED, wxLIST_STATE_FOCUSED);
			}
		}
		RefreshItems(first, last);
	}

	m_isSorting = false;
}

// --- live auto-sort scheduling --------------------------------------------

bool CMuleVirtualListCtrl::IsInteracting() const
{
	// A context menu is open, or the user is holding the mouse (click /
	// drag-select): defer the reorder so rows don't slide under the pointer.
	if (IsMenuOpen()) {
		return true;
	}
	return wxGetMouseState().LeftIsDown();
}

void CMuleVirtualListCtrl::ScheduleResort()
{
	if (m_resortScheduled) {
		return; // one CallAfter coalesces the whole update burst
	}
	m_resortScheduled = true;
	CallAfter(&CMuleVirtualListCtrl::MaybeResortNow);
}

void CMuleVirtualListCtrl::MaybeResortNow()
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
	// Hold off while interacting; the retry timer polls (only during
	// interaction) until idle.
	if (IsInteracting()) {
		if (!m_resortTimer.IsRunning()) {
			m_resortTimer.Start(MVLC_RESORT_RETRY_MS, wxTIMER_ONE_SHOT);
		}
		return;
	}
	m_resortPending = false;
	SortList();
}

void CMuleVirtualListCtrl::OnResortTimer(wxTimerEvent &WXUNUSED(evt))
{
	MaybeResortNow();
}
