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
// Since #1220 it also covers the shared-files source-availability bar, whose
// colour was a gradient the GUI and the Web UI each computed with their own
// endpoints and their own saturation point. The fade is now one function, so
// the endpoints are asserted as literals worked out by hand, and the two
// interior samples pin the rounding; the last two tests read the browser's own
// components.js and app.css and compare, because nothing generates one surface
// from the other and a hand-maintained duplicate rots quietly.
//
// It links no wx and opens no display session, which is the point: this is the
// half of the feature that a headless CI run can actually check. The drawing
// (16x16 swatches through a wxMemoryDC, the gradient swatch included) and the
// context-menu entry that opens the dialog are not reachable from here.

#include <muleunit/test.h>

#include "PartBarLegend.h"
#include "PartBarSpans.h"

#include <cstddef>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

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

//! The shared-files availability fade, likewise spelled out. These are the Web
//! UI's endpoints (--piece-avail-lo / --piece-avail, src/webapi/static/css/
//! app.css:29-30) transcribed by hand, and the two interior values are what
//! rounding the blend produces at n=5 and n=9 -- worked out independently of
//! the header's arithmetic, because a test that ran the same expression would
//! agree with a wrong one.
constexpr BarColour kExpectedZeroSources{ 255, 0, 0 };
constexpr BarColour kExpectedOneSource{ 166, 212, 238 };
constexpr BarColour kExpectedFiveSources{ 113, 181, 225 };
constexpr BarColour kExpectedNineSources{ 60, 151, 211 };
constexpr BarColour kExpectedManySources{ 47, 143, 208 };

//! The hashing legend's two fills. Written out again rather than reused from
//! kExpectedPending / kExpectedNextPending above: that the not-yet-hashed flat
//! fill is byte-identical to the sources bar's next-requested cue is the
//! collision this change resolved by naming them apart, and a test that shared
//! one literal between the two legends would be asserting they are the same
//! thing, which is what stopped being true.
constexpr BarColour kExpectedHashed{ 0, 192, 0 };
constexpr BarColour kExpectedFlatHashed{ 0, 150, 0 };
constexpr BarColour kExpectedHashPending{ 255, 208, 0 };
constexpr BarColour kExpectedFlatHashPending{ 255, 255, 100 };

//! The green the shared-files hashing bar used to draw
//! (src/SharedFilesCtrl.cpp:646). The bar adopts the palette's kBoth instead,
//! so this value must no longer appear anywhere in the legend.
constexpr BarColour kRetiredHashingGreen{ 0, 224, 0 };

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

//! The shared-files list's own column ids, as literals. They are plain
//! #defines (src/SharedFilesCtrl.h:31-46) in a header that needs wx, and they
//! deliberately never enter PartBarLegend.h -- putting them there would undo
//! the separation the typed key exists for. Stated here so the two id spaces
//! can be exercised against each other.
constexpr int kAllSharedFilesColumns[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };

constexpr std::size_t kAllSharedFilesColumnsSize =
	sizeof(kAllSharedFilesColumns) / sizeof(kAllSharedFilesColumns[0]);

//! COLUMN_SHARED_PART, the shared-files bar column. Named because the point of
//! the exhaustive test below is what happens to this particular number.
constexpr int kSharedFilesBarColumnId = 8;

// SRCDIR arrives as an unquoted path; stringize it without wx, since nothing
// else in this suite needs a wxString for a filename.
#define PARTBAR_STRINGIZE_(x) #x
#define PARTBAR_STRINGIZE(x) PARTBAR_STRINGIZE_(x)

//! The Web UI's sources, relative to this directory. Two different files, so
//! the comparison below is not a restatement of either side's assumption.
const std::string kWebUiJs =
	std::string(PARTBAR_STRINGIZE(SRCDIR)) + "/../../src/webapi/static/js/components.js";
const std::string kWebUiCss = std::string(PARTBAR_STRINGIZE(SRCDIR)) + "/../../src/webapi/static/css/app.css";

//! Whole file as text, or empty if it could not be opened -- which the caller
//! reports as a failure naming the path, because a pin that silently passes
//! when it cannot find the other surface is worse than no pin.
std::string ReadWholeFile(const std::string &path)
{
	std::ifstream in(path.c_str());
	if (!in) {
		return std::string();
	}
	std::ostringstream all;
	all << in.rdbuf();
	return all.str();
}

