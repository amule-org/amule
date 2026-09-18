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
#include <libutp/utp.h>

#include <map>
#include <utility>
#include <vector>

namespace
{
class CUtpLibraryAdapter final : public IUtpLibrary, public IUtpSocketOperations
{
public:
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
		// Closed before the userdata goes, so the FIN this sends carries the
		// stream's crypt parameters like every other datagram on it.
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
		if (!m_context || transport || ip == 0 || port == 0 || (encrypt && userHash == nullptr)) {
			return false;
		}
		auto *socket = utp_create_socket(m_context);
		if (socket == nullptr) {
			return false;
		}
		auto pending = std::make_unique<CUtpSocketTransport>(
			*this, socket, CNetworkAddress::FromIPv4NetworkOrder(ip), port);
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
		transport = std::move(pending);
		return true;
	}

	void SetAcceptor(IUtpStreamAcceptor *acceptor) override { m_acceptor = acceptor; }

	bool HasRegisteredPeer(uint32_t ip, uint16_t port) const override { return m_peers.Has(ip, port); }
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
		m_context = nullptr;
		m_acceptor = nullptr;
		if (s_self == this) {
			s_self = nullptr;
		}
	}
	bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) override
	{
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		auto *bytes = reinterpret_cast<uint8_t *>(&address.sin_addr.s_addr);
		for (unsigned i = 0; i < 4; ++i) {
			bytes[i] = static_cast<uint8_t>(ip >> (8 * i));
		}
		const bool claimed = utp_process_udp(m_context,
					     payload,
					     length,
					     reinterpret_cast<const sockaddr *>(&address),
					     sizeof(address)) != 0;
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
		auto *sink = static_cast<IUtpDatagramSink *>(utp_context_get_userdata(args->context));
		// From the socket, never the destination: an address can belong to
		// several clients. A context-level send has no verified peer.
		bool encrypt = false;
		const uint8_t *userHash = nullptr;
		if (args->socket != nullptr) {
			const auto *transport =
				static_cast<const CUtpSocketTransport *>(utp_get_userdata(args->socket));
			if (transport != nullptr) {
				encrypt = transport->CryptParameters(&userHash);
			}
		}
		sink->SendUtpDatagram(args->buf, args->len, ip, ntohs(address->sin_port), encrypt, userHash);
		return 0;
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

		// Interface type so the seam can move out of it; the concrete pointer
		// stays for the setup below.
		auto owned = std::make_unique<CUtpSocketTransport>(
			*s_self, args->socket, CNetworkAddress::FromIPv4NetworkOrder(ip), port);
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
		if (!s_self->m_acceptor->AcceptStream(transport, ip, port)) {
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
			transport->OnEnded(EUtpTransportFailure::Refused);
			break;
		case UTP_ETIMEDOUT:
			transport->OnEnded(EUtpTransportFailure::TimedOut);
			break;
		case UTP_ECONNRESET:
		default:
			transport->OnEnded(EUtpTransportFailure::Reset);
			break;
		}
		return 0;
	}

	static uint64 OnRead(utp_callback_arguments *args)
	{
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
	// Sockets refused before a transport existed, closed once libutp has
	// finished with the datagram.
	std::vector<utp_socket *> m_refused;
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
