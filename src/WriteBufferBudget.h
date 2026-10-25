// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WRITEBUFFERBUDGET_H
#define WRITEBUFFERBUDGET_H

#include <cstdint>
#include <memory>
#include <mutex>

// Credits cover only buffered, unwritten payload. Already requested responses
// remain admissible during pressure; each fragment releases its own charge when
// written and freed, or discarded. Failed writes keep their charge for retry.
// Every part file lives in the one temp directory, so one limit covers them all.
class WriteBufferBudget
{
	struct State
	{
		std::mutex mutex;
		uint64_t used = 0;
		bool pressured = false;
	};

public:
	static constexpr uint64_t kLimit = 16 * 1024 * 1024;
	class Lease
	{
	public:
		~Lease()
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			m_state->used -= m_bytes;
		}
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;

		Lease(std::shared_ptr<State> state, uint64_t bytes)
		: m_state(std::move(state))
		, m_bytes(bytes)
		{
		}

	private:
		std::shared_ptr<State> m_state;
		uint64_t m_bytes;
	};
	using Credit = std::shared_ptr<Lease>;

	explicit WriteBufferBudget(uint64_t limit = kLimit)
	: m_limit(limit)
	{
	}
	static WriteBufferBudget &Get()
	{
		static WriteBufferBudget budget;
		return budget;
	}

	Credit Charge(uint64_t bytes)
	{
		auto lease = std::make_shared<Lease>(m_state, bytes);
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->used += bytes;
		return lease;
	}
	uint64_t Used() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->used;
	}
	// Stop selection at the budget, and resume below half.
	bool IsPressured() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (m_state->used >= m_limit) {
			m_state->pressured = true;
		} else if (m_state->used <= m_limit / 2) {
			m_state->pressured = false;
		}
		return m_state->pressured;
	}
	// Flush much earlier, to leave room for arrivals between main-thread ticks.
	bool ShouldFlush() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->used >= m_limit / 8;
	}

private:
	uint64_t m_limit;
	std::shared_ptr<State> m_state = std::make_shared<State>();
};
#endif
