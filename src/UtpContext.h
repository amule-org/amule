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
		return Configure() && m_library->ProcessDatagram(payload, length, ip, port);
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

// Keep CPacket's application types out of the libutp translation unit.
// The real socket and fake queue exercise this same plaintext send path.
template <typename Packet, typename Socket>
void QueueUtpDatagram(Socket &socket, const uint8_t *payload, size_t length, uint32_t ip, uint16_t port)
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
	socket.SendPacket(packet.release(), ip, port, false, nullptr, false, 0);
}

#endif // UTP_CONTEXT_H
// File_checked_for_headers
