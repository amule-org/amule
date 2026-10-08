//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef PROTOCOLPEERIDENTITY_H
#define PROTOCOLPEERIDENTITY_H

#include "NetworkAddress.h"
#include <cstdint>
#include <optional>

class CProtocolPeerIdentity
{
public:
	enum class Kind : std::uint8_t
	{
		Absent,
		LegacyIPv4Id,
		NativeIPv6
	};

	static CProtocolPeerIdentity Absent() { return CProtocolPeerIdentity(); }
	static CProtocolPeerIdentity FromLegacyIPv4Id(std::uint32_t id)
	{
		CProtocolPeerIdentity result;
		if (id != 0) {
			result.m_kind = Kind::LegacyIPv4Id;
			result.m_legacyId = id;
		}
		return result;
	}
	static CProtocolPeerIdentity FromNativeIPv6(const CNetworkAddress &address)
	{
		CProtocolPeerIdentity result;
		if (address.IsPresent() && address.IsIPv6() && !address.IsUnspecified() &&
			!address.IsIPv4Mapped()) {
			result.m_kind = Kind::NativeIPv6;
			result.m_nativeIPv6 = address;
		}
		return result;
	}
	static CProtocolPeerIdentity FromLegacyIPv4IdOrNativeIPv6(
		std::uint32_t id, const CNetworkAddress &address)
	{
		return id != 0 ? FromLegacyIPv4Id(id) : FromNativeIPv6(address);
	}

	Kind GetKind() const noexcept { return m_kind; }
	std::optional<std::uint32_t> TryGetLegacyIPv4Id() const noexcept
	{
		return m_kind == Kind::LegacyIPv4Id ? std::optional<std::uint32_t>(m_legacyId) : std::nullopt;
	}
	const CNetworkAddress &NativeIPv6() const noexcept { return m_nativeIPv6; }

	friend bool operator==(const CProtocolPeerIdentity &a, const CProtocolPeerIdentity &b) noexcept
	{
		return a.m_kind == b.m_kind && a.m_legacyId == b.m_legacyId &&
		       a.m_nativeIPv6 == b.m_nativeIPv6;
	}
	friend bool operator!=(const CProtocolPeerIdentity &a, const CProtocolPeerIdentity &b) noexcept
	{
		return !(a == b);
	}
	friend bool operator<(const CProtocolPeerIdentity &a, const CProtocolPeerIdentity &b) noexcept
	{
		if (a.m_kind != b.m_kind)
			return a.m_kind < b.m_kind;
		if (a.m_kind == Kind::LegacyIPv4Id)
			return a.m_legacyId < b.m_legacyId;
		if (a.m_kind == Kind::NativeIPv6)
			return a.m_nativeIPv6 < b.m_nativeIPv6;
		return false;
	}

private:
	CProtocolPeerIdentity() = default;
	Kind m_kind = Kind::Absent;
	std::uint32_t m_legacyId = 0;
	CNetworkAddress m_nativeIPv6;
};

#endif
