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

#ifndef AMULE_PREFSSIDEBAR_H
#define AMULE_PREFSSIDEBAR_H

#include <wx/bmpbndl.h>
#include <wx/dc.h>
#if wxUSE_GRAPHICS_CONTEXT
#include <wx/graphics.h>
#endif
#include <wx/settings.h>
#ifdef __WXMSW__
#include <wx/control.h>     // wxControl::Ellipsize
#include <wx/msw/wrapwin.h> // SystemParametersInfo, for high contrast
#include <wx/utils.h>       // wxGetMousePosition
#include <wx/vlbox.h>
#if wxUSE_ACCESSIBILITY
#include <wx/access.h>
#endif
#else
#include <wx/dataview.h>
#include <wx/wupdlock.h>
#endif

#include "MuleColour.h" // MixColours

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

// Corner radius, in DIPs, of the selected sidebar row and of the search box above it, both drawn
// by aMule on Windows. Windows 11 rounds list items and text boxes by 4 px, macOS its sidebar
// selection by about 6 points, and KDE's Breeze its controls by 3 to 5 px: 5 sits between them.
constexpr int kPrefsCornerRadius = 5;
//! Size, in DIPs, of the page icons in the sidebar.
constexpr int kPrefsIconSize = 20;

// Drawing for the rounded highlights, with colours mixed from the system's so they follow light
// and dark mode.
namespace PrefsHighlight
{
//! Whether the system asks for high contrast. The highlights are tints of the system colours,
//! which a high-contrast theme leaves too faint to see; they use the plain colours instead.
inline bool HighContrast()
{
#ifdef __WXMSW__
	HIGHCONTRAST highContrast = { sizeof(HIGHCONTRAST), 0, nullptr };
	return ::SystemParametersInfo(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
	       (highContrast.dwFlags & HCF_HIGHCONTRASTON);
#else
	return false;
#endif
}

//! Fills @p rect with a rounded rectangle, anti-aliased where the platform can, outlined inside
//! the rectangle in @p border when @p borderWidth is positive.
inline void FillRounded(wxDC &dc,
	const wxRect &rect,
	int radius,
	const wxColour &fill,
	const wxColour &border = wxNullColour,
	int borderWidth = 0)
{
	const bool outlined = borderWidth > 0 && border.IsOk();
#if wxUSE_GRAPHICS_CONTEXT
	const std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
	if (gc) {
		// Exact coordinates: the outline sits half its width inside the rectangle, where it covers
		// whole pixels. The context's own half-pixel shift for odd widths would move it onto pixel
		// edges, blurring it and pushing part of it outside the rectangle.
		gc->DisableOffset();
		gc->SetBrush(wxBrush(fill));
		if (outlined) {
			gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(border).Width(borderWidth)));
		} else {
			gc->SetPen(*wxTRANSPARENT_PEN);
		}
		const double inset = outlined ? borderWidth / 2.0 : 0;
		gc->DrawRoundedRectangle(rect.x + inset,
			rect.y + inset,
			rect.width - 2 * inset,
			rect.height - 2 * inset,
			radius);
		return;
	}
#endif
	dc.SetBrush(wxBrush(fill));
	dc.SetPen(outlined ? wxPen(border, borderWidth) : *wxTRANSPARENT_PEN);
	dc.DrawRoundedRectangle(rect, radius);
}
} // namespace PrefsHighlight

// The preferences dialog's page list, an icon and a title per row. Both versions below share an
// interface: SetRows(), PageAt(), SelectRow() and FitWidth(), and choosing a row with the mouse,
// the keyboard or assistive technology sends wxEVT_LISTBOX with the row as its selection.
#ifdef __WXMSW__

//! Windows: drawn by aMule, the selected row as a rounded highlight. wx's list draws a square
//! selection there and cannot be asked for anything else; it is wx's own generic list on Windows,
//! so drawing one here costs nothing native, and Accessible below keeps the rows readable to
//! screen readers. SelectRow() sends no wxEVT_LISTBOX.
class PrefsSidebar : public wxVListBox
{
public:
	struct Row
	{
		wxString label;
		wxBitmapBundle icon;
		unsigned page; //!< The `pages[]` index the row stands for.
	};

	//! Space, in DIPs, to leave around the list. Its highlights are inset by kRowInset more, which
	//! lines them up with the search field above.
	static constexpr int kMargin = 2;

