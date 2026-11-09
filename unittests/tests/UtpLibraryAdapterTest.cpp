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

#include <StreamTransport.h>
#include <UtpSocketTransport.h>
#include <UtpStream.h>
#include <UtpLibraryAdapter.h>
#include "UtpDialPolicy.h"
#include <libs/common/Format.h>

#include <libutp/utp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(UtpLibraryAdapter)

namespace
{
constexpr uint32_t kPeerIp = 0x0100007F; // 127.0.0.1 in aMule's low-byte-first form
constexpr uint16_t kPeerPort = 4672;

// A deterministic loopback through real libutp; datagrams are handed over by hand.
struct SLoopback
{
	std::unique_ptr<IUtpLibrary> server;
	utp_context *client = nullptr;
	utp_socket *clientSocket = nullptr;
	std::deque<std::vector<uint8_t>> toServer, toClient;
	std::vector<uint8_t> clientRead;
	bool clientConnected = false;
	bool sawChangedPortDatagram = false;
	bool sawRemapCandidate = false;
	//! The last datagram the peer put on the wire, so a test can replay it or a copy of it.
	std::vector<uint8_t> lastClientDatagram;
	uint16_t clientSourcePort = kPeerPort;
	uint64_t nowMs = 100;
};

SLoopback *g_loop = nullptr;

//! Everything the server sends goes to the client.
class CServerSink : public IUtpDatagramSink
{
public:
	void SendUtpDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash) override
	{
		lastIp = ip;
		lastPort = port;
		lastEncrypt = encrypt;
		lastPayload.assign(payload, payload + length);
		lastUserHash.clear();
		if (userHash != nullptr) {
			lastUserHash.assign(userHash, userHash + 16);
		}
		events.push_back(encrypt ? "datagram-keyed" : "datagram-plain");
		keys.push_back(lastUserHash);
		++datagrams;
		if (!dropOldPort || port == g_loop->clientSourcePort) {
			g_loop->toClient.emplace_back(payload, payload + length);
		} else {
			++droppedOldPortDatagrams;
		}
	}
	void SendUtpKeyFrame(uint32_t ip, uint16_t port, const uint8_t *peerHash) override
	{
		events.push_back("key-frame");
		keys.emplace_back(peerHash, peerHash + 16);
		keyFrameIp = ip;
		keyFramePort = port;
	}
	void OnUtpStreamMoved() override { ++streamMoves; }
	uint32_t lastIp = 0;
	uint16_t lastPort = 0;
	//! The last datagram this side put on the wire, so a test can read the numbers it carried.
	std::vector<uint8_t> lastPayload;
	uint32_t keyFrameIp = 0;
	uint16_t keyFramePort = 0;
	std::vector<std::string> events;
	std::vector<std::vector<uint8_t>> keys;
	bool dropOldPort = false;
	bool lastEncrypt = false;
	std::vector<uint8_t> lastUserHash;
	unsigned droppedOldPortDatagrams = 0;
	unsigned datagrams = 0;
	unsigned streamMoves = 0;
};

//! Admission, reduced to the decision each test varies.
class CFakeAcceptor : public IUtpStreamAcceptor
{
public:
	bool AdmitsSyn(uint32_t, uint16_t, std::size_t halfOpen) override
	{
		++synChecks;
		lastHalfOpen = halfOpen;
		return admitSyn;
	}

	SUtpIncomingSynDecision DecideIncomingSyn(
		uint32_t, uint16_t, const SUtpSynCandidateInfo &candidate) override
	{
		++synDecisions;
		lastCandidate = candidate;
		SUtpIncomingSynDecision decision = synDecision;
		if (decision.disposition == EUtpIncomingSynDisposition::AcceptKeepIncoming &&
			decision.candidateToken == 0) {
			decision.candidateToken = candidate.clientToken;
		}
		return decision;
	}

	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport,
		uint32_t ip,
		uint16_t port,
		const SUtpIncomingSynDecision &decision) override
	{
		lastDecision = decision;
		return AcceptStream(transport, ip, port);
	}

	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport, uint32_t ip, uint16_t port) override
	{
		++offers;
		lastIp = ip;
		lastPort = port;
		if (!admit) {
			return false;
		}
		accepted = std::move(transport);
		return true;
	}

	bool admit = true;
	bool admitSyn = true;
	int synChecks = 0;
	std::size_t lastHalfOpen = 0;
	int offers = 0;
	int synDecisions = 0;
	uint32_t lastIp = 0;
	uint16_t lastPort = 0;
	SUtpSynCandidateInfo lastCandidate;
	SUtpIncomingSynDecision synDecision;
	SUtpIncomingSynDecision lastDecision;
	std::unique_ptr<IStreamTransport> accepted;
};

uint64 ClientSendTo(utp_callback_arguments *args)
{
	g_loop->lastClientDatagram.assign(args->buf, args->buf + args->len);
	g_loop->toServer.emplace_back(args->buf, args->buf + args->len);
	return 0;
}

uint64 ClientOnAccept(utp_callback_arguments *args)
{
	g_loop->clientSocket = args->socket;
	return 0;
}

uint64 ClientOnState(utp_callback_arguments *args)
{
	if (args->state == UTP_STATE_CONNECT || args->state == UTP_STATE_WRITABLE) {
		g_loop->clientConnected = true;
	}
	return 0;
}

uint64 ClientOnRead(utp_callback_arguments *args)
{
	g_loop->clientRead.insert(g_loop->clientRead.end(), args->buf, args->buf + args->len);
	utp_read_drained(args->socket);
	return 0;
}

sockaddr_in Address(uint32_t ip, uint16_t port)
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	auto *bytes = reinterpret_cast<uint8_t *>(&address.sin_addr.s_addr);
	for (unsigned i = 0; i < 4; ++i) {
		bytes[i] = static_cast<uint8_t>(ip >> (8 * i));
	}
	return address;
}

//! A bare 20-byte uTP frame, laid out as PacketFormatV1 puts it (utp_internal.cpp:113-136), so a
//! test can hand the adapter a header it did not receive from libutp.
std::vector<uint8_t> UtpFrame(
	uint8_t type, uint16_t connectionId, uint16_t sequenceNumber, uint16_t acknowledgeNumber)
{
	std::vector<uint8_t> frame(20, 0);
	frame[0] = static_cast<uint8_t>((type << 4) | 1);
	frame[2] = static_cast<uint8_t>(connectionId >> 8);
	frame[3] = static_cast<uint8_t>(connectionId);
	frame[16] = static_cast<uint8_t>(sequenceNumber >> 8);
	frame[17] = static_cast<uint8_t>(sequenceNumber);
	frame[18] = static_cast<uint8_t>(acknowledgeNumber >> 8);
	frame[19] = static_cast<uint8_t>(acknowledgeNumber);
	return frame;
}

//! Hands datagrams across until both directions are idle.
void Pump(SLoopback &loop, int rounds = 64, uint16_t remapClientPortAfterFirstServerPacket = 0)
{
	const sockaddr_in peer = Address(kPeerIp, kPeerPort);
	bool remapped = false;
	// Before the loop: the window update is deferred through schedule_ack(),
	// and with both queues empty the loop never runs.
	loop.server->IssueDeferredAcks();
	utp_issue_deferred_acks(loop.client);
	for (int i = 0; i < rounds && (!loop.toServer.empty() || !loop.toClient.empty()); ++i) {
		while (!loop.toServer.empty()) {
			const std::vector<uint8_t> datagram = loop.toServer.front();
			loop.toServer.pop_front();
			if (loop.clientSourcePort != kPeerPort) {
				loop.sawChangedPortDatagram = true;
				loop.sawRemapCandidate |= loop.server->HasExpectedPortRemapPeer(
					kPeerIp, loop.clientSourcePort, loop.nowMs);
			}
			loop.server->ProcessDatagram(
				datagram.data(), datagram.size(), kPeerIp, loop.clientSourcePort, loop.nowMs);
		}
		while (!loop.toClient.empty()) {
			const std::vector<uint8_t> datagram = loop.toClient.front();
			loop.toClient.pop_front();
			utp_process_udp(loop.client,
				datagram.data(),
				datagram.size(),
				reinterpret_cast<const sockaddr *>(&peer),
				sizeof(peer));
			if (remapClientPortAfterFirstServerPacket != 0 && !remapped) {
				loop.clientSourcePort = remapClientPortAfterFirstServerPacket;
				remapped = true;
			}
		}
		loop.server->IssueDeferredAcks();
		utp_issue_deferred_acks(loop.client);
	}
}

