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

#include "ClientNameListCtrl.h" // Interface declarations

#include "MuleBarRenderer.h" // Needed for CBarFillSpec, CMuleBarRenderer

/**
 * Draws the Name column.
 *
 * Not a bar: CMuleBarRenderer is borrowed for its identity-carrying CBarFillSpec, which brings the
 * row's data here, the same way CDownloadBarRenderer reaches its CPartFile.
 */
class CClientNameListCtrl::NameRenderer : public CMuleBarRenderer
{
public:
	explicit NameRenderer(const CClientNameListCtrl &list)
	: m_list(list)
	{
	}

	bool Render(wxRect rect, wxDC *dc, int WXUNUSED(state)) override
	{
		const wxUIntPtr data = GetSpec().GetIdentity();
		if (data != 0) {
			const auto cell = m_list.NameCellOf(data);
			if (cell) {
				DrawClientNameCell(*cell, rect, dc);
			}
		}
		return true;
	}

private:
	const CClientNameListCtrl &m_list;
};

CClientNameListCtrl::CClientNameListCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: CMuleVirtualDataViewCtrl(parent, winid, pos, size, style, name)
{
	EnableCellToolTips();
}

void CClientNameListCtrl::AddNameColumn(
	const wxString &label, unsigned modelColumn, const wxString &key, int width, int flags)
{
	m_nameColumn = modelColumn;
	AddBarColumn(label, modelColumn, key, width, flags, new NameRenderer(*this));
}

std::optional<ClientNameCell> CClientNameListCtrl::NameCellAt(
	const wxDataViewItem &item, unsigned column) const
{
	if (column != m_nameColumn) {
		return std::nullopt;
	}
	return NameCellOf(ItemAt(GetModelRow(item)));
}

int CClientNameListCtrl::GetCustomFitWidth(const wxDataViewItem &item, unsigned column, wxDC &dc) const
{
	const auto cell = NameCellAt(item, column);
	return cell ? MeasureClientNameCell(*cell, dc) : -1;
}

wxString CClientNameListCtrl::GetCellToolTip(const wxDataViewItem &item, unsigned column) const
{
	const auto cell = NameCellAt(item, column);
	return cell ? ClientNameCellToolTip(*cell) : wxString();
}

void CClientNameListCtrl::GetItemBarFill(wxUIntPtr data, unsigned column, CBarFillSpec &out) const
{
	if (column == m_nameColumn) {
		out = CBarFillSpec(data, 0, {});
	}
}
