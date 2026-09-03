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

// The palette of the chunk-bar columns -- the two in the client lists and the
// source-availability one in the shared-files list -- and the legends the row
// context menu opens to explain them.
//
// The point of this header is that there is exactly one copy of each colour.
// A legend that restated the palette -- in words, or in swatches filled from a
// second set of constants -- would be free to drift away from the pixels it
// claims to explain, and nothing would fail when it did. Here the renderer
// (CGenericClientListCtrl::GetItemBarFill) and the legend both read
// SourcePartColour()/PeerPartColour(), so a colour can only change for both at
// once.
//
// "One copy of each colour" is a rule about definitions, not about values. Two
// constants may hold the same RGB when they mean different things and never
// appear in the same legend -- kNextPending and kFlatHashPending do -- and
// collapsing them would tie a shared-files bar to a client-list cue for no
// reason. So the no-two-rows-alike invariant is scoped PER LEGEND: within one
// legend a repeated colour is a distinction the reader cannot see, which is a
// real defect; across two legends it is a coincidence.
//
// It pulls in nothing but <cstddef>/<cstdint>, ClientListColumns.h and
// PartBarSpans.h -- itself wx-free -- so the part of the feature worth checking
// -- which states a legend lists, in which order, in which colour, and which
// legend a column gets -- is reachable from a unit test with no wx, no app and
// no display session. Same rationale as webapi/PartIndex.h. What is left
// needing a display is the drawing itself.

#include <cstddef>
#include <cstdint>

#include "ClientListColumns.h" // Needed for GenericColumnEnum
#include "PartBarSpans.h"      // Needed for BarMode

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

// The part of a shared file not yet re-hashed, drawn flat. Byte-identical to
// kNextPending and deliberately a separate constant: one is a request cue in a
// client list, the other is unread bytes of a local file, and the only thing
// they have in common is the shade someone picked. Merging them would mean a
// change to either bar silently moved the other.
constexpr BarColour kFlatHashPending{ 255, 255, 100 };

// The shared-files availability bar. A part no source holds is its own state,
// not the dark end of the fade, so it keeps its own constant.
constexpr BarColour kZeroSources{ 255, 0, 0 };

//! Source count at which the fade reaches its dark end. Adding an eleventh
//! source darkens nothing. This is AVAIL_FULL in
//! src/webapi/static/js/components.js, and the two surfaces have to agree on it
//! or the same file looks differently shared in the GUI and in a browser.
constexpr unsigned kAvailFull = 10;

//! The endpoints of the fade: one source, and kAvailFull or more. Taken from
//! the Web UI's --piece-avail-lo / --piece-avail (src/webapi/static/css/
//! app.css:29-30), which is the pair that survives; the GUI's former
//! (0,210,255) -> (0,0,255) does not.
constexpr BarColour kAvailFew{ 166, 212, 238 };
constexpr BarColour kAvailMany{ 47, 143, 208 };

//! Steps between the two endpoints, so kAvailFull is stated once.
constexpr int kAvailFadeSteps = static_cast<int>(kAvailFull) - 1;

/**
 * One channel of the blend, @p k steps of kAvailFadeSteps from @p lo to @p hi.
 *
 * round(lo + (hi - lo) * k / 9) without floating point: doubling numerator and
 * denominator and adding half the denominator turns C++ truncating division
 * into rounding. The numerator's minimum is hi * 9, which is positive, so the
 * truncation is a floor and the identity holds.
 *
 * Nothing here can land on a half. lo * 9 + (hi - lo) * k over a denominator of
 * 9 has a fractional part in ninths, and 1/2 is not one of them, so the
 * rounding mode never matters -- Math.round in components.js:404-407,
 * std::lround and banker's rounding all produce these same numbers. That is why
 * the browser and the GUI agree by construction rather than by convention.
 */
constexpr std::uint8_t AvailabilityFadeChannel(int lo, int hi, int k)
{
	return static_cast<std::uint8_t>(
		(2 * (lo * kAvailFadeSteps + (hi - lo) * k) + kAvailFadeSteps) / (2 * kAvailFadeSteps));
}

//! How far along the fade @p sources sits, saturating at kAvailFull.
constexpr int AvailabilityFadeStep(unsigned sources)
{
	return sources >= kAvailFull ? kAvailFadeSteps : static_cast<int>(sources) - 1;
}

/**
 * Bar colour for a part @p sources peers hold, for @p sources >= 1.
 *
 * Takes the source count and nothing else. That arity is deliberate: it is what
 * stops the shared-files list and the downloads list from drifting apart again,
 * because neither can pass its own endpoints without adding a parameter, and a
 * changed signature is something a reviewer sees.
 *
 * @p sources == 0 is not on this fade at all -- see kZeroSources.
 */
