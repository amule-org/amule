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
#include <SHAHashSet.h>
#include <SearchFile.h>
#include <memory>
#include <algorithm>
#include <numeric>
#include <random>
#include <stdexcept>
#include <KadAICHVotes.h>
#include <Preferences.h>
#include <Logger.h>
#include <kademlia/kademlia/AICHHashList.h>

using namespace muleunit;
using Kademlia::CKadAICHHashList;

DECLARE_SIMPLE(AICHSearchResult)

// Ownerless consensus is supported; enable real logging so these tests do not
// rely on MULEUNIT discarding expressions that access the owner.

static CAICHHash MakeRoot(uint8_t seed)
{
	CAICHHash hash;
	memset(hash.GetRawHash(), seed, CAICHHash::GetHashSize());
	return hash;
}

TEST(AICHSearchResult, FabricatedCountsContributeOnlyOneVote)
{
	theLogger.SetVerbose(true);
	for (uint8_t claimed : { 2, 3, 255 }) {
		std::vector<uint8_t> payload(2 + CAICHHash::GetHashSize(), 0xAB);
		payload[0] = 1;
		payload[1] = claimed;
		std::vector<CKadAICHHashList::SResultHash> decoded;
		ASSERT_TRUE(CKadAICHHashList::DecodeResultTag(payload.data(), payload.size(), decoded));
		const auto *candidate = CKadAICHHashList::SelectCandidate(decoded, claimed);
		ASSERT_TRUE(candidate != nullptr);
		CAICHHash root;
		memcpy(root.GetRawHash(), candidate->m_hash.data(), CAICHHash::GetHashSize());
		CAICHHashSet hashes(nullptr);
		// Peer byte order for 1.2.3.4. The Kad call site swaps its address once.
		const uint32_t responder = 0x04030201;
		hashes.SearchResultHashReceived(root, true, responder);
		ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
		for (unsigned i = 0; i < 20; ++i) {
			hashes.SearchResultHashReceived(root, true, responder);
			// Another address in the same /20 is also not an independent vote.
			hashes.UntrustedHashReceived(root, 0x05030201);
		}
		ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
		for (uint32_t i = 2; i <= 9; ++i) {
			hashes.UntrustedHashReceived(root, 0x04030200 | i);
			ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
		}
		hashes.UntrustedHashReceived(root, 0x0403020A);
		ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
		ASSERT_TRUE(hashes.GetMasterHash() == root);
	}
}

TEST(AICHSearchResult, ConflictingReportsStillRequireAgreement)
{
	CAICHHashSet hashes(nullptr);
	const CAICHHash candidate = MakeRoot(0xAB);
	const CAICHHash other = MakeRoot(0xCD);
	hashes.SearchResultHashReceived(candidate, true, 0x04030201);
	for (uint32_t i = 2; i <= 11; ++i) {
		hashes.UntrustedHashReceived(other, 0x04030200 | i);
	}
	// Ten of eleven is below the existing 92% threshold.
	ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
	hashes.UntrustedHashReceived(other, 0x0403020C);
	ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
	hashes.UntrustedHashReceived(other, 0x0403020D);
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == other);
}

TEST(AICHSearchResult, MissingProvenanceDoesNotVoteAndServerPolicyIsUnchanged)
{
	const CAICHHash root = MakeRoot(0xAB);
	CAICHHashSet kad(nullptr);
	kad.SearchResultHashReceived(root, true, 0);
	ASSERT_EQUALS(AICH_EMPTY, kad.GetStatus());
	ASSERT_FALSE(kad.HasValidMasterHash());

	CAICHHashSet server(nullptr);
	server.SearchResultHashReceived(root, false, 0);
	ASSERT_EQUALS(AICH_TRUSTED, server.GetStatus());
	ASSERT_TRUE(server.GetMasterHash() == root);
}

TEST(AICHSearchResult, VerifiedRootCannotBeReplacedByKad)
{
	CAICHHashSet hashes(nullptr);
	const CAICHHash verified = MakeRoot(0xAB);
	hashes.SetMasterHash(verified, AICH_VERIFIED);
	hashes.SearchResultHashReceived(MakeRoot(0xCD), true, 0x04030201);
	ASSERT_EQUALS(AICH_VERIFIED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == verified);
}

