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

#ifndef PARTBARLEGENDUI_H
#define PARTBARLEGENDUI_H

// The wx side of the chunk-bar legends: the dialog the row context menus open,
// and the one conversion from the palette's plain components to CMuleColour.
//
// Separate from PartBarLegend.h on purpose. That header states which legend a
// column gets, which rows it lists, in which order and in which colour, and it
// states all of it without wx so a headless run can check it. This file draws
// what that one decided, and drawing is the part no test in this tree can look
// at. Keeping the two apart is what stops the unverifiable half from dragging
// the verifiable half out of reach.
//
// It lives here rather than in GenericClientListCtrl.cpp -- where it was, in an
// anonymous namespace -- because since #1220 the shared-files list opens legends
// too, and a second copy of a swatch is a second thing to keep in step with the
// palette.

#include <wx/bitmap.h>
#include <wx/string.h>

#include "MuleColour.h"    // Needed for CMuleColour
#include "PartBarLegend.h" // Needed for partbar::BarColour, partbar::BarLegendKind

class wxSizer;
class wxWindow;

//! The palette's components as the drawing code wants them. One conversion, so
//! a bar and the swatch explaining it cannot pick different ones.
inline CMuleColour ToMuleColour(const partbar::BarColour &colour)
{
	return CMuleColour(colour.red, colour.green, colour.blue);
}

/**
 * A 16x16 swatch of one bar colour, drawn the way CCatDialog::MakeBitmap()
 * draws the category colour: a wxMemoryDC over a wxBitmap, default pen, so the
 * fill keeps a border and the two pale greys stay visible on a light dialog.
 */
wxBitmap MakeLegendSwatch(const partbar::BarColour &colour);

/**
 * The same swatch, faded from @a from on the left to @a to on the right.
 *
 * Both endpoints are arguments and neither is known here: the availability
 * legend passes the very colours its own FewSources and ManySources rows show,
 * which are the fade's ends as PartBarLegend.h defines them and its suite pins
 * them. So there is nothing left in this function to get wrong except the
 * gradient itself, which is one toolkit call and cannot be checked without a
 * display.
 */
wxBitmap MakeGradientLegendSwatch(const partbar::BarColour &from, const partbar::BarColour &to);

//! One swatch-and-text row of a legend.
void AddLegendRow(wxWindow *parent, wxSizer *grid, const partbar::BarColour &colour, const wxString &label);

/**
 * Pops up the legend of @a kind, titled with @a columnTitle.
 *
 * Swatches are filled from partbar::SourcePartColour(), PeerPartColour(),
 * AvailabilityPartColour() and HashingPartColour() -- the same functions the
 * GetItemBarFill() implementations fill the bars themselves from, under the bar
 * preference in force right now, so a swatch cannot disagree with the pixels it
 * explains.
 *
 * BarLegendKind::None shows nothing: a caller that could not work out what its
 * cell was drawing has nothing to explain.
 */
void ShowPartBarLegend(wxWindow *parent, partbar::BarLegendKind kind, const wxString &columnTitle);

#endif // PARTBARLEGENDUI_H
