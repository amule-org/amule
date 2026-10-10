//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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
#include <OfferFilesPolicy.h>
#include <Tag.h>
#include <MemFile.h>
#include <tags/ServerTags.h>
#include <algorithm>
#include <array>
#include <limits>
using namespace muleunit;
DECLARE_SIMPLE(OfferFilesPolicy)

static COfferFilesAdvertisement Valid(uint32 soft = 60000,
	uint32 hard = 1000,
	uint32 batch = 200,
	uint32 interval = 500,
	uint32 version = 1,
	int omit = -1)
{
	COfferFilesAdvertisement policy;
	std::array<CTagInt32, 5> tags{ { CTagInt32("offerfiles_v", version),
		CTagInt32("offerfiles_batch_max", batch),
		CTagInt32("offerfiles_min_interval_ms", interval),
		CTagInt32(ST_SOFTFILES, soft),
		CTagInt32(ST_HARDFILES, hard) } };
	for (int i = 0; i < 5; ++i) {
		if (i != omit) {
			policy.AddTag(tags[i]);
		}
	}
	return policy;
}

TEST(OfferFilesPolicy, CompleteAdvertisementBoundsBudgetAndRate)
{
	const auto policy = Valid();
	ASSERT_TRUE(policy.IsValid());
	ASSERT_EQUALS(uint32(200), policy.BatchLimit(0));
	ASSERT_EQUALS(uint32(1), policy.BatchLimit(59999));
	ASSERT_EQUALS(uint32(0), policy.BatchLimit(60000));
	ASSERT_EQUALS(uint32(0), policy.BatchLimit(std::numeric_limits<uint32>::max()));
	ASSERT_EQUALS(uint32(500), policy.IntervalMs());
	ASSERT_EQUALS(uint32(500), policy.PublicationIntervalMs(true, 60000));
	ASSERT_EQUALS(uint32(60000), policy.PublicationIntervalMs(false, 60000));
	ASSERT_EQUALS(uint32(120000), Valid(9, 11, 10, 120000).PublicationIntervalMs(false, 60000));
	ASSERT_EQUALS(uint32(200), Valid(60000, 10000, 9999, 1).BatchLimit(0));
	ASSERT_EQUALS(uint32(500), Valid(60000, 10000, 9999, 1).IntervalMs());
	ASSERT_EQUALS(uint32(9), Valid(9, 11, 10, 1200).BatchLimit(0));
	ASSERT_EQUALS(uint32(1200), Valid(9, 11, 10, 1200).IntervalMs());
}

TEST(OfferFilesPolicy, InvalidValuesMissingFieldsAndDuplicatesFallBack)
{
	for (int i = 0; i < 5; ++i) {
		ASSERT_FALSE(Valid(60000, 1000, 200, 500, 1, i).IsValid());
	}
	ASSERT_FALSE(Valid(0).IsValid());
	ASSERT_FALSE(Valid(60000, 200).IsValid()); // Strictly below hard boundary.
	ASSERT_FALSE(Valid(60000, 1000, 0).IsValid());
	ASSERT_FALSE(Valid(60000, 1000, 200, 0).IsValid());
	ASSERT_FALSE(Valid(60000, 1000, 200, 500, 2).IsValid());
	const CTagInt32 duplicates[] = { CTagInt32("offerfiles_v", 1),
		CTagInt32("offerfiles_batch_max", 200),
		CTagInt32("offerfiles_min_interval_ms", 500),
		CTagInt32(ST_SOFTFILES, 60000),
		CTagInt32(ST_HARDFILES, 1000) };
	for (const auto &tag : duplicates) {
		auto policy = Valid();
		policy.AddTag(tag);
		ASSERT_FALSE(policy.IsValid());
		ASSERT_EQUALS(uint32(0), policy.BatchLimit(0));
		ASSERT_EQUALS(uint32(60000), policy.IntervalMs());
	}
}

TEST(OfferFilesPolicy, WrongTypesCannotActivateOrRepairTheAdvertisement)
{
	for (const CTag &tag : { static_cast<const CTag &>(CTagInt8("offerfiles_v", 1)),
		     static_cast<const CTag &>(CTagInt16("offerfiles_v", 1)),
		     static_cast<const CTag &>(CTagInt64("offerfiles_v", 1)),
		     static_cast<const CTag &>(CTagString("offerfiles_v", "1")) }) {
		auto policy = Valid(60000, 1000, 200, 500, 1, 0);
		policy.AddTag(tag);
		ASSERT_FALSE(policy.IsValid());
		policy.AddTag(CTagInt32("offerfiles_v", 1));
		ASSERT_FALSE(policy.IsValid());
	}
	auto policy = Valid();
	policy.AddTag(CTagString("unrelated_extension", "ignored"));
	ASSERT_TRUE(policy.IsValid());
}

