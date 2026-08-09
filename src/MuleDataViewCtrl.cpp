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

#include "MuleDataViewCtrl.h" // Interface declarations

#include <wx/menu.h> // Needed for wxMenu (header show/hide menu)

#include <algorithm> // Needed for std::find, std::sort, std::min, std::max
#include <vector>    // Needed for std::vector

#include <common/MenuIDs.h> // Needed for MP_LISTCOL_1 .. MP_LISTCOL_15

#include "GetTickCount.h"         // Needed for GetTickCount64()
#include "MuleBarRenderer.h"      // Needed for CMuleBarRenderer
#include "MuleIconTextRenderer.h" // Needed for CMuleIconTextRenderer

namespace
{
//! How long a pause resets the accumulated type-ahead string, in ms.
const uint64 kTypeAheadResetMs = 1500;
} // namespace

wxBEGIN_EVENT_TABLE(CMuleDataViewCtrl, wxDataViewCtrl)
	EVT_DATAVIEW_COLUMN_HEADER_CLICK(wxID_ANY, CMuleDataViewCtrl::OnColumnHeaderClick)
	EVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK(wxID_ANY, CMuleDataViewCtrl::OnColumnHeaderRightClick)
	EVT_MENU_RANGE(MP_LISTCOL_1, MP_LISTCOL_20, CMuleDataViewCtrl::OnColumnMenuSelected)
	EVT_IDLE(CMuleDataViewCtrl::OnIdle)
	EVT_CHAR(CMuleDataViewCtrl::OnChar)
	EVT_KEY_DOWN(CMuleDataViewCtrl::OnKeyDown)
wxEND_EVENT_TABLE()

CMuleDataViewCtrl::CMuleDataViewCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: wxDataViewCtrl(parent, winid, pos, size, style | wxDV_MULTIPLE | wxDV_ROW_LINES, wxDefaultValidator, name)
, m_widthAdapter(this)
{
	// amuleDlg sets wxIdleEvent::SetMode(wxIDLE_PROCESS_SPECIFIED), so only
	// windows carrying this style are sent idle events at all -- without it
	// OnIdle() never runs.
	SetExtraStyle(GetExtraStyle() | wxWS_EX_PROCESS_IDLE);
}

CMuleDataViewCtrl::~CMuleDataViewCtrl() = default;

int CMuleDataViewCtrl::ColumnWidthAdapter::GetColumnWidth(int col) const
{
	if (m_ctrl->IsColumnHidden(col)) {
		return 0;
	}
	const int width = m_ctrl->GetColumn(col)->GetWidth();
	if (width > 0) {
		return width;
	}
	// A visible column should never report zero -- the spacer appended on
	// macOS exists so the trailing-column sizing lands there instead. This
	// is the backstop if it ever does anyway: CListColumnStore reads a width
	// <= 0 as hidden and persists it negative, so a stray zero would hide a
	// real column on the next launch, then do the same to whichever column
	// became last, losing one per restart.
	const int cached = m_ctrl->m_columnStore.GetCachedWidth(col);
	return (cached > 0) ? cached : m_ctrl->m_columnStore.GetColumnDefaultWidth(col);
}

