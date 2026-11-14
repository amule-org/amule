// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <KadKeywordPublishTimes.h>
#include <MemFile.h>
#include <common/DataFileVersion.h>
#include <protocol/kad/Constants.h>

using namespace muleunit;
using Kademlia::CUInt128;
using KadKeywordPublishTimes::Times;

DECLARE_SIMPLE(KadKeywordPublishTimes)

static const uint32_t kNow = 1000000;

TEST(KadKeywordPublishTimes, RoundTripKeepsOnlyFutureTimes)
{
	Times written;
	written[CUInt128(1u)] = kNow + 60;
	written[CUInt128(2u)] = kNow;                                 // Due now: published as if never saved.
	written[CUInt128(3u)] = kNow - 60;                            // Overdue.
	written[CUInt128(4u)] = kNow + KADEMLIAREPUBLISHTIMEK + 3600; // Clock set back since.
	CMemFile file;
	KadKeywordPublishTimes::Write(file, written);

	file.Seek(0);
	Times read;
	ASSERT_TRUE(KadKeywordPublishTimes::Read(file, kNow, read));
	ASSERT_EQUALS(2u, read.size());
	ASSERT_EQUALS(kNow + 60, read[CUInt128(1u)]);
	ASSERT_EQUALS(kNow + KADEMLIAREPUBLISHTIMEK, read[CUInt128(4u)]);
}

TEST(KadKeywordPublishTimes, DamagedFileKeepsWhatWasRead)
{
	Times written;
	written[CUInt128(1u)] = kNow + 60;
	written[CUInt128(2u)] = kNow + 120;
	CMemFile full;
	KadKeywordPublishTimes::Write(full, written);
	// Cut inside the second entry.
	CMemFile cut(full.GetRawBuffer(), full.GetLength() - 2);

	Times read;
	ASSERT_FALSE(KadKeywordPublishTimes::Read(cut, kNow, read));
	ASSERT_EQUALS(1u, read.size());
	ASSERT_EQUALS(kNow + 60, read[CUInt128(1u)]);
}

TEST(KadKeywordPublishTimes, UnknownVersionReadsNothing)
{
	CMemFile file;
	file.WriteUInt8(KADKEYWORDS_VERSION + 1);
	file.WriteUInt32(0);
	file.Seek(0);
	Times read;
	ASSERT_FALSE(KadKeywordPublishTimes::Read(file, kNow, read));
	ASSERT_TRUE(read.empty());
}
