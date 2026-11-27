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
		uint32_t publishTime = 1; // 0: never published.
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
	static uint32_t PublishTime(const SFile &file) { return file.publishTime; }
	static bool Publish(SFile &file)
	{
		if (!file.due) {
			return false;
		}
		file.due = false;
		file.publishTime = Republish;
		return true;
	}
	static constexpr uint32_t Republish = 100000;
};

int NextKey(std::map<int, uint32_t> &waits, uint32_t now, SFakeFiles &files)
{
	Kademlia::CKadPublishOrder<int> order;
	const SFakeFiles::SFile *file = Kademlia::NextKadSourcePublish(waits, order, now, 100, files);
	return file ? file->key : 0;
}

int NextKey(Kademlia::CKadPublishOrder<int> &order, uint32_t now, SFakeFiles &files)
{
	std::map<int, uint32_t> waits;
	const SFakeFiles::SFile *file = Kademlia::NextKadSourcePublish(waits, order, now, 100, files);
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
		uint32_t publishTime = 1; // 0: never published.
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
	SKeyword *Find(int key)
	{
		for (SKeyword &keyword : list) {
			if (keyword.key == key) {
				return &keyword;
			}
		}
		return nullptr;
	}
	static int Key(const SKeyword &keyword) { return keyword.key; }
	static uint32_t PublishTime(const SKeyword &keyword) { return keyword.publishTime; }
	static bool Due(const SKeyword &keyword, uint32_t) { return keyword.due; }
	bool Start(SKeyword &keyword, uint32_t)
	{
		if (!keyword.starts) {
			return false;
		}
		++started;
		keyword.due = false;
		keyword.publishTime = 100000;
		return true;
	}
};

int NextKeyword(SFakeKeywords &keywords, size_t maxChecks = 100)
{
	Kademlia::CKadPublishOrder<int> order;
	const SFakeKeywords::SKeyword *keyword =
		Kademlia::NextKeywordPublish(order, 1000, maxChecks, keywords);
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

TEST(KadCallbackPolicy, ADueRepublishGoesBeforeAFirstPublish)
{
	SFakeFiles files;
	files.shared = { { 1, true, false, 0 }, { 2, true, false, 950 }, { 3, true, false, 900 } };
	files.cursor = { 1, 2, 3 };
	Kademlia::CKadPublishOrder<int> order;
	order.Add(950, 2);
	order.Add(900, 3);
	// The most overdue first, then the other, then the cursor's file never published.
	ASSERT_EQUALS(3, NextKey(order, 1000, files));
	ASSERT_EQUALS(2, NextKey(order, 1000, files));
	ASSERT_EQUALS(1, NextKey(order, 1000, files));
}

TEST(KadCallbackPolicy, AnOrderEntryForAFileRepublishedSinceIsSkipped)
{
	SFakeFiles files;
	files.shared = { { 2, false, false, 5000 } };
	Kademlia::CKadPublishOrder<int> order;
	order.Add(900, 2);
	ASSERT_EQUALS(0, NextKey(order, 1000, files));
}

TEST(KadCallbackPolicy, AnOrderEntryForAFileNoLongerSharedIsSkipped)
{
	SFakeFiles files;
	files.shared = { { 2, true, false, 900 } };
	files.cursor = { 2 };
	Kademlia::CKadPublishOrder<int> order;
	order.Add(800, 1);
	ASSERT_EQUALS(2, NextKey(order, 1000, files));
}

// A route change makes a file due before its publish time; only the cursor finds it.
TEST(KadCallbackPolicy, AFileDueEarlyIsFoundByTheCursor)
{
	SFakeFiles files;
	files.shared = { { 5, true, false, 50000 } };
	files.cursor = { 5 };
	Kademlia::CKadPublishOrder<int> order;
	order.Add(50000, 5);
	ASSERT_EQUALS(5, NextKey(order, 1000, files));
}

TEST(KadCallbackPolicy, ADueFileFromTheOrderWaitsForItsSeeds)
{
	SFakeFiles files;
	files.shared = { { 2, true, true, 900 } };
	Kademlia::CKadPublishOrder<int> order;
	order.Add(900, 2);
	std::map<int, uint32_t> waits;
	ASSERT_TRUE(Kademlia::NextKadSourcePublish(waits, order, 1000, 100, files) == nullptr);
	ASSERT_EQUALS(1000u, waits[2]);
	ASSERT_EQUALS(1, files.waited);
}

TEST(KadCallbackPolicy, EveryTenthSourceStartIsAFirstPublish)
{
	SFakeFiles files;
	for (int key = 1; key <= 20; ++key) {
		files.shared.push_back({ key, true, false, key <= 15 ? static_cast<uint32_t>(key) : 0 });
		files.cursor.push_back(key);
	}
	Kademlia::CKadPublishOrder<int> order;
	for (int key = 1; key <= 15; ++key) {
		order.Add(key, key);
	}
	std::vector<int> keys;
	for (int i = 0; i < 20; ++i) {
		keys.push_back(NextKey(order, 1000, files));
	}
	const std::vector<int> want = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20
	};
	ASSERT_TRUE(keys == want);
}

