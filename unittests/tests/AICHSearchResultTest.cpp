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
#include <tags/FileTags.h>
#include <MemFile.h>
#include <Tag.h>
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

	// Rows carry an AICH root as an FT_AICH_HASH string tag, as the network delivers it.
	static void AddAICHTag(CSearchFile *file, const CAICHHash &root)
	{
		file->m_taglist.push_back(CTagString(FT_AICH_HASH, root.GetString()));
	}

	// An eD2k server answer, initialised as the network constructor does it.
	static CSearchFile *ServerRow(const wxString &name, const CAICHHash &root, uint32_t serverIP)
	{
		CSearchFile *result = Result(name, 0, root, CKadAICHVotes::Key{}, false);
		result->m_clientServerIP = serverIP;
		AddAICHTag(result, root);
		result->InitEd2kAICHRoot();
		return result;
	}
};

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
		hashes.KadHashReceived(root, CAICHUntrustedHash::SigningSubnet(responder));
		ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
		for (unsigned i = 0; i < 20; ++i) {
			hashes.KadHashReceived(root, CAICHUntrustedHash::SigningSubnet(responder));
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
	hashes.KadHashReceived(candidate, CAICHUntrustedHash::SigningSubnet(0x04030201));
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
	CKadAICHVotes::Key key{};
	key[0] = 1;
	CKadAICHVotes kad(key);
	kad.Add(0, root);
	ASSERT_EQUALS(size_t(0), kad.GetSlotCount());

	std::unique_ptr<CSearchFile> server(CSearchFileTestFixture::ServerRow("x.iso", root, 0x0D0C0B0A));
	CAICHHashSet hashes(nullptr);
	ASSERT_TRUE(server->ApplyAICHEvidence(hashes));
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == root);
}

TEST(AICHSearchResult, VerifiedRootCannotBeReplacedByKad)
{
	CAICHHashSet hashes(nullptr);
	const CAICHHash verified = MakeRoot(0xAB);
	hashes.SetMasterHash(verified, AICH_VERIFIED);
	hashes.KadHashReceived(MakeRoot(0xCD), CAICHUntrustedHash::SigningSubnet(0x04030201));
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
		ASSERT_TRUE(child->ApplyAICHEvidence(downloaded));
		ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
		ASSERT_TRUE(downloaded.GetMasterHash() == root);
		ASSERT_TRUE(child->GetFileName() == filename);
	}
	CAICHHashSet parentDownload(nullptr);
	ASSERT_TRUE(group->ApplyAICHEvidence(parentDownload));
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
	ASSERT_TRUE(majority->ApplyAICHEvidence(downloaded));
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
	ASSERT_TRUE(group->GetChildren().back()->ApplyAICHEvidence(downloaded));
	ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
	// Every retained subnet now contradicts itself in the search list.
	for (uint32_t i = 1; i <= 10; ++i) {
		group->AddChild(CSearchFileTestFixture::Result("third", i, MakeRoot(2)));
	}
	ASSERT_TRUE(group->GetKadAICHVotes().empty());
	ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
	ASSERT_TRUE(downloaded.GetMasterHash() == MakeRoot(1));
	CAICHHashSet nextDownload(nullptr);
	ASSERT_FALSE(group->GetChildren().back()->ApplyAICHEvidence(nextDownload));
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
			ASSERT_TRUE(child->ApplyAICHEvidence(downloaded));
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
	ASSERT_TRUE(search->ApplyAICHEvidence(hashes));
	ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus());
	file.reset(); // Delete the real CSearchFile through CAbstractFile's vtable.
}
TEST(AICHSearchResult, ServerFirstAllSearchGroupReplaysKadVotes)
{
	const CAICHHash root = MakeRoot(0xAB);
	CKadAICHVotes::Key key{};
	key[0] = 42;
	// Server answers carry no sampling key; the group takes the key of its first Kad vote.
	std::unique_ptr<CSearchFile> group(
		CSearchFileTestFixture::Result("server", 0, root, CKadAICHVotes::Key{}, false));
	for (uint32_t i = 1; i <= 10; ++i) {
		group->AddChild(CSearchFileTestFixture::Result("kad", i, root, key));
	}
	ASSERT_FALSE(group->IsKademlia());
	ASSERT_EQUALS(size_t(10), group->GetKadAICHVotes().size());
	for (const CSearchFile *child : group->GetChildren()) {
		CAICHHashSet downloaded(nullptr);
		ASSERT_TRUE(child->ApplyAICHEvidence(downloaded));
		ASSERT_EQUALS(AICH_TRUSTED, downloaded.GetStatus());
		ASSERT_TRUE(downloaded.GetMasterHash() == root);
	}
}

