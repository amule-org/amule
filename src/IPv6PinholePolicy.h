// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#ifndef IPV6_PINHOLE_POLICY_H
#define IPV6_PINHOLE_POLICY_H

#include <string>

inline bool ShouldRequestIPv6Pinhole(bool ipv6Built, bool listenerBoundToAllIPv6, bool backendAvailable)
{
	return ipv6Built && listenerBoundToAllIPv6 && backendAvailable;
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
