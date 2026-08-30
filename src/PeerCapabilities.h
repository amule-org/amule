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

#ifndef PEERCAPABILITIES_H
#define PEERCAPABILITIES_H

#include <cstddef>
#include <cstdint>

#include <wx/intl.h>   // Needed for _()
#include <wx/string.h> // Needed for wxString

/**
 * eMuleAI vendor capability bits, carried in the CT_MOD_MISCOPTIONS (0xAA)
 * handshake tag.
 *
 * These positions are wire format. They mirror eMuleAI's UModMiscOptions
 * union field for field, least significant bit first, because that is the
 * order the reference implementation lays that union out on the wire.
 *
 * A one-bit offset here has no runtime signal. aMule would claim a transport
 * it does not have, the peer would open a handshake, and that handshake would
 * simply never complete -- nothing logs, nothing fails, the source is just
 * quietly unusable. PeerCapabilitiesTest pins each value as a literal word so
 * a renumbering cannot pass.
 */
enum EModMiscOptions : uint32_t
{
	//! Extended source exchange with variable source info.
	MOD_MISCOPT_EXTENDED_XS = 1u << 0,
	//! Legacy uTP NAT traversal (simple UDP traversal through NATs).
	MOD_MISCOPT_NAT_TRAVERSAL = 1u << 1,
	//! IPv6 support.
	MOD_MISCOPT_IPV6 = 1u << 2,
	//! Vendor buddy-info pull.
	MOD_MISCOPT_SERVING_BUDDY_PULL = 1u << 3,
	//! QUIC NAT-T data transport.
	MOD_MISCOPT_NAT_TRAVERSAL_QUIC = 1u << 4
};

//! Bits 0-4 are defined. Bits 5-31 are reserved and travel as zero in both
//! directions: they are masked out of what a peer sends, so no capability can
//! be inferred from them, and never set in what aMule sends.
constexpr uint32_t MOD_MISCOPT_KNOWN_MASK = 0x0000001Fu;

/**
 * What a peer told us it can do.
 *
 * Read-only as far as this change is concerned: aMule records the peer's
 * claims so later changes in this set can act on them, and advertises nothing
 * new itself. See LocalAdvertisedModMiscOptions().
 */
class CPeerCapabilities
{
public:
	CPeerCapabilities() = default;

	//! Decode a CT_MOD_MISCOPTIONS word. Reserved bits are discarded here,
	//! once, rather than at each query site.
	void SetFromWire(uint32_t bits) { m_bits = bits & MOD_MISCOPT_KNOWN_MASK; }

	//! The peer's recorded capability bits, already limited to the known set
	//! by SetFromWire(). For display and logging, not for re-encoding.
	uint32_t KnownBits() const { return m_bits; }

	void Reset() { m_bits = 0; }
	bool IsEmpty() const { return m_bits == 0; }

	bool Has(EModMiscOptions bit) const { return (m_bits & bit) != 0; }

	void Set(EModMiscOptions bit, bool enabled)
	{
		if (enabled) {
			m_bits |= bit;
		} else {
			m_bits &= ~static_cast<uint32_t>(bit);
		}
	}

	bool SupportsExtendedSourceExchange() const { return Has(MOD_MISCOPT_EXTENDED_XS); }
	bool SupportsNatTraversal() const { return Has(MOD_MISCOPT_NAT_TRAVERSAL); }
	bool SupportsIPv6() const { return Has(MOD_MISCOPT_IPV6); }
	bool SupportsServingBuddyPull() const { return Has(MOD_MISCOPT_SERVING_BUDDY_PULL); }
	bool SupportsNatTraversalQuic() const { return Has(MOD_MISCOPT_NAT_TRAVERSAL_QUIC); }

	/**
	 * The capability word as a comma-separated list, for client details.
	 *
	 * The capability names are protocol feature names, not prose, so they
	 * are not translated -- only the empty case is. A peer that sent no
	 * CT_MOD_MISCOPTIONS tag and one that sent an all-zero word are the
	 * same state and read the same way, because eMuleAI omits the tag when
	 * the word is zero, exactly as aMule does.
	 *
	 * Lives here rather than on CUpDownClient because the core client and
	 * the remote GUI's EC mirror both need it, and two copies of this table
	 * would drift the moment a bit is added.
	 */
	wxString GetDisplayText() const
	{
		if (IsEmpty()) {
			return _("None");
		}

		static const struct
		{
			EModMiscOptions bit;
			const char *name;
		} names[] = {
			{ MOD_MISCOPT_EXTENDED_XS, "Extended SX" },
			{ MOD_MISCOPT_NAT_TRAVERSAL, "uTP NAT-T" },
			{ MOD_MISCOPT_IPV6, "IPv6" },
			{ MOD_MISCOPT_SERVING_BUDDY_PULL, "Buddy pull" },
			{ MOD_MISCOPT_NAT_TRAVERSAL_QUIC, "QUIC NAT-T" },
		};

		wxString text;
		for (const auto &entry : names) {
			if (Has(entry.bit)) {
				if (!text.IsEmpty()) {
					text += ", ";
				}
				text += entry.name;
			}
		}
		return text;
	}

private:
	uint32_t m_bits = 0;
};

/**
 * The CT_MOD_MISCOPTIONS word aMule puts in its own handshake.
 *
 * Zero, and deliberately so: none of the five features exists in this tree
 * yet. Recognising a capability and implementing it are separate changes, and
 * advertising one aMule does not have is worse than advertising nothing --
 * the peer opens a handshake that cannot complete and neither side logs a
 * reason. Each bit turns on in the change that ships its transport.
 *
 * Because the word is zero, no CT_MOD_MISCOPTIONS tag is emitted at all: an
 * absent tag and an all-zero one mean the same thing to eMuleAI, and the
 * absent one costs no bytes.
 */
constexpr uint32_t LocalAdvertisedModMiscOptions()
{
	return 0;
}

/**
 * Decode the 128-bit address carried by the "ip6" and "bi6" Kad tags.
 *
 * The wire form is exactly 32 hexadecimal characters, most significant byte
 * first, upper or lower case -- the reference implementation writes them with
 * its 128-bit hex formatter and reads them back with its MD4-hash parser, so
 * the length is fixed and not a prefix. Anything else is a malformed tag, not
 * a shorter address, and the caller must ignore it.
 *
 * @param text  the tag's string value; may be NULL.
 * @param length  its length in characters.
 * @param out  receives 16 bytes, big-endian. Untouched unless true is
 *             returned.
 * @return true when the whole run decoded.
 */
inline bool DecodeIPv6HexTag(const char *text, size_t length, uint8_t *out)
{
	if (text == nullptr || out == nullptr || length != 32) {
		return false;
	}

	uint8_t decoded[16];
	for (size_t i = 0; i < 16; ++i) {
		uint8_t byte = 0;
		for (size_t nibbleIndex = 0; nibbleIndex < 2; ++nibbleIndex) {
			const char c = text[(i * 2) + nibbleIndex];
			uint8_t nibble;
			if (c >= '0' && c <= '9') {
				nibble = static_cast<uint8_t>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				nibble = static_cast<uint8_t>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				nibble = static_cast<uint8_t>(c - 'A' + 10);
			} else {
				return false;
			}
			byte = static_cast<uint8_t>((byte << 4) | nibble);
		}
		decoded[i] = byte;
	}

	for (size_t i = 0; i < 16; ++i) {
		out[i] = decoded[i];
	}
	return true;
}

#endif // PEERCAPABILITIES_H
