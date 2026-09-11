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

#ifndef UTP_CONTEXT_H
#define UTP_CONTEXT_H

#include "ReservedProtocolFrames.h"

#include <cstdint>
#include <memory>
#include <utility>

// IPv4 uses aMule's low-byte-first integer representation; ports are host order.
class IUtpDatagramSink
{
public:
	virtual ~IUtpDatagramSink() = default;
	virtual void SendUtpDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) = 0;
};

class IUtpContext
{
public:
	virtual ~IUtpContext() = default;
	virtual bool Configure() = 0;
	virtual void Destroy() = 0;
	virtual bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) = 0;
	virtual void Tick() = 0;
};

// Library seam: no libutp types or stream operations escape the adapter.
class IUtpLibrary
{
public:
	virtual ~IUtpLibrary() = default;
	virtual bool Create(IUtpDatagramSink &sink) = 0;
	virtual void Destroy() = 0;
	virtual bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) = 0;
	virtual void IssueDeferredAcks() = 0;
	virtual void CheckTimeouts() = 0;
};

// Main-thread only. Tick never recreates state abandoned by socket Close().
class CUtpContext final : public IUtpContext
{
public:
	CUtpContext(std::unique_ptr<IUtpLibrary> library, IUtpDatagramSink &sink)
	: m_library(std::move(library))
	, m_sink(sink)
	{
	}
	~CUtpContext() override { Destroy(); }
	bool Configure() override
	{
		if (!m_active) {
			m_active = m_library->Create(m_sink);
		}
		return m_active;
	}
	void Destroy() override
	{
		if (m_active) {
			m_library->Destroy();
			m_active = false;
		}
	}
	bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) override
	{
		if (!Configure()) {
			return false;
		}
		const bool claimed = m_library->ProcessDatagram(payload, length, ip, port);
		m_library->IssueDeferredAcks();
		return claimed;
	}
	void Tick() override
	{
		if (m_active) {
			m_library->IssueDeferredAcks();
			m_library->CheckTimeouts();
		}
	}

private:
	std::unique_ptr<IUtpLibrary> m_library;
	IUtpDatagramSink &m_sink;
	bool m_active = false;
};

inline bool ProcessUtpFrame(
	IUtpContext &context, const SReservedProt2Frame &frame, uint32_t ip, uint16_t port)
{
	return frame.disposition == RP2_KNOWN_TYPE && frame.type == OP_NATT_FRAME_UTP &&
	       context.ProcessDatagram(frame.payload, frame.payloadLength, ip, port);
}

// UDP sizing for libutp, accounting for the two-byte aMule envelope.
//
// libutp's own defaults (utp_default_get_udp_mtu / _overhead in utp_utils.cpp) branch on the
// address family and know nothing of the envelope, so overriding them is what makes libutp size
// packets that do not fragment. Branching the same way keeps the family awareness those defaults
// had: an IPv6 peer costs 20 more header bytes than IPv4, and libutp assumes Teredo because it
// cannot know the local interface either.
//
// Taken as a bool rather than a sockaddr so this stays free of socket headers and testable without
// one; the adapter does the sa_family comparison.
constexpr std::uint64_t kUtpEnvelopeBytes = 2;

constexpr std::uint64_t UtpUdpMtu(bool isIPv6)
{
	// IPv4:   1500 ethernet - 20 IPv4 - 8 UDP - 24 GRE - 8 PPPoE - 2 MPPE - 36 fudge.
	// Teredo: 1280 - 40 IPv6 - 8 UDP.
	return (isIPv6 ? UINT64_C(1232) : UINT64_C(1402)) - kUtpEnvelopeBytes;
}

constexpr std::uint64_t UtpUdpOverhead(bool isIPv6)
{
	// IPv4: 20 + 8. Teredo: that, plus 40 IPv6 + 8 UDP again.
	return (isIPv6 ? UINT64_C(76) : UINT64_C(28)) + kUtpEnvelopeBytes;
}

// Keep CPacket's application types out of the libutp translation unit.
template <typename Packet, typename Socket>
void QueueUtpDatagram(Socket &socket,
	const uint8_t *payload,
	size_t length,
	uint32_t ip,
	uint16_t port,
	bool encrypt,
	const uint8_t *hash)
{
	// Maximum IPv4 UDP payload, less the aMule envelope.
	if (length > 65507 - 2 || (length != 0 && payload == nullptr)) {
		return;
	}
	auto packet = std::make_unique<Packet>(
		OP_NATT_FRAME_UTP, static_cast<uint32_t>(length), OP_UDPRESERVEDPROT2);
	if (length != 0) {
		packet->CopyToDataBuffer(0, payload, static_cast<unsigned int>(length));
	}
	socket.SendPacket(packet.release(), ip, port, encrypt, hash, false, 0);
}

#endif // UTP_CONTEXT_H
// File_checked_for_headers