	PrefsSidebar(wxWindow *parent, wxWindowID id)
	: wxVListBox(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
	{
		UpdateRowHeight();
		Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent &event) {
			UpdateRowHeight();
			event.Skip();
		});
		Bind(wxEVT_KEY_DOWN, &PrefsSidebar::OnKeyDown, this);
		Bind(wxEVT_MOUSEWHEEL, &PrefsSidebar::OnMouseWheel, this);
		Bind(wxEVT_MOTION, [this](wxMouseEvent &event) {
			SetHoverAt(event.GetPosition());
			event.Skip();
		});
		Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) {
			SetHover(wxNOT_FOUND);
			event.Skip();
		});
		// Scrolling moves the rows under a pointer that stays put.
		for (const auto &type : { wxEVT_SCROLLWIN_TOP,
			     wxEVT_SCROLLWIN_BOTTOM,
			     wxEVT_SCROLLWIN_LINEUP,
			     wxEVT_SCROLLWIN_LINEDOWN,
			     wxEVT_SCROLLWIN_PAGEUP,
			     wxEVT_SCROLLWIN_PAGEDOWN,
			     wxEVT_SCROLLWIN_THUMBTRACK,
			     wxEVT_SCROLLWIN_THUMBRELEASE }) {
			Bind(type, [this](wxScrollWinEvent &event) {
				event.Skip();
				CallAfter([this] { SetHoverAt(ScreenToClient(wxGetMousePosition())); });
			});
		}
#if wxUSE_ACCESSIBILITY
		Bind(wxEVT_LISTBOX, [this](wxCommandEvent &event) {
			NotifySelection();
			event.Skip();
		});
		// wxVListBox repaints the selected row itself; screen readers are told which row has the
		// focus once a click that gave it has also changed the selection.
		Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &event) {
			CallAfter([this] { NotifyFocus(); });
			event.Skip();
		});
		new Accessible(this); // Owned by the window, which it registers itself with.
#endif
	}

	//! Replaces the rows, leaving none selected: the selected position would now be another page,
	//! and wxVListBox keeps it, and the scroll position, across a change of rows.
	void SetRows(std::vector<Row> rows)
	{
		SetSelection(wxNOT_FOUND);
		m_rows = std::move(rows);
		m_hover = wxNOT_FOUND;
		SetItemCount(m_rows.size());
		Refresh();
#if wxUSE_ACCESSIBILITY
		wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_REORDER, this, wxOBJID_CLIENT, wxACC_SELF);
#endif
	}

	unsigned PageAt(size_t row) const { return m_rows[row].page; }

	//! Selects @p row and scrolls it into view, without sending wxEVT_LISTBOX.
	void SelectRow(int row)
	{
		if (row != GetSelection()) {
			SetSelection(row);
			NotifySelection();
		}
	}

	//! Sizes the list to show @p labels in full beside their icons: the same sums as OnDrawItem(),
	//! so the widest title fits to the pixel.
	void FitWidth(const std::vector<wxString> &labels)
	{
		int text = 0;
		for (const wxString &label : labels) {
			text = std::max(text, GetTextExtent(label).GetWidth());
		}
		// Plus a vertical scroller: a short dialog needs one, and it narrows every row.
		const int width = text + 2 * FromDIP(kRowInset) + 2 * FromDIP(kPadding) +
				  FromDIP(kPrefsIconSize + kIconGap) +
				  wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this);
		SetMinSize(wxSize(width, -1));
		SetMaxSize(wxSize(width, -1));
	}

	//! An empty list (a search with no matches) has nothing to focus.
	bool AcceptsFocus() const override { return !m_rows.empty() && wxVListBox::AcceptsFocus(); }

	bool SetFont(const wxFont &font) override
	{
		if (!wxVListBox::SetFont(font)) {
			return false;
		}
		UpdateRowHeight();
		return true;
	}

