#include <muleunit/test.h>
#include "NatRendezvousInitiatorPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(NatRendezvousInitiatorPolicy)

TEST(NatRendezvousInitiatorPolicy, AllowsOnlyFullyEstablishedRequest)
{
	const SNatRendezvousInitiatorFacts facts{ true, true, 0x1234, 7, 7, 7, true, true, true, true };
	ASSERT_TRUE(CanInitiateNatRendezvous(facts));
}

TEST(NatRendezvousInitiatorPolicy, DeniesEachMissingRequirement)
{
	const SNatRendezvousInitiatorFacts accepted{ true, true, 0x1234, 7, 7, 7, true, true, true, true };
	for (unsigned bit = 0; bit < 10; ++bit) {
		auto facts = accepted;
		switch (bit) {
		case 0:
			facts.localLowId = false;
			break;
		case 1:
			facts.targetLowId = false;
			break;
		case 2:
			facts.targetIdentity = 0;
			break;
		case 3:
			facts.currentServerContext = 8;
			break;
		case 4:
			facts.targetServerContext = 8;
			break;
		case 5:
			facts.servingBuddyServerContext = 8;
			break;
		case 6:
			facts.servingBuddyConnected = false;
			break;
		case 7:
			facts.servingBuddyHighId = false;
			break;
		case 8:
			facts.servingBuddyServerVerified = false;
			break;
		case 9:
			facts.traversalCapabilityAdvertised = false;
			break;
		}
		ASSERT_FALSE(CanInitiateNatRendezvous(facts));
	}
}
