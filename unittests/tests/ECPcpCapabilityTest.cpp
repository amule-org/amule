//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
//

//
// Forwarding-capability seam shared by the real CEC_Prefs_Packet constructor
// (emission, core side) and its CLIENT_GUI apply path (decoding, remote GUI side).
// The suite exercises the real ec library — actual CECTag trees, actual CECPacket
// wire serialization through CECMemSocket and actual decoding through the real
// reader — so the wire shape tested is the wire shape production produces.
//
// This source is compiled twice from the same build tree: once unmodified (base)
// and once as a capable core-build shape (ENABLE_UPNP + ENABLE_IPV6 forced on the
// command line when the tree itself does not provide them, no CLIENT_GUI). Expected
// values are derived from the compiled configuration, so both binaries pin the
// config→wire mapping of their own shape instead of assuming the current tree's.
//

#include <muleunit/test.h>

#include <ec/cpp/ECForwardingCapabilities.h>
#include <ec/cpp/ECMemSocket.h>
#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECSpecialTags.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(ECPcpCapability)

namespace
{

// Expected capability of the shape this binary was compiled in. Mirrors the
// production condition in ECForwardingCapabilities.h on purpose: the duplication
// pins the config→wire mapping against accidental condition changes.
#if defined(ENABLE_UPNP)
const bool kExpectUpnp = true;
#else
const bool kExpectUpnp = false;
#endif
#if defined(ENABLE_IPV6) && !defined(CLIENT_GUI) && \
	(defined(__linux__) || defined(__APPLE__) || defined(_WIN32))
const bool kExpectPcp = true;
#else
const bool kExpectPcp = false;
#endif

unsigned int CountTags(const CECTag &parent, ec_tagname_t name)
{
	unsigned int count = 0;
	for (const auto &tag : parent) {
		if (tag.GetTagName() == name) {
			++count;
		}
	}
	return count;
}

/// Round-trips packets through the real EC wire format: SendPacket drives the actual
/// packet writer and drains into the in-memory wire buffer; OnInput drives the actual
/// header/reader path and dispatches to OnPacketReceived. The socket owns received
/// packets (CSmartPtr in OnInput), so the GENERAL tag is copied out there.
class CRoundTripProbe : public CECMemSocket
{
public:
	/// Sends one packet through the real writer; captured bytes land in wire().
	void Send(const CECPacket &packet) { SendPacket(&packet); }

	/// Decodes previously fed bytes through the real reader and returns the copied-out
	/// GENERAL tag of the first received SET_PREFERENCES packet (absent tag if none).
	CECTag Receive()
	{
		OnInput();
		return m_general;
	}

	/// Queues wire bytes for the next Receive() call.
	void FeedWire(const std::vector<unsigned char> &bytes)
	{
		m_input = bytes;
		m_consumed = 0;
	}

	bool received() const { return m_captured; }
	const std::vector<unsigned char> &wire() const { return m_wire; }

private:
	uint32 InternalWrite(const void *ptr, uint32 len) override
	{
		const unsigned char *p = static_cast<const unsigned char *>(ptr);
		m_wire.insert(m_wire.end(), p, p + len);
		return len;
	}

	uint32 InternalRead(void *buffer, uint32 length) override
	{
		const uint32 count = std::min(
			static_cast<uint32>(length), static_cast<uint32>(m_input.size() - m_consumed));
		if (count != 0) {
			std::memcpy(buffer, m_input.data() + m_consumed, count);
			m_consumed += count;
		}
		return count;
	}

	const CECPacket *OnPacketReceived(const CECPacket *packet, uint32) override
	{
		if (packet->GetOpCode() == EC_OP_SET_PREFERENCES) {
			if (const CECTag *general = packet->GetTagByName(EC_TAG_PREFS_GENERAL)) {
				m_general = *general;
				m_captured = true;
			}
		}
		return nullptr;
	}

	std::vector<unsigned char> m_wire;
	std::vector<unsigned char> m_input = std::vector<unsigned char>(4096, 0);
	size_t m_consumed = 0;
	CECTag m_general;
	bool m_captured = false;
};

/// Serializes a packet with the real writer and decodes the bytes back with the real
/// reader; returns the received GENERAL tag.
CECTag WireRoundTrip(const CECPacket &packet)
{
	CRoundTripProbe sender;
	sender.Send(packet);
	ASSERT_FALSE(sender.wire().empty());

	CRoundTripProbe receiver;
	receiver.FeedWire(sender.wire());
	return receiver.Receive();
}

bool s_upnpCalled = false;
bool s_upnpValue = false;
bool s_pcpCalled = false;
bool s_pcpValue = false;

void RecordUpnp(bool value)
{
	s_upnpCalled = true;
	s_upnpValue = value;
}

void RecordPcp(bool value)
{
	s_pcpCalled = true;
	s_pcpValue = value;
}

void ResetRecorders()
{
	s_upnpCalled = false;
	s_upnpValue = false;
	s_pcpCalled = false;
	s_pcpValue = false;
}

} // namespace

TEST(ECPcpCapability, CompiledCapabilityMatchesBuildFlags)
{
	ASSERT_EQUALS(kExpectUpnp, ECForwardingCapabilities::UpnpAvailable());
	ASSERT_EQUALS(kExpectPcp, ECForwardingCapabilities::PcpAvailable());
}