protected:
	wxCoord OnMeasureItem(size_t WXUNUSED(n)) const override { return m_rowHeight; }

	void OnDrawBackground(wxDC &dc, const wxRect &rect, size_t n) const override
	{
		const bool selected = IsSelected(n);
		if (!selected && static_cast<int>(n) != m_hover) {
			return;
		}
		const int radius = FromDIP(kPrefsCornerRadius);
		const wxColour highlight = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
		if (PrefsHighlight::HighContrast()) {
			// The plain colours, and an outline for the focus, as the native lists draw them.
			if (selected) {
				PrefsHighlight::FillRounded(dc,
					Pill(rect),
					radius,
					highlight,
					HasFocus() ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
						   : wxNullColour,
					FromDIP(2));
			} else {
				PrefsHighlight::FillRounded(
					dc, Pill(rect), radius, GetBackgroundColour(), highlight, FromDIP(1));
			}
			return;
		}
		const wxColour back = GetBackgroundColour();
		const wxColour fill =
			selected ? MixColours(back, highlight, HasFocus() ? 0.38 : 0.22)
				 : MixColours(
					   back, wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT), 0.07);
		// Outlined like the focused search field, so keyboard focus shows in dark mode too.
		PrefsHighlight::FillRounded(dc,
			Pill(rect),
			radius,
			fill,
			selected && HasFocus() ? highlight : wxNullColour,
			FromDIP(2));
	}

	void OnDrawItem(wxDC &dc, const wxRect &rect, size_t n) const override
	{
		const Row &row = m_rows[n];
		const wxRect pill = Pill(rect);
		const int iconX = pill.x + FromDIP(kPadding);
		const wxBitmap icon = row.icon.GetBitmapFor(this);
		if (icon.IsOk()) {
			dc.DrawBitmap(
				icon, iconX, pill.y + (pill.height - FromDIP(kPrefsIconSize)) / 2, true);
		}
		const int textX = iconX + FromDIP(kPrefsIconSize + kIconGap);
		const wxRect text(
			textX, pill.y, pill.x + pill.width - FromDIP(kPadding) - textX, pill.height);
		dc.SetFont(GetFont());
		const bool inverted = IsSelected(n) && PrefsHighlight::HighContrast();
		dc.SetTextForeground(wxSystemSettings::GetColour(
			inverted ? wxSYS_COLOUR_HIGHLIGHTTEXT : wxSYS_COLOUR_LISTBOXTEXT));
		// A vertical scrollbar takes its width out of the rows: the title gives way rather than run
		// past its highlight.
		dc.DrawLabel(wxControl::Ellipsize(row.label, dc, wxELLIPSIZE_END, text.width),
			text,
			wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
	}

private:
	// Space between the list's edges and a highlight, and between highlights.
	static constexpr int kRowInset = 6;
	static constexpr int kRowGap = 1;
	// Space inside a highlight before the icon and after the text, and between the two.
	static constexpr int kPadding = 10;
	static constexpr int kIconGap = 8;

	std::vector<Row> m_rows;
	int m_hover = wxNOT_FOUND;
	//! Every row is as tall; worked out once, since wx asks for it on every hit test and paint.
	wxCoord m_rowHeight = 0;

	wxRect Pill(const wxRect &row) const
	{
		return wxRect(row).Deflate(FromDIP(kRowInset), FromDIP(kRowGap));
	}

	void UpdateRowHeight()
	{
		m_rowHeight = std::max(FromDIP(kPrefsIconSize), GetCharHeight()) + FromDIP(12);
		if (GetItemCount() > 0) {
			RefreshAll();
		}
	}

	void OnKeyDown(wxKeyEvent &event)
	{
		switch (event.GetKeyCode()) {
		case WXK_RETURN:
		case WXK_NUMPAD_ENTER:
			// Nothing to open: a page shows as soon as it is selected. Kept here, or the key
			// would go on to press the dialog's default button.
			return;
		case WXK_PAGEUP:
		case WXK_PAGEDOWN:
		case WXK_NUMPAD_PAGEUP:
		case WXK_NUMPAD_PAGEDOWN:
			// Handled here: wx 3.2 moves a PageDown to the first row when every row fits.
			if (!event.HasAnyModifiers() && !m_rows.empty()) {
				const bool down = event.GetKeyCode() == WXK_PAGEDOWN ||
						  event.GetKeyCode() == WXK_NUMPAD_PAGEDOWN;
				MovePage(down);
			}
			return;
		case WXK_HOME:
		case WXK_END:
		case WXK_UP:
		case WXK_DOWN:
		case WXK_NUMPAD_HOME:
		case WXK_NUMPAD_END:
		case WXK_NUMPAD_UP:
		case WXK_NUMPAD_DOWN:
			// wxVListBox would select a row that an empty list does not have.
			if (m_rows.empty()) {
				return;
			}
			break;
		default:
			break;
		}
		event.Skip();
	}

	void MovePage(bool down)
	{
		const int count = static_cast<int>(m_rows.size());
		const int page =
			std::max(1, static_cast<int>(GetVisibleRowsEnd() - GetVisibleRowsBegin()) - 1);
		const int current = GetSelection() == wxNOT_FOUND ? 0 : GetSelection();
		Activate(down ? std::min(count - 1, current + page) : std::max(0, current - page));
	}

	//! Selects @p row and sends wxEVT_LISTBOX, as a click on it would.
	void Activate(int row)
	{
		if (row != GetSelection()) {
			SetSelection(row);
			SendSelectedEvent();
		}
	}

	void OnMouseWheel(wxMouseEvent &event)
	{
		// The list only scrolls up and down; wx's scrolling would take a sideways swipe for one.
		if (event.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL) {
			return;
		}
		event.Skip();
		CallAfter([this] { SetHoverAt(ScreenToClient(wxGetMousePosition())); });
	}

	void SetHoverAt(const wxPoint &pos)
	{
		SetHover(GetClientRect().Contains(pos) ? VirtualHitTest(pos.y) : wxNOT_FOUND);
	}

	void SetHover(int row)
	{
		if (row == m_hover) {
			return;
		}
		if (m_hover != wxNOT_FOUND) {
			RefreshRow(m_hover);
		}
		m_hover = row;
		if (m_hover != wxNOT_FOUND) {
			RefreshRow(m_hover);
		}
	}

	// Screen readers follow the selection and the focus by these.
	void NotifySelection()
	{
#if wxUSE_ACCESSIBILITY
		const int row = GetSelection();
		if (row == wxNOT_FOUND) {
			return;
		}
		wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_SELECTION, this, wxOBJID_CLIENT, row + 1);
		if (HasFocus()) {
			NotifyFocus();
		}
#endif
	}

