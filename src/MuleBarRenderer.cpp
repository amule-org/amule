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

#include "MuleBarRenderer.h" // Interface declarations

#include "Preferences.h" // Needed for thePrefs::UseFlatBar(), CPreferences::Get3DDepth()

wxIMPLEMENT_DYNAMIC_CLASS(CBarFillSpec, wxObject);
// Non-"wx"-prefixed spelling -- see the comment on the DECLARE_VARIANT_OBJECT
// use in MuleBarRenderer.h.
IMPLEMENT_VARIANT_OBJECT(CBarFillSpec)

CMuleBarRenderer::CMuleBarRenderer()
: wxDataViewCustomRenderer("CBarFillSpec", wxDATAVIEW_CELL_INERT, wxALIGN_LEFT)
{
}

bool CMuleBarRenderer::SetValue(const wxVariant &value)
{
	m_spec << value;
	return true;
}

bool CMuleBarRenderer::GetValue(wxVariant &value) const
{
	value << m_spec;
	return true;
}

wxSize CMuleBarRenderer::GetSize() const
{
	// A minimum only: the column's actual on-screen width comes from
	// AppendBarColumn()'s caller and, after that, CListColumnStore.
	return wxSize(40, GetTextExtent("Xg").GetHeight());
}

bool CMuleBarRenderer::Render(wxRect cell, wxDC *dc, int WXUNUSED(state))
{
	if (cell.GetWidth() <= 0 || cell.GetHeight() <= 0) {
		return true;
	}

	const bool bFlat = thePrefs::UseFlatBar();

	wxRect barRect = cell;
	if (!bFlat) {
		// Round bar has a black border; the bar itself is inset by one pixel
		// on each side -- matches every pre-port CBarShader caller.
		barRect.x++;
		barRect.y++;
		barRect.width -= 2;
		barRect.height -= 2;
	}
	if (barRect.GetWidth() <= 0 || barRect.GetHeight() <= 0) {
		return true;
	}

	m_bar.SetWidth(barRect.GetWidth());
	m_bar.SetHeight(barRect.GetHeight());
	m_bar.Set3dDepth(CPreferences::Get3DDepth());
	m_bar.SetFileSize(m_spec.GetFileSize());

	if (!m_spec.GetSpans().empty()) {
		for (const CBarFillSpan &span : m_spec.GetSpans()) {
			m_bar.FillRange(span.start, span.end, span.colour);
		}
		m_bar.Draw(dc, barRect.x, barRect.y, bFlat);
	}

	if (!bFlat) {
		wxDCPenChanger pen(*dc, *wxBLACK_PEN);
		wxDCBrushChanger brush(*dc, *wxTRANSPARENT_BRUSH);
		dc->DrawRectangle(cell);
	}
	return true;
}
