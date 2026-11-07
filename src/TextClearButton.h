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

#ifndef TEXT_CLEAR_BUTTON_H
#define TEXT_CLEAR_BUTTON_H

#include "CamuleArtProvider.h"

#include <wx/settings.h>
#include <wx/bmpbuttn.h>
#include <wx/sizer.h>
#include <wx/textentry.h>
#include <algorithm>

// Cocoa's bitmap-only button positions this 16 DIP image too far right inside
// the small clear-button bezel. Shift the artwork within its bitmap so the X
// is centered; leave the button and its spacing unchanged.
inline wxBitmapBundle CenterClearButtonArtwork(wxBitmapBundle artwork, wxWindow *window)
{
#if defined(__WXOSX__)
	const wxBitmap source = artwork.GetBitmapFor(window);
	if (!source.IsOk()) {
		return artwork;
	}
	const wxImage original = source.ConvertToImage();
	const int offset = window->FromDIP(3);
	if (offset <= 0 || offset >= original.GetWidth()) {
		return artwork;
	}

	wxImage shifted(original.GetWidth(), original.GetHeight());
	shifted.InitAlpha();
	const size_t pixelCount = size_t(original.GetWidth()) * original.GetHeight();
	std::fill_n(shifted.GetData(), pixelCount * 3, 0);
	std::fill_n(shifted.GetAlpha(), pixelCount, 0);
	const unsigned char *sourceData = original.GetData();
	const unsigned char *sourceAlpha = original.GetAlpha();
	unsigned char *shiftedData = shifted.GetData();
	unsigned char *shiftedAlpha = shifted.GetAlpha();
	for (int y = 0; y < original.GetHeight(); ++y) {
		for (int x = 0; x < original.GetWidth() - offset; ++x) {
			const size_t sourcePixel = size_t(y) * original.GetWidth() + x + offset;
			const size_t targetPixel = size_t(y) * original.GetWidth() + x;
			std::copy_n(sourceData + sourcePixel * 3, 3, shiftedData + targetPixel * 3);
			shiftedAlpha[targetPixel] = sourceAlpha ? sourceAlpha[sourcePixel] : 255;
		}
	}
	return wxBitmapBundle::FromBitmap(wxBitmap(shifted, wxBITMAP_SCREEN_DEPTH, source.GetScaleFactor()));
#else
	(void)window;
	return artwork;
#endif
}

// Keep the original text control (including the search-history dropdown) and
// its text events. The clear button stays visible even when the field is empty.
inline wxSizer *WithTextClearButton(wxWindow *input)
{
	wxWindow *parent = input->GetParent();
	const wxWindowID inputId = input->GetId();
	auto *row = new wxBoxSizer(wxHORIZONTAL);
	row->Add(input, wxSizerFlags(1).Expand());
	// Width for the 16px icon + padding; height stays flexible so the sizer
	// stretches the button to the field's height -- a fixed square computed
	// from GetBestSize().y before layout ends up smaller than the real field
	// on macOS and the X looks off-center.
	const int side = std::max(input->GetBestSize().y, input->FromDIP(22));
	auto *clear = new wxBitmapButton(
		parent, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	clear->SetToolTip(_("Clear"));
	// Fixed width, flexible height: the row stretches it to match the field,
	// so the bitmap stays vertically centered on every platform. The max
	// height keeps the button's own best size (native chrome is taller than
	// the field on some platforms) from raising the row above the field's
	// height; the min height stays open so the button still stretches when
	// the field ends up taller (macOS bezel).
	clear->SetMinSize(wxSize(side, -1));
	clear->SetMaxSize(wxSize(side, input->GetBestSize().y));
	const auto updateArtwork = [clear]() {
		// Bundles take a size at 100% DPI and rasterize the SVG at each scale.
		clear->SetBitmap(
			CenterClearButtonArtwork(CamuleArtProvider::GetMenuBitmapBundle("menu_x_lg",
							 wxSize(16, 16),
							 wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)),
				clear));
		clear->SetBitmapDisabled(
			CenterClearButtonArtwork(CamuleArtProvider::GetMenuBitmapBundle("menu_x_lg",
							 wxSize(16, 16),
							 wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT)),
				clear));
	};
	updateArtwork();
	clear->Bind(wxEVT_SYS_COLOUR_CHANGED, [updateArtwork](wxSysColourChangedEvent &event) {
		updateArtwork();
		event.Skip();
	});
	// Keep clear of the macOS focus ring, which is drawn outside the field.
	row->Add(clear, wxSizerFlags().Expand().Border(wxLEFT, input->FromDIP(5)));
	clear->Enable(!dynamic_cast<wxTextEntry *>(input)->GetValue().IsEmpty());

	// Bind by ID on the parent: Search can replace its Name control when the
	// remember-history preference changes. Neither callback retains that control.
	parent->Bind(
		wxEVT_TEXT,
		[clear](wxCommandEvent &event) {
			clear->Enable(!event.GetString().IsEmpty());
			event.Skip();
		},
		inputId);
	clear->Bind(wxEVT_BUTTON, [parent, inputId](wxCommandEvent &) {
		wxWindow *field = parent->FindWindow(inputId);
		if (auto *entry = dynamic_cast<wxTextEntry *>(field)) {
			// wxComboBox::Clear() also deletes history; only clear the text.
			entry->SetValue(wxEmptyString);
			field->SetFocus();
		}
	});
	return row;
}

#endif // TEXT_CLEAR_BUTTON_H
