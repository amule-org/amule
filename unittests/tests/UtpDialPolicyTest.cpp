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
#include "UtpDialPolicy.h"

#include <optional>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(UtpDialPolicy)

TEST(UtpDialPolicy, DecisionMatrix)
{
	// The one accepting combination: every fact set except the proxy.
	for (unsigned bits = 0; bits < 64; ++bits) {
		const SUtpDialFacts facts{ (bits % 2) != 0,
			((bits / 2) % 2) != 0,
			((bits / 4) % 2) != 0,
			((bits / 8) % 2) != 0,
			((bits / 16) % 2) != 0,
			((bits / 32) % 2) != 0 };
		ASSERT_EQUALS(bits == 55, DecideUtpDial(facts) == EUtpDialDecision::TryUtp);
	}
}

TEST(UtpDialPolicy, ServerVouchedLowIdMayDialWithoutPeerHello)
{
	SUtpDialFacts facts{ true, true, false, false, true, true, true };
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::TryUtp);
	facts.serverVouchedLowId = false;
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::PreserveLegacy);
	facts.serverVouchedLowId = true;
	facts.routableEndpoint = false;
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::PreserveLegacy);
}

TEST(UtpDialPolicy, EndpointRoutability)
{
	SUtpDialFacts facts{ true, true, true, false, true, true };
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::TryUtp);
	facts.routableEndpoint = false;
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::PreserveLegacy);
}

TEST(UtpDialPolicy, SimultaneousConnectionsUseOppositeDirectionDecisions)
{
	const uint8_t lower[16] = { 0 };
	const uint8_t higher[16] = { 1 };

	ASSERT_TRUE(ShouldKeepFoundUtp(false, lower, higher));
	ASSERT_TRUE(ShouldKeepFoundUtp(true, higher, lower));
	ASSERT_FALSE(ShouldKeepFoundUtp(true, lower, higher));
	ASSERT_FALSE(ShouldKeepFoundUtp(false, higher, lower));
}

TEST(UtpDialPolicy, SimultaneousOpenUsesEndpointTupleOrder)
{
	const SUtpEndpoint localLower{ { 192, 0, 2, 1 }, 4672 };
	const SUtpEndpoint remoteHigher{ { 192, 0, 2, 2 }, 4672 };
	const SUtpEndpoint sameAddressHigherPort{ { 192, 0, 2, 1 }, 4673 };

	ASSERT_TRUE(DecideUtpSimultaneousOpen(localLower, remoteHigher, nullptr, false, nullptr, false) ==
		    EUtpSimultaneousOpenDecision::KeepOutgoing);
	ASSERT_TRUE(DecideUtpSimultaneousOpen(remoteHigher, localLower, nullptr, false, nullptr, false) ==
		    EUtpSimultaneousOpenDecision::KeepIncoming);
	ASSERT_TRUE(DecideUtpSimultaneousOpen(
			    localLower, sameAddressHigherPort, nullptr, false, nullptr, false) ==
		    EUtpSimultaneousOpenDecision::KeepOutgoing);
}

TEST(UtpDialPolicy, EndpointFactoryUsesEd2kNetworkOrder)
{
	const auto endpoint = MakeUtpEndpointFromNetworkOrder(0x010200C0, 4672);
	ASSERT_TRUE(endpoint.has_value());
	ASSERT_EQUALS((uint8_t)192, endpoint->address[0]);
	ASSERT_EQUALS((uint8_t)0, endpoint->address[1]);
	ASSERT_EQUALS((uint8_t)2, endpoint->address[2]);
	ASSERT_EQUALS((uint8_t)1, endpoint->address[3]);
	ASSERT_FALSE(MakeUtpEndpointFromNetworkOrder(0, 4672).has_value());
	ASSERT_FALSE(MakeUtpEndpointFromNetworkOrder(0x010200C0, 0).has_value());
}

TEST(UtpDialPolicy, LocalEndpointUsesBestAvailableUdpPort)
{
	const std::uint32_t publicIp = 0x010200C0;
	ASSERT_EQUALS((int)4777, (int)MakeLocalUtpEndpoint(publicIp, 4777, 4662, 4661)->port);
	ASSERT_EQUALS((int)4662, (int)MakeLocalUtpEndpoint(publicIp, 0, 4662, 4661)->port);
	ASSERT_EQUALS((int)4661, (int)MakeLocalUtpEndpoint(publicIp, 0, 0, 4661)->port);
	ASSERT_FALSE(MakeLocalUtpEndpoint(0, 4777, 4662, 4661).has_value());
}

