// This file is part of the aMule Project.
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU GPL version 2 or later.

#include "UtpDialPolicy.h"

namespace
{
constexpr uint32_t kPeer = 0x04030201; // 1.2.3.4, low byte first

constexpr bool DecisionMatrix()
{
	// Exhaust every combination of capabilities, service, route, proxy,
	// address family, address presence, and port presence. Legacy preserves
	// callbacks/buddy lookup for LowID and refusal for unsupported addresses.
	for (unsigned bits = 0; bits < 128; ++bits) {
		const SUtpDialFacts facts{ (bits % 2) != 0,
			((bits / 2) % 2) != 0,
			((bits / 4) % 2) != 0,
			((bits / 8) % 2) != 0,
			((bits / 16) % 2) != 0,
			((bits / 32) % 2) != 0 ? kPeer : 0,
			static_cast<uint16_t>((bits / 64) != 0 ? 4672 : 0) };
		const bool eligible = bits == (1 | 2 | 4 | 16 | 32 | 64);
		if ((DecideUtpDial(facts) == EUtpDialDecision::TryUtp) != eligible) {
			return false;
		}
	}
	return true;
}

constexpr bool EndpointBoundaries()
{
	return !IsUsableUtpEndpoint(0, 4672) && !IsUsableUtpEndpoint(0x01020300, 4672) && // 0/8
	       !IsUsableUtpEndpoint(0x010000E0, 4672) &&                                  // multicast
	       !IsUsableUtpEndpoint(0x010000F0, 4672) &&                                  // reserved
	       !IsUsableUtpEndpoint(0xffffffff, 4672) &&                                  // broadcast
	       !IsUsableUtpEndpoint(kPeer, 0) && IsUsableUtpEndpoint(kPeer, 1) &&
	       IsUsableUtpEndpoint(kPeer, 65535) && IsUsableUtpEndpoint(0x0100007f, 4672) && // loopback
	       IsUsableUtpEndpoint(0x0100000a, 4672);                                        // private
}

// These are compile-time behavior tests too, allowing focused validation
// without generating build artifacts or linking the application/dependencies.
static_assert(DecisionMatrix(), "uTP requires every prerequisite and no proxy");
static_assert(EndpointBoundaries(), "only usable IPv4 UDP endpoints may dial");
static_assert(
	DecideUtpDial({}) == EUtpDialDecision::PreserveLegacy, "unknown facts must preserve legacy behavior");
} // namespace

int main()
{
	return DecisionMatrix() && EndpointBoundaries() ? 0 : 1;
}
