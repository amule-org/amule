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
#if defined(__WXOSX__)
#include "MacAppHelper.h"
#endif

// Keep the original text control (including the search-history dropdown) and
// its text events. The clear button stays visible even when the field is empty.
inline wxSizer *WithTextClearButton(wxWindow *input)
{
	wxWindow *parent = input->GetParent();
	const wxWindowID inputId = input->GetId();
	auto *row = new wxBoxSizer(wxHORIZONTAL);
	row->Add(input, wxSizerFlags(1).CenterVertical());
	long clearStyle = wxBU_EXACTFIT;
#if defined(__WXOSX__)
	// Cocoa's default rounded bezel needs a 40 DIP fitting width for a 16 DIP
	// bitmap, but this row gives the button 24 DIP. Use the compact square bezel,
	// whose native fitting size is smaller than the button.
	clearStyle |= wxBORDER_SIMPLE;
#endif
	const int side = input->GetBestSize().y;
	auto *clear = new wxBitmapButton(
		parent, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(side, side), clearStyle);
	clear->SetToolTip(_("Clear"));
	clear->SetMinSize(wxSize(side, side));
	const auto updateArtwork = [clear]() {
		// Bundles take a size at 100% DPI and rasterize the SVG at each scale.
		clear->SetBitmap(CamuleArtProvider::GetMenuBitmapBundle(
			"menu_x_lg", wxSize(16, 16), wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
		clear->SetBitmapDisabled(CamuleArtProvider::GetMenuBitmapBundle(
			"menu_x_lg", wxSize(16, 16), wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT)));
#if defined(__WXOSX__)
		// SetBitmap() resets Cocoa's image position to NSImageLeft.
		mac_set_button_image_only(clear->GetHandle());
#endif
	};
	updateArtwork();
	clear->Bind(wxEVT_SYS_COLOUR_CHANGED, [updateArtwork](wxSysColourChangedEvent &event) {
		updateArtwork();
		event.Skip();
	});
	// Keep clear of the macOS focus ring, which is drawn outside the field.
	row->Add(clear, wxSizerFlags().CenterVertical().Border(wxLEFT, input->FromDIP(5)));
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
