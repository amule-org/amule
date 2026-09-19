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

#ifndef UPLOADQUEUEADDRESSPOLICY_H
#define UPLOADQUEUEADDRESSPOLICY_H

#include "PeerAddressing.h"

namespace UploadQueueAddressPolicy
{
/** Match a UDP requester to a queued peer without narrowing native IPv6 to zero. */
inline bool Matches(const CNetworkAddress &requester, const CNetworkAddress &peer) noexcept
{
	return PeerAddressing::IsIndexable(requester) && PeerAddressing::IsIndexable(peer) &&
	       PeerAddressing::IndexKey(requester) == PeerAddressing::IndexKey(peer);
}
} // namespace UploadQueueAddressPolicy

#endif // UPLOADQUEUEADDRESSPOLICY_H
