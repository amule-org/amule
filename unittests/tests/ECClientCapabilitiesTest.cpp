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

// EC_TAG_CLIENT_MOD_CAPABILITIES is the only way to observe a peer's
// negotiated vendor capabilities without a display: the Client Details dialog
// exists in `amule` and `amulegui`, and neither starts without an X server.
// So this tag is what makes that display verifiable, and its encoding is
// pinned here rather than left to a GUI nobody can open in CI.
//
// CEC_UpDownClient_Tag's only constructor takes a live CUpDownClient, which
// reaches theApp and cannot be linked into a unit test. Its accessor is a
// one-line call to CECTag::AssignIfExist, which is public, so the tag tree is
// built directly here and read through exactly the call the accessor makes.
// What that leaves untested is the one line of forwarding, not the encoding.

#include <muleunit/test.h>

#include <ec/cpp/ECTag.h>
#include <ECCodes.h>
#include <ECTagTypes.h>

#include <PeerCapabilities.h>

using namespace muleunit;

DECLARE_SIMPLE(ECClientCapabilities)

// The tag code is wire format. A literal, not a restatement of the generated
// header it checks.
TEST(ECClientCapabilities, TagCodeIsExact)
{
	ASSERT_EQUALS(0x0632, (int)EC_TAG_CLIENT_MOD_CAPABILITIES);
}

// The core emits CPeerCapabilities::KnownBits() and an EC client reads it back
// with AssignIfExist. Nothing in between may alter the value.
//
// Deliberately no assertion on the tag's *type*: libec narrows an integer tag
// to the smallest width that holds it, so a word of 0x14 travels as a uint8
// and GetInt() widens it back. Pinning the type here would pin that narrowing
// rather than the protocol, and would break the first time a capability bit
// above 8 is defined.
TEST(ECClientCapabilities, WordSurvivesTheRoundTrip)
{
	for (uint32_t bits = 0; bits <= MOD_MISCOPT_KNOWN_MASK; ++bits) {
		CPeerCapabilities caps;
		caps.SetFromWire(bits);

		CECTag client(EC_TAG_CLIENT, (uint32_t)1);
		client.AddTag(CECTag(EC_TAG_CLIENT_MOD_CAPABILITIES, caps.KnownBits()));

		uint32_t received = 0xDEADBEEF;
		ASSERT_TRUE(client.AssignIfExist(EC_TAG_CLIENT_MOD_CAPABILITIES, received));
		ASSERT_EQUALS(bits, received);

		// And it decodes on the far side to the same capability set.
		CPeerCapabilities mirrored;
		mirrored.SetFromWire(received);
		ASSERT_EQUALS(caps.KnownBits(), mirrored.KnownBits());
	}
}

// The spec delta requires reserved bits to be masked off and no capability
// inferred from them. That masking happens in the core, before the tag is
// built, so a reserved bit a peer set must not reach an EC client at all --
// an EC client is then free to be ignorant of which bits are defined.
TEST(ECClientCapabilities, ReservedBitsNeverCrossTheProtocol)
{
	CPeerCapabilities caps;
	caps.SetFromWire(0xFFFFFFFFu);

	CECTag client(EC_TAG_CLIENT, (uint32_t)1);
	client.AddTag(CECTag(EC_TAG_CLIENT_MOD_CAPABILITIES, caps.KnownBits()));

	uint32_t received = 0;
	ASSERT_TRUE(client.AssignIfExist(EC_TAG_CLIENT_MOD_CAPABILITIES, received));
	ASSERT_EQUALS(MOD_MISCOPT_KNOWN_MASK, received);
	ASSERT_EQUALS(0x00000000u, received & ~MOD_MISCOPT_KNOWN_MASK);
}

// Additive tag, so there are three states and not two: a peer that advertised
// nothing (word 0, tag present) and a daemon too old to send the tag at all
// must not read the same. The reference form of the accessor returns a bool
// precisely so a client can tell them apart; a caller that used the
// value-returning overload would collapse both to 0.
TEST(ECClientCapabilities, AbsentTagIsDistinctFromAnEmptyWord)
{
	// Old daemon: no tag.
	CECTag oldDaemon(EC_TAG_CLIENT, (uint32_t)1);
	uint32_t target = 0xABCDEF01;
	ASSERT_FALSE(oldDaemon.AssignIfExist(EC_TAG_CLIENT_MOD_CAPABILITIES, target));
	// Untouched, so the caller's own default survives.
	ASSERT_EQUALS(0xABCDEF01u, target);

	// Current daemon, peer advertised nothing.
	CECTag noCapabilities(EC_TAG_CLIENT, (uint32_t)1);
	noCapabilities.AddTag(CECTag(EC_TAG_CLIENT_MOD_CAPABILITIES, (uint32_t)0));
	target = 0xABCDEF01;
	ASSERT_TRUE(noCapabilities.AssignIfExist(EC_TAG_CLIENT_MOD_CAPABILITIES, target));
	ASSERT_EQUALS(0x00000000u, target);
}

// The dialog and the EC tag must not drift, so both read the same word through
// the same class. This pins the pairing: the text an EC client would render
// from the received word is the text the GUI renders from the live client.
TEST(ECClientCapabilities, DisplayTextMatchesOnBothSidesOfTheProtocol)
{
	CPeerCapabilities core;
	core.SetFromWire(MOD_MISCOPT_IPV6 | MOD_MISCOPT_NAT_TRAVERSAL_QUIC);

	CECTag client(EC_TAG_CLIENT, (uint32_t)1);
	client.AddTag(CECTag(EC_TAG_CLIENT_MOD_CAPABILITIES, core.KnownBits()));

	uint32_t received = 0;
	ASSERT_TRUE(client.AssignIfExist(EC_TAG_CLIENT_MOD_CAPABILITIES, received));

	CPeerCapabilities remote;
	remote.SetFromWire(received);

	ASSERT_EQUALS(core.GetDisplayText(), remote.GetDisplayText());
	// The names are translated, so this is the untranslated msgid -- which is
	// what a test binary with no catalog loaded renders.
	ASSERT_EQUALS(wxString("IPv6, NAT traversal (QUIC)"), remote.GetDisplayText());
}
