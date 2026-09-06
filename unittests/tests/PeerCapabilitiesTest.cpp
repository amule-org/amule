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

// The CT_MOD_MISCOPTIONS bit positions are wire format shared with eMuleAI,
// and getting one of them wrong has no runtime signal: aMule would claim a
// transport it does not implement, the peer would open a handshake, and the
// handshake would simply never complete. So the positions are pinned here as
// literal words rather than restated from the enum -- a test that reads the
// same enum it is checking cannot catch a renumbering.
//
// The reference is eMuleAI's UModMiscOptions union, srchybrid/Opcodes.h:710.
//
// CUpDownClient reaches theApp and cannot be linked into a unit test, which is
// why the capability model lives in a header of its own.

#include <muleunit/test.h>

#include <PeerCapabilities.h>

// The unknown-tag-tolerance test below reads a tag stream through exactly the
// classes CUpDownClient::ProcessHelloTypePacket() uses. CUpDownClient itself
// cannot be linked here, but the property that matters is a wire property, not
// a client one: an unknown tag must consume its own bytes.
#include <tags/ClientTags.h>
#include <tags/FileTags.h>
#include "MemFile.h"
#include "Tag.h"

#include <cstring>

using namespace muleunit;

DECLARE_SIMPLE(PeerCapabilities)

// Each bit, alone, at the position eMuleAI puts it. Literal words on purpose.
TEST(PeerCapabilities, BitPositionsAreExact)
{
	ASSERT_EQUALS(0x00000001u, (uint32_t)MOD_MISCOPT_EXTENDED_XS);
	ASSERT_EQUALS(0x00000002u, (uint32_t)MOD_MISCOPT_NAT_TRAVERSAL);
	ASSERT_EQUALS(0x00000004u, (uint32_t)MOD_MISCOPT_IPV6);
	ASSERT_EQUALS(0x00000008u, (uint32_t)MOD_MISCOPT_SERVING_BUDDY_PULL);
	ASSERT_EQUALS(0x00000010u, (uint32_t)MOD_MISCOPT_NAT_TRAVERSAL_QUIC);
	ASSERT_EQUALS(0x0000001Fu, MOD_MISCOPT_KNOWN_MASK);
}

// One decoded flag per bit, so a rotation of the enum shows up as a wrong
// accessor rather than as a still-passing mask test.
TEST(PeerCapabilities, EachBitDecodesToItsOwnAccessor)
{
	CPeerCapabilities caps;

	caps.SetFromWire(0x00000001u);
	ASSERT_TRUE(caps.SupportsExtendedSourceExchange());
	ASSERT_FALSE(caps.SupportsNatTraversal());
	ASSERT_FALSE(caps.SupportsIPv6());
	ASSERT_FALSE(caps.SupportsServingBuddyPull());
	ASSERT_FALSE(caps.SupportsNatTraversalQuic());

	caps.SetFromWire(0x00000002u);
	ASSERT_FALSE(caps.SupportsExtendedSourceExchange());
	ASSERT_TRUE(caps.SupportsNatTraversal());
	ASSERT_FALSE(caps.SupportsIPv6());
	ASSERT_FALSE(caps.SupportsServingBuddyPull());
	ASSERT_FALSE(caps.SupportsNatTraversalQuic());

	caps.SetFromWire(0x00000004u);
	ASSERT_FALSE(caps.SupportsExtendedSourceExchange());
	ASSERT_FALSE(caps.SupportsNatTraversal());
	ASSERT_TRUE(caps.SupportsIPv6());
	ASSERT_FALSE(caps.SupportsServingBuddyPull());
	ASSERT_FALSE(caps.SupportsNatTraversalQuic());

	caps.SetFromWire(0x00000008u);
	ASSERT_FALSE(caps.SupportsExtendedSourceExchange());
	ASSERT_FALSE(caps.SupportsNatTraversal());
	ASSERT_FALSE(caps.SupportsIPv6());
	ASSERT_TRUE(caps.SupportsServingBuddyPull());
	ASSERT_FALSE(caps.SupportsNatTraversalQuic());

	caps.SetFromWire(0x00000010u);
	ASSERT_FALSE(caps.SupportsExtendedSourceExchange());
	ASSERT_FALSE(caps.SupportsNatTraversal());
	ASSERT_FALSE(caps.SupportsIPv6());
	ASSERT_FALSE(caps.SupportsServingBuddyPull());
	ASSERT_TRUE(caps.SupportsNatTraversalQuic());
}

