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

#include <muleunit/test.h>

#include "UdpThrottleCharge.h"

using namespace muleunit;

DECLARE_SIMPLE(UdpThrottleCharge)

TEST(UdpThrottleCharge, AnOrdinaryDatagramChargesItsWireBytes)
{
	ASSERT_EQUALS(1200u, ThrottledUdpBytes(false, 1200));
}

TEST(UdpThrottleCharge, ATransportDatagramChargesNothingBecauseItsStreamAlreadyPaid)
{
	ASSERT_EQUALS(0u, ThrottledUdpBytes(true, 1200));
}