// With no file left that was never published, republishes take the turn, and it stays for the
// next new file.
TEST(KadCallbackPolicy, AFirstPublishTurnStaysUntilAFileNeverPublishedStarts)
{
	SFakeFiles files;
	Kademlia::CKadPublishOrder<int> order;
	for (int key = 1; key <= 11; ++key) {
		files.shared.push_back({ key, true, false, static_cast<uint32_t>(key) });
		files.cursor.push_back(key);
		order.Add(key, key);
	}
	for (int key = 1; key <= 10; ++key) {
		ASSERT_EQUALS(key, NextKey(order, 1000, files));
	}
	ASSERT_TRUE(order.FirstPublishTurn());
	files.shared.push_back({ 12, true, false, 0 });
	files.cursor.push_back(12);
	ASSERT_EQUALS(12, NextKey(order, 1000, files));
	ASSERT_FALSE(order.FirstPublishTurn());
	ASSERT_EQUALS(11, NextKey(order, 1000, files));
}

namespace
{
int NextKeyword(Kademlia::CKadPublishOrder<int> &order, SFakeKeywords &keywords)
{
	const SFakeKeywords::SKeyword *keyword = Kademlia::NextKeywordPublish(order, 1000, 100, keywords);
	return keyword ? keyword->key : 0;
}
} // namespace

TEST(KadCallbackPolicy, ADueKeywordRepublishGoesBeforeAFirstPublish)
{
	SFakeKeywords keywords;
	keywords.list = { { 1, true, true, 0 }, { 2, true, true, 950 }, { 3, true, true, 900 } };
	Kademlia::CKadPublishOrder<int> order;
	order.Add(950, 2);
	order.Add(900, 3);
	ASSERT_EQUALS(3, NextKeyword(order, keywords));
	ASSERT_EQUALS(2, NextKeyword(order, keywords));
	ASSERT_EQUALS(1, NextKeyword(order, keywords));
}

TEST(KadCallbackPolicy, AnOrderEntryForAKeywordRepublishedSinceIsSkipped)
{
	SFakeKeywords keywords;
	keywords.list = { { 2, false, true, 5000 } };
	Kademlia::CKadPublishOrder<int> order;
	order.Add(900, 2);
	ASSERT_EQUALS(0, NextKeyword(order, keywords));
}

// Its nodes reported high load: it leaves the order, and the cursor tries it again.
TEST(KadCallbackPolicy, AKeywordThatFailsToStartIsRetriedByTheCursor)
{
	SFakeKeywords keywords;
	keywords.list = { { 2, true, false, 900 } };
	Kademlia::CKadPublishOrder<int> order;
	order.Add(900, 2);
	ASSERT_EQUALS(0, NextKeyword(order, keywords));
	keywords.list[0].starts = true;
	ASSERT_EQUALS(2, NextKeyword(order, keywords));
}

TEST(KadCallbackPolicy, EveryTenthKeywordStartIsAFirstPublish)
{
	SFakeKeywords keywords;
	for (int key = 1; key <= 12; ++key) {
		keywords.list.push_back({ key, true, true, key <= 10 ? static_cast<uint32_t>(key) : 0 });
	}
	Kademlia::CKadPublishOrder<int> order;
	for (int key = 1; key <= 10; ++key) {
		order.Add(key, key);
	}
	std::vector<int> keys;
	for (int i = 0; i < 12; ++i) {
		keys.push_back(NextKeyword(order, keywords));
	}
	const std::vector<int> want = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 10, 12 };
	ASSERT_TRUE(keys == want);
}

