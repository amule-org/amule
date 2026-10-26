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
#include <wx/event.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/stattext.h>

#include <functional>
#include <utility>
#include <vector>

// Wrap to the space assigned by the containing sizer, including nested layouts.
class WrappingStaticText : public wxStaticText
{
public:
	WrappingStaticText(wxWindow *parent, wxWindowID id, const wxString &text)
	: wxStaticText(parent, id, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE)
	, m_text(text)
	{
		SetLabelText(m_text);
		// The original translation must not dictate the container's minimum width.
		SetMinSize(wxSize(FromDIP(20), -1));
		Bind(wxEVT_SIZE, &WrappingStaticText::OnSize, this);
		for (wxWindow *ancestor = parent; ancestor; ancestor = ancestor->GetParent()) {
			if (auto *page = dynamic_cast<wxScrolledWindow *>(ancestor)) {
				m_viewport = page;
				page->Bind(wxEVT_SIZE, &WrappingStaticText::OnSize, this);
				break;
			}
		}
	}

	~WrappingStaticText() override
	{
		if (m_viewport) {
			m_viewport->Unbind(wxEVT_SIZE, &WrappingStaticText::OnSize, this);
		}
	}

	void SetLayoutCallback(std::function<void()> callback) { m_layoutCallback = std::move(callback); }

	bool SetFont(const wxFont &font) override
	{
		const bool changed = wxStaticText::SetFont(font);
		if (changed) {
			m_wrapWidth = -1;
			Rewrap();
		}
		return changed;
	}

private:
	void OnSize(wxSizeEvent &event)
	{
		Rewrap();
		event.Skip();
	}

	void Rewrap()
	{
		int width = GetClientSize().x;
		// A page measured before it is shown still has its default size (about 20 px on
		// wxOSX), which would wrap one word per line and inflate its minimum height.
		if (m_viewport && m_viewport->IsShownOnScreen()) {
			// Other fields may keep the virtual page wider than its viewport.
			int left = 0;
			for (wxWindow *window = this; window != m_viewport; window = window->GetParent()) {
				left += window->GetPosition().x + window->GetClientAreaOrigin().x;
			}
			width = wxMin(width, m_viewport->GetClientSize().x - wxMax(0, left));
		}
		if (width <= 0 || width == m_wrapWidth) {
			return;
		}
		m_wrapWidth = width;
		// New wxWidgets caches the width; older versions only insert line breaks.
		Wrap(-1);
		SetLabelText(m_text);
		Wrap(width);
		InvalidateBestSize();
		if (!m_layoutPending) {
			m_layoutPending = true;
			// Finish the current sizer pass before applying the new label height.
			// Queue on the control so destruction also discards the pending callback.
			CallAfter([this] {
				m_layoutPending = false;
				InvalidateBestSize();
				if (m_layoutCallback) {
					m_layoutCallback();
				} else {
					GetParent()->Layout();
				}
			});
		}
	}

	const wxString m_text;
	int m_wrapWidth = -1;
	bool m_layoutPending = false;
	std::function<void()> m_layoutCallback;
	wxScrolledWindow *m_viewport = nullptr;
};

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

// The scrollbar metrics can understate what a scrollbar really takes (GTK2
// leaves out its spacing), so make up whatever the content would still lack.
// contentSize need not be what the window holds now: a paged dialog shows one
// page while being sized for the largest.
inline void GrowDialogToContent(wxWindow *dialog, wxScrolledWindow *content, const wxSize &contentSize)
{
	// Laying out afterwards would reset the virtual size to the current content.
	dialog->Layout();
	content->SetVirtualSize(contentSize);
	wxSize missing = contentSize - content->GetClientSize();
	missing.IncTo(wxSize(0, 0));
	wxSize corrected = dialog->GetSize() + missing;
	corrected.DecTo(GetDialogSizeLimit(dialog));
	dialog->SetSize(corrected);
	content->FitInside();
}

// The scrollable content has a small explicit minimum; its natural size only
// influences the initial window size, never how far the user can shrink it.
inline void FitScrollableDialog(wxWindow *dialog, wxScrolledWindow *content)
{
	content->FitInside();
	const wxSize contentSize = content->GetSizer()->GetMinSize();
	const int fixedHeight =
		dialog->GetSizer()->GetMinSize().GetHeight() - content->GetMinSize().GetHeight();
	FitDialogToDisplay(dialog, contentSize + wxSize(0, fixedHeight));
	GrowDialogToContent(dialog, content, contentSize);
}

#endif // DIALOG_LAYOUT_H