TEST(AICHSearchResult, MergedRespondersReachConsensus)
{
	CKadAICHVotes merged;
	const CAICHHash root = MakeRoot(0xAB);
	for (uint32_t i = 1; i <= 10; ++i) {
		CKadAICHVotes incoming;
		incoming.Add(0x04030200 | i, root);
		merged.Merge(incoming);
	}
	ASSERT_EQUALS(size_t(10), merged.Get().size());
	CAICHHashSet hashes(nullptr);
	for (const auto &vote : merged.Get()) {
		hashes.UntrustedHashReceived(vote.second, vote.first);
	}
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == root);
}

TEST(AICHSearchResult, MergePreservesDisagreementAndDeduplicatesResponders)
{
	CKadAICHVotes merged;
	const CAICHHash root = MakeRoot(0xAB);
	const CAICHHash other = MakeRoot(0xCD);
	merged.Add(0x0403020C, other);
	for (uint32_t i = 1; i <= 11; ++i) {
		CKadAICHVotes incoming;
		incoming.Add(0x04030200 | i, root);
		merged.Merge(incoming);
	}
	ASSERT_EQUALS(size_t(12), merged.Get().size());
	ASSERT_TRUE(merged.Get().at(CAICHUntrustedHash::SigningSubnet(0x0403020C)) == other);
	CAICHHashSet hashes(nullptr);
	for (const auto &vote : merged.Get()) {
		hashes.UntrustedHashReceived(vote.second, vote.first);
	}
	ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
}

TEST(AICHSearchResult, VotesAreBoundedAndUnknownRespondersExcluded)
{
	CKadAICHVotes merged;
	const CAICHHash root = MakeRoot(0xAB);
	merged.Add(0, root);
	ASSERT_TRUE(merged.Get().empty());
	for (uint32_t i = 1; i <= 1000; ++i) {
		CKadAICHVotes incoming;
		incoming.Add(i, root);
		merged.Merge(incoming);
	}
	ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, merged.Get().size());
	CKadAICHVotes copy(merged);
	ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, copy.Get().size());
	ASSERT_TRUE(copy.Get() == merged.Get());
}

// Build model rows without consulting the live download/known-file queues.
// Constructors, ownership, copying, AddChild, MergeResults and evidence replay
// are production code; the fixture only supplies incoming search data.
class CSearchFileTestFixture
{
public:
	static CSearchFile *Result(const wxString &name,
		uint32_t responder,
		const CAICHHash &root,
		const CKadAICHVotes::Key &key = CKadAICHVotes::Key{},
		bool kad = true)
	{
		auto *result = new CSearchFile;
		result->SetFileName(CPath(name));
		result->SetFileSize(1024);
		result->m_kademlia = kad;
		result->m_kadAICHVotes = CKadAICHVotes(key);
		result->m_kadAICHVotes.Add(responder, root);
		return result;
	}
};

TEST(AICHSearchResult, EveryFilenameChildReplaysAllGroupVotes)
{
	const CAICHHash root = MakeRoot(0xAB);
	std::unique_ptr<CSearchFile> group(CSearchFileTestFixture::Result("first", 1, root));
	// Exercise the duplicate-name deletion path before a group has children.
	group->AddChild(CSearchFileTestFixture::Result("first", 2, root));
	ASSERT_FALSE(group->HasChildren());
	// Creating the next variant copies the original row into the first child.
	for (uint32_t i = 3; i <= 10; ++i) {
		group->AddChild(CSearchFileTestFixture::Result(i <= 6 ? "second" : "third", i, root));
	}
	ASSERT_EQUALS(size_t(3), group->GetChildren().size());
	ASSERT_EQUALS(size_t(10), group->GetKadAICHVotes().size());
	ASSERT_EQUALS(size_t(2), group->GetChildren().front()->GetKadAICHVotes().size());
	for (const CSearchFile *child : group->GetChildren()) {
		ASSERT_TRUE(child->GetParent() == group.get());
		const CPath filename = child->GetFileName();
		CAICHHashSet downloaded(nullptr);
		// This is the same production handoff called by CPartFile's constructor.
		ASSERT_TRUE(child->ApplyKadAICHVotes(downloaded));
		ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
		ASSERT_TRUE(downloaded.GetMasterHash() == root);
		ASSERT_TRUE(child->GetFileName() == filename);
	}
	CAICHHashSet parentDownload(nullptr);
	ASSERT_TRUE(group->ApplyKadAICHVotes(parentDownload));
	ASSERT_EQUALS(AICH_TRUSTED, parentDownload.GetStatus());
}

