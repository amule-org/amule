#include <muleunit/test.h>
#include "UploadCallbackPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(UploadCallbackPolicy)

TEST(UploadCallbackPolicy, OnlyTheSpentMarkerDecides)
{
	ASSERT_TRUE(MayStartUploadCallback(false));
	ASSERT_FALSE(MayStartUploadCallback(true));
}

TEST(UploadCallbackPolicy, InFlightBoundary)
{
	ASSERT_TRUE(IsUploadCallbackInFlight(100, 100 + kUploadCallbackInFlightMs - 1));
	ASSERT_FALSE(IsUploadCallbackInFlight(100, 100 + kUploadCallbackInFlightMs));
	ASSERT_FALSE(IsUploadCallbackInFlight(0, 100));
	ASSERT_FALSE(IsUploadCallbackInFlight(100, 99));
}

TEST(UploadCallbackPolicy, InFlightCeilingIncludesAlternationSlot)
{
	ASSERT_EQUALS(0U, MaxInflightUploadCallbacks(0, false));
	ASSERT_EQUALS(1U, MaxInflightUploadCallbacks(0, true));
	ASSERT_EQUALS(3U, MaxInflightUploadCallbacks(3, false));
	ASSERT_EQUALS(4U, MaxInflightUploadCallbacks(3, true));
}

TEST(UploadCallbackPolicy, DirectCallbackThrottleBoundary)
{
	ASSERT_FALSE(IsDirectCallbackThrottled(0, 100, 1000));
	ASSERT_TRUE(IsDirectCallbackThrottled(100, 100, 1000));
	ASSERT_TRUE(IsDirectCallbackThrottled(100, 100 + 999, 1000));
	ASSERT_FALSE(IsDirectCallbackThrottled(100, 100 + 1000, 1000));
	ASSERT_FALSE(IsDirectCallbackThrottled(100, 99, 1000));
}

namespace
{
// A peer we can reach both ways: it takes a direct callback and is on our server.
SUploadCallbackRouteFacts PeerWithBothRoutes()
{
	SUploadCallbackRouteFacts facts;
	facts.canTakeCallback = true;
	facts.directCallback = true;
	facts.onOurServer = true;
	return facts;
}
} // namespace

TEST(UploadCallbackPolicy, DirectCallbackComesFirst)
{
	ASSERT_TRUE(UploadCallbackRoute(PeerWithBothRoutes()) == EContactRouteReport::DirectUdpCallback);
}

TEST(UploadCallbackPolicy, KadOnlyPeerGetsTheDirectCallback)
{
	SUploadCallbackRouteFacts facts = PeerWithBothRoutes();
	facts.onOurServer = false;
	ASSERT_TRUE(UploadCallbackRoute(facts) == EContactRouteReport::DirectUdpCallback);
	facts.directCallback = false;
	ASSERT_TRUE(UploadCallbackRoute(facts) == EContactRouteReport::None);
}

TEST(UploadCallbackPolicy, ThrottledDirectCallbackFallsBackToTheServer)
{
	SUploadCallbackRouteFacts facts = PeerWithBothRoutes();
	facts.directThrottled = true;
	ASSERT_TRUE(UploadCallbackRoute(facts) == EContactRouteReport::ServerCallback);
	facts.onOurServer = false;
	ASSERT_TRUE(UploadCallbackRoute(facts) == EContactRouteReport::None);
}

TEST(UploadCallbackPolicy, NoRouteWhenThePeerCannotReachUs)
{
	SUploadCallbackRouteFacts facts = PeerWithBothRoutes();
	facts.canTakeCallback = false;
	ASSERT_TRUE(UploadCallbackRoute(facts) == EContactRouteReport::None);
}