#if wxUSE_ACCESSIBILITY
	void NotifyFocus()
	{
		if (!HasFocus()) {
			return;
		}
		const int row = GetSelection();
		wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_FOCUS,
			this,
			wxOBJID_CLIENT,
			row == wxNOT_FOUND ? wxACC_SELF : row + 1);
	}

	// A list of items to assistive technology, which would otherwise see one unnamed window.
	class Accessible : public wxWindowAccessible
	{
	public:
		explicit Accessible(PrefsSidebar *list)
		: wxWindowAccessible(list)
		, m_list(list)
		{
		}

		wxAccStatus GetChildCount(int *childCount) override
		{
			*childCount = static_cast<int>(m_list->m_rows.size());
			return wxACC_OK;
		}

		wxAccStatus GetChild(int childId, wxAccessible **child) override
		{
			// The rows are simple elements: no accessible object of their own.
			*child = nullptr;
			return IsRow(childId) || childId == wxACC_SELF ? wxACC_OK : wxACC_INVALID_ARG;
		}

		wxAccStatus GetName(int childId, wxString *name) override
		{
			if (IsRow(childId)) {
				*name = m_list->m_rows[childId - 1].label;
				return wxACC_OK;
			}
			// The label the dialog gives the list; wx 3.2 would name it by its class otherwise.
			*name = m_list->GetLabel();
			return name->empty() ? wxWindowAccessible::GetName(childId, name) : wxACC_OK;
		}

		wxAccStatus GetRole(int childId, wxAccRole *role) override
		{
			*role = IsRow(childId) ? wxROLE_SYSTEM_LISTITEM : wxROLE_SYSTEM_LIST;
			return wxACC_OK;
		}

		wxAccStatus GetState(int childId, long *state) override
		{
			const bool focused = m_list->HasFocus();
			if (!IsRow(childId)) {
				// wx gives a window that is not a wxControl no focus states of its own.
				long windowState = 0;
				if (wxWindowAccessible::GetState(childId, &windowState) != wxACC_OK) {
					windowState = 0;
				}
				*state = windowState | wxACC_STATE_SYSTEM_FOCUSABLE;
				if (focused) {
					*state |= wxACC_STATE_SYSTEM_FOCUSED;
				}
				return wxACC_OK;
			}
			const int row = childId - 1;
			*state = wxACC_STATE_SYSTEM_SELECTABLE | wxACC_STATE_SYSTEM_FOCUSABLE;
			if (!m_list->IsRowVisible(row)) {
				*state |= wxACC_STATE_SYSTEM_OFFSCREEN | wxACC_STATE_SYSTEM_INVISIBLE;
			}
			if (m_list->GetSelection() == row) {
				*state |= wxACC_STATE_SYSTEM_SELECTED;
				if (focused) {
					*state |= wxACC_STATE_SYSTEM_FOCUSED;
				}
			}
			return wxACC_OK;
		}

		wxAccStatus GetLocation(wxRect &rect, int elementId) override
		{
			if (!IsRow(elementId)) {
				return wxWindowAccessible::GetLocation(rect, elementId);
			}
			// Rows scrolled out of view are where they would be drawn, above or below the list.
			const int row = elementId - 1;
			const int first = static_cast<int>(m_list->GetVisibleRowsBegin());
			rect = wxRect(0,
				(row - first) * m_list->m_rowHeight,
				m_list->GetClientSize().GetWidth(),
				m_list->m_rowHeight);
			rect.SetPosition(m_list->ClientToScreen(rect.GetPosition()));
			return wxACC_OK;
		}

		wxAccStatus HitTest(const wxPoint &pt, int *childId, wxAccessible **child) override
		{
			const wxPoint pos = m_list->ScreenToClient(pt);
			*child = nullptr;
			*childId = wxACC_SELF;
			if (!m_list->GetClientRect().Contains(pos)) {
				return wxACC_FALSE;
			}
			const int row = m_list->VirtualHitTest(pos.y);
			if (row == wxNOT_FOUND) {
				*child = this;
			} else {
				*childId = row + 1;
			}
			return wxACC_OK;
		}

		wxAccStatus GetFocus(int *childId, wxAccessible **child) override
		{
			*child = nullptr;
			*childId = wxACC_SELF;
			if (!m_list->HasFocus()) {
				return wxACC_FALSE;
			}
			const int row = m_list->GetSelection();
			if (row == wxNOT_FOUND) {
				*child = this;
			} else {
				*childId = row + 1;
			}
			return wxACC_OK;
		}

		wxAccStatus GetSelections(wxVariant *selections) override
		{
			const int row = m_list->GetSelection();
			if (row == wxNOT_FOUND) {
				selections->MakeNull();
			} else {
				*selections = static_cast<long>(row) + 1;
			}
			return wxACC_OK;
		}

		wxAccStatus Select(int childId, wxAccSelectionFlags selectFlags) override
		{
			if (!IsRow(childId)) {
				return wxACC_NOT_IMPLEMENTED;
			}
			if (selectFlags & wxACC_SEL_TAKEFOCUS) {
				m_list->SetFocus();
			}
			if (selectFlags & (wxACC_SEL_TAKESELECTION | wxACC_SEL_ADDSELECTION)) {
				m_list->Activate(childId - 1);
			}
			return wxACC_OK;
		}

		wxAccStatus DoDefaultAction(int childId) override
		{
			if (!IsRow(childId)) {
				return wxACC_NOT_IMPLEMENTED;
			}
			m_list->Activate(childId - 1);
			return wxACC_OK;
		}

	private:
		PrefsSidebar *m_list;

		bool IsRow(int childId) const
		{
			return childId > 0 && childId <= static_cast<int>(m_list->m_rows.size());
		}
	};