void ConfigureClient(SLoopback &loop)
{
	loop.client = utp_init(2);
	utp_set_callback(loop.client, UTP_SENDTO, ClientSendTo);
	utp_set_callback(loop.client, UTP_ON_ACCEPT, ClientOnAccept);
	utp_set_callback(loop.client, UTP_ON_STATE_CHANGE, ClientOnState);
	utp_set_callback(loop.client, UTP_ON_READ, ClientOnRead);
}

void StartClient(SLoopback &loop)
{
	ConfigureClient(loop);
	loop.clientSocket = utp_create_socket(loop.client);
	const sockaddr_in peer = Address(kPeerIp, kPeerPort);
	utp_connect(loop.clientSocket, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
}

void StartClientAcceptor(SLoopback &loop)
{
	ConfigureClient(loop);
}

void Teardown(SLoopback &loop)
{
	if (loop.client != nullptr) {
		utp_destroy(loop.client);
		loop.client = nullptr;
	}
	loop.server.reset();
	g_loop = nullptr;
}

//! Dials the client with the handshake remap allowed while its NAT moves it to the edge of the
//! window. Returns the port the stream then holds.
uint16_t DialRemapped(SLoopback &loop, CServerSink &sink, std::unique_ptr<IStreamTransport> &transport)
{
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	StartClientAcceptor(loop);
	const SUtpDialOptions options{ true, loop.nowMs };
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, options, transport));
	Pump(loop, 64, kPeerPort + kUtpPortRemapWindow);
	ASSERT_TRUE(transport != nullptr);
	return kPeerPort + kUtpPortRemapWindow;
}

//! So the adapter has seen the peer's connection id and sequence numbers, and a send of ours.
void ExchangeOneFrameEachWay(SLoopback &loop, IStreamTransport &transport, uint8_t payload)
{
	ASSERT_EQUALS(1, (int)transport.Write(&payload, 1));
	transport.Flush();
	Pump(loop);
	utp_iovec vector{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	Pump(loop);
}

struct SFrameNumbers
{
	uint16_t connectionId;
	uint16_t seqNr;
	uint16_t ackNr;
};

//! Parsed by hand, so the tests do not share ReadUtpWireHeader() with the code under test.
SFrameNumbers FrameNumbers(const std::vector<uint8_t> &frame)
{
	ASSERT_TRUE(frame.size() >= kUtpWireHeaderBytes);
	return { static_cast<uint16_t>((frame[2] << 8) | frame[3]),
		static_cast<uint16_t>((frame[16] << 8) | frame[17]),
		static_cast<uint16_t>((frame[18] << 8) | frame[19]) };
}

//! Pumps until @a transport yields data; returns the bytes read into @a buffer.
template <typename Buffer>
uint32_t ReadUntilData(SLoopback &loop, IStreamTransport &transport, Buffer &buffer)
{
	uint32_t receivedBytes = 0;
	for (int round = 0; round < 64 && receivedBytes == 0; ++round) {
		Pump(loop);
		receivedBytes = transport.Read(buffer.data(), static_cast<uint32_t>(buffer.size()));
	}
	return receivedBytes;
}
} // namespace

TEST(UtpLibraryAdapter, AnInboundSynReachesAdmissionWithItsPeer)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);

	Pump(loop);

	ASSERT_EQUALS(1, acceptor.offers);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_EQUALS(kPeerIp, acceptor.lastIp);
	ASSERT_EQUALS(kPeerPort, acceptor.lastPort);
	// Registered only on success, which is what lets the peer's later non-SYN
	// frames through the ingress filter.
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

namespace
{
//! Feeds the client's SYN to the server as having arrived obfuscated or not.
bool AcceptedStreamObfuscates(bool synObfuscated)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	loop.server->Create(sink);
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	const std::vector<uint8_t> syn = loop.toServer.front();
	loop.toServer.pop_front();
	loop.server->ProcessDatagram(syn.data(), syn.size(), kPeerIp, kPeerPort, loop.nowMs, synObfuscated);
	const bool obfuscates = acceptor.accepted != nullptr && acceptor.accepted->ObfuscatesStream();
	Teardown(loop);
	return obfuscates;
}
} // namespace

TEST(UtpLibraryAdapter, AnObfuscatedSynOpensAStreamThatReportsObfuscation)
{
	ASSERT_TRUE(AcceptedStreamObfuscates(true));
}

TEST(UtpLibraryAdapter, APlainSynOpensAStreamThatDoesNotReportObfuscation)
{
	ASSERT_FALSE(AcceptedStreamObfuscates(false));
}

TEST(UtpLibraryAdapter, IncomingSynRejectsAfterTheAcceptorKeepsTheOutgoingAttempt)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	acceptor.synDecision.disposition = EUtpIncomingSynDisposition::RejectKeepOutgoing;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);

	std::unique_ptr<IStreamTransport> outgoing;
	const SUtpDialOptions options{ true, loop.nowMs };
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, options, outgoing));
	const std::vector<uint8_t> incomingSyn = loop.toServer.front();
	loop.toServer.pop_front();
	loop.server->ProcessDatagram(incomingSyn.data(), incomingSyn.size(), kPeerIp, kPeerPort, loop.nowMs);

	ASSERT_EQUALS(1, acceptor.synDecisions);
	ASSERT_TRUE(acceptor.lastCandidate.status == EUtpSynCandidateStatus::Unique);
	ASSERT_EQUALS(kPeerIp, acceptor.lastCandidate.ip);
	ASSERT_EQUALS(kPeerPort, acceptor.lastCandidate.port);
	ASSERT_EQUALS(0, acceptor.offers);
	ASSERT_TRUE(outgoing != nullptr);
	ASSERT_FALSE(outgoing->IsConnected());
	outgoing.reset();
	Teardown(loop);
}

