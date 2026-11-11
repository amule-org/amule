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
#include <thread>
using namespace muleunit;
using namespace Kademlia;
DECLARE_SIMPLE(PublishMetrics)

TEST(PublishMetrics, AcknowledgementsRequireAQueuedDestinationAndAreUniquePerIP)
{
	CSourcePublishTrace trace;
	trace.Start(100);
	ASSERT_FALSE(trace.Acknowledge(1, 4672));
	trace.Queued(1, 4672);
	trace.Queued(1, 4672); // Retransmission is traffic, not another replica.
	trace.Queued(2, 4672);
	ASSERT_TRUE(trace.Acknowledge(1, 4672));
	ASSERT_FALSE(trace.Acknowledge(1, 4672));
	ASSERT_FALSE(trace.Acknowledge(3, 4672));
	ASSERT_TRUE(trace.Acknowledge(2, 4672));
	ASSERT_EQUALS(uint64_t(3), trace.queued);
	ASSERT_EQUALS(uint64_t(2), trace.acknowledged);
}

TEST(PublishMetrics, CompletionFailureCancellationAndDuration)
{
	CPublishMetrics metrics;
	metrics.Attempt(false);
	metrics.Attempt(true);
	metrics.Start();
	CSourcePublishTrace trace;
	trace.Start(100);
	trace.completed = true;
	trace.deadline = true;
	metrics.Finish(trace, 30100); // Includes retained state occupying the slot.
	metrics.Finish(trace, 40100); // Idempotent.
	ASSERT_EQUALS(uint64_t(2), metrics.attempts);
	ASSERT_EQUALS(uint64_t(1), metrics.startFailures);
	ASSERT_EQUALS(uint64_t(1), metrics.finished);
	ASSERT_EQUALS(uint64_t(1), metrics.zeroAcknowledgements);
	ASSERT_EQUALS(uint64_t(1), metrics.deadlines);
	ASSERT_EQUALS(uint64_t(30000), metrics.durationMs);
	ASSERT_EQUALS(uint64_t(30000), metrics.maxDurationMs);
	ASSERT_EQUALS(uint64_t(0), metrics.inFlight);
	CSourcePublishTrace interrupted;
	interrupted.Start(40000);
	metrics.Start();
	metrics.Finish(interrupted, 41000);
	ASSERT_EQUALS(uint64_t(1), metrics.cancelled);
	ASSERT_EQUALS(uint64_t(1), metrics.zeroAcknowledgements);
	CSourcePublishTrace accepted;
	accepted.Start(42000);
	accepted.Queued(1, 4672);
	accepted.Acknowledge(1, 4672);
	accepted.completed = true;
	metrics.Start();
	metrics.Finish(accepted, 43000);
	ASSERT_EQUALS(uint64_t(2), metrics.finished);
	ASSERT_EQUALS(uint64_t(1), metrics.zeroAcknowledgements);
	ASSERT_EQUALS(uint64_t(31000), metrics.durationMs);
}

TEST(PublishMetrics, DestinationMemoryIsBoundedAndReportsOmissions)
{
	CSourcePublishTrace trace;
	for (uint32_t ip = 1; ip <= CSourcePublishTrace::MaxDestinations + 1; ++ip) {
		trace.Queued(ip, 4672);
	}
	ASSERT_EQUALS(uint64_t(1), trace.omitted);
	ASSERT_FALSE(trace.Acknowledge(CSourcePublishTrace::MaxDestinations + 1, 4672));
	ASSERT_TRUE(trace.Acknowledge(1, 4672));
}

TEST(PublishMetrics, RollingMinuteUsesDestinationIPAndExpiresAtBoundary)
{
	CPublishTransportMetrics metrics;
	metrics.LookupSent(1, 100);
	metrics.LookupSent(2, 101);
	metrics.LookupSent(1, 59999);
	ASSERT_EQUALS(uint64_t(2), metrics.GetSnapshot(0).maxRequestsPerIP60s);
	metrics.LookupSent(1, 60100); // First request is outside (now-60s, now].
	ASSERT_EQUALS(uint64_t(2), metrics.GetSnapshot(0).maxRequestsPerIP60s);
	metrics.LookupSent(1, 60101);
	ASSERT_EQUALS(uint64_t(3), metrics.GetSnapshot(0).maxRequestsPerIP60s);
	metrics.LookupSent(1, 120102); // Entire old window expired; peak remains historical.
	ASSERT_EQUALS(uint64_t(3), metrics.GetSnapshot(0).maxRequestsPerIP60s);
}

TEST(PublishMetrics, RateMemoryBoundAndRecovery)
{
	CPublishTransportMetrics metrics;
	for (size_t i = 0; i < CPublishTransportMetrics::MaxRateSamples + 2; ++i) {
		metrics.LookupSent(1, 0);
	}
	ASSERT_EQUALS(uint64_t(2), metrics.GetSnapshot(0).omittedRateSamples);
	ASSERT_EQUALS(uint64_t(CPublishTransportMetrics::MaxRateSamples),
		metrics.GetSnapshot(0).maxRequestsPerIP60s);
	metrics.LookupSent(2, 60000);
	ASSERT_EQUALS(uint64_t(2), metrics.GetSnapshot(0).omittedRateSamples);
}