#endif
};

#else // !__WXMSW__

//! macOS and GTK: the toolkit's own list, which screen readers read row by row. wx can expose the
//! rows of a list it does not draw natively only on Windows. macOS draws the selection rounded and
//! inset in its row; GTK keeps its square one.
class PrefsSidebar : public wxDataViewListCtrl
{
public:
	struct Row
	{
		wxString label;
		wxBitmapBundle icon;
		unsigned page; //!< The `pages[]` index the row stands for.
	};

	//! Space, in DIPs, to leave around the list.
	static constexpr int kMargin = 4;

	PrefsSidebar(wxWindow *parent, wxWindowID id)
	: wxDataViewListCtrl(
		  parent, id, wxDefaultPosition, wxDefaultSize, wxDV_SINGLE | wxDV_NO_HEADER | wxBORDER_NONE)
	{
		// Taller rows than the toolkit's tightest, as in current settings windows: the sidebar is
		// the dialog's main navigation, not a dense data list. A no-op where the port cannot set it.
		SetRowHeight(std::max(FromDIP(kPrefsIconSize), GetCharHeight()) + FromDIP(12));
		m_column = AppendIconTextColumn(
			wxString(), wxDATAVIEW_CELL_INERT, wxCOL_WIDTH_DEFAULT, wxALIGN_LEFT, 0);
		Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent &event) {
			event.Skip();
			// Also sent, with no item, when a refill clears the selection.
			if (event.GetItem().IsOk()) {
				wxCommandEvent choice(wxEVT_LISTBOX, GetId());
				choice.SetEventObject(this);
				choice.SetInt(ItemToRow(event.GetItem()));
				ProcessWindowEvent(choice);
			}
		});
	}

	//! Replaces the rows, leaving none selected.
	void SetRows(std::vector<Row> rows)
	{
		wxWindowUpdateLocker freeze(this);
		DeleteAllItems();
		for (Row &row : rows) {
			// The page index rides along as the row's item data.
			const wxVector<wxVariant> values(
				1, wxVariant(wxDataViewIconText(row.label, row.icon)));
			AppendItem(values, static_cast<wxUIntPtr>(row.page));
		}
	}

	unsigned PageAt(size_t row) const
	{
		return static_cast<unsigned>(GetItemData(RowToItem(static_cast<int>(row))));
	}

	//! Selects @p row and scrolls it into view, which wxDataViewListCtrl::SelectRow() does not.
	void SelectRow(int row)
	{
		wxDataViewListCtrl::SelectRow(static_cast<unsigned>(row));
		EnsureVisible(RowToItem(row));
	}

	//! Sizes the list to show @p labels in full beside their icons. An estimate of what the
	//! toolkit's cells need beyond the text, which only macOS can measure, once the list is on
	//! screen (MeasureWidth()).
	void FitWidth(const std::vector<wxString> &labels)
	{
		int text = 0;
		for (const wxString &label : labels) {
			text = std::max(text, GetTextExtent(label).GetWidth());
		}
#ifdef __WXMAC__
		m_textWidth = text;
#endif
		SetColumnWidth(text + FromDIP(kPrefsIconSize + kCellPadding));
	}

