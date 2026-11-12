//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef DOWNLOAD_PERCENTAGE_DRAWING_H
#define DOWNLOAD_PERCENTAGE_DRAWING_H

#include <cmath>

#include <wx/dc.h>
#include <wx/window.h>

// Bold, because the contrast outline thins regular strokes. The list font follows
// the OS and display scaling, which a fixed pixel size would not.
inline void DrawDownloadPercentage(
	wxDC &dc, wxRect area, const wxString &text, const wxWindow &view, bool hashing)
{
	if (area.IsEmpty()) {
		return;
	}
	const wxFont oldFont = dc.GetFont();
	const wxColour oldColour = dc.GetTextForeground();
	const int oldBackgroundMode = dc.GetBackgroundMode();
	wxDCClipper clipper(dc, area);
	dc.SetFont(view.GetFont().Bold());
	dc.SetBackgroundMode(wxTRANSPARENT);
	const wxSize extent = dc.GetTextExtent(text);
	const int x = area.x + (area.width - extent.x) / 2;
	// The text is often taller than the area; rounding toward zero would push it down.
	const int y = area.y + static_cast<int>(std::floor((area.height - extent.y) / 2.0));
	// Outline white text so it stays readable on light fills.
	if (!hashing) {
		dc.SetTextForeground(*wxBLACK);
		const int offset = view.FromDIP(1);
		dc.DrawText(text, x - offset, y);
		dc.DrawText(text, x + offset, y);
		dc.DrawText(text, x, y - offset);
		dc.DrawText(text, x, y + offset);
	}
	dc.SetTextForeground(hashing ? *wxBLACK : *wxWHITE);
	dc.DrawText(text, x, y);
	dc.SetBackgroundMode(oldBackgroundMode);
	dc.SetTextForeground(oldColour);
	dc.SetFont(oldFont);
}

#endif // DOWNLOAD_PERCENTAGE_DRAWING_H
