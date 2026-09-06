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

#ifndef KAD2CONSTANTS_H
#define KAD2CONSTANTS_H

// Kad protocol versions, as advertised in the version byte of every Kad2
// contact record and compared against CContact::GetVersion().  The named
// constants exist so that version gates read as protocol requirements rather
// than as magic numbers: a peer at 0x08 must not be sent 0x09 features.
#define KADEMLIA_VERSION1_46c 0x01 /* 45b - 46c */
#define KADEMLIA_VERSION2_47a 0x02 /* 47a */
#define KADEMLIA_VERSION3_47b 0x03 /* 47b */
#define KADEMLIA_VERSION4_47c 0x04 /* 47c */
#define KADEMLIA_VERSION5_48a 0x05 /* -0.48a */
#define KADEMLIA_VERSION6_49aBETA \
	0x06                       /* -0.49aBETA1: OP_FWCHECKUDPREQ, obfuscation, direct callbacks, \
				      source type 6, UDP firewall check */
#define KADEMLIA_VERSION7_49a 0x07 /* -0.49a: OP_KAD_FWTCPCHECK_ACK, KADEMLIA_FIREWALLED2_REQ */
#define KADEMLIA_VERSION8_49b 0x08 /* TAG_KADMISCOPTIONS, KADEMLIA2_HELLO_RES_ACK */
#define KADEMLIA_VERSION9_50a 0x09 /* AICH hashes on keyword storage */

// Our own advertised version.  0x0a alongside the AICH keyword-storage support
// that 0x09 introduced; 0x0a adds no further wire element of its own and is the
// level eMule/eMuleAI advertise.
//
// Gated on ENABLE_KAD_PROTOCOL_10 (configure-time, OFF by default) because this
// byte goes out in every Kad2 hello: without the switch we keep advertising
// 0x08, which is what a build without the 0x09 features can honestly claim.
//
// Note for a future bump: CT_EMULE_MISCOPTIONS2 has to change once the Kad
// version reaches 0x0F, because the eD2k capability field only reserves four
// bits for it.
#ifdef ENABLE_KAD_PROTOCOL_10
#define KADEMLIA_VERSION 0x0a
#else
#define KADEMLIA_VERSION 0x08 /* 0.49b */
#endif

#endif // KAD2CONSTANTS_H