TEST(UtpLibraryAdapter, IncomingSynWinnerFlowsThroughVerifiedHelloAttachmentSeam)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	acceptor.synDecision = { EUtpIncomingSynDisposition::AcceptKeepIncoming, 0 };
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);

	std::unique_ptr<IStreamTransport> outgoing;
	const SUtpDialOptions options{ true, loop.nowMs, 91 };
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, options, outgoing));
	const std::vector<uint8_t> incomingSyn = loop.toServer.front();
	loop.toServer.pop_front();
	loop.server->ProcessDatagram(incomingSyn.data(), incomingSyn.size(), kPeerIp, kPeerPort, loop.nowMs);

	ASSERT_TRUE(acceptor.lastCandidate.status == EUtpSynCandidateStatus::Unique);
	ASSERT_EQUALS((uint32_t)91, acceptor.lastCandidate.clientToken);
	ASSERT_TRUE(acceptor.lastDecision.disposition == EUtpIncomingSynDisposition::AcceptKeepIncoming);
	ASSERT_EQUALS((uint32_t)91, acceptor.lastDecision.candidateToken);
	ASSERT_EQUALS(1, acceptor.offers);
	ASSERT_TRUE(acceptor.accepted != nullptr);

	// Model the application boundary after OP_HELLO identifies the intended existing client.
	struct SPeerFixture
	{
		uint32_t ecid;
		IStreamTransport *socket;
		bool fileRequestLost = false;
		int fileRequestsSent = 0;
	};
	SPeerFixture intended{ 91, outgoing.get() };
	SPeerFixture temporary{ 0, acceptor.accepted.get() };
	SUtpIncomingSynWinner winner;
	winner.Select(acceptor.lastDecision.candidateToken);
	const uint32_t helloPeerToken = intended.ecid;
	const bool verifiedHelloMatch = winner.Consume(helloPeerToken) && helloPeerToken == intended.ecid;
	ASSERT_TRUE(verifiedHelloMatch);
	ASSERT_FALSE(winner.Consume(helloPeerToken));

	const uint8_t localHash[16] = { 0 };
	const uint8_t peerHash[16] = { 1 };
	bool outgoingRetired = false;
	const SUtpHelloAttachmentResult attachment = AttachUtpSocketAfterVerifiedHello(
		verifiedHelloMatch,
		false,
		true,
		localHash,
		peerHash,
		true,
		[&]() { intended.fileRequestLost = true; },
		[&]() {
			intended.socket = nullptr;
			outgoingRetired = true;
		},
		[&]() {
			intended.socket = temporary.socket;
			temporary.socket = nullptr;
		});
	ASSERT_FALSE(attachment.decision.keepFound);
	ASSERT_TRUE(attachment.decision.socketSwap);
	ASSERT_TRUE(attachment.socketAttached);
	ASSERT_TRUE(outgoingRetired);
	ASSERT_TRUE(intended.socket == acceptor.accepted.get());
	ASSERT_TRUE(temporary.socket == nullptr);

	ASSERT_TRUE(RetryFileRequestAfterUtpSocketSwap(
		intended.fileRequestLost, true, [&]() { ++intended.fileRequestsSent; }));
	ASSERT_EQUALS(1, intended.fileRequestsSent);
	ASSERT_FALSE(intended.fileRequestLost);
	ASSERT_FALSE(RetryFileRequestAfterUtpSocketSwap(
		intended.fileRequestLost, true, [&]() { ++intended.fileRequestsSent; }));
	ASSERT_EQUALS(1, intended.fileRequestsSent);

	acceptor.accepted.reset();
	outgoing.reset();
	Teardown(loop);
}

TEST(UtpLibraryAdapter, LoopbackSinkDropsPacketsAddressedToOldPeerPort)
{
	SLoopback loop;
	g_loop = &loop;
	loop.clientSourcePort = kPeerPort + kUtpPortRemapWindow;
	CServerSink sink;
	sink.dropOldPort = true;
	const uint8_t payload = 0x5A;
	sink.SendUtpDatagram(&payload, 1, kPeerIp, kPeerPort, false, nullptr);
	ASSERT_EQUALS(1, (int)sink.droppedOldPortDatagrams);
	ASSERT_TRUE(loop.toClient.empty());
	Teardown(loop);
}

TEST(UtpLibraryAdapter, OutboundDialSendsItsSynToTheRequestedEndpoint)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));

	std::unique_ptr<IStreamTransport> transport;
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, transport));
	ASSERT_EQUALS(kPeerIp, sink.lastIp);
	ASSERT_EQUALS((int)kPeerPort, (int)sink.lastPort);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, OutboundEncryptedSynRetainsItsSocketCryptParameters)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	const uint8_t userHash[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

	std::unique_ptr<IStreamTransport> transport;
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, true, userHash, transport));
	ASSERT_TRUE(sink.lastEncrypt);
	ASSERT_EQUALS(16, (int)sink.lastUserHash.size());
	for (unsigned i = 0; i < sink.lastUserHash.size(); ++i) {
		ASSERT_EQUALS((int)userHash[i], (int)sink.lastUserHash[i]);
	}
	Teardown(loop);
}

TEST(UtpLibraryAdapter, OutboundHandshakeRemapsOneExpectedPeerToItsObservedPort)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	DialRemapped(loop, sink, transport);

	ASSERT_TRUE(transport->IsConnected());
	ASSERT_TRUE(loop.sawRemapCandidate);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort + kUtpPortRemapWindow));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	ASSERT_EQUALS((int)(kPeerPort + kUtpPortRemapWindow), (int)transport->GetPeerPort());
	// Following the port during the handshake is not a move of a connected stream.
	ASSERT_EQUALS(0, (int)sink.streamMoves);

	const uint8_t payload = 0xA5;
	sink.dropOldPort = true;
	ASSERT_EQUALS(1, (int)transport->Write(&payload, 1));
	transport->Flush();
	Pump(loop);
	ASSERT_EQUALS(0, (int)sink.droppedOldPortDatagrams);
	ASSERT_TRUE(!loop.clientRead.empty());
	ASSERT_EQUALS((int)payload, (int)loop.clientRead.back());

	utp_iovec vector{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	std::vector<uint8_t> received(1024);
	const uint32_t receivedBytes = ReadUntilData(loop, *transport, received);
	ASSERT_EQUALS(1, (int)receivedBytes);
	ASSERT_EQUALS((int)payload, (int)received[0]);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, ReadsTheBaseHeaderFieldsAMoveIsDecidedOn)
{
	const std::vector<uint8_t> frame = UtpFrame(2, 0x1122, 0x07D0, 0xAB09);
	const auto header = ReadUtpWireHeader(frame.data(), frame.size());
	ASSERT_TRUE(header.has_value());
	ASSERT_EQUALS((uint8_t)2, header->type);
	ASSERT_EQUALS((uint32_t)0x1122, header->connectionId);
	ASSERT_EQUALS((uint16_t)0x07D0, header->sequenceNumber);
	ASSERT_EQUALS((uint16_t)0xAB09, header->acknowledgeNumber);

	// Anything ClassifyUtpFrame() will not parse yields nothing to decide on.
	ASSERT_FALSE(ReadUtpWireHeader(frame.data(), 19).has_value());
	ASSERT_FALSE(ReadUtpWireHeader(nullptr, 20).has_value());
	std::vector<uint8_t> wrongVersion = frame;
	wrongVersion[0] = static_cast<uint8_t>((2 << 4) | 2);
	ASSERT_FALSE(ReadUtpWireHeader(wrongVersion.data(), wrongVersion.size()).has_value());
}

