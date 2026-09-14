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
#include "UtpContext.h"
#include "Packet.h"
#include "libs/common/Format.h"
#include <vector>

using namespace muleunit;
DECLARE_SIMPLE(UtpContext)

namespace
{
struct State
{
	int creates = 0, destroys = 0, receives = 0, acks = 0, timeouts = 0;
	bool claimed = true, createSucceeds = true;
	uint32_t ip = 0;
	uint16_t port = 0;
	std::vector<uint8_t> payload;
	IUtpDatagramSink *sink = nullptr;
	IUtpStreamAcceptor *acceptor = nullptr;
	CUtpPeerRegistry registered;
};

class CFakeAcceptor : public IUtpStreamAcceptor
{
public:
	bool AcceptStream(std::unique_ptr<IStreamTransport> &, uint32_t, uint16_t) override { return false; }
};

class FakeLibrary : public IUtpLibrary
{
public:
	explicit FakeLibrary(State &state)
	: s(state)
	{
	}
	bool Create(IUtpDatagramSink &sink) override
	{
		++s.creates;
		s.sink = &sink;
		return s.createSucceeds;
	}
	void SetAcceptor(IUtpStreamAcceptor *acceptor) override { s.acceptor = acceptor; }
	bool HasRegisteredPeer(uint32_t ip, uint16_t port) const override
	{
		return s.registered.Has(ip, port);
	}
	void Destroy() override
	{
		++s.destroys;
		s.payload.clear();
		s.sink = nullptr;
	}
	bool ProcessDatagram(const uint8_t *data, size_t len, uint32_t ip, uint16_t port) override
	{
		++s.receives;
		s.payload.assign(data, data + len);
		s.ip = ip;
		s.port = port;
		return s.claimed;
	}
	void IssueDeferredAcks() override { ++s.acks; }
	void CheckTimeouts() override { ++s.timeouts; }

private:
	State &s;
};

class Sink : public IUtpDatagramSink
{
public:
	std::vector<uint8_t> wire, peerHash, sentHash;
	uint32_t ip = 0;
	uint16_t port = 0;
	bool encrypted = true, kad = true, hasHash = true;
	uint32_t key = 1;
	void SendUtpDatagram(const uint8_t *data,
		size_t len,
		uint32_t address,
		uint16_t service,
		bool encrypt,
		const uint8_t *userHash) override
	{
		// The caller's parameters win when it supplies them; the peerHash
		// member is the older per-sink default these cases were written around.
		const bool obfuscate = encrypt || !peerHash.empty();
		const uint8_t *hash = encrypt ? userHash : (peerHash.empty() ? nullptr : peerHash.data());
		QueueUtpDatagram<CPacket>(*this, data, len, address, service, obfuscate, hash);
	}
	void SendPacket(CPacket *raw,
		uint32_t address,
		uint16_t service,
		bool encrypt,
		const uint8_t *hash,
		bool isKad,
		uint32_t verifyKey)
	{
		std::unique_ptr<CPacket> packet(raw);
		wire.assign(packet->GetUDPHeader(), packet->GetUDPHeader() + 2);
		wire.insert(wire.end(),
			packet->GetDataBuffer(),
			packet->GetDataBuffer() + packet->GetPacketSize());
		ip = address;
		port = service;
		encrypted = encrypt;
		kad = isKad;
		hasHash = hash != nullptr;
		sentHash.clear();
		if (hash) {
			sentHash.assign(hash, hash + 16);
		}
		key = verifyKey;
	}
};
} // namespace

TEST(UtpContext, ClassifiedPayloadAndClaimArePreserved)
{
	State state;
	Sink sink;
	CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
	const uint8_t wire[] = { 0xB2, 0x00, 0x41, 0x00, 0xFF, 0xB2 };
	const auto frame = ClassifyReservedProt2Frame(wire + 1, sizeof(wire) - 1);
	ASSERT_TRUE(ProcessUtpFrame(context, frame, 0x04030201, 4665));
	ASSERT_EQUALS(4, (int)state.payload.size());
	for (size_t i = 0; i < state.payload.size(); ++i) {
		ASSERT_EQUALS((int)wire[i + 2], (int)state.payload[i]);
	}
	ASSERT_EQUALS(0x04030201u, state.ip);
	ASSERT_EQUALS(4665, (int)state.port);
	state.claimed = false;
	ASSERT_FALSE(ProcessUtpFrame(context, frame, state.ip, state.port));
	ASSERT_EQUALS(1, state.creates);
}

