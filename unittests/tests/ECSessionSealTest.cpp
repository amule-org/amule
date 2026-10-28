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
#include <ec/cpp/ECCrypt.h>
#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECSocket.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

using namespace muleunit;

// Once a session has received a sealed packet, a cleartext one can only have been injected. The
// synchronous SendRecvPacket path (amulecmd, amuleweb, amuleapi) must reject it like the async
// client and the daemon do, while a failed login's clear AUTH_FAIL, which arrives before any
// sealed packet, must still get through.

DECLARE_SIMPLE(ECSessionSeal)

namespace
{

// A synchronous CECSocket over two byte buffers: what it writes is captured, what it reads is
// whatever the test fed it.
class CLoopSocket : public CECSocket
{
public:
	CLoopSocket()
	: CECSocket(false)
	{
		m_my_flags |= EC_FLAG_UTF8_NUMBERS | EC_FLAG_LARGE_TAG_COUNT;
		SetTxFlags(m_my_flags);
	}

	std::vector<unsigned char> TakeWritten()
	{
		std::vector<unsigned char> out;
		out.swap(m_out);
		return out;
	}
	void Feed(const std::vector<unsigned char> &bytes)
	{
		m_in.insert(m_in.end(), bytes.begin(), bytes.end());
	}
	bool Lost() const { return m_lost; }

private:
	std::vector<unsigned char> m_in;
	std::vector<unsigned char> m_out;
	size_t m_pos = 0;
	bool m_lost = false;

	void WriteDoneAndQueueEmpty() override {}
	bool InternalConnect(uint32_t, uint16_t, bool) override { return true; }
	bool InternalWaitOnConnect(long, long) override { return true; }
	bool InternalWaitForWrite(long, long) override { return true; }
	bool InternalWaitForRead(long, long) override { return m_pos < m_in.size(); }
	int InternalGetLastError() override { return 0; }
	void InternalClose() override {}
	bool InternalError() override { return false; }
	uint32 InternalRead(void *ptr, uint32 len) override
	{
		const size_t n = std::min<size_t>(len, m_in.size() - m_pos);
		std::memcpy(ptr, m_in.data() + m_pos, n);
		m_pos += n;
		return static_cast<uint32>(n);
	}
	uint32 InternalWrite(const void *ptr, uint32 len) override
	{
		const unsigned char *p = static_cast<const unsigned char *>(ptr);
		m_out.insert(m_out.end(), p, p + len);
		return len;
	}
	bool InternalIsConnected() override { return true; }
	void InternalDestroy() override {}
	void OnLost() override { m_lost = true; }
};

void Pair(CLoopSocket &client, CLoopSocket &daemon)
{
	const uint8_t cipher = ECCrypt::SupportedCiphers()[0];
	const std::vector<uint8_t> ikm(32, 7);
	const std::vector<uint8_t> sn(ECCrypt::NONCE_TAG_LEN, 1);
	const std::vector<uint8_t> cn(ECCrypt::NONCE_TAG_LEN, 2);
	const std::vector<uint8_t> transcript(10, 3);
	ASSERT_TRUE(client.SetupAEAD(cipher, ikm, sn, cn, transcript, false));
	ASSERT_TRUE(daemon.SetupAEAD(cipher, ikm, sn, cn, transcript, true));
	client.EnableAEADNow();
	daemon.EnableAEADNow();
}

// The wire bytes of `packet` as the daemon sends it (sealed) or as an unkeyed socket does (clear).
std::vector<unsigned char> Wire(CLoopSocket &from, ec_opcode_t opcode)
{
	CECPacket packet(opcode);
	from.SendPacket(&packet);
	return from.TakeWritten();
}

// One synchronous request; the reply's opcode, or -1 when the client rejected it.
int Exchange(CLoopSocket &client)
{
	CECPacket request(EC_OP_STAT_REQ);
	std::unique_ptr<const CECPacket> reply(client.SendRecvPacket(&request));
	return reply ? reply->GetOpCode() : -1;
}

} // namespace

TEST(ECSessionSeal, ClearReplyAfterSealedIsRejected)
{
	CLoopSocket client, daemon, intruder;
	Pair(client, daemon);
	client.Feed(Wire(daemon, EC_OP_NOOP));
	client.Feed(Wire(intruder, EC_OP_FAILED));

	ASSERT_EQUALS(static_cast<int>(EC_OP_NOOP), Exchange(client));
	ASSERT_EQUALS(-1, Exchange(client));
	ASSERT_TRUE(client.Lost());
}

TEST(ECSessionSeal, ClearReplyBeforeAnySealedIsAccepted)
{
	CLoopSocket client, daemon, plain;
	Pair(client, daemon);
	client.Feed(Wire(plain, EC_OP_AUTH_FAIL));

	ASSERT_EQUALS(static_cast<int>(EC_OP_AUTH_FAIL), Exchange(client));
	ASSERT_FALSE(client.Lost());
}

TEST(ECSessionSeal, SealedRepliesKeepFlowing)
{
	CLoopSocket client, daemon;
	Pair(client, daemon);
	client.Feed(Wire(daemon, EC_OP_NOOP));
	client.Feed(Wire(daemon, EC_OP_MISC_DATA));

	ASSERT_EQUALS(static_cast<int>(EC_OP_NOOP), Exchange(client));
	ASSERT_EQUALS(static_cast<int>(EC_OP_MISC_DATA), Exchange(client));
	ASSERT_FALSE(client.Lost());
}
