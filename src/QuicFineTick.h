//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef QUICFINETICK_H
#define QUICFINETICK_H

#include <cstdint>

/**
 * Decides when ngtcp2's timers need a tick finer than the core timer.
 *
 * Pacing, ACK delay and loss recovery all have deadlines of a few milliseconds, and the core
 * timer ticks only every 100 ms (GUI) or 300 ms (daemon). The fine tick runs while some
 * connection has a deadline within one core tick, and stops a full core tick after the last
 * one, so an idle connection costs no wakeups and a short lull does not restart a timer thread.
 */
class CQuicFineTick
{
public:
	enum class EAction
	{
		None,
		Start,
		Stop
	};

	//! Also how late a paced packet can leave.
	static constexpr uint64_t kPeriodMs = 5;

	explicit CQuicFineTick(uint64_t horizonMs)
	: m_horizonMs(horizonMs)
	{
	}

	//! @p nextExpiryMs: the earliest deadline of any connection, UINT64_MAX for none.
	EAction Update(uint64_t nextExpiryMs, uint64_t nowMs)
	{
		if (!m_running && nowMs < m_retryAfterMs) {
			return EAction::None;
		}
		if (nextExpiryMs <= nowMs + m_horizonMs) {
			m_lastNeededMs = nowMs;
			if (!m_running) {
				m_running = true;
				return EAction::Start;
			}
			return EAction::None;
		}
		if (m_running && nowMs - m_lastNeededMs >= m_horizonMs) {
			m_running = false;
			return EAction::Stop;
		}
		return EAction::None;
	}

	bool IsRunning() const { return m_running; }

	//! The timer could not start: the core tick serves every connection, and the next start is
	//! tried one core tick later, not on every datagram.
	void StartFailed(uint64_t nowMs)
	{
		m_running = false;
		m_retryAfterMs = nowMs + m_horizonMs;
	}

private:
	const uint64_t m_horizonMs;
	uint64_t m_lastNeededMs = 0;
	uint64_t m_retryAfterMs = 0;
	bool m_running = false;
};

#endif
