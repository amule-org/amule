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

#ifndef DIALOG_LAYOUT_H
#define DIALOG_LAYOUT_H

#include <wx/display.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/settings.h>

#include <utility>
#include <vector>

// Horizontal scroll units must be exact pixels: larger units round the range
// past the virtual content edge and expose a blank strip at the right.
inline void ConfigureDialogScrolling(wxScrolledWindow *content)
{
	content->SetScrollRate(1, content->FromDIP(10));
}

// A section can dominate the total minimum while individual columns still change.
// Track every sizer item's minimum, including nested panels, rather than just the total.
inline void CollectDialogContentSizes(wxSizer *sizer, std::vector<wxSize> &sizes)
{
	for (wxSizerItem *item : sizer->GetChildren()) {
		sizes.push_back(item->IsShown() ? item->GetMinSize() : wxDefaultSize);
		wxSizer *nested = item->IsSizer()    ? item->GetSizer()
				  : item->IsWindow() ? item->GetWindow()->GetSizer()
						     : nullptr;
		if (nested) {
			CollectDialogContentSizes(nested, sizes);
		}
	}
}

inline bool UpdateDialogContentLayout(wxScrolledWindow *content, std::vector<wxSize> &previousSizes)
{
	std::vector<wxSize> sizes;
	sizes.push_back(content->GetSizer()->GetMinSize());
	CollectDialogContentSizes(content->GetSizer(), sizes);
	if (sizes != previousSizes) {
		content->FitInside();
		previousSizes = std::move(sizes);
		return true;
	}
	return false;
}

// The largest initial window size: a fraction of the work area of the parent's display.
inline wxSize GetDialogSizeLimit(wxWindow *dialog)
{
	int displayIndex = wxDisplay::GetFromWindow(dialog->GetParent() ? dialog->GetParent() : dialog);
	if (displayIndex == wxNOT_FOUND) {
		displayIndex = 0;
	}
	const wxSize available = wxDisplay(displayIndex).GetClientArea().GetSize();
	return wxSize(available.GetWidth() * 4 / 5, available.GetHeight() * 4 / 5);
}

// Keep the minimum dictated by the fixed controls, while bounding the initial
// window size to the work area of the parent's display.
inline void FitDialogToDisplay(wxWindow *dialog, const wxSize &preferredClientSize)
{
	dialog->GetSizer()->SetSizeHints(dialog);
	wxSize preferred = dialog->ClientToWindowSize(preferredClientSize);
	preferred.IncTo(dialog->GetMinSize());
	const wxSize limit = GetDialogSizeLimit(dialog);
	if (preferred.GetWidth() > limit.GetWidth()) {
		preferred.y += wxMax(0, wxSystemSettings::GetMetric(wxSYS_HSCROLL_Y, dialog));
	}
	if (preferred.GetHeight() > limit.GetHeight()) {
		preferred.x += wxMax(0, wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, dialog));
	}
	preferred.DecTo(limit);
	dialog->SetSize(preferred);
}

// The scrollable content has a small explicit minimum; its natural size only
// influences the initial window size, never how far the user can shrink it.
inline void FitScrollableDialog(wxWindow *dialog, wxScrolledWindow *content)
{
	content->FitInside();
	const int fixedHeight =
		dialog->GetSizer()->GetMinSize().GetHeight() - content->GetMinSize().GetHeight();
	FitDialogToDisplay(dialog, content->GetSizer()->GetMinSize() + wxSize(0, fixedHeight));

	// The scrollbar metrics can understate what a scrollbar really takes (GTK2
	// leaves out its spacing), so make up whatever the content still lacks.
	dialog->Layout();
	wxSize missing = content->GetVirtualSize() - content->GetClientSize();
	missing.IncTo(wxSize(0, 0));
	wxSize corrected = dialog->GetSize() + missing;
	corrected.DecTo(GetDialogSizeLimit(dialog));
	dialog->SetSize(corrected);
}

#endif // DIALOG_LAYOUT_H
