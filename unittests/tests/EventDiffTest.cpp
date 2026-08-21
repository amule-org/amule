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
#include "ServerFlagNames.h"
#include "State.h"

#include <chrono>
#include <cstdint>
#include <map>
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

// PR #646 / issue #115: upload_file_name (the partfile a peer is downloading
// FROM us) is part of the base client field set, so it must ride the
// client_added SSE payload — otherwise the WebUI clients table has no way to
// fill the File column for an upload-only peer (it shows a blank "—").
// Drives one status change through the real emit path and returns the
// status_changed payload, so these assert what a subscriber actually sees
// rather than reaching into EventDiff's internals.
namespace
{
std::string EmitStatusAndGetPayload(const StatusSnapshot &next)
{
	CState state;
	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state); // baseline tick
	state.WriteStatus(next);
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &ev : DrainAll(bus)) {
		if (ev.name == "status_changed")
			payload = ev.data;
	}
	return payload;
}
} // namespace

// The free-space sentinel must reach the SSE payload as JSON null, never as
// a number. amuled's FREE_SPACE_UNKNOWN is -1 and its EC serializer casts it
// to uint64, so the wire carries 0xFFFFFFFFFFFFFFFF; emitting that unsigned
// would tell a consumer 17 exabytes are free, and 0 would read as a full
// disk. Same rule as the REST body, asserted so the two cannot drift apart.
TEST(EventDiff, StatusEventSerialisesUnknownFreeSpaceAsNull)
{
	StatusSnapshot s;
	s.temp_free_bytes = -1;
	s.incoming_free_bytes = 48318382080LL;

	const std::string payload = EmitStatusAndGetPayload(s);

	ASSERT_TRUE(payload.find("\"temp_free_bytes\":null") != std::string::npos);
	ASSERT_TRUE(payload.find("\"incoming_free_bytes\":48318382080") != std::string::npos);
	// The unsigned reading of the sentinel must appear nowhere.
	ASSERT_TRUE(payload.find("18446744073709551615") == std::string::npos);
}

// high_id is positive-sense precisely so the disconnected case does not read
// as a firewall verdict, and the id/public_ip pair rides the same payload.
TEST(EventDiff, StatusEventCarriesIdentityFields)
{
	StatusSnapshot s;
	s.ed2k_state = "connected";
	s.ed2k_high_id = true;
	s.ed2k_id = 3523226697u;
	s.ed2k_public_ip = "210.2.150.73";
	s.download_overhead_bps = 8700;

	const std::string payload = EmitStatusAndGetPayload(s);

	ASSERT_TRUE(payload.find("\"high_id\":true") != std::string::npos);
	ASSERT_TRUE(payload.find("\"id\":3523226697") != std::string::npos);
	ASSERT_TRUE(payload.find("\"public_ip\":\"210.2.150.73\"") != std::string::npos);
	ASSERT_TRUE(payload.find("\"download_overhead_bps\":8700") != std::string::npos);
	// The retired spelling must not linger anywhere in the payload.
	ASSERT_TRUE(payload.find("low_id") == std::string::npos);
}

// A tick where only the overhead moved still has to fire: the field is in the
// REST body, so if the SSE twin stays silent the two diverge until something
// else happens to move.
TEST(EventDiff, StatusEventFiresWhenOnlyOverheadMoved)
{
	StatusSnapshot s;
	s.download_overhead_bps = 8700;

	const std::string payload = EmitStatusAndGetPayload(s);

	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"download_overhead_bps\":8700") != std::string::npos);
}

TEST(EventDiff, ClientAddedCarriesUploadFileName)
{
	CState state;
	CEventBus bus;
	LastSeenState prev;

	// Tick 1: baseline with no clients.
	EmitDiffsAndUpdate(bus, prev, state);

	// Tick 2: a peer we are uploading to appears.
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &cache) {
		ClientSnapshot c;
		c.ecid = 10;
		c.client_name = "peer-up";
		c.upload_state = "uploading";
		c.upload_file_name = "upload.iso";
		cache.emplace(c.ecid, c);
	});
	EmitDiffsAndUpdate(bus, prev, state);

	const auto drained = DrainAll(bus);
	int added = 0;
	std::string payload;
	for (const auto &ev : drained) {
		if (ev.name == "client_added") {
			++added;
			payload = ev.data;
		}
	}
	ASSERT_EQUALS(1, added);
	ASSERT_TRUE(payload.find("upload_file_name") != std::string::npos);
	ASSERT_TRUE(payload.find("upload.iso") != std::string::npos);
}