TEST(UtpLibraryAdapter, AFrameWithoutThePeersConnectionIdNeverMovesTheStream)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);
	ASSERT_TRUE(transport->IsConnected());
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));

	const uint8_t payload = 0x5A;
	ExchangeOneFrameEachWay(loop, *transport, payload);

	// A frame from a plausible new port, with plausible numbers but a connection id no stream here
	// uses. utp_process_udp() would have answered 1 for it all the same.
	const uint16_t movedPort = heldPort + 100;
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, movedPort));
	const std::vector<uint8_t> forged = UtpFrame(kUtpTypeData, 0, 1, 1);
	loop.server->ProcessDatagram(forged.data(), forged.size(), kPeerIp, movedPort, loop.nowMs);
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
	ASSERT_EQUALS((int)heldPort, (int)transport->GetPeerPort());
	ASSERT_EQUALS(0, (int)sink.streamMoves);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AConnectedStreamFollowsANATPortChange)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);
	ASSERT_TRUE(transport->IsConnected());
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));

	// One frame each way, so the adapter has the peer's connection id, its sequence numbers and one
	// of our own sends to hold a move to.
	const uint8_t payload = 0x33;
	ExchangeOneFrameEachWay(loop, *transport, payload);
	ASSERT_TRUE(!loop.clientRead.empty());

	// The NAT moves the source port once more, still inside the window of the port we hold.
	const uint16_t movedPort = heldPort + 100;
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, movedPort));
	loop.clientSourcePort = movedPort;
	utp_iovec second{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &second, 1));
	Pump(loop);

	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
	ASSERT_EQUALS((int)movedPort, (int)transport->GetPeerPort());
	ASSERT_EQUALS(1, (int)sink.streamMoves);

	// Both directions still cross on the moved stream. The buffer covers the payload, so the
	// inlined deque copy in UtpStream::Read is bounded by the object it writes into.
	std::array<uint8_t, 4> received{};
	ReadUntilData(loop, *transport, received);
	ASSERT_EQUALS((int)payload, (int)received[0]);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AFrameThatDoesNotProveItselfIsDroppedWithoutAnRst)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);
	ASSERT_TRUE(transport->IsConnected());
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));

	// One frame each way, then keep a genuine one: its connection id is the peer's own, which is
	// what a forgery would have to guess.
	const uint8_t payload = 0x77;
	ExchangeOneFrameEachWay(loop, *transport, payload);
	const SFrameNumbers genuine = FrameNumbers(loop.lastClientDatagram);

	const uint16_t newPort = heldPort + 100;
	// A reset is refused even carrying the right connection id: libutp would act on it from the id
	// alone (utp_internal.cpp:2866-2887 through LookupAdjacent at :2809-2823).
	const std::vector<uint8_t> reset =
		UtpFrame(kUtpTypeReset, genuine.connectionId, genuine.seqNr + 5, 0);
	// A connection id no stream here has ever used.
	const std::vector<uint8_t> stranger = UtpFrame(kUtpTypeData, 0, genuine.seqNr + 5, 0);

	for (const std::vector<uint8_t> &frame : { reset, stranger }) {
		ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, newPort));
		const unsigned sentBefore = sink.datagrams;
		loop.server->ProcessDatagram(frame.data(), frame.size(), kPeerIp, newPort, loop.nowMs);
		// Nothing moved.
		ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
		ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, newPort));
		ASSERT_EQUALS((int)heldPort, (int)transport->GetPeerPort());
		// And nothing left the socket at all: an unmatched non-SYN handed to libutp would have
		// been answered with an RST to that source (utp_internal.cpp:2925-2940).
		ASSERT_EQUALS((int)sentBefore, (int)sink.datagrams);
		ASSERT_TRUE(sink.lastPort != newPort);
		ASSERT_EQUALS(0, (int)sink.streamMoves);
	}
	Teardown(loop);
}

