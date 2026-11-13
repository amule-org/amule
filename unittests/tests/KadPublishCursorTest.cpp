// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include "KadPublishCursor.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(KadPublishCursor)

namespace
{
std::map<int, int> Files(int count)
{
	std::map<int, int> files;
	for (int i = 0; i < count; ++i) {
		files.emplace(i, 0);
	}
	return files;
}

std::vector<int> Pass(CKadPublishCursor<int> &cursor, const std::map<int, int> &files)
{
	std::vector<int> keys;
	for (size_t i = 0; i < files.size(); ++i) {
		keys.push_back(cursor.Next(files)->first);
	}
	return keys;
}
} // namespace

TEST(KadPublishCursor, EmptyAndSingleFile)
{
	std::map<int, int> files;
	CKadPublishCursor<int> cursor;
	ASSERT_TRUE(cursor.Next(files) == files.end());
	files.emplace(0, 0);
	for (int i = 0; i < 3; ++i) {
		ASSERT_EQUALS(0, cursor.Next(files)->first);
	}
}

TEST(KadPublishCursor, EveryFileOncePerPassInAStableOrder)
{
	const auto files = Files(60000);
	CKadPublishCursor<int> cursor;
	const auto first = Pass(cursor, files);
	ASSERT_EQUALS(files.size(), std::set<int>(first.begin(), first.end()).size());
	// The next pass repeats the order, so files keep their spacing as they come due again.
	ASSERT_TRUE(first == Pass(cursor, files));
}

TEST(KadPublishCursor, NearHashesArePublishedFarApart)
{
	// Keys close in hash order share their leading bits, and so their Kad storing nodes.
	for (int count : { 1000, 1646, 60000 }) {
		const auto files = Files(count);
		CKadPublishCursor<int> cursor;
		const auto keys = Pass(cursor, files);
		std::vector<int> slot(count);
		for (int i = 0; i < count; ++i) {
			slot[keys[i]] = i;
		}
		for (int key = 0; key + 3 < count; ++key) {
			for (int distance = 1; distance <= 3; ++distance) {
				const int gap = std::abs(slot[key] - slot[key + distance]);
				const int apart = std::min(gap, count - gap); // Passes repeat.
				ASSERT_TRUE(apart >= (distance == 1 ? count * 38 / 100 : count * 14 / 100));
			}
		}
	}
}

TEST(KadPublishCursor, AddAndRemoveDuringPass)
{
	std::map<int, int> files{ { 10, 0 }, { 20, 0 }, { 30, 0 } };
	CKadPublishCursor<int> cursor;
	const int first = cursor.Next(files)->first;
	files.erase(first);  // Removing the last selected file must not invalidate the cursor.
	files.emplace(5, 0); // A new file waits for the next pass.
	std::set<int> rest{ cursor.Next(files)->first, cursor.Next(files)->first };
	ASSERT_TRUE(rest.count(5) == 0 && rest.count(first) == 0);
	const auto next = Pass(cursor, files);
	ASSERT_EQUALS(size_t(3), std::set<int>(next.begin(), next.end()).size());
	ASSERT_TRUE(std::set<int>(next.begin(), next.end()).count(5) == 1);
}

TEST(KadPublishCursor, UnvisitedRemovedFilesAreSkipped)
{
	std::map<int, int> files{ { 10, 0 }, { 20, 0 }, { 30, 0 } };
	CKadPublishCursor<int> cursor;
	const int kept = cursor.Next(files)->first;
	for (int key : { 10, 20, 30 }) {
		if (key != kept) {
			files.erase(key);
		}
	}
	const auto next = cursor.Next(files); // Both files left in this pass are gone.
	ASSERT_TRUE(next != files.end());
	ASSERT_EQUALS(kept, next->first);
}

TEST(KadPublishCursor, ClearAndReload)
{
	std::map<int, int> files{ { 10, 0 } };
	CKadPublishCursor<int> cursor;
	ASSERT_EQUALS(10, cursor.Next(files)->first);
	files.clear();
	ASSERT_TRUE(cursor.Next(files) == files.end());
	files.emplace(5, 0);
	ASSERT_EQUALS(5, cursor.Next(files)->first);
}

TEST(KadPublishCursor, SourceAndNotesAdvanceIndependently)
{
	const std::map<int, int> files{ { 10, 0 }, { 20, 0 }, { 30, 0 } };
	CKadPublishCursor<int> source;
	CKadPublishCursor<int> notes;
	const int sourceFirst = source.Next(files)->first;
	source.Next(files);
	ASSERT_EQUALS(sourceFirst, notes.Next(files)->first); // Notes start their own pass.
}
