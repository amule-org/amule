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

// The known.met encoding of a "Verify Local Data" result (FT_VERIFY_CORRUPTMD4 /
// FT_VERIFY_CORRUPTAICH). The codec lives in its own header precisely so this can be tested:
// CKnownFile reaches theApp and cannot be linked into a unit test.

#include <muleunit/test.h>

#include <VerifyLocalDataResult.h>

using namespace muleunit;

DECLARE_SIMPLE(VerifyLocalDataResult)

namespace
{
// 3 full parts plus a 1-byte fourth: parts 0..3, part 3 holding a single AICH block.
const uint64 kFileSize = 3 * PARTSIZE + 1;
// Blocks in a full part: 9728000 / 184320 rounded up.
const unsigned kFullPartBlocks = (PARTSIZE + EMBLOCKSIZE - 1) / EMBLOCKSIZE;
} // namespace

TEST(VerifyLocalDataResult, CleanResultEncodesToNothing)
{
	CVerifyLocalDataResult result;
	result.date = 1234;
	ASSERT_FALSE(result.IsCorrupt());
	ASSERT_TRUE(result.EncodeCorruptedMD4().IsEmpty());
	ASSERT_TRUE(result.EncodeCorruptedAICH().IsEmpty());
}

TEST(VerifyLocalDataResult, EncodesTheDocumentedFormat)
{
	CVerifyLocalDataResult result;
	result.corruptedMD4 = { 0, 2 };
	result.corruptedAICH = { { 0, { 1, 5 } }, { 2, { 52 } } };
	ASSERT_TRUE(result.IsCorrupt());
	ASSERT_EQUALS(wxString("0,2"), result.EncodeCorruptedMD4());
	ASSERT_EQUALS(wxString("0:1.5;2:52"), result.EncodeCorruptedAICH());
}

TEST(VerifyLocalDataResult, RoundTrips)
{
	CVerifyLocalDataResult in;
	in.corruptedMD4 = { 1, 3 };
	in.corruptedAICH = { { 1, { 0, 7, (uint8)(kFullPartBlocks - 1) } }, { 3, { 0 } } };

	CVerifyLocalDataResult out;
	out.DecodeCorrupted(in.EncodeCorruptedMD4(), in.EncodeCorruptedAICH(), kFileSize);
	ASSERT_TRUE(in.corruptedMD4 == out.corruptedMD4);
	ASSERT_TRUE(in.corruptedAICH == out.corruptedAICH);
}

// A damaged or hand-edited known.met must not smuggle impossible part or block numbers in.
TEST(VerifyLocalDataResult, DropsOutOfRangeRepeatedAndMalformed)
{
	CVerifyLocalDataResult out;
	out.DecodeCorrupted("1,4,1,x,,3", "", kFileSize);
	ASSERT_TRUE((CVerifyLocalDataResult::PartList{ 1, 3 }) == out.corruptedMD4);

	// Part 4 does not exist; part 3 has only block 0; block kFullPartBlocks is one past the
	// end of a full part; a repeated part keeps its first entry; no ':' is malformed.
	out.DecodeCorrupted(
		"", wxString(CFormat("4:0;3:0.1;0:%u.2.2;0:9;junk;2:") % kFullPartBlocks), kFileSize);
	CVerifyLocalDataResult::BlockList expected = { { 3, { 0 } }, { 0, { 2 } } };
	ASSERT_TRUE(expected == out.corruptedAICH);
}

// A file of exactly n * PARTSIZE has n parts, not n + 1.
TEST(VerifyLocalDataResult, ExactMultipleOfPartSize)
{
	CVerifyLocalDataResult out;
	out.DecodeCorrupted("1,2", "1:0", 2 * PARTSIZE);
	ASSERT_TRUE((CVerifyLocalDataResult::PartList{ 1 }) == out.corruptedMD4);
	ASSERT_EQUALS(1u, (unsigned)out.corruptedAICH.size());
}

// Decoding replaces, never appends: CKnownFile reloads the same object when it copies a record.
TEST(VerifyLocalDataResult, DecodeReplacesPreviousLists)
{
	CVerifyLocalDataResult out;
	out.DecodeCorrupted("0", "0:0", kFileSize);
	out.DecodeCorrupted("", "", kFileSize);
	ASSERT_FALSE(out.IsCorrupt());
}