//! First capture of @p pattern in @p text, or empty if it does not match.
std::string FirstCapture(const std::string &text, const std::string &pattern)
{
	std::smatch found;
	const std::regex expression(pattern);
	if (!std::regex_search(text, found, expression) || found.size() < 2) {
		return std::string();
	}
	return found[1].str();
}

//! "a6d4ee" -> (166,212,238). Six digits only; the Web UI writes them long.
BarColour ColourFromHex(const std::string &hex)
{
	const unsigned long packed = std::stoul(hex, nullptr, 16);
	return BarColour{ static_cast<std::uint8_t>((packed >> 16) & 0xFF),
		static_cast<std::uint8_t>((packed >> 8) & 0xFF),
		static_cast<std::uint8_t>(packed & 0xFF) };
}

//! Compare channel by channel, so a failure names the channel and the two
//! numbers instead of only saying the colours differ.
void AssertColourEquals(const BarColour &expected, const BarColour &actual)
{
	ASSERT_EQUALS((int)expected.red, (int)actual.red);
	ASSERT_EQUALS((int)expected.green, (int)actual.green);
	ASSERT_EQUALS((int)expected.blue, (int)actual.blue);
}

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

// --- the shared-files availability fade ---------------------------------

TEST(PartBarLegend, TheFadeRunsBetweenTheWebUiEndpoints)
{
	// One definition serves the shared-files list, the downloads list and the
	// Web UI, so the endpoints have to be one set of numbers. Decision 3 picks
	// the Web UI's: the GUI's old (0,210,255) -> (0,0,255) does not survive.
	CONTEXT("one source");
	AssertColourEquals(kExpectedOneSource, SourceAvailabilityColour(1));
}

TEST(PartBarLegend, TheFadeRoundsItsInteriorStepsToTheseValues)
{
	// Halfway and one step short of saturation. Both are exact: the blend has
	// a denominator of nine, so no channel can land on .5 and every rounding
	// mode -- Math.round in components.js included -- gives these numbers.
	{
		CONTEXT("five sources");
		AssertColourEquals(kExpectedFiveSources, SourceAvailabilityColour(5));
	}
	{
		CONTEXT("nine sources");
		AssertColourEquals(kExpectedNineSources, SourceAvailabilityColour(9));
	}
}

TEST(PartBarLegend, TheFadeSaturatesAtTenSources)
{
	// Ten, not the GUI's former eleven: AVAIL_FULL in
	// src/webapi/static/js/components.js:389 is what a user comparing the two
	// surfaces sees, and one more source past saturation must not darken the
	// part further.
	{
		CONTEXT("ten sources");
		AssertColourEquals(kExpectedManySources, SourceAvailabilityColour(10));
	}
	{
		CONTEXT("eleven sources");
		AssertColourEquals(kExpectedManySources, SourceAvailabilityColour(11));
	}
	{
		CONTEXT("a thousand sources");
		AssertColourEquals(kExpectedManySources, SourceAvailabilityColour(1000));
	}
}

TEST(PartBarLegend, TheFadeNeverBrightensAsSourcesAreAdded)
{
	// More sources must never read as less availability. Checked as a property
	// across the whole range rather than at the sampled points, because a sign
	// slip in one channel is invisible at the endpoints.
	for (unsigned n = 1; n < 40; ++n) {
		CONTEXT(wxString::Format("from %u to %u sources", n, n + 1));
		const BarColour here = SourceAvailabilityColour(n);
		const BarColour next = SourceAvailabilityColour(n + 1);
		ASSERT_TRUE(next.red <= here.red);
		ASSERT_TRUE(next.green <= here.green);
		ASSERT_TRUE(next.blue <= here.blue);
	}
}

