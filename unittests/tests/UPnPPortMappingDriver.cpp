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
// a fake gateway and writes to stdin once every service has registered.
// Usage: UPnPPortMappingDriver <tcp port> <udp port> (add|delete)...

#include "config.h"
#include "UPnPBase.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

	std::vector<CUPnPPortMapping> mappings;
	mappings.emplace_back(tcpPort, "TCP", true, "aMule test TCP");
	mappings.emplace_back(std::atoi(argv[2]), "UDP", true, "aMule test UDP");
	mappings.emplace_back(tcpPort + 1, "TCP", false, "aMule test disabled");

	try {
		CUPnPControlPoint controlPoint(0);
		if (std::getchar() == EOF) {
			return EXIT_FAILURE;
		}
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
