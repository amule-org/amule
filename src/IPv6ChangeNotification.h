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

#ifndef IPV6_CHANGE_NOTIFICATION_H
#define IPV6_CHANGE_NOTIFICATION_H

#include "NetworkAddress.h"

#include <optional>

inline bool ShouldSendPendingIPv6Change(bool pending,
	bool peerAdvertisedIPv6,
	bool connected,
	const std::optional<CNetworkAddress> &advertised)
{
	return pending && peerAdvertisedIPv6 && connected && advertised.has_value();
}

inline bool EncodeIPv6ChangePayload(const CNetworkAddress &address, uint8_t payload[16])
{
	return address.ToIPv6Bytes(payload);
}

#endif // IPV6_CHANGE_NOTIFICATION_H
