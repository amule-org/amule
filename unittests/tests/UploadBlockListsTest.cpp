//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include "UploadBlockLists.h"
#include <cstring>

// A peer can re-request any block it still has in flight: at most 24 for aMule peers.
static_assert(kMaxDoneBlocks >= 24, "the done list must cover a peer's in-flight blocks");

using namespace muleunit;
DECLARE_SIMPLE(UploadBlockLists)

namespace
{
Requested_Block_Struct *Block(uint64 index)
{
	Requested_Block_Struct *block = new Requested_Block_Struct;
	std::memset(block, 0, sizeof(*block));
	block->StartOffset = index * 184320;
	block->EndOffset = block->StartOffset + 184320;
	return block;
}

void Free(std::list<Requested_Block_Struct *> &done)
{
	for (Requested_Block_Struct *block : done) {
		delete block;
	}
	done.clear();
}
} // namespace

TEST(UploadBlockLists, KeepsEveryBlockBelowTheLimit)
{
	std::list<Requested_Block_Struct *> done;
	for (uint64 i = 0; i < kMaxDoneBlocks; ++i) {
		PushDoneBlock(done, Block(i));
	}
	ASSERT_EQUALS(kMaxDoneBlocks, done.size());
	ASSERT_EQUALS(static_cast<uint64>(kMaxDoneBlocks - 1), done.front()->StartOffset / 184320);
	ASSERT_EQUALS(static_cast<uint64>(0), done.back()->StartOffset / 184320);
	Free(done);
}

TEST(UploadBlockLists, DropsTheOldestPastTheLimit)
{
	std::list<Requested_Block_Struct *> done;
	const uint64 sent = 10000;
	for (uint64 i = 0; i < sent; ++i) {
		PushDoneBlock(done, Block(i));
	}
	ASSERT_EQUALS(kMaxDoneBlocks, done.size());
	// Newest first: the session-end check reads front().
	uint64 expected = sent - 1;
	for (const Requested_Block_Struct *block : done) {
		ASSERT_EQUALS(expected, block->StartOffset / 184320);
		--expected;
	}
	Free(done);
}
