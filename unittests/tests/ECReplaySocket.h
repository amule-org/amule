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

#ifndef AMULE_EC_REPLAY_SOCKET_H
#define AMULE_EC_REPLAY_SOCKET_H

#include <ec/cpp/ECMemSocket.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

// Serves a recorded reply to SendRecvPacket; the request lands unread in CECMemSocket's buffer.
// The socket's own flags only shape what it sends: replies are parsed by their header flags.
class CReplaySocket : public CECMemSocket
{
public:
	explicit CReplaySocket(std::vector<unsigned char> reply)
	: m_reply(std::move(reply))
	{
	}

private:
	std::vector<unsigned char> m_reply;
	size_t m_pos = 0;

	uint32 InternalRead(void *ptr, uint32 len) override
	{
		const uint32 n = std::min<size_t>(len, m_reply.size() - m_pos);
		memcpy(ptr, m_reply.data() + m_pos, n);
		m_pos += n;
		return n;
	}
};

#endif // AMULE_EC_REPLAY_SOCKET_H
