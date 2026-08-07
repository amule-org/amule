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
	// The generic backend passes the real cell rect to Render() and treats this
	// only as a best-width hint, but the native macOS backend takes GetSize()
	// as the cell size itself (wxCustomRendererObject::cellSize), so a constant
	// here paints a fixed-width bar inside an arbitrarily wide column. Report
	// the owning column's current width to keep both backends in step; the
	// literal is the fallback for before the renderer is attached.
	int width = 40;
	if (const wxDataViewColumn *column = GetOwner()) {
		width = std::max(width, column->GetWidth());
	}
	// Height is deliberately wxDefaultCoord: wxDataViewCustomRendererBase::
	// WXCallRender() only applies its vertical alignment -- which shrinks the
	// rect it hands Render() down to GetSize().y -- when the height is >= 0.
	// Reporting a concrete height there means a row taller than that (the
	// generic backend pads rows past the text extent) draws the bar at the top
	// with a gap under it. Opting out leaves the bar the full cell.
	return wxSize(width, wxDefaultCoord);
}

bool CMuleBarRenderer::Render(wxRect cell, wxDC *dc, int WXUNUSED(state))
{
	if (cell.GetWidth() <= 0 || cell.GetHeight() <= 0) {
		return true;
	}

	const bool bFlat = thePrefs::UseFlatBar();

	// Round bar has a black border; the bar itself is inset by one pixel on
	// each side -- matches every pre-port CBarShader caller.
	const wxRect barRect = InsetForBar(cell, bFlat);
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
