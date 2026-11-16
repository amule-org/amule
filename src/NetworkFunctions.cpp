//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
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

#include "NetworkFunctions.h" // Interface declaration
#include "amuleIPV4Address.h"
#include <common/Macros.h> // Needed for itemsof()

bool StringIPtoUint32(const wxString &strIP, uint32 &Ip)
{
	// The current position in the current field, used to detect malformed fields (x.y..z).
	unsigned digit = 0;

	// The current field, used to ensure only IPs that looks like a.b.c.d are supported
	unsigned field = 0;

	// The value of the current field
	unsigned value = 0;

	// Stores the work-value of the IP, reference is not changed unless the str was valid
	uint32 tmp_ip = 0;

	wxString str = strIP.Strip(wxString::both);
	for (size_t i = 0; i < str.Length(); i++) {
		wxChar c = str.GetChar(i);

		if (c >= '0' && c <= '9' && (value >> 8) == 0) {
			value = (value * 10) + (c - '0');
			++digit;
		} else if (c == '.') {
			if (digit && (value >> 8) == 0) {
				tmp_ip = tmp_ip | value << (field * 8);

				// Rest the current field values
				value = digit = 0;
				++field;
			} else {
				return false;
			}
		} else {
			return false;
		}
	}

	// Only set the referenced value if it was a valid IP
	if (field == 3 && digit && (value >> 8) == 0) {
		Ip = tmp_ip | value << 24;
		return true;
	}

	return false;
}

uint32 StringHosttoUint32(const wxString &Host)
{
	if (Host.IsEmpty()) {
		return 0;
	}
	amuleIPV4Address solver;
	if (solver.Hostname(Host)) {
		uint32 result = StringIPtoUint32(solver.IPAddress());
		if (result != (uint32)-1) { // should not happen
			return result;
		}
	}
	// This actually happens on wrong hostname
	return 0;
}

struct filter_st
{
	uint32 addr; // Address in anti-host order.
	uint32 mask; // Mask in anti-host order.
};

// Some special-purpose blocks from the IANA registry (RFC 6890), and multicast. RFC 3330 also listed
// 39/8, 128.0/16, 191.255/16 and 223.255.255/24 as reserved, but the RIRs have allocated them since.
static filter_st reserved_ranges[] = {
	{ 0x00000000, 0x000000ff }, // 0.0.0.0/8       "This network" [RFC 791]
	{ 0x0000007f, 0x000000ff }, // 127.0.0.0/8     Loopback [RFC 1122]
	{ 0x0000fea9, 0x0000ffff }, // 169.254.0.0/16  Link local [RFC 3927]
	{ 0x000000c0, 0x00ffffff }, // 192.0.0.0/24    IETF protocol assignments [RFC 6890]
	{ 0x000200c0, 0x00ffffff }, // 192.0.2.0/24    TEST-NET-1 [RFC 5737]
	{ 0x006358c0, 0x00ffffff }, // 192.88.99.0/24  6to4 relay anycast, deprecated [RFC 7526]
	{ 0x000012c6, 0x0000feff }, // 198.18.0.0/15   Benchmarking [RFC 2544]
	{ 0x000000e0, 0x000000f0 }, // 224.0.0.0/4     Multicast [RFC 5771]
	{ 0x000000f0, 0x000000f0 }  // 240.0.0.0/4     Reserved [RFC 1112]
};

// Private-Use Networks [RFC1918]
static filter_st lan_ranges[] = {
	{ 0x0000000a, 0x000000ff }, // 10.0.0.0/8
	{ 0x000010ac, 0x0000f0ff }, // 172.16.0.0/12
	{ 0x0000a8c0, 0x0000ffff }  // 192.168.0.0/16
};

bool IsGoodIP(uint32 ip, bool filterLAN) noexcept
{
	for (unsigned int i = 0; i < itemsof(reserved_ranges); ++i) {
		if (((ip ^ reserved_ranges[i].addr) & reserved_ranges[i].mask) == 0) {
			return false;
		}
	}

	return !(filterLAN && IsLanIP(ip));
}

bool IsGoodServerIP(uint32 ip, bool filterLAN) noexcept
{
	return (!filterLAN && IsLoopbackIP(ip)) || IsGoodIP(ip, filterLAN);
}

bool IsLanIP(uint32_t ip) noexcept
{
	for (unsigned int i = 0; i < itemsof(lan_ranges); ++i) {
		if (((ip ^ lan_ranges[i].addr) & lan_ranges[i].mask) == 0) {
			return true;
		}
	}
	return false;
}

bool IsLoopbackIP(uint32_t ip) noexcept
{
	// 127.0.0.0/8 in anti-host order -- the first octet (127 = 0x7f) sits in the low byte of
	// the uint32, matching the encoding StringIPtoUint32 / IsLanIP use.
	return (ip & 0x000000ff) == 0x0000007f;
}

bool IsLinkLocalIP(uint32_t ip) noexcept
{
	// 169.254.0.0/16 in anti-host order: 169 = 0xa9 in the low byte,
	// 254 = 0xfe in the next byte → 0x0000fea9 with mask 0x0000ffff.
	return (ip & 0x0000ffff) == 0x0000fea9;
}
