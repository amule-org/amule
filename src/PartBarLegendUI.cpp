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

#include "PartBarLegendUI.h" // Interface declarations

#include <wx/dcmemory.h> // Needed for wxMemoryDC (legend swatches)
#include <wx/intl.h>     // Needed for _()
#include <wx/sizer.h>    // Needed for wxSizer
#include <wx/statbmp.h>  // Needed for wxStaticBitmap
#include <wx/stattext.h> // Needed for wxStaticText
#include <wx/window.h>   // Needed for wxWindow

#include "InfoGridDialog.h" // Needed for ShowInfoGridDialog
#include "Preferences.h"    // Needed for thePrefs::UseFlatBar

namespace
{

//! Swatch size in both directions. One constant, so the gradient covers exactly
//! the area a flat swatch fills and the rows line up.
constexpr int kSwatchSide = 16;

//! The two cells of one legend row, once, whatever the swatch was drawn by.
void AddSwatchRow(wxWindow *parent, wxSizer *grid, const wxBitmap &swatch, const wxString &label)
{
	grid->Add(new wxStaticBitmap(parent, wxID_ANY, swatch), 0, wxALIGN_CENTRE_VERTICAL);
	grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTRE_VERTICAL);
}

wxString SourcePartStateLabel(partbar::SourcePartState state)
{
	switch (state) {
	case partbar::SourcePartState::Missing:
		return _("This source does not have the part");
	case partbar::SourcePartState::Complete:
		return _("You and this source both have the part");
	case partbar::SourcePartState::Downloading:
		return _("Being downloaded from this source now");
	case partbar::SourcePartState::NextRequested:
		return _("The next part that will be asked of this source");
	case partbar::SourcePartState::Needed:
		return _("This source has the part and you still need it");
	}
	return wxEmptyString;
}

wxString PeerPartStateLabel(partbar::PeerPartState state)
{
	switch (state) {
	case partbar::PeerPartState::Present:
		return _("This peer already has the part");
	case partbar::PeerPartState::Missing:
		return _("This peer does not have the part");
	}
	return wxEmptyString;
}

// "other" is load-bearing. CKnownFile::UpdateAvailablePartsCount() fills
// m_AvailPartFrequency from the GetUpPartStatus() of remote clients only; our own
// copy is never counted. On the shared-files list we hold every part by
// definition, so a red block does not mean the part is gone -- it means no peer
// we know of has it too.
wxString AvailabilityPartStateLabel(partbar::AvailabilityPartState state)
{
	switch (state) {
	case partbar::AvailabilityPartState::ZeroSources:
		return _("No other source has this part");
	case partbar::AvailabilityPartState::FewSources:
		return _("One other source has this part");
	case partbar::AvailabilityPartState::ManySources:
		// Formatted from kAvailFull rather than spelled out: the number is
		// where the fade stops darkening, and a label naming a different
		// one would be wrong in a way no reader could tell from the bar.
		return wxString::Format(_("%u or more other sources have this part"), partbar::kAvailFull);
	}
	return wxEmptyString;
}

wxString HashingPartStateLabel(partbar::HashingPartState state)
{
	switch (state) {
	case partbar::HashingPartState::Hashed:
		return _("Already read back and hashed");
	case partbar::HashingPartState::NotYetHashed:
		return _("Still to be read");
	}
	return wxEmptyString;
}

//! The line above the grid, saying what one block of the bar stands for.
wxString LegendIntro(partbar::BarLegendKind kind)
{
	switch (kind) {
	case partbar::BarLegendKind::SourceParts:
		return _("One block per part of the file being downloaded.");
	case partbar::BarLegendKind::PeerParts:
		return _("One block per part of the shared file.");
	case partbar::BarLegendKind::SharedAvailability:
		return _(
			"One block per part of the shared file, coloured by how many other sources have it.");
	case partbar::BarLegendKind::SharedHashing:
		return _("One block per part of the shared file, showing how far the re-hash has read.");
	case partbar::BarLegendKind::None:
		break;
	}
	return wxEmptyString;
}