TEST(AICHSearchResult, ChildDownloadRetainsOtherVariantsDisagreement)
{
	const CAICHHash root = MakeRoot(0xAB);
	const CAICHHash other = MakeRoot(0xCD);
	std::unique_ptr<CSearchFile> group(CSearchFileTestFixture::Result("minority", 1, other));
	for (uint32_t i = 2; i <= 11; ++i) {
		group->AddChild(CSearchFileTestFixture::Result("majority", i, root));
	}
	ASSERT_EQUALS(size_t(2), group->GetChildren().size());
	const CSearchFile *majority = group->GetChildren().back();
	ASSERT_EQUALS(size_t(10), majority->GetKadAICHVotes().size());
	CAICHHashSet downloaded(nullptr);
	ASSERT_TRUE(majority->ApplyKadAICHVotes(downloaded));
	// Its own ten matching votes would be trusted. Including the other filename's
	// disagreement keeps ten out of eleven below the existing 92% threshold.
	ASSERT_EQUALS(AICH_UNTRUSTED, downloaded.GetStatus());
}

TEST(AICHSearchResult, ContradictingSubnetCannotInflateConsensusDenominator)
{
	const auto root = MakeRoot(0xAB);
	CAICHHashSet hashes(nullptr);
	hashes.UntrustedHashReceived(root, 0x04030201);
	// Different addresses of the same /20, and many different roots.
	for (uint8_t i = 1; i < 100; ++i) {
		hashes.UntrustedHashReceived(MakeRoot(i), 0xFF0F0201);
	}
	for (uint32_t i = 2; i <= 10; ++i) {
		hashes.UntrustedHashReceived(root, 0x04030200 | i);
	}
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == root);
}

TEST(AICHSearchResult, SubnetMaskUsesPeerOrderAndProbeDoesNotInsert)
{
	// Peer-order 1.2.3.4 and 1.2.15.255 share a /20; 1.2.16.4 does not.
	CAICHUntrustedHash entry;
	ASSERT_TRUE(entry.AddSigningIP(0x04030201, true));
	ASSERT_TRUE(entry.m_adwIpsSigning.empty());
	ASSERT_TRUE(entry.AddSigningIP(0x04030201));
	ASSERT_FALSE(entry.AddSigningIP(0xFF0F0201, true));
	ASSERT_FALSE(entry.AddSigningIP(0xFF0F0201));
	ASSERT_TRUE(entry.AddSigningIP(0x04100201));
	ASSERT_EQUALS(size_t(2), entry.m_adwIpsSigning.size());
	CKadAICHVotes votes;
	votes.Add(0x04030201, MakeRoot(1));
	votes.Add(0xFF0F0201, MakeRoot(1));
	votes.Add(0x04100201, MakeRoot(1));
	ASSERT_EQUALS(size_t(2), votes.Get().size());
}

TEST(AICHSearchResult, SameSubnetFloodCannotConsumeTheBudget)
{
	CKadAICHVotes votes;
	const auto root = MakeRoot(1);
	for (uint32_t i = 0; i < 4096; ++i) {
		votes.Add(0x00000201 | ((i & 15) << 16) | ((i >> 4) << 24), root);
	}
	ASSERT_EQUALS(size_t(1), votes.GetSlotCount());
	for (uint32_t i = 2; i <= CKadAICHVotes::kMaxWitnesses; ++i) {
		votes.Add(i, root);
	}
	ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, votes.Get().size());
}

TEST(AICHSearchResult, ContradictorySubnetIsExcludedAcrossArrivalAndMergeOrders)
{
	const auto root = MakeRoot(1);
	const auto other = MakeRoot(2);
	CKadAICHVotes first, second, conflict;
	first.Add(0x04030201, root);
	second.Add(0xFF0F0201, other);
	conflict.Merge(first);
	conflict.Merge(second);
	ASSERT_TRUE(conflict.Get().empty());
	ASSERT_EQUALS(size_t(1), conflict.GetSlotCount());
	CKadAICHVotes reverse;
	reverse.Merge(second);
	reverse.Merge(first);
	ASSERT_TRUE(reverse.Get().empty());
	// Repeats and a previously consistent partial sample cannot resurrect it.
	conflict.Add(0x04030201, root);
	conflict.Merge(first);
	ASSERT_TRUE(conflict.Get().empty());
	CKadAICHVotes copy(conflict);
	copy.Merge(reverse);
	ASSERT_TRUE(copy.Get().empty());
	copy.Add(2, root);
	ASSERT_EQUALS(size_t(1), copy.Get().size());
	ASSERT_EQUALS(size_t(2), copy.GetSlotCount());
}