void CMuleDataViewCtrl::AppendSpacerColumn(unsigned modelColumn)
{
#ifdef __WXOSX__
	// Resizable is load-bearing: macOS hands the leftover space to the last
	// *resizable* column, so a fixed spacer cannot shrink and the collapse
	// falls through to the last real column instead.
	AppendTextColumn(
		wxEmptyString, modelColumn, wxDATAVIEW_CELL_INERT, 1, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
	m_hasSpacer = true;
#else
	(void)modelColumn;
#endif
}

void CMuleDataViewCtrl::AppendBarColumn(
	const wxString &title, unsigned modelColumn, int width, int flags, CMuleBarRenderer *renderer)
{
	wxDataViewColumn *column = new wxDataViewColumn(
		title, renderer ? renderer : new CMuleBarRenderer(), modelColumn, width, wxALIGN_LEFT, flags);
	AppendColumn(column);
}

void CMuleDataViewCtrl::AddTextColumn(const wxString &label,
	unsigned modelColumn,
	const wxString &key,
	int width,
	wxAlignment align,
	int flags,
	wxDataViewCellMode mode)
{
	AppendTextColumn(label, modelColumn, mode, width, align, flags);
	m_columnStore.RegisterColumn(static_cast<int>(modelColumn), width, key);
}

void CMuleDataViewCtrl::AddIconTextColumn(const wxString &label,
	unsigned modelColumn,
	const wxString &key,
	int width,
	wxAlignment align,
	int flags,
	wxDataViewCellMode mode)
{
	// CMuleIconTextRenderer rather than wx's, which reserves the icon slot on
	// every row whether or not that row has an icon -- see the class comment.
	CMuleIconTextRenderer *renderer = new CMuleIconTextRenderer();
	renderer->SetMode(mode);
	// Supply the vertical half of the alignment when the caller has not.
	// wx's own text renderer resolves a missing vertical component to its
	// default, which is centred, but a custom renderer gets exactly what it
	// is given -- so a plain wxALIGN_LEFT top-aligns the text while the icon
	// drawn beside it is centred, and the name columns sit visibly higher
	// than every other column in the same row (#867). wxALIGN_TOP is 0 and
	// so indistinguishable from "unspecified" (wx says as much in defs.h),
	// which is why this tests for the two bits that do carry intent.
	const bool hasVertical = (align & (wxALIGN_BOTTOM | wxALIGN_CENTRE_VERTICAL)) != 0;
	renderer->SetAlignment(hasVertical ? align : (align | wxALIGN_CENTRE_VERTICAL));
	AppendColumn(new wxDataViewColumn(label, renderer, modelColumn, width, align, flags));
	m_columnStore.RegisterColumn(static_cast<int>(modelColumn), width, key);
}

void CMuleDataViewCtrl::AddBarColumn(const wxString &label,
	unsigned modelColumn,
	const wxString &key,
	int width,
	int flags,
	CMuleBarRenderer *renderer)
{
	AppendBarColumn(label, modelColumn, width, flags, renderer);
	m_columnStore.RegisterColumn(static_cast<int>(modelColumn), width, key);
}

unsigned CMuleDataViewCtrl::RealColumnCount() const
{
	const unsigned columns = GetColumnCount();
	if (m_hasSpacer && columns > 0) {
		return columns - 1;
	}
	return columns;
}

void CMuleDataViewCtrl::InitColumnState()
{
	// Every appended column must sit at the view position matching its model
	// id. Several things here index by one and are read as the other:
	// FitColumnsToContent() walks a single index as both, m_columnHidden and
	// the header menu are keyed by view position, and RegisterColumn() by model
	// id. They agree only because the two orders have always coincided, so
	// inserting a column mid-list -- which is what surfaced this -- silently
	// sizes and hides the wrong ones. Assert rather than document it: the
	// failure is invisible at runtime and looks like a rendering bug.
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		wxASSERT_MSG(GetColumn(i)->GetModelColumn() == i,
			"column ids must be declared in the order the columns are appended");
	}

#ifdef __WXOSX__
	// Forgetting the spacer costs the last column: macOS gives the leftover
	// width to the last *resizable* one, which collapses it to nothing as soon
	// as the columns outgrow the control. Nothing about that is visible until
	// somebody narrows the list far enough, which is how the clients list
	// shipped without one -- so fail loudly here instead.
	wxASSERT_MSG(m_hasSpacer, "every list must AppendSpacerColumn() before InitColumnState()");
#endif

	m_columnHidden.resize(RealColumnCount(), false);
	m_lastKnownWidths.clear();
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		m_lastKnownWidths.push_back(GetColumn(i)->GetWidth());
	}
}

bool CMuleDataViewCtrl::IsColumnHidden(int col) const
{
	return (col >= 0) && (static_cast<size_t>(col) < m_columnHidden.size()) && m_columnHidden[col];
}

void CMuleDataViewCtrl::SetColumnHidden(int col, bool hidden, int width)
{
	if (col < 0 || static_cast<unsigned>(col) >= RealColumnCount()) {
		return;
	}
	if (static_cast<size_t>(col) >= m_columnHidden.size()) {
		m_columnHidden.resize(RealColumnCount(), false);
	}

	m_columnHidden[col] = hidden;
	wxDataViewColumn *column = GetColumn(static_cast<unsigned>(col));
	column->SetHidden(hidden);
	if (!hidden && width > COL_SIZE_MIN) {
		column->SetWidth(width);
	}
}

