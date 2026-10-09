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

#ifndef DEADSOURCEKEY_H
#define DEADSOURCEKEY_H

#include "ProtocolPeerIdentity.h"
#include <cstdint>

class CDeadSourceKey
{
public:
	CDeadSourceKey(const CProtocolPeerIdentity &identity,
		std::uint32_t hybridID,
		std::uint16_t tcpPort,
		std::uint32_t serverIP,
		std::uint16_t kadPort)
	: m_identity(identity)
	, m_hybridID(hybridID)
	, m_tcpPort(tcpPort)
	, m_serverIP(serverIP)
	, m_kadPort(kadPort)
	{
	}

	bool Matches(const CDeadSourceKey &other) const
	{
		if (IsNativeIPv6() || other.IsNativeIPv6())
			return IsNativeIPv6() && other.IsNativeIPv6() &&
			       m_identity.NativeIPv6() == other.m_identity.NativeIPv6() &&
			       (m_tcpPort == other.m_tcpPort || m_kadPort == other.m_kadPort);

		return m_hybridID == other.m_hybridID &&
		       (m_tcpPort == other.m_tcpPort || m_kadPort == other.m_kadPort) &&
		       (!IsLowID(m_hybridID) || m_serverIP == other.m_serverIP);
	}

	//! Hybrid ID 0 reads as LowID, but a native IPv6 peer is reachable by its address.
	bool UsesFirewalledBlockTime() const { return !IsNativeIPv6() && IsLowID(m_hybridID); }

	friend bool operator<(const CDeadSourceKey &a, const CDeadSourceKey &b)
	{
		if (a.IsNativeIPv6() != b.IsNativeIPv6())
			return a.IsNativeIPv6() < b.IsNativeIPv6();
		if (a.IsNativeIPv6())
			return a.m_identity.NativeIPv6() < b.m_identity.NativeIPv6();
		return a.m_hybridID < b.m_hybridID;
	}

private:
	bool IsNativeIPv6() const { return m_identity.GetKind() == CProtocolPeerIdentity::Kind::NativeIPv6; }

	CProtocolPeerIdentity m_identity;
	std::uint32_t m_hybridID;
	std::uint16_t m_tcpPort;
	std::uint32_t m_serverIP;
	std::uint16_t m_kadPort;
};

#endif
