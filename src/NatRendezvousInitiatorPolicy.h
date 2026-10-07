//
// This file is part of the aMule Project.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License.
//
#ifndef NATRENDEZVOUSINITIATORPOLICY_H
#define NATRENDEZVOUSINITIATORPOLICY_H

#include <cstdint>

/** Pure requester-side admission policy for NAT rendezvous initiation. */
struct SNatRendezvousInitiatorFacts
{
	bool localLowId;
	bool targetLowId;
	std::uint64_t targetIdentity;
	std::uint64_t currentServerContext;
	std::uint64_t targetServerContext;
	std::uint64_t servingBuddyServerContext;
	bool servingBuddyConnected;
	bool servingBuddyHighId;
	bool servingBuddyServerVerified;
	bool traversalCapabilityAdvertised;
};

inline bool CanInitiateNatRendezvous(const SNatRendezvousInitiatorFacts &facts) noexcept
{
	return facts.localLowId && facts.targetLowId && facts.targetIdentity != 0 &&
	       facts.targetServerContext == facts.currentServerContext &&
	       facts.servingBuddyServerContext == facts.currentServerContext && facts.servingBuddyConnected &&
	       facts.servingBuddyHighId && facts.servingBuddyServerVerified &&
	       facts.traversalCapabilityAdvertised;
}

#endif // NATRENDEZVOUSINITIATORPOLICY_H