// The scenario from the spec delta: a peer that advertises IPv6 and QUIC NAT-T
// and nothing else. 0x14 == bit 2 | bit 4.
TEST(PeerCapabilities, PeerAdvertisingIPv6AndQuic)
{
	CPeerCapabilities caps;
	caps.SetFromWire(0x00000014u);

	ASSERT_TRUE(caps.SupportsIPv6());
	ASSERT_TRUE(caps.SupportsNatTraversalQuic());
	ASSERT_FALSE(caps.SupportsExtendedSourceExchange());
	ASSERT_FALSE(caps.SupportsNatTraversal());
	ASSERT_FALSE(caps.SupportsServingBuddyPull());
}

// Round-trip: the fixture encoding all five documented bits survives
// decode -> encode unchanged.
TEST(PeerCapabilities, RoundTripsEveryKnownBit)
{
	for (uint32_t bits = 0; bits <= MOD_MISCOPT_KNOWN_MASK; ++bits) {
		CPeerCapabilities caps;
		caps.SetFromWire(bits);
		ASSERT_EQUALS(bits, caps.KnownBits());
	}
}

// Reserved bits 5..31 carry no meaning yet. A peer setting bit 7 must not
// produce a capability, and must not survive a re-encode either -- otherwise
// aMule would relay a claim it cannot interpret.
TEST(PeerCapabilities, ReservedBitsAreMaskedOff)
{
	CPeerCapabilities caps;
	caps.SetFromWire(0x00000080u); // bit 7

	ASSERT_TRUE(caps.IsEmpty());
	ASSERT_EQUALS(0x00000000u, caps.KnownBits());
	ASSERT_FALSE(caps.SupportsExtendedSourceExchange());
	ASSERT_FALSE(caps.SupportsNatTraversal());
	ASSERT_FALSE(caps.SupportsIPv6());
	ASSERT_FALSE(caps.SupportsServingBuddyPull());
	ASSERT_FALSE(caps.SupportsNatTraversalQuic());

	// All reserved bits at once, plus every known bit: only the known five
	// come back out.
	caps.SetFromWire(0xFFFFFFFFu);
	ASSERT_EQUALS(MOD_MISCOPT_KNOWN_MASK, caps.KnownBits());
}

// The whole point of the change: aMule reads these capabilities but implements
// none of them, so it must advertise none of them. This assertion is expected
// to change exactly once per shipped feature -- and a change to it that is not
// accompanied by a shipped transport is the bug the spec warns about.
TEST(PeerCapabilities, AdvertisesNoUnimplementedCapability)
{
	ASSERT_EQUALS(0x00000000u, LocalAdvertisedModMiscOptions());
}

// Setters exist for the advertise side; they must land on the same bits the
// decoder reads, or the two halves drift.
TEST(PeerCapabilities, SetterAndDecoderAgree)
{
	CPeerCapabilities caps;

	caps.Set(MOD_MISCOPT_IPV6, true);
	caps.Set(MOD_MISCOPT_NAT_TRAVERSAL_QUIC, true);
	ASSERT_EQUALS(0x00000014u, caps.KnownBits());

	caps.Set(MOD_MISCOPT_IPV6, false);
	ASSERT_EQUALS(0x00000010u, caps.KnownBits());

	caps.Reset();
	ASSERT_TRUE(caps.IsEmpty());
}

