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

#include "UtpLibraryAdapter.h"

#include "Logger.h"             // Needed for AddDebugLogLineN
#include "UtpSocketTransport.h" // per-socket crypt parameters, resolved from userdata
#include "UtpDialPolicy.h"
#include <common/Format.h> // Needed for CFormat
#include <libutp/utp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace
{
class CUtpLibraryAdapter final : public IUtpLibrary, public IUtpSocketOperations
{
public:
	using Endpoint = std::pair<uint32_t, uint16_t>;

	~CUtpLibraryAdapter() override { Destroy(); }

	// -- IUtpSocketOperations, the four per-socket libutp calls ---------

	std::ptrdiff_t WriteToSocket(Handle socket, const uint8_t *data, size_t length) override
	{
		utp_iovec vector{ const_cast<uint8_t *>(data), length };
		// As it came: clamping utp_writev's -1 to 0 would lose the sentinel at
		// the one seam that can still tell them apart.
		return utp_writev(static_cast<utp_socket *>(socket), &vector, 1);
	}

	void NotifyReadDrained(Handle socket) override
	{
		utp_read_drained(static_cast<utp_socket *>(socket));
	}

	void CloseSocket(Handle socket) override
	{
		auto *raw = static_cast<utp_socket *>(socket);
		// Kept until DESTROYING, so the FIN's retransmits and the final ACKs stay
		// obfuscated after the transport is gone. Before utp_close(), which can
		// destroy the socket before it returns.
		const uint8_t *userHash = nullptr;
		if (const CUtpSocketTransport *transport = TransportOf(raw)) {
			if (transport->CryptParameters(&userHash)) {
				std::copy(userHash, userHash + kUserHashBytes, m_closingCrypt[raw].begin());
			}
		}
		utp_close(raw);
		// utp_close() only starts the socket dying; DESTROYING arrives later and
		// would hand a callback a freed transport.
		utp_set_userdata(raw, nullptr);
		// The endpoint stays registered until DESTROYING: a closing socket still
		// has to receive the peer's acknowledgement of that FIN, and the ingress
		// gate drops anything from an endpoint it does not know. m_endpoints is
		// what lets the removal happen without the userdata this just cleared.
	}

	void SetReceiveBuffer(Handle socket, size_t bytes) override
	{
		utp_setsockopt(static_cast<utp_socket *>(socket), UTP_RCVBUF, static_cast<int>(bytes));
	}
	bool Create(IUtpDatagramSink &sink) override
	{
		if (m_context) {
			return true;
		}
		m_context = utp_init(2);
		if (!m_context) {
			return false;
		}
		utp_context_set_userdata(m_context, &sink);
		s_self = this;
		utp_set_callback(m_context, UTP_SENDTO, SendTo);
		utp_set_callback(m_context, UTP_ON_OVERHEAD_STATISTICS, OnOverheadStatistics);
		utp_set_callback(m_context, UTP_GET_UDP_MTU, GetUdpMtu);
		utp_set_callback(m_context, UTP_GET_UDP_OVERHEAD, GetUdpOverhead);
		utp_set_callback(m_context, UTP_ON_FIREWALL, OnFirewall);
		utp_set_callback(m_context, UTP_ON_ACCEPT, OnAccept);
		utp_set_callback(m_context, UTP_ON_STATE_CHANGE, OnStateChange);
		utp_set_callback(m_context, UTP_ON_ERROR, OnError);
		utp_set_callback(m_context, UTP_ON_READ, OnRead);
		utp_set_callback(m_context, UTP_GET_READ_BUFFER_SIZE, GetReadBufferSize);
		return true;
	}

	bool Dial(uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash,
		std::unique_ptr<IStreamTransport> &transport) override
	{
		return Dial(ip, port, encrypt, userHash, SUtpDialOptions{}, transport);
	}
	bool Dial(uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash,
		const SUtpDialOptions &options,
		std::unique_ptr<IStreamTransport> &transport) override
	{
		if (!m_context || transport || ip == 0 || port == 0 || (encrypt && userHash == nullptr)) {
			return false;
		}
		auto *socket = utp_create_socket(m_context);
		if (socket == nullptr) {
			return false;
		}
		auto pending = std::make_unique<CUtpSocketTransport>(
			*this, socket, CNetworkAddress::FromIPv4NetworkOrder(ip), port, nullptr, false);
		utp_set_userdata(socket, pending.get());
		pending->ApplyReceiveBound();
		pending->SetCryptParameters(encrypt, userHash);
		// Register before the SYN, so its reply passes the ingress gate and the
		// socket-to-endpoint record removes it on every close path.
		Register(socket, ip, port);
		// Ahead of the SYN, which utp_connect() sends at once: the peer learns from it how to
		// obfuscate its SYN-ACK. A plain dial announces nothing, as that would publish our hash.
		if (encrypt) {
			auto *sink = static_cast<IUtpDatagramSink *>(utp_context_get_userdata(m_context));
			sink->SendUtpKeyFrame(ip, port, userHash);
		}
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		auto *bytes = reinterpret_cast<uint8_t *>(&address.sin_addr.s_addr);
		for (unsigned i = 0; i < 4; ++i) {
			bytes[i] = static_cast<uint8_t>(ip >> (8 * i));
		}
		if (utp_connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
			!pending->IsOk()) {
			return false;
		}
		AddRemapCandidate(socket,
			ip,
			port,
			options.nowMs,
			options.allowInitialPortRemap,
			options.clientToken,
			true);
		transport = std::move(pending);
		return true;
	}

	void SetAcceptor(IUtpStreamAcceptor *acceptor) override { m_acceptor = acceptor; }

	bool HasRegisteredPeer(uint32_t ip, uint16_t port) const override { return m_peers.Has(ip, port); }
	bool HasExpectedPortRemapPeer(uint32_t ip, uint16_t port, std::uint64_t nowMs) const override
	{
		return FindUniqueUtpPortRemapCandidate(m_remapCandidates, ip, port, nowMs).has_value();
	}
	//! Answers the ingress gate before libutp sees the frame, so it stays read-only: the attempt
	//! itself, and its budget, belong to ProcessDatagram().
	bool HasRepointCandidate(uint32_t ip, uint16_t port) const override
	{
		if (m_socketsAtEndpoint.count({ ip, port }) != 0) {
			return false;
		}
		return FindRepointCandidate(ip, port).has_value();
	}
	void Destroy() override
	{
		if (!m_context) {
			return;
		}
		// Not closed here: struct_utp_context owns UTPSocketHT, whose map holds
		// each socket in a unique_ptr with a deleter, so `delete ctx` destroys
		// them and ~UTPSocket emits DESTROYING (utp_internal.cpp:2499) -- the
		// callback each transport needs. Closing first would utp_close() a
		// dying socket, which it asserts against.
		m_refusedStreams.clear();
		for (utp_socket *refused : m_refused) {
			utp_close(refused);
		}
		m_refused.clear();
		utp_destroy(m_context);
		// Every live socket emitted DESTROYING above; anything left is a socket
		// libutp never told us about, and its endpoint is dead either way.
		m_endpoints.clear();
		m_remapCandidates.clear();
		m_remapInputs.clear();
		m_remapOutputs.clear();
		m_socketRemaps.clear();
		m_peerStreams.clear();
		m_socketsAtEndpoint.clear();
		m_sentTo.clear();
		m_acceptedAtMs.clear();
		m_context = nullptr;
		m_acceptor = nullptr;
		if (s_self == this) {
			s_self = nullptr;
		}
	}
	bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) override
	{
		return ProcessDatagram(payload, length, ip, port, 0);
	}
	bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs) override
	{
		return ProcessDatagram(payload, length, ip, port, nowMs, false);
	}
	bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs,
		bool obfuscated) override
	{
		return ProcessDatagram(payload, length, ip, port, nowMs, obfuscated, nullptr);
	}
	bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs,
		bool obfuscated,
		const uint8_t *preHelloKey) override
	{
		if (length > kMaxUtpFrameBytes) {
			return false;
		}
		m_nowMs = nowMs;
		// OnAccept() is a bare libutp callback with no datagram argument, so the origin of the SYN
		// it is about to admit travels here for exactly the span of this call.
		m_datagramObfuscated = obfuscated;
		// libutp sends the SYN-ACK before UTP_ON_ACCEPT and with no socket, so the key travels the
		// same way, and only to the endpoint this datagram came from.
		m_ingressKey = preHelloKey;
		m_ingress = { ip, port };
		const Endpoint observed{ ip, port };
		const std::optional<SUtpMoveHeader> header = ReadUtpWireHeader(payload, length);
		Endpoint expected = observed;
		utp_socket *remappedSocket = nullptr;
		// A non-SYN frame that a handshake or a connected stream could own but that does not prove
		// itself is dropped rather than handed over: libutp answers an unmatched non-SYN with an RST
		// to that source (:2925-2940), which is the reflection the ingress gate exists to prevent.
		bool unproven = false;
		// A frame from an endpoint that already owns a socket is that socket's. Offering it to a
		// handshake or a stream on a nearby port would let one forged SYN from that port claim it.
		const bool fromRegisteredEndpoint = m_socketsAtEndpoint.count(observed) != 0;
		if (header.has_value() && header->type == kUtpTypeReset) {
			const auto sent = m_sentTo.find(observed);
			unproven = sent == m_sentTo.end() ||
				   !IsPlausibleUtpReset(
					   *header, sent->second.hasSent, sent->second.lastSequenceNumber);
		}
		const auto translation = m_remapInputs.find(observed);
		if (translation != m_remapInputs.end()) {
			expected = translation->second;
		} else if (!fromRegisteredEndpoint && !unproven) {
			const auto candidateIndex =
				FindUniqueUtpPortRemapCandidate(m_remapCandidates, ip, port, nowMs);
			if (candidateIndex.has_value()) {
				const SPendingHandshake &candidate = m_remapCandidates[*candidateIndex];
				const Endpoint candidateExpected{ candidate.ip, candidate.port };
				// The reply must acknowledge the SYN this candidate sent: utp_process_udp()
				// answers 1 for every non-SYN frame it is handed, claimed or not
				// (utp_internal.cpp:2909), so its return value is no evidence that this frame
				// is our SYN-ACK. A second socket must also never overwrite a live
				// translation, so ambiguity fails closed.
				if (AcknowledgesOurSyn(candidate, header) &&
					CanInstallPortRemap(candidate.socket, candidateExpected, observed)) {
					expected = candidateExpected;
					remappedSocket = candidate.socket;
					// libutp keys the socket by its original tuple. Install a provisional
					// destination-keyed translation so its immediate response reaches the
					// observed NAT mapping even though UTP_SENDTO has no socket argument.
					InstallPortRemap(remappedSocket, expected, observed);
				} else if (ClassifyUtpFrame(payload, length) == EUtpFrameKind::Existing) {
					// The ingress gate admitted this because a handshake expected that
					// port, and it does not acknowledge our SYN. There is no connected
					// stream to fall through to, so libutp would answer it with an RST:
					// drop it here instead.
					unproven = true;
				}
			}
		}
		// A frame from a port nothing holds may belong to the one connected stream on that address
		// after its NAT moved the port. libutp cannot re-key a socket, and it answers 1 to every
		// non-SYN frame it is given -- processed, stale or unmatched alike (:2909, :2925, :2933,
		// :2940) -- so the header decides here, before the frame is handed over. One attempt, one
		// candidate, or none at all; a SYN and a reset are refused by the decision itself. A frame
		// the handshake branch has already refused is never offered to a stream as well.
		utp_socket *repointedSocket = nullptr;
		Endpoint repointedFrom{ 0, 0 };
		std::optional<std::pair<Endpoint, Endpoint>> repointedPrevious;
		if (!unproven && !fromRegisteredEndpoint && translation == m_remapInputs.end() &&
			remappedSocket == nullptr) {
			const std::optional<SRepointCandidate> candidate = FindRepointCandidate(ip, port);
			if (candidate.has_value()) {
				// A move never goes back to the port this stream has just left, for as long
				// as the move limit itself lasts.
				const bool abandoned = IsUtpAbandonedPort(
					port, candidate->abandonedPort, candidate->abandonedAtMs, nowMs);
				if (!abandoned && header.has_value() &&
					DecideUtpMove(*header, MoveFactsFor(candidate->socket)) ==
						EUtpMoveVerdict::Move &&
					m_repointLimiter.TryAdmit(StreamKey(candidate->socket), nowMs)) {
					// Installed before the hand-over, as the handshake path does: libutp
					// can send while it handles this frame, and those frames must reach
					// the new port. A move libutp refuses gets the previous pair back.
					repointedSocket = candidate->socket;
					repointedFrom = candidate->libutpEndpoint;
					repointedPrevious = PreviousRemapOf(candidate->socket);
					RemovePortRemap(candidate->socket);
					InstallPortRemap(candidate->socket, repointedFrom, observed);
					expected = repointedFrom;
				} else if (ClassifyUtpFrame(payload, length) == EUtpFrameKind::Existing) {
					// One connected stream could own this frame and it did not prove
					// itself. Handing it over anyway would make libutp answer an
					// unmatched non-SYN with an RST to that source (:2925-2940), which is
					// the reflection the ingress gate exists to prevent, so it is dropped
					// instead. A SYN is not one of these: it asks for a new connection
					// and keeps normal admission.
					unproven = true;
				}
			}
		}
		const sockaddr_in address = MakeAddress(expected.first, expected.second);
		// A frame that a candidate could own but that did not prove itself stops here.
		bool claimed = false;
		if (!unproven) {
			claimed = utp_process_udp(m_context,
					  payload,
					  length,
					  reinterpret_cast<const sockaddr *>(&address),
					  sizeof(address)) != 0;
		}
		// Both translations stay for the life of the socket: libutp keeps the tuple it was created
		// with, so these maps are what hold the stream on the observed port in both directions. Only
		// a refused claim gives one up, and Forget() drops them with the socket.
		if (remappedSocket != nullptr) {
			if (claimed) {
				UpdatePeerEndpoint(remappedSocket, ip, port);
			} else {
				RemovePortRemap(remappedSocket);
			}
		}
		if (repointedSocket != nullptr) {
			if (claimed) {
				// The pair is already the new one: remember the port being left, so a later
				// frame cannot bring the stream back to it while that refusal lasts.
				if (const auto here = m_endpoints.find(repointedSocket);
					here != m_endpoints.end()) {
					SPeerStreamState &state = m_peerStreams[repointedSocket];
					state.abandonedPort = here->second.second;
					state.abandonedAtMs = nowMs;
				}
				UpdatePeerEndpoint(repointedSocket, ip, port);
				// Formatted here rather than through NetworkFunctions.h, whose Types.h
				// typedefs clash with libutp's own in this translation unit.
				AddDebugLogLineN(logClient,
					CFormat("uTP: moved the stream to %u.%u.%u.%u from port %u to port "
						"%u") %
						(uint8)ip % (uint8)(ip >> 8) % (uint8)(ip >> 16) %
						(uint8)(ip >> 24) % repointedFrom.second % port);
				static_cast<IUtpDatagramSink *>(utp_context_get_userdata(m_context))
					->OnUtpStreamMoved();
			} else {
				// libutp refused the frame after all: put back whatever pair the socket held
				// before the attempt, so the stream is never left without a translation.
				RemovePortRemap(repointedSocket);
				if (repointedPrevious.has_value()) {
					InstallPortRemap(repointedSocket,
						repointedPrevious->first,
						repointedPrevious->second);
				}
			}
		}
		if (claimed) {
			NoteInboundHeader(remappedSocket != nullptr ? remappedSocket : repointedSocket,
				observed,
				header);
		}
		m_datagramObfuscated = false;
		m_ingressKey = nullptr;
		// Not inside UTP_ON_ACCEPT: utp_close() can produce DESTROYING before
		// it returns, re-entering the callback that is still deciding.
		// Destruction is the close, exactly once.
		m_refusedStreams.clear();
		for (utp_socket *refused : m_refused) {
			utp_close(refused);
		}
		m_refused.clear();
		return claimed;
	}
	bool ApplyAnnouncedKey(uint32_t ip, uint16_t port, const uint8_t *userHash) override
	{
		bool applied = false;
		for (const auto &entry : m_endpoints) {
			if (entry.second != Endpoint{ ip, port }) {
				continue;
			}
			CUtpSocketTransport *transport = TransportOf(entry.first);
			const uint8_t *current = nullptr;
			// Only a dial still in its handshake: once connected, the hello is about to decide.
			if (transport == nullptr || transport->IsInbound() || transport->IsConnected() ||
				transport->CryptParameters(&current)) {
				continue;
			}
			transport->SetPreHelloKey(userHash);
			applied = true;
		}
		return applied;
	}
	void IssueDeferredAcks() override { utp_issue_deferred_acks(m_context); }
	void CheckTimeouts() override
	{
		utp_check_timeouts(m_context);
		ProbeIdleStreams();
	}