// Regression guard for the EventDiff.cpp Equal() half of PR #646: Equal() must
// compare every field ToJson emits (see the note above Equal()), so a change
// to upload_file_name alone still fires client_updated. Before the fix the
// field was in neither, and an upload-file change would have been dropped —
// the SSE-backed table would keep showing the stale filename.
TEST(EventDiff, ClientUpdatedFiresOnUploadFileNameChange)
{
	CState state;
	CEventBus bus;
	LastSeenState prev;

	// Baseline: no clients.
	EmitDiffsAndUpdate(bus, prev, state);

	// A peer appears, uploading "a.iso" from us (client_added).
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &cache) {
		ClientSnapshot c;
		c.ecid = 10;
		c.client_name = "peer-up";
		c.upload_state = "uploading";
		c.upload_file_name = "a.iso";
		cache.emplace(c.ecid, c);
	});
	EmitDiffsAndUpdate(bus, prev, state);

	// Only upload_file_name changes -> must fire client_updated.
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &cache) {
		cache.at(10).upload_file_name = "b.iso";
	});
	EmitDiffsAndUpdate(bus, prev, state);

	const auto drained = DrainAll(bus);
	int updated = 0;
	std::string payload;
	for (const auto &ev : drained) {
		if (ev.name == "client_updated") {
			++updated;
			payload = ev.data;
		}
	}
	ASSERT_EQUALS(1, updated);
	ASSERT_TRUE(payload.find("b.iso") != std::string::npos);
}

// Same contract as the client test above, for the capability bitmasks issue
// #974 added: a server announcing its flags after the first UDP status reply
// has to fire exactly one server_updated, and the payload has to carry the
// decoded object -- not just the raw bitmask.
TEST(EventDiff, ServerUpdatedFiresOnTcpFlagsChange)
{
	CState state;
	CEventBus bus;
	LastSeenState prev;

	EmitDiffsAndUpdate(bus, prev, state);

	// A freshly added server, before any UDP status reply came back:
	// nothing announced, so every bit is clear.
	state.MutateServers([](std::map<std::uint32_t, ServerSnapshot> &cache) {
		ServerSnapshot s;
		s.ecid = 5;
		s.name = "srv";
		cache.emplace(s.ecid, s);
	});
	EmitDiffsAndUpdate(bus, prev, state);

	// The reply lands and the server announces what it supports.
	state.MutateServers([](std::map<std::uint32_t, ServerSnapshot> &cache) {
		cache.at(5).tcp_flags = SRV_TCPFLG_COMPRESSION | SRV_TCPFLG_RELATEDSEARCH;
	});
	EmitDiffsAndUpdate(bus, prev, state);

	const auto drained = DrainAll(bus);
	int updated = 0;
	std::string payload;
	for (const auto &ev : drained) {
		if (ev.name == "server_updated") {
			++updated;
			payload = ev.data;
		}
	}
	ASSERT_EQUALS(1, updated);
	ASSERT_TRUE(payload.find("\"related_search\":true") != std::string::npos);
	ASSERT_TRUE(payload.find("\"compression\":true") != std::string::npos);
	// A bit that was not announced is present and false, never absent:
	// consumers are documented as never having to test for the key.
	ASSERT_TRUE(payload.find("\"unicode\":false") != std::string::npos);
}

// The publishing limits move independently of the flags and are likewise
// carried in the payload rather than requiring a re-GET.
TEST(EventDiff, ServerUpdatedFiresOnFileLimitChange)
{
	CState state;
	CEventBus bus;
	LastSeenState prev;

	EmitDiffsAndUpdate(bus, prev, state);

	state.MutateServers([](std::map<std::uint32_t, ServerSnapshot> &cache) {
		ServerSnapshot s;
		s.ecid = 6;
		s.name = "srv";
		cache.emplace(s.ecid, s);
	});
	EmitDiffsAndUpdate(bus, prev, state);

	state.MutateServers([](std::map<std::uint32_t, ServerSnapshot> &cache) {
		cache.at(6).soft_file_limit = 1000;
		cache.at(6).hard_file_limit = 5000;
	});
	EmitDiffsAndUpdate(bus, prev, state);

	const auto drained = DrainAll(bus);
	int updated = 0;
	std::string payload;
	for (const auto &ev : drained) {
		if (ev.name == "server_updated") {
			++updated;
			payload = ev.data;
		}
	}
	ASSERT_EQUALS(1, updated);
	ASSERT_TRUE(payload.find("\"soft_file_limit\":1000") != std::string::npos);
	ASSERT_TRUE(payload.find("\"hard_file_limit\":5000") != std::string::npos);
}

// The flags object is built by one shared helper so the REST writer
// (Api.cpp, CJsonWriter) and this SSE writer emit the same bytes. Pin the
// exact shape: key order follows the wire-bit order, bitmask leads.
TEST(EventDiff, ServerFlagsJsonShape)
{
	ASSERT_EQUALS(std::string("{\"bitmask\":0,\"compression\":false,\"new_tags\":false,"
				  "\"unicode\":false,\"related_search\":false,"
				  "\"type_tag_integer\":false,\"large_files\":false,"
				  "\"tcp_obfuscation\":false}"),
		webapi::ServerTcpFlagsJson(0));

	// An unnamed bit survives in `bitmask` even though no boolean
	// describes it -- that is what the field is there for.
	ASSERT_TRUE(webapi::ServerUdpFlagsJson(0x8000u).find("\"bitmask\":32768") != std::string::npos);
}
