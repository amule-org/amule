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

#include <wx/dcclient.h>   // Needed for wxClientDC
#include <wx/menu.h>       // Needed for wxMenu (header show/hide menu)
#include <wx/scopeguard.h> // Needed for wxON_BLOCK_EXIT_SET

#include <algorithm> // Needed for std::find, std::sort, std::min, std::max
#include <vector>    // Needed for std::vector

#include <common/MenuIDs.h> // Needed for MP_LISTCOL_1 .. MP_LISTCOL_25

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
	EVT_MENU_RANGE(MP_LISTCOL_1, MP_LISTCOL_25, CMuleDataViewCtrl::OnColumnMenuSelected)
	EVT_DPI_CHANGED(CMuleDataViewCtrl::OnDPIChanged)
	EVT_IDLE(CMuleDataViewCtrl::OnIdle)
	EVT_CHAR(CMuleDataViewCtrl::OnChar)
	EVT_KEY_DOWN(CMuleDataViewCtrl::OnKeyDown)
	EVT_CONTEXT_MENU(CMuleDataViewCtrl::OnContextMenuKey)
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
	// amuleDlg sets wxIdleEvent::SetMode(wxIDLE_PROCESS_SPECIFIED), so only windows carrying
	// this style are sent idle events at all -- without it OnIdle() never runs.
	SetExtraStyle(GetExtraStyle() | wxWS_EX_PROCESS_IDLE);
}

CMuleDataViewCtrl::~CMuleDataViewCtrl() = default;

double CMuleDataViewCtrl::ColumnWidthAdapter::GetWidthScale() const
{
#ifdef wxHAS_DPI_INDEPENDENT_PIXELS
	// GTK3 and macOS column widths already use logical coordinates.
	return 1.0;
#else
	return m_ctrl->GetDPIScaleFactor();
#endif
}

int CMuleDataViewCtrl::ColumnWidthAdapter::GetColumnWidth(int col) const
{
	if (m_ctrl->IsColumnHidden(col)) {
		return 0;
	}
	const int width = m_ctrl->m_useCachedColumnWidths ? 0 : m_ctrl->GetColumn(col)->GetWidth();
	if (width > 0) {
		return width;
	}
	// A visible column should never report zero -- the spacer appended on macOS exists so the
	// trailing-column sizing lands there instead. This is the backstop: CListColumnStore reads
	// a width <= 0 as hidden and persists it negative, so a stray zero would hide a real column
	// on the next launch, then do the same to whichever column became last, losing one per
	// restart.
	const int cached = m_ctrl->m_columnStore.GetCachedWidth(col);
	return (cached > 0) ? cached : m_ctrl->m_columnStore.GetColumnDefaultWidth(col);
}

