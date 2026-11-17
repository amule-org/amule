#include <muleunit/test.h>
#include "ForwardingCapability.h"

using namespace muleunit;
DECLARE_SIMPLE(ForwardingCapability)

TEST(ForwardingCapability, UpnpOnly)
{
	ASSERT_TRUE(forwarding::CanForward(true, false));
}

TEST(ForwardingCapability, PcpOnly)
{
	ASSERT_TRUE(forwarding::CanForward(false, true));
}

TEST(ForwardingCapability, Both)
{
	ASSERT_TRUE(forwarding::CanForward(true, true));
}

TEST(ForwardingCapability, Neither)
{
	ASSERT_FALSE(forwarding::CanForward(false, false));
}

TEST(ForwardingCapability, LegacyDaemonWithoutPcpCapability)
{
	// The absent optional EC tag is represented by pcpAvailable=false.
	ASSERT_TRUE(forwarding::CanForward(true, false));
	ASSERT_FALSE(forwarding::CanForward(false, false));
}
