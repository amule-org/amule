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

#include "MuleIconTextRenderer.h" // Interface declarations

#include <wx/dc.h>   // Needed for wxDC (not pulled in by dataview.h on GTK)
#include <wx/icon.h> // Needed for wxIcon

#include <algorithm> // Needed for std::max

CMuleIconTextRenderer::CMuleIconTextRenderer()
: wxDataViewCustomRenderer("wxDataViewIconText", wxDATAVIEW_CELL_INERT, wxALIGN_LEFT)
{
}

bool CMuleIconTextRenderer::SetValue(const wxVariant &value)
{
	m_value << value;
	return true;
}

bool CMuleIconTextRenderer::GetValue(wxVariant &value) const
{
	value << m_value;
	return true;
}

bool CMuleIconTextRenderer::Render(wxRect cell, wxDC *dc, int state)
{
	int xoffset = 0;

	const wxIcon &icon = m_value.GetIcon();
	if (icon.IsOk()) {
		// Centred vertically: the icon is a fixed size while the row height
		// follows the font, so the two rarely match.
		const int y = cell.y + std::max(0, (cell.height - icon.GetHeight()) / 2);
		dc->DrawIcon(icon, cell.x, y);
		xoffset = icon.GetWidth() + kIconTextGap;
	}

	// A row with no icon passes xoffset 0 and starts at the cell edge -- the
	// whole point of not using wx's renderer here.
	RenderText(m_value.GetText(), xoffset, cell, dc, state);
	return true;
}

wxSize CMuleIconTextRenderer::GetSize() const
{
	// Measured from a non-empty string when the cell has no text: an empty
	// extent would report zero height, and on the native macOS backend this
	// height is what the row is sized from.
	const wxString &text = m_value.GetText();
	wxSize size = GetTextExtent(text.IsEmpty() ? wxString("Xg") : text);

	const wxIcon &icon = m_value.GetIcon();
	if (icon.IsOk()) {
		size.x += icon.GetWidth() + kIconTextGap;
		size.y = std::max(size.y, icon.GetHeight());
	}
	return size;
}
