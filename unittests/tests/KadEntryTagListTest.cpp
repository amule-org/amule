//								-*- C++ -*-
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

#include <muleunit/test.h>
#include <kademlia/kademlia/Entry.h>
#include <kademlia/kademlia/AICHHashList.h>
#include <MemFile.h>
#include <Tag.h>
#include <tags/FileTags.h>

using namespace muleunit;
using Kademlia::CKadAICHHashList;
using Kademlia::CKeyEntry;

// A search answer carries its tag count in a single byte, so the invariant these tests pin is that
// the byte equals the number of tags that actually follow it. It matters more than it looks:
// CKademliaUDPListener::ProcessSearchResponse() reads several answers out of one packet in
// sequence, so a count that disagrees with the tags present does not merely garble its own answer,
// it desynchronises every later one in the packet.
//
// Each test therefore appends a sentinel where the next answer would start and reads it back. That
// catches a miscount in either direction, which asserting on the tag total alone does not.
//
// The expected tag total is deliberately never hardcoded: WriteTagListWithPublishInfo() writes one
// tag of its own without ENABLE_KAD_PROTOCOL_10 and up to two with it, and these tests are meant
// to hold in both configurations.

static const uint32_t SENTINEL = 0xA1C4DEADu;

// WriteTagListWithPublishInfo() needs a publisher, or it wxFAILs and falls back to a plain tag
// list. Registering one means two steps, not one: the entry's own list, and the global per-/24
// count. ReCalculateTrustValue() looks the publisher's subnet up in that global map and wxFAILs
// on a miss, so pushing onto the list alone leaves the entry in a state the trust calculation
// treats as an inconsistency. The destructor decrements the same global count, so the two stay
// balanced as long as the entry is left to destruct normally.
class CTestKeyEntry : public CKeyEntry
{
public:
	void AddTestPublisher(uint32_t ip)
	{
		if (m_publishingIPs == NULL) {
			m_publishingIPs = new PublishingIPList;
		}
		sPublishingIP publisher;
		publisher.m_ip = ip;
		publisher.m_lastPublish = 0;
		publisher.m_aichHashIdx = CKadAICHHashList::INVALID_INDEX;
		m_publishingIPs->push_back(publisher);
		AdjustGlobalPublishTracking(ip, true, wxT("test publisher"));
	}

	// GetTrustValue() only recalculates when its last result is older than ten minutes, and the
	// timestamp starts at zero, so on a freshly booted machine the comparison is against uptime
	// and the recalculation never happens. Force it instead of depending on how long the runner
	// has been up.
	void RecalculateTrustNow() { ReCalculateTrustValue(); }

	void AddDistinctTags(uint32_t count)
	{
		for (uint32_t i = 0; i < count; ++i) {
			// Multi-character names, so none can collide with the single-byte tags the
			// writer handles specially (TAG_FILENAME, TAG_FILESIZE, TAG_PUBLISHINFO).
			AddTag(new CTagVarInt(wxString::Format(wxT("t%u"), i), i), 0);
		}
	}
};

// Writes one answer the way a keyword search response does, then a sentinel where the next answer
// would begin. Returns the tag count byte the writer put on the wire.
static uint8_t WriteAnswerWithSentinel(CTestKeyEntry &entry, CMemFile &file)
{
	entry.WriteTagListWithPublishInfo(&file);
	file.WriteUInt32(SENTINEL);

	file.Seek(0);
	const uint8_t declared = file.ReadUInt8();
	file.Seek(0);
	return declared;
}

// Reads the tag list back and checks the stream lands exactly on the sentinel afterwards.
static void ReadBackAndCheckAlignment(CMemFile &file, TagPtrList *tags)
{
	file.ReadTagPtrList(tags, false /*bOptACP*/);
	ASSERT_EQUALS(SENTINEL, file.ReadUInt32());
	ASSERT_EQUALS(file.GetLength(), file.GetPosition());
}

// How many of the tags came from AddDistinctTags().
static uint32_t CountEntryTags(const TagPtrList &tags)
{
	uint32_t found = 0;
	for (TagPtrList::const_iterator it = tags.begin(); it != tags.end(); ++it) {
		if ((*it)->GetName().StartsWith(wxT("t"))) {
			found++;
		}
	}
	return found;
}