TEST(UtpLibraryAdapter, ALateFrameFromThePortThePeerLeftNeverMovesItBack)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);

	const uint8_t payload = 0x44;
	ExchangeOneFrameEachWay(loop, *transport, payload);
	// The frame the peer sent while it still used heldPort.
	const std::vector<uint8_t> lateFrame = loop.lastClientDatagram;
	ASSERT_TRUE(lateFrame.size() >= kUtpWireHeaderBytes);

	const uint16_t movedPort = heldPort + 100;
	loop.clientSourcePort = movedPort;
	utp_iovec second{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &second, 1));
	Pump(loop);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));

	// Replaying it from the port the peer has left must change nothing: a move only ever follows
	// newer sequence numbers, so a late frame cannot drag the stream back.
	const unsigned sentBefore = sink.datagrams;
	loop.server->ProcessDatagram(lateFrame.data(), lateFrame.size(), kPeerIp, heldPort, loop.nowMs);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
	ASSERT_EQUALS((int)movedPort, (int)transport->GetPeerPort());
	ASSERT_EQUALS((int)sentBefore, (int)sink.datagrams);
	ASSERT_TRUE(sink.lastPort != heldPort);
	ASSERT_EQUALS(1, (int)sink.streamMoves);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, ARefusedMoveSendsNothingAndLeavesTheStreamWhereItIs)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);

	const uint8_t payload = 0x22;
	ExchangeOneFrameEachWay(loop, *transport, payload);

	// First move, at the same injected time as the second attempt below.
	const uint16_t movedPort = heldPort + 100;
	loop.clientSourcePort = movedPort;
	utp_iovec second{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &second, 1));
	Pump(loop);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));

	// A second genuine frame from a third port, inside the same ten-second window: the limiter
	// refuses it, and a refused move must leave nothing on the wire either.
	utp_iovec third{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &third, 1));
	ASSERT_FALSE(loop.toServer.empty());
	const std::vector<uint8_t> frame = loop.toServer.front();
	loop.toServer.pop_front();
	const uint16_t thirdPort = movedPort + 100;
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, thirdPort));
	const unsigned sentBefore = sink.datagrams;
	loop.server->ProcessDatagram(frame.data(), frame.size(), kPeerIp, thirdPort, loop.nowMs);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, thirdPort));
	ASSERT_EQUALS((int)movedPort, (int)transport->GetPeerPort());
	ASSERT_EQUALS((int)sentBefore, (int)sink.datagrams);
	ASSERT_TRUE(sink.lastPort != thirdPort);
	ASSERT_EQUALS(1, (int)sink.streamMoves);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AnAckOnlyFrameFromANewPortMovesTheStream)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);

	const uint8_t payload = 0x11;
	ExchangeOneFrameEachWay(loop, *transport, payload);

	// A peer that only downloads sends nothing but ST_STATE, whose seq_nr is the number its next
	// data frame would carry and therefore never advances (utp_internal.cpp:782). It must still be
	// able to move its stream, which is the whole point of the feature.
	const auto [connectionId, peerSeqNr, peerAckNr] = FrameNumbers(loop.lastClientDatagram);

	const uint16_t newPort = heldPort + 100;
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, newPort));
	const std::vector<uint8_t> ackOnly = UtpFrame(kUtpTypeState, connectionId, peerSeqNr, peerAckNr);
	loop.server->ProcessDatagram(ackOnly.data(), ackOnly.size(), kPeerIp, newPort, loop.nowMs);

	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, newPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
	ASSERT_EQUALS((int)newPort, (int)transport->GetPeerPort());
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AKeepaliveFrameFromANewPortMovesTheStream)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);

	const uint8_t payload = 0x13;
	ExchangeOneFrameEachWay(loop, *transport, payload);

	// A keepalive repeats the sequence number and sends the ack one lower than the last one
	// (utp_internal.cpp:833-843), which is what an idle stream emits between keepalive intervals.
	const auto [connectionId, peerSeqNr, peerAckNr] = FrameNumbers(loop.lastClientDatagram);

	const uint16_t newPort = heldPort + 100;
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, newPort));
	const std::vector<uint8_t> keepalive =
		UtpFrame(kUtpTypeState, connectionId, peerSeqNr, static_cast<uint16_t>(peerAckNr - 1));
	loop.server->ProcessDatagram(keepalive.data(), keepalive.size(), kPeerIp, newPort, loop.nowMs);

	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, newPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
	ASSERT_EQUALS((int)newPort, (int)transport->GetPeerPort());
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AMoveBackToThePortTheStreamLeftIsRefused)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);

	const uint8_t payload = 0x15;
	ExchangeOneFrameEachWay(loop, *transport, payload);
	const auto [connectionId, peerSeqNr, peerAckNr] = FrameNumbers(loop.lastClientDatagram);

	const uint16_t movedPort = heldPort + 100;
	loop.clientSourcePort = movedPort;
	utp_iovec vector{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	Pump(loop);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));

	// A frame that would otherwise prove itself - right connection id, one number ahead, an ack in
	// range - but comes from the port this stream has just left: the guard refuses it.
	const std::vector<uint8_t> fromTheLeftPort =
		UtpFrame(kUtpTypeState, connectionId, static_cast<uint16_t>(peerSeqNr + 1), peerAckNr);
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, heldPort));
	const unsigned sentBefore = sink.datagrams;
	loop.server->ProcessDatagram(
		fromTheLeftPort.data(), fromTheLeftPort.size(), kPeerIp, heldPort, loop.nowMs);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
	ASSERT_EQUALS((int)movedPort, (int)transport->GetPeerPort());
	ASSERT_EQUALS((int)sentBefore, (int)sink.datagrams);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AFailedSynAckSendsNothing)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	StartClientAcceptor(loop);

	// A dial that allows the initial port remap registers a handshake candidate at once.
	std::unique_ptr<IStreamTransport> transport;
	const SUtpDialOptions options{ true, loop.nowMs };
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, options, transport));
	ASSERT_TRUE(transport != nullptr);
	const uint16_t synSeqNr = FrameNumbers(sink.lastPayload).seqNr;
	const uint16_t reportedPort = kPeerPort + 200;

	// The ingress gate admits a frame from that port because the handshake expects it. This one does
	// not acknowledge our SYN, and there is no connected stream to fall through to, so handing it to
	// libutp would draw an RST to that port (utp_internal.cpp:2925-2940).
	const std::vector<uint8_t> notOurSynAck =
		UtpFrame(kUtpTypeState, 0x4242, 1, static_cast<uint16_t>(synSeqNr + 1));
	const unsigned sentBefore = sink.datagrams;
	loop.server->ProcessDatagram(
		notOurSynAck.data(), notOurSynAck.size(), kPeerIp, reportedPort, loop.nowMs);

	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, reportedPort));
	ASSERT_EQUALS((int)sentBefore, (int)sink.datagrams);
	ASSERT_TRUE(sink.lastPort != reportedPort);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AResendBehindTheLastNumberStillMovesTheStream)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);

	const uint8_t payload = 0x18;
	ExchangeOneFrameEachWay(loop, *transport, payload);

	// A peer whose send window filled up and whose acks went to a dead mapping resends its OLDEST
	// unacked packet, whose number sits BEHIND the highest we have seen. That frame is the one that
	// recovers a bulk transfer, so it has to be admitted (libutp measures the same distance and
	// accepts it, utp_internal.cpp:1894-1907). libutp's own retransmit timer is not reachable in this
	// harness, so the resend is reproduced as the frame it produces.
	const auto [connectionId, peerSeqNr, peerAckNr] = FrameNumbers(loop.lastClientDatagram);
	const uint16_t oldestUnacked = static_cast<uint16_t>(peerSeqNr - kUtpMoveSeqWindow);

	const uint16_t newPort = heldPort + 100;
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, newPort));
	const std::vector<uint8_t> resend = UtpFrame(kUtpTypeState, connectionId, oldestUnacked, peerAckNr);
	ASSERT_TRUE(UtpSeqDistance(oldestUnacked, peerSeqNr) <= kUtpMoveSeqWindow);
	loop.server->ProcessDatagram(resend.data(), resend.size(), kPeerIp, newPort, loop.nowMs);

	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, newPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
	ASSERT_EQUALS((int)newPort, (int)transport->GetPeerPort());
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AFailingHandshakeLeavesTheConnectedStreamTranslated)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	// First dial: established, and moved to the observed port by the handshake remap.
	std::unique_ptr<IStreamTransport> connected;
	const uint16_t heldPort = DialRemapped(loop, sink, connected);

	const uint8_t payload = 0x1A;
	ExchangeOneFrameEachWay(loop, *connected, payload);
	const auto [connectionId, peerSeqNr, peerAckNr] = FrameNumbers(loop.lastClientDatagram);

	// Second dial to the same address: a handshake candidate now sits at kPeerPort, as a second user
	// behind the same CGNAT would leave it. Its SYN is the last datagram this side sent.
	std::unique_ptr<IStreamTransport> pending;
	const SUtpDialOptions options{ true, loop.nowMs };
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, options, pending));
	const uint16_t pendingSynSeqNr = FrameNumbers(sink.lastPayload).seqNr;

	// A frame from a port both candidates cover, carrying the CONNECTED stream's own numbers, so the
	// move decision would say Move - but it cannot acknowledge the pending SYN.
	const uint16_t sharedPort = kPeerPort + 300;
	const uint16_t ackNr =
		peerAckNr == pendingSynSeqNr ? static_cast<uint16_t>(peerAckNr - 1) : peerAckNr;
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, sharedPort));
	const std::vector<uint8_t> frame = UtpFrame(kUtpTypeState, connectionId, peerSeqNr, ackNr);
	const unsigned sentBefore = sink.datagrams;
	loop.server->ProcessDatagram(frame.data(), frame.size(), kPeerIp, sharedPort, loop.nowMs);

	// The handshake refused it, so it never reached the stream: nothing moved, nothing was sent, and
	// the translation the stream already holds is still in place.
	ASSERT_EQUALS((int)sentBefore, (int)sink.datagrams);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, heldPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, sharedPort));
	ASSERT_EQUALS((int)heldPort, (int)connected->GetPeerPort());

	// Proof the translation survived: a genuine frame from the held port still reaches the stream
	// instead of drawing an RST.
	const uint8_t second = 0x1B;
	utp_iovec followUp{ const_cast<uint8_t *>(&second), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &followUp, 1));
	Pump(loop);
	std::array<uint8_t, 4> received{};
	const uint32_t receivedBytes = ReadUntilData(loop, *connected, received);
	ASSERT_TRUE(receivedBytes != 0);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AnIdleReceiverProbesSoThePeerCanFollowItsPort)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;

	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);

	// An accepted socket is not writable until it has received data: libutp leaves it in CS_SYN_RECV
	// and only ST_DATA moves it to CS_CONNECTED (utp_internal.cpp:3168, :2166-2168), so this side
	// speaks first. That also starts the probe rule's uploader window on our own send, which is why
	// the wait below has to outlast kUtpProbeIdleMs and not merely reach it.
	const uint8_t greeting = 0x20;
	ASSERT_EQUALS(1, (int)transport->Write(&greeting, 1));
	transport->Flush();
	Pump(loop);

	// Data from the peer, which also moves the stream to its new port. Nothing else is written from
	// this side, so utp_check_timeouts() has no retransmission of ours to send and any datagram that
	// leaves below can only be the probe's.
	const uint8_t payload = 0x21;
	utp_iovec vector{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	const uint16_t movedPort = heldPort + 100;
	loop.clientSourcePort = movedPort;
	Pump(loop);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));

	// While the data is fresh there is nothing to probe.
	const unsigned beforeFresh = sink.datagrams;
	loop.server->CheckTimeouts();
	ASSERT_EQUALS((int)beforeFresh, (int)sink.datagrams);

	// After the idle window a datagram must leave, and it must go where the peer is now: that ack is
	// the only thing that tells it the stream moved. If the mechanism broke - libutp stopping acking
	// a grown window, or the flush not reaching the wire - nothing would leave and this fails. The
	// margin keeps both the received-data and the sent-data windows comfortably in the past, since
	// real time is what this rule runs on.
	std::this_thread::sleep_for(std::chrono::milliseconds(kUtpProbeIdleMs + 600));
	const unsigned beforeProbe = sink.datagrams;
	loop.server->CheckTimeouts();
	ASSERT_TRUE(sink.datagrams > beforeProbe);
	ASSERT_EQUALS((int)movedPort, (int)sink.lastPort);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AnAcceptedStreamIsNoHandshakeCandidateButFollowsItsPeerPort)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);

	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_TRUE(loop.clientConnected);
	// The handshake remap is for our own dials only, so this stream the peer opened never offered a
	// remap candidate and holds the endpoint its SYN came from.
	ASSERT_FALSE(loop.sawRemapCandidate);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	ASSERT_EQUALS((int)kPeerPort, (int)acceptor.accepted->GetPeerPort());

	// One frame each way, so the adapter knows this peer's connection id and sequence numbers and
	// has a send of its own to hold a move to.
	uint8_t payload = 0x5A;
	utp_iovec vector{ &payload, 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	Pump(loop);

	// After the handshake a frame that proves itself may move an accepted stream too: the header has
	// to carry the peer's own connection id and sequence numbers, so this is not a takeover.
	const uint16_t movedPort = kPeerPort + kUtpPortRemapWindow;
	loop.clientSourcePort = movedPort;
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, movedPort));
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	Pump(loop);

	ASSERT_TRUE(loop.sawChangedPortDatagram);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, movedPort));
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	ASSERT_EQUALS((int)movedPort, (int)acceptor.accepted->GetPeerPort());
	Teardown(loop);
}