TEST(PublishMetrics, TransportSnapshotsAreSafeAcrossSenderAndMainThreads)
{
	CPublishTransportMetrics metrics;
	std::thread sender([&metrics] {
		for (int i = 0; i < 10000; ++i) {
			metrics.SourceFinished(true, 120);
		}
	});
	bool consistent = true;
	for (int i = 0; i < 10000; ++i) {
		const auto snapshot = metrics.GetSnapshot(0);
		consistent &= snapshot.sourcePackets * 120 == snapshot.sourceBytes;
	}
	sender.join();
	ASSERT_TRUE(consistent);
	ASSERT_EQUALS(uint64_t(10000), metrics.GetSnapshot(0).sourcePackets);
	ASSERT_EQUALS(uint64_t(1200000), metrics.GetSnapshot(0).sourceBytes);
}

TEST(PublishMetrics, SummariesAreOnceAMinuteIncludingIdleTime)
{
	CPublishMetrics metrics;
	ASSERT_FALSE(metrics.ReportDue(0));
	ASSERT_FALSE(metrics.ReportDue(59999));
	ASSERT_TRUE(metrics.ReportDue(60000));
	ASSERT_FALSE(metrics.ReportDue(60001));
	ASSERT_TRUE(metrics.ReportDue(120000));
}

TEST(PublishMetrics, DelayedCancellationDoesNotBecomeAZeroAckCompletion)
{
	CPublishMetrics metrics;
	metrics.Start();
	CSourcePublishTrace trace;
	trace.Start(0);
	trace.cancelled = true;
	trace.completed = true; // Deletion later happens in the ordinary lifetime sweep.
	metrics.Finish(trace, 15000);
	ASSERT_EQUALS(uint64_t(1), metrics.cancelled);
	ASSERT_EQUALS(uint64_t(0), metrics.finished);
	ASSERT_EQUALS(uint64_t(0), metrics.zeroAcknowledgements);
}

TEST(PublishMetrics, LargeLibraryDoesNotRequireRetainingFileHistory)
{
	CPublishMetrics metrics;
	for (int file = 0; file < 60000; ++file) {
		CSourcePublishTrace trace;
		metrics.Attempt(true);
		metrics.Start();
		trace.Start(0);
		trace.Queued(1, 4672);
		trace.Acknowledge(1, 4672);
		trace.completed = true;
		metrics.Finish(trace, 20000);
	}
	ASSERT_EQUALS(uint64_t(60000), metrics.finished);
	ASSERT_EQUALS(uint64_t(0), metrics.zeroAcknowledgements);
	ASSERT_EQUALS(uint64_t(0), metrics.inFlight);
	ASSERT_EQUALS(uint64_t(1200000000), metrics.durationMs);
}

TEST(PublishMetrics, ConcurrentTimestampOrderMustNotEraseTheRollingWindow)
{
	CPublishTransportMetrics metrics;
	metrics.LookupSent(1, 60000);
	metrics.LookupSent(1, 59999); // Another completion sampled time before acquiring the mutex.
	ASSERT_EQUALS(uint64_t(2), metrics.GetSnapshot(0).maxRequestsPerIP60s);
}

TEST(PublishMetrics, ShutdownMarksTheFinalTraceAsCancelled)
{
	CPublishMetrics metrics;
	CSourcePublishTrace trace;
	metrics.Start();
	trace.Start(100);
	metrics.Finish(trace, 200);
	ASSERT_TRUE(trace.cancelled);
	ASSERT_FALSE(trace.completed);
}

TEST(PublishMetrics, AcknowledgementsRequireTheContactedPortButDeduplicateAcrossPorts)
{
	CSourcePublishTrace trace;
	trace.Queued(1, 4000);
	trace.Queued(1, 4001);
	ASSERT_FALSE(trace.Acknowledge(1, 4002));
	ASSERT_EQUALS(uint64_t(0), trace.acknowledged);
	ASSERT_TRUE(trace.Acknowledge(1, 4001));
	ASSERT_FALSE(trace.Acknowledge(1, 4000));
	ASSERT_FALSE(trace.Acknowledge(1, 4001));
	ASSERT_EQUALS(uint64_t(1), trace.acknowledged);
}

TEST(PublishMetrics, IdleSnapshotsReleaseExpiredSamplesAndKeepTheHistoricalPeak)
{
	CPublishTransportMetrics metrics;
	metrics.LookupSent(1, 100);
	metrics.LookupSent(2, 101);
	ASSERT_EQUALS(size_t(2), metrics.GetSnapshot(60099).rateSamples);
	ASSERT_EQUALS(size_t(1), metrics.GetSnapshot(60100).rateSamples);
	ASSERT_EQUALS(size_t(0), metrics.GetSnapshot(60101).rateSamples);
	ASSERT_EQUALS(uint64_t(1), metrics.GetSnapshot(60101).maxRequestsPerIP60s);
	metrics.LookupSent(1, 60102);
	ASSERT_EQUALS(size_t(1), metrics.GetSnapshot(60102).rateSamples);
}

TEST(PublishMetrics, FailedNativeSendsDoNotCountPacketsOrBytes)
{
	CPublishTransportMetrics metrics;
	metrics.SourceFinished(false, 80); // Even a partial/failed native result is not a datagram.
	metrics.SourceFinished(true, 120);
	const auto snapshot = metrics.GetSnapshot(0);
	ASSERT_EQUALS(uint64_t(1), snapshot.sourcePackets);
	ASSERT_EQUALS(uint64_t(120), snapshot.sourceBytes);
	ASSERT_EQUALS(uint64_t(1), snapshot.failedSourceSends);
}
