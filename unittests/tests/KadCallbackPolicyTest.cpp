// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <KadCallbackPolicy.h>

#include <map>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(KadCallbackPolicy)

TEST(KadCallbackPolicy, ReachabilityTruthTable)
{
	for (bool tcpFirewalled : { false, true }) {
		for (bool udpFirewalled : { false, true }) {
			for (bool udpVerified : { false, true }) {
				const bool direct = Kademlia::DirectCallbackAvailable(
					tcpFirewalled, udpFirewalled, udpVerified);
				const bool buddy =
					Kademlia::NeedsBuddy(tcpFirewalled, udpFirewalled, udpVerified);
				if (!tcpFirewalled) {
					ASSERT_FALSE(direct);
					ASSERT_FALSE(buddy);
				} else if (!udpFirewalled && udpVerified) {
					ASSERT_TRUE(direct);
					ASSERT_FALSE(buddy);
				} else {
					ASSERT_FALSE(direct);
					ASSERT_TRUE(buddy);
				}
			}
		}
	}
}

TEST(KadCallbackPolicy, BuddyToDirectRouteBypassesPublishTimer)
{
	const uint32_t now = 1000;
	const uint32_t lastBuddy = 0x12345678;
	const uint32_t nextPublish = now + 5 * 60 * 60;
	ASSERT_FALSE(Kademlia::CanPublishSource(true, lastBuddy, lastBuddy, nextPublish, now, 4665, 4665));
	// UDP becomes verified open, and ClientList drops the old buddy.
	ASSERT_TRUE(Kademlia::CanPublishSource(false, 0, lastBuddy, nextPublish, now + 1, 0, 4665));
	// Once the new route is published, the regular throttle applies again.
	ASSERT_FALSE(Kademlia::CanPublishSource(false, 0, 0, nextPublish, now + 2, 0, 0));
}

TEST(KadCallbackPolicy, DirectToBuddyAndChangedBuddyBypassPublishTimer)
{
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 123, 0, 5000, 1000, 4665, 0));
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 456, 123, 5000, 1000, 4665, 4665));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 456, 456, 5000, 1000, 4665, 4665));
}

TEST(KadCallbackPolicy, NoRouteNeverStartsAPublish)
{
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 0, 123, 5000, 1000, 0, 4665));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 0, 123, 0, 1000, 0, 4665));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 0, 0, 0, 1000, 0, 0));
}

TEST(KadCallbackPolicy, ChangedBuddyPortBypassesPublishTimer)
{
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 123, 123, 5000, 1000, 4665, 4665));
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 123, 123, 5000, 1000, 4675, 4665));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 123, 123, 5000, 1000, 4675, 4675));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 0, 123, 5000, 1000, 4675, 4665));
	ASSERT_TRUE(Kademlia::CanPublishSource(false, 0, 123, 5000, 1000, 0, 4665));
}

TEST(KadCallbackPolicy, RepublishBoundaryAndAbortedPublishRetry)
{
	ASSERT_FALSE(Kademlia::CanPublishSource(false, 0, 0, 1000, 999, 0, 0));
	ASSERT_TRUE(Kademlia::CanPublishSource(false, 0, 0, 1000, 1000, 0, 0));
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 123, 123, 1000, 1000, 4665, 4665));
	// Search cleared the timestamp after losing its callback route.
	ASSERT_TRUE(Kademlia::CanPublishSource(false, 0, 0, 0, 1000, 0, 0));
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 123, 0, 0, 1000, 4665, 0));
}

namespace
{
constexpr uint32_t MaxWait = 300;

bool Waits(uint32_t nextPublish,
	uint32_t now,
	uint32_t waitingSince,
	bool seedOutOfBudget,
	bool *asked = nullptr)
{
	return Kademlia::KadSourcePublishWaits(nextPublish, now, waitingSince, MaxWait, [&] {
		if (asked) {
			*asked = true;
		}
		return seedOutOfBudget;
	});
}
} // namespace

TEST(KadCallbackPolicy, ADueSourcePublishWaitsOnlyForASeedOutOfBudget)
{
	ASSERT_TRUE(Waits(1000, 1000, 0, true));
	ASSERT_FALSE(Waits(1000, 1000, 0, false));
	// Seeds that have budget again end the wait.
	ASSERT_FALSE(Waits(1000, 1100, 1000, false));
}

// A file due early after a buddy change publishes its new route at once, without asking the seeds.
TEST(KadCallbackPolicy, AFileNotDueNeverWaits)
{
	bool asked = false;
	ASSERT_FALSE(Waits(5000, 1000, 0, true, &asked));
	ASSERT_FALSE(asked);
}

TEST(KadCallbackPolicy, ASourcePublishWaitsAtMostMaxWait)
{
	ASSERT_TRUE(Waits(1000, 1000 + MaxWait - 1, 1000, true));
	bool asked = false;
	ASSERT_FALSE(Waits(1000, 1000 + MaxWait, 1000, true, &asked));
	ASSERT_FALSE(asked);
}

namespace
{
// Shared files, each due or not, and with a seed out of budget or not.
struct SFakeFiles
{
	struct SFile
	{
		int key;
		bool due;
		bool seedOutOfBudget;
	};
	std::vector<SFile> shared;
	std::vector<int> cursor; // Keys the cursor returns, in order.
	size_t next = 0;
	int waited = 0;