// A peer that claims nothing produces an empty string, not a word. That is
// the contract the client details dialog hides its row on, so it is pinned
// here rather than left to the dialog: a version of this that returned
// "None" would put a permanent, meaningless row in front of nearly every
// user, and the dialog could not tell that apart from a real claim.
//
// Reserved bits go the same way: they are masked off, so a peer setting only
// bit 7 claims nothing and reads as nothing.
TEST(PeerCapabilities, ClaimingNothingDisplaysAsEmpty)
{
	CPeerCapabilities caps;
	ASSERT_TRUE(caps.GetDisplayText().IsEmpty());

	caps.SetFromWire(0x00000000u);
	ASSERT_TRUE(caps.GetDisplayText().IsEmpty());

	caps.SetFromWire(0x00000080u); // reserved bit 7 only
	ASSERT_TRUE(caps.GetDisplayText().IsEmpty());
}

// Each bit's name, one bit at a time, so the display table is pinned to the
// positions rather than to the order it happens to be written in. The names
// are marked for translation, so these are the msgids -- a test binary loads
// no catalog, and it is the pairing that matters here, not the wording.
TEST(PeerCapabilities, EachBitDisplaysItsOwnName)
{
	CPeerCapabilities caps;

	caps.SetFromWire(MOD_MISCOPT_EXTENDED_XS);
	ASSERT_EQUALS(wxString("Extended source exchange"), caps.GetDisplayText());

	caps.SetFromWire(MOD_MISCOPT_NAT_TRAVERSAL);
	ASSERT_EQUALS(wxString("NAT traversal (uTP)"), caps.GetDisplayText());

	caps.SetFromWire(MOD_MISCOPT_IPV6);
	ASSERT_EQUALS(wxString("IPv6"), caps.GetDisplayText());

	caps.SetFromWire(MOD_MISCOPT_SERVING_BUDDY_PULL);
	ASSERT_EQUALS(wxString("Buddy info pull"), caps.GetDisplayText());

	caps.SetFromWire(MOD_MISCOPT_NAT_TRAVERSAL_QUIC);
	ASSERT_EQUALS(wxString("NAT traversal (QUIC)"), caps.GetDisplayText());

	// All five at once: comma-separated, in table order, no trailing comma.
	caps.SetFromWire(MOD_MISCOPT_KNOWN_MASK);
	ASSERT_EQUALS(wxString("Extended source exchange, NAT traversal (uTP), IPv6, "
			       "Buddy info pull, NAT traversal (QUIC)"),
		caps.GetDisplayText());
}

// The "ip6" / "bi6" Kad tags carry a 128-bit address as 32 hex characters,
// big-endian -- eMuleAI writes them with CUInt128::ToHexString() and reads
// them back with strmd4(). Anything that is not exactly 32 hex characters is
// a malformed tag, not a shorter address.
TEST(PeerCapabilities, DecodesIPv6HexTag)
{
	uint8_t out[16];

	const char *loopback = "00000000000000000000000000000001";
	ASSERT_TRUE(DecodeIPv6HexTag(loopback, 32, out));
	for (int i = 0; i < 15; ++i) {
		ASSERT_EQUALS(0, (int)out[i]);
	}
	ASSERT_EQUALS(1, (int)out[15]);

	// 2001:db8::dead:beef -- mixed case accepted, byte order big-endian.
	const char *documented = "20010DB80000000000000000DeAdBeeF";
	ASSERT_TRUE(DecodeIPv6HexTag(documented, 32, out));
	ASSERT_EQUALS(0x20, (int)out[0]);
	ASSERT_EQUALS(0x01, (int)out[1]);
	ASSERT_EQUALS(0x0D, (int)out[2]);
	ASSERT_EQUALS(0xB8, (int)out[3]);
	ASSERT_EQUALS(0xDE, (int)out[12]);
	ASSERT_EQUALS(0xAD, (int)out[13]);
	ASSERT_EQUALS(0xBE, (int)out[14]);
	ASSERT_EQUALS(0xEF, (int)out[15]);
}

