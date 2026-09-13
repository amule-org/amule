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

#include <set>
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
		// Returned as it came: utp_writev answers -1 on a refusal, and clamping
		// that to 0 here would turn the sentinel into "took nothing" at the one
		// seam that can still tell the difference.
		return utp_writev(static_cast<utp_socket *>(socket), &vector, 1);
	}

	void NotifyReadDrained(Handle socket) override
	{
		utp_read_drained(static_cast<utp_socket *>(socket));
	}

	void CloseSocket(Handle socket) override { utp_close(static_cast<utp_socket *>(socket)); }

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

	void SetAcceptor(IUtpStreamAcceptor *acceptor) override { m_acceptor = acceptor; }

	bool HasRegisteredPeer(uint32_t ip, uint16_t port) const override { return m_peers.Has(ip, port); }
	void Destroy() override
	{
		if (!m_context) {
			return;
		}
		// Closed before the context goes. utp_destroy() is `delete ctx` and
		// libutp declares no destructor for it, so a socket still alive at that
		// point is neither closed nor announced: its transport would keep a
		// handle into freed memory and close it later. Closing here produces
		// UTP_STATE_DESTROYING for each, which is what clears those handles.
		const std::vector<utp_socket *> live(m_live.begin(), m_live.end());
		for (utp_socket *socket : live) {
			utp_close(socket);
		}
		m_live.clear();
		for (utp_socket *refused : m_refused) {
			utp_close(refused);
		}
		m_refused.clear();
		utp_destroy(m_context);
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
		// Closed here rather than inside UTP_ON_ACCEPT: utp_close() can produce
		// UTP_STATE_DESTROYING before it returns, which would re-enter the
		// callback that is still deciding.
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
		// Resolved from the socket, never from the destination: the peer that owns
		// this socket is known, whereas an address can belong to several clients.
		// args->socket is null for a context-level send, such as the RST libutp
		// answers an unmatched frame with, and there is no verified peer for that.
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
			// No acceptor installed refuses every inbound SYN, which is what
			// this build did before one existed.
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

		auto transport = std::make_unique<CUtpSocketTransport>(
			*s_self, args->socket, CNetworkAddress::FromIPv4NetworkOrder(ip), port);
		CUtpSocketTransport *raw = transport.get();
		// Set before admission can produce any callback, so a stream event that
		// arrives during it still finds its transport.
		utp_set_userdata(args->socket, raw);
		// A4: until this runs, libutp's 1 MiB default is the effective receive
		// bound rather than the 64 KiB kDefaultReadBound names.
		raw->ApplyReceiveBound();
		// The socket is connected from libutp's side but reaches CS_CONNECTED
		// only on the peer's first ST_DATA, silently, so the acceptor marks it.
		raw->MarkConnected();

		if (!s_self->m_acceptor->AcceptStream(std::move(transport), ip, port)) {
			utp_set_userdata(args->socket, nullptr);
			s_self->m_refused.push_back(args->socket);
			return 0;
		}
		s_self->m_peers.Add(ip, port);
		s_self->m_live.insert(args->socket);
		return 0;
	}

	static uint64 OnStateChange(utp_callback_arguments *args)
	{
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
			// Forgotten before the transport is told, so nothing can look the
			// peer up and find a socket that is already dying. The handle dies
			// with this callback, so userdata goes with it.
			if (s_self != nullptr) {
				s_self->m_peers.Remove(transport->GetPeerAddress().ToIPv4NetworkOrderOrZero(),
					transport->GetPeerPort());
			}
			if (s_self != nullptr) {
				s_self->m_live.erase(args->socket);
			}
			utp_set_userdata(args->socket, nullptr);
			transport->OnEnded(EUtpTransportFailure::Destroying);
			break;
		case UTP_STATE_CONNECT:
			// Outgoing only: libutp guards it with conn->state == CS_SYN_SENT,
			// and this build never dials. Mapping it would be dead code that
			// reads like a supported path.
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
		// Occupancy, not free space. libutp advertises opt_rcvbuf minus this
		// number, so reporting the free figure runs the feedback backwards and
		// nothing fails loudly when it does.
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
	utp_context *m_context = nullptr;
	IUtpStreamAcceptor *m_acceptor = nullptr;
	CUtpPeerRegistry m_peers;
	// Refused sockets, closed once libutp has finished with the datagram.
	std::vector<utp_socket *> m_refused;
	// Accepted sockets still alive, so shutdown can close them before the
	// context they live in is deleted.
	std::set<utp_socket *> m_live;
	// libutp's callbacks are free functions with no user pointer of their own
	// beyond the context's, which already carries the datagram sink. One
	// adapter exists per process, created in CamuleApp::OnInit.
	static CUtpLibraryAdapter *s_self;
};

CUtpLibraryAdapter *CUtpLibraryAdapter::s_self = nullptr;
} // namespace

std::unique_ptr<IUtpLibrary> CreateUtpLibrary()
{
	return std::make_unique<CUtpLibraryAdapter>();
}
// File_checked_for_headers
