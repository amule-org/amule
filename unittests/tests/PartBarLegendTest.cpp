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

// The chunk-bar palette and the legend the row context menu opens to explain
// it (issue #1192).
//
// A legend explaining colours is only worth anything while it agrees with the
// bar, and the way that agreement dies is silent: someone changes a colour in
// the renderer and the explanation, wherever it lives, keeps saying the old
// thing. PartBarLegend.h answers that by having exactly one copy of each
// colour, which both the renderer and the legend read. This suite is the other
// half: it pins the values themselves against what the bar drew before they
// were lifted out of GenericClientListCtrl.cpp, so a colour cannot be edited on
// the way past without a test saying so, and it pins the properties a legend
// needs in order to be readable at all -- one row per state the bar can draw,
// in the order the renderer decides them, no two rows the same colour.
//
// It links no wx and opens no display session, which is the point: this is the
// half of the feature that a headless CI run can actually check. The drawing
// (16x16 swatches through a wxMemoryDC) and the context-menu entry that opens
// the dialog are not reachable from here.

#include <muleunit/test.h>

#include "PartBarLegend.h"

#include <cstddef>

using namespace muleunit;
using namespace partbar;

namespace
{

//! Spelled out rather than compared to the constants in the header: a test
//! that read the palette from the same place the renderer does would pass no
//! matter what either said.
constexpr BarColour kExpectedNeither{ 240, 240, 240 };
constexpr BarColour kExpectedFlatNeither{ 224, 224, 224 };
constexpr BarColour kExpectedBoth{ 0, 192, 0 };
constexpr BarColour kExpectedFlatBoth{ 0, 150, 0 };
constexpr BarColour kExpectedClientOnly{ 104, 104, 104 };
constexpr BarColour kExpectedFlatClientOnly{ 0, 0, 0 };
constexpr BarColour kExpectedPending{ 255, 208, 0 };
constexpr BarColour kExpectedNextPending{ 255, 255, 100 };

//! Every column id, so the legend mapping can be checked exhaustively rather
//! than on the two interesting cases alone.
constexpr GenericColumnEnum kAllColumns[] = { ColumnUserName,
	ColumnUserDownloaded,
	ColumnUserUploaded,
	ColumnUserSpeedDown,
	ColumnUserSpeedUp,
	ColumnUserProgress,
	ColumnUserAvailable,
	ColumnUserVersion,
	ColumnUserQueueRankLocal,
	ColumnUserQueueRankRemote,
	ColumnUserOrigin,
	ColumnUserFileNameDownload,
	ColumnUserFileNameUpload,
	ColumnUserFileNameDownloadRemote,
	ColumnUserSharedFiles,
	ColumnInvalid };

constexpr std::size_t kAllColumnsSize = sizeof(kAllColumns) / sizeof(kAllColumns[0]);

} // namespace

DECLARE_SIMPLE(PartBarLegend)

// --- the palette itself -------------------------------------------------

TEST(PartBarLegend, SourceColoursAreTheOnesTheBarDrew)
{
	// The five shaded-bar fills, in the order GetItemBarFill() tests them.
	ASSERT_TRUE(kExpectedNeither == SourcePartColour(SourcePartState::Missing, false));
	ASSERT_TRUE(kExpectedBoth == SourcePartColour(SourcePartState::Complete, false));
	ASSERT_TRUE(kExpectedPending == SourcePartColour(SourcePartState::Downloading, false));
	ASSERT_TRUE(kExpectedNextPending == SourcePartColour(SourcePartState::NextRequested, false));
	ASSERT_TRUE(kExpectedClientOnly == SourcePartColour(SourcePartState::Needed, false));
}

TEST(PartBarLegend, SourceColoursHaveTheirOwnFlatBarValues)
{
	ASSERT_TRUE(kExpectedFlatNeither == SourcePartColour(SourcePartState::Missing, true));
	ASSERT_TRUE(kExpectedFlatBoth == SourcePartColour(SourcePartState::Complete, true));
	ASSERT_TRUE(kExpectedFlatClientOnly == SourcePartColour(SourcePartState::Needed, true));

	// The two request cues are the exception: flat and shaded draw them the
	// same, which is why neither has a crFlat* counterpart to drift from.
	ASSERT_TRUE(kExpectedPending == SourcePartColour(SourcePartState::Downloading, true));
	ASSERT_TRUE(kExpectedNextPending == SourcePartColour(SourcePartState::NextRequested, true));
}