TEST(AICHSearchResult, TopKIsIndependentOfArrivalAndTruncatedMergeOrders)
{
	CKadAICHVotes::Key key{};
	key[0] = 42;
	std::vector<uint32_t> responders(1000);
	std::iota(responders.begin(), responders.end(), 1);
	CKadAICHVotes forward(key), reverse(key), shuffled(key);
	for (auto ip : responders) {
		forward.Add(ip, MakeRoot(ip % 3));
		if (ip % 5 == 0) {
			forward.Add(ip, MakeRoot(99));
		}
	}
	for (auto it = responders.rbegin(); it != responders.rend(); ++it) {
		reverse.Add(*it, MakeRoot(*it % 3));
		if (*it % 5 == 0) {
			reverse.Add(*it, MakeRoot(99));
		}
	}
	std::mt19937 rng(123);
	std::shuffle(responders.begin(), responders.end(), rng);
	std::vector<CKadAICHVotes> batches(5, CKadAICHVotes(key));
	for (size_t i = 0; i < responders.size(); ++i) {
		const auto ip = responders[i];
		shuffled.Add(ip, MakeRoot(ip % 3));
		batches[i % batches.size()].Add(ip, MakeRoot(ip % 3));
		if (ip % 5 == 0) {
			shuffled.Add(ip, MakeRoot(99));
			batches[i % batches.size()].Add(ip, MakeRoot(99));
		}
	}
	CKadAICHVotes merged(key), mergedReverse(key);
	for (const auto &batch : batches) {
		ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, batch.GetSlotCount());
		merged.Merge(batch);
	}
	for (auto it = batches.rbegin(); it != batches.rend(); ++it) {
		mergedReverse.Merge(*it);
	}
	ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, forward.GetSlotCount());
	ASSERT_TRUE(forward.Get() == reverse.Get());
	ASSERT_TRUE(forward.Get() == shuffled.Get());
	ASSERT_TRUE(forward.Get() == merged.Get());
	ASSERT_TRUE(forward.Get() == mergedReverse.Get());
	// The selected set includes later witnesses, both confirmation and disagreement.
	bool lateConfirmation = false, lateDisagreement = false;
	for (const auto &vote : forward.Get()) {
		if (vote.first > 64) {
			lateConfirmation |= vote.second == MakeRoot(1);
			lateDisagreement |= vote.second == MakeRoot(2);
		}
	}
	ASSERT_TRUE(lateConfirmation);
	ASSERT_TRUE(lateDisagreement);
}

TEST(AICHSearchResult, ConflictTombstonesRemainBoundedAndMergeableAfterEviction)
{
	CKadAICHVotes all, left, right;
	for (uint32_t i = 1; i <= 1000; ++i) {
		all.Add(i, MakeRoot(1));
		all.Add(i, MakeRoot(2));
		left.Add(i, MakeRoot(1));
		right.Add(i, MakeRoot(2));
	}
	left.Merge(right);
	ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, all.GetSlotCount());
	ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, left.GetSlotCount());
	ASSERT_TRUE(all.Get().empty());
	ASSERT_TRUE(left.Get().empty());
	for (uint32_t i = 1000; i != 0; --i) {
		left.Add(i, MakeRoot(1));
	}
	ASSERT_TRUE(left.Get().empty());
}

TEST(AICHSearchResult, DifferentSearchKeysCannotMergeTruncatedSamples)
{
	CKadAICHVotes::Key key{};
	key[0] = 1;
	CKadAICHVotes first, second(key);
	first.Add(1, MakeRoot(1));
	second.Add(2, MakeRoot(1));
	// Evidence under a different search key is skipped, never merged: the first
	// sample keeps exactly the single witness it admitted, and the merge does
	// not throw (the UDP socket callback would otherwise abort the client).
	first.Merge(second);
	ASSERT_EQUALS(size_t(1), first.GetSlotCount());
	ASSERT_EQUALS(size_t(1), first.Get().size());
}

