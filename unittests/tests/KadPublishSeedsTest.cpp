// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <KadPublishSeeds.h>
#include <MemFile.h>
#include <common/DataFileVersion.h>

using namespace muleunit;
using Kademlia::CKadUDPKey;
using Kademlia::CUInt128;
using KadPublishSeeds::Entry;
using KadPublishSeeds::Seed;
using KadPublishSeeds::Seeds;

DECLARE_SIMPLE(KadPublishSeeds)

static const uint32_t kNow = 1000000;

static Seed MakeSeed(uint32_t n)
{
	Seed seed;
	seed.id = CUInt128(n);
	seed.ip = 0x0A000000 + n;
	seed.udpPort = 4672;
	seed.tcpPort = 4662;
	seed.version = 9;
	seed.key = CKadUDPKey(0x1234 + n, 0x0B000001);
	seed.verified = n % 2;
	return seed;
}

static Entry MakeEntry(uint32_t saved, uint32_t count)
{
	Entry entry;
	entry.saved = saved;
	for (uint32_t n = 1; n <= count; ++n) {
		entry.seeds.push_back(MakeSeed(n));
	}
	return entry;
}

TEST(KadPublishSeeds, RoundTripKeepsEverySeedField)
{
	Seeds written;
	written[CUInt128(1u)] = MakeEntry(kNow - 60, 3);
	CMemFile file;
	KadPublishSeeds::Write(file, written, kNow);

	file.Seek(0);
	Seeds read;
	ASSERT_TRUE(KadPublishSeeds::Read(file, kNow, read));
	ASSERT_EQUALS(1u, read.size());
	const Entry &entry = read[CUInt128(1u)];
	ASSERT_EQUALS(kNow - 60, entry.saved);
	ASSERT_EQUALS(3u, entry.seeds.size());
	for (uint32_t n = 1; n <= 3; ++n) {
		const Seed &got = entry.seeds[n - 1];
		const Seed want = MakeSeed(n);
		ASSERT_TRUE(got.id == want.id);
		ASSERT_EQUALS(want.ip, got.ip);
		ASSERT_EQUALS(want.udpPort, got.udpPort);
		ASSERT_EQUALS(want.tcpPort, got.tcpPort);
		ASSERT_EQUALS(want.version, got.version);
		ASSERT_EQUALS(want.key.GetKeyValue(0x0B000001), got.key.GetKeyValue(0x0B000001));
		ASSERT_EQUALS(want.verified, got.verified);
	}
}

TEST(KadPublishSeeds, OnlyCurrentEntriesSurvive)
{
	Seeds written;
	written[CUInt128(1u)] = MakeEntry(kNow, 1);
	written[CUInt128(2u)] = MakeEntry(kNow - KadPublishSeeds::MaxAge, 1);
	written[CUInt128(3u)] = MakeEntry(kNow - KadPublishSeeds::MaxAge - 1, 1); // Too old.
	written[CUInt128(4u)] = MakeEntry(kNow + 60, 1);                          // Clock set back since.
	CMemFile file;
	KadPublishSeeds::Write(file, written, kNow);

	// Ages again between the save and the next start.
	file.Seek(0);
	Seeds read;
	ASSERT_TRUE(KadPublishSeeds::Read(file, kNow + 1, read));
	ASSERT_EQUALS(1u, read.size());
	ASSERT_EQUALS(1u, read.count(CUInt128(1u)));
}

TEST(KadPublishSeeds, WritesAtMostMaxSeeds)
{
	Seeds written;
	written[CUInt128(1u)] = MakeEntry(kNow, KadPublishSeeds::MaxSeeds + 5);
	CMemFile file;
	KadPublishSeeds::Write(file, written, kNow);

	file.Seek(0);
	Seeds read;
	ASSERT_TRUE(KadPublishSeeds::Read(file, kNow, read));
	ASSERT_EQUALS(KadPublishSeeds::MaxSeeds, read[CUInt128(1u)].seeds.size());
}

TEST(KadPublishSeeds, ReadDropsSeedsWithoutAnAddressAndCaps)
{
	CMemFile file;
	file.WriteUInt8(KADSEEDS_VERSION);
	file.WriteUInt32(1);
	file.WriteUInt128(CUInt128(1u));
	file.WriteUInt32(kNow);
	file.WriteUInt8(KadPublishSeeds::MaxSeeds + 5);
	for (uint32_t n = 0; n < KadPublishSeeds::MaxSeeds + 5; ++n) {
		Seed seed = MakeSeed(n + 1);
		if (n == 0) {
			seed.udpPort = 0;
		} else if (n == 1) {
			seed.ip = 0;
		}
		file.WriteUInt128(seed.id);
		file.WriteUInt32(seed.ip);
		file.WriteUInt16(seed.udpPort);
		file.WriteUInt16(seed.tcpPort);
		file.WriteUInt8(seed.version);
		seed.key.StoreToFile(file);
		file.WriteUInt8(seed.verified);
	}
	file.Seek(0);
	Seeds read;
	ASSERT_TRUE(KadPublishSeeds::Read(file, kNow, read));
	const Entry &entry = read[CUInt128(1u)];
	ASSERT_EQUALS(KadPublishSeeds::MaxSeeds, entry.seeds.size());
	ASSERT_TRUE(entry.seeds[0].id == CUInt128(3u));
}

TEST(KadPublishSeeds, DamagedFileKeepsWhatWasRead)
{
	Seeds written;
	written[CUInt128(1u)] = MakeEntry(kNow, 2);
	written[CUInt128(2u)] = MakeEntry(kNow, 2);
	CMemFile full;
	KadPublishSeeds::Write(full, written, kNow);
	// Cut inside the second entry.
	CMemFile cut(full.GetRawBuffer(), full.GetLength() - 2);

	Seeds read;
	ASSERT_FALSE(KadPublishSeeds::Read(cut, kNow, read));
	ASSERT_EQUALS(1u, read.size());
	ASSERT_EQUALS(1u, read.count(CUInt128(1u)));
}

TEST(KadPublishSeeds, UnknownVersionReadsNothing)
{
	CMemFile file;
	file.WriteUInt8(KADSEEDS_VERSION + 1);
	file.WriteUInt32(0);
	file.Seek(0);
	Seeds read;
	ASSERT_FALSE(KadPublishSeeds::Read(file, kNow, read));
	ASSERT_TRUE(read.empty());
}
