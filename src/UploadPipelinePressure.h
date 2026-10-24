// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UPLOADPIPELINEPRESSURE_H
#define UPLOADPIPELINEPRESSURE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Pure observation policy: no scheduling, allocation or socket operations.
// Busy means the transport reports a pending/blocked write, not proven network
// congestion. Multiple stages/reasons may be present at the same time.
class UploadPipelinePressure
{
public:
	static constexpr uint64_t kSampleMs = 500;
	static constexpr uint64_t kMaxSampleGapMs = 1500;
	enum Signal : std::size_t
	{
		Underfilled,
		ReadWait,
		TransportBusy,
		AdmissionWait,
		Deferred,
		StorageAdmission,
		ClientMemory,
		GlobalMemory,
		WorkersFull,
		CompletionsReady,
		SignalCount
	};
	struct Socket
	{
		uint64_t queuedPayload = 0;
		bool busy = false;
	};
	struct Input
	{
		uint64_t active = 0, requested = 0, pendingClients = 0;
		uint64_t readWait = 0, transportBusy = 0, ready = 0;
		uint64_t admissionWait = 0, deferred = 0, drained = 0;
		uint64_t readyBytes = 0, pendingReads = 0, oldestReadMs = 0;
		uint64_t chargedBytes = 0, wireRate = 0, limit = 0, wireWindowMs = 0;
		bool wireValid = false;
		uint64_t queuedJobs = 0, runningJobs = 0, finishedJobs = 0;
		uint64_t lastReadMs = 0, lastHandoffMs = 0, lastCompletedMs = 0;
		bool storageDenied = false, clientMemoryDenied = false, globalMemoryDenied = false;
		bool workersFull = false;

		void AddClient(bool requests, bool pending, bool retrying, const Socket &socket)
		{
			++active;
			requested += requests;
			pendingClients += pending;
			readyBytes += socket.queuedPayload;
			ready += socket.queuedPayload != 0 && !socket.busy;
			transportBusy += socket.busy;
			readWait += pending && socket.queuedPayload == 0 && !socket.busy;
			deferred += retrying;
			admissionWait += requests && !pending && !retrying && socket.queuedPayload == 0 &&
					 !socket.busy;
			drained += !requests && !pending && !retrying && socket.queuedPayload == 0 &&
				   !socket.busy;
		}
		bool HasDemand() const { return active > drained; }
	};
	struct Snapshot
	{
		bool valid = false;
		uint64_t sampledMs = 0;
		Input input;
		std::array<bool, SignalCount> signals{};
		std::array<uint64_t, SignalCount> ageMs{};
		bool sustainedUnderfill = false;
		bool persistentUnderfill = false;
	};

	struct Traffic
	{
		uint64_t rate = 0, windowMs = 0;
		bool valid = false;
	};
	// Fixed-space rate over the latest two sampling intervals (normally one
	// second). No dependency on UI timers or resettable session/display counters.
	Traffic ObserveTraffic(uint64_t total, uint64_t now)
	{
		if (!m_havePrevious || now <= m_previous.time || now - m_previous.time > kMaxSampleGapMs ||
			total < m_previous.bytes) {
			m_previous = { total, now };
			m_havePrevious = true;
			m_haveOlder = false;
			return {};
		}
		const auto start = m_haveOlder ? m_older : m_previous;
		const uint64_t elapsed = now - start.time;
		const long double rate = static_cast<long double>(total - start.bytes) * 1000 / elapsed;
		const uint64_t boundedRate =
			rate >= static_cast<long double>(std::numeric_limits<uint64_t>::max())
				? std::numeric_limits<uint64_t>::max()
				: static_cast<uint64_t>(rate);
		m_older = m_previous;
		m_previous = { total, now };
		m_haveOlder = true;
		return { boundedRate, elapsed, elapsed >= 1000 };
	}

	Snapshot Observe(const Input &input, uint64_t now)
	{
		// eMuleBB's material-gap idea, with the minimum margin bounded for
		// low configured rates. Compare differences without overflowing.
		const uint64_t margin = std::min(input.limit / 2,
			std::max(uint64_t{ 1024 }, input.limit / 50 + (input.limit % 50 != 0)));
		const bool underfilled = input.wireValid && input.HasDemand() && input.limit != 0 &&
					 input.wireRate < input.limit &&
					 input.limit - input.wireRate > margin;
		const std::array<bool, SignalCount> signals{ underfilled,
			input.readWait != 0,
			input.transportBusy != 0,
			input.admissionWait != 0,
			input.deferred != 0,
			input.storageDenied,
			input.clientMemoryDenied,
			input.globalMemoryDenied,
			input.workersFull,
			input.finishedJobs != 0 };
		const bool continuous = m_snapshot.valid && now >= m_snapshot.sampledMs &&
					now - m_snapshot.sampledMs <= kMaxSampleGapMs;
		Snapshot next;
		next.valid = true;
		next.sampledMs = now;
		next.input = input;
		next.signals = signals;
		for (std::size_t i = 0; i < SignalCount; ++i) {
			if (!continuous || !signals[i] || !m_snapshot.signals[i]) {
				m_since[i] = now;
			}
			next.ageMs[i] = signals[i] ? now - m_since[i] : 0;
		}
		next.sustainedUnderfill = signals[Underfilled] && next.ageMs[Underfilled] >= 2000;
		next.persistentUnderfill = signals[Underfilled] && next.ageMs[Underfilled] >= 10000;
		m_snapshot = next;
		return next;
	}

private:
	struct TrafficPoint
	{
		uint64_t bytes = 0, time = 0;
	};
	TrafficPoint m_previous, m_older;
	bool m_havePrevious = false, m_haveOlder = false;
	Snapshot m_snapshot;
	std::array<uint64_t, SignalCount> m_since{};
};
#endif