// A download source outranks a replayed Kad search result in its own /20.
TEST(AICHSearchResult, SourceInKadSubnetTakesOverTheSlot)
{
	const CAICHHash forged = MakeRoot(0x11);
	const CAICHHash real = MakeRoot(0x22);
	CAICHHashSet hashes(nullptr);
	hashes.KadHashReceived(forged, CAICHUntrustedHash::SigningSubnet(0x04030201)); // 1.2.3.4
	for (uint32_t i = 2; i <= 12; ++i) {
		hashes.UntrustedHashReceived(real, 0x04030200 | i); // i.2.3.4
	}
	ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus()); // 11 of 12 is under 92%
	// A source inside the Kad responder's /20 (1.2.3.200) replaces its vote: 12 of 12.
	hashes.UntrustedHashReceived(real, 0xC8030201);
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == real);
}

// Only Kad-backed slots move: sources still cannot contradict their own /20, and Kad cannot
// displace a source.
TEST(AICHSearchResult, SourceSlotsStayPinned)
{
	const CAICHHash first = MakeRoot(0x31);
	const CAICHHash second = MakeRoot(0x32);
	CAICHHashSet hashes(nullptr);
	hashes.UntrustedHashReceived(first, 0x04030201);
	hashes.UntrustedHashReceived(second, 0xC8030201);
	hashes.KadHashReceived(second, CAICHUntrustedHash::SigningSubnet(0xC8030201));
	for (uint32_t i = 2; i <= 10; ++i) {
		hashes.UntrustedHashReceived(first, 0x04030200 | i);
	}
	// Ten /20s for the first root and none for the second. Either rejected report counting
	// would make it 10 of 11 and leave the file untrusted.
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == first);
}

// A source confirming a Kad vote makes that slot a source slot, which no longer moves.
TEST(AICHSearchResult, SourceConfirmationPinsAKadSlot)
{
	const CAICHHash root = MakeRoot(0x41);
	const CAICHHash other = MakeRoot(0x42);
	CAICHHashSet hashes(nullptr);
	hashes.KadHashReceived(root, CAICHUntrustedHash::SigningSubnet(0x04030201));
	hashes.UntrustedHashReceived(root, 0x05030201);  // same /20, now source-backed
	hashes.UntrustedHashReceived(other, 0xC8030201); // same /20 again: rejected, not moved
	for (uint32_t i = 2; i <= 10; ++i) {
		hashes.UntrustedHashReceived(root, 0x04030200 | i);
	}
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus()); // 10 of 10
	ASSERT_TRUE(hashes.GetMasterHash() == root);
}

// The eD2k root of a group no longer depends on whether its row or a Kad row came first.
TEST(AICHSearchResult, Ed2kRootIsIndependentOfArrivalOrder)
{
	CKadAICHVotes::Key key{};
	key[0] = 7;
	const CAICHHash root = MakeRoot(0x44);
	const uint32_t server = 0x0D0C0B0A;
	std::unique_ptr<CSearchFile> kadFirst(CSearchFileTestFixture::Result("x.iso", 0x04030201, root, key));
	kadFirst->AddChild(CSearchFileTestFixture::ServerRow("x.iso", root, server));
	std::unique_ptr<CSearchFile> serverFirst(CSearchFileTestFixture::ServerRow("x.iso", root, server));
	serverFirst->AddChild(CSearchFileTestFixture::Result("x.iso", 0x04030201, root, key));
	for (const CSearchFile *group : { kadFirst.get(), serverFirst.get() }) {
		CAICHHashSet hashes(nullptr);
		ASSERT_TRUE(group->ApplyAICHEvidence(hashes));
		// Kad agrees with the server, so the root is trusted at once.
		ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
		ASSERT_TRUE(hashes.GetMasterHash() == root);
	}
}

