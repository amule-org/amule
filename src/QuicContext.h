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

class IQuicConnection
{
public:
	virtual ~IQuicConnection() = default;
	virtual bool ProcessDatagram(const uint8_t *, size_t) = 0;
	virtual bool IsClosed() const = 0;
	virtual void Close() = 0;
	//! True for a connection ID this server issued and has not retired. After the server's
	//! first Initial the client switches its DCID to one of these (RFC 9000 section 7.2).
	virtual bool OwnsConnectionId(const std::string &) const = 0;
};

class IQuicConnectionFactory
{
public:
	virtual ~IQuicConnectionFactory() = default;
	virtual std::unique_ptr<IQuicConnection> CreateInbound(
		const uint8_t *, size_t, const CNetworkAddress &, uint16_t, const std::string &) = 0;
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
	bool AdmitNewConnection(const CNetworkAddress &, uint64_t nowMs);
	void EraseClosedConnections();

	IQuicConnectionFactory *m_factory;
	ConnectionMap m_connections;
	NatRendezvous::CRequesterLimiter m_newConnectionLimiter;
};

#endif
