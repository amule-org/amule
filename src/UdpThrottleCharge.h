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

#ifndef UDPTHROTTLECHARGE_H
#define UDPTHROTTLECHARGE_H

#include <cstdint>

/**
 * Bytes a sent UDP datagram charges to the upload throttler. A uTP or QUIC datagram carries stream
 * bytes the throttler already charged when the client socket wrote them, as it does for a TCP
 * stream, so it charges nothing: counting it again would spend the upload budget, and report the
 * upload, twice.
 */
inline std::uint32_t ThrottledUdpBytes(bool chargedToStream, std::uint32_t wireBytes)
{
	return chargedToStream ? 0 : wireBytes;
}

#endif // UDPTHROTTLECHARGE_H
