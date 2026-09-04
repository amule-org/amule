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

// Where each part of a file sits in a chunk bar, and which of the two things the
// shared-files bar is drawing.
//
// Wx-free and header-only, like PartBarLegendTest: the expected values are
// spelled out rather than recomputed from the header's own formula, because a
// test that derived them the way production does would pass whatever either
// said. The drawing itself is not reachable from here and is not attempted.

#include <muleunit/test.h>

#include "PartBarSpans.h"

#include <cstddef>
#include <cstdint>

using namespace muleunit;
using namespace partbar;

namespace
{

//! PARTSIZE as the callers use it: 9500 KiB.
constexpr std::uint64_t kPartSize = 9728000ULL;

} // namespace

DECLARE_SIMPLE(PartBarSpans)

TEST(PartBarSpans, AdjacentSpansDoNotOverlap)
{
	// CBarShader::FillRange takes end as inclusive and increments it itself
	// (src/BarShader.cpp:113-124), so part i must end exactly one byte before
	// part i+1 begins. Overlapping spans paint one byte twice, and the shared
	// files bar has been doing that where the other two bars do not.
	const std::uint64_t fileSize = kPartSize * 4 + 12345;
	for (std::size_t i = 0; i + 1 < 5; ++i) {
		const Span here = SpanFor(i, kPartSize, fileSize);
		const Span next = SpanFor(i + 1, kPartSize, fileSize);
		ASSERT_EQUALS(next.start - 1, here.end);
	}
}

TEST(PartBarSpans, TheLastSpanEndsAtTheLastByte)
{
	// A file that does not divide evenly has a short last part. Its end is the
	// file's last byte, not one past it: FillRange clamps a span that runs past
	// m_FileSize, so an over-long end is silently truncated rather than
	// reported, which is why this was never noticed.
	const std::uint64_t fileSize = kPartSize * 3 + 1;
	const Span last = SpanFor(3, kPartSize, fileSize);
	ASSERT_EQUALS(fileSize - 1, last.end);
}

TEST(PartBarSpans, AFileOfExactlyOnePartCoversItWhole)
{
	const Span only = SpanFor(0, kPartSize, kPartSize);
	ASSERT_EQUALS((std::uint64_t)0, only.start);
	ASSERT_EQUALS(kPartSize - 1, only.end);
}

TEST(PartBarSpans, SpansStartWherePartSizeSaysAndNowhereElse)
{
	const std::uint64_t fileSize = kPartSize * 3;
	ASSERT_EQUALS((std::uint64_t)0, SpanFor(0, kPartSize, fileSize).start);
	ASSERT_EQUALS(kPartSize, SpanFor(1, kPartSize, fileSize).start);
	ASSERT_EQUALS(kPartSize * 2, SpanFor(2, kPartSize, fileSize).start);
}

TEST(PartBarSpans, NoPartsMeansNothingToDrawWhateverProgressSays)
{
	// The caller has no span to fill, so a hashing bar over zero parts would be
	// a bar over nothing. Progress must not override that.
	ASSERT_TRUE(BarMode::None == ModeFor(0, 0));
	ASSERT_TRUE(BarMode::None == ModeFor(7, 0));
}

TEST(PartBarSpans, ProgressAboveZeroSelectsHashingAndZeroSelectsAvailability)
{
	ASSERT_TRUE(BarMode::Availability == ModeFor(0, 9));
	ASSERT_TRUE(BarMode::Hashing == ModeFor(1, 9));
	ASSERT_TRUE(BarMode::Hashing == ModeFor(4, 9));
}

TEST(PartBarSpans, TheHashedCountIsClampedToTheParts)
{
	// CHashingTask reports part + 1 (src/ThreadTasks.cpp:179, :693), so a
	// finished pass reports one past the last part. Used unclamped as an index
	// or a count, that reads or fills one span too many.
	ASSERT_EQUALS((std::size_t)4, HashedPartsClamped(4, 9));
	ASSERT_EQUALS((std::size_t)9, HashedPartsClamped(9, 9));
	ASSERT_EQUALS((std::size_t)9, HashedPartsClamped(10, 9));
	ASSERT_EQUALS((std::size_t)0, HashedPartsClamped(0, 9));
}
