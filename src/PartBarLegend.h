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

#ifndef PARTBARLEGEND_H
#define PARTBARLEGEND_H

// The palette of the two chunk-bar columns of the client lists, and the legend
// the row context menu opens to explain it.
//
// The point of this header is that there is exactly one copy of each colour.
// A legend that restated the palette -- in words, or in swatches filled from a
// second set of constants -- would be free to drift away from the pixels it
// claims to explain, and nothing would fail when it did. Here the renderer
// (CGenericClientListCtrl::GetItemBarFill) and the legend both read
// SourcePartColour()/PeerPartColour(), so a colour can only change for both at
// once.
//
// It pulls in nothing but <cstddef>/<cstdint> and ClientListColumns.h, so the
// part of the feature worth checking -- which states a legend lists, in which
// order, in which colour, and which legend a column gets -- is reachable from
// a unit test with no wx, no app and no display session. Same rationale as
// webapi/PartIndex.h. What is left needing a display is the drawing itself.

#include <cstddef>
#include <cstdint>

#include "ClientListColumns.h" // Needed for GenericColumnEnum

namespace partbar
{

//! One bar colour, as plain components. Deliberately not CMuleColour, which
//! cannot be constructed without wx; the GUI converts on use.
struct BarColour
{
	std::uint8_t red;
	std::uint8_t green;
	std::uint8_t blue;
};

constexpr bool operator==(const BarColour &a, const BarColour &b)
{
	return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

constexpr bool operator!=(const BarColour &a, const BarColour &b)
{
	return !(a == b);
}

// The palette itself. Two variants of each fill: the flat one is used when
// thePrefs::UseFlatBar() is set, which drops the gradient the shaded bar draws
// and so needs its own, more contrasty values.
constexpr BarColour kBoth{ 0, 192, 0 };
constexpr BarColour kFlatBoth{ 0, 150, 0 };

constexpr BarColour kNeither{ 240, 240, 240 };
constexpr BarColour kFlatNeither{ 224, 224, 224 };

constexpr BarColour kClientOnly{ 104, 104, 104 };
constexpr BarColour kFlatClientOnly{ 0, 0, 0 };

// The two request cues have no flat variant: they are already flat colours,
// and the shaded bar draws them unshaded too.
constexpr BarColour kPending{ 255, 208, 0 };
constexpr BarColour kNextPending{ 255, 255, 100 };

constexpr BarColour kUnavailable{ 240, 240, 240 };
constexpr BarColour kFlatUnavailable{ 224, 224, 224 };

constexpr BarColour kAvailable{ 104, 104, 104 };
constexpr BarColour kFlatAvailable{ 0, 0, 0 };

/**
 * The five fills the Sources bar (ColumnUserProgress) distinguishes, in the
 * order GetItemBarFill() tests them -- which is also the order the legend
 * lists them in, so a reader can follow one against the other.
 */
enum class SourcePartState
{
	Missing = 0,   //!< the source does not have this part
	Complete,      //!< the source has it and the file is complete here
	Downloading,   //!< being downloaded from this source right now
	NextRequested, //!< the next part that will be asked of this source
	Needed         //!< the source has it and it is still missing here
};

/**
 * The two fills the Peers bar (ColumnUserAvailable) distinguishes. A peer of
 * one of our shared files is either holding a part or not; none of the
 * request-state cues above apply, which is why the two columns cannot share
 * one legend and, since #1192, no longer share one header label either.
 */
enum class PeerPartState
{
	Present = 0, //!< this peer already has the part
	Missing      //!< this peer does not have it
};

//! Colour the Sources bar fills a part with, given its state and whether the
//! flat-bar preference is on. The renderer calls this; so does the legend.
constexpr BarColour SourcePartColour(SourcePartState state, bool flat)
{
	return state == SourcePartState::Missing         ? (flat ? kFlatNeither : kNeither)
	       : state == SourcePartState::Complete      ? (flat ? kFlatBoth : kBoth)
	       : state == SourcePartState::Downloading   ? kPending
	       : state == SourcePartState::NextRequested ? kNextPending
							 : (flat ? kFlatClientOnly : kClientOnly);
}

//! Colour the Peers bar fills a part with. Same contract as above.
constexpr BarColour PeerPartColour(PeerPartState state, bool flat)
{
	return state == PeerPartState::Present ? (flat ? kFlatAvailable : kAvailable)
					       : (flat ? kFlatUnavailable : kUnavailable);
}

//! Which legend, if any, explains a column's cells.
enum class BarLegendKind
{
	None = 0,    //!< the column draws no chunk bar
	SourceParts, //!< ColumnUserProgress, five states
	PeerParts    //!< ColumnUserAvailable, two states
};

//! The legend a column's colours are explained by. BarLegendKind::None means
//! the column draws no bar, so a list showing only such columns offers no
//! legend at all.
constexpr BarLegendKind LegendForColumn(GenericColumnEnum cid)
{
	return cid == ColumnUserProgress    ? BarLegendKind::SourceParts
	       : cid == ColumnUserAvailable ? BarLegendKind::PeerParts
					    : BarLegendKind::None;
}

//! Rows of the Sources legend, top to bottom.
constexpr SourcePartState kSourceLegendOrder[] = { SourcePartState::Missing,
	SourcePartState::Complete,
	SourcePartState::Downloading,
	SourcePartState::NextRequested,
	SourcePartState::Needed };

constexpr std::size_t kSourceLegendSize = sizeof(kSourceLegendOrder) / sizeof(kSourceLegendOrder[0]);

//! Rows of the Peers legend, top to bottom.
constexpr PeerPartState kPeerLegendOrder[] = { PeerPartState::Present, PeerPartState::Missing };

constexpr std::size_t kPeerLegendSize = sizeof(kPeerLegendOrder) / sizeof(kPeerLegendOrder[0]);

} // namespace partbar

#endif // PARTBARLEGEND_H