#ifdef __WXMAC__
	//! Replaces FitWidth()'s estimate with the width the list measures for its rows, cell padding
	//! included. Call once the list is on screen; only the first call measures.
	void MeasureWidth()
	{
		if (m_measured) {
			return;
		}
		m_measured = true;
		// The answer is resolved while the list is laid out, after SetWidth() returns.
		m_column->SetWidth(wxCOL_WIDTH_AUTOSIZE);
		CallAfter([this]() {
			const int reported = m_column->GetWidth();
			// Narrower than the text is no measurement of icon and text: the estimate stands.
			if (reported > m_textWidth) {
				// The inset row style pads both ends of every row, out of the label's room,
				// and the measurement leaves that padding out.
				SetColumnWidth(reported + FromDIP(16));
				InvalidateBestSize();
				wxGetTopLevelParent(this)->Layout();
			}
		});
	}
#endif

private:
	// Room, in DIPs, for the gap after the icon and the cell's padding at both ends. Generous on
	// macOS: what MeasureWidth() reports is bounded by the room the list already has.
#ifdef __WXMAC__
	static constexpr int kCellPadding = 48;
#else
	static constexpr int kCellPadding = 24;
#endif

	wxDataViewColumn *m_column = nullptr;
#ifdef __WXMAC__
	//! Widest label, as the list's font measures it.
	int m_textWidth = 0;
	//! The measurement is a one-off; the dialog is shown more than once.
	bool m_measured = false;
#endif

	void SetColumnWidth(int width)
	{
		m_column->SetWidth(width);
		// The list draws its cells a little in from its left edge -- 6 points on macOS, and not
		// zero elsewhere -- so a list exactly as wide as its column clips the cell on the right.
		const int listWidth = width + FromDIP(6);
		SetMinSize(wxSize(listWidth, -1));
		SetMaxSize(wxSize(listWidth, -1));
	}
};

#endif // __WXMSW__

#endif // AMULE_PREFSSIDEBAR_H