void CMuleDataViewCtrl::AppendSpacerColumn(unsigned modelColumn)
{
#ifdef __WXOSX__
	// Resizable is load-bearing: macOS hands the leftover space to the last *resizable* column,
	// so a fixed spacer cannot shrink and the collapse falls through to the last real column
	// instead.
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

int CMuleDataViewCtrl::GetDefaultColumnWidth(const wxString &label, int width) const
{
	// Preserve wx's special auto-size values. Saved pixel widths are restored later.
	if (width < 0) {
		return width;
	}
	return std::max(FromDIP(width), GetTextExtent(label).x + FromDIP(28));
}

void CMuleDataViewCtrl::AddTextColumn(const wxString &label,
	unsigned modelColumn,
	const wxString &key,
	int width,
	wxAlignment align,
	int flags,
	wxDataViewCellMode mode)
{
	width = GetDefaultColumnWidth(label, width);
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
	width = GetDefaultColumnWidth(label, width);
	// CMuleIconTextRenderer rather than wx's, which reserves the icon slot on
	// every row whether or not that row has an icon -- see the class comment.
	CMuleIconTextRenderer *renderer = new CMuleIconTextRenderer();
	renderer->SetMode(mode);
	// Supply the vertical half of the alignment when the caller has not. wx's own text renderer
	// resolves a missing vertical component to centred, but a custom renderer gets exactly what
	// it is given, so a plain wxALIGN_LEFT top-aligns the text while the icon beside it is
	// centred (#867). wxALIGN_TOP is 0 and so indistinguishable from "unspecified", which is
	// why this tests the two bits that do carry intent.
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
	width = GetDefaultColumnWidth(label, width);
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

unsigned CMuleDataViewCtrl::VisibleColumnCount() const
{
	unsigned visible = 0;
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		if (!IsColumnHidden(static_cast<int>(i))) {
			++visible;
		}
	}
	return visible;
}

void CMuleDataViewCtrl::InitColumnState()
{
	// Every appended column must sit at the view position matching its model id. Several things
	// here index by one and are read as the other: FitColumnsToContent() walks a single index
	// as both, m_columnHidden and the header menu are keyed by view position, and
	// RegisterColumn() by model id. They agree only because the two orders have always
	// coincided, so inserting a column mid-list silently sizes and hides the wrong ones.
	// Asserted rather than documented: the failure looks like a rendering bug at runtime.
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		wxASSERT_MSG(GetColumn(i)->GetModelColumn() == i,
			"column ids must be declared in the order the columns are appended");
	}

#ifdef __WXOSX__
	// Forgetting the spacer costs the last column: macOS gives the leftover width to the last
	// *resizable* one, which collapses to nothing as soon as the columns outgrow the control,
	// and nothing shows until somebody narrows the list far enough.
	wxASSERT_MSG(m_hasSpacer, "every list must AppendSpacerColumn() before InitColumnState()");
#endif

	m_columnHidden.resize(RealColumnCount(), false);
	// Old profiles may already have every column hidden. Recover a useful default
	// column before the header becomes the only way to repair the layout.
	if (VisibleColumnCount() == 0 && RealColumnCount() > 0) {
		unsigned first = 0;
		for (unsigned i = 0; i < m_defaultColumnHidden.size(); ++i) {
			if (!m_defaultColumnHidden[i]) {
				first = i;
				break;
			}
		}
		SetColumnHidden(static_cast<int>(first),
			false,
			m_columnStore.GetColumnDefaultWidth(static_cast<int>(first)));
		UpdateExpanderColumn();
	}
	ResetKnownWidths();
}

void CMuleDataViewCtrl::ResetKnownWidths()
{
	m_lastKnownWidths.clear();
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		m_lastKnownWidths.push_back(GetColumn(i)->GetWidth());
	}
	// A programmatic change is not a drag in progress, so nothing is pending either --
	// otherwise the next quiet idle would report a resize the user never made.
	m_widthsSettling = false;
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
	// The expander belongs on the leftmost visible column: hiding the column that currently
	// owns it would take any group triangles with it, and re-showing a column to its left has
	// to take them back.
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

void CMuleDataViewCtrl::EnableCellToolTips()
{
	// Generic data views receive mouse events on their inner client window.
	wxWindow *body = GetMainWindow();
	body->Bind(wxEVT_MOTION, [this](wxMouseEvent &event) {
		m_toolTipPos = event.GetPosition();
		UpdateCellToolTip(event.GetPosition());
		event.Skip();
	});
	const auto hide = [this, body](wxMouseEvent &event) {
		m_toolTipPos.reset();
		body->UnsetToolTip();
		event.Skip();
	};
	body->Bind(wxEVT_LEAVE_WINDOW, hide);
	body->Bind(wxEVT_MOUSEWHEEL, hide);
}

void CMuleDataViewCtrl::RecheckCellToolTip()
{
	// Only a tooltip already set can be wrong; a cell without one gets one on the next move.
	// The same text for the new row leaves the shown tooltip alone.
	if (m_toolTipPos && GetMainWindow()->GetToolTip() != nullptr) {
		UpdateCellToolTip(*m_toolTipPos);
	}
}