void CMuleDataViewCtrl::UpdateExpanderColumn()
{
	// The expander belongs on the leftmost visible column: hiding the column
	// that currently owns it would take any group triangles with it, and
	// re-showing a column to its left has to take them back.
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		if (!IsColumnHidden(static_cast<int>(i))) {
			wxDataViewColumn *column = GetColumn(i);
			if (GetExpanderColumn() != column) {
				SetExpanderColumn(column);
			}
			return;
		}
	}
}

int CMuleDataViewCtrl::CompareItems(const wxDataViewItem &item1, const wxDataViewItem &item2) const
{
	for (const CColPair &entry : m_sort_orders) {
		const unsigned column = entry.first;
		const unsigned order = entry.second;
		const int modifier = (order & SORT_DES) ? -1 : 1;
		const bool alt = (order & SORT_ALT) != 0;

		const int result = CompareByColumn(item1, item2, column, alt, modifier);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

void CMuleDataViewCtrl::ShowSortCaret(unsigned column, unsigned order, SortTrigger trigger)
{
	if (column >= RealColumnCount()) {
		return;
	}

	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		if (i != column && GetColumn(i)->IsSortKey()) {
			GetColumn(i)->UnsetAsSortKey();
		}
	}

	bool ascending = !(order & SORT_DES);

#ifdef __WXGTK__
	// Two GTK quirks stack up here, and they only cancel each other out on
	// the click path -- which is why the caret used to come back up the wrong
	// way round after a restart, then correct itself the moment a header was
	// clicked.
	//
	// The first is the glyph: with gtk-alternative-sort-arrows off (the
	// default) GTK draws GTK_SORT_ASCENDING as "pan-down-symbolic", so an A-Z
	// sort gets a *down* arrow, the opposite of macOS and MSW.
	//
	// The second is that GTK sorts on its own. wx makes every sortable column
	// call gtk_tree_view_column_set_sort_column_id(), which leaves GTK's own
	// handler connected: on the button *release*, after this control has
	// already handled the button press and set the caret, GTK flips the order
	// it finds and writes it back. Two inversions, so a clicked header ends
	// up looking right.
	//
	// Nothing restores a saved sort, so hand wx the flipped value there and
	// both paths agree. Only the arrow is affected -- rows are ordered from
	// m_sort_orders, and the model deliberately ignores wx's ascending flag.
	if (trigger == SortTrigger::Programmatic) {
		ascending = !ascending;
	}
#else
	wxUnusedVar(trigger);
#endif

	GetColumn(column)->SetSortOrder(ascending);
}

void CMuleDataViewCtrl::ApplySorting(unsigned column, unsigned order, SortTrigger trigger)
{
	// Push to the front: the most recently clicked column is primary, the
	// rest stay as tie-breakers, matching CMuleListCtrl::SetSorting().
	for (CSortingList::iterator it = m_sort_orders.begin(); it != m_sort_orders.end(); ++it) {
		if (it->first == column) {
			m_sort_orders.erase(it);
			break;
		}
	}
	m_sort_orders.emplace_front(column, order);

	ShowSortCaret(column, order, trigger);

	OnSortingChanged();
}

namespace
{
// This control's sort bits and CListColumnStore's are different values for the
// same two ideas, so every crossing converts -- see the note in MuleListCtrl.cpp
// for what passing them through unconverted costs. The numbers currently
// coincide; that is incidental and must not be relied on.
unsigned ToStoreFlags(unsigned order)
{
	return (order & CMuleDataViewCtrl::SORT_DES ? CListColumnStore::SORT_DESCENDING : 0) |
	       (order & CMuleDataViewCtrl::SORT_ALT ? CListColumnStore::SORT_ALTERNATE : 0);
}

unsigned FromStoreFlags(unsigned storeFlags)
{
	return (storeFlags & CListColumnStore::SORT_DESCENDING ? CMuleDataViewCtrl::SORT_DES : 0) |
	       (storeFlags & CListColumnStore::SORT_ALTERNATE ? CMuleDataViewCtrl::SORT_ALT : 0);
}
} // namespace

void CMuleDataViewCtrl::LoadColumnSettings()
{
	if (!m_columnStore.HasTableName()) {
		return;
	}

	CListColumnStore::CSortingList decoded;
	m_hasPersistedWidths = m_columnStore.LoadSettings(m_widthAdapter, GetOldColumnOrder(), decoded);

	// Restored widths can leave the default expander column hidden.
	UpdateExpanderColumn();

	// LoadSettings() returns the orders primary-LAST: CMuleListCtrl applied
	// them by calling SetSorting() on each in turn, and each call pushes to
	// the front, so the last one processed ends up primary. ApplySorting()
	// has the same semantics, so replaying them in order reproduces it.
	// Only replace what the list installed for itself when the config actually
	// has something stored. A profile with no saved TableOrdering for this list
	// decodes to an empty list, and clearing unconditionally would throw away
	// the default the list set before calling here -- CServerListCtrl sorts by
	// name in exactly that way.
	if (!decoded.empty()) {
		m_sort_orders.clear();
		for (const CListColumnStore::CColPair &pair : decoded) {
			ApplySorting(pair.first, FromStoreFlags(pair.second));
		}
	}

	// A list that installs no default of its own (CSearchListCtrl) still has to
	// end up with a sort chain: with none, CompareItems() answers 0 for every
	// pair and nothing is ordered. CMuleListCtrl::LoadSettings() has always
	// guaranteed at least one entry; keep that guarantee here.
	if (m_sort_orders.empty()) {
		ApplySorting(0, 0);
	}
}

void CMuleDataViewCtrl::SaveColumnSettings()
{
	if (!m_columnStore.HasTableName()) {
		return;
	}
	CListColumnStore::CSortingList stored;
	for (const CColPair &pair : m_sort_orders) {
		stored.emplace_back(pair.first, ToStoreFlags(pair.second));
	}

	m_columnStore.SaveSettings(m_widthAdapter, stored);
}

void CMuleDataViewCtrl::OnColumnHeaderClick(wxDataViewEvent &event)
{
	wxDataViewColumn *col = event.GetDataViewColumn();
	if (!col) {
		event.Skip();
		return;
	}
	const unsigned column = static_cast<unsigned>(col->GetModelColumn());

	// Mirrors CMuleListCtrl::OnColumnLClick's cycle: the same column clicked
	// again flips ascending<->descending, and once descending, a further
	// click on an alt-eligible column flips to ascending with the alt
	// criterion toggled instead of clearing the sort.
	unsigned sort_order = 0;
	if (!m_sort_orders.empty() && m_sort_orders.front().first == column) {
		sort_order = m_sort_orders.front().second;
		if (sort_order & SORT_DES) {
			if (AltSortAllowed(column)) {
				sort_order = (~sort_order) & SORT_ALT;
			} else {
				sort_order = 0;
			}
		} else {
			sort_order = SORT_DES | (sort_order & SORT_ALT);
		}
	} else {
		// Clicking back to a column that is still in the chain resumes the
		// direction it was last sorted by, rather than starting over.
		for (const CColPair &entry : m_sort_orders) {
			if (entry.first == column) {
				sort_order = entry.second;
				break;
			}
		}
	}

	ApplySorting(column, sort_order, SortTrigger::HeaderClick);
	SaveColumnSettings();
}

void CMuleDataViewCtrl::OnColumnHeaderRightClick(wxDataViewEvent &event)
{
	// Show/hide menu, as CMuleListCtrl::OnColumnRClick offered on every
	// list built on the old base.
	wxMenu menu;
	// Truncating here hides a column from the menu with no other symptom, so
	// say so rather than let it pass: the range is meant to cover every list.
	const unsigned menuSlots = MP_LISTCOL_20 - MP_LISTCOL_1 + 1;
	wxASSERT_MSG(RealColumnCount() <= menuSlots,
		"more columns than MP_LISTCOL_* slots -- the extra ones cannot be shown or hidden");
	const unsigned columns = std::min<unsigned>(RealColumnCount(), menuSlots);
	for (unsigned i = 0; i < columns; ++i) {
		menu.AppendCheckItem(static_cast<int>(i) + MP_LISTCOL_1, GetColumn(i)->GetTitle());
		menu.Check(static_cast<int>(i) + MP_LISTCOL_1, !IsColumnHidden(static_cast<int>(i)));
	}

	PopupMenu(&menu);
	event.Skip();
}

void CMuleDataViewCtrl::OnColumnMenuSelected(wxCommandEvent &evt)
{
	const int col = evt.GetId() - MP_LISTCOL_1;
	if (col < 0 || static_cast<unsigned>(col) >= RealColumnCount()) {
		return;
	}

	if (!IsColumnHidden(col)) {
		// Remember the width so re-showing restores what the user had.
		m_columnStore.SetCachedWidth(col, GetColumn(static_cast<unsigned>(col))->GetWidth());
		SetColumnHidden(col, true, 0);
	} else {
		const int cached = m_columnStore.GetCachedWidth(col);
		SetColumnHidden(col, false, cached > 0 ? cached : m_columnStore.GetColumnDefaultWidth(col));
	}
	UpdateExpanderColumn();
	OnColumnVisibilityChanged();
	SaveColumnSettings();
}

void CMuleDataViewCtrl::OnIdle(wxIdleEvent &event)
{
	event.Skip();

	// No portable wxDataViewCtrl "column resized" event exists to hook
	// directly (unlike wxListCtrl's EVT_LIST_COL_END_DRAG), so a drag-resize
	// is detected by comparing against the last-seen widths.
	bool changed = false;
	for (int i = 0;
		i < static_cast<int>(RealColumnCount()) && i < static_cast<int>(m_lastKnownWidths.size());
		++i) {
		const int width = GetColumn(i)->GetWidth();
		if (width != m_lastKnownWidths[i]) {
			m_lastKnownWidths[i] = width;
			changed = true;
		}
	}
	if (changed) {
		OnColumnWidthsChanged();
	}

	OnIdleHook();
}

void CMuleDataViewCtrl::OnChar(wxKeyEvent &evt)
{
	// The list gets first refusal on every key. Doing this before the
	// WXK_START test is deliberate: WXK_DELETE is 127, well below it, so a
	// delete-key handler placed after that test would never see it, while
	// WXK_NUMPAD_DELETE (above it) would be skipped to the backend instead.
	if (OnListKey(evt)) {
		return;
	}

	int key = evt.GetKeyCode();
	if (key == 0) {
		// GetKeyCode() returns 0 for characters it can't map; the unicode
		// key is the fallback. GetUnicodeKey() returns wxChar -- a signed
		// char in wx's UTF-8 build but wchar_t in the wide build, so an
		// unsigned-char cast would truncate the wide case.
		// NOLINTNEXTLINE(bugprone-signed-char-misuse)
		key = evt.GetUnicodeKey();
	} else if (key >= WXK_START) {
		// Navigation keys belong to the backend's own cursor handling.
		evt.Skip();
		return;
	}

	if (evt.AltDown() || evt.ControlDown() || evt.MetaDown()) {
		// Cmd/Ctrl+A: only wxGTK's backend selects all by itself, so do it
		// here for all three. A control-modified 'a' arrives as SOH on most
		// ports, but not universally, so accept the letter too.
		const int plain = wxTolower(evt.GetKeyCode());
		if (evt.CmdDown() && (evt.GetKeyCode() == 0x01 || plain == 'a')) {
			SelectAll();
			return;
		}
		evt.Skip();
		return;
	}

	const uint64 now = GetTickCount64();
	if (m_ttsTime + kTypeAheadResetMs < now) {
		m_ttsText.Clear();
	}
	m_ttsTime = now;
	m_ttsText.Append(wxTolower(static_cast<wxChar>(key)));

	wxDataViewItemArray ordered;
	GetDisplayOrder(ordered);
	const size_t count = ordered.GetCount();
	if (count == 0) {
		return;
	}

	// A fresh single keystroke starts one past the current match so that
	// tapping the same letter cycles; further keystrokes refine in place.
	size_t start = 0;
	for (size_t i = 0; i < count; ++i) {
		if (ordered[i].GetID() == m_ttsItem) {
			start = i;
			if (m_ttsText.length() == 1) {
				++start;
			}
			break;
		}
	}

	for (size_t i = 0; i < count; ++i) {
		const wxDataViewItem item = ordered[(start + i) % count];
		if (GetRowLabel(item).Lower().StartsWith(m_ttsText)) {
			m_ttsItem = item.GetID();
			UnselectAll();
			Select(item);
			SetCurrentItem(item);
			EnsureVisible(item);
			return;
		}
	}
	// No match: the accumulated text is kept (so a typo can be corrected by
	// continuing to type) but the selection stays where it is.
}

void CMuleDataViewCtrl::OnKeyDown(wxKeyEvent &evt)
{
#ifdef __WXOSX__
	// wx's Cocoa backend only ever raises wxEVT_DATAVIEW_ITEM_CONTEXT_MENU in
	// response to a physical right-click: there is no keyboard-triggered path
	// and no AXShowMenu implementation for any control on this port, not
	// specific to wxDataViewCtrl (wxWidgets/wxWidgets#13010, open since 2011).
	// VoiceOver's context-menu gesture (VO+Shift+M) performs AXShowMenu, so it
	// never reaches wx here at all -- reported for aMule in
	// amule-org/amule#180. Shift+F10 raises the same event ourselves,
	// sidestepping the missing AXShowMenu entirely. Ctrl+Return was tried
	// first (a menu-key alternative some VoiceOver users prefer to
	// function-row keys) and rejected: NSOutlineView's own keyDown: treats
	// bare Return as "activate the row" before this handler ever sees it --
	// confirmed by it opening a shared file's associated app instead of a
	// menu, Control held or not -- so it can never fire here on this port.
	if (evt.ShiftDown() && evt.GetKeyCode() == WXK_F10) {
		wxDataViewEvent menuEvent(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, this, GetCurrentItem());
		ProcessWindowEvent(menuEvent);
		return;
	}

	// NSOutlineView scrolls the view for these keys without touching either
	// the selection or the cursor, so on this platform they are ours to
	// implement -- shifted, where GTK and MSW extend the selection and the
	// native control does nothing at all, and unshifted too.
	//
	// Unshifted was left to the platform at first, on the grounds that
	// scroll-without-select is the macOS convention. It reads as broken
	// here: the cursor stays where it was, so the next arrow key jumps the
	// view straight back to it, and the page key looks like it did nothing.
	// Moving the cursor with the view is what the other two ports do, and it
	// is what makes the following arrow key continue from what is on screen.
	{
		const bool extend = evt.ShiftDown();
		switch (evt.GetKeyCode()) {
		case WXK_PAGEUP:
			MoveByPage(PageMotion::PageUp, extend);
			return;
		case WXK_PAGEDOWN:
			MoveByPage(PageMotion::PageDown, extend);
			return;
		case WXK_HOME:
			MoveByPage(PageMotion::Home, extend);
			return;
		case WXK_END:
			MoveByPage(PageMotion::End, extend);
			return;
		default:
			break;
		}
	}
#endif
	evt.Skip();
}

#ifdef __WXOSX__
void CMuleDataViewCtrl::MoveByPage(PageMotion motion, bool extend)
{
	wxDataViewItemArray ordered;
	GetDisplayOrder(ordered);
	const size_t rowCount = ordered.GetCount();
	if (rowCount == 0) {
		return;
	}

	const wxDataViewItem currentItem = GetCurrentItem();
	const int last = static_cast<int>(rowCount) - 1;
	const bool forward = (motion == PageMotion::PageDown) || (motion == PageMotion::End);

	int current = forward ? -1 : static_cast<int>(rowCount);
	if (currentItem.IsOk()) {
		const int found = ordered.Index(currentItem);
		if (found != wxNOT_FOUND) {
			current = found;
		}
	}

	int target = 0;
	switch (motion) {
	case PageMotion::Home:
		target = 0;
		break;
	case PageMotion::End:
		target = last;
		break;
	default: {
		// GetCountPerPage() is implemented by the native macOS backend;
		// GetItemRect() is not a substitute, since it returns an empty rect
		// for rows that aren't currently on screen and the resulting height
		// of zero collapses a page to a single row.
		int rows = GetCountPerPage();
		if (rows <= 0) {
			rows = 10;
		} else {
			// Leave one row of overlap, as the other ports scroll.
			rows = std::max(1, rows - 1);
		}
		const int delta = (motion == PageMotion::PageDown) ? rows : -rows;
		target = std::min(last, std::max(0, current + delta));
		break;
	}
	}

	const wxDataViewItem targetItem = ordered[target];

	wxDataViewItemArray selection;
	if (extend) {
		// Grow the existing selection to cover everything between where the
		// cursor was and where it lands, so repeated presses keep extending.
		GetSelections(selection);
		const int from = std::min(current < 0 ? target : current, target);
		const int to = std::max(current > last ? target : current, target);
		for (int i = std::max(0, from); i <= std::min(last, to); ++i) {
			if (selection.Index(ordered[i]) == wxNOT_FOUND) {
				selection.Add(ordered[i]);
			}
		}
	} else {
		// Unshifted: the row landed on becomes the selection, as an arrow
		// key would leave it.
		selection.Add(targetItem);
	}

	SetSelections(selection);
	// The cursor, not just the selection: it is what the arrow keys move
	// from next, and leaving it behind is what made an unshifted page key
	// look like it had done nothing as soon as one was pressed.
	SetCurrentItem(targetItem);
	EnsureVisible(targetItem);
}
#endif // __WXOSX__