TEST(OfferFilesPolicy, RealWireTagsDecodeInAnyOrder)
{
	const CTagInt32 tags[] = { CTagInt32("offerfiles_v", 1),
		CTagInt32("offerfiles_batch_max", 200),
		CTagInt32("offerfiles_min_interval_ms", 500),
		CTagInt32(ST_SOFTFILES, 60000),
		CTagInt32(ST_HARDFILES, 1000) };
	std::array<unsigned, 5> order{ { 0, 1, 2, 3, 4 } };
	do {
		CMemFile wire;
		for (auto i : order) {
			tags[i].WriteTagToFile(&wire);
		}
		wire.Seek(0);
		COfferFilesAdvertisement policy;
		for (size_t i = 0; i < order.size(); ++i) {
			policy.AddTag(CTag(wire, true, false));
		}
		ASSERT_TRUE(policy.IsValid());
		ASSERT_EQUALS(uint32(60000), policy.SoftLimit());
	} while (std::next_permutation(order.begin(), order.end()));
}

TEST(OfferFilesPolicy, WireIntegerWidthsMustBeExactlyUint32)
{
	const wxString names[] = { "offerfiles_v", "offerfiles_batch_max", "offerfiles_min_interval_ms" };
	const uint32 values[] = { 1, 200, 1, 200, 201 };
	for (uint8 bits : { 8, 16, 64 }) {
		for (int i = 0; i < 5; ++i) {
			const CTagIntSized tag =
				i < 3 ? CTagIntSized(names[i], values[i], bits)
				      : CTagIntSized(i == 3 ? ST_SOFTFILES : ST_HARDFILES, values[i], bits);
			CMemFile wire;
			ASSERT_TRUE(tag.WriteTagToFile(&wire));
			wire.Seek(0);
			const CTag strict(wire, true, false);
			ASSERT_EQUALS(tag.GetType(), strict.GetType());
			ASSERT_EQUALS(tag.GetInt(), strict.GetInt());
			auto policy = Valid(60000, 1000, 200, 500, 1, i);
			policy.AddTag(strict);
			ASSERT_FALSE(policy.IsValid());

			// Existing callers continue to receive normalized narrow integers.
			wire.Seek(0);
			const CTag legacy(wire, true);
			ASSERT_EQUALS(bits == 64 ? TAGTYPE_UINT64 : TAGTYPE_UINT32, legacy.GetType());
			ASSERT_EQUALS(tag.GetInt(), legacy.GetInt());
		}
	}
}

TEST(OfferFilesPolicy, ConnectionSnapshotFailsClosedAndResets)
{
	COfferFilesConnectionPolicy state;
	ASSERT_TRUE(state.Get() == nullptr);
	state.Commit(Valid()); // Cannot commit without beginning a packet.
	ASSERT_TRUE(state.Get() == nullptr);
	ASSERT_TRUE(state.BeginAdvertisement());
	ASSERT_TRUE(state.Get() == nullptr); // Truncated decode has no active snapshot.
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() != nullptr);
	state.Commit(Valid(10)); // Snapshot is fixed, not revised by a second commit.
	ASSERT_EQUALS(uint32(60000), state.Get()->SoftLimit());
	ASSERT_FALSE(state.BeginAdvertisement());
	ASSERT_TRUE(state.Get() == nullptr);
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() == nullptr);
	state.Reset(); // Disconnect/new server.
	ASSERT_TRUE(state.BeginAdvertisement());
	state.Commit(Valid(10));
	ASSERT_EQUALS(uint32(10), state.Get()->SoftLimit());
	state.Reset();
	ASSERT_TRUE(state.BeginAdvertisement());
	state.Commit(COfferFilesAdvertisement());
	ASSERT_TRUE(state.Get() == nullptr);
}

TEST(OfferFilesPolicy, UndecodableAdvertisementInvalidatesAndCannotBeRepaired)
{
	COfferFilesConnectionPolicy state;
	ASSERT_TRUE(state.BeginAdvertisement());
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() != nullptr);
	state.RejectAdvertisement(); // Failed decompression of a repeat SERVERIDENT.
	ASSERT_TRUE(state.Get() == nullptr);
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() == nullptr);
	ASSERT_FALSE(state.BeginAdvertisement());
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() == nullptr);

	state.Reset();
	state.RejectAdvertisement(); // Even an undecodable first packet consumes negotiation.
	ASSERT_FALSE(state.BeginAdvertisement());
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() == nullptr);
	state.Reset();
	ASSERT_TRUE(state.BeginAdvertisement());
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() != nullptr);
}

