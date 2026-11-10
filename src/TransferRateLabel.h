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

#ifndef TRANSFER_RATE_LABEL_H
#define TRANSFER_RATE_LABEL_H

#include <algorithm>

#include <wx/sizer.h>
#include <wx/stattext.h>

// A sizer-managed rate label must not resize itself during SetLabel(): even a
// temporary shrink emits native size events before the strip restores its width.
class TransferRateLabel : public wxStaticText
{
public:
	TransferRateLabel(wxWindow *parent, wxWindowID id, const wxString &text)
	: wxStaticText(parent, id, text, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE)
	{
	}

	void UpdateRate(const wxString &text, bool showOverhead)
	{
		const wxFont font = GetFont();
		const wxSize dpi = GetDPI();
		const bool resetWidth = font != m_font || dpi != m_dpi || showOverhead != m_showOverhead;
		m_font = font;
		m_dpi = dpi;
		m_showOverhead = showOverhead;
		const wxSize previousMinimum = GetMinSize();
		if (GetLabel() != text) {
			SetLabel(text);
		}
		// An explicit minimum can mask the natural width, including larger text.
		SetMinSize(wxDefaultSize);
		InvalidateBestSize();
		wxSize best = GetBestSize();
		if (!resetWidth) {
			best.x = std::max(previousMinimum.x, best.x);
		}
		SetMinSize(wxSize(best.x, -1));
		// Shorter samples retain their space, but a preference/font/DPI change
		// must release it. Only relayout the strip when its geometry changes:
		// laying out the parent also repaints the main content on wxMSW.
		if (previousMinimum != GetMinSize() || GetSize().y != best.y) {
			if (wxSizer *strip = GetContainingSizer()) {
				strip->Layout();
			}
		}
	}

private:
	wxFont m_font;
	wxSize m_dpi;
	bool m_showOverhead = false;
};

#endif // TRANSFER_RATE_LABEL_H
