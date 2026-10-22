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

#include "UtpSocketTransport.h" // per-socket crypt parameters, resolved from userdata
#include "UtpDialPolicy.h"
#include <libutp/utp.h>

#include <algorithm>
#include <array>
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
		m_nowMs = nowMs;
		const Endpoint observed{ ip, port };
		Endpoint expected = observed;
		utp_socket *remappedSocket = nullptr;
		const auto translation = m_remapInputs.find(observed);
		if (translation != m_remapInputs.end()) {
			expected = translation->second;
		} else {
			const auto candidateIndex =
				FindUniqueUtpPortRemapCandidate(m_remapCandidates, ip, port, nowMs);
			if (candidateIndex.has_value()) {
				const SPendingHandshake &candidate = m_remapCandidates[*candidateIndex];
				const Endpoint candidateExpected{ candidate.ip, candidate.port };
				// A second socket must never overwrite a live endpoint translation:
				// libutp has no socket in UTP_SENDTO, so ambiguity must fail closed.
				if (CanInstallPortRemap(candidate.socket, candidateExpected, observed)) {
					expected = candidateExpected;
					remappedSocket = candidate.socket;
					// libutp keys the socket by its original tuple. Install a provisional
					// destination-keyed translation so its immediate response reaches the
					// observed NAT mapping even though UTP_SENDTO has no socket argument.
					InstallPortRemap(remappedSocket, expected, observed);
				}
			}
		}
		const sockaddr_in address = MakeAddress(expected.first, expected.second);
		const bool claimed = utp_process_udp(m_context,
					     payload,
					     length,
					     reinterpret_cast<const sockaddr *>(&address),
					     sizeof(address)) != 0;
		if (remappedSocket != nullptr) {
			if (claimed) {
				UpdatePeerEndpoint(remappedSocket, ip, port);
			} else {
				RemovePortRemap(remappedSocket);
			}
		}
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
	void IssueDeferredAcks() override { utp_issue_deferred_acks(m_context); }
	void CheckTimeouts() override { utp_check_timeouts(m_context); }

private:
	struct SPendingHandshake : SUtpRemapCandidate
	{
		utp_socket *socket = nullptr;
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
		m_peers.Remove(found->second.first, found->second.second);
		m_peers.Add(ip, port);
		found->second = { ip, port };
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
		const auto *bytes = reinterpret_cast<const uint8_t *>(&address->sin_addr.s_addr);
		uint32_t ip = 0;
		for (unsigned i = 0; i < 4; ++i) {
			ip |= static_cast<uint32_t>(bytes[i]) << (8 * i);
		}
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
		const auto *bytes = reinterpret_cast<const uint8_t *>(&address->sin_addr.s_addr);
		uint32_t ip = 0;
		for (unsigned i = 0; i < 4; ++i) {
			ip |= static_cast<uint32_t>(bytes[i]) << (8 * i);
		}
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
		// libutp reaches CS_CONNECTED on the peer's first ST_DATA, silently.
		raw->MarkConnected();

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
			// The transport notifies its attached socket after the state transition;
			// outbound and inbound streams use the same connection-completion path.
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
	}

	void Forget(utp_socket *socket)
	{
		if (m_sending == socket) {
			m_sending = nullptr;
		}
		m_closingCrypt.erase(socket);
		RemoveRemapCandidate(socket);
		RemovePortRemap(socket);
		const auto found = m_endpoints.find(socket);
		if (found == m_endpoints.end()) {
			return;
		}
		m_peers.Remove(found->second.first, found->second.second);
		m_endpoints.erase(found);
	}

	utp_context *m_context = nullptr;
	IUtpStreamAcceptor *m_acceptor = nullptr;
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
	// Sockets refused before a transport existed, closed once libutp has
	// finished with the datagram.
	std::vector<utp_socket *> m_refused;
	utp_socket *m_sending = nullptr;
	static constexpr size_t kUserHashBytes = 16;
	// Crypt parameters of closed sockets that libutp still sends on.
	std::map<utp_socket *, std::array<uint8_t, kUserHashBytes>> m_closingCrypt;
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
