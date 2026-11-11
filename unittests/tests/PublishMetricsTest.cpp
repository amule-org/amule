//								-*- C++ -*-
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

#include <muleunit/test.h>
#include "kademlia/utils/PublishMetrics.h"
using namespace muleunit;
using namespace Kademlia;
DECLARE_SIMPLE(PublishMetrics)

TEST(PublishMetrics, TotalsAndChangedReports)
{
	CPublishMetrics metrics;
	CLookupTrace trace;
	trace.Query({ 1, 1 }, {}, 0);
	trace.Query({ 1, 2 }, {}, 0);
	trace.Query({ 2, 1 }, {}, 0);
	trace.ItemRequest({ 1, 1 });
	trace.ItemRequest({ 1, 2 });
	trace.ItemReply({ 1, 3 }); // Wrong port.
	trace.ItemReply({ 2, 1 }); // No store request.
	ASSERT_EQUALS(uint64_t(0), CPublishMetrics::Acknowledged(trace));
	trace.ItemReply({ 1, 1 });
	trace.ItemReply({ 1, 1 });
	trace.ItemReply({ 1, 2 });
	ASSERT_EQUALS(uint64_t(1), CPublishMetrics::Acknowledged(trace));
	ASSERT_FALSE(metrics.ReportDue(0));
	metrics.Attempt(false);
	metrics.Attempt(true);
	metrics.Start();
	metrics.Queued();
	metrics.Finish(trace, true, 20);
	metrics.Start();
	metrics.Finish({}, false, 30);
	metrics.Start();
	metrics.Finish({}, true, 40);
	ASSERT_EQUALS(uint64_t(2), metrics.attempts);
	ASSERT_EQUALS(uint64_t(1), metrics.startFailures);
	ASSERT_EQUALS(uint64_t(3), metrics.starts);
	ASSERT_EQUALS(uint64_t(1), metrics.queued);
	ASSERT_EQUALS(uint64_t(1), metrics.acknowledged);
	ASSERT_EQUALS(uint64_t(2), metrics.finished);
	ASSERT_EQUALS(uint64_t(1), metrics.cancelled);
	ASSERT_EQUALS(uint64_t(1), metrics.zeroAcknowledgements);
	ASSERT_EQUALS(uint64_t(60), metrics.durationMs);
	ASSERT_EQUALS(uint64_t(40), metrics.maxDurationMs);
	ASSERT_FALSE(metrics.ReportDue(59999));
	ASSERT_TRUE(metrics.ReportDue(60000));
	ASSERT_FALSE(metrics.ReportDue(120000)); // Idle, including stopped Kad.
	metrics.LookupQueued(1, 120000);
	metrics.LookupQueued(1, 120001);
	metrics.LookupQueued(2, 120002);
	ASSERT_EQUALS(uint64_t(2), metrics.maxRequestsPerIP60s);
	ASSERT_TRUE(metrics.ReportDue(180000));
	metrics.LookupQueued(1, 180002); // Both old IP 1 samples have expired.
	metrics.LookupQueued(1, 180002);
	ASSERT_EQUALS(uint64_t(2), metrics.maxRequestsPerIP60s);
	for (size_t i = 0; i < CPublishMetrics::MaxRateSamples - 1; ++i) {
		metrics.LookupQueued(3, 180002);
	}
	ASSERT_EQUALS(uint64_t(1), metrics.omittedRateSamples);
	ASSERT_TRUE(metrics.ReportDue(240002));
	metrics.LookupQueued(4, 240003); // Neither the peak nor any logged counter changes.
	ASSERT_FALSE(metrics.ReportDue(300002));
}
