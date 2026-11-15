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
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/vlbox.h>
#if wxUSE_ACCESSIBILITY
#include <wx/access.h>
#endif

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

// Corner radius, in DIPs, of the selected sidebar row and of the search box above it. Windows 11
// rounds list items and text boxes by 4 px, macOS its sidebar selection by about 6 points, and
// KDE's Breeze its controls by 3 to 5 px: 5 sits between them and reads as rounded on all three.
constexpr int kPrefsCornerRadius = 5;
//! Size, in DIPs, of the page icons in the sidebar.
constexpr int kPrefsIconSize = 20;

// Colours for the rounded highlights, mixed from the system's so they follow light and dark mode.
namespace PrefsHighlight
{
//! @p from moved toward @p to by @p amount, from 0 (all @p from) to 1 (all @p to).
inline wxColour Mix(const wxColour &from, const wxColour &to, double amount)
{
	return wxColour(wxColour::AlphaBlend(to.Red(), from.Red(), amount),
		wxColour::AlphaBlend(to.Green(), from.Green(), amount),
		wxColour::AlphaBlend(to.Blue(), from.Blue(), amount));
}

//! Fills @p rect with a rounded rectangle, anti-aliased where the platform can, and outlines it
//! with @p border unless that is wxTransparentColour.
inline void FillRounded(wxDC &dc,
	const wxRect &rect,
	double radius,
	const wxColour &fill,
	const wxColour &border = wxTransparentColour,
	double borderWidth = 1)
{
	const std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
	if (gc) {
		gc->SetBrush(wxBrush(fill));
		if (border.IsOk() && border != wxTransparentColour) {
			gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(border).Width(borderWidth)));
		} else {
			gc->SetPen(*wxTRANSPARENT_PEN);
		}
		// Inset by half the pen so the outline stays inside the rectangle.
		const double inset = border == wxTransparentColour ? 0 : borderWidth / 2;
		gc->DrawRoundedRectangle(rect.x + inset,
			rect.y + inset,
			rect.width - 2 * inset,
			rect.height - 2 * inset,
			radius);
		return;
	}
	dc.SetBrush(wxBrush(fill));
	dc.SetPen(border == wxTransparentColour ? *wxTRANSPARENT_PEN : wxPen(border));
	dc.DrawRoundedRectangle(rect, radius);
}
} // namespace PrefsHighlight

//! The preferences dialog's page list: an icon and a title per row, the selected row drawn as a
//! rounded highlight. Drawn here rather than by the toolkit's list control, which draws a square
//! selection on Windows and GTK and cannot be asked for anything else, so all three platforms
//! get the same look. Selecting a row with the mouse or the keyboard sends wxEVT_LISTBOX;
//! SelectRow() does not.
class PrefsSidebar : public wxVListBox
{
public:
	struct Row
	{
		wxString label;
		wxBitmapBundle icon;
		unsigned page; //!< The `pages[]` index the row stands for.
	};

	PrefsSidebar(wxWindow *parent, wxWindowID id)
	: wxVListBox(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
	{
		Bind(wxEVT_MOTION, [this](wxMouseEvent &event) {
			SetHover(VirtualHitTest(event.GetY()));
			event.Skip();
		});
		Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) {
			SetHover(wxNOT_FOUND);
			event.Skip();
		});
		// The selected row is drawn stronger while the list has the focus.
		Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &event) {
			RefreshSelected();
			NotifySelection();
			event.Skip();
		});
		Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) {
			RefreshSelected();
			event.Skip();
		});
		Bind(wxEVT_LISTBOX, [this](wxCommandEvent &event) {
			NotifySelection();
			event.Skip();
		});
#if wxUSE_ACCESSIBILITY
		new Accessible(this); // Owned by the window, which it registers itself with.
#endif
	}

	void SetRows(std::vector<Row> rows)
	{
		m_rows = std::move(rows);
		m_hover = wxNOT_FOUND;
		SetItemCount(m_rows.size());
		Refresh();
	}

	unsigned PageAt(size_t row) const { return m_rows[row].page; }

	//! Selects @p row without sending wxEVT_LISTBOX.
	void SelectRow(int row)
	{
		SetSelection(row);
		NotifySelection();
	}

	//! The width that shows @p labels in full beside their icons.
	int WidthFor(const std::vector<wxString> &labels) const
	{
		int text = 0;
		for (const wxString &label : labels) {
			text = std::max(text, GetTextExtent(label).GetWidth());
		}
		return text + FromDIP(kPrefsIconSize + 2 * kRowInset + 2 * kPadding + kIconGap);
	}