TEST(UtpLibraryAdapter, DialCompletesThroughTheRealLoopbackAcceptor)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	StartClientAcceptor(loop);

	std::unique_ptr<IStreamTransport> transport;
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, transport));
	ASSERT_TRUE(transport != nullptr);
	Pump(loop);
	ASSERT_TRUE(transport->IsConnected());
	ASSERT_TRUE(loop.clientSocket != nullptr);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, WithNoAcceptorInstalledEveryInboundSynIsRefused)
{
	// The state before an acceptor existed, and after one is detached.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	StartClient(loop);

	Pump(loop);

	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, ARefusedStreamIsNeverRegistered)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	acceptor.admit = false;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);

	Pump(loop);

	ASSERT_EQUALS(1, acceptor.offers);
	ASSERT_TRUE(acceptor.accepted == nullptr);
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, BytesCrossTheStreamInOrder)
{
	// The point of the series: an accepted stream carries bytes.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);

	std::vector<uint8_t> payload(4096);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>(i & 0xFF);
	}
	// utp_writev takes only what the window allows; ignoring the return would
	// "prove" the stream carries one packet.
	std::vector<uint8_t> received(payload.size());
	size_t offered = 0;
	uint32_t total = 0;
	for (int round = 0; round < 512 && (offered < payload.size() || total < payload.size()); ++round) {
		if (offered < payload.size()) {
			utp_iovec vector{ payload.data() + offered, payload.size() - offered };
			const ssize_t accepted = utp_writev(loop.clientSocket, &vector, 1);
			if (accepted > 0) {
				offered += static_cast<size_t>(accepted);
			}
		}
		Pump(loop);
		while (total < payload.size()) {
			const uint32_t taken = acceptor.accepted->Read(
				received.data() + total, static_cast<uint32_t>(payload.size() - total));
			if (taken == 0) {
				break;
			}
			total += taken;
		}
	}
	ASSERT_EQUALS((unsigned)payload.size(), (unsigned)total);
	for (size_t i = 0; i < payload.size(); ++i) {
		CFormat format("byte %u differs");
		ASSERT_EQUALS_M((int)payload[i], (int)received[i], format % unsigned(i));
	}
	Teardown(loop);
}

TEST(UtpLibraryAdapter, DeliveryStopsAtTheConfiguredBoundAndResumesOnTheCrossing)
{
	// The window is opt_rcvbuf minus reported occupancy, so a reader that stops
	// stops the peer. Resuming needs the crossing, not an empty buffer.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);

	const size_t bound = CUtpStream::kDefaultReadBound;
	std::vector<uint8_t> payload(bound * 2);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>((i * 7) & 0xFF);
	}

	// Offer everything without reading a byte. Delivery must stall at the
	// bound: more than that in flight would mean the 64 KiB never took effect.
	size_t offered = 0;
	for (int round = 0; round < 64 && offered < payload.size(); ++round) {
		utp_iovec vector{ payload.data() + offered, payload.size() - offered };
		const ssize_t accepted = utp_writev(loop.clientSocket, &vector, 1);
		if (accepted > 0) {
			offered += static_cast<size_t>(accepted);
		}
		Pump(loop);
	}
	// Occupancy is this type's business, not every transport's.
	auto *utp = static_cast<CUtpSocketTransport *>(acceptor.accepted.get());
	const size_t buffered = utp->ReadBufferSize();
	// Short of the bound: libutp stops once the window cannot hold a packet.
	// Without UTP_RCVBUF the 1 MiB default would govern and all of it would be
	// here.
	CFormat stalled("buffered %u, bound %u, payload %u");
	const wxString detail = stalled % unsigned(buffered) % unsigned(bound) % unsigned(payload.size());
	ASSERT_TRUE_M(buffered < payload.size(), detail);
	ASSERT_TRUE_M(buffered + 2048 >= bound, detail);

	// Read just past the bound, leaving the buffer full but below it. An
	// empty-buffer rule would signal nothing here and the peer would stall.
	// Enough to reopen the window without emptying the buffer.
	const size_t toRead = CUtpStream::kWindowSlackBytes * 2;
	std::vector<uint8_t> received(payload.size());
	size_t total = 0;
	while (total < toRead) {
		const uint32_t taken = acceptor.accepted->Read(
			received.data() + total, static_cast<uint32_t>(toRead - total));
		if (taken == 0) {
			break;
		}
		total += taken;
	}
	ASSERT_TRUE(utp->ReadBufferSize() != 0);

	// The rest must now arrive.
	for (int round = 0; round < 512 && (offered < payload.size() || total < payload.size()); ++round) {
		if (offered < payload.size()) {
			utp_iovec vector{ payload.data() + offered, payload.size() - offered };
			const ssize_t accepted = utp_writev(loop.clientSocket, &vector, 1);
			if (accepted > 0) {
				offered += static_cast<size_t>(accepted);
			}
		}
		Pump(loop);
		// Read the way CEMSocket::OnReceive does: a six-byte header, then a
		// bounded body, then return with the rest still queued. A reader that
		// keeps pace this way never empties the buffer, which is precisely why
		// an empty-buffer rule would never reopen the window and this transfer
		// would stall short of the payload.
		// One packet per round, like CEMSocket. Does NOT discriminate the
		// crossing rule from an empty-buffer one: the reader outpaces the
		// sender and the buffer empties. PacketReaderReopensWindowWithoutEmptying
		// covers that.
		for (int packet = 0; packet < 1 && total < payload.size(); ++packet) {
			uint8_t header[6] = { 0 };
			const uint32_t headerTaken = acceptor.accepted->Read(header, sizeof(header));
			if (headerTaken == 0) {
				break;
			}
			std::copy(header, header + headerTaken, received.begin() + total);
			total += headerTaken;
			const size_t body = std::min<size_t>(1024, payload.size() - total);
			if (body == 0) {
				break;
			}
			total +=
				acceptor.accepted->Read(received.data() + total, static_cast<uint32_t>(body));
		}
	}
	CFormat progress("offered=%u total=%u buffered=%u drained-edges=%u");
	const wxString diag = progress % unsigned(offered) % unsigned(total) %
			      unsigned(utp->ReadBufferSize()) % unsigned(sink.datagrams);
	ASSERT_EQUALS_M((unsigned)payload.size(), (unsigned)total, diag);
	for (size_t i = 0; i < payload.size(); ++i) {
		CFormat format("byte %u differs after the window reopened");
		ASSERT_EQUALS_M((int)payload[i], (int)received[i], format % unsigned(i));
	}
	Teardown(loop);
}