constexpr BarColour SourceAvailabilityColour(unsigned sources)
{
	return BarColour{ AvailabilityFadeChannel(
				  kAvailFew.red, kAvailMany.red, AvailabilityFadeStep(sources)),
		AvailabilityFadeChannel(kAvailFew.green, kAvailMany.green, AvailabilityFadeStep(sources)),
		AvailabilityFadeChannel(kAvailFew.blue, kAvailMany.blue, AvailabilityFadeStep(sources)) };
}

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

/**
 * What the source-availability bar can say about one part, as its legend lists
 * it. The two blue rows are the ends of a continuum rather than two discrete
 * fills, so the legend explains a gradient by naming where it starts and where
 * it stops; the red is not on that gradient at all.
 */
enum class AvailabilityPartState
{
	ZeroSources = 0, //!< no source holds this part
	FewSources,      //!< one source: the light end of the fade
	ManySources      //!< kAvailFull or more: the dark end
};

/**
 * What the same cell says while a re-hash is running. Unrelated to the above:
 * availability is a fact about the swarm, this is progress through local data
 * that happens to occupy the same column.
 */
enum class HashingPartState
{
	Hashed = 0,  //!< already read back and hashed
	NotYetHashed //!< still to be read
};

/**
 * Colour the source-availability bar fills a part with.
 *
 * @p flat is accepted and ignored, on purpose. The renderer took the flat-bar
 * preference and never consulted it for this bar, so inventing a second set of
 * values here would have the legend explaining a distinction the bar does not
 * draw. The parameter stays for the shape it shares with its siblings, and the
 * suite asserts both arguments give the same colour.
 */
constexpr BarColour AvailabilityPartColour(AvailabilityPartState state, bool)
{
	return state == AvailabilityPartState::ZeroSources  ? kZeroSources
	       : state == AvailabilityPartState::FewSources ? SourceAvailabilityColour(1)
							    : SourceAvailabilityColour(kAvailFull);
}

//! Colour the hashing bar fills a part with. Hashed reuses the palette's green
//! rather than the private one the shared-files bar used to carry.
constexpr BarColour HashingPartColour(HashingPartState state, bool flat)
{
	return state == HashingPartState::Hashed ? (flat ? kFlatBoth : kBoth)
						 : (flat ? kFlatHashPending : kPending);
}

//! Which legend, if any, explains a column's cells.
enum class BarLegendKind
{
	None = 0,           //!< the column draws no chunk bar
	SourceParts,        //!< ColumnUserProgress, five states
	PeerParts,          //!< ColumnUserAvailable, two states
	SharedAvailability, //!< the shared-files bar, source counts
	SharedHashing       //!< the same cell, re-hash progress
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

/**
 * The shared-files list's bar column, as a type of its own.
 *
 * That list identifies its columns with plain #defines
 * (src/SharedFilesCtrl.h:31-46) whose numbers overlap GenericColumnEnum's:
 * COLUMN_SHARED_PART is 8, which is also ColumnUserQueueRankLocal. Passing one
 * where the other is wanted therefore compiled, silently, and returned a legend
 * for the wrong thing. A distinct scoped enum has no conversion to the other, so
 * that call is now a compile error -- and none of the existing call sites had to
 * change, because this is an overload rather than a replacement.
 *
 * A deliberate static_cast still gets through; types cannot stop that, and the
 * suite pins what it produces instead.
 */
enum class SharedFilesBarColumn
{
	SourceAvailability //!< COLUMN_SHARED_PART, the only bar column in that list
};

/**
 * The legend explaining the shared-files bar, given what that row's bar is
 * drawing.
 *
 * One column, two legends. The cell shows source availability most of the time
 * and re-hash progress while a hash runs, and neither explains the other, so
 * the mode has to reach the selector. It arrives as a value -- ModeFor() in
 * PartBarSpans.h turns two integers into it -- which keeps this header free of
 * row state and the whole thing constexpr.
 */
constexpr BarLegendKind LegendForColumn(SharedFilesBarColumn column, BarMode mode)
{
	return column != SharedFilesBarColumn::SourceAvailability ? BarLegendKind::None
	       : mode == BarMode::Availability                    ? BarLegendKind::SharedAvailability
	       : mode == BarMode::Hashing                         ? BarLegendKind::SharedHashing
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

//! Rows of the source-availability legend, top to bottom: the distinct state
//! first, then the fade from its light end to its dark one.
constexpr AvailabilityPartState kAvailabilityLegendOrder[] = { AvailabilityPartState::ZeroSources,
	AvailabilityPartState::FewSources,
	AvailabilityPartState::ManySources };

constexpr std::size_t kAvailabilityLegendSize =
	sizeof(kAvailabilityLegendOrder) / sizeof(kAvailabilityLegendOrder[0]);

//! Rows of the hashing legend, top to bottom.
constexpr HashingPartState kHashingLegendOrder[] = { HashingPartState::Hashed,
	HashingPartState::NotYetHashed };

constexpr std::size_t kHashingLegendSize = sizeof(kHashingLegendOrder) / sizeof(kHashingLegendOrder[0]);

} // namespace partbar

#endif // PARTBARLEGEND_H
