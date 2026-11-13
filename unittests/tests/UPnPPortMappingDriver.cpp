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
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
//

// Prints what the real control point returns. UPnPPortMappingIntegrationTest.py runs it against
// a fake gateway. It prints a line when the constructor returns, then waits for a line on stdin
// before each operation.
// Usage: UPnPPortMappingDriver <tcp port> <udp port> (add|delete|pending)...

#include "config.h"
#include "UPnPBase.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
const int EXIT_SKIP = 77;
// The address the fake gateway should see as the pinhole's inside end, and a lease long enough that
// no renewal is attempted by the timer during the run.
const char *const PINHOLE_CLIENT = "2001:db8::1";
const unsigned int PINHOLE_LEASE = 3600;
const unsigned int PINHOLE_LEASE_SHORT = 1;
} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		return EXIT_FAILURE;
	}
	const int tcpPort = std::atoi(argv[1]);

	std::vector<CUPnPPortMapping> mappings;
	mappings.emplace_back(tcpPort, "TCP", true, "aMule test TCP");
	mappings.emplace_back(std::atoi(argv[2]), "UDP", true, "aMule test UDP");
	mappings.emplace_back(tcpPort + 1, "TCP", false, "aMule test disabled");

	try {
		CUPnPControlPoint controlPoint(0);
		std::puts("constructed");
		std::fflush(stdout);
		for (int i = 3; i < argc; ++i) {
			if (std::getchar() == EOF) {
				return EXIT_SUCCESS;
			}
			bool result;
			if (std::strcmp(argv[i], "add") == 0) {
				result = controlPoint.AddPortMappings(mappings);
			} else if (std::strcmp(argv[i], "delete") == 0) {
				result = controlPoint.DeletePortMappings(mappings);
			} else if (std::strcmp(argv[i], "addpinhole") == 0) {
				controlPoint.AddIPv6Pinhole(PINHOLE_CLIENT,
					static_cast<unsigned short>(tcpPort),
					6,
					PINHOLE_LEASE);
				result = true;
			} else if (std::strcmp(argv[i], "addpinholeshort") == 0) {
				// A lease already inside the renewal margin, so the production pass renews it
				// instead of skipping it.
				controlPoint.AddIPv6Pinhole(PINHOLE_CLIENT,
					static_cast<unsigned short>(tcpPort),
					6,
					PINHOLE_LEASE_SHORT);
				result = true;
			} else if (std::strcmp(argv[i], "renewexpiring") == 0) {
				// Driven the way production drives it: the whole renewal pass, not a single
				// key. The short lease needs its second to elapse before the pass runs, or
				// the policy correctly decides nothing is near expiry.
				std::this_thread::sleep_for(std::chrono::seconds(2));
				controlPoint.RenewExpiringIPv6Pinholes(
					static_cast<unsigned short>(tcpPort), PINHOLE_CLIENT);
				result = true;
			} else if (std::strcmp(argv[i], "deletepinholes") == 0) {
				controlPoint.DeleteIPv6Pinholes();
				result = true;
			} else {
				result = controlPoint.DiscoveryPending();
			}
			std::printf("%s=%s\n", argv[i], result ? "true" : "false");
			std::fflush(stdout);
		}
		// A late service still registers until the test closes stdin.
		while (std::getchar() != EOF) {
		}
	} catch (const CUPnPException &) {
		std::puts("libupnp could not start");
		return EXIT_SKIP;
	}
	return EXIT_SUCCESS;
}