protected:
	wxCoord OnMeasureItem(size_t WXUNUSED(n)) const override
	{
		return std::max(FromDIP(kPrefsIconSize), GetCharHeight()) + FromDIP(12);
	}

	void OnDrawBackground(wxDC &dc, const wxRect &rect, size_t n) const override
	{
		const bool selected = IsSelected(n);
		if (!selected && static_cast<int>(n) != m_hover) {
			return;
		}
		const wxColour back = GetBackgroundColour();
		const wxColour fill =
			selected ? PrefsHighlight::Mix(back,
					   wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT),
					   HasFocus() ? 0.38 : 0.22)
				 : PrefsHighlight::Mix(
					   back, wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT), 0.07);
		PrefsHighlight::FillRounded(dc, Pill(rect), FromDIP(kPrefsCornerRadius), fill);
	}

	void OnDrawItem(wxDC &dc, const wxRect &rect, size_t n) const override
	{
		const Row &row = m_rows[n];
		const wxRect pill = Pill(rect);
		int x = pill.x + FromDIP(kPadding);
		const wxBitmap icon = row.icon.GetBitmapFor(this);
		if (icon.IsOk()) {
			dc.DrawBitmap(icon, x, pill.y + (pill.height - FromDIP(kPrefsIconSize)) / 2, true);
		}
		x += FromDIP(kPrefsIconSize + kIconGap);
		dc.SetFont(GetFont());
		dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT));
		const wxRect text(x, pill.y, pill.GetRight() - x - FromDIP(kPadding), pill.height);
		dc.DrawLabel(row.label, text, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
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

	wxRect Pill(const wxRect &row) const
	{
		return wxRect(row).Deflate(FromDIP(kRowInset), FromDIP(kRowGap));
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

	void RefreshSelected()
	{
		if (GetSelection() != wxNOT_FOUND) {
			RefreshRow(GetSelection());
		}
	}

	// Screen readers follow the page list by these, as they would a native one.
	void NotifySelection()
	{
#if wxUSE_ACCESSIBILITY
		const int row = GetSelection();
		if (row == wxNOT_FOUND) {
			return;
		}
		wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_SELECTION, this, wxOBJID_CLIENT, row + 1);
		if (HasFocus()) {
			wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_FOCUS, this, wxOBJID_CLIENT, row + 1);
		}
#endif
	}

#if wxUSE_ACCESSIBILITY
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
			if (!IsRow(childId)) {
				return wxWindowAccessible::GetName(childId, name);
			}
			*name = m_list->m_rows[childId - 1].label;
			return wxACC_OK;
		}

		wxAccStatus GetRole(int childId, wxAccRole *role) override
		{
			*role = IsRow(childId) ? wxROLE_SYSTEM_LISTITEM : wxROLE_SYSTEM_LIST;
			return wxACC_OK;
		}

		wxAccStatus GetState(int childId, long *state) override
		{
			if (!IsRow(childId)) {
				return wxWindowAccessible::GetState(childId, state);
			}
			*state = wxACC_STATE_SYSTEM_SELECTABLE | wxACC_STATE_SYSTEM_FOCUSABLE;
			if (m_list->GetSelection() == childId - 1) {
				*state |= wxACC_STATE_SYSTEM_SELECTED;
				if (m_list->HasFocus()) {
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
			rect = m_list->GetItemRect(elementId - 1);
			rect.SetPosition(m_list->ClientToScreen(rect.GetPosition()));
			return wxACC_OK;
		}

		wxAccStatus HitTest(const wxPoint &pt, int *childId, wxAccessible **child) override
		{
			const int row = m_list->VirtualHitTest(m_list->ScreenToClient(pt).y);
			*childId = row == wxNOT_FOUND ? wxACC_SELF : row + 1;
			*child = nullptr;
			return wxACC_OK;
		}

		wxAccStatus GetFocus(int *childId, wxAccessible **child) override
		{
			*child = nullptr;
			if (!m_list->HasFocus()) {
				*childId = wxACC_SELF;
				return wxACC_FALSE;
			}
			const int row = m_list->GetSelection();
			*childId = row == wxNOT_FOUND ? wxACC_SELF : row + 1;
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

	private:
		PrefsSidebar *m_list;

		bool IsRow(int childId) const
		{
			return childId > 0 && childId <= static_cast<int>(m_list->m_rows.size());
		}
	};
#endif
};

#endif // AMULE_PREFSSIDEBAR_H