void CMuleDataViewCtrl::UpdateCellToolTip(const wxPoint &pos)
{
	wxWindow *body = GetMainWindow();
	wxDataViewItem item;
	wxDataViewColumn *column = nullptr;
	HitTest(ScreenToClient(body->ClientToScreen(pos)), item, column);
	const wxString tip = item.IsOk() && column != nullptr ? GetCellToolTip(item, column->GetModelColumn())
							      : wxString();
	if (tip != body->GetToolTipText()) {
		if (tip.empty()) {
			body->UnsetToolTip();
		} else {
			body->SetToolTip(tip);
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
	// Two GTK quirks stack up here, and they cancel out only on the click path -- which is why
	// the caret used to come back up the wrong way round after a restart and correct itself on
	// the first header click.
	//
	// The first is the glyph: with gtk-alternative-sort-arrows off (the default) GTK draws
	// GTK_SORT_ASCENDING as "pan-down-symbolic", so an A-Z sort gets a DOWN arrow, the opposite
	// of macOS and MSW.
	//
	// The second is that GTK sorts on its own: wx makes every sortable column call
	// gtk_tree_view_column_set_sort_column_id(), which leaves GTK's handler connected, so on
	// the button release -- after this control has handled the press and set the caret -- GTK
	// flips the order it finds and writes it back. Two inversions, so a clicked header ends up
	// looking right.
	//
	// Nothing restores a saved sort, so hand wx the flipped value there and both paths agree.
	// Only the arrow is affected: rows are ordered from m_sort_orders.
	if (trigger == SortTrigger::Programmatic) {
		ascending = !ascending;
	}
#else
	wxUnusedVar(trigger);
#endif

	GetColumn(column)->SetSortOrder(ascending);
}

void CMuleDataViewCtrl::SuspendHeaderSort()
{
	if (m_headerSortSuspended) {
		return;
	}
	m_headerSortSuspended = true;
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		if (GetColumn(i)->IsSortKey()) {
			GetColumn(i)->UnsetAsSortKey();
		}
	}
}

void CMuleDataViewCtrl::RestoreHeaderSort()
{
	if (!m_headerSortSuspended) {
		return;
	}
	m_headerSortSuspended = false;
	if (!m_sort_orders.empty()) {
		const CColPair &primary = m_sort_orders.front();
		ShowSortCaret(primary.first, primary.second);
	}
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
// This control's sort bits and CListColumnStore's are different values for the same two ideas, so
// every crossing converts -- see the note in MuleListCtrl.cpp for what passing them through
// unconverted costs. The numbers currently coincide; that is incidental and must not be relied on.
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
	CaptureDefaultColumnState();
	if (!m_columnStore.HasTableName()) {
		return;
	}

	CListColumnStore::CSortingList decoded;
	m_hasPersistedWidths = m_columnStore.LoadSettings(m_widthAdapter, GetOldColumnOrder(), decoded);

	// Restored widths can leave the default expander column hidden.
	UpdateExpanderColumn();

	// LoadSettings() returns the orders primary-LAST, because each SetSorting() call pushes to
	// the front, so the last one processed ends up primary; ApplySorting() has the same
	// semantics, so replaying in order reproduces it. Only replace what the list installed for
	// itself when the config has something stored: a profile with no saved TableOrdering
	// decodes to an empty list, and clearing unconditionally would throw away the list's own
	// default (as CServerListCtrl's name sort).
	if (!decoded.empty()) {
		m_sort_orders.clear();
		for (const CListColumnStore::CColPair &pair : decoded) {
			ApplySorting(pair.first, FromStoreFlags(pair.second));
		}
	}

	// A list that installs no default of its own (CSearchListCtrl) still has to end up with a
	// sort chain: with none, CompareItems() answers 0 for every pair and nothing is ordered.
	// CMuleListCtrl::LoadSettings() guaranteed at least one entry; keep that guarantee.
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

	// Mirrors CMuleListCtrl::OnColumnLClick's cycle: the same column clicked again flips
	// ascending<->descending, and once descending, a further click on an alt-eligible column
	// flips to ascending with the alt criterion toggled instead of clearing the sort.
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
	ShowColumnMenu(event.GetDataViewColumn());
	event.Skip();
}

void CMuleDataViewCtrl::CaptureDefaultColumnState()
{
	if (!m_defaultColumnHidden.empty()) {
		return;
	}
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		m_defaultColumnHidden.push_back(IsColumnHidden(static_cast<int>(i)));
	}
}