TEST(UtpContext, OtherTypesNeverReachLibrary)
{
	State state;
	Sink sink;
	CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
	for (unsigned type = 1; type <= 255; ++type) {
		const uint8_t frame[] = { static_cast<uint8_t>(type), 0x41 };
		ASSERT_FALSE(
			ProcessUtpFrame(context, ClassifyReservedProt2Frame(frame, sizeof(frame)), 1, 2));
	}
	ASSERT_FALSE(ProcessUtpFrame(context, ClassifyReservedProt2Frame(nullptr, 0), 1, 2));
	ASSERT_EQUALS(0, state.creates);
	ASSERT_EQUALS(0, state.receives);
}

TEST(UtpContext, LibrarySendIsPlaintextWhenNoPeerKnown)
{
	State state;
	Sink sink;
	CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
	ASSERT_TRUE(context.Configure());
	const uint8_t payload[] = { 0x41, 0x00, 0xFF, 0xB2 };
	state.sink->SendUtpDatagram(payload, sizeof(payload), 0x04030201, 65535, false, nullptr);
	ASSERT_EQUALS(6, (int)sink.wire.size());
	ASSERT_EQUALS(0xB2, (int)sink.wire[0]);
	ASSERT_EQUALS(0x00, (int)sink.wire[1]);
	for (size_t i = 0; i < sizeof(payload); ++i) {
		ASSERT_EQUALS((int)payload[i], (int)sink.wire[i + 2]);
	}
	ASSERT_EQUALS(0x04030201u, sink.ip);
	ASSERT_EQUALS(65535, (int)sink.port);
	ASSERT_FALSE(sink.encrypted);
	ASSERT_FALSE(sink.kad);
	ASSERT_FALSE(sink.hasHash);
	ASSERT_EQUALS(0u, sink.key);
}

TEST(UtpContext, LibrarySendIsEncryptedWithHashWhenPeerKnown)
{
	State state;
	Sink sink;
	sink.peerHash = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
	ASSERT_TRUE(context.Configure());
	const uint8_t payload[] = { 0x41, 0x00, 0xFF, 0xB2 };
	state.sink->SendUtpDatagram(payload, sizeof(payload), 0x04030201, 65535, false, nullptr);
	ASSERT_EQUALS(6, (int)sink.wire.size());
	ASSERT_EQUALS(0xB2, (int)sink.wire[0]);
	ASSERT_EQUALS(0x00, (int)sink.wire[1]);
	for (size_t i = 0; i < sizeof(payload); ++i) {
		ASSERT_EQUALS((int)payload[i], (int)sink.wire[i + 2]);
	}
	ASSERT_EQUALS(0x04030201u, sink.ip);
	ASSERT_EQUALS(65535, (int)sink.port);
	ASSERT_TRUE(sink.encrypted);
	ASSERT_TRUE(sink.hasHash);
	ASSERT_TRUE(sink.sentHash == sink.peerHash);
	ASSERT_FALSE(sink.kad);
	ASSERT_EQUALS(0u, sink.key);
}

TEST(UtpContext, ProcessingIssuesDeferredAcksWithoutTick)
{
	State state;
	Sink sink;
	CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
	const uint8_t payload[] = { 0x41 };
	ASSERT_TRUE(context.ProcessDatagram(payload, sizeof(payload), 1, 2));
	ASSERT_EQUALS(1, state.acks);
	ASSERT_EQUALS(0, state.timeouts);
	state.claimed = false;
	ASSERT_FALSE(context.ProcessDatagram(payload, sizeof(payload), 1, 2));
	ASSERT_EQUALS(2, state.acks);
	ASSERT_EQUALS(0, state.timeouts);
}

TEST(UtpContext, TickAndCloseAbandonStateUntilNextDatagram)
{
	State state;
	Sink sink;
	{
		CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
		context.Tick();
		ASSERT_EQUALS(0, state.creates);
		ASSERT_EQUALS(0, state.acks);
		const uint8_t payload[] = { 0x41 };
		ASSERT_TRUE(context.ProcessDatagram(payload, sizeof(payload), 1, 2));
		context.Tick();
		ASSERT_EQUALS(2, state.acks);
		ASSERT_EQUALS(1, state.timeouts);
		context.Destroy(); // The operation used by CClientUDPSocket::Close.
		ASSERT_EQUALS(1, state.destroys);
		ASSERT_TRUE(state.payload.empty());
		context.Destroy();
		context.Tick();
		ASSERT_EQUALS(1, state.destroys);
		ASSERT_EQUALS(2, state.acks);
		ASSERT_EQUALS(1, state.timeouts);
		ASSERT_TRUE(context.ProcessDatagram(payload, sizeof(payload), 3, 4));
		ASSERT_EQUALS(2, state.creates);
	}
	ASSERT_EQUALS(2, state.destroys);
}

