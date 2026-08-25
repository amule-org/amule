//
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

#include <ec/cpp/ECCodes.h>
#include <ec/cpp/ECMemSocket.h>
#include <ec/cpp/ECSocket.h>

using namespace muleunit;

// m_my_flags mixes two kinds of capability bit, and the difference decides
// which ones a reconnect may drop:
//
//   - chosen locally (EC_FLAG_ZLIB, EC_FLAG_UTF8_NUMBERS), from preferences
//     via SetCapabilities, which the reconnect path does not call again;
//   - agreed with the peer (EC_FLAG_LARGE_TAG_COUNT), set only when the daemon
//     echoes EC_TAG_CAN_LARGE_TAG_COUNT in AUTH_OK.
//
// Getting either half wrong is silent. Left set across a reconnect, the
// negotiated bit makes the client keep sending the extended tag-count format
// to a daemon that never advertised it, and the receiver misparses every tag
// after the count. Clearing too much instead disables compression for the rest
// of the session -- no error either way, just a broken or slow list.
//
// The transition is pure state, so it needs no daemon: CECMemSocket is a
// concrete in-tree CECSocket whose constructor already sets both
// EC_FLAG_UTF8_NUMBERS and EC_FLAG_LARGE_TAG_COUNT.

namespace
{

// m_my_flags is protected, and deliberately so; a subclass is how the tree
// itself reaches it (CECMemSocket's constructor does exactly this).
class CFlagProbe : public CECMemSocket
{
public:
	void Add(uint32_t flags) { m_my_flags |= flags; }
	uint32_t Flags() const { return m_my_flags; }
};

} // namespace

DECLARE_SIMPLE(ECSocketFlags)

// The reason this was not a one-line clear of the whole word.
TEST(ECSocketFlags, ClearPeerNegotiatedKeepsLocalCapabilities)
{
	CFlagProbe socket;
	socket.Add(EC_FLAG_ZLIB);

	// Precondition: all three bits set, as after a completed handshake.
	ASSERT_TRUE((socket.Flags() & EC_FLAG_ZLIB) != 0);
	ASSERT_TRUE((socket.Flags() & EC_FLAG_UTF8_NUMBERS) != 0);
	ASSERT_TRUE((socket.Flags() & EC_FLAG_LARGE_TAG_COUNT) != 0);

	socket.ClearPeerNegotiatedFlags();

	// Only the negotiated bit goes.
	ASSERT_TRUE((socket.Flags() & EC_FLAG_LARGE_TAG_COUNT) == 0);
	ASSERT_TRUE((socket.Flags() & EC_FLAG_ZLIB) != 0);
	ASSERT_TRUE((socket.Flags() & EC_FLAG_UTF8_NUMBERS) != 0);
}

// Every packet carries the version-sanity bit, and ReadPacket rejects one that
// does not -- so the clear must not disturb it.
TEST(ECSocketFlags, ClearPeerNegotiatedKeepsVersionBit)
{
	CFlagProbe socket;
	const uint32_t versionBit = socket.Flags() & 0x60;

	socket.ClearPeerNegotiatedFlags();

	ASSERT_EQUALS(versionBit, socket.Flags() & 0x60);
}

// The clear lives outside ResetProtocolState on purpose. CECMemSocket sets
// EC_FLAG_LARGE_TAG_COUNT as a local property of the wire format it caches,
// not as something a peer agreed to, so a reset that dropped it would silently
// downgrade the cache to the short tag-count format.
TEST(ECSocketFlags, ResetProtocolStateLeavesCapabilitiesAlone)
{
	CFlagProbe socket;
	socket.Add(EC_FLAG_ZLIB);
	const uint32_t before = socket.Flags();

	socket.ResetProtocolState();

	ASSERT_EQUALS(before, socket.Flags());
}

// The tx block size decides how many send() calls a packet costs, but it also
// has two hard constraints that are easy to break while "simplifying" the
// arithmetic: the first block has to leave room for the 8-byte header that
// SealOutputQueue keeps in clear, and the cap has to hold, because a packet may
// legitimately be hundreds of megabytes and a block that size would be copied
// again by the send path.
namespace
{

class CTxChunkProbe : public CECMemSocket
{
public:
	using CECSocket::TxChunkSize;
};

const size_t kFloor = 2048;
const size_t kCap = 64 * 1024;
const size_t kHeader = 8;

} // namespace

TEST(ECSocketFlags, TxChunkSizeNeverDropsBelowTheHeaderFloor)
{
	// A body smaller than the floor still gets the floor, so the header always
	// fits and no packet gets smaller blocks than before the size became
	// per-packet.
	ASSERT_EQUALS(kFloor, CTxChunkProbe::TxChunkSize(0));
	ASSERT_EQUALS(kFloor, CTxChunkProbe::TxChunkSize(1));
	ASSERT_EQUALS(kFloor, CTxChunkProbe::TxChunkSize(kFloor - kHeader));
	ASSERT_TRUE(CTxChunkProbe::TxChunkSize(0) >= kHeader);
}

TEST(ECSocketFlags, TxChunkSizeCoversBodyPlusHeaderInBetween)
{
	// Between the floor and the cap the block is exactly what the packet needs,
	// body plus the 8-byte header, so the whole thing is one block.
	ASSERT_EQUALS(kFloor + 1, CTxChunkProbe::TxChunkSize(kFloor - kHeader + 1));
	ASSERT_EQUALS((size_t)10000 + kHeader, CTxChunkProbe::TxChunkSize(10000));
	ASSERT_EQUALS(kCap - 1, CTxChunkProbe::TxChunkSize(kCap - 1 - kHeader));
}

TEST(ECSocketFlags, TxChunkSizeStopsAtTheCap)
{
	// At and past the cap the block stops growing: a 256 MB packet must not
	// become a 256 MB contiguous allocation.
	ASSERT_EQUALS(kCap, CTxChunkProbe::TxChunkSize(kCap - kHeader));
	ASSERT_EQUALS(kCap, CTxChunkProbe::TxChunkSize(kCap));
	ASSERT_EQUALS(kCap, CTxChunkProbe::TxChunkSize(4 * 1024 * 1024));
	ASSERT_EQUALS(kCap, CTxChunkProbe::TxChunkSize(256u * 1024 * 1024));
}