DECLARE_SIMPLE(KadEntryTagList)

// The ordinary case: every tag the entry holds fits, and the count describes all of them plus the
// writer's own.
TEST(KadEntryTagList, CountDescribesEveryTagThatFollows)
{
	CTestKeyEntry entry;
	entry.AddTestPublisher(0x0A000001);
	entry.SetFileName(wxT("knoppix.iso"));
	entry.m_uSize = 4096;
	entry.AddDistinctTags(5);
	ASSERT_EQUALS(7u, entry.GetTagCount()); // 5 + size + filename

	// The publisher is registered in the global per-subnet map, so the trust calculation finds
	// its /24 and scores it. A zero here means the registration was skipped and
	// ReCalculateTrustValue() took its wxFAIL branch instead -- which is silent on some
	// platforms, so this assertion is what makes that visible everywhere.
	entry.RecalculateTrustNow();
	ASSERT_TRUE(entry.GetTrustValue() > 0.0);

	CMemFile file;
	const uint8_t declared = WriteAnswerWithSentinel(entry, file);

	TagPtrList tags;
	ReadBackAndCheckAlignment(file, &tags);

	ASSERT_EQUALS((uint32_t)declared, (uint32_t)tags.size());
	// Nothing was dropped: all five entry tags are there, as are the name and size.
	ASSERT_EQUALS(5u, CountEntryTags(tags));
	ASSERT_TRUE(tags.size() > entry.GetTagCount()); // plus at least the publish-info tag

	deleteTagPtrListEntries(&tags);
}

// More tags than one byte can describe. Before the clamp the count wrapped to 0 and every tag was
// written anyway, so a reader took 0 tags and then parsed tag bytes as the next answer's hash.
TEST(KadEntryTagList, AnOverFullEntrySendsAShortAnswerThatStillParses)
{
	CTestKeyEntry entry;
	entry.AddTestPublisher(0x0A000001);
	entry.SetFileName(wxT("knoppix.iso"));
	entry.m_uSize = 4096;
	entry.AddDistinctTags(300);
	ASSERT_EQUALS(302u, entry.GetTagCount());

	CMemFile file;
	const uint8_t declared = WriteAnswerWithSentinel(entry, file);

	// Clamped to what the byte can express, not wrapped.
	ASSERT_EQUALS(0xFFu, (uint32_t)declared);

	TagPtrList tags;
	ReadBackAndCheckAlignment(file, &tags);
	ASSERT_EQUALS(0xFFu, (uint32_t)tags.size());

	// The writer's own tags are reserved out of the same budget rather than dropped, so the
	// surplus comes off the entry's tag list and the publish info still arrives.
	ASSERT_TRUE(CountEntryTags(tags) < 0xFFu);
	bool publishInfoPresent = false;
	for (TagPtrList::const_iterator it = tags.begin(); it != tags.end(); ++it) {
		if (!(*it)->GetName().Cmp(TAG_PUBLISHINFO)) {
			publishInfoPresent = true;
		}
	}
	ASSERT_TRUE(publishInfoPresent);

	deleteTagPtrListEntries(&tags);
}

// Exactly at the boundary, where an off-by-one in the reservation would show up.
TEST(KadEntryTagList, TheBoundaryIsDescribedExactly)
{
	CTestKeyEntry entry;
	entry.AddTestPublisher(0x0A000001);
	entry.SetFileName(wxT("knoppix.iso"));
	entry.m_uSize = 4096;
	entry.AddDistinctTags(253); // 253 + size + filename == 255
	ASSERT_EQUALS(0xFFu, entry.GetTagCount());

	CMemFile file;
	const uint8_t declared = WriteAnswerWithSentinel(entry, file);
	ASSERT_EQUALS(0xFFu, (uint32_t)declared);

	TagPtrList tags;
	ReadBackAndCheckAlignment(file, &tags);
	ASSERT_EQUALS(0xFFu, (uint32_t)tags.size());

	deleteTagPtrListEntries(&tags);
}