TEST(UtpDialPolicy, RejectsIncomingSynOnlyWhenExternalUdpPortIsKnown)
{
	ASSERT_TRUE(CanRejectIncomingUtpSynForSimultaneousOpen(4672));
	ASSERT_FALSE(CanRejectIncomingUtpSynForSimultaneousOpen(0));
}

TEST(UtpDialPolicy, TranslationRefusesEndpointCollisions)
{
	ASSERT_TRUE(CanInstallUtpPortTranslation(false, false));
	ASSERT_FALSE(CanInstallUtpPortTranslation(true, false));
	ASSERT_FALSE(CanInstallUtpPortTranslation(false, true));
	ASSERT_FALSE(CanInstallUtpPortTranslation(true, true));
}

TEST(UtpDialPolicy, HelloAttachmentHonorsOnlyTheVerifiedSynWinnerToken)
{
	const uint8_t localHash[16] = { 0 };
	const uint8_t peerHash[16] = { 1 };

	const SUtpHelloAttachmentDecision synWinner =
		DecideUtpHelloAttachment(true, false, true, localHash, peerHash);
	ASSERT_FALSE(synWinner.keepFound);
	ASSERT_TRUE(synWinner.socketSwap);

	const SUtpHelloAttachmentDecision differentHelloPeer =
		DecideUtpHelloAttachment(false, false, true, localHash, peerHash);
	ASSERT_TRUE(differentHelloPeer.keepFound);
	ASSERT_FALSE(differentHelloPeer.socketSwap);

	const SUtpHelloAttachmentDecision withoutPeerHash =
		DecideUtpHelloAttachment(false, false, false, localHash, nullptr);
	ASSERT_TRUE(withoutPeerHash.keepFound);
	ASSERT_FALSE(withoutPeerHash.socketSwap);
}

TEST(UtpDialPolicy, HelloAttachmentSwapsToVerifiedWinnerAndRecoversFileRequest)
{
	const uint8_t localHash[16] = { 0 };
	const uint8_t peerHash[16] = { 1 };
	const SUtpHelloAttachmentDecision decision =
		DecideUtpHelloAttachment(true, false, true, localHash, peerHash);
	bool fileRequestLost = false;
	bool outgoingAlive = true;
	bool incomingAttached = false;
	int sequence = 0;

	const bool swapped = ApplyUtpHelloSocketSwap(
		decision,
		true,
		[&]() {
			ASSERT_EQUALS(0, sequence);
			sequence = 1;
			fileRequestLost = true;
		},
		[&]() {
			ASSERT_EQUALS(1, sequence);
			sequence = 2;
			outgoingAlive = false;
		},
		[&]() {
			ASSERT_EQUALS(2, sequence);
			sequence = 3;
			incomingAttached = true;
		});

	ASSERT_TRUE(swapped);
	ASSERT_FALSE(outgoingAlive);
	ASSERT_TRUE(incomingAttached);
	ASSERT_TRUE(fileRequestLost);
	ASSERT_EQUALS(3, sequence);
	int requestsSent = 0;
	if (ConsumeUtpFileRequestRetry(fileRequestLost, true)) {
		++requestsSent;
	}
	ASSERT_EQUALS(1, requestsSent);
	ASSERT_FALSE(fileRequestLost);
}

TEST(UtpDialPolicy, IncomingWinnerTokenIsConsumedOnlyByOneVerifiedHello)
{
	SUtpIncomingSynWinner winner;
	winner.Select(91);
	ASSERT_FALSE(winner.Consume(92));
	ASSERT_FALSE(winner.Consume(91));

	winner.Select(91);
	ASSERT_TRUE(winner.Consume(91));
	ASSERT_FALSE(winner.Consume(91));

	winner.Select(91);
	ASSERT_EQUALS((uint32_t)91, winner.Take());
	ASSERT_EQUALS((uint32_t)0, winner.Take());
}

TEST(UtpDialPolicy, HelloSocketSwapRetriesFileRequestOnSurvivingConnection)
{
	bool fileRequestLost = true;
	int requestsSent = 0;
	ASSERT_TRUE(RetryFileRequestAfterUtpSocketSwap(fileRequestLost, true, [&]() { ++requestsSent; }));
	ASSERT_EQUALS(1, requestsSent);
	ASSERT_FALSE(fileRequestLost);
	ASSERT_FALSE(RetryFileRequestAfterUtpSocketSwap(fileRequestLost, true, [&]() { ++requestsSent; }));
	ASSERT_EQUALS(1, requestsSent);
}

