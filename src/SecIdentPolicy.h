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

#ifndef SECIDENTPOLICY_H
#define SECIDENTPOLICY_H

namespace SecIdent
{
enum Version
{
	Unavailable = 0,
	V1 = 1,
	V2 = 2
};

inline unsigned SupportedVersions(bool cryptoAvailable, bool hasPeerIPv4)
{
	return cryptoAvailable ? (hasPeerIPv4 ? V1 | V2 : V1) : Unavailable;
}

inline Version SignatureVersion(unsigned peerVersions, bool hasPeerIPv4)
{
	// Preserve the historical v1 preference, even on IPv4 connections.
	if (peerVersions & V1) {
		return V1;
	}
	return (hasPeerIPv4 && (peerVersions & V2)) ? V2 : Unavailable;
}
} // namespace SecIdent

#endif // SECIDENTPOLICY_H