	SFile *Find(int key)
	{
		for (SFile &file : shared) {
			if (file.key == key) {
				return &file;
			}
		}
		return nullptr;
	}
	SFile *Next() { return next < cursor.size() ? Find(cursor[next++]) : nullptr; }
	static int Key(const SFile &file) { return file.key; }
	static bool Waits(const SFile &file, uint32_t now, uint32_t waitingSince)
	{
		return Kademlia::KadSourcePublishWaits(
			file.due ? 0 : now + 1, now, waitingSince, MaxWait, [&file] {
				return file.seedOutOfBudget;
			});
	}
	void Waited(const SFile &) { ++waited; }
	static bool Publish(const SFile &file) { return file.due; }
};

int NextKey(std::map<int, uint32_t> &waits, uint32_t now, SFakeFiles &files)
{
	const SFakeFiles::SFile *file = Kademlia::NextKadSourcePublish(waits, now, 100, files);
	return file ? file->key : 0;
}
} // namespace

TEST(KadCallbackPolicy, AWaitingFileIsCheckedFirstOnTheNextTick)
{
	SFakeFiles files;
	files.shared = { { 1, true, false }, { 2, true, false } };
	files.cursor = { 2 };
	std::map<int, uint32_t> waits = { { 1, 1000 } };
	// File 1's seeds take stores again: it goes before the cursor's file 2.
	ASSERT_EQUALS(1, NextKey(waits, 1010, files));
	ASSERT_TRUE(waits.empty());
}

TEST(KadCallbackPolicy, AWaitingFileNoLongerSharedIsDropped)
{
	SFakeFiles files;
	files.shared = { { 2, true, false } };
	files.cursor = { 2 };
	std::map<int, uint32_t> waits = { { 1, 1000 } };
	ASSERT_EQUALS(2, NextKey(waits, 1010, files));
	ASSERT_TRUE(waits.empty());
}

TEST(KadCallbackPolicy, TheCursorStepsOverAWaitingFile)
{
	SFakeFiles files;
	files.shared = { { 1, true, true }, { 2, true, false } };
	files.cursor = { 1, 2 };
	std::map<int, uint32_t> waits = { { 1, 1000 } };
	ASSERT_EQUALS(2, NextKey(waits, 1010, files));
	ASSERT_EQUALS(1u, waits.size());
	ASSERT_EQUALS(1000u, waits[1]);
	ASSERT_EQUALS(0, files.waited);
}

TEST(KadCallbackPolicy, AWaitCountsOnce)
{
	SFakeFiles files;
	files.shared = { { 1, true, true } };
	files.cursor = { 1, 1 };
	std::map<int, uint32_t> waits;
	ASSERT_EQUALS(0, NextKey(waits, 1000, files));
	ASSERT_EQUALS(1000u, waits[1]);
	ASSERT_EQUALS(0, NextKey(waits, 1100, files));           // Still waiting, not counted again.
	ASSERT_EQUALS(1, NextKey(waits, 1000 + MaxWait, files)); // Its 5 minutes are up.
	ASSERT_EQUALS(1, files.waited);
}

namespace
{
// Keywords in list order, each due or not, and whose publish starts or not.
struct SFakeKeywords
{
	struct SKeyword
	{
		int key;
		bool due;
		bool starts;
	};
	std::vector<SKeyword> list;
	size_t cursor = 0;
	int started = 0;

	SKeyword *Next()
	{
		if (list.empty()) {
			return nullptr;
		}
		SKeyword *keyword = &list[cursor];
		cursor = (cursor + 1) % list.size();
		return keyword;
	}
	static bool Due(const SKeyword &keyword, uint32_t) { return keyword.due; }
	bool Start(const SKeyword &keyword, uint32_t)
	{
		started += keyword.starts ? 1 : 0;
		return keyword.starts;
	}
};

int NextKeyword(SFakeKeywords &keywords, size_t maxChecks = 100)
{
	const SFakeKeywords::SKeyword *keyword = Kademlia::NextKeywordPublish(1000, maxChecks, keywords);
	return keyword ? keyword->key : 0;
}
} // namespace

TEST(KadCallbackPolicy, AKeywordPublishPassesOverKeywordsNotDue)
{
	SFakeKeywords keywords;
	keywords.list = { { 1, false, true }, { 2, false, true }, { 3, true, true } };
	ASSERT_EQUALS(3, NextKeyword(keywords));
}

// A lookup for it is running, its nodes reported high load, or it has no complete files.
TEST(KadCallbackPolicy, AKeywordPublishPassesOverDueKeywordsThatCannotStart)
{
	SFakeKeywords keywords;
	keywords.list = { { 1, true, false }, { 2, true, true } };
	ASSERT_EQUALS(2, NextKeyword(keywords));
}

TEST(KadCallbackPolicy, AKeywordPublishStartsOnePerTickAndResumesAfterIt)
{
	SFakeKeywords keywords;
	keywords.list = { { 1, true, true }, { 2, true, true }, { 3, false, true } };
	ASSERT_EQUALS(1, NextKeyword(keywords));
	ASSERT_EQUALS(1, keywords.started);
	ASSERT_EQUALS(2, NextKeyword(keywords));
	ASSERT_EQUALS(2, keywords.started);
}

TEST(KadCallbackPolicy, AKeywordPublishChecksAtMostMaxChecks)
{
	SFakeKeywords keywords;
	keywords.list = { { 1, false, true }, { 2, false, true }, { 3, true, true } };
	ASSERT_EQUALS(0, NextKeyword(keywords, 2));
	ASSERT_EQUALS(3, NextKeyword(keywords, 2)); // The next tick goes on from keyword 3.
}
