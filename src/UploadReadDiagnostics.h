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
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef UPLOADREADDIAGNOSTICS_H
#define UPLOADREADDIAGNOSTICS_H

#include <algorithm>
#include <array>
#include <cstdint>

namespace UploadReadDiagnostics
{
// Millisecond buckets with power-of-two upper bounds; the last one takes everything above.
// Median is an upper estimate (less than twice the observed value); maximum remains exact.
struct Histogram
{
	static constexpr unsigned BucketCount = 32;
	std::array<uint64_t, BucketCount> buckets{};
	uint64_t count = 0;
	uint64_t maximum = 0;
	static uint64_t Bound(unsigned bucket) { return bucket == 0 ? 0 : uint64_t{ 1 } << (bucket - 1); }
	void Add(uint64_t value)
	{
		unsigned bucket = 0;
		while (bucket < BucketCount - 1 && Bound(bucket) < value) {
			++bucket;
		}
		++buckets[bucket];
		++count;
		maximum = std::max(maximum, value);
	}
	uint64_t Median() const
	{
		uint64_t cumulative = 0;
		for (unsigned i = 0; i < BucketCount; ++i) {
			cumulative += buckets[i];
			if (cumulative >= (count + 1) / 2) {
				return i == BucketCount - 1 ? maximum : Bound(i);
			}
		}
		return 0;
	}
};
enum class SlotState
{
	Storage,
	Reading,
	Receiver,
	Peer,
	Sending,
	Count
};
inline SlotState Classify(bool pending, bool requests, bool atTarget, bool queued)
{
	if (pending) {
		return SlotState::Reading;
	}
	if (!requests) {
		// Every requested block is read. Queued data has not left the socket yet.
		return queued ? SlotState::Sending : SlotState::Peer;
	}
	return atTarget ? SlotState::Receiver : SlotState::Storage;
}
// Another thread can stamp `since` after the caller read `now`.
inline uint64_t ElapsedSince(uint64_t now, uint64_t since)
{
	return now >= since ? now - since : 0;
}
// Retained per slot, so tracking another block needs no allocation. Identity
// includes the file: a peer can switch files without starting a new slot.
struct DispatchWait
{
	uint64_t since = 0;
	uint64_t session = 0;
	uint64_t begin = 0;
	uint64_t end = 0;
	std::array<uint8_t, 16> file{};
	bool active = false;
	void Observe(
		uint64_t newSession, uint64_t newBegin, uint64_t newEnd, const uint8_t *newFile, uint64_t now)
	{
		if (!active || session != newSession || begin != newBegin || end != newEnd ||
			!std::equal(file.begin(), file.end(), newFile)) {
			since = now;
			session = newSession;
			begin = newBegin;
			end = newEnd;
			std::copy(newFile, newFile + file.size(), file.begin());
		}
		active = true;
	}
	void Reset() { active = false; }
	uint64_t Elapsed(uint64_t now) const { return active ? ElapsedSince(now, since) : 0; }
};
// One upload socket's sends since the last report. The owner serialises access.
struct SendStats
{
	uint64_t sentBytes = 0;
	uint64_t refusedWrites = 0;
	uint64_t busyMaximum = 0;
	uint64_t wakeMaximum = 0;
	uint64_t lastSent = 0;
	uint64_t busySince = 0;
	void Refused(uint64_t now)
	{
		++refusedWrites;
		if (busySince == 0) {
			busySince = now;
		}
	}
	// wakeDelay is how long the wake waited for the main thread after the blocking send finished.
	void Resumed(uint64_t wakeDelay, uint64_t now)
	{
		if (busySince == 0) {
			return;
		}
		busyMaximum = std::max(busyMaximum, now - busySince);
		busySince = 0;
		wakeMaximum = std::max(wakeMaximum, wakeDelay);
	}
	// A socket still busy reports its wait so far and keeps timing it.
	SendStats Take(uint64_t now)
	{
		SendStats report = *this;
		if (busySince != 0) {
			report.busyMaximum = std::max(busyMaximum, ElapsedSince(now, busySince));
		}
		*this = SendStats{};
		lastSent = report.lastSent;
		busySince = report.busySince;
		return report;
	}
};
struct Volume
{
	Histogram read;
	Histogram dispatch;
	uint64_t bytes = 0;
	uint64_t deliveryMaximum = 0;
	uint64_t turns = 0;
	uint64_t turnReads = 0;
	uint64_t busySkips = 0;
};
} // namespace UploadReadDiagnostics
#endif