TEST(PeerCapabilities, RejectsMalformedIPv6HexTag)
{
	uint8_t out[16];

	ASSERT_FALSE(DecodeIPv6HexTag("", 0, out));
	ASSERT_FALSE(DecodeIPv6HexTag("0001", 4, out));                               // too short
	ASSERT_FALSE(DecodeIPv6HexTag("000000000000000000000000000000012", 33, out)); // too long
	ASSERT_FALSE(DecodeIPv6HexTag("0000000000000000000000000000000g", 32, out));  // not hex
	ASSERT_FALSE(DecodeIPv6HexTag(NULL, 32, out));

	// A space anywhere in the run is a malformed tag, not a shorter address.
	char spaced[33];
	memset(spaced, '0', 32);
	spaced[32] = '\0';
	spaced[17] = ' ';
	ASSERT_FALSE(DecodeIPv6HexTag(spaced, 32, out));
}

// Spec delta, "Unknown vendor tag": a tag the client does not recognise must be
// ignored and the handshake must continue as if it were absent.
//
// The failure this guards against is not the ignoring -- the switch in
// ProcessHelloTypePacket() has no default branch and has always fallen through
// for unknown ids. It is desynchronisation: the tag loop reads a fixed count of
// tags, so an unknown tag whose bytes are not fully consumed shifts every later
// tag by that much, and the hello then decodes as garbage with nothing logged.
// So what is pinned here is that a stream of known-unknown-known survives with
// the third tag's value intact.
TEST(PeerCapabilities, UnknownVendorTagDoesNotDesynchroniseTheStream)
{
	CMemFile stream;

	CTagVarInt written(CT_MOD_MISCOPTIONS, 0x00000014u, 32);
	written.WriteTagToFile(&stream);

	// 0xAB is inside the vendor range and defined by nothing aMule knows.
	CTagString unknown(static_cast<uint8_t>(0xAB), "some future value");
	unknown.WriteTagToFile(&stream);

	CTagVarInt trailing(CT_EMULE_VERSION, 0x0A010203u, 32);
	trailing.WriteTagToFile(&stream);

	stream.Seek(0, wxFromStart);

	const CTag first(stream, true);
	ASSERT_EQUALS((int)CT_MOD_MISCOPTIONS, (int)first.GetNameID());
	CPeerCapabilities caps;
	caps.SetFromWire((uint32_t)first.GetInt());
	ASSERT_TRUE(caps.SupportsIPv6());
	ASSERT_TRUE(caps.SupportsNatTraversalQuic());

	// Read and discard, exactly as the handshake does for an id it has no
	// case for.
	const CTag ignored(stream, true);
	ASSERT_EQUALS(0xAB, (int)ignored.GetNameID());

	// The tag after the unknown one is still where it should be. This is the
	// assertion that fails if an unknown tag under-consumes.
	const CTag third(stream, true);
	ASSERT_EQUALS((int)CT_EMULE_VERSION, (int)third.GetNameID());
	ASSERT_EQUALS(0x0A010203u, (uint32_t)third.GetInt());

	// And the stream is fully consumed: no trailing bytes, so nothing
	// over-consumed either.
	ASSERT_EQUALS(stream.GetLength(), stream.GetPosition());
}

// The vendor tag ids and the Kad tag names are wire format shared with eMuleAI,
// and nothing at runtime notices a wrong one: aMule would write its IPv6
// address under an id the peer decodes as something else, or under a name the
// peer never looks up, and the handshake would carry on regardless. So the
// values are pinned here as literals -- an assertion that reads the same
// symbol it is checking cannot catch a renumbering or a renamed tag.
//
// The reference is eMuleAI's srchybrid/Opcodes.h and its Kad tag names.
TEST(PeerCapabilities, VendorTagIdsAndKadTagNamesAreExact)
{
	ASSERT_EQUALS(0xA0, (int)CT_EMULE_SERVINGBUDDYIPV6);
	ASSERT_EQUALS(0xAA, (int)CT_MOD_MISCOPTIONS);
	ASSERT_EQUALS(0xAD, (int)CT_MOD_YOUR_IP);
	ASSERT_EQUALS(0xAE, (int)CT_MOD_IP_V6);

	ASSERT_EQUALS(wxString(wxT("ip6")), wxString(TAG_IPV6));
	ASSERT_EQUALS(wxString(wxT("bi6")), wxString(TAG_SERVINGBUDDYIPV6));
}