TEST(ECPcpCapability, ConstructorSeamEmitsBothDistinctCapabilityTags)
{
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	ECForwardingCapabilities::AddCapabilityTags(general);

	// Both signals are always emitted by 3.1+ daemons: false means "compiled out",
	// so a client can distinguish it from an old daemon predating the tag (absent).
	ASSERT_EQUALS(1u, CountTags(general, EC_TAG_GENERAL_UPNP_AVAILABLE));
	ASSERT_EQUALS(1u, CountTags(general, EC_TAG_GENERAL_PCP_AVAILABLE));

	const CECTag *upnp = general.GetTagByName(EC_TAG_GENERAL_UPNP_AVAILABLE);
	const CECTag *pcp = general.GetTagByName(EC_TAG_GENERAL_PCP_AVAILABLE);
	ASSERT_TRUE(upnp != nullptr);
	ASSERT_TRUE(pcp != nullptr);
	ASSERT_TRUE(upnp != pcp);
	ASSERT_TRUE(upnp->IsInt());
	ASSERT_TRUE(pcp->IsInt());
	ASSERT_EQUALS(kExpectUpnp, upnp->GetInt() != 0);
	ASSERT_EQUALS(kExpectPcp, pcp->GetInt() != 0);
}

TEST(ECPcpCapability, WireRoundTripPreservesCapabilityTagsAndUnrelatedPreferences)
{
	CECPacket packet(EC_OP_SET_PREFERENCES);
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	ECForwardingCapabilities::AddCapabilityTags(general);
	// An unrelated preference proves the round-trip is lossless, not capability-specific.
	general.AddTag(CECTag(EC_TAG_USER_NICK, wxString("tester")));
	packet.AddTag(general);

	CECTag received = WireRoundTrip(packet);
	ASSERT_EQUALS(1u, CountTags(received, EC_TAG_GENERAL_UPNP_AVAILABLE));
	ASSERT_EQUALS(1u, CountTags(received, EC_TAG_GENERAL_PCP_AVAILABLE));
	ASSERT_EQUALS(1u, CountTags(received, EC_TAG_USER_NICK));
	ASSERT_EQUALS(kExpectUpnp, received.GetTagByName(EC_TAG_GENERAL_UPNP_AVAILABLE)->GetInt() != 0);
	ASSERT_EQUALS(kExpectPcp, received.GetTagByName(EC_TAG_GENERAL_PCP_AVAILABLE)->GetInt() != 0);
	ASSERT_EQUALS(wxString("tester"), received.GetTagByName(EC_TAG_USER_NICK)->GetStringData());
}

TEST(ECPcpCapability, ApplySeamKeepsPcpAndUpnpSignalsIndependent)
{
	// PCP explicitly true; UPnP tag absent (a daemon without the UPnP signal).
	ResetRecorders();
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	general.AddTag(CECTag(EC_TAG_GENERAL_PCP_AVAILABLE, true));
	ECForwardingCapabilities::ApplyCapabilityTags(general, &RecordUpnp, &RecordPcp);
	ASSERT_TRUE(s_upnpCalled);
	ASSERT_TRUE(s_pcpCalled);
	ASSERT_EQUALS(false, s_upnpValue);
	ASSERT_EQUALS(true, s_pcpValue);

	// Mirror case: UPnP explicitly true; PCP absent.
	ResetRecorders();
	CECEmptyTag general2(EC_TAG_PREFS_GENERAL);
	general2.AddTag(CECTag(EC_TAG_GENERAL_UPNP_AVAILABLE, true));
	ECForwardingCapabilities::ApplyCapabilityTags(general2, &RecordUpnp, &RecordPcp);
	ASSERT_TRUE(s_upnpCalled);
	ASSERT_TRUE(s_pcpCalled);
	ASSERT_EQUALS(true, s_upnpValue);
	ASSERT_EQUALS(false, s_pcpValue);
}

TEST(ECPcpCapability, ApplySeamTreatsAbsentLegacyTagAsUnavailable)
{
	ResetRecorders();
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	ECForwardingCapabilities::ApplyCapabilityTags(general, &RecordUpnp, &RecordPcp);
	ASSERT_TRUE(s_upnpCalled);
	ASSERT_TRUE(s_pcpCalled);
	ASSERT_EQUALS(false, s_upnpValue);
	ASSERT_EQUALS(false, s_pcpValue);
}

TEST(ECPcpCapability, ApplySeamDistinguishesExplicitFalseFromCompiledOut)
{
	// Explicit false (a daemon with the feature compiled out) also disables, but the
	// tag was read: the setters must still have been called for both capabilities.
	ResetRecorders();
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	general.AddTag(CECTag(EC_TAG_GENERAL_UPNP_AVAILABLE, false));
	general.AddTag(CECTag(EC_TAG_GENERAL_PCP_AVAILABLE, false));
	ECForwardingCapabilities::ApplyCapabilityTags(general, &RecordUpnp, &RecordPcp);
	ASSERT_TRUE(s_upnpCalled);
	ASSERT_TRUE(s_pcpCalled);
	ASSERT_EQUALS(false, s_upnpValue);
	ASSERT_EQUALS(false, s_pcpValue);
}

TEST(ECPcpCapability, RoundTrippedPacketAppliesWithLegacyAbsenceSemantics)
{
	CECPacket packet(EC_OP_SET_PREFERENCES);
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	ECForwardingCapabilities::AddCapabilityTags(general);
	packet.AddTag(general);

	CECTag received = WireRoundTrip(packet);
	ResetRecorders();
	ECForwardingCapabilities::ApplyCapabilityTags(received, &RecordUpnp, &RecordPcp);
	ASSERT_TRUE(s_upnpCalled);
	ASSERT_TRUE(s_pcpCalled);
	ASSERT_EQUALS(kExpectUpnp, s_upnpValue);
	ASSERT_EQUALS(kExpectPcp, s_pcpValue);
}