private:
	struct SPendingHandshake : SUtpRemapCandidate
	{
		utp_socket *socket = nullptr;
	};

	//! What we last put on the wire to one destination. hasSyn/synSequenceNumber hold the SYN we sent
	//! there, which the reply has to acknowledge.
	struct SSentToEndpoint
	{
		bool hasSent = false;
		std::uint16_t lastSequenceNumber = 0;
		bool hasSyn = false;
		std::uint16_t synSequenceNumber = 0;
		//! Set when we sent ST_DATA there, which is what marks this side as the uploader for the
		//! probe.
		bool sentData = false;
		std::uint64_t lastDataMs = 0;
	};

	//! A connected stream, with what the repoint needs beyond the shared facts.
	struct SRepointCandidate : SUtpConnectedStream
	{
		utp_socket *socket = nullptr;
		//! The tuple libutp keys this socket by, which is the observed one only until it moves.
		Endpoint libutpEndpoint;
		//! The port this stream moved away from, and when, so a move never returns to it while that
		//! refusal lasts.
		std::uint16_t abandonedPort = 0;
		std::uint64_t abandonedAtMs = 0;
	};

	//! What the adapter knows about one stream: the peer facts a move is decided on, plus where the
	//! stream came from.
	struct SPeerStreamState : SUtpPeerStreamFacts
	{
		std::uint16_t abandonedPort = 0;
		std::uint64_t abandonedAtMs = 0;
		//! Whether this stream has received ST_DATA, when, and when it was last probed.
		bool receivedData = false;
		std::uint64_t lastDataReceivedMs = 0;
		bool probed = false;
		std::uint64_t lastProbeMs = 0;
	};

	static sockaddr_in MakeAddress(uint32_t ip, uint16_t port)
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

	//! MakeAddress() in reverse.
	static uint32_t IpOf(const sockaddr_in &address)
	{
		const auto *bytes = reinterpret_cast<const uint8_t *>(&address.sin_addr.s_addr);
		uint32_t ip = 0;
		for (unsigned i = 0; i < 4; ++i) {
			ip |= static_cast<uint32_t>(bytes[i]) << (8 * i);
		}
		return ip;
	}

	void AddRemapCandidate(utp_socket *socket,
		uint32_t ip,
		uint16_t port,
		std::uint64_t registeredAtMs,
		bool remapAllowed,
		uint32_t clientToken,
		bool outbound)
	{
		RemoveRemapCandidate(socket);
		SPendingHandshake candidate;
		candidate.ip = ip;
		candidate.port = port;
		candidate.registeredAtMs = registeredAtMs;
		candidate.handshakePending = true;
		candidate.remapAllowed = remapAllowed;
		candidate.outbound = outbound;
		candidate.clientToken = clientToken;
		candidate.socket = socket;
		m_remapCandidates.push_back(candidate);
	}

	void RemoveRemapCandidate(utp_socket *socket)
	{
		m_remapCandidates.erase(std::remove_if(m_remapCandidates.begin(),
						m_remapCandidates.end(),
						[socket](const SPendingHandshake &candidate) {
							return candidate.socket == socket;
						}),
			m_remapCandidates.end());
	}

	bool CanInstallPortRemap(utp_socket *socket, const Endpoint &expected, const Endpoint &observed) const
	{
		const auto input = m_remapInputs.find(observed);
		const auto output = m_remapOutputs.find(expected);
		return CanInstallUtpPortTranslation(
			       input != m_remapInputs.end(), output != m_remapOutputs.end()) &&
		       m_socketRemaps.find(socket) == m_socketRemaps.end();
	}

	//! The pair a socket already holds, so a move that libutp refuses can put it back.
	std::optional<std::pair<Endpoint, Endpoint>> PreviousRemapOf(utp_socket *socket) const
	{
		const auto found = m_socketRemaps.find(socket);
		if (found == m_socketRemaps.end()) {
			return std::nullopt;
		}
		return found->second;
	}

	//! The tuple libutp keys a socket by. libutp has no way to re-key a live socket, which is why a
	//! moved stream keeps both translations: the outgoing one only knows this tuple.
	Endpoint LibutpEndpointOf(utp_socket *socket) const
	{
		const auto remap = m_socketRemaps.find(socket);
		if (remap != m_socketRemaps.end()) {
			return remap->second.first;
		}
		const auto endpoint = m_endpoints.find(socket);
		return endpoint == m_endpoints.end() ? Endpoint{ 0, 0 } : endpoint->second;
	}

	//! Every stream this adapter believes is connected at this address, with the tuple libutp holds
	//! for it. The repoint window applies to the peer port we believe, not to that tuple, since the
	//! belief is what a repoint moves. Accepted streams are candidates too: the header has to prove
	//! itself before anything moves, so a stranger cannot take one over by guessing its port.
	std::vector<SRepointCandidate> ConnectedStreamsAt(uint32_t ip) const
	{
		std::vector<SRepointCandidate> streams;
		for (const auto &entry : m_endpoints) {
			const CUtpSocketTransport *transport = TransportOf(entry.first);
			if (entry.second.first != ip || transport == nullptr || !transport->IsConnected()) {
				continue;
			}
			// A stream we have seen no frame of has no facts a move could be checked against, so
			// it must not make another stream on the address ambiguous either.
			const auto state = m_peerStreams.find(entry.first);
			if (state == m_peerStreams.end() || !state->second.known) {
				continue;
			}
			SRepointCandidate candidate;
			candidate.ip = entry.second.first;
			candidate.port = entry.second.second;
			candidate.connected = true;
			candidate.socket = entry.first;
			candidate.libutpEndpoint = LibutpEndpointOf(entry.first);
			candidate.abandonedPort = state->second.abandonedPort;
			candidate.abandonedAtMs = state->second.abandonedAtMs;
			streams.push_back(candidate);
		}
		return streams;
	}

	//! Identifies a stream to the repoint limiter. A reused address inherits a spent budget, which
	//! only ever refuses a move.
	static std::uint64_t StreamKey(utp_socket *socket)
	{
		return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(socket));
	}

	//! A receiver whose data has stopped has nothing to acknowledge, so after a port change the peer
	//! hears nothing and gives up long before the next keepalive. Growing the receive buffer raises the
	//! window, which makes utp_read_drained() schedule an ack (utp_internal.cpp:3232-3239); flushing it
	//! sends that ack through whatever mapping the stream now holds, which is what tells the peer where
	//! the stream went. Reading, writing the option, draining and flushing are all public libutp API.
	void SendIdleProbe(utp_socket *socket)
	{
		const int window = utp_getsockopt(socket, UTP_RCVBUF);
		utp_setsockopt(socket, UTP_RCVBUF, window + 1);
		utp_read_drained(socket);
		utp_issue_deferred_acks(m_context);
	}

	//! Pokes every connected stream the probe rule names. Runs from CheckTimeouts(), so a silent
	//! downloader emits an ack at most once per idle window without any datagram having to arrive.
	void ProbeIdleStreams()
	{
		const std::uint64_t nowMs = NowMs();
		std::vector<utp_socket *> idle;
		for (const auto &entry : m_endpoints) {
			const CUtpSocketTransport *transport = TransportOf(entry.first);
			if (transport == nullptr || !transport->IsConnected()) {
				continue;
			}
			SUtpProbeFacts facts;
			const auto state = m_peerStreams.find(entry.first);
			if (state != m_peerStreams.end()) {
				facts.receivedData = state->second.receivedData;
				facts.lastDataReceivedMs = state->second.lastDataReceivedMs;
				facts.probed = state->second.probed;
				facts.lastProbeMs = state->second.lastProbeMs;
			}
			const auto sent = m_sentTo.find(entry.second);
			if (sent != m_sentTo.end()) {
				facts.sentData = sent->second.sentData;
				facts.lastDataSentMs = sent->second.lastDataMs;
			}
			if (ShouldProbeIdleStream(facts, nowMs)) {
				idle.push_back(entry.first);
			}
		}
		for (utp_socket *socket : idle) {
			SendIdleProbe(socket);
			SPeerStreamState &state = m_peerStreams[socket];
			state.probed = true;
			state.lastProbeMs = nowMs;
		}
	}

	//! Monotonic milliseconds for the probe's own bookkeeping. The probe is driven by CheckTimeouts(),
	//! which carries no time, so it keeps one clock domain of its own rather than mixing the caller's
	//! injected one into it.
	static std::uint64_t NowMs()
	{
		using Clock = std::chrono::steady_clock;
		const auto sinceEpoch = Clock::now().time_since_epoch();
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count());
	}

	//! Remembers what we last put on the wire to one destination. UTP_SENDTO carries no socket
	//! (utp_call_sendto(ctx, NULL, ...), utp_internal.cpp:709), so the destination is the only key
	//! available; a move requires exactly one connected stream on that address, which makes it
	//! unambiguous.
	void NoteSentHeader(uint32_t ip, uint16_t port, const uint8_t *payload, size_t length)
	{
		const std::optional<SUtpMoveHeader> header = ReadUtpWireHeader(payload, length);
		if (!header.has_value() || header->type == kUtpTypeReset) {
			return;
		}
		const Endpoint destination{ ip, port };
		if (m_sentTo.find(destination) == m_sentTo.end() && m_sentTo.size() >= kMaxSentEndpoints) {
			// Forgetting a destination only ever refuses a move, so the oldest-by-address goes.
			m_sentTo.erase(m_sentTo.begin());
		}
		SSentToEndpoint &sent = m_sentTo[destination];
		// Only forwards: a resend carries an older number, and letting it lower the mark would refuse
		// the ack that actually arrives for the newer frame.
		if (!sent.hasSent || IsUtpSeqAhead(header->sequenceNumber, sent.lastSequenceNumber)) {
			sent.lastSequenceNumber = header->sequenceNumber;
		}
		sent.hasSent = true;
		if (header->type == kUtpTypeData) {
			sent.sentData = true;
			sent.lastDataMs = NowMs();
		}
		if (header->type == kUtpTypeSyn) {
			sent.hasSyn = true;
			sent.synSequenceNumber = header->sequenceNumber;
		}
	}

	//! The socket a frame from this endpoint belongs to, when only one registered stream could own it
	//! or a recorded connection id picks one out. Ambiguity yields none, so nothing is learned.
	utp_socket *ResolveInboundSocket(const Endpoint &source, std::uint32_t connectionId) const
	{
		const auto at = m_socketsAtEndpoint.find(source);
		if (at == m_socketsAtEndpoint.end()) {
			return nullptr;
		}
		const std::vector<utp_socket *> &sockets = at->second;
		if (sockets.size() == 1) {
			return sockets.front();
		}
		// Several streams can share an endpoint; only a recorded connection id tells them apart.
		for (utp_socket *socket : sockets) {
			const auto state = m_peerStreams.find(socket);
			if (state != m_peerStreams.end() && state->second.known &&
				state->second.connectionId == connectionId) {
				return socket;
			}
		}
		return nullptr;
	}

	//! Records what a peer's own frames say about it: its connection id, and the sequence numbers it
	//! last used and acknowledged. libutp exposes none of this, and a move has to be decided on it.
	void NoteInboundHeader(
		utp_socket *socket, const Endpoint &source, const std::optional<SUtpMoveHeader> &header)
	{
		// A SYN belongs to a new connection and a reset to none: neither describes this stream.
		if (!header.has_value() || header->type == kUtpTypeSyn || header->type == kUtpTypeReset) {
			return;
		}
		if (socket == nullptr) {
			socket = ResolveInboundSocket(source, header->connectionId);
		}
		if (socket == nullptr) {
			return;
		}
		SPeerStreamState &state = m_peerStreams[socket];
		if (state.known && state.connectionId != header->connectionId) {
			// Not this stream's peer, so it teaches us nothing about it.
			return;
		}
		state.known = true;
		state.connectionId = header->connectionId;
		state.peerSequenceNumber = header->sequenceNumber;
		state.peerAcknowledgeNumber = header->acknowledgeNumber;
		if (header->type == kUtpTypeData) {
			// Only data tells the probe this stream is a live download whose peer went quiet.
			state.receivedData = true;
			state.lastDataReceivedMs = NowMs();
		}
	}

	//! What the move decision may rely on for this socket, complete or not at all: a missing send
	//! record or a stream we never saw a frame on both refuse the move.
	SUtpPeerStreamFacts MoveFactsFor(utp_socket *socket) const
	{
		SUtpPeerStreamFacts facts;
		const auto state = m_peerStreams.find(socket);
		const auto endpoint = m_endpoints.find(socket);
		if (state == m_peerStreams.end() || endpoint == m_endpoints.end() || !state->second.known) {
			return facts;
		}
		const auto sent = m_sentTo.find(endpoint->second);
		if (sent == m_sentTo.end() || !sent->second.hasSent) {
			return facts;
		}
		facts.known = true;
		facts.connectionId = state->second.connectionId;
		facts.peerSequenceNumber = state->second.peerSequenceNumber;
		facts.peerAcknowledgeNumber = state->second.peerAcknowledgeNumber;
		facts.lastSentSequenceNumber = sent->second.lastSequenceNumber;
		return facts;
	}

	//! Whether this frame acknowledges the SYN this candidate sent. The acceptor copies the SYN's
	//! seq_nr into its ack_nr (utp_internal.cpp:2982) and sends that back (send_ack, :777), so an
	//! exact match is the evidence, not libutp's return value.
	bool AcknowledgesOurSyn(
		const SPendingHandshake &candidate, const std::optional<SUtpMoveHeader> &header) const
	{
		if (!header.has_value() || header->type == kUtpTypeSyn || header->type == kUtpTypeReset) {
			return false;
		}
		const auto sent = m_sentTo.find(Endpoint{ candidate.ip, candidate.port });
		return sent != m_sentTo.end() && sent->second.hasSyn &&
		       header->acknowledgeNumber == sent->second.synSequenceNumber;
	}

	//! The one connected stream a frame from this source port could belong to, or none when two
	//! could. Pure: the ingress gate asks it, and only ProcessDatagram() acts on the answer.
	std::optional<SRepointCandidate> FindRepointCandidate(uint32_t ip, uint16_t port) const
	{
		const std::vector<SRepointCandidate> streams = ConnectedStreamsAt(ip);
		const std::optional<std::size_t> index = FindUniqueUtpRepointCandidate(streams, ip, port);
		if (!index.has_value()) {
			return std::nullopt;
		}
		return streams[*index];
	}

	void InstallPortRemap(utp_socket *socket, const Endpoint &expected, const Endpoint &observed)
	{
		m_remapInputs.emplace(observed, expected);
		m_remapOutputs.emplace(expected, observed);
		m_socketRemaps.emplace(socket, std::make_pair(expected, observed));
	}

	void RemovePortRemap(utp_socket *socket)
	{
		const auto found = m_socketRemaps.find(socket);
		if (found == m_socketRemaps.end()) {
			return;
		}
		const auto input = m_remapInputs.find(found->second.second);
		if (input != m_remapInputs.end() && input->second == found->second.first) {
			m_remapInputs.erase(input);
		}
		const auto output = m_remapOutputs.find(found->second.first);
		if (output != m_remapOutputs.end() && output->second == found->second.second) {
			m_remapOutputs.erase(output);
		}
		m_socketRemaps.erase(found);
	}

	void UpdatePeerEndpoint(utp_socket *socket, uint32_t ip, uint16_t port)
	{
		const auto found = m_endpoints.find(socket);
		if (found == m_endpoints.end() ||
			(found->second.first == ip && found->second.second == port)) {
			return;
		}
		UnindexSocket(socket, found->second);
		m_peers.Remove(found->second.first, found->second.second);
		m_peers.Add(ip, port);
		found->second = { ip, port };
		IndexSocket(socket, found->second);
		if (CUtpSocketTransport *transport = TransportOf(socket)) {
			transport->UpdatePeerEndpoint(CNetworkAddress::FromIPv4NetworkOrder(ip), port);
		}
	}

	static uint64 SendTo(utp_callback_arguments *args)
	{
		if (!args->address || args->address->sa_family != AF_INET ||
			args->address_len < sizeof(sockaddr_in)) {
			return 0;
		}
		const auto *address = reinterpret_cast<const sockaddr_in *>(args->address);
		uint32_t ip = IpOf(*address);
		uint16_t port = ntohs(address->sin_port);
		if (s_self != nullptr) {
			const auto translation = s_self->m_remapOutputs.find({ ip, port });
			if (translation != s_self->m_remapOutputs.end()) {
				ip = translation->second.first;
				port = translation->second.second;
			}
		}
		auto *sink = static_cast<IUtpDatagramSink *>(utp_context_get_userdata(args->context));
		// From the socket, never the destination: an address can belong to
		// several clients. A context-level send has no verified peer.
		bool encrypt = false;
		const uint8_t *userHash = nullptr;
		utp_socket *socket = args->socket;
		if (socket == nullptr && s_self != nullptr) {
			socket = std::exchange(s_self->m_sending, nullptr);
			if (socket != nullptr && !IsPeerOf(socket, args->address, args->address_len)) {
				socket = nullptr;
			}
		}
		if (socket != nullptr) {
			const auto *transport =
				static_cast<const CUtpSocketTransport *>(utp_get_userdata(socket));
			if (transport != nullptr) {
				encrypt = transport->CryptParameters(&userHash);
			} else if (s_self != nullptr) {
				const auto closing = s_self->m_closingCrypt.find(socket);
				if (closing != s_self->m_closingCrypt.end()) {
					encrypt = true;
					userHash = closing->second.data();
				}
			}
		}
		// The SYN-ACK leaves before UTP_ON_ACCEPT, from a socket with no transport yet.
		if (!encrypt && s_self != nullptr && s_self->m_ingressKey != nullptr &&
			s_self->m_ingress == Endpoint{ ip, port }) {
			encrypt = true;
			userHash = s_self->m_ingressKey;
		}
		if (s_self != nullptr) {
			// After the translation, so the record follows where the packet actually went.
			s_self->NoteSentHeader(ip, port, args->buf, args->len);
		}
		sink->SendUtpDatagram(args->buf, args->len, ip, port, encrypt, userHash);
		return 0;
	}
	// libutp passes UTP_SENDTO no socket, but reports each socket's send here
	// immediately before it.
	static uint64 OnOverheadStatistics(utp_callback_arguments *args)
	{
		if (s_self != nullptr && args->send) {
			s_self->m_sending = args->socket;
		}
		return 0;
	}
	// The SYN-ACK is also reported with no send after it; a socket-less reset
	// that follows must not inherit it.
	static bool IsPeerOf(utp_socket *socket, const sockaddr *address, socklen_t length)
	{
		sockaddr_storage peer{};
		socklen_t peerLength = sizeof(peer);
		return utp_getpeername(socket, reinterpret_cast<sockaddr *>(&peer), &peerLength) == 0 &&
		       peerLength == length && std::memcmp(&peer, address, length) == 0;
	}
	//! The transport that owns a socket, or null for one we never accepted.
	static CUtpSocketTransport *TransportOf(utp_socket *socket)
	{
		if (socket == nullptr) {
			return nullptr;
		}
		return static_cast<CUtpSocketTransport *>(utp_get_userdata(socket));
	}

	/**
	 * Inbound streams whose peer has not sent data yet: all a forged source can hold open. One that
	 * waited kUtpHalfOpenLimitMs is ended instead of counted. Only a SYN reads the budget, so the
	 * SYN's own clock is the one that matters.
	 */
	std::size_t ExpireAndCountHalfOpen(std::uint64_t nowMs)
	{
		std::size_t count = 0;
		for (auto it = m_acceptedAtMs.begin(); it != m_acceptedAtMs.end();) {
			CUtpSocketTransport *transport = TransportOf(it->first);
			if (transport == nullptr || !transport->IsHalfOpen()) {
				it = m_acceptedAtMs.erase(it);
				continue;
			}
			if (IsUtpHalfOpenExpired(it->second, nowMs)) {
				// Reported lost, so its socket closes it from the main thread.
				transport->OnEnded(EUtpTransportFailure::TimedOut);
				it = m_acceptedAtMs.erase(it);
				continue;
			}
			++count;
			++it;
		}
		return count;
	}

	//! libutp's question before it creates a socket for a SYN or sends the SYN-ACK: nonzero refuses.
	static uint64 OnFirewall(utp_callback_arguments *args)
	{
		if (s_self == nullptr || s_self->m_acceptor == nullptr || args->address == nullptr ||
			args->address->sa_family != AF_INET) {
			return 1;
		}
		const auto *address = reinterpret_cast<const sockaddr_in *>(args->address);
		return s_self->m_acceptor->AdmitsSyn(IpOf(*address),
			       ntohs(address->sin_port),
			       s_self->ExpireAndCountHalfOpen(s_self->m_nowMs))
			       ? 0
			       : 1;
	}

	static uint64 OnAccept(utp_callback_arguments *args)
	{
		if (s_self == nullptr || s_self->m_acceptor == nullptr || args->address == nullptr ||
			args->address->sa_family != AF_INET) {
			// No acceptor refuses every SYN, as this build did before one.
			if (s_self != nullptr && args->socket != nullptr) {
				s_self->m_refused.push_back(args->socket);
			}
			return 0;
		}
		const auto *address = reinterpret_cast<const sockaddr_in *>(args->address);
		const uint32_t ip = IpOf(*address);
		const uint16_t port = ntohs(address->sin_port);
		const SUtpSynCandidateMatch candidateMatch =
			FindUniqueUtpSynCandidate(s_self->m_remapCandidates, ip, port, s_self->m_nowMs);
		SUtpSynCandidateInfo candidateInfo;
		candidateInfo.status = candidateMatch.status;
		if (candidateMatch.status == EUtpSynCandidateStatus::Unique) {
			const SPendingHandshake &candidate = s_self->m_remapCandidates[candidateMatch.index];
			candidateInfo.ip = candidate.ip;
			candidateInfo.port = candidate.port;
			candidateInfo.clientToken = candidate.clientToken;
		}
		const SUtpIncomingSynDecision decision =
			s_self->m_acceptor->DecideIncomingSyn(ip, port, candidateInfo);
		if (decision.disposition == EUtpIncomingSynDisposition::RejectKeepOutgoing) {
			// Close only after utp_process_udp returns; utp_close re-enters callbacks.
			s_self->m_refused.push_back(args->socket);
			return 0;
		}

		// Interface type so the seam can move out of it; the concrete pointer
		// stays for the setup below.
		auto owned = std::make_unique<CUtpSocketTransport>(*s_self,
			args->socket,
			CNetworkAddress::FromIPv4NetworkOrder(ip),
			port,
			nullptr,
			true);
		CUtpSocketTransport *raw = owned.get();
		std::unique_ptr<IStreamTransport> transport(std::move(owned));
		// Before admission can produce a callback that needs it.
		utp_set_userdata(args->socket, raw);
		// Until this runs, libutp's 1 MiB default is the receive bound.
		raw->ApplyReceiveBound();
		// Not connected yet: a SYN proves no address. The peer's first data marks it.
		raw->MarkSynObfuscated(s_self->m_datagramObfuscated);
		if (s_self->m_ingressKey != nullptr) {
			raw->SetPreHelloKey(s_self->m_ingressKey);
		}

		// Registered before admission so that this pairs with the removal in
		// CloseSocket(), which runs for every socket that has a transport --
		// including a refused one, whose destructor closes it. Registering only
		// the admitted ones would make a refused stream decrement the count of a
		// live stream from the same endpoint, since a peer uses one UDP port, and
		// drop that live stream off the gate its own frames pass through.
		// Briefly registering a refused endpoint costs nothing: libutp still holds
		// the socket until the close below.
		s_self->Register(args->socket, ip, port);
		if (!s_self->m_acceptor->AcceptStream(transport, ip, port, decision)) {
			// Forgotten at once, unlike an admitted stream: a refused peer has no
			// FIN to acknowledge, and leaving its endpoint on the gate until the
			// socket is destroyed would let its next frames reach libutp.
			s_self->Forget(args->socket);
			// Destroying it closes the socket, which must happen after libutp
			// has finished with the datagram.
			s_self->m_refusedStreams.push_back(std::move(transport));
			return 0;
		}
		s_self->m_acceptedAtMs[args->socket] = s_self->m_nowMs;
		return 0;
	}

	static uint64 OnStateChange(utp_callback_arguments *args)
	{
		if (args->state == UTP_STATE_DESTROYING && s_self != nullptr) {
			// Before the transport check: a closed socket has no userdata left,
			// and this is the only notice that its endpoint is now dead.
			s_self->Forget(args->socket);
		}
		if (args->state == UTP_STATE_CONNECT && s_self != nullptr) {
			s_self->RemoveRemapCandidate(args->socket);
		}
		CUtpSocketTransport *transport = TransportOf(args->socket);
		if (transport == nullptr) {
			return 0;
		}
		switch (args->state) {
		case UTP_STATE_WRITABLE:
			transport->OnWritable();
			break;
		case UTP_STATE_EOF:
			transport->OnEnded(EUtpTransportFailure::Eof);
			break;
		case UTP_STATE_DESTROYING:
			// Forgotten before the transport is told, and the handle dies with
			// this callback.
			utp_set_userdata(args->socket, nullptr);
			transport->OnEnded(EUtpTransportFailure::Destroying);
			break;
		case UTP_STATE_CONNECT:
			// Only a dial gets this. An accepted stream is connected by its peer's
			// first data, in CUtpSocketTransport::OnPayload().
			transport->MarkConnected();
			break;
		default:
			break;
		}
		return 0;
	}

	static uint64 OnError(utp_callback_arguments *args)
	{
		CUtpSocketTransport *transport = TransportOf(args->socket);
		if (transport == nullptr) {
			return 0;
		}
		switch (args->error_code) {
		case UTP_ECONNREFUSED:
			if (s_self != nullptr) {
				s_self->RemoveRemapCandidate(args->socket);
			}
			transport->OnEnded(EUtpTransportFailure::Refused);
			break;
		case UTP_ETIMEDOUT:
			if (s_self != nullptr) {
				s_self->RemoveRemapCandidate(args->socket);
			}
			transport->OnEnded(EUtpTransportFailure::TimedOut);
			break;
		case UTP_ECONNRESET:
		default:
			if (s_self != nullptr) {
				s_self->RemoveRemapCandidate(args->socket);
			}
			transport->OnEnded(EUtpTransportFailure::Reset);
			break;
		}
		return 0;
	}

	static uint64 OnRead(utp_callback_arguments *args)
	{
		if (s_self != nullptr) {
			s_self->RemoveRemapCandidate(args->socket);
		}
		CUtpSocketTransport *transport = TransportOf(args->socket);
		if (transport != nullptr) {
			transport->OnPayload(args->buf, args->len);
		}
		return 0;
	}

	static uint64 GetReadBufferSize(utp_callback_arguments *args)
	{
		// Occupancy, not free space: libutp advertises opt_rcvbuf minus this,
		// so the free figure runs the feedback backwards, silently.
		const CUtpSocketTransport *transport = TransportOf(args->socket);
		return transport == nullptr ? 0 : transport->ReadBufferSize();
	}

	static uint64 GetUdpMtu(utp_callback_arguments *args)
	{
		return UtpUdpMtu(args->address->sa_family == AF_INET6);
	}
	static uint64 GetUdpOverhead(utp_callback_arguments *args)
	{
		return UtpUdpOverhead(args->address->sa_family == AF_INET6);
	}
	void Register(utp_socket *socket, uint32_t ip, uint16_t port)
	{
		m_peers.Add(ip, port);
		m_endpoints[socket] = { ip, port };
		IndexSocket(socket, { ip, port });
	}

	//! Keeps the endpoint index in step with m_endpoints.
	void IndexSocket(utp_socket *socket, const Endpoint &endpoint)
	{
		m_socketsAtEndpoint[endpoint].push_back(socket);
	}

	//! An endpoint whose last socket has left is forgotten with it.
	void UnindexSocket(utp_socket *socket, const Endpoint &endpoint)
	{
		const auto at = m_socketsAtEndpoint.find(endpoint);
		if (at == m_socketsAtEndpoint.end()) {
			return;
		}
		std::vector<utp_socket *> &sockets = at->second;
		sockets.erase(std::remove(sockets.begin(), sockets.end(), socket), sockets.end());
		if (sockets.empty()) {
			m_socketsAtEndpoint.erase(at);
		}
	}

	void Forget(utp_socket *socket)
	{
		if (m_sending == socket) {
			m_sending = nullptr;
		}
		m_closingCrypt.erase(socket);
		m_acceptedAtMs.erase(socket);
		m_peerStreams.erase(socket);
		RemoveRemapCandidate(socket);
		RemovePortRemap(socket);
		const auto found = m_endpoints.find(socket);
		if (found == m_endpoints.end()) {
			return;
		}
		m_sentTo.erase(found->second);
		UnindexSocket(socket, found->second);
		m_peers.Remove(found->second.first, found->second.second);
		m_endpoints.erase(found);
	}

	utp_context *m_context = nullptr;
	IUtpStreamAcceptor *m_acceptor = nullptr;
	bool m_datagramObfuscated = false;
	// Borrowed for the span of one ProcessDatagram() call, like m_datagramObfuscated.
	const uint8_t *m_ingressKey = nullptr;
	Endpoint m_ingress;
	CUtpPeerRegistry m_peers;
	// Which endpoint each socket holds, so the registry can be kept until the
	// socket is destroyed rather than until its userdata is cleared.
	std::map<utp_socket *, std::pair<uint32_t, uint16_t>> m_endpoints;
	std::vector<SPendingHandshake> m_remapCandidates;
	// Client UDP ingress, libutp callbacks, and TickUtp all run on the main
	// thread. libutp itself is not thread-safe, so these maps share that
	// confinement rather than adding a lock around only adapter bookkeeping.
	// libutp keeps its socket hash under the original tuple, while the NAT
	// now uses the observed one. Keep both translations until DESTROYING.
	std::map<Endpoint, Endpoint> m_remapInputs;
	std::map<Endpoint, Endpoint> m_remapOutputs;
	std::map<utp_socket *, std::pair<Endpoint, Endpoint>> m_socketRemaps;
	std::uint64_t m_nowMs = 0;
	//! Bounds how often a connected stream may follow a NAT source-port change.
	UtpPortRepointLimiter m_repointLimiter;
	//! What each stream's peer last told us: its connection id and sequence numbers, learned from
	//! frames on that socket's own tuple. lastSentSequenceNumber is filled from m_sentTo when a move
	//! is decided, not stored here.
	std::map<utp_socket *, SPeerStreamState> m_peerStreams;
	//! Which sockets hold an endpoint, so a claimed frame is attributed without scanning every
	//! stream. Bounded by the live sockets and pruned wherever m_endpoints is.
	std::map<Endpoint, std::vector<utp_socket *>> m_socketsAtEndpoint;
	//! What we last sent to one destination. UTP_SENDTO has no socket (utp_internal.cpp:709), so this
	//! is keyed by where the packet went; a move requires exactly one connected stream on that
	//! address.
	std::map<Endpoint, SSentToEndpoint> m_sentTo;
	static constexpr std::size_t kMaxSentEndpoints = 512;
	// Sockets refused before a transport existed, closed once libutp has
	// finished with the datagram.
	std::vector<utp_socket *> m_refused;
	utp_socket *m_sending = nullptr;
	static constexpr size_t kUserHashBytes = 16;
	// Crypt parameters of closed sockets that libutp still sends on.
	std::map<utp_socket *, std::array<uint8_t, kUserHashBytes>> m_closingCrypt;
	//! When each accepted stream that may still be half-open was accepted, in the datagram clock.
	std::map<utp_socket *, std::uint64_t> m_acceptedAtMs;
	// Refused streams, destroyed at the same point. Destruction is the close.
	std::vector<std::unique_ptr<IStreamTransport>> m_refusedStreams;
	// libutp's callbacks carry only the context's user pointer, which already
	// holds the datagram sink. One adapter exists per process.
	static CUtpLibraryAdapter *s_self;
};

CUtpLibraryAdapter *CUtpLibraryAdapter::s_self = nullptr;
} // namespace

std::unique_ptr<IUtpLibrary> CreateUtpLibrary()
{
	return std::make_unique<CUtpLibraryAdapter>();
}
// File_checked_for_headers