TEST(AICHSearchResult, SearchUpdatesDoNotChangeTheDownloadedSnapshot)
{
	std::unique_ptr<CSearchFile> group(CSearchFileTestFixture::Result("first", 1, MakeRoot(1)));
	for (uint32_t i = 2; i <= 10; ++i) {
		group->AddChild(CSearchFileTestFixture::Result("second", i, MakeRoot(1)));
	}
	CAICHHashSet downloaded(nullptr);
	ASSERT_TRUE(group->GetChildren().back()->ApplyKadAICHVotes(downloaded));
	ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
	// Every retained subnet now contradicts itself in the search list.
	for (uint32_t i = 1; i <= 10; ++i) {
		group->AddChild(CSearchFileTestFixture::Result("third", i, MakeRoot(2)));
	}
	ASSERT_TRUE(group->GetKadAICHVotes().empty());
	ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
	ASSERT_TRUE(downloaded.GetMasterHash() == MakeRoot(1));
	CAICHHashSet nextDownload(nullptr);
	ASSERT_FALSE(group->GetChildren().back()->ApplyKadAICHVotes(nextDownload));
	ASSERT_EQUALS(AICH_EMPTY, nextDownload.GetStatus());
}

TEST(AICHSearchResult, BoundedFilenameGroupsReplayTheSameFinalSample)
{
	CKadAICHVotes::Key key{};
	key[0] = 17;
	const auto root = MakeRoot(1);
	CKadAICHVotes expected(key);
	std::unique_ptr<CSearchFile> forward(CSearchFileTestFixture::Result("a", 1, root, key));
	std::unique_ptr<CSearchFile> reverse(CSearchFileTestFixture::Result("b", 300, root, key));
	expected.Add(1, root);
	for (uint32_t i = 2; i <= 300; ++i) {
		expected.Add(i, root);
		forward->AddChild(CSearchFileTestFixture::Result(i % 2 ? "a" : "b", i, root, key));
	}
	for (uint32_t i = 299; i != 0; --i) {
		reverse->AddChild(CSearchFileTestFixture::Result(i % 2 ? "a" : "b", i, root, key));
	}
	ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, expected.Get().size());
	ASSERT_TRUE(forward->GetKadAICHVotes() == expected.Get());
	ASSERT_TRUE(reverse->GetKadAICHVotes() == expected.Get());
	for (const auto *group : { forward.get(), reverse.get() }) {
		for (const auto *child : group->GetChildren()) {
			CAICHHashSet downloaded(nullptr);
			ASSERT_TRUE(child->ApplyKadAICHVotes(downloaded));
			ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
			ASSERT_TRUE(downloaded.GetMasterHash() == root);
		}
	}
}

TEST(AICHSearchResult, SamplingKeyChangesSelectionAndDuplicatesDoNot)
{
	CKadAICHVotes::Key key{};
	key[0] = 7;
	CKadAICHVotes first, second(key);
	for (uint32_t i = 1; i <= 300; ++i) {
		first.Add(i, MakeRoot(1));
		second.Add(i, MakeRoot(1));
	}
	ASSERT_FALSE(first.Get() == second.Get());
	const auto snapshot = first.Get();
	for (uint32_t i = 300; i != 0; --i) {
		// A different host in the same /20 does not change its priority.
		first.Add(i | 0xFF000000, MakeRoot(1));
	}
	ASSERT_TRUE(first.Get() == snapshot);
	first.Merge(first);
	ASSERT_TRUE(first.Get() == snapshot);
}

// Sampling keys come from the process RNG: each search gets a fresh, non-zero secret.
TEST(AICHSearchResult, GeneratedKeysAreFreshPerSearch)
{
	const CKadAICHVotes::Key first = CKadAICHVotes::GenerateKey();
	const CKadAICHVotes::Key second = CKadAICHVotes::GenerateKey();
	ASSERT_FALSE(first == CKadAICHVotes::Key{});
	ASSERT_FALSE(first == second);
}

TEST(AICHSearchResult, ConsensusTrustRemainsProvisionalUntilLocalVerification)
{
	CAICHHashSet hashes(nullptr);
	for (uint32_t i = 1; i <= 10; ++i) {
		hashes.UntrustedHashReceived(MakeRoot(1), i);
	}
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	hashes.UntrustedHashReceived(MakeRoot(2), 11);
	ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
	for (uint32_t i = 12; i <= 125; ++i) {
		hashes.UntrustedHashReceived(MakeRoot(2), i);
	}
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == MakeRoot(2));
}