TEST(OfferFilesPolicy, TruncatedWireCannotCommitPartialAdvertisement)
{
	CMemFile complete;
	CTagInt32("offerfiles_v", 1).WriteTagToFile(&complete);
	CTagInt32("offerfiles_batch_max", 200).WriteTagToFile(&complete);
	CMemFile truncated(complete.GetRawBuffer(), complete.GetLength() - 1);
	COfferFilesConnectionPolicy state;
	ASSERT_TRUE(state.BeginAdvertisement());
	bool rejected = false;
	try {
		COfferFilesAdvertisement candidate;
		candidate.AddTag(CTag(truncated, true, false));
		candidate.AddTag(CTag(truncated, true, false));
		state.Commit(candidate);
	} catch (const CEOFException &) {
		rejected = true;
	}
	ASSERT_TRUE(rejected);
	ASSERT_TRUE(state.Get() == nullptr);
	ASSERT_FALSE(state.BeginAdvertisement());
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() == nullptr);
}

TEST(OfferFilesPolicy, LegacyCandidatesConsumeNegotiatedBudgetOnlyOnce)
{
	COfferFilesPublication publication;
	unsigned char bytes[16] = {};
	bytes[0] = 1;
	const CMD4Hash first(bytes);
	bytes[0] = 2;
	const CMD4Hash second(bytes);
	publication.Record(first); // Legacy packet queued before SERVERIDENT.
	publication.Record(first); // A refresh does not consume another distinct slot.
	ASSERT_EQUALS(uint32(1), publication.Count());
	ASSERT_TRUE(publication.Contains(first));
	ASSERT_FALSE(publication.Contains(second));
	const auto policy = Valid(2);
	ASSERT_EQUALS(uint32(1), policy.BatchLimit(publication.Count()));
	publication.Record(second);
	ASSERT_EQUALS(uint32(0), policy.BatchLimit(publication.Count()));
	publication.Reset(); // Another socket must start with an empty budget.
	ASSERT_EQUALS(uint32(0), publication.Count());
	ASSERT_FALSE(publication.Contains(first));
}

TEST(OfferFilesPolicy, PacingIncludesLegacyOffersAndNeverAccumulatesBursts)
{
	COfferFilesPublication publication;
	ASSERT_TRUE(publication.Due(0, 500));
	publication.Sent(1000); // Legacy offer before negotiation.
	ASSERT_FALSE(publication.Due(1499, 500));
	ASSERT_TRUE(publication.Due(1500, 500));
	publication.Sent(10000); // A late tick grants one packet, no catch-up entitlement.
	ASSERT_FALSE(publication.Due(10000, 500));
	ASSERT_FALSE(publication.Due(10499, 500));
	ASSERT_TRUE(publication.Due(10500, 500));
	ASSERT_FALSE(publication.Due(10500, 60000)); // Opt-out restores legacy delay.
	publication.Reset();
	ASSERT_TRUE(publication.Due(10500, 60000));
}

TEST(OfferFilesPolicy, LiveLimitsRestrictWithoutExpandingNegotiatedBudget)
{
	const auto policy = Valid(300, 1000, 200, 500);
	ASSERT_EQUALS(uint32(200), policy.BatchLimit(0, 0, 0));
	ASSERT_EQUALS(uint32(3), policy.BatchLimit(297, 1000, 2000));
	ASSERT_EQUALS(uint32(10), policy.BatchLimit(100, 110, 1000));
	ASSERT_EQUALS(uint32(0), policy.BatchLimit(100, 99, 1000));
	ASSERT_EQUALS(uint32(0), policy.BatchLimit(100, 100, 1000));
	ASSERT_EQUALS(uint32(4), policy.BatchLimit(0, 300, 5));
	ASSERT_EQUALS(uint32(0), policy.BatchLimit(0, 300, 1));
}

TEST(OfferFilesPolicy, ValidSnapshotStaysBindingUntilDisconnect)
{
	COfferFilesConnectionPolicy state;
	ASSERT_TRUE(state.Get() == nullptr); // Support unknown.
	ASSERT_TRUE(state.BeginAdvertisement());
	state.Commit(COfferFilesAdvertisement()); // Legacy or incomplete advertisement.
	ASSERT_TRUE(state.Get() == nullptr);
	state.Reset();
	ASSERT_TRUE(state.BeginAdvertisement());
	state.Commit(Valid());
	ASSERT_TRUE(state.Get() != nullptr); // Turning acceleration off cannot relax server limits.
	state.RejectAdvertisement();
	ASSERT_TRUE(state.Get() == nullptr); // Broken renegotiation cannot activate.
	state.Reset();
	ASSERT_TRUE(state.Get() == nullptr); // Support cannot carry to a new socket.
}
