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

#ifndef TRACKEDCLIENTRECORD_H
#define TRACKEDCLIENTRECORD_H

#include "Types.h" // Needed for uint16, uint64
#include "PeerAddressing.h"

#include <map>

/**
 * The user hash each deleted peer had per address and port, kept after its client is gone so a
 * peer that returns with a different hash can be banned for it. The tick is a parameter, so expiry
 * is testable without the application clock.
 */
class CTrackedClientRecord
{
public:
	bool Add(const CNetworkAddress &address, uint16 port, void *hash, uint64 now)
	{
		const auto key = PeerAddressing::IndexKey(address);
		if (key.IsAbsent()) {
			return false;
		}
		auto &record = m_clients[key];
		record.inserted = now;
		record.hashes[port] = hash;
		return true;
	}

	bool Compare(const CNetworkAddress &address, uint16 port, void *hash) const
	{
		const auto client = m_clients.find(PeerAddressing::IndexKey(address));
		if (client == m_clients.end()) {
			return true;
		}
		const auto prior = client->second.hashes.find(port);
		return prior == client->second.hashes.end() || prior->second == hash;
	}

	void DropLapsed(uint64 now, uint64 duration)
	{
		for (auto it = m_clients.begin(); it != m_clients.end();) {
			if (it->second.inserted + duration < now) {
				it = m_clients.erase(it);
			} else {
				++it;
			}
		}
	}

private:
	struct Record
	{
		uint64 inserted = 0;
		std::map<uint16, void *> hashes;
	};
	std::map<CNetworkAddress, Record> m_clients;
};

#endif // TRACKEDCLIENTRECORD_H
