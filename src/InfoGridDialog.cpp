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

#include "InfoGridDialog.h" // Interface declarations

#include <wx/artprov.h>
#include <wx/bmpbndl.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/window.h>

namespace
{
// GUI thread only. ShowModal() does not stop a click reaching the control that
// opened the dialog: the event is queued and delivered once ShowModal()
// returns, so without this each click made while the dialog was up opens
// another one on close.
bool g_dialogOpen = false;
} // namespace

void ShowInfoGridDialog(wxWindow *parent,
	const wxString &title,
	const wxString &intro,
	const std::function<void(wxWindow *, wxSizer *)> &fillGrid,
	const wxString &introArt,
	const wxColour &introColour)
{
	if (g_dialogOpen) {
		return;
	}
	g_dialogOpen = true;

	wxDialog dialog(parent, wxID_ANY, title);
	wxFlexGridSizer *grid = new wxFlexGridSizer(2, wxSize(8, 6));
	fillGrid(&dialog, grid);

	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	if (!intro.IsEmpty()) {
		// A stock id can exist without the platform's provider supplying art
		// for it, so fall back to the bare line rather than leaving a gap.
		const wxBitmapBundle introBmp =
			introArt.IsEmpty()
				? wxBitmapBundle()
				: wxArtProvider::GetBitmapBundle(introArt, wxART_MESSAGE_BOX, wxSize(16, 16));
		wxStaticText *introText = new wxStaticText(&dialog, wxID_ANY, intro);
		if (introColour.IsOk()) {
			introText->SetForegroundColour(introColour);
		}
		if (!introBmp.IsOk()) {
			top->Add(introText, 0, wxALL, 10);
		} else {
			wxBoxSizer *introRow = new wxBoxSizer(wxHORIZONTAL);
			introRow->Add(new wxStaticBitmap(&dialog, wxID_ANY, introBmp),
				0,
				wxALIGN_CENTRE_VERTICAL | wxRIGHT,
				6);
			introRow->Add(introText, 0, wxALIGN_CENTRE_VERTICAL);
			top->Add(introRow, 0, wxALL, 10);
		}
	}
	top->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
	if (wxSizer *buttons = dialog.CreateButtonSizer(wxOK)) {
		top->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);
	}

	dialog.SetSizerAndFit(top);
	dialog.CentreOnParent();
	dialog.ShowModal();

	// Cleared behind the queued clicks rather than here, so they are dispatched
	// -- and rejected above -- before the flag drops.
	if (parent) {
		parent->CallAfter([] { g_dialogOpen = false; });
	} else {
		g_dialogOpen = false;
	}
}