TEST(AICHSearchResult, IndependentGoldenSampleSurvivesRandomizedMergeTrees)
{
	CKadAICHVotes::Key key{};
	key[0] = 42;
	// Computed independently using Python's hmac.digest(key, ip.to_bytes(4,
	// 'little'), 'sha256'), sorted over ALL 300 subnets before taking 64.
	const uint32_t selected[] = { 1,
		8,
		11,
		15,
		21,
		25,
		27,
		33,
		40,
		46,
		47,
		64,
		66,
		75,
		76,
		78,
		80,
		81,
		84,
		85,
		87,
		90,
		110,
		112,
		114,
		125,
		126,
		134,
		137,
		143,
		146,
		148,
		154,
		155,
		160,
		163,
		170,
		172,
		177,
		187,
		190,
		192,
		208,
		209,
		218,
		219,
		234,
		235,
		238,
		242,
		250,
		256,
		264,
		269,
		271,
		274,
		275,
		280,
		284,
		285,
		289,
		293,
		298,
		299 };
	std::map<uint32_t, CAICHHash> expected;
	std::vector<std::pair<uint32_t, CAICHHash>> reports;
	for (uint32_t subnet = 1; subnet <= 300; ++subnet) {
		const auto root = MakeRoot(1 + subnet % 3);
		for (uint32_t host = 1; host <= 3; ++host) {
			reports.emplace_back(subnet | (host << 24), root);
		}
		if (subnet % 7 == 0) {
			reports.emplace_back(subnet | 0xFF000000, MakeRoot(99));
		}
	}
	for (auto subnet : selected) {
		if (subnet % 7 != 0) {
			expected.emplace(subnet, MakeRoot(1 + subnet % 3));
		}
	}
	// Unknown provenance must not occupy a slot or change the sample.
	reports.emplace_back(0, MakeRoot(99));
	for (unsigned trial = 0; trial < 20; ++trial) {
		std::mt19937 rng(trial);
		std::shuffle(reports.begin(), reports.end(), rng);
		CKadAICHVotes sequential(key);
		std::vector<CKadAICHVotes> samples(8, CKadAICHVotes(key));
		for (const auto &report : reports) {
			sequential.Add(report.first, report.second);
			samples[rng() % samples.size()].Add(report.first, report.second);
		}
		ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, sequential.GetSlotCount());
		ASSERT_TRUE(sequential.Get() == expected);
		// Fold differently truncated partial histories through arbitrary trees.
		while (samples.size() > 1) {
			const size_t target = rng() % samples.size();
			const size_t source = (target + 1 + rng() % (samples.size() - 1)) % samples.size();
			samples[target].Merge(samples[source]);
			samples.erase(samples.begin() + source);
		}
		ASSERT_EQUALS(CKadAICHVotes::kMaxWitnesses, samples.front().GetSlotCount());
		ASSERT_TRUE(samples.front().Get() == expected);
	}
}

TEST(AICHSearchResult, PolymorphicSearchResultDispatchAndLifetime)
{
	// Use production objects through their actual base, exercising the vptr
	// checks on virtual dispatch and destruction rather than only direct calls.
	std::unique_ptr<CAbstractFile> file(CSearchFileTestFixture::Result("before", 1, MakeRoot(1)));
	file->SetFileName(CPath("after"));
	file->SetFileSize(2048);
	ASSERT_TRUE(file->GetFileName() == CPath("after"));
	ASSERT_EQUALS(uint64(2048), file->GetFileSize());
	auto *search = dynamic_cast<CSearchFile *>(file.get());
	ASSERT_TRUE(search != nullptr);
	ASSERT_TRUE(dynamic_cast<CKnownFile *>(file.get()) == nullptr);
	CAICHHashSet hashes(nullptr);
	ASSERT_TRUE(search->ApplyKadAICHVotes(hashes));
	ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
	file.reset(); // Delete the real CSearchFile through CAbstractFile's vtable.
}
TEST(AICHSearchResult, ServerFirstAllSearchGroupReplaysKadVotes)
{
	const CAICHHash root = MakeRoot(0xAB);
	CKadAICHVotes::Key key{};
	key[0] = 42;
	std::unique_ptr<CSearchFile> group(CSearchFileTestFixture::Result("server", 0, root, key, false));
	for (uint32_t i = 1; i <= 10; ++i) {
		group->AddChild(CSearchFileTestFixture::Result("kad", i, root, key));
	}
	ASSERT_FALSE(group->IsKademlia());
	ASSERT_EQUALS(size_t(10), group->GetKadAICHVotes().size());
	for (const CSearchFile *child : group->GetChildren()) {
		CAICHHashSet downloaded(nullptr);
		ASSERT_TRUE(child->ApplyKadAICHVotes(downloaded));
		ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
		ASSERT_TRUE(downloaded.GetMasterHash() == root);
	}
}
