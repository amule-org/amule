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

#ifndef MULEICONTEXTRENDERER_H
#define MULEICONTEXTRENDERER_H

#include <wx/dataview.h> // Needed for wxDataViewCustomRenderer, wxDataViewIconText

/**
 * Draws a wxDataViewIconText cell, but only reserves room for the icon on the
 * rows that actually have one.
 *
 * wx's own icon+text renderer reserves the icon slot unconditionally, so in a
 * column where the icon is the exception rather than the rule -- a rating
 * smiley on the handful of files that carry one -- every other row's text is
 * indented past an empty gap. On the native macOS cell the reserved width is
 * not even adjustable: wxImageTextCell hardcodes a 5px leading shift, a 5px
 * icon-to-text gap and a 16x16 icon as private ivars, none of them reachable
 * from C++.
 *
 * Text is drawn through wxDataViewCustomRenderer::RenderText(), so ellipsizing,
 * the selected-row foreground colour and RTL layout stay exactly as the stock
 * renderers do them; only the icon placement is ours.
 */
class CMuleIconTextRenderer : public wxDataViewCustomRenderer
{
public:
	CMuleIconTextRenderer();

	bool SetValue(const wxVariant &value) override;
	bool GetValue(wxVariant &value) const override;

	bool Render(wxRect cell, wxDC *dc, int state) override;
	wxSize GetSize() const override;

private:
	//! Gap between the icon and the text. No leading inset: the icon starts at
	//! the cell edge, which is what keeps an icon row aligned with a bare one.
	static const int kIconTextGap = 4;

	wxDataViewIconText m_value;
};

#endif // MULEICONTEXTRENDERER_H
