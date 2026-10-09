//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

#ifndef PROTOCOLPEERIDENTITY_H
#define PROTOCOLPEERIDENTITY_H

#include "NetworkAddress.h"
#include "NetworkFunctions.h"
#include "PeerAddressing.h"
#include <cstdint>
#include <optional>

class CProtocolPeerIdentity
{
public:
	enum class Kind : std::uint8_t
	{
		Absent,
		IPv4HighID,
		ServerScopedLowID,
		NativeIPv6
	};
	static CProtocolPeerIdentity Absent() { return CProtocolPeerIdentity(); }
	static CProtocolPeerIdentity FromIPv4HighID(std::uint32_t id)
	{
		CProtocolPeerIdentity r;
		const auto address = CNetworkAddress::FromIPv4HostOrder(id);
		const auto key = PeerAddressing::IndexKey(address);
		if (!IsLowID(id) && PeerAddressing::IsSecurityKey(key)) {
			r.m_kind = Kind::IPv4HighID;
			r.m_address = key;
		}
		return r;
	}
	static CProtocolPeerIdentity FromNativeIPv6(const CNetworkAddress &address)
	{
		CProtocolPeerIdentity r;
		const auto key = PeerAddressing::IndexKey(address);
		if (key.IsIPv6() && PeerAddressing::IsSecurityKey(key)) {
			r.m_kind = Kind::NativeIPv6;
			r.m_address = key;
		}
		return r;
	}
	static CProtocolPeerIdentity FromServerScopedLowID(
		std::uint32_t id, const CNetworkAddress &server, std::uint16_t port)
	{
		CProtocolPeerIdentity r;
		const auto key = PeerAddressing::IndexKey(server);
		if (id != 0 && port != 0 && PeerAddressing::IsSecurityKey(key)) {
			r.m_kind = Kind::ServerScopedLowID;
			r.m_lowID = id;
			r.m_address = key;
			r.m_port = port;
		}
		return r;
	}
	static CProtocolPeerIdentity FromClientState(std::uint32_t id,
		bool lowID,
		const CNetworkAddress &user,
		std::uint32_t serverIP,
		std::uint16_t serverPort)
	{
		const auto userKey = PeerAddressing::IndexKey(user);
		if (userKey.IsIPv6() && PeerAddressing::IsSecurityKey(userKey))
			return FromNativeIPv6(userKey);
		if (lowID)
			return FromServerScopedLowID(
				id, CNetworkAddress::FromIPv4NetworkOrder(serverIP), serverPort);
		return FromIPv4HighID(id);
	}

	Kind GetKind() const noexcept { return m_kind; }
	std::optional<CNetworkAddress> TryGetIPv4HighID() const noexcept
	{
		return m_kind == Kind::IPv4HighID ? std::optional<CNetworkAddress>(m_address) : std::nullopt;
	}
	std::optional<std::uint32_t> TryGetServerScopedLowID() const noexcept
	{
		return m_kind == Kind::ServerScopedLowID ? std::optional<std::uint32_t>(m_lowID)
							 : std::nullopt;
	}
	const CNetworkAddress &Address() const noexcept { return m_address; }
	std::uint16_t ServerPort() const noexcept { return m_port; }
	const CNetworkAddress &NativeIPv6() const noexcept { return m_address; }
	friend bool operator==(const CProtocolPeerIdentity &a, const CProtocolPeerIdentity &b) noexcept
	{
		return a.m_kind == b.m_kind && a.m_lowID == b.m_lowID && a.m_address == b.m_address &&
		       a.m_port == b.m_port;
	}
	friend bool operator!=(const CProtocolPeerIdentity &a, const CProtocolPeerIdentity &b) noexcept
	{
		return !(a == b);
	}
	friend bool operator<(const CProtocolPeerIdentity &a, const CProtocolPeerIdentity &b) noexcept
	{
		if (a.m_kind != b.m_kind)
			return a.m_kind < b.m_kind;
		if (a.m_address != b.m_address)
			return a.m_address < b.m_address;
		if (a.m_lowID != b.m_lowID)
			return a.m_lowID < b.m_lowID;
		return a.m_port < b.m_port;
	}

private:
	CProtocolPeerIdentity() = default;
	Kind m_kind = Kind::Absent;
	std::uint32_t m_lowID = 0;
	CNetworkAddress m_address;
	std::uint16_t m_port = 0;
};
#endif
