#include <muleunit/test.h>
#include "UploadCallbackPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(UploadCallbackPolicy)

TEST(UploadCallbackPolicy, CandidateRequiresMarkAndClassicRoute)
{
	ASSERT_FALSE(IsUploadCallbackCandidate(false, false));
	ASSERT_FALSE(IsUploadCallbackCandidate(false, true));
	ASSERT_FALSE(IsUploadCallbackCandidate(true, false));
	ASSERT_TRUE(IsUploadCallbackCandidate(true, true));
}

TEST(UploadCallbackPolicy, SpentRecordRearmsAfterPeerRequest)
{
	ASSERT_TRUE(MayStartUploadCallback(false, 0, 0));
	ASSERT_FALSE(MayStartUploadCallback(true, 100, 100));
	ASSERT_TRUE(MayStartUploadCallback(true, 100, 101));
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

TEST(UploadCallbackPolicy, RespectsSharedTargetBudget)
{
	ASSERT_TRUE(UploadCallbackFitsTargetBudget(0));
	ASSERT_TRUE(UploadCallbackFitsTargetBudget(14));
	ASSERT_FALSE(UploadCallbackFitsTargetBudget(15));
	ASSERT_FALSE(UploadCallbackFitsTargetBudget(16));
}