TEST(PartBarLegend, ZeroSourcesIsItsOwnStateAndNotTheFadesDarkEnd)
{
	// A part nobody holds is a different fact from a part one peer holds, and
	// the bar says so in red. Putting it on the fade would make "unavailable"
	// the darkest shade of "available".
	AssertColourEquals(kExpectedZeroSources, kZeroSources);
	ASSERT_TRUE(kZeroSources != SourceAvailabilityColour(1));
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

TEST(PartBarLegend, TheAvailabilityLegendListsItsThreeStatesInOrder)
{
	// Nobody has it, a few have it, many have it. Three rows, because the red
	// is a different fact rather than the dark end of the blue.
	ASSERT_EQUALS((std::size_t)3, kAvailabilityLegendSize);
	ASSERT_TRUE(AvailabilityPartState::ZeroSources == kAvailabilityLegendOrder[0]);
	ASSERT_TRUE(AvailabilityPartState::FewSources == kAvailabilityLegendOrder[1]);
	ASSERT_TRUE(AvailabilityPartState::ManySources == kAvailabilityLegendOrder[2]);
}

TEST(PartBarLegend, TheHashingLegendListsItsTwoStatesInOrder)
{
	ASSERT_EQUALS((std::size_t)2, kHashingLegendSize);
	ASSERT_TRUE(HashingPartState::Hashed == kHashingLegendOrder[0]);
	ASSERT_TRUE(HashingPartState::NotYetHashed == kHashingLegendOrder[1]);
}

TEST(PartBarLegend, TheAvailabilityLegendRowsAreTheFadesOwnColours)
{
	// The legend's two blue rows are the fade's endpoints, read from the same
	// function the renderer calls, so a changed endpoint moves both or
	// neither. The literals are asserted separately, above and below.
	{
		CONTEXT("few sources row");
		AssertColourEquals(SourceAvailabilityColour(1),
			AvailabilityPartColour(AvailabilityPartState::FewSources, false));
	}
	{
		CONTEXT("many sources row");
		AssertColourEquals(SourceAvailabilityColour(kAvailFull),
			AvailabilityPartColour(AvailabilityPartState::ManySources, false));
	}
}

TEST(PartBarLegend, TheAvailabilityLegendRowsAreTheseValues)
{
	{
		CONTEXT("zero sources row");
		AssertColourEquals(kExpectedZeroSources,
			AvailabilityPartColour(AvailabilityPartState::ZeroSources, false));
	}
	{
		CONTEXT("few sources row");
		AssertColourEquals(
			kExpectedOneSource, AvailabilityPartColour(AvailabilityPartState::FewSources, false));
	}
	{
		CONTEXT("many sources row");
		AssertColourEquals(kExpectedManySources,
			AvailabilityPartColour(AvailabilityPartState::ManySources, false));
	}
}

TEST(PartBarLegend, TheAvailabilityLegendIgnoresTheFlatBarPreference)
{
	// The renderer never consulted UseFlatBar() for this bar
	// (src/SharedFilesCtrl.cpp:658-678 takes bFlat and does not use it), so
	// the legend must not invent a second set of values for a distinction the
	// bar does not draw.
	for (std::size_t i = 0; i < kAvailabilityLegendSize; ++i) {
		CONTEXT(wxString::Format("availability row %zu", i));
		AssertColourEquals(AvailabilityPartColour(kAvailabilityLegendOrder[i], false),
			AvailabilityPartColour(kAvailabilityLegendOrder[i], true));
	}
}

TEST(PartBarLegend, TheHashingLegendColoursAreTheOnesTheBarDraws)
{
	AssertColourEquals(kExpectedHashed, HashingPartColour(HashingPartState::Hashed, false));
	AssertColourEquals(kExpectedFlatHashed, HashingPartColour(HashingPartState::Hashed, true));
	AssertColourEquals(kExpectedHashPending, HashingPartColour(HashingPartState::NotYetHashed, false));
	AssertColourEquals(kExpectedFlatHashPending, HashingPartColour(HashingPartState::NotYetHashed, true));
}

TEST(PartBarLegend, TheHashingBarGaveUpItsPrivateGreen)
{
	// It carried its own (0,224,0) next to the palette's (0,192,0) for no
	// stated reason. Adopting the palette's darkens the hashing bar slightly,
	// which is deliberate: two greens meaning "have it" is the drift this
	// header exists to remove.
	ASSERT_TRUE(kRetiredHashingGreen != HashingPartColour(HashingPartState::Hashed, false));
	ASSERT_TRUE(kRetiredHashingGreen != HashingPartColour(HashingPartState::Hashed, true));
}

TEST(PartBarLegend, NoTwoRowsOfALegendShareAColour)
{
	// Two rows of one swatch cannot be told apart on screen, so the legend
	// would be explaining a distinction the bar does not draw. Checked under
	// both bar styles, since each has its own values.
	//
	// Scoped to one legend at a time, and that is not laziness: the hashing
	// legend's flat not-yet-hashed fill and the sources legend's
	// next-requested cue are both (255,255,100) and mean different things. A
	// global uniqueness check would fail on two colours that never appear side
	// by side, and the only way to satisfy it would be to repaint a bar for
	// the sake of a legend it is not in.
	for (int flat = 0; flat <= 1; ++flat) {
		for (std::size_t i = 0; i < kSourceLegendSize; ++i) {
			for (std::size_t j = i + 1; j < kSourceLegendSize; ++j) {
				ASSERT_TRUE(SourcePartColour(kSourceLegendOrder[i], flat != 0) !=
					    SourcePartColour(kSourceLegendOrder[j], flat != 0));
			}
		}
		ASSERT_TRUE(PeerPartColour(kPeerLegendOrder[0], flat != 0) !=
			    PeerPartColour(kPeerLegendOrder[1], flat != 0));

		for (std::size_t i = 0; i < kAvailabilityLegendSize; ++i) {
			for (std::size_t j = i + 1; j < kAvailabilityLegendSize; ++j) {
				ASSERT_TRUE(AvailabilityPartColour(kAvailabilityLegendOrder[i], flat != 0) !=
					    AvailabilityPartColour(kAvailabilityLegendOrder[j], flat != 0));
			}
		}
		ASSERT_TRUE(HashingPartColour(kHashingLegendOrder[0], flat != 0) !=
			    HashingPartColour(kHashingLegendOrder[1], flat != 0));
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

TEST(PartBarLegend, TheSharedFilesBarColumnGetsALegendPerMode)
{
	// One column, two legends: the cell draws availability most of the time
	// and re-hash progress while a hash is running, and they share nothing.
	// Which one to show is decided by the row's mode, not by the column.
	ASSERT_TRUE(BarLegendKind::SharedAvailability ==
		    LegendForColumn(SharedFilesBarColumn::SourceAvailability, BarMode::Availability));
	ASSERT_TRUE(BarLegendKind::SharedHashing ==
		    LegendForColumn(SharedFilesBarColumn::SourceAvailability, BarMode::Hashing));

	// A row with no parts draws no bar, so there is nothing to explain.
	ASSERT_TRUE(BarLegendKind::None ==
		    LegendForColumn(SharedFilesBarColumn::SourceAvailability, BarMode::None));
}

TEST(PartBarLegend, ASharedFilesRowResolvesItsLegendFromTwoIntegers)
{
	// What the row context menu has to decide when it is clicked: the file
	// gives a hashed-part count and a part count, and the legend follows from
	// those two numbers alone. Composed once here rather than at the call site
	// so the composition is checkable without a display -- opening the dialog
	// is not, but choosing which one to open is.
	//
	// Expected values are the mode table of the spec, not a second call to
	// ModeFor(): if the composition were wired the wrong way round, reading the
	// answer back out of the same expression would agree with it.
	ASSERT_TRUE(BarLegendKind::SharedAvailability == LegendForSharedFilesRow(0, 9));
	ASSERT_TRUE(BarLegendKind::SharedHashing == LegendForSharedFilesRow(1, 9));
	ASSERT_TRUE(BarLegendKind::SharedHashing == LegendForSharedFilesRow(9, 9));

	// A count past the end of the file still means a hash is running:
	// CHashingTask reports part + 1, so a finished pass reports one too many.
	ASSERT_TRUE(BarLegendKind::SharedHashing == LegendForSharedFilesRow(9, 4));

	// No parts, no bar, no legend -- for every hashed count, including one
	// that claims progress through a file with nothing in it.
	ASSERT_TRUE(BarLegendKind::None == LegendForSharedFilesRow(0, 0));
	ASSERT_TRUE(BarLegendKind::None == LegendForSharedFilesRow(1, 0));
	ASSERT_TRUE(BarLegendKind::None == LegendForSharedFilesRow(9, 0));
}

TEST(PartBarLegend, TheClientListLegendsAreUnaffectedByTheSharedFilesModes)
{
	// The mode argument belongs to the shared-files overload alone. The two
	// client-list bar columns have one legend each and keep answering the
	// single-argument call, so nothing already in the tree had to change.
	ASSERT_TRUE(BarLegendKind::SourceParts == LegendForColumn(ColumnUserProgress));
	ASSERT_TRUE(BarLegendKind::PeerParts == LegendForColumn(ColumnUserAvailable));
	ASSERT_TRUE(BarLegendKind::SharedAvailability != BarLegendKind::SourceParts);
	ASSERT_TRUE(BarLegendKind::SharedHashing != BarLegendKind::PeerParts);
}

TEST(PartBarLegend, ACastFromTheSharedFilesIdSpaceReachesTheWrongLegend)
{
	// The two id spaces overlap. COLUMN_SHARED_AREQ is 5 and so is
	// ColumnUserProgress; COLUMN_SHARED_TRA is 6 and so is
	// ColumnUserAvailable; COLUMN_SHARED_PART -- the bar column, the one that
	// actually has a legend -- is 8, which is ColumnUserQueueRankLocal and has
	// none.
	//
	// The distinct enum types stop this by accident, but a deliberate
	// static_cast still compiles, and this is what it gets: a legend for two
	// text columns and no legend for the bar. Pinned rather than argued about,
	// so that if the id spaces are ever merged the numbers here fail loudly.
	for (std::size_t i = 0; i < kAllSharedFilesColumnsSize; ++i) {
		const int id = kAllSharedFilesColumns[i];
		CONTEXT(wxString::Format("shared-files column id %d cast across", id));

		const BarLegendKind wrong = LegendForColumn(static_cast<GenericColumnEnum>(id));
		if (id == 5) {
			ASSERT_TRUE(BarLegendKind::SourceParts == wrong);
		} else if (id == 6) {
			ASSERT_TRUE(BarLegendKind::PeerParts == wrong);
		} else {
			ASSERT_TRUE(BarLegendKind::None == wrong);
		}
	}

	// Including the bar column itself, which is the whole problem.
	ASSERT_TRUE(BarLegendKind::None ==
		    LegendForColumn(static_cast<GenericColumnEnum>(kSharedFilesBarColumnId)));

	// The typed call is the one that works.
	ASSERT_TRUE(BarLegendKind::SharedAvailability ==
		    LegendForColumn(SharedFilesBarColumn::SourceAvailability, BarMode::Availability));
}

// --- the other surface --------------------------------------------------

TEST(PartBarLegend, TheWebUiSaturatesAtTheSameSourceCount)
{
	// Nothing generates one side from the other: there is no JS build step
	// under src/webapi/static/, so AVAIL_FULL and kAvailFull are two numbers
	// maintained by hand. That is the honest description of the arrangement,
	// and this is what stops it rotting quietly -- the test reads the
	// browser's own source and fails naming both files.
	const std::string js = ReadWholeFile(kWebUiJs);
	ASSERT_TRUE_M(!js.empty(),
		wxString("Could not read ") + kWebUiJs +
			" -- the pin between partbar::kAvailFull and the Web UI's AVAIL_FULL "
			"cannot be checked, so it is failing rather than passing silently");

	const std::string found = FirstCapture(js, "AVAIL_FULL\\s*=\\s*([0-9]+)");
	ASSERT_TRUE_M(!found.empty(),
		wxString("No AVAIL_FULL assignment in ") + kWebUiJs +
			" -- it was renamed or reformatted; src/PartBarLegend.h and this test "
			"both need updating with it");

	ASSERT_EQUALS((int)kAvailFull, std::stoi(found));
}

TEST(PartBarLegend, TheWebUiFadesBetweenTheSameTwoColours)
{
	// The endpoints live in CSS custom properties, so a theme edit is the
	// realistic way they drift. Read them from app.css and compare to the two
	// constants the GUI fade is built from.
	const std::string css = ReadWholeFile(kWebUiCss);
	ASSERT_TRUE_M(!css.empty(),
		wxString("Could not read ") + kWebUiCss +
			" -- the pin between the GUI fade endpoints and --piece-avail-lo / "
			"--piece-avail cannot be checked");

	const std::string lo = FirstCapture(css, "--piece-avail-lo\\s*:\\s*#([0-9a-fA-F]{6})");
	const std::string hi = FirstCapture(css, "--piece-avail\\s*:\\s*#([0-9a-fA-F]{6})");
	ASSERT_TRUE_M(!lo.empty() && !hi.empty(),
		wxString("--piece-avail-lo and --piece-avail are not both six-digit hex in ") + kWebUiCss +
			" -- src/PartBarLegend.h's kAvailFew/kAvailMany are pinned to them");

	{
		CONTEXT("--piece-avail-lo against kAvailFew");
		AssertColourEquals(ColourFromHex(lo), SourceAvailabilityColour(1));
	}
	{
		CONTEXT("--piece-avail against kAvailMany");
		AssertColourEquals(ColourFromHex(hi), SourceAvailabilityColour(kAvailFull));
	}
}
