//
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

#include "RateLimiter.h"

CRateLimiter::Decision CRateLimiter::Check(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(m_mu);
	const std::time_t now = m_clock();
	auto it = m_buckets.find(ip);
	if (it == m_buckets.end())
		return Decision{};

	Bucket &b = it->second;
	if (b.lockout_until > now) {
		Decision d;
		d.locked_out = true;
		d.retry_after_seconds = b.lockout_until - now;
		return d;
	}
	// Lockout window expired -- wipe the bucket so a stale lockout can't
	// accidentally fire on the next Check after a long quiet period.
	if (b.lockout_until != 0 && b.lockout_until <= now) {
		b.lockout_until = 0;
		b.failures.clear();
	}
	// Mirror NoteFailure's per-stamp expiry so Check is self-consistent.
	// Otherwise stale stamps from a long-idle bucket remain in failures
	// until the next NoteFailure fires.
	while (!b.failures.empty() && (now - b.failures.front()) > m_cfg.window_seconds) {
		b.failures.pop_front();
	}
	return Decision{};
}

void CRateLimiter::NoteFailure(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(m_mu);
	const std::time_t now = m_clock();
	Bucket &b = m_buckets[ip];

	// Sliding window: drop any failure stamp older than `window_seconds`,
	// then append now. Lockout fires when the live stamp count crosses
	// `threshold`. See the Bucket comment for why per-stamp expiry rather
	// than a wholesale reset.
	while (!b.failures.empty() && (now - b.failures.front()) > m_cfg.window_seconds) {
		b.failures.pop_front();
	}
	b.failures.push_back(now);

	if (b.failures.size() >= m_cfg.threshold) {
		b.lockout_until = now + m_cfg.lockout_seconds;
	}
}

void CRateLimiter::NoteSuccess(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(m_mu);
	m_buckets.erase(ip);
}