TEST(UtpDialPolicy, SimultaneousOpenUsesHashesOnlyWhenEndpointsCannotOrder)
{
	const SUtpEndpoint endpoint{ { 192, 0, 2, 1 }, 4672 };
	const uint8_t lowerHash[16] = { 0 };
	const uint8_t higherHash[16] = { 1 };
	const std::optional<SUtpEndpoint> unavailable;

	ASSERT_TRUE(DecideUtpSimultaneousOpen(unavailable, endpoint, lowerHash, true, higherHash, true) ==
		    EUtpSimultaneousOpenDecision::KeepOutgoing);
	ASSERT_TRUE(DecideUtpSimultaneousOpen(unavailable, endpoint, higherHash, true, lowerHash, true) ==
		    EUtpSimultaneousOpenDecision::KeepIncoming);
	ASSERT_TRUE(DecideUtpSimultaneousOpen(endpoint, endpoint, lowerHash, true, higherHash, true) ==
		    EUtpSimultaneousOpenDecision::KeepOutgoing);
}

TEST(UtpDialPolicy, SimultaneousOpenRejectsWhenNoDeterministicOrderExists)
{
	const uint8_t equalHash[16] = { 1 };
	const std::optional<SUtpEndpoint> unavailable;

	ASSERT_TRUE(DecideUtpSimultaneousOpen(unavailable, unavailable, nullptr, false, nullptr, false) ==
		    EUtpSimultaneousOpenDecision::KeepOutgoing);
	ASSERT_TRUE(DecideUtpSimultaneousOpen(unavailable, unavailable, equalHash, true, equalHash, true) ==
		    EUtpSimultaneousOpenDecision::KeepOutgoing);
	ASSERT_TRUE(DecideUtpSimultaneousOpen(unavailable, unavailable, equalHash, false, equalHash, false) ==
		    EUtpSimultaneousOpenDecision::KeepOutgoing);
}

// A peer owed obfuscation is kept on TCP rather than dialled in the clear: the
// stream handshake does not run over a transport, so the frames are the only
// thing that could carry it.
TEST(UtpDialPolicy, ObfuscationOwedButUnavailableStaysOnTcp)
{
	SUtpDialFacts facts{ true, true, true, false, true, true };
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::TryUtp);
	facts.obfuscationSatisfied = false;
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::PreserveLegacy);
}

