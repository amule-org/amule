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

// The two predicates behind `next_requested_part_index` and
// `downloading_part_index` on GET /{downloads,shared}/{hash}/clients.
//
// Every branch here is decided by a value amuled reports about a peer, and the
// ones that matter are the ones no live peer will hand over on request: the
// 0xffff "no block pending" sentinel, the boundary at part_count, a file with
// more than 65535 parts (~637 GB, where 0xffff is a legitimate chunk and the
// sentinel is therefore indistinguishable from real data by range alone), and a
// source whose download_state flips to "downloading". Waiting for a daemon to
// produce those is waiting forever, which is why the predicates live in a
// header of their own -- no wx, no Boost.Beast, no EC -- and are driven here.

#include <muleunit/test.h>

#include "PartIndex.h"

#include <cstdint>
#include <string>

using namespace muleunit;
using namespace webapi;

namespace
{

// A file with more than 65535 parts: 65536 * 9.28 MB is about 608 GiB, so this
// is a real size a real user reaches, not a synthetic extreme. Named because
// every "large file" case below turns on it being > kNoPartPendingSentinel.
constexpr std::uint64_t kPartCountAbove16Bit = 200000u;

} // namespace

DECLARE_SIMPLE(PartIndex)

TEST(PartIndex, SentinelIsTheValueTheCoreActuallySends)
{
	// Pinned rather than assumed: the whole point of testing the sentinel by
	// name is that the name and the wire value agree.
	ASSERT_EQUALS(0xffffu, (unsigned)kNoPartPendingSentinel);
	ASSERT_TRUE(std::string("downloading") == kDownloadStateDownloading);
}

// --- UsablePartIndex ----------------------------------------------------

TEST(PartIndex, AnUnreportedIndexIsUnusable)
{
	// The has_* flag false: the peer never sent the tag at all. Nothing else
	// in the call can rescue it -- not an in-range value, not part 0.
	ASSERT_FALSE(UsablePartIndex(false, 0, 100));
	ASSERT_FALSE(UsablePartIndex(false, 7, 100));
	ASSERT_FALSE(UsablePartIndex(false, kNoPartPendingSentinel, 100));
}

TEST(PartIndex, AnInRangeIndexIsUsable)
{
	// The positive path, including part 0 -- a real and common chunk index,
	// never a stand-in for "unknown".
	ASSERT_TRUE(UsablePartIndex(true, 0, 1));
	ASSERT_TRUE(UsablePartIndex(true, 0, 100));
	ASSERT_TRUE(UsablePartIndex(true, 42, 100));
}

TEST(PartIndex, PartCountMinusOneIsInsideAndPartCountIsNot)
{
	// The boundary. part_count is a count, the index is 0-based, so the last
	// addressable chunk is part_count - 1 and part_count itself is the first
	// value that must be refused.
	ASSERT_TRUE(UsablePartIndex(true, 9, 10));
	ASSERT_FALSE(UsablePartIndex(true, 10, 10));
	ASSERT_FALSE(UsablePartIndex(true, 11, 10));
}

TEST(PartIndex, AFileWithNoPartsAddressesNothing)
{
	// part_count 0: `0 < 0` is false, so even index 0 is refused. This is the
	// shape a row whose bitmap could not be resolved arrives in.
	ASSERT_FALSE(UsablePartIndex(true, 0, 0));
	ASSERT_FALSE(UsablePartIndex(true, 1, 0));
	ASSERT_FALSE(UsablePartIndex(true, kNoPartPendingSentinel, 0));
}

TEST(PartIndex, TheSentinelIsRefusedOnAnOrdinaryFile)
{
	// Here the bounds check would have caught it anyway (65535 >= 100). The
	// next test is the one that proves the by-name test is load-bearing.
	ASSERT_FALSE(UsablePartIndex(true, kNoPartPendingSentinel, 100));
	ASSERT_FALSE(UsablePartIndex(true, kNoPartPendingSentinel, 65535));
}

TEST(PartIndex, TheSentinelIsRefusedEvenWhere65535IsARealChunk)
{
	// The 637 GB case, and the entire reason UsablePartIndex names the
	// sentinel instead of leaving it to the bound: with 200000 parts, 65535
	// is squarely in range, so a bounds check alone would relay "no block
	// pending" as an index and paint a stripe on chunk 65535 of exactly the
	// files a per-source bar is most useful on.
	ASSERT_TRUE(65535u < kPartCountAbove16Bit); // the bound would have passed it
	ASSERT_FALSE(UsablePartIndex(true, kNoPartPendingSentinel, kPartCountAbove16Bit));

	// Only the sentinel value is special, not its neighbourhood: 65534 on the
	// same file is an ordinary chunk and must still come through.
	ASSERT_TRUE(UsablePartIndex(true, 65534, kPartCountAbove16Bit));

	// And part_count exactly 65536, the smallest file on which the sentinel is
	// in range at all -- the first size where the by-name test starts mattering.
	ASSERT_FALSE(UsablePartIndex(true, kNoPartPendingSentinel, 65536));
	ASSERT_TRUE(UsablePartIndex(true, 65534, 65536));
}

// --- UsableLastDownloadingPart ------------------------------------------

TEST(PartIndex, LastDownloadingPartNeedsTheDownloadingState)
{
	// The defect the guard exists for. The core initialises the field to 0 and
	// ships it unconditionally, so a source that is merely connected or queued
	// reports a perfectly in-range 0 -- and since most sources in a list are
	// queued, relaying it would mark chunk 0 as "downloading now" on nearly
	// every row. Only the exact state string opens the gate.
	ASSERT_TRUE(UsableLastDownloadingPart("downloading", true, 0, 100));

	ASSERT_FALSE(UsableLastDownloadingPart("onqueue", true, 0, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("connecting", true, 0, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("connected", true, 0, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("", true, 0, 100));

	// Not just the stale 0: a genuinely in-range value is still refused while
	// the peer is not transferring, because it describes a chunk that finished
	// arriving at some point in the past.
	ASSERT_FALSE(UsableLastDownloadingPart("onqueue", true, 42, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("toomanyconns", true, 42, 100));
}

TEST(PartIndex, TheStateComparisonIsExact)
{
	// The state string comes from one enum-to-token mapping (Refresher.cpp:
	// ClientDownloadStateName) and is compared verbatim, so nothing that merely
	// contains or resembles the token opens the gate.
	ASSERT_FALSE(UsableLastDownloadingPart("Downloading", true, 5, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("downloading ", true, 5, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("notdownloading", true, 5, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("downloading_from", true, 5, 100));
}

TEST(PartIndex, TheStateGuardDoesNotReplaceTheRangeChecks)
{
	// "downloading" is necessary, never sufficient: every UsablePartIndex
	// branch still applies underneath it.
	ASSERT_FALSE(UsableLastDownloadingPart("downloading", false, 5, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("downloading", true, kNoPartPendingSentinel, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("downloading", true, 100, 100));
	ASSERT_FALSE(UsableLastDownloadingPart("downloading", true, 0, 0));

	// Including on the large file, where the sentinel is in range: a
	// transferring peer does not make 0xffff mean chunk 65535.
	ASSERT_FALSE(
		UsableLastDownloadingPart("downloading", true, kNoPartPendingSentinel, kPartCountAbove16Bit));
	ASSERT_TRUE(UsableLastDownloadingPart("downloading", true, 65534, kPartCountAbove16Bit));

	// And the boundary once more through this entry point.
	ASSERT_TRUE(UsableLastDownloadingPart("downloading", true, 99, 100));
}
