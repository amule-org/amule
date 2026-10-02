//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef QUIC_CONTEXT_H
#define QUIC_CONTEXT_H

#include "NatRendezvousPolicy.h"
#include "NetworkAddress.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class IQuicConnection
{
public:
	virtual ~IQuicConnection() = default;
	//! @p nowMs is a monotonic millisecond tick, the same one CQuicContext::ProcessDatagram()
	//! received -- the only clock reading taken for this datagram.
	virtual bool ProcessDatagram(const uint8_t *, size_t, uint64_t nowMs) = 0;
	virtual bool IsClosed() const = 0;
	virtual void Close() = 0;
	//! True for a connection ID this server issued and has not retired. After the server's
	//! first Initial the client switches its DCID to one of these (RFC 9000 section 7.2).
	virtual bool OwnsConnectionId(const std::string &) const = 0;
	//! The server-issued source connection ID, if the implementation exposes one. Empty by
	//! default: only meaningful for connections whose engine actually issues one.
	virtual std::string GetIssuedConnectionId() const { return std::string(); }
	//! Bytes received so far on the peer's stream, and clears them. Empty if no stream has
	//! opened yet, or this implementation has none (the default).
	virtual std::vector<uint8_t> DrainStreamData() { return {}; }
	//! Reopens @p bytes of flow control as the application drains what DrainStreamData()
	//! returned. A no-op by default.
	virtual void ExtendStreamReadWindow(size_t) {}
	//! Services this connection's timers (RFC 9002 loss detection/retransmission, idle
	//! timeout): must run periodically and independently of inbound datagrams, or a connection
	//! that stops receiving ACKs never retransmits and never times out. A no-op by default; a
	//! real implementation closes the connection itself if the timer handling is fatal (e.g.
	//! the idle timeout has elapsed).
	virtual void Tick(uint64_t nowMs) { (void)nowMs; }
};

class IQuicConnectionFactory
{
public:
	virtual ~IQuicConnectionFactory() = default;
	//! nowMs lets an implementation stamp the new connection's own clock (e.g. the deadline a
	//! handshake timeout is measured from) with the same monotonic time ProcessDatagram() is
	//! already using, rather than an implicit zero that would only coincide with "now" for a
	//! connection created in the first instant of the process's life.
	virtual std::unique_ptr<IQuicConnection> CreateInbound(const uint8_t *,
		size_t,
		const CNetworkAddress &,
		uint16_t,
		const std::string &,
		uint64_t nowMs) = 0;
};

class CQuicContext final
{
public:
	static constexpr size_t kMaxConnections = 256;
	static constexpr size_t kMaxConnectionsPerScope = 8;
	//! RFC 9000 section 14.1: a server discards Initials in smaller datagrams.
	static constexpr size_t kMinInitialDatagram = 1200;

	explicit CQuicContext(IQuicConnectionFactory *factory = nullptr)
	: m_factory(factory)
	{
	}
	~CQuicContext();
	CQuicContext(const CQuicContext &) = delete;
	CQuicContext &operator=(const CQuicContext &) = delete;

	//! @p nowMs is a monotonic millisecond tick for new-connection rate limiting.
	bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		const CNetworkAddress &address,
		uint16_t port,
		uint64_t nowMs);
	//! Services every live connection's timers. Must be called periodically regardless of
	//! whether any datagram has arrived -- this is what lets a connection recover from loss
	//! instead of hanging forever once nothing more is received from the peer.
	void Tick(uint64_t nowMs);
	size_t ConnectionCount() const { return m_connections.size(); }

private:
	struct SConnectionKey
	{
		CNetworkAddress address;
		uint16_t port;
		std::string cid;
		bool operator<(const SConnectionKey &other) const
		{
			return std::tie(address, port, cid) < std::tie(other.address, other.port, other.cid);
		}
	};
	using ConnectionMap = std::map<SConnectionKey, std::unique_ptr<IQuicConnection>>;

	static bool ClassifyInitial(const uint8_t *, size_t, std::string &);
	ConnectionMap::iterator FindSoleEndpointConnection(const CNetworkAddress &, uint16_t);
	ConnectionMap::iterator FindInitialConnection(const CNetworkAddress &, uint16_t, const std::string &);
	ConnectionMap::iterator FindNonInitialConnection(
		const CNetworkAddress &, uint16_t, const uint8_t *, size_t);
	bool AdmitNewConnection(const CNetworkAddress &, uint64_t nowMs);
	void EraseClosedConnections();

	IQuicConnectionFactory *m_factory;
	ConnectionMap m_connections;
	NatRendezvous::CRequesterLimiter m_newConnectionLimiter;
};

#endif