TEST(UtpContext, EmptyPayloadIsHandedToLibraryWithoutEnvelope)
{
	State state;
	state.claimed = false;
	Sink sink;
	CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
	const uint8_t wire[] = { 0xB2, 0x00 };
	ASSERT_FALSE(ProcessUtpFrame(context, ClassifyReservedProt2Frame(wire + 1, sizeof(wire) - 1), 1, 2));
	ASSERT_EQUALS(1, state.receives);
	ASSERT_TRUE(state.payload.empty());
}

TEST(UtpContext, OutgoingEmptyAndInvalidPayloads)
{
	Sink sink;
	sink.SendUtpDatagram(nullptr, 0, 1, 2, false, nullptr);
	ASSERT_EQUALS(2, (int)sink.wire.size());
	ASSERT_EQUALS(0xB2, (int)sink.wire[0]);
	ASSERT_EQUALS(0x00, (int)sink.wire[1]);
	sink.wire.clear();
	sink.SendUtpDatagram(nullptr, 1, 1, 2, false, nullptr);
	ASSERT_TRUE(sink.wire.empty());
	const uint8_t payload[] = { 0x41 };
	sink.SendUtpDatagram(payload, 65506, 1, 2, false, nullptr);
	ASSERT_TRUE(sink.wire.empty());
}

TEST(UtpContext, FailedCreationDropsWithoutMaintenance)
{
	State state;
	state.createSucceeds = false;
	Sink sink;
	CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
	const uint8_t frame[] = { 0x00 };
	ASSERT_FALSE(ProcessUtpFrame(context, ClassifyReservedProt2Frame(frame, sizeof(frame)), 1, 2));
	context.Tick();
	ASSERT_EQUALS(0, state.receives);
	ASSERT_EQUALS(0, state.acks);
	ASSERT_EQUALS(0, state.timeouts);
}
TEST(UtpContext, UdpSizingKeepsTheFamilyAwarenessLibutpHad)
{
	// libutp's own defaults branch on the address family; overriding them for the two-byte
	// envelope must not flatten that, or an IPv6 peer gets IPv4 numbers and libutp sizes
	// packets 20 bytes too large.
	ASSERT_EQUALS(1392ull, UtpUdpMtu(false));
	ASSERT_EQUALS(1222ull, UtpUdpMtu(true));
	ASSERT_EQUALS(30ull, UtpUdpOverhead(false));
	ASSERT_EQUALS(78ull, UtpUdpOverhead(true));

	// The envelope is what the override exists for: each is libutp's own
	// constant moved by exactly two bytes, in the direction that leaves room.
	ASSERT_EQUALS(1402ull - kUtpEnvelopeBytes - kUtpCryptHeaderBytes, UtpUdpMtu(false));
	ASSERT_EQUALS(1232ull - kUtpEnvelopeBytes - kUtpCryptHeaderBytes, UtpUdpMtu(true));
	ASSERT_EQUALS(28ull + kUtpEnvelopeBytes, UtpUdpOverhead(false));
	ASSERT_EQUALS(76ull + kUtpEnvelopeBytes, UtpUdpOverhead(true));
}

// The obfuscation header is part of the datagram too. EncryptSendClient() prepends
// CRYPT_HEADER_WITHOUTPADDING, so a budget that counts only the envelope lets a full-size
// encrypted packet exceed the very limit the override exists to respect. Dormant while the send
// path is unencrypted, and the reason the two must be fixed together rather than in either order.
TEST(UtpContext, TheUdpBudgetLeavesRoomForTheObfuscationHeader)
{
	ASSERT_EQUALS(8ull, kUtpCryptHeaderBytes);

	// A full-size payload plus everything aMule puts in front of it still fits the number
	// libutp was sizing against before the crypt header was accounted for.
	ASSERT_TRUE(UtpUdpMtu(false) + kUtpEnvelopeBytes + kUtpCryptHeaderBytes <= 1402ull);
	ASSERT_TRUE(UtpUdpMtu(true) + kUtpEnvelopeBytes + kUtpCryptHeaderBytes <= 1232ull);
}