TEST(UtpLibraryAdapter, DestroyingTheContextEndsTheStreamsItOwned)
{
	// utp_destroy() destroys the sockets it owns; ~UTPSocket emits DESTROYING,
	// which is what makes each transport drop its handle.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	// Live before the teardown, or what follows proves nothing.
	ASSERT_TRUE(acceptor.accepted->IsOk());

	loop.server->Destroy();

	// The discriminating assertion: without DESTROYING the handle stays set,
	// pointing into a destroyed context.
	auto *utp = static_cast<CUtpSocketTransport *>(acceptor.accepted.get());
	ASSERT_TRUE(utp->SocketHandle() == nullptr);
	ASSERT_FALSE(acceptor.accepted->IsOk());
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	// The order a real teardown produces; both must be harmless.
	loop.server->Destroy();
	acceptor.accepted->Close();
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AClosingStreamKeepsItsEndpointUntilTheSocketDies)
{
	// utp_close() only starts the socket dying: the peer still has to
	// acknowledge our FIN, and the ingress gate drops anything from an endpoint
	// it does not know. Deregistering at close therefore hid that acknowledgement
	// from libutp, which retransmitted the FIN until it gave up. The endpoint is
	// dropped when the socket is destroyed, which is what the registry is for.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));

	acceptor.accepted->Close();
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	// The peer's answer to the FIN still reaches libutp rather than the gate.
	Pump(loop);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));

	loop.server->Destroy();
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AClosedStreamStaysObfuscatedUntilTheSocketDies)
{
	// CloseSocket() drops the transport, but libutp still sends on the socket:
	// here the ACK of data the peer sent before it saw our FIN.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	const uint8_t userHash[16] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 9, 8, 7, 6, 5, 4 };
	static_cast<CUtpSocketTransport *>(acceptor.accepted.get())->SetCryptParameters(true, userHash);
	// Takes the accepted socket out of SYN_RECV, where utp_close() sends no FIN.
	const uint8_t payload = 0xA5;
	utp_iovec vector{ const_cast<uint8_t *>(&payload), 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	Pump(loop);

	acceptor.accepted->Close();
	const unsigned afterFin = sink.datagrams;
	ASSERT_TRUE(sink.lastEncrypt);
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	sink.lastEncrypt = false;
	Pump(loop);
	Pump(loop);
	ASSERT_TRUE(sink.datagrams > afterFin);
	ASSERT_TRUE(sink.lastEncrypt);
	ASSERT_EQUALS(16, (int)sink.lastUserHash.size());
	for (unsigned i = 0; i < sink.lastUserHash.size(); ++i) {
		ASSERT_EQUALS((int)userHash[i], (int)sink.lastUserHash[i]);
	}
	Teardown(loop);
}

TEST(UtpLibraryAdapter, ARefusedSecondStreamMustNotDeregisterTheLivePeer)
{
	// A peer uses one client UDP port, so a second stream shares its endpoint
	// with the first. CloseSocket() removes for every socket that has a
	// transport, refused ones included, so registering only the admitted ones
	// would have the refusal decrement the live stream's count and drop it off
	// the gate its own frames pass through -- for about fifteen seconds, until
	// the peer times out. TooManySockets is refused exactly when streams are
	// live, so this is the busy case.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));

	acceptor.admit = false;
	utp_socket *second = utp_create_socket(loop.client);
	const sockaddr_in peer = Address(kPeerIp, kPeerPort);
	utp_connect(second, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
	Pump(loop);
	Pump(loop);

	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

namespace
{
const uint8_t kPeerKey[16] = { 7, 7, 7, 7, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
const uint8_t kOtherKey[16] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };

bool IsKey(const std::vector<uint8_t> &key, const uint8_t (&expected)[16])
{
	return key.size() == 16 && std::equal(key.begin(), key.end(), expected);
}

//! Hands the client's SYN to the server as plain, keyed for the reply with @a preHelloKey.
void DeliverSyn(SLoopback &loop, const uint8_t *preHelloKey)
{
	const std::vector<uint8_t> syn = loop.toServer.front();
	loop.toServer.pop_front();
	loop.server->ProcessDatagram(
		syn.data(), syn.size(), kPeerIp, kPeerPort, loop.nowMs, false, preHelloKey);
}

//! The client sends one byte so the server's socket leaves SYN_RECV and acknowledges it.
void ClientSendsAByte(SLoopback &loop)
{
	const uint8_t payload = 0xA5;
	utp_iovec vector{ const_cast<uint8_t *>(&payload), 1 };
	utp_writev(loop.clientSocket, &vector, 1);
	Pump(loop);
}
} // namespace

TEST(UtpLibraryAdapter, AnObfuscatedDialSendsOurKeyFrameBeforeTheSyn)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));

	std::unique_ptr<IStreamTransport> transport;
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, true, kPeerKey, transport));
	ASSERT_TRUE(sink.events.size() >= 2);
	ASSERT_TRUE(sink.events[0] == "key-frame");
	ASSERT_TRUE(IsKey(sink.keys[0], kPeerKey));
	ASSERT_EQUALS(kPeerIp, sink.keyFrameIp);
	ASSERT_EQUALS((int)kPeerPort, (int)sink.keyFramePort);
	ASSERT_TRUE(sink.events[1] == "datagram-keyed");
	Teardown(loop);
}

TEST(UtpLibraryAdapter, APlainDialSendsNoKeyFrame)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));

	std::unique_ptr<IStreamTransport> transport;
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, transport));
	ASSERT_EQUALS(1, (int)sink.events.size());
	ASSERT_TRUE(sink.events[0] == "datagram-plain");
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AKeyedSynHasItsSynAckAndPreHelloFramesObfuscatedWithThatKey)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);

	DeliverSyn(loop, kPeerKey);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_EQUALS(1, (int)sink.events.size());
	ASSERT_TRUE(sink.events[0] == "datagram-keyed");
	ASSERT_TRUE(IsKey(sink.keys[0], kPeerKey));
	// The key only chooses how we send; the peer's SYN arrived plain.
	ASSERT_FALSE(acceptor.accepted->ObfuscatesStream());

	Pump(loop);
	ClientSendsAByte(loop);
	ASSERT_TRUE(sink.events.size() > 1);
	ASSERT_TRUE(sink.lastEncrypt);
	ASSERT_TRUE(IsKey(sink.lastUserHash, kPeerKey));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, ASynWithoutAKeyStillHasItsSynAckSentPlain)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);

	DeliverSyn(loop, nullptr);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_EQUALS(1, (int)sink.events.size());
	ASSERT_TRUE(sink.events[0] == "datagram-plain");
	Teardown(loop);
}

TEST(UtpLibraryAdapter, TheHelloReplacesThePreHelloKey)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	DeliverSyn(loop, kPeerKey);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);

	acceptor.accepted->SetCryptParameters(false, nullptr);
	ClientSendsAByte(loop);
	ASSERT_FALSE(sink.lastEncrypt);

	acceptor.accepted->SetCryptParameters(true, kOtherKey);
	ClientSendsAByte(loop);
	ASSERT_TRUE(sink.lastEncrypt);
	ASSERT_TRUE(IsKey(sink.lastUserHash, kOtherKey));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AnAnnouncedKeySwitchesAPlainDialStillInItsHandshake)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	StartClientAcceptor(loop);

	std::unique_ptr<IStreamTransport> transport;
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, false, nullptr, transport));
	ASSERT_FALSE(loop.server->ApplyAnnouncedKey(kPeerIp, kPeerPort + 1, kPeerKey));
	ASSERT_TRUE(loop.server->ApplyAnnouncedKey(kPeerIp, kPeerPort, kPeerKey));
	Pump(loop);
	ASSERT_TRUE(transport->IsConnected());
	const uint8_t payload = 0x5A;
	ASSERT_EQUALS(1, (int)transport->Write(&payload, 1));
	transport->Flush();
	Pump(loop);
	ASSERT_TRUE(sink.lastEncrypt);
	ASSERT_TRUE(IsKey(sink.lastUserHash, kPeerKey));
	// Connected: the hello is about to decide, and a late key frame changes nothing.
	ASSERT_FALSE(loop.server->ApplyAnnouncedKey(kPeerIp, kPeerPort, kOtherKey));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AnAnnouncedKeyNeverReplacesTheKeyWeDialledWith)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	StartClientAcceptor(loop);

	std::unique_ptr<IStreamTransport> transport;
	ASSERT_TRUE(loop.server->Dial(kPeerIp, kPeerPort, true, kPeerKey, transport));
	ASSERT_FALSE(loop.server->ApplyAnnouncedKey(kPeerIp, kPeerPort, kOtherKey));
	Pump(loop);
	ASSERT_TRUE(IsKey(sink.lastUserHash, kPeerKey));
	Teardown(loop);
}

