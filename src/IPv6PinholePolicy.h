// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#ifndef IPV6_PINHOLE_POLICY_H
#define IPV6_PINHOLE_POLICY_H

#include <string>

//! Whether to record a pinhole request. Deliberately not gated on a discovered gateway: discovery
//! is asynchronous, so gating on it would never record a request at startup, and a request recorded
//! before the gateway registers is replayed when the service appears.
inline bool ShouldRequestIPv6Pinhole(bool ipv6Built, bool listenerBoundToAllIPv6)
{
	return ipv6Built && listenerBoundToAllIPv6;
}

inline bool ShouldRenewIPv6Pinhole(unsigned int remainingLease, unsigned int renewalMargin)
{
	return remainingLease <= renewalMargin;
}

inline bool IsIPv6PinholeStale(unsigned short currentPort,
	const std::string &currentAddress,
	unsigned short pinholePort,
	const std::string &pinholeAddress)
{
	return currentPort != pinholePort || currentAddress != pinholeAddress;
}

#endif // IPV6_PINHOLE_POLICY_H