TEST(KadCallbackPolicy, ASmallLibraryKeepsTheStockPublishSlots)
{
	ASSERT_EQUALS(3u, Kademlia::KadPublishSlotsNeeded(0, Kademlia::KadFilesPerSourceSlot, 3));
	ASSERT_EQUALS(3u, Kademlia::KadPublishSlotsNeeded(4500, Kademlia::KadFilesPerSourceSlot, 3));
	ASSERT_EQUALS(2u, Kademlia::KadPublishSlotsNeeded(9000, Kademlia::KadKeywordsPerKeywordSlot, 2));
}

TEST(KadCallbackPolicy, PublishSlotsGrowWithTheLibrary)
{
	ASSERT_EQUALS(4u, Kademlia::KadPublishSlotsNeeded(4501, Kademlia::KadFilesPerSourceSlot, 3));
	ASSERT_EQUALS(14u, Kademlia::KadPublishSlotsNeeded(20000, Kademlia::KadFilesPerSourceSlot, 3));
	ASSERT_EQUALS(3u, Kademlia::KadPublishSlotsNeeded(10771, Kademlia::KadKeywordsPerKeywordSlot, 2));
}

TEST(KadCallbackPolicy, TheSettingCapsThePublishSlotsAndLogsOncePerCap)
{
	bool capped = false;
	int logs = 0;
	const auto log = [&logs] { ++logs; };
	ASSERT_EQUALS(10u, Kademlia::CappedKadPublishSlots(10, 12, capped, log));
	ASSERT_EQUALS(12u, Kademlia::CappedKadPublishSlots(14, 12, capped, log));
	ASSERT_EQUALS(12u, Kademlia::CappedKadPublishSlots(14, 12, capped, log));
	ASSERT_EQUALS(1, logs);
	ASSERT_EQUALS(12u, Kademlia::CappedKadPublishSlots(12, 12, capped, log));
	ASSERT_EQUALS(12u, Kademlia::CappedKadPublishSlots(13, 12, capped, log));
	ASSERT_EQUALS(2, logs);
}

TEST(KadCallbackPolicy, MoreSlotsStartMoreLookupsPerTick)
{
	const uint32_t source = Kademlia::KadSourceSlotsPerStart;
	ASSERT_EQUALS(1u, Kademlia::KadPublishStartsNow(3, 0, source));
	ASSERT_EQUALS(2u, Kademlia::KadPublishStartsNow(4, 0, source));
	ASSERT_EQUALS(2u, Kademlia::KadPublishStartsNow(6, 0, source));
	ASSERT_EQUALS(3u, Kademlia::KadPublishStartsNow(8, 0, source));
	ASSERT_EQUALS(4u, Kademlia::KadPublishStartsNow(12, 0, source));
	// The default keyword cap keeps one start per tick, as tested live.
	const uint32_t keyword = Kademlia::KadKeywordSlotsPerStart;
	ASSERT_EQUALS(1u, Kademlia::KadPublishStartsNow(2, 0, keyword));
	ASSERT_EQUALS(1u, Kademlia::KadPublishStartsNow(8, 0, keyword));
	ASSERT_EQUALS(2u, Kademlia::KadPublishStartsNow(9, 0, keyword));
	ASSERT_EQUALS(4u, Kademlia::KadPublishStartsNow(32, 0, keyword));
}

TEST(KadCallbackPolicy, AStartNeverTakesMoreThanTheFreeSlots)
{
	const uint32_t source = Kademlia::KadSourceSlotsPerStart;
	ASSERT_EQUALS(1u, Kademlia::KadPublishStartsNow(12, 11, source));
	ASSERT_EQUALS(0u, Kademlia::KadPublishStartsNow(12, 12, source));
	// The library shrank under the running lookups.
	ASSERT_EQUALS(0u, Kademlia::KadPublishStartsNow(3, 5, source));
}
