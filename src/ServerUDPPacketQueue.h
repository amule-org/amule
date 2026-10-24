// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#ifndef SERVERUDPPACKETQUEUE_H
#define SERVERUDPPACKETQUEUE_H

#include "Packet.h"
#include <wx/string.h>
#include <list>
#include <memory>
#include <utility>

struct ServerUDPPacket
{
	std::unique_ptr<CPacket> packet;
	uint32 ip = 0;
	uint16 port = 0;
	wxString addr;
	// The server's TCP port, for GetServerByAddress(); port is where the packet goes.
	uint16 serverPort = 0;
};

// DNS can stall delivery indefinitely. Bound both the number of retained
// packets and their wire bytes, and release them when the socket is destroyed.
class CServerUDPPacketQueue
{
public:
	static constexpr size_t MaxPackets = 1024;
	static constexpr size_t MaxBytes = 1024 * 1024;

	CServerUDPPacketQueue() = default;
	// This queue belongs to one socket. Keep the byte counter and packet list
	// together rather than allowing a default move to leave a stale counter.
	CServerUDPPacketQueue(const CServerUDPPacketQueue &) = delete;
	CServerUDPPacketQueue &operator=(const CServerUDPPacketQueue &) = delete;
	CServerUDPPacketQueue(CServerUDPPacketQueue &&) = delete;
	CServerUDPPacketQueue &operator=(CServerUDPPacketQueue &&) = delete;

	// Takes ownership even on rejection; retain the front packet for its DNS callback.
	bool Push(ServerUDPPacket item)
	{
		if (!item.packet || m_packets.size() >= MaxPackets) {
			return false;
		}
		const uint64 bytes = static_cast<uint64>(item.packet->GetPacketSize()) + 2;
		if (bytes > MaxBytes - m_bytes) {
			return false;
		}
		m_packets.push_back(std::move(item));
		m_bytes += bytes;
		return true;
	}

	ServerUDPPacket Pop()
	{
		m_bytes -= static_cast<uint64>(m_packets.front().packet->GetPacketSize()) + 2;
		ServerUDPPacket item = std::move(m_packets.front());
		m_packets.pop_front();
		return item;
	}

	void RotateFront() { m_packets.splice(m_packets.end(), m_packets, m_packets.begin()); }
	ServerUDPPacket &Front() { return m_packets.front(); }
	bool Empty() const { return m_packets.empty(); }
	size_t Size() const { return m_packets.size(); }
	uint64 Bytes() const { return m_bytes; }

private:
	std::list<ServerUDPPacket> m_packets;
	uint64 m_bytes = 0;
};

#endif // SERVERUDPPACKETQUEUE_H