TEST(UtpDialPolicy, RemapRequiresOneLiveExpectedPeerDuringTheHandshake)
{
	constexpr std::uint32_t ip = 0x0100007F;
	std::vector<SUtpRemapCandidate> candidates{ { ip, 4672, 100, true, true, true } };

	const auto lowerBoundary = FindUniqueUtpPortRemapCandidate(candidates, ip, 4160, 30'099);
	ASSERT_TRUE(lowerBoundary.has_value());
	ASSERT_EQUALS((std::size_t)0, *lowerBoundary);
	const auto upperBoundary = FindUniqueUtpPortRemapCandidate(candidates, ip, 5184, 30'099);
	ASSERT_TRUE(upperBoundary.has_value());
	ASSERT_EQUALS((std::size_t)0, *upperBoundary);
	ASSERT_FALSE(FindUniqueUtpPortRemapCandidate(candidates, ip, 5185, 30'099).has_value());
	ASSERT_FALSE(FindUniqueUtpPortRemapCandidate(candidates, ip, 4160, 30'100).has_value());
	ASSERT_FALSE(FindUniqueUtpPortRemapCandidate(candidates, ip + 1, 4160, 30'099).has_value());
	ASSERT_FALSE(FindUniqueUtpPortRemapCandidate(candidates, ip, 4672, 30'099).has_value());

	candidates.push_back({ ip, 5000, 100, true, true, true });
	ASSERT_FALSE(FindUniqueUtpPortRemapCandidate(candidates, ip, 4800, 30'099).has_value());
	candidates.pop_back();
	candidates[0].handshakePending = false;
	ASSERT_FALSE(FindUniqueUtpPortRemapCandidate(candidates, ip, 4160, 30'099).has_value());
	candidates[0].handshakePending = true;
	candidates[0].remapAllowed = false;
	ASSERT_FALSE(FindUniqueUtpPortRemapCandidate(candidates, ip, 4160, 30'099).has_value());
	candidates[0].remapAllowed = true;
	candidates[0].registeredAtMs = 30'200;
	ASSERT_FALSE(FindUniqueUtpPortRemapCandidate(candidates, ip, 4160, 30'099).has_value());
}

TEST(UtpDialPolicy, IncomingSynFindsOnlyOneLiveOutboundHandshake)
{
	constexpr std::uint32_t ip = 0x0100007F;
	std::vector<SUtpRemapCandidate> candidates{ { ip, 4672, 100, true, false, true } };

	const auto exact = FindUniqueUtpSynCandidate(candidates, ip, 4672, 100);
	ASSERT_TRUE(exact.status == EUtpSynCandidateStatus::Unique);
	ASSERT_EQUALS((std::size_t)0, exact.index);

	candidates[0].remapAllowed = true;
	const auto changedPort = FindUniqueUtpSynCandidate(candidates, ip, 4160, 30'099);
	ASSERT_TRUE(changedPort.status == EUtpSynCandidateStatus::Unique);
	ASSERT_EQUALS((std::size_t)0, changedPort.index);
	ASSERT_TRUE(FindUniqueUtpSynCandidate(candidates, ip, 4160, 30'100).status ==
		    EUtpSynCandidateStatus::None);
	ASSERT_TRUE(FindUniqueUtpSynCandidate(candidates, ip + 1, 4160, 30'099).status ==
		    EUtpSynCandidateStatus::None);

	candidates[0].outbound = false;
	ASSERT_TRUE(
		FindUniqueUtpSynCandidate(candidates, ip, 4672, 100).status == EUtpSynCandidateStatus::None);
	candidates[0].outbound = true;
	candidates.push_back({ ip, 5000, 100, true, true, true });
	ASSERT_TRUE(FindUniqueUtpSynCandidate(candidates, ip, 4800, 30'099).status ==
		    EUtpSynCandidateStatus::Ambiguous);
}

TEST(UtpDialPolicy, ARecentFailureAtTheEndpointKeepsTheLegacyConnect)
{
	SUtpDialFacts facts{ true, true, true, false, true, true };
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::TryUtp);

	facts.recentDialFailure = true;
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::PreserveLegacy);
}

TEST(UtpDialPolicy, AFailedEndpointIsSkippedUntilItsCooldownEnds)
{
	UtpDialFailureMemory failures;
	ASSERT_FALSE(failures.IsCoolingDown(0x0A000001, 4672, 1'000));

	failures.RecordFailure(0x0A000001, 4672, 1'000);
	ASSERT_TRUE(failures.IsCoolingDown(0x0A000001, 4672, 1'000));
	ASSERT_TRUE(failures.IsCoolingDown(0x0A000001, 4672, 1'000 + UtpDialFailureMemory::CooldownMs - 1));
	ASSERT_FALSE(failures.IsCoolingDown(0x0A000001, 4672, 1'000 + UtpDialFailureMemory::CooldownMs));

	// The path failed, not the peer: another port or address is dialled as before.
	failures.RecordFailure(0x0A000001, 4672, 2'000);
	ASSERT_FALSE(failures.IsCoolingDown(0x0A000001, 4673, 2'000));
	ASSERT_FALSE(failures.IsCoolingDown(0x0A000002, 4672, 2'000));
}

TEST(UtpDialPolicy, ARecordedFailureRestartsItsCooldown)
{
	UtpDialFailureMemory failures;
	failures.RecordFailure(0x0A000001, 4672, 1'000);
	failures.RecordFailure(0x0A000001, 4672, 5'000);
	ASSERT_TRUE(failures.IsCoolingDown(0x0A000001, 4672, 1'000 + UtpDialFailureMemory::CooldownMs));
	ASSERT_FALSE(failures.IsCoolingDown(0x0A000001, 4672, 5'000 + UtpDialFailureMemory::CooldownMs));
}

TEST(UtpDialPolicy, FailureMemoryIsBoundedAndForgetsTheOldestFirst)
{
	UtpDialFailureMemory failures;
	for (std::uint32_t index = 0; index < UtpDialFailureMemory::MaxEntries; ++index) {
		failures.RecordFailure(0x0A000000 + index, 4672, 1'000 + index);
	}
	ASSERT_TRUE(failures.IsCoolingDown(0x0A000000, 4672, 2'000));

	failures.RecordFailure(0x0B000000, 4672, 3'000);
	ASSERT_FALSE(failures.IsCoolingDown(0x0A000000, 4672, 3'000));
	ASSERT_TRUE(failures.IsCoolingDown(0x0A000001, 4672, 3'000));
	ASSERT_TRUE(failures.IsCoolingDown(0x0B000000, 4672, 3'000));
}

TEST(UtpDialPolicy, RepointIsABoundedChangeOfAnExistingPort)
{
	// Both edges of the window are inside it, one port past them is not.
	ASSERT_TRUE(IsUtpPortRepointOf(4673, 4672));
	ASSERT_TRUE(IsUtpPortRepointOf(4672 + kUtpPortRemapWindow, 4672));
	ASSERT_TRUE(IsUtpPortRepointOf(4672 - kUtpPortRemapWindow, 4672));
	ASSERT_FALSE(IsUtpPortRepointOf(4672 + kUtpPortRemapWindow + 1, 4672));
	ASSERT_FALSE(IsUtpPortRepointOf(4672 - kUtpPortRemapWindow - 1, 4672));

	// The same port is the tuple we already hold, and a zero port is no endpoint at all.
	ASSERT_FALSE(IsUtpPortRepointOf(4672, 4672));
	ASSERT_FALSE(IsUtpPortRepointOf(0, 4672));
	ASSERT_FALSE(IsUtpPortRepointOf(4672, 0));
	ASSERT_FALSE(IsUtpPortRepointOf(0, 0));
}

TEST(UtpDialPolicy, RepointNeedsExactlyOneConnectedStreamOnThatAddress)
{
	constexpr std::uint32_t ip = 0x0100007F;
	ASSERT_FALSE(FindUniqueUtpRepointCandidate(std::vector<SUtpConnectedStream>{}, ip, 4160).has_value());

	std::vector<SUtpConnectedStream> streams{ { ip, 4672, true } };
	const auto unique = FindUniqueUtpRepointCandidate(streams, ip, 4160);
	ASSERT_TRUE(unique.has_value());
	ASSERT_EQUALS((std::size_t)0, *unique);

	// A stream that is not connected is no candidate, and neither is another address.
	streams[0].connected = false;
	ASSERT_FALSE(FindUniqueUtpRepointCandidate(streams, ip, 4160).has_value());
	streams[0].connected = true;
	ASSERT_FALSE(FindUniqueUtpRepointCandidate(streams, ip + 1, 4160).has_value());
	ASSERT_FALSE(FindUniqueUtpRepointCandidate(streams, ip, 4672).has_value());

	// Two streams within the window are a guess, so they fail closed; 4160 is within reach of
	// 4672 only, which makes that one unique again.
	streams.push_back({ ip, 5000, true });
	ASSERT_FALSE(FindUniqueUtpRepointCandidate(streams, ip, 4800).has_value());
	const auto bounded = FindUniqueUtpRepointCandidate(streams, ip, 4160);
	ASSERT_TRUE(bounded.has_value());
	ASSERT_EQUALS((std::size_t)0, *bounded);
	// The same endpoint twice is ambiguous even before the window is consulted.
	streams[1].port = 4672;
	ASSERT_FALSE(FindUniqueUtpRepointCandidate(streams, ip, 4160).has_value());
}

TEST(UtpDialPolicy, RepointLimiterAllowsOneMovePerStreamPerWindow)
{
	UtpPortRepointLimiter limiter;
	constexpr std::uint64_t stream = 7;
	ASSERT_TRUE(limiter.TryAdmit(stream, 1'000));
	ASSERT_FALSE(limiter.TryAdmit(stream, 1'000));
	ASSERT_FALSE(limiter.TryAdmit(stream, 1'000 + UtpPortRepointLimiter::WindowMs - 1));

	// The budget is per stream: another one still has its own.
	ASSERT_TRUE(limiter.TryAdmit(8, 1'000));
}

TEST(UtpDialPolicy, RepointLimiterRefillsOnceTheWindowPasses)
{
	UtpPortRepointLimiter limiter;
	constexpr std::uint64_t stream = 3;
	ASSERT_TRUE(limiter.TryAdmit(stream, 1'000));
	ASSERT_FALSE(limiter.TryAdmit(stream, 1'000 + UtpPortRepointLimiter::WindowMs - 1));
	ASSERT_TRUE(limiter.TryAdmit(stream, 1'000 + UtpPortRepointLimiter::WindowMs));
}

TEST(UtpDialPolicy, RepointLimiterFailsClosedWhenItsTableIsFull)
{
	UtpPortRepointLimiter limiter;
	for (std::uint64_t stream = 0; stream < UtpPortRepointLimiter::MaxStreams; ++stream) {
		ASSERT_TRUE(limiter.TryAdmit(stream, 1'000));
	}
	// A stream with budget left is still refused: a live record is never evicted for it.
	ASSERT_FALSE(limiter.TryAdmit(100, 1'000));
	ASSERT_TRUE(limiter.TryAdmit(100, 1'000 + UtpPortRepointLimiter::WindowMs));
}

TEST(UtpDialPolicy, MoveNeedsThePeersConnectionIdAndAWitnessedStream)
{
	// A stream whose peer said connId 0x1234, seq 100, ack 98, and to which we last sent seq 104.
	const SUtpPeerStreamFacts facts{ true, 0x1234, 100, 98, 104 };
	const SUtpMoveHeader matching{ kUtpTypeState, 0x1234, 101, 100 };
	ASSERT_TRUE(DecideUtpMove(matching, facts) == EUtpMoveVerdict::Move);

	SUtpMoveHeader wrongConnection = matching;
	wrongConnection.connectionId = 0x1235;
	ASSERT_TRUE(DecideUtpMove(wrongConnection, facts) == EUtpMoveVerdict::Refuse);
	wrongConnection.connectionId = 0;
	ASSERT_TRUE(DecideUtpMove(wrongConnection, facts) == EUtpMoveVerdict::Refuse);

	// Nothing witnessed on the stream means nothing to hold the frame to.
	SUtpPeerStreamFacts unknown = facts;
	unknown.known = false;
	ASSERT_TRUE(DecideUtpMove(matching, unknown) == EUtpMoveVerdict::Refuse);
}

TEST(UtpDialPolicy, MoveAdmitsAckOnlyKeepaliveAndOutOfOrderResends)
{
	// The peer's last frame used sequence number 1'000, so 1'001 is the number its next data frame
	// will carry and the one an ack-only ST_STATE carries today (utp_internal.cpp:782).
	const SUtpPeerStreamFacts facts{ true, 0x1234, 1'001, 98, 104 };
	SUtpMoveHeader header{ kUtpTypeState, 0x1234, 1'001, 100 };

	// Zero apart is ordinary: a download-only peer only ever sends ST_STATE, so its sequence number
	// never moves. A keepalive repeats the same one.
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Move);
	header.sequenceNumber = 1'001 + kUtpMoveSeqWindow;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Move);

	// Behind is admitted too, because that is what recovers a stalled bulk transfer: a peer whose
	// send window is full resends its OLDEST unacked packet, which sits behind the highest number we
	// have seen. libutp measures the same distance and accepts it (:1894-1907).
	header.sequenceNumber = 1'001 - kUtpMoveSeqWindow;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Move);
	header.sequenceNumber = 1'000;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Move);

	// One past either end of the window is not in flight and is refused.
	header.sequenceNumber = 1'001 + kUtpMoveSeqWindow + 1;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Refuse);
	header.sequenceNumber = 1'001 - kUtpMoveSeqWindow - 1;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Refuse);

	// Counting is modulo 2^16, so both directions hold across the wrap.
	ASSERT_EQUALS((std::uint16_t)2, UtpSeqDistance(1, 65'535));
	ASSERT_EQUALS((std::uint16_t)2, UtpSeqDistance(65'535, 1));
	ASSERT_EQUALS((std::uint16_t)0, UtpSeqDistance(7, 7));
	const SUtpPeerStreamFacts wrapped{ true, 0x1234, 65'535, 98, 104 };
	const SUtpMoveHeader newer{ kUtpTypeState, 0x1234, 1, 100 };
	ASSERT_TRUE(DecideUtpMove(newer, wrapped) == EUtpMoveVerdict::Move);
	const SUtpMoveHeader sameAcrossTheWrap{ kUtpTypeState, 0x1234, 65'535, 100 };
	ASSERT_TRUE(DecideUtpMove(sameAcrossTheWrap, wrapped) == EUtpMoveVerdict::Move);
	const SUtpMoveHeader olderAcrossTheWrap{ kUtpTypeState, 0x1234, 65'530, 100 };
	ASSERT_TRUE(DecideUtpMove(olderAcrossTheWrap, wrapped) == EUtpMoveVerdict::Move);
	const SUtpMoveHeader tooOldAcrossTheWrap{
		kUtpTypeState, 0x1234, 65'535 - kUtpMoveSeqWindow - 1, 100
	};
	ASSERT_TRUE(DecideUtpMove(tooOldAcrossTheWrap, wrapped) == EUtpMoveVerdict::Refuse);
}

TEST(UtpDialPolicy, MoveNeedsAnAckFromOneBelowThePeersLastUpToOurLastSend)
{
	// The peer last acknowledged 98 and we last sent 104.
	const SUtpPeerStreamFacts facts{ true, 0x1234, 101, 98, 104 };
	SUtpMoveHeader header{ kUtpTypeState, 0x1234, 101, 104 };

	// The upper end, then the peer's own last ack, then the one below it a keepalive carries
	// (utp_internal.cpp:833-843).
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Move);
	header.acknowledgeNumber = 98;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Move);
	header.acknowledgeNumber = 97;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Move);

	// Further back than a keepalive ever goes, and past anything we sent.
	header.acknowledgeNumber = 96;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Refuse);
	header.acknowledgeNumber = 105;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Refuse);

	// Facts that would mean the peer acknowledged more than we sent fail closed.
	SUtpPeerStreamFacts stale = facts;
	stale.peerAcknowledgeNumber = 104 - kUtpMoveAckSpanLimit - 1;
	header.acknowledgeNumber = 104;
	ASSERT_TRUE(DecideUtpMove(header, stale) == EUtpMoveVerdict::Refuse);

	// The range is modular, so a stream can move while its numbers sit either side of the wrap.
	const SUtpPeerStreamFacts wrapped{ true, 0x1234, 101, 0, 3 };
	SUtpMoveHeader aroundTheWrap{ kUtpTypeState, 0x1234, 101, 0 };
	ASSERT_TRUE(DecideUtpMove(aroundTheWrap, wrapped) == EUtpMoveVerdict::Move);
	aroundTheWrap.acknowledgeNumber = 3;
	ASSERT_TRUE(DecideUtpMove(aroundTheWrap, wrapped) == EUtpMoveVerdict::Move);
	// 65'535 is the peer's last ack minus one across the wrap, so it is still admissible.
	aroundTheWrap.acknowledgeNumber = 65'535;
	ASSERT_TRUE(DecideUtpMove(aroundTheWrap, wrapped) == EUtpMoveVerdict::Move);
	aroundTheWrap.acknowledgeNumber = 65'534;
	ASSERT_TRUE(DecideUtpMove(aroundTheWrap, wrapped) == EUtpMoveVerdict::Refuse);
	aroundTheWrap.acknowledgeNumber = 4;
	ASSERT_TRUE(DecideUtpMove(aroundTheWrap, wrapped) == EUtpMoveVerdict::Refuse);
}

TEST(UtpDialPolicy, AMoveNeverReturnsToThePortTheStreamLeftUntilThatRefusalExpires)
{
	const std::uint64_t nowMs = 1'000;
	ASSERT_FALSE(IsUtpAbandonedPort(4672, 0, 0, nowMs));
	ASSERT_TRUE(IsUtpAbandonedPort(4672, 4672, nowMs, nowMs));
	ASSERT_TRUE(IsUtpAbandonedPort(4672, 4672, nowMs, nowMs + UtpPortRepointLimiter::WindowMs - 1));
	ASSERT_FALSE(IsUtpAbandonedPort(4673, 4672, nowMs, nowMs));

	// A port-preserving NAT can hand the old port back, so the refusal ends with the move window.
	ASSERT_FALSE(IsUtpAbandonedPort(4672, 4672, nowMs, nowMs + UtpPortRepointLimiter::WindowMs));
}

TEST(UtpDialPolicy, AResendNeverLowersTheLastSentNumber)
{
	ASSERT_TRUE(IsUtpSeqAhead(101, 100));
	ASSERT_TRUE(IsUtpSeqAhead(1, 65'535)); // across the wrap

	// A resend carries an older number: it must not drag the record backwards, or the ack that
	// arrives for the newer frame would fall outside the range.
	ASSERT_FALSE(IsUtpSeqAhead(99, 100));
	ASSERT_FALSE(IsUtpSeqAhead(100, 100));
	ASSERT_FALSE(IsUtpSeqAhead(65'535, 1));
}

TEST(UtpDialPolicy, ProbeFiresOnlyForAFreshlySilentReceiver)
{
	SUtpProbeFacts facts;
	ASSERT_FALSE(ShouldProbeIdleStream(facts, 10'000)); // never received data

	facts.receivedData = true;
	facts.lastDataReceivedMs = 10'000;
	ASSERT_FALSE(ShouldProbeIdleStream(facts, 10'000));
	ASSERT_FALSE(ShouldProbeIdleStream(facts, 10'000 + kUtpProbeIdleMs - 1));
	ASSERT_TRUE(ShouldProbeIdleStream(facts, 10'000 + kUtpProbeIdleMs));
	ASSERT_TRUE(ShouldProbeIdleStream(facts, 10'000 + kUtpProbeFreshMs - 1));
	// Too stale: the sender has already given up, so a probe would only talk to itself.
	ASSERT_FALSE(ShouldProbeIdleStream(facts, 10'000 + kUtpProbeFreshMs));

	// The uploader also receives data, and its own sends are what keep it quiet.
	facts.sentData = true;
	facts.lastDataSentMs = 12'000;
	ASSERT_FALSE(ShouldProbeIdleStream(facts, 12'500));
	ASSERT_FALSE(ShouldProbeIdleStream(facts, 12'000 + kUtpProbeIdleMs - 1));
	ASSERT_TRUE(ShouldProbeIdleStream(facts, 12'000 + kUtpProbeIdleMs));

	// One probe per idle window.
	facts.sentData = false;
	facts.probed = true;
	facts.lastProbeMs = 13'000;
	ASSERT_FALSE(ShouldProbeIdleStream(facts, 13'000 + kUtpProbeIdleMs - 1));
	ASSERT_TRUE(ShouldProbeIdleStream(facts, 13'000 + kUtpProbeIdleMs));
}

TEST(UtpDialPolicy, MoveRefusesResetAndSynWhileTheOwnTupleIsNeverAStepCompetitor)
{
	const SUtpPeerStreamFacts facts{ true, 0x1234, 100, 98, 104 };
	SUtpMoveHeader header{ kUtpTypeState, 0x1234, 101, 100 };

	// libutp acts on a reset from its connection id alone (utp_internal.cpp:2866-2887) and a SYN
	// asks for a new connection: neither may be mapped onto a live stream.
	header.type = kUtpTypeReset;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Refuse);
	header.type = kUtpTypeSyn;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Refuse);
	header.type = 0;
	ASSERT_TRUE(DecideUtpMove(header, facts) == EUtpMoveVerdict::Move);

	// A frame on the socket's own tuple is never a move: the port does not change, so the adapter
	// hands it to libutp untouched and libutp's own handling of a reset still applies there.
	constexpr std::uint32_t ip = 0x0100007F;
	std::vector<SUtpConnectedStream> streams{ { ip, 4672, true } };
	ASSERT_FALSE(IsUtpPortRepointOf(4672, 4672));
	ASSERT_FALSE(FindUniqueUtpRepointCandidate(streams, ip, 4672).has_value());
}

TEST(UtpDialPolicy, AResetMustAnswerAPacketWeSent)
{
	SUtpMoveHeader reset;
	reset.type = kUtpTypeReset;
	// libutp echoes the seq_nr of the frame it could not match.
	reset.acknowledgeNumber = 500;
	ASSERT_TRUE(IsPlausibleUtpReset(reset, true, 500));
	ASSERT_TRUE(IsPlausibleUtpReset(reset, true, static_cast<uint16_t>(500 + kUtpResetAckSpan - 1)));
	ASSERT_FALSE(IsPlausibleUtpReset(reset, true, static_cast<uint16_t>(500 + kUtpResetAckSpan)));
	// Ahead of anything we sent.
	ASSERT_FALSE(IsPlausibleUtpReset(reset, true, 499));
	// Modular across the wrap.
	reset.acknowledgeNumber = 65530;
	ASSERT_TRUE(IsPlausibleUtpReset(reset, true, 10));
	// Nothing sent to that endpoint: nothing to answer.
	ASSERT_FALSE(IsPlausibleUtpReset(reset, false, 65530));
}

TEST(UtpDialPolicy, AHalfOpenStreamExpiresAtItsLimit)
{
	ASSERT_FALSE(IsUtpHalfOpenExpired(1000, 1000 + kUtpHalfOpenLimitMs - 1));
	ASSERT_TRUE(IsUtpHalfOpenExpired(1000, 1000 + kUtpHalfOpenLimitMs));
	// A clock behind the accept expires nothing.
	ASSERT_FALSE(IsUtpHalfOpenExpired(1000, 999));
}
