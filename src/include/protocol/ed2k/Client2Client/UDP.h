//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef ED2KC2CUDP_H
#define ED2KC2CUDP_H

// Extended prot client <-> Extended prot client UDP
enum ED2KExtendedClientUDP
{
	OP_REASKFILEPING = 0x90, // <HASH 16>
	OP_REASKACK = 0x91,      // <RANG 2>
	OP_FILENOTFOUND = 0x92,  // (null)
	OP_QUEUEFULL = 0x93,     // (null)
	OP_REASKCALLBACKUDP = 0x94,
#ifdef ENABLE_NATT_SERVER_COORDINATION
	// Empty peer UDP punch: the recipient dials uTP to the sender's observed UDP port.
	// Distinct from the raw UDP protocol byte NatPeerUdp::OP_NATT_HOLEPUNCH (0xB3).
	OP_HOLEPUNCH = 0xA1, // (null)
#endif
	OP_PORTTEST = 0xFE // Connection Test
};

#endif // ED2KC2CUDP_H