void CMuleDataViewCtrl::RestoreColumnLayout(bool defaults)
{
	Freeze();
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		const int col = static_cast<int>(i);
		const bool hidden = defaults && i < m_defaultColumnHidden.size() && m_defaultColumnHidden[i];
		const int current = IsColumnHidden(col) ? m_columnStore.GetCachedWidth(col)
							: m_widthAdapter.GetColumnWidth(col);
		const int width = defaults || current <= COL_SIZE_MIN
					  ? m_columnStore.GetColumnDefaultWidth(col)
					  : current;
		m_columnStore.SetCachedWidth(col, width);
		SetColumnHidden(col, hidden, width);
	}
	UpdateExpanderColumn();
	Thaw();
	ResetKnownWidths();
	// GTK may still report the pre-reset native widths until the next paint.
	// Both mirroring and persistence must use the widths just requested.
	wxON_BLOCK_EXIT_SET(m_useCachedColumnWidths, m_useCachedColumnWidths);
	m_useCachedColumnWidths = true;
	// The visibility hook mirrors the complete layout, including widths.
	OnColumnVisibilityChanged();
	SaveColumnSettings();
	Refresh();
}

void CMuleDataViewCtrl::CopyColumnLayoutFrom(const CMuleDataViewCtrl &source)
{
	wxCHECK_RET(RealColumnCount() == source.RealColumnCount(), "incompatible table layouts");
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		const int col = static_cast<int>(i);
		const bool hidden = source.IsColumnHidden(col);
		int width = hidden ? source.m_columnStore.GetCachedWidth(col)
				   : source.m_widthAdapter.GetColumnWidth(col);
		if (width <= COL_SIZE_MIN) {
			width = source.m_columnStore.GetColumnDefaultWidth(col);
		}
		// A hidden native column can still report its old, pre-reset width.
		// Transfer the cache as well, so showing it in another tab restores the
		// latest choice rather than whichever width that tab last remembered.
		m_columnStore.SetCachedWidth(col, width);
		if (IsColumnHidden(col) != hidden || (!hidden && GetColumn(i)->GetWidth() != width)) {
			SetColumnHidden(col, hidden, width);
		}
	}
	UpdateExpanderColumn();
	// GTK can clamp widths differently between tabs. Do not have the idle poll
	// mistake a copied layout for a drag and mirror it back indefinitely.
	ResetKnownWidths();
}

void CMuleDataViewCtrl::ShowColumnMenu(wxDataViewColumn *clicked)
{
	// Show/hide menu, as CMuleListCtrl::OnColumnRClick offered on every
	// list built on the old base.
	wxMenu menu;
	// Truncating here hides a column from the menu with no other symptom, so
	// say so rather than let it pass: the range is meant to cover every list.
	const unsigned menuSlots = MP_LISTCOL_25 - MP_LISTCOL_1 + 1;
	wxASSERT_MSG(RealColumnCount() <= menuSlots,
		"more columns than MP_LISTCOL_* slots -- the extra ones cannot be shown or hidden");
	const unsigned columns = std::min<unsigned>(RealColumnCount(), menuSlots);
	const unsigned visible = VisibleColumnCount();
	for (unsigned i = 0; i < columns; ++i) {
		menu.AppendCheckItem(static_cast<int>(i) + MP_LISTCOL_1, GetColumn(i)->GetTitle());
		menu.Check(static_cast<int>(i) + MP_LISTCOL_1, !IsColumnHidden(static_cast<int>(i)));
		menu.Enable(static_cast<int>(i) + MP_LISTCOL_1,
			visible > 1 || IsColumnHidden(static_cast<int>(i)));
	}

	menu.AppendSeparator();
	const wxWindowIDRef fitColumn = wxWindow::NewControlId();
	if (clicked && clicked->GetModelColumn() < RealColumnCount()) {
		menu.Append(fitColumn, _("Fit column to contents"));
		menu.Bind(
			wxEVT_MENU,
			[this, clicked](wxCommandEvent &) {
				FitColumnWidths(static_cast<int>(clicked->GetModelColumn()));
			},
			fitColumn);
	}
	const wxWindowIDRef fitAll = wxWindow::NewControlId();
	menu.Append(fitAll, _("Fit visible columns to contents"));
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { FitColumnWidths(-1); }, fitAll);

	menu.AppendSeparator();
	const wxWindowIDRef showAll = wxWindow::NewControlId();
	menu.Append(showAll, _("Show all columns (keep widths)"));
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { RestoreColumnLayout(false); }, showAll);
	const wxWindowIDRef restore = wxWindow::NewControlId();
	menu.Append(restore, _("Restore default columns and widths"));
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { RestoreColumnLayout(true); }, restore);
	// Keyboard invocation must stay at the focused table even if the pointer
	// is over another window. Header clicks retain the pointer position.
	PopupMenu(&menu, clicked ? wxDefaultPosition : wxPoint(0, 0));
}

