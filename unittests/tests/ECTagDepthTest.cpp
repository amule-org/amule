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
#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECSocket.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

using namespace muleunit;

namespace
{

// Serves a recorded reply to SendRecvPacket and discards the request.
class CReplaySocket : public CECSocket
{
public:
	explicit CReplaySocket(std::vector<unsigned char> reply)
	: CECSocket(false)
	, m_reply(std::move(reply))
	{
	}

private:
	std::vector<unsigned char> m_reply;
	size_t m_pos = 0;

	void WriteDoneAndQueueEmpty() override {}
	bool InternalConnect(uint32_t, uint16_t, bool) override { return true; }
	bool InternalWaitOnConnect(long, long) override { return true; }
	bool InternalWaitForWrite(long, long) override { return true; }
	bool InternalWaitForRead(long, long) override { return true; }
	int InternalGetLastError() override { return 0; }
	void InternalClose() override {}
	bool InternalError() override { return false; }
	uint32 InternalRead(void *ptr, uint32 len) override
	{
		const uint32 n = std::min<size_t>(len, m_reply.size() - m_pos);
		memcpy(ptr, m_reply.data() + m_pos, n);
		m_pos += n;
		return n;
	}
	uint32 InternalWrite(const void *, uint32 len) override { return len; }
	bool InternalIsConnected() override { return true; }
	void InternalDestroy() override {}
};

// A packet holding one chain of tags nested @a depth levels deep.
std::vector<unsigned char> NestedReply(unsigned depth)
{
	CECTag chain(EC_TAG_STRING, wxString("leaf"));
	for (unsigned i = 1; i < depth; ++i) {
		CECTag parent(EC_TAG_STRING, wxString("node"));
		parent.AddTag(chain);
		chain = parent;
	}
	CECPacket reply(EC_OP_NOOP);
	reply.AddTag(chain);

	CECMemSocket wire;
	wire.SendPacket(&reply);
	return wire.TakeBytes();
}

bool Parses(unsigned depth)
{
	CReplaySocket socket(NestedReply(depth));
	const CECPacket request(EC_OP_NOOP);
	std::unique_ptr<const CECPacket> reply(socket.SendRecvPacket(&request));
	return reply != nullptr;
}

} // namespace

DECLARE_SIMPLE(ECTagDepth)

TEST(ECTagDepth, ShallowTreeParses)
{
	ASSERT_TRUE(Parses(8));
}

TEST(ECTagDepth, LimitIsSixtyFourLevels)
{
	ASSERT_TRUE(Parses(64));
	ASSERT_FALSE(Parses(65));
}

// The parser recursed once per level with no bound, so a deep enough tree
// exhausted the stack before any check ran.
TEST(ECTagDepth, DeepTreeIsRejected)
{
	ASSERT_FALSE(Parses(1000));
}
