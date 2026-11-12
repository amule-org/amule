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

#include <wx/intl.h>
#include <wx/stattext.h>

// A sizer-managed rate label must not resize itself during SetLabel(): even a
// temporary shrink emits native size events before the strip restores its width.
class TransferRateLabel : public wxStaticText
{
public:
	TransferRateLabel(wxWindow *parent, wxWindowID id, const wxString &text)
	: wxStaticText(parent, id, text, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE | wxALIGN_RIGHT)
	{
	}

	static wxString FormatRate(double bytesPerSecond, double overheadBytesPerSecond, bool showOverhead)
	{
		const double kibibytes = bytesPerSecond / 1024.0;
		const bool mebibytes = kibibytes >= 1024.0;
		wxString text = wxString::Format("%.1f%s",
			mebibytes ? kibibytes / 1024.0 : kibibytes,
			mebibytes ? _(" MiB/s") : _(" KiB/s"));
		if (showOverhead) {
			text += wxString::Format(" (%.1f)", overheadBytesPerSecond / 1024.0);
		}
		return text;
	}

	// Returns true when the strip needs a new layout.
	bool UpdateRate(const wxString &text, bool showOverhead)
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
		// Shorter samples retain their space; a preference, font or DPI change releases it.
		SetMinSize(wxSize(best.x, -1));
		return previousMinimum != GetMinSize() || GetSize().y != best.y;
	}

private:
	wxFont m_font;
	wxSize m_dpi;
	bool m_showOverhead = false;
};

#endif // TRANSFER_RATE_LABEL_H