// File_checked_for_headers

TEST(UtpLibraryAdapter, ARefusedSynGetsNoSynAckAndNoSocket)
{
	// Asked before libutp creates a socket or answers, so a forged source learns nothing.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	acceptor.admitSyn = false;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);

	ASSERT_EQUALS(1, acceptor.synChecks);
	ASSERT_EQUALS(0, acceptor.synDecisions);
	ASSERT_EQUALS(0, acceptor.offers);
	ASSERT_EQUALS(0u, sink.datagrams);
	ASSERT_FALSE(loop.clientConnected);
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AnAcceptedStreamIsHalfOpenUntilItsPeerSendsData)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_EQUALS(0, (int)acceptor.lastHalfOpen);
	ASSERT_FALSE(acceptor.accepted->IsConnected());
	// The fake acceptor holds only the latest stream.
	const std::unique_ptr<IStreamTransport> first = std::move(acceptor.accepted);

	// A second SYN while the first waits counts it. Its SYN-ACK goes nowhere.
	sink.dropOldPort = true;
	const std::vector<uint8_t> syn = UtpFrame(kUtpTypeSyn, 0x4242, 1, 0);
	loop.server->ProcessDatagram(syn.data(), syn.size(), kPeerIp, kPeerPort + 7, loop.nowMs);
	ASSERT_EQUALS(1, (int)acceptor.lastHalfOpen);

	uint8_t payload = 0x5A;
	utp_iovec vector{ &payload, 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	Pump(loop);
	ASSERT_TRUE(first->IsConnected());
	ASSERT_FALSE(acceptor.accepted->IsConnected());
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AForgedSynNextToALiveStreamLeavesItsFramesAlone)
{
	// One forged SYN from a port beside a live stream must neither make its own frames look like
	// a move nor make a real move ambiguous.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);
	loop.server->SetAcceptor(&acceptor);
	ExchangeOneFrameEachWay(loop, *transport, 0x11);

	std::array<uint8_t, 4> received{};
	ASSERT_EQUALS(1, (int)ReadUntilData(loop, *transport, received));

	// The forged SYN's SYN-ACK goes nowhere.
	sink.dropOldPort = true;
	const std::vector<uint8_t> syn = UtpFrame(kUtpTypeSyn, 0x4242, 1, 0);
	loop.server->ProcessDatagram(syn.data(), syn.size(), kPeerIp, heldPort + 1, loop.nowMs);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_FALSE(loop.server->HasRepointCandidate(kPeerIp, heldPort));
	ASSERT_TRUE(loop.server->HasRepointCandidate(kPeerIp, heldPort + 100));

	loop.clientRead.clear();
	ExchangeOneFrameEachWay(loop, *transport, 0x22);
	ASSERT_TRUE(!loop.clientRead.empty());
	ASSERT_EQUALS(1, (int)ReadUntilData(loop, *transport, received));
	ASSERT_EQUALS(0x22, (int)received[0]);
	ASSERT_EQUALS(0, (int)sink.streamMoves);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, OnlyAResetThatAnswersOurLastSendEndsTheStream)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	std::unique_ptr<IStreamTransport> transport;
	const uint16_t heldPort = DialRemapped(loop, sink, transport);
	ExchangeOneFrameEachWay(loop, *transport, 0x33);
	const SFrameNumbers ours = FrameNumbers(sink.lastPayload);
	const SFrameNumbers theirs = FrameNumbers(loop.lastClientDatagram);

	// The right connection id, but an ack far from anything we sent.
	const std::vector<uint8_t> forged = UtpFrame(
		kUtpTypeReset, theirs.connectionId, 1, static_cast<uint16_t>(ours.seqNr + kUtpResetAckSpan));
	loop.server->ProcessDatagram(forged.data(), forged.size(), kPeerIp, heldPort, loop.nowMs);
	ASSERT_TRUE(transport->IsConnected());

	const std::vector<uint8_t> genuine = UtpFrame(kUtpTypeReset, theirs.connectionId, 1, ours.seqNr);
	loop.server->ProcessDatagram(genuine.data(), genuine.size(), kPeerIp, heldPort, loop.nowMs);
	ASSERT_FALSE(transport->IsConnected());
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AFrameAboveTheMtuNeverReachesLibutp)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	std::vector<uint8_t> syn = UtpFrame(kUtpTypeSyn, 0x4242, 1, 0);
	syn.resize(kMaxUtpFrameBytes + 1);
	ASSERT_FALSE(loop.server->ProcessDatagram(syn.data(), syn.size(), kPeerIp, kPeerPort, loop.nowMs));
	ASSERT_EQUALS(0, acceptor.synChecks);
	ASSERT_TRUE(ClassifyUtpFrame(syn.data(), syn.size()) == EUtpFrameKind::Malformed);
	syn.resize(kMaxUtpFrameBytes);
	ASSERT_TRUE(ClassifyUtpFrame(syn.data(), syn.size()) == EUtpFrameKind::Syn);
	Teardown(loop);
}

TEST(UtpLibraryAdapter, AHalfOpenStreamIsEndedAtTheFirstSynAfterItsLimit)
{
	// A forged SYN holds a budget slot only as long as an honest dialer needs to speak.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	const std::unique_ptr<IStreamTransport> honest = std::move(acceptor.accepted);
	ASSERT_TRUE(honest != nullptr);
	uint8_t payload = 0x5A;
	utp_iovec vector{ &payload, 1 };
	ASSERT_EQUALS(1, (int)utp_writev(loop.clientSocket, &vector, 1));
	Pump(loop);
	ASSERT_TRUE(honest->IsConnected());

	// Its SYN-ACK goes nowhere, so its peer never speaks.
	sink.dropOldPort = true;
	const std::vector<uint8_t> forged = UtpFrame(kUtpTypeSyn, 0x4242, 1, 0);
	loop.server->ProcessDatagram(forged.data(), forged.size(), kPeerIp, kPeerPort + 7, loop.nowMs);
	const std::unique_ptr<IStreamTransport> stale = std::move(acceptor.accepted);
	ASSERT_TRUE(stale != nullptr);

	const std::vector<uint8_t> early = UtpFrame(kUtpTypeSyn, 0x4343, 1, 0);
	loop.server->ProcessDatagram(
		early.data(), early.size(), kPeerIp, kPeerPort + 8, loop.nowMs + kUtpHalfOpenLimitMs - 1);
	ASSERT_EQUALS(1, (int)acceptor.lastHalfOpen);
	ASSERT_TRUE(stale->IsOk());
	const std::unique_ptr<IStreamTransport> fresh = std::move(acceptor.accepted);

	const std::vector<uint8_t> late = UtpFrame(kUtpTypeSyn, 0x4444, 1, 0);
	loop.server->ProcessDatagram(
		late.data(), late.size(), kPeerIp, kPeerPort + 9, loop.nowMs + kUtpHalfOpenLimitMs);
	// The stale stream is ended and not counted. The fresh one still waits, and the honest one
	// spoke, so it was never at risk.
	ASSERT_EQUALS(1, (int)acceptor.lastHalfOpen);
	ASSERT_FALSE(stale->IsOk());
	ASSERT_TRUE(fresh->IsOk());
	ASSERT_TRUE(honest->IsConnected());
	Teardown(loop);
}
