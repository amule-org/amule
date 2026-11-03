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
// a fake gateway. Usage: UPnPPortMappingDriver <tcp port> <udp port, 0 for none> (add|delete)...

#include "config.h"
#include "UPnPBase.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
const int EXIT_SKIP = 77;
} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		return EXIT_FAILURE;
	}
	const int tcpPort = std::atoi(argv[1]);
	const int udpPort = std::atoi(argv[2]);

	std::vector<CUPnPPortMapping> mappings;
	mappings.emplace_back(tcpPort, "TCP", true, "aMule test TCP");
	if (udpPort != 0) {
		mappings.emplace_back(udpPort, "UDP", true, "aMule test UDP");
	}
	mappings.emplace_back(tcpPort + 1, "TCP", false, "aMule test disabled");

	try {
		CUPnPControlPoint controlPoint(0);
		for (int i = 0; i < 100 && !controlPoint.WanServiceDetected(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!controlPoint.WanServiceDetected()) {
			std::puts("no gateway discovered");
			return EXIT_SKIP;
		}
		// A gateway's services register one at a time.
		std::this_thread::sleep_for(std::chrono::seconds(1));

		for (int i = 3; i < argc; ++i) {
			const bool add = std::strcmp(argv[i], "add") == 0;
			const bool ok = add ? controlPoint.AddPortMappings(mappings)
					    : controlPoint.DeletePortMappings(mappings);
			std::printf("%s=%s\n", argv[i], ok ? "true" : "false");
		}
	} catch (const CUPnPException &) {
		std::puts("libupnp could not start");
		return EXIT_SKIP;
	}
	return EXIT_SUCCESS;
}