// When Kad disagrees, the eD2k root is one vote, and sources confirming it can only help it.
TEST(AICHSearchResult, DisputedEd2kRootGainsFromConfirmations)
{
	CKadAICHVotes::Key key{};
	key[0] = 8;
	const CAICHHash kadRoot = MakeRoot(0x55);
	const CAICHHash serverRoot = MakeRoot(0x66);
	std::unique_ptr<CSearchFile> group(
		CSearchFileTestFixture::ServerRow("x.iso", serverRoot, 0x0D0C0B0A));
	for (uint32_t i = 1; i <= 3; ++i) {
		group->AddChild(CSearchFileTestFixture::Result("x.iso", 0x04030200 | i, kadRoot, key));
	}
	CAICHHashSet hashes(nullptr);
	ASSERT_TRUE(group->ApplyAICHEvidence(hashes));
	ASSERT_EQUALS(AICH_UNTRUSTED, hashes.GetStatus()); // three Kad votes against one
	ASSERT_TRUE(hashes.GetMasterHash() == kadRoot);
	hashes.UntrustedHashReceived(serverRoot, 0x04030210); // two against three
	ASSERT_TRUE(hashes.GetMasterHash() == kadRoot);
	hashes.UntrustedHashReceived(serverRoot, 0x04030211);
	hashes.UntrustedHashReceived(serverRoot, 0x04030212); // four against three
	ASSERT_TRUE(hashes.GetMasterHash() == serverRoot);
}

// Two servers naming different roots leave the group with none, and a third report agreeing
// with either one does not revive it, whatever the order.
TEST(AICHSearchResult, ConflictingEd2kRootsAreDroppedInAnyOrder)
{
	const CAICHHash first = MakeRoot(0x71);
	const CAICHHash second = MakeRoot(0x72);
	for (bool firstArrivesFirst : { true, false }) {
		std::unique_ptr<CSearchFile> group(CSearchFileTestFixture::ServerRow(
			"a.iso", firstArrivesFirst ? first : second, 0x0D0C0B0A));
		group->AddChild(CSearchFileTestFixture::ServerRow(
			"b.iso", firstArrivesFirst ? second : first, 0x0D0C0B0B));
		group->AddChild(CSearchFileTestFixture::ServerRow("c.iso", first, 0x0D0C0B0C));
		CAICHHashSet hashes(nullptr);
		ASSERT_FALSE(group->ApplyAICHEvidence(hashes));
		ASSERT_EQUALS(AICH_EMPTY, hashes.GetStatus());
	}
}

// One StoredSearches.met record in CSearchFile::WriteToFile()'s layout. WriteToFile() itself
// needs the live search list, which this harness does not have.
static void WriteStoredEd2kRow(
	CMemFile &out, const wxString &name, const CAICHHash *root, uint32_t serverIP, uint16_t children)
{
	out.WriteHash(CMD4Hash());
	out.WriteUInt32(root ? 2 : 1);
	CTagString(FT_FILENAME, name).WriteTagToFile(&out);
	if (root) {
		CTagString(FT_AICH_HASH, root->GetString()).WriteTagToFile(&out);
	}
	out.WriteUInt8(0); // not Kad
	out.WriteString(wxEmptyString, utf8strRaw);
	out.WriteUInt32(0); // client ID
	out.WriteUInt16(0); // client port
	out.WriteUInt32(serverIP);
	out.WriteUInt16(4661);
	out.WriteUInt32(0); // Kad publish info
	out.WriteUInt16(0); // clients
	out.WriteUInt16(children);
}

// A restored group keeps the root its live group had, even when only a child carried it.
TEST(AICHSearchResult, RestoredGroupKeepsItsEd2kRoot)
{
	const CAICHHash root = MakeRoot(0x81);
	CMemFile stored;
	WriteStoredEd2kRow(stored, "a.iso", nullptr, 0x0D0C0B0A, 1);
	WriteStoredEd2kRow(stored, "b.iso", &root, 0x0D0C0B0B, 0);
	stored.Seek(0, wxFromStart);
	std::unique_ptr<CSearchFile> restored = CSearchFile::LoadFromFile(&stored);
	ASSERT_TRUE(restored != nullptr);
	ASSERT_EQUALS(size_t(1), restored->GetChildren().size());
	CAICHHashSet hashes(nullptr);
	ASSERT_TRUE(restored->ApplyAICHEvidence(hashes));
	ASSERT_EQUALS(AICH_TRUSTED, hashes.GetStatus());
	ASSERT_TRUE(hashes.GetMasterHash() == root);
}