TEST(PartBarLegend, PeerColoursAreTheOnesTheBarDrew)
{
	// The peers bar reuses the greys of the sources bar, but for its own two
	// states -- has / has not -- with no request cues in between.
	ASSERT_TRUE(kExpectedClientOnly == PeerPartColour(PeerPartState::Present, false));
	ASSERT_TRUE(kExpectedNeither == PeerPartColour(PeerPartState::Missing, false));
	ASSERT_TRUE(kExpectedFlatClientOnly == PeerPartColour(PeerPartState::Present, true));
	ASSERT_TRUE(kExpectedFlatNeither == PeerPartColour(PeerPartState::Missing, true));
}

// --- the legends --------------------------------------------------------

TEST(PartBarLegend, TheSourceLegendListsEveryStateOnceInRendererOrder)
{
	// One row per fill the sources bar can draw: a state the renderer knows
	// and the legend does not is a colour with no explanation, which is the
	// bug the legend exists to fix.
	ASSERT_EQUALS((std::size_t)5, kSourceLegendSize);
	ASSERT_TRUE(SourcePartState::Missing == kSourceLegendOrder[0]);
	ASSERT_TRUE(SourcePartState::Complete == kSourceLegendOrder[1]);
	ASSERT_TRUE(SourcePartState::Downloading == kSourceLegendOrder[2]);
	ASSERT_TRUE(SourcePartState::NextRequested == kSourceLegendOrder[3]);
	ASSERT_TRUE(SourcePartState::Needed == kSourceLegendOrder[4]);
}

TEST(PartBarLegend, ThePeerLegendListsItsTwoStates)
{
	// Two, not five: the asymmetry between the two bar columns is the reason
	// they were named apart, so collapsing them into one legend would undo
	// the change it belongs to.
	ASSERT_EQUALS((std::size_t)2, kPeerLegendSize);
	ASSERT_TRUE(PeerPartState::Present == kPeerLegendOrder[0]);
	ASSERT_TRUE(PeerPartState::Missing == kPeerLegendOrder[1]);
}

TEST(PartBarLegend, NoTwoRowsOfALegendShareAColour)
{
	// Two rows of one swatch cannot be told apart on screen, so the legend
	// would be explaining a distinction the bar does not draw. Checked under
	// both bar styles, since each has its own values.
	for (int flat = 0; flat <= 1; ++flat) {
		for (std::size_t i = 0; i < kSourceLegendSize; ++i) {
			for (std::size_t j = i + 1; j < kSourceLegendSize; ++j) {
				ASSERT_TRUE(SourcePartColour(kSourceLegendOrder[i], flat != 0) !=
					    SourcePartColour(kSourceLegendOrder[j], flat != 0));
			}
		}
		ASSERT_TRUE(PeerPartColour(kPeerLegendOrder[0], flat != 0) !=
			    PeerPartColour(kPeerLegendOrder[1], flat != 0));
	}
}

// --- which legend a column gets -----------------------------------------

TEST(PartBarLegend, OnlyTheTwoBarColumnsHaveALegend)
{
	ASSERT_TRUE(BarLegendKind::SourceParts == LegendForColumn(ColumnUserProgress));
	ASSERT_TRUE(BarLegendKind::PeerParts == LegendForColumn(ColumnUserAvailable));

	// Every other column, exhaustively: a legend offered for a text column
	// would explain a bar that is not there.
	for (std::size_t i = 0; i < kAllColumnsSize; ++i) {
		if (kAllColumns[i] == ColumnUserProgress || kAllColumns[i] == ColumnUserAvailable) {
			continue;
		}
		ASSERT_TRUE(BarLegendKind::None == LegendForColumn(kAllColumns[i]));
	}
}
