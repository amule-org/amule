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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include <muleunit/test.h>

#include "EventBus.h"
#include "EventDiff.h"
#include "State.h"

#include <chrono>
#include <string>
#include <vector>

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(EventDiff)

// Drain `bus` non-blockingly and return all events in id order.
static std::vector<Event> DrainAll(CEventBus &bus)
{
	std::vector<Event> out;
	bus.Drain(0, std::chrono::milliseconds(0), out);
	return out;
}

// log_appended cold-start: the first tick must not emit log_appended
// for pre-existing lines (clients GET /api/v0/logs/amule for the
// history; the event channel is live-tail only).
TEST(EventDiff, LogAppendedColdStartSilent)
{
	CState state;
	state.AppendAmuleLog({ "old line 1\n", "old line 2\n" });
	CEventBus bus;
	LastSeenState prev;

	EmitDiffsAndUpdate(bus, prev, state);

	const auto drained = DrainAll(bus);
	for (const auto &ev : drained) {
		ASSERT_TRUE(ev.name != "log_appended");
	}
	// Baseline counter must equal the pre-existing log size so the
	// next tick's diff sees zero new lines until amuled actually
	// logs something.
	ASSERT_EQUALS(static_cast<std::size_t>(2), prev.amule_log_count);
	ASSERT_TRUE(prev.amule_log_initialised);
}

// After cold-start, a single appended line publishes exactly one
// log_appended event with the new line in `lines`.
TEST(EventDiff, LogAppendedFiresOnSingleNewLine)
{
	CState state;
	state.AppendAmuleLog({ "old line\n" });
	CEventBus bus;
	LastSeenState prev;

	// Tick 1: baseline.
	EmitDiffsAndUpdate(bus, prev, state);
	// Tick 2: amuled appended a fresh line. Expect log_appended.
	state.AppendAmuleLog({ "new line\n" });
	EmitDiffsAndUpdate(bus, prev, state);

	const auto drained = DrainAll(bus);
	int log_events = 0;
	std::string payload;
	for (const auto &ev : drained) {
		if (ev.name == "log_appended") {
			++log_events;
			payload = ev.data;
		}
	}
	ASSERT_EQUALS(1, log_events);
	// Payload must contain the new line content and NOT the old one.
	ASSERT_TRUE(payload.find("new line") != std::string::npos);
	ASSERT_TRUE(payload.find("old line") == std::string::npos);
	// Counter advanced to 2.
	ASSERT_EQUALS(static_cast<std::size_t>(2), prev.amule_log_count);
}

// A batch of multiple new lines lands in one event with a `lines`
// array — never N separate events. Bus traffic ≪ line traffic.
TEST(EventDiff, LogAppendedBatchesMultipleLinesIntoOneEvent)
{
	CState state;
	CEventBus bus;
	LastSeenState prev;

	EmitDiffsAndUpdate(bus, prev, state); // cold-start, log is empty
	state.AppendAmuleLog({ "A\n", "B\n", "C\n" });
	EmitDiffsAndUpdate(bus, prev, state);

	const auto drained = DrainAll(bus);
	int log_events = 0;
	std::string payload;
	for (const auto &ev : drained) {
		if (ev.name == "log_appended") {
			++log_events;
			payload = ev.data;
		}
	}
	ASSERT_EQUALS(1, log_events);
	ASSERT_TRUE(payload.find("\"A") != std::string::npos);
	ASSERT_TRUE(payload.find("\"B") != std::string::npos);
	ASSERT_TRUE(payload.find("\"C") != std::string::npos);
	ASSERT_EQUALS(static_cast<std::size_t>(3), prev.amule_log_count);
}

// Idle ticks (no new lines) must not publish log_appended.
TEST(EventDiff, LogAppendedSilentOnIdleTick)
{
	CState state;
	state.AppendAmuleLog({ "baseline\n" });
	CEventBus bus;
	LastSeenState prev;

	EmitDiffsAndUpdate(bus, prev, state);
	(void)DrainAll(bus); // discard cold-start events

	EmitDiffsAndUpdate(bus, prev, state); // idle
	EmitDiffsAndUpdate(bus, prev, state); // idle

	const auto drained = DrainAll(bus);
	for (const auto &ev : drained) {
		ASSERT_TRUE(ev.name != "log_appended");
	}
}

// JSON escaping: a line containing characters that need JSON-escaping
// (backslash, double quote, control chars) must produce a valid JSON
// payload. The EscJson helper backing this is the same one the
// snapshot payloads use; covering it here pins the contract for
// the log path specifically.
TEST(EventDiff, LogAppendedEscapesJsonHazards)
{
	CState state;
	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	// A line with: a quote, a backslash, a control char.
	state.AppendAmuleLog({ std::string("hi \"quoted\\path\" \x01 done\n") });
	EmitDiffsAndUpdate(bus, prev, state);

	const auto drained = DrainAll(bus);
	std::string payload;
	for (const auto &ev : drained) {
		if (ev.name == "log_appended")
			payload = ev.data;
	}
	// The raw characters must NOT appear unescaped in the payload.
	// `\"` must become `\\\"`, `\\` must become `\\\\`, `\x01` must
	// be `\\u0001`.
	ASSERT_TRUE(payload.find("\\\"") != std::string::npos);
	ASSERT_TRUE(payload.find("\\\\") != std::string::npos);
	ASSERT_TRUE(payload.find("\\u0001") != std::string::npos);
}

// Truncation case (DELETE /logs/amule shrinks the vector): the diff
// must silently resync the baseline counter without publishing.
TEST(EventDiff, LogAppendedSilentOnTruncation)
{
	CState state;
	state.AppendAmuleLog({ "a\n", "b\n", "c\n" });
	CEventBus bus;
	LastSeenState prev;

	EmitDiffsAndUpdate(bus, prev, state);
	ASSERT_EQUALS(static_cast<std::size_t>(3), prev.amule_log_count);

	// Force a smaller log: rebuild State with a shorter vector.
	CState state2;
	state2.AppendAmuleLog({ "a\n" });
	EmitDiffsAndUpdate(bus, prev, state2);

	const auto drained = DrainAll(bus);
	for (const auto &ev : drained) {
		ASSERT_TRUE(ev.name != "log_appended");
	}
	ASSERT_EQUALS(static_cast<std::size_t>(1), prev.amule_log_count);
}
