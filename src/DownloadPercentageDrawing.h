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

#include <wx/dc.h>
#include <wx/window.h>

// Use the live list font, including its point size and display scaling. A fixed
// pixel font would become too small on high-DPI displays. Measure after choosing
// the weight, so the label stays centred as the font or column changes.
inline void DrawDownloadPercentage(
	wxDC &dc, wxRect cell, const wxString &text, const wxWindow &view, bool hashing)
{
	if (cell.IsEmpty()) {
		return;
	}
	const wxFont oldFont = dc.GetFont();
	const wxColour oldColour = dc.GetTextForeground();
	const int oldBackgroundMode = dc.GetBackgroundMode();
	wxDCClipper clipper(dc, cell);
	// Use point size so display scaling does not change the weight choice.
	const wxFont font = view.GetFont();
	dc.SetFont(font.GetFractionalPointSize() <= 12.0 ? font.Bold() : font);
	dc.SetBackgroundMode(wxTRANSPARENT);
	const wxSize extent = dc.GetTextExtent(text);
	const int x = cell.x + (cell.width - extent.x) / 2;
	const int y = cell.y + (cell.height - extent.y) / 2;
	// Keep the contrast outline at every font size on ordinary bars.
	// Hashing bars retain their black text on green/yellow.
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
