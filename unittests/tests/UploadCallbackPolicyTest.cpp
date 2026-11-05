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