void CMuleDataViewCtrl::FitColumnWidths(int column)
{
	wxDataViewModel *model = GetModel();
	if (!model) {
		return;
	}
	wxDataViewItemArray rows;
	GetColumnFitRows(rows, 512);
	const size_t samples = rows.size();
	wxClientDC dc(this);
	dc.SetFont(GetFont());
	const int padding = FromDIP(16);
	const int contentLimit = std::max(FromDIP(160), std::min(FromDIP(600), GetClientSize().x * 2 / 3));

	Freeze();
	for (unsigned i = 0; i < RealColumnCount(); ++i) {
		const int columnIndex = static_cast<int>(i);
		if ((column >= 0 && columnIndex != column) || IsColumnHidden(columnIndex)) {
			continue;
		}
		wxDataViewColumn *col = GetColumn(i);
		const int headerWidth = GetTextExtent(col->GetTitle()).x + FromDIP(28);
		int width = rows.empty()
				    ? std::max(headerWidth, m_columnStore.GetColumnDefaultWidth(columnIndex))
				    : headerWidth;
		// Genuine bars keep a useful default. Composite cells using the same
		// variant may supply a measured width through GetCustomFitWidth().
		if (dynamic_cast<CMuleBarRenderer *>(col->GetRenderer())) {
			width = std::max(width, m_columnStore.GetColumnDefaultWidth(columnIndex));
		}
		for (size_t row = 0; row < samples; ++row) {
			if (!model->HasValue(rows[row], col->GetModelColumn())) {
				continue;
			}
			wxDataViewItemAttr attr;
			dc.SetFont(model->GetAttr(rows[row], col->GetModelColumn(), attr)
					   ? attr.GetEffectiveFont(GetFont())
					   : GetFont());
			const int custom = GetCustomFitWidth(rows[row], col->GetModelColumn(), dc);
			if (custom >= 0) {
				width = std::max(width, std::min(custom + padding, contentLimit));
				continue;
			}
			if (dynamic_cast<CMuleBarRenderer *>(col->GetRenderer())) {
				continue;
			}
			wxVariant value;
			model->GetValue(value, rows[row], col->GetModelColumn());
			wxString text;
			int iconWidth = 0;
			if (value.GetType() == "string") {
				text = value.GetString();
			} else if (value.GetType() == "wxDataViewIconText") {
				wxDataViewIconText cell;
				cell << value;
				text = cell.GetText();
				if (cell.GetIcon().IsOk()) {
					iconWidth = cell.GetIcon().GetWidth() + FromDIP(4);
				}
			}

			int cellWidth = dc.GetTextExtent(text).x + iconWidth + padding;
			if (col == GetExpanderColumn() && !model->IsListModel()) {
				cellWidth += FromDIP(20);
				for (wxDataViewItem parent = model->GetParent(rows[row]); parent.IsOk();
					parent = model->GetParent(parent)) {
					cellWidth += std::max(GetIndent(), FromDIP(16));
				}
			}
			width = std::max(width, std::min(cellWidth, contentLimit));
		}
		col->SetWidth(width);
	}
	Thaw();
	ResetKnownWidths();
	// Use the normal notification so search tabs receive the new layout too.
	OnColumnWidthsChanged();
	Refresh();
}