TEST(UtpContext, FrameClassificationTable)
{
	// Mirrors libutp's own validity test (UTP_Version): type below ST_NUM_STATES,
	// first extension below 3, version 1. Anything else it would not look at.
	const struct
	{
		const char *label;
		uint8_t verType;
		uint8_t extension;
		size_t length;
		EUtpFrameKind expected;
	} cases[] = {
		{ "syn v1", 0x41, 0, 20, EUtpFrameKind::Syn },
		{ "data v1", 0x01, 0, 20, EUtpFrameKind::Existing },
		{ "fin v1", 0x11, 0, 20, EUtpFrameKind::Existing },
		{ "state v1", 0x21, 0, 20, EUtpFrameKind::Existing },
		{ "reset v1", 0x31, 0, 20, EUtpFrameKind::Existing },
		{ "syn with extension 2", 0x41, 2, 20, EUtpFrameKind::Syn },
		{ "unknown type 5", 0x51, 0, 20, EUtpFrameKind::Malformed },
		{ "version 0", 0x40, 0, 20, EUtpFrameKind::Malformed },
		{ "version 2", 0x42, 0, 20, EUtpFrameKind::Malformed },
		{ "extension 3", 0x41, 3, 20, EUtpFrameKind::Malformed },
		{ "one byte short", 0x41, 0, 19, EUtpFrameKind::Malformed },
		{ "empty", 0x41, 0, 0, EUtpFrameKind::Malformed },
	};
	for (const auto &row : cases) {
		CFormat format("%s: ver_type=0x%02x ext=%u len=%u");
		const wxString message =
			format % row.label % row.verType % row.extension % unsigned(row.length);
		uint8_t frame[20] = { 0 };
		frame[0] = row.verType;
		frame[1] = row.extension;
		const uint8_t *payload = row.length == 0 ? nullptr : frame;
		ASSERT_TRUE_M(ClassifyUtpFrame(payload, row.length) == row.expected, message);
	}
}

TEST(UtpContext, ARegisteredPeerSurvivesOneOfItsSocketsClosing)
{
	// One endpoint can hold more than one socket: a peer behind a NAT reusing
	// its source port, or a second connection opened while the first is dying.
	// Forgetting on the first close would strand the survivor, whose traffic
	// would then be answered with an RST as though it came from a stranger.
	CUtpPeerRegistry registry;
	ASSERT_FALSE(registry.Has(0x0100007F, 4672));

	registry.Add(0x0100007F, 4672);
	registry.Add(0x0100007F, 4672);
	ASSERT_TRUE(registry.Has(0x0100007F, 4672));
	ASSERT_EQUALS(1u, (unsigned)registry.Size());

	registry.Remove(0x0100007F, 4672);
	ASSERT_TRUE(registry.Has(0x0100007F, 4672));
	registry.Remove(0x0100007F, 4672);
	ASSERT_FALSE(registry.Has(0x0100007F, 4672));
	ASSERT_EQUALS(0u, (unsigned)registry.Size());
}

TEST(UtpContext, TheRegistryKeysOnAddressAndPortTogether)
{
	// Same address, different port is a different peer; so is the reverse.
	CUtpPeerRegistry registry;
	registry.Add(0x0100007F, 4672);
	ASSERT_FALSE(registry.Has(0x0100007F, 4673));
	ASSERT_FALSE(registry.Has(0x0200007F, 4672));
	ASSERT_TRUE(registry.Has(0x0100007F, 4672));
}

TEST(UtpContext, ForgettingAnUnknownPeerIsHarmless)
{
	// DESTROYING can arrive for a socket that never got registered, because
	// admission rejected it.
	CUtpPeerRegistry registry;
	registry.Remove(0x0100007F, 4672);
	ASSERT_EQUALS(0u, (unsigned)registry.Size());
}

TEST(UtpContext, TheAcceptorSurvivesTheContextBeingRebuilt)
{
	// CClientUDPSocket::Close() destroys the context and Open() lets it come
	// back lazily, which is what a Kad reconnect does without rebuilding the
	// object. An acceptor installed once at construction would be gone for the
	// session, and every inbound SYN refused with no line anywhere.
	State state;
	Sink sink;
	CUtpContext context(std::make_unique<FakeLibrary>(state), sink);
	CFakeAcceptor acceptor;
	context.SetAcceptor(&acceptor);
	ASSERT_TRUE(context.Configure());
	ASSERT_TRUE(state.acceptor == &acceptor);

	context.Destroy();
	state.acceptor = nullptr;

	ASSERT_TRUE(context.Configure());
	ASSERT_TRUE(state.acceptor == &acceptor);
}

// File_checked_for_headers