//! Fills the grid of the legend @a kind explains, under bar preference @a flat.
void FillLegendGrid(wxWindow *dlg, wxSizer *grid, partbar::BarLegendKind kind, bool flat)
{
	switch (kind) {
	case partbar::BarLegendKind::SourceParts:
		for (const partbar::SourcePartState state : partbar::kSourceLegendOrder) {
			AddLegendRow(dlg,
				grid,
				partbar::SourcePartColour(state, flat),
				SourcePartStateLabel(state));
		}
		return;

	case partbar::BarLegendKind::PeerParts:
		for (const partbar::PeerPartState state : partbar::kPeerLegendOrder) {
			AddLegendRow(
				dlg, grid, partbar::PeerPartColour(state, flat), PeerPartStateLabel(state));
		}
		return;

	case partbar::BarLegendKind::SharedAvailability: {
		for (const partbar::AvailabilityPartState state : partbar::kAvailabilityLegendOrder) {
			AddLegendRow(dlg,
				grid,
				partbar::AvailabilityPartColour(state, flat),
				AvailabilityPartStateLabel(state));
		}
		// The two blue rows above are the ends of a continuum, not two
		// fills the bar picks between, so one more row shows the ramp
		// itself. Its endpoints are those same two rows -- read from the
		// legend order rather than from the fade, so the swatch cannot
		// illustrate a range the rows above it do not name.
		const partbar::BarColour from =
			partbar::AvailabilityPartColour(partbar::AvailabilityPartState::FewSources, flat);
		const partbar::BarColour to =
			partbar::AvailabilityPartColour(partbar::AvailabilityPartState::ManySources, flat);
		AddSwatchRow(dlg,
			grid,
			MakeGradientLegendSwatch(from, to),
			_("In between, darkening as other sources are added"));
		return;
	}

	case partbar::BarLegendKind::SharedHashing:
		for (const partbar::HashingPartState state : partbar::kHashingLegendOrder) {
			AddLegendRow(dlg,
				grid,
				partbar::HashingPartColour(state, flat),
				HashingPartStateLabel(state));
		}
		return;

	case partbar::BarLegendKind::None:
		return;
	}
}

} // namespace

wxBitmap MakeLegendSwatch(const partbar::BarColour &colour)
{
	wxBitmap bitmap(kSwatchSide, kSwatchSide);
	wxMemoryDC dc(bitmap);

	dc.SetBrush(ToMuleColour(colour).GetBrush());
	dc.DrawRectangle(0, 0, kSwatchSide, kSwatchSide);

	return bitmap;
}

wxBitmap MakeGradientLegendSwatch(const partbar::BarColour &from, const partbar::BarColour &to)
{
	wxBitmap bitmap(kSwatchSide, kSwatchSide);
	wxMemoryDC dc(bitmap);

	// wxEAST: left to right, matching the bar, which fills a file from its
	// first part on the left. No pen is set, so unlike the flat swatches this
	// one has no border -- a border would read as a fill of its own next to
	// the pale left end.
	dc.GradientFillLinear(
		wxRect(0, 0, kSwatchSide, kSwatchSide), ToMuleColour(from), ToMuleColour(to), wxEAST);

	return bitmap;
}

void AddLegendRow(wxWindow *parent, wxSizer *grid, const partbar::BarColour &colour, const wxString &label)
{
	AddSwatchRow(parent, grid, MakeLegendSwatch(colour), label);
}

void ShowPartBarLegend(wxWindow *parent, partbar::BarLegendKind kind, const wxString &columnTitle)
{
	if (kind == partbar::BarLegendKind::None) {
		return;
	}

	// Read once and captured, not asked again inside the lambda: every swatch
	// in one dialog has to answer the same preference, and the grid is filled
	// while a nested event loop is not yet running anyway.
	const bool flat = thePrefs::UseFlatBar();

	ShowInfoGridDialog(
		parent, columnTitle, LegendIntro(kind), [kind, flat](wxWindow *dlg, wxSizer *grid) {
			FillLegendGrid(dlg, grid, kind, flat);
		});
}