void CMuleDataViewCtrl::OnColumnMenuSelected(wxCommandEvent &evt)
{
	const int col = evt.GetId() - MP_LISTCOL_1;
	if (col < 0 || static_cast<unsigned>(col) >= RealColumnCount()) {
		return;
	}

	if (!IsColumnHidden(col)) {
		if (VisibleColumnCount() <= 1) {
			return;
		}
		// Remember the width so re-showing restores what the user had.
		m_columnStore.SetCachedWidth(col, m_widthAdapter.GetColumnWidth(col));
		SetColumnHidden(col, true, 0);
	} else {
		const int cached = m_columnStore.GetCachedWidth(col);
		SetColumnHidden(col, false, cached > 0 ? cached : m_columnStore.GetColumnDefaultWidth(col));
	}
	UpdateExpanderColumn();
	ResetKnownWidths();
	OnColumnVisibilityChanged();
	SaveColumnSettings();
}

void CMuleDataViewCtrl::OnDPIChanged(wxDPIChangedEvent &event)
{
#ifndef wxHAS_DPI_INDEPENDENT_PIXELS
	// wx rescales the native columns, but hidden columns restore their widths from this
	// cache. Keep every cached width in the same units before idle can persist the layout.
	for (int col = 0; col < static_cast<int>(RealColumnCount()); ++col) {
		const int width = m_columnStore.GetCachedWidth(col);
		if (width > 0) {
			m_columnStore.SetCachedWidth(col, event.ScaleX(width));
		}
	}
#endif
	event.Skip();
}

void CMuleDataViewCtrl::OnIdle(wxIdleEvent &event)
{
	event.Skip();

	// No portable wxDataViewCtrl "column resized" event exists to hook directly (unlike
	// wxListCtrl's EVT_LIST_COL_END_DRAG), so a drag-resize is detected by comparing against
	// the last-seen widths.
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

	// Act when the drag ends, not on every step. Idle fires as long as the mouse moves, so a
	// change on this tick means the pointer is still dragging and the width that matters is the
	// one it stops at. Firing per observed change ran a wxConfig write, and for a search list a
	// full column-sync across every other open tab, for each pixel of travel (issue #1022).
	if (changed) {
		m_widthsSettling = true;
	} else if (m_widthsSettling) {
		m_widthsSettling = false;
		OnColumnWidthsChanged();
	}

	RecheckCellToolTip();
	OnIdleHook();
}

