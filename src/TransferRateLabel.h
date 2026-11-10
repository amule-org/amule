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

// Keep the space already needed by transfer rates when the next sample is shorter
// (especially when zero traffic hides the units). Grow for larger values, translated
// text or font changes, but never move the following status fields back and forth.
inline void UpdateTransferRateLabel(wxStaticText *label, const wxString &text)
{
	if (!label) {
		return;
	}
	const bool changed = label->GetLabel() != text;
	if (changed) {
		label->SetLabel(text);
	}
	// Measure without the previous explicit minimum: wxWidgets can use it in
	// place of the natural width, which would prevent larger rates from fitting.
	const int previousWidth = label->GetMinSize().x;
	label->SetMinSize(wxDefaultSize);
	label->InvalidateBestSize();
	wxSize best = label->GetBestSize();
	best.x = std::max(previousWidth, best.x);
	label->SetMinSize(wxSize(best.x, -1));
	if (!changed && label->GetSize() == best) {
		return;
	}
	if (wxSizer *strip = label->GetContainingSizer()) {
		strip->Layout();
	}
}

#endif // TRANSFER_RATE_LABEL_H
