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
// If not, see <https://www.gnu.org/licenses/>.
//

//
// Capability-tag decoding against the real ec library: actual CECTag trees and
// the actual reader used by the CLIENT_GUI apply path, so the shapes tested are
// the shapes production produces. Three cases cover the wire: tag true, tag
// false, and tag absent.
//

#include <muleunit/test.h>

#include <ec/cpp/ECForwardingCapabilities.h>
#include <ec/cpp/ECSpecialTags.h>

using namespace muleunit;

DECLARE_SIMPLE(ECPcpCapability)

namespace
{
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

TEST(ECPcpCapability, TrueTagEnablesBothCapabilities)
{
	ResetRecorders();
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	general.AddTag(CECTag(EC_TAG_GENERAL_UPNP_AVAILABLE, true));
	general.AddTag(CECTag(EC_TAG_GENERAL_PCP_AVAILABLE, true));
	ECForwardingCapabilities::ApplyCapabilityTags(general, &RecordUpnp, &RecordPcp);
	ASSERT_TRUE(s_upnpCalled);
	ASSERT_TRUE(s_pcpCalled);
	ASSERT_TRUE(s_upnpValue);
	ASSERT_TRUE(s_pcpValue);
}

TEST(ECPcpCapability, FalseTagDisablesBothCapabilities)
{
	// Explicit false (a daemon with the feature compiled out) also disables, but
	// the tag was read: the setters must still have been called for both.
	ResetRecorders();
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	general.AddTag(CECTag(EC_TAG_GENERAL_UPNP_AVAILABLE, false));
	general.AddTag(CECTag(EC_TAG_GENERAL_PCP_AVAILABLE, false));
	ECForwardingCapabilities::ApplyCapabilityTags(general, &RecordUpnp, &RecordPcp);
	ASSERT_TRUE(s_upnpCalled);
	ASSERT_TRUE(s_pcpCalled);
	ASSERT_FALSE(s_upnpValue);
	ASSERT_FALSE(s_pcpValue);
}

TEST(ECPcpCapability, AbsentTagMeansUnavailable)
{
	// A daemon predating a tag omits it (the PCP tag is new in this revision);
	// absence means unavailable, and the setters still run for both capabilities.
	ResetRecorders();
	CECEmptyTag general(EC_TAG_PREFS_GENERAL);
	ECForwardingCapabilities::ApplyCapabilityTags(general, &RecordUpnp, &RecordPcp);
	ASSERT_TRUE(s_upnpCalled);
	ASSERT_TRUE(s_pcpCalled);
	ASSERT_FALSE(s_upnpValue);
	ASSERT_FALSE(s_pcpValue);
}