void CMuleDataViewCtrl::OnChar(wxKeyEvent &evt)
{
	// The list gets first refusal on every key. Before the WXK_START test on purpose:
	// WXK_DELETE is 127, well below it, so a delete-key handler after that test would never see
	// it, while WXK_NUMPAD_DELETE would be skipped to the backend instead.
	if (OnListKey(evt)) {
		return;
	}

	int key = evt.GetKeyCode();
	if (key == 0) {
		// GetKeyCode() returns 0 for characters it cannot map; the unicode key is the fallback.
		// GetUnicodeKey() returns wxChar -- a signed char in wx's UTF-8 build but wchar_t in the
		// wide build, so an unsigned-char cast would truncate the wide case.
		// NOLINTNEXTLINE(bugprone-signed-char-misuse)
		key = evt.GetUnicodeKey();
	} else if (key >= WXK_START) {
		// Navigation keys belong to the backend's own cursor handling.
		evt.Skip();
		return;
	}

	if (evt.AltDown() || evt.ControlDown() || evt.MetaDown()) {
		// Cmd/Ctrl+A: only wxGTK's backend selects all by itself, so do it here for all
		// three. A control-modified 'a' arrives as SOH on most ports, but not universally,
		// so accept the letter too.
		const int plain = wxTolower(evt.GetKeyCode());
		if (evt.CmdDown() && (evt.GetKeyCode() == 0x01 || plain == 'a')) {
			SelectAll();
			return;
		}
		evt.Skip();
		return;
	}

	// Control keys are commands, not type-ahead text: the backend turns Return into item
	// activation and Tab into focus navigation.
	if (key < WXK_SPACE || key == WXK_DELETE) {
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

void CMuleDataViewCtrl::OnContextMenuKey(wxContextMenuEvent &evt)
{
	if (evt.GetPosition() != wxDefaultPosition) {
		// A real right-click is turned into wxEVT_DATAVIEW_ITEM_CONTEXT_MENU by every port;
		// this handler exists for the keyboard-origin case (Shift+F10 / the Applications
		// key on MSW and GTK, both of which fire this separate, dataview-agnostic event
		// instead).
		//
		// Skipping is load-bearing on macOS, where that port raises the dataview event FROM
		// this one in wxDataViewCtrl::OnContextMenu(). Our handler runs first, so
		// swallowing a mouse-origin event would leave right-click with no context menu at
		// all.
		evt.Skip();
		return;
	}

	RaiseItemContextMenu();
}

void CMuleDataViewCtrl::RaiseItemContextMenu()
{
	const wxDataViewItem item = GetCurrentItem();
	if (item.IsOk()) {
		// GetItemRect() answers an empty rect for a row that is not on screen, and the menu
		// would then open in the control's top-left corner rather than at the row it acts
		// on -- reachable by clicking a row and scrolling away before pressing the key.
		// Bringing it into view also shows what the menu applies to, and does not scroll
		// when the row is already visible.
		EnsureVisible(item);
	}

	wxDataViewEvent menuEvent(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, this, item);
	const wxRect rect = item.IsOk() ? GetItemRect(item) : wxRect(GetClientSize());
	menuEvent.SetPosition(rect.GetLeft(), rect.GetBottom());
	ProcessWindowEvent(menuEvent);
}

void CMuleDataViewCtrl::OnKeyDown(wxKeyEvent &evt)
{
	// A focused list can open its column settings even with an empty header or
	// no rows. Ctrl+Shift+L (Command+Shift+L on macOS) leaves row menus unchanged.
	if (evt.CmdDown() && evt.ShiftDown() && !evt.AltDown() && evt.GetKeyCode() == 'L') {
		ShowColumnMenu();
		return;
	}
#ifdef __WXOSX__
	// wx's Cocoa backend has no keyboard-triggered path to wxEVT_DATAVIEW_ITEM_CONTEXT_MENU. It
	// does handle wxEVT_CONTEXT_MENU -- that is how a right-click becomes a dataview event
	// there -- but nothing on this port raises that event from a keystroke, and there is no
	// AXShowMenu implementation for any control on it (wxWidgets/wxWidgets#13010, open since
	// 2011), so VoiceOver's VO+Shift+M never reaches wx either. Ctrl+Return was tried and
	// rejected: NSOutlineView's keyDown: treats bare Return as "activate the row" before this
	// handler sees it, Control held or not.
	if (evt.ShiftDown() && evt.GetKeyCode() == WXK_F10) {
		RaiseItemContextMenu();
		return;
	}
	// Control+M too: easier to find by touch than F10. The physical Control key, since wx's
	// "Ctrl" is Command here and Command+M minimizes; no Option, which VoiceOver's VO+M uses.
	if (evt.GetModifiers() == wxMOD_RAW_CONTROL && evt.GetKeyCode() == 'M') {
		RaiseItemContextMenu();
		return;
	}

	// NSOutlineView scrolls the view for these keys without touching the selection or the
	// cursor, so on this platform they are ours to implement -- shifted, where GTK and MSW
	// extend the selection, and unshifted too.
	//
	// Unshifted was left to the platform at first, scroll-without-select being the macOS
	// convention, but it reads as broken: the cursor stays put, so the next arrow key jumps the
	// view straight back and the page key looks like it did nothing. Moving the cursor with the
	// view is what the other two ports do.
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
		// GetCountPerPage() is implemented by the native macOS backend. GetItemRect() is
		// not a substitute: it returns an empty rect for rows not on screen, and the
		// resulting zero height collapses a page to a single row.
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
	// The cursor, not just the selection: it is what the arrow keys move from next, and leaving
	// it behind is what made an unshifted page key look like it had done nothing as soon as one
	// was pressed.
	SetCurrentItem(targetItem);
	EnsureVisible(targetItem);
}
#endif // __WXOSX__
