// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UPLOADREADDIAGNOSTICS_H
#define UPLOADREADDIAGNOSTICS_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <utility>

// A request owns its timestamps. The pool publishes them with the completion,
// so workers never share a diagnostics lock with the coordinator or sockets.
struct UploadReadTiming
{
	uint64_t submitted = 0;
	uint64_t started = 0;
	uint64_t finished = 0;
	bool enabled = true;

	template <typename Work, typename Clock> void Run(Work work, Clock clock)
	{
		if (!enabled) {
			work();
			return;
		}
		started = clock();
		struct Finish
		{
			uint64_t &stamp;
			Clock &clock;
			~Finish() { stamp = clock(); }
		} finish{ finished, clock };
		work();
	}
};

// Coordinator-owned, constant-space interval counters. Pressure is sampled once
// per sweep: milliseconds during which at least one admission was last observed
// denied, NOT summed client wait time. Reasons may overlap. A worker slot stays
// occupied until its completion is taken, including queued/finished work.
class UploadReadDiagnostics
{
public:
	enum Pressure : std::size_t
	{
		Storage,
		ClientMemory,
		GlobalMemory,
		Workers,
		PressureCount
	};
	using PressureMask = std::array<bool, PressureCount>;
	struct Latency
	{
		uint64_t count = 0;
		uint64_t totalMs = 0;
		uint64_t maxMs = 0;
		uint64_t overSecond = 0;
		void Add(uint64_t ms)
		{
			++count;
			totalMs += ms;
			maxMs = std::max(maxMs, ms);
			overSecond += ms >= 1000;
		}
	};
	struct Snapshot
	{
		Latency queue, work, completion, close;
		uint64_t failed = 0;
		uint64_t deferred = 0;
		std::array<uint64_t, PressureCount> denials{};
		std::array<uint64_t, PressureCount> pressureMs{};
	};

	void Completed(
		const UploadReadTiming &timing, uint64_t now, bool closeOnly, bool failed, bool deferred)
	{
		if (!timing.enabled) {
			return;
		}
		if (closeOnly) {
			m_stats.close.Add(timing.finished - timing.started);
			return;
		}
		m_stats.queue.Add(timing.started - timing.submitted);
		m_stats.work.Add(timing.finished - timing.started);
		m_stats.completion.Add(now - timing.finished);
		m_stats.failed += failed;
		m_stats.deferred += deferred;
	}
	void Denied(Pressure reason)
	{
		++m_stats.denials[reason];
		m_sweep[reason] = true;
	}
	void EndSweep(uint64_t now, bool workersFull)
	{
		Advance(now);
		m_pressure = m_sweep;
		m_pressure[Workers] = workersFull;
		m_sweep = {};
	}
	const PressureMask &LastPressure() const { return m_pressure; }
	Snapshot Take(uint64_t now)
	{
		Advance(now);
		return std::exchange(m_stats, Snapshot{});
	}

private:
	void Advance(uint64_t now)
	{
		if (m_sampled) {
			for (std::size_t i = 0; i < PressureCount; ++i) {
				if (m_pressure[i]) {
					m_stats.pressureMs[i] += now - m_lastSample;
				}
			}
		}
		m_sampled = true;
		m_lastSample = now;
	}
	bool m_sampled = false;
	uint64_t m_lastSample = 0;
	PressureMask m_pressure{}, m_sweep{};
	Snapshot m_stats;
};
#endif
