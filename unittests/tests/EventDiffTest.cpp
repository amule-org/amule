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

// Drain `bus` non-blockingly and return every event newer than `since` in id
// order. Drain is a replay from a cursor, not a consume: draining from 0 twice
// yields the same events twice, so a test asserting that a later tick emitted
// *nothing* has to carry the cursor forward.
static std::vector<Event> DrainSince(CEventBus &bus, std::uint64_t since)
{
	std::vector<Event> out;
	bus.Drain(since, std::chrono::milliseconds(0), out);
	return out;
}

// Drain `bus` non-blockingly and return all events in id order.
static std::vector<Event> DrainAll(CEventBus &bus)
{
	return DrainSince(bus, 0);
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
	s.ed2k_user_id = 1234567890u;
	s.ed2k_public_ip = "210.2.150.73";
	s.download_overhead_bps = 8700;

	const std::string payload = EmitStatusAndGetPayload(s);

	ASSERT_TRUE(payload.find("\"high_id\":true") != std::string::npos);
	ASSERT_TRUE(payload.find("\"user_id\":1234567890") != std::string::npos);
	ASSERT_TRUE(payload.find("\"public_ip\":\"210.2.150.73\"") != std::string::npos);
	ASSERT_TRUE(payload.find("\"download_overhead_bps\":8700") != std::string::npos);
	// The retired spellings must not linger anywhere in the payload. A bare
	// "id" would also match inside "user_id", so the quoted key is the test.
	ASSERT_TRUE(payload.find("low_id") == std::string::npos);
	ASSERT_TRUE(payload.find("\"id\":") == std::string::npos);
}

// The Kad firewall verdict is the one field on this payload a subscriber is
// most likely to be watching for, and Equal(StatusSnapshot) is what decides
// whether the event is published at all. Drop it from that comparator and a
// firewall flip stops reaching subscribers entirely -- silently, since the
// REST body keeps reporting the new value. Pinned here so a future edit to
// the comparator cannot quietly lose it.
TEST(EventDiff, StatusEventFiresWhenOnlyKadFirewalledTcpMoved)
{
	StatusSnapshot s;
	s.kad_firewalled_tcp = true;

	const std::string payload = EmitStatusAndGetPayload(s);

	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"firewalled_tcp\":true") != std::string::npos);
	// The pre-rename spelling must not survive anywhere in the payload.
	ASSERT_TRUE(payload.find("\"firewalled\":") == std::string::npos);
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

// EVENTS.md promises this payload is "identical to the REST /status envelope",
// and both connected_since timestamps are part of that envelope. They were
// missing from the event, so a subscriber could never learn when the daemon
// connected without falling back to a poll.
TEST(EventDiff, StatusEventCarriesBothConnectedSince)
{
	StatusSnapshot s;
	s.ed2k_state = "connected";
	s.kad_state = "connected";
	s.ed2k_connected_since = 1751000000ull;
	s.kad_connected_since = 1751000042ull;

	const std::string payload = EmitStatusAndGetPayload(s);

	ASSERT_TRUE(payload.find("\"connected_since\":1751000000") != std::string::npos);
	ASSERT_TRUE(payload.find("\"connected_since\":1751000042") != std::string::npos);
}

// A reconnect can leave every other field identical -- same server, same id,
// idle transfer rates -- and move only the timestamp. Without it in Equal()
// that tick emits nothing and subscribers keep showing the old uptime.
TEST(EventDiff, StatusEventFiresWhenOnlyConnectedSinceMoved)
{
	StatusSnapshot s;
	s.ed2k_connected_since = 1751000000ull;

	const std::string payload = EmitStatusAndGetPayload(s);

	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"connected_since\":1751000000") != std::string::npos);
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

// --- search_result_added is the results-list entry, verbatim ---------
//
// EVENTS.md promises the payload is byte-for-byte a
// GET /search/{id}/results entry with `search_id` prepended. That used to
// be two hand-written serialisers kept in step by review, and they had
// already drifted apart. Both now go through WriteSearchResultFields, so
// this pins the fields the event MUST carry -- including the two the
// hand-rolled copy had been missing.
TEST(EventDiff, SearchResultAddedCarriesTheFullResultsEntry)
{
	CState state;
	state.MarkSearchStarted(42, "browse", "SomePeerNick");

	CEventBus bus;
	LastSeenState prev;
	// First tick baselines the (empty) slot: cold start never replays
	// history as events.
	EmitDiffsAndUpdate(bus, prev, state);

	state.MutateSearch(42, [](std::map<std::uint32_t, SearchResult> &cache) {
		SearchResult r;
		r.ecid = 7;
		r.hash = "8b54a3c28b54a3c28b54a3c28b54a3c2";
		r.name = "peer-shared.iso";
		r.size = 4096;
		r.source_count = 3;
		r.complete_source_count = 1;
		r.status = "new";
		r.type = "iso";
		r.directory = "Incoming/ISOs";
		SearchResult::Child c;
		c.ecid = 8;
		c.name = "peer-shared-copy.iso";
		c.hash = r.hash;
		c.directory = "Backup/ISOs";
		r.children.push_back(c);
		cache.emplace(r.ecid, r);
	});
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "search_result_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	// search_id leads, so a consumer can demux before parsing the rest.
	ASSERT_TRUE(payload.compare(0, 15, "{\"search_id\":42") == 0);
	// The browse-only folder, on the result and on its child.
	ASSERT_TRUE(payload.find("\"directory\":\"Incoming/ISOs\"") != std::string::npos);
	ASSERT_TRUE(payload.find("\"directory\":\"Backup/ISOs\"") != std::string::npos);
	// The two fields the previously hand-rolled event payload omitted
	// while the REST writer emitted them.
	ASSERT_TRUE(payload.find("\"kad_comment_search_running\":false") != std::string::npos);
	ASSERT_TRUE(payload.find("\"comments\":[]") != std::string::npos);
}

// --- search_closed fires when a slot disappears ----------------------
//
// A subscriber holding one view per search otherwise only learns the
// search is gone by 404ing on a later read -- and with SSE live it may
// never read again.
TEST(EventDiff, SearchClosedFiresOnceWhenTheSlotIsFreed)
{
	CState state;
	state.MarkSearchStarted(42, "global", "ubuntu");
	state.MarkSearchStarted(43, "kad", "debian");

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state); // baseline: both known

	// The bus replays from the start on every drain, so count over the
	// whole history rather than treating a drain as "what is new".
	auto CountClosed = [&bus]() {
		std::size_t n = 0;
		std::string last;
		for (const auto &e : DrainAll(bus)) {
			if (e.name == "search_closed") {
				++n;
				last = e.data;
			}
		}
		return std::make_pair(n, last);
	};
	ASSERT_EQUALS(static_cast<size_t>(0), CountClosed().first);

	state.CloseSearch(42);
	EmitDiffsAndUpdate(bus, prev, state);
	const auto after_close = CountClosed();
	ASSERT_EQUALS(static_cast<size_t>(1), after_close.first);
	ASSERT_EQUALS(std::string("{\"search_id\":42}"), after_close.second);

	// Further ticks stay silent: the baseline was pruned with the event,
	// so a freed search is announced exactly once and its surviving
	// sibling is never swept up with it.
	EmitDiffsAndUpdate(bus, prev, state);
	EmitDiffsAndUpdate(bus, prev, state);
	ASSERT_EQUALS(static_cast<size_t>(1), CountClosed().first);
	ASSERT_TRUE(state.HasSearch(43));
}

// --- the client payload carries part_progress_percent ------------------
//
// EVENTS.md promises an `_updated` subscriber gets the full new state and
// never has to re-GET. This field was the one exception on the client
// resource: it is derived rather than refreshed (it needs the part count of
// the linked download, which lives in a different snapshot), so the diff pass
// never computed it and the payload silently lacked a key the REST row had.
// #1159 section 1. ClientSnapshot carries has_available_parts precisely so a
// peer that never reported its part map can be told apart from one reporting
// zero -- and zero is a real answer, being what a fresh source looks like
// before its map arrives. The field was emitted unconditionally, so nothing
// read the flag and both cases went out as 0.
TEST(EventDiff, ClientEventEmitsNullAvailablePartsWhenTheMapIsUnreported)
{
	CState state;
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &clients) {
		ClientSnapshot c;
		c.ecid = 71;
		c.client_name = "no-map";
		// has_available_parts stays false: the tag never arrived.
		clients.emplace(c.ecid, c);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "client_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"available_parts\":null") != std::string::npos);
}

// ...and a peer that did report zero still says zero.
TEST(EventDiff, ClientEventEmitsZeroAvailablePartsWhenTheMapSaysZero)
{
	CState state;
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &clients) {
		ClientSnapshot c;
		c.ecid = 72;
		c.client_name = "empty-map";
		c.available_parts = 0;
		c.has_available_parts = true;
		clients.emplace(c.ecid, c);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "client_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"available_parts\":0") != std::string::npos);
	ASSERT_TRUE(payload.find("\"available_parts\":null") == std::string::npos);
}

// The comparator has to see the flag too. null -> 0 is a visible change; with
// only the value compared it reads as equal and the row never updates.
TEST(EventDiff, ClientUpdateFiresWhenThePartMapFinallyArrivesReportingZero)
{
	CState state;
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &clients) {
		ClientSnapshot c;
		c.ecid = 73;
		c.client_name = "late-map";
		clients.emplace(c.ecid, c);
	});
	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);
	DrainAll(bus);

	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &clients) {
		auto it = clients.find(73);
		it->second.available_parts = 0;
		it->second.has_available_parts = true;
	});
	EmitDiffsAndUpdate(bus, prev, state);

	bool updated = false;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "client_updated")
			updated = true;
	}
	ASSERT_TRUE(updated);
}

// #1159 section 9. amuled sends 0xffff for "that peer's queue is full" rather
// than a position, so relaying it verbatim rendered "position 65535" and sorted
// full queues to the far end as if they were merely very distant.
TEST(EventDiff, ClientEventEmitsNullRemoteQueueRankWhenTheQueueIsFull)
{
	CState state;
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &clients) {
		ClientSnapshot full;
		full.ecid = 74;
		full.client_name = "full-queue";
		full.remote_queue_rank = kRemoteQueueFullSentinel;
		clients.emplace(full.ecid, full);

		ClientSnapshot ranked;
		ranked.ecid = 75;
		ranked.client_name = "ranked";
		ranked.remote_queue_rank = 12;
		clients.emplace(ranked.ecid, ranked);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string full_payload, ranked_payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name != "client_added")
			continue;
		if (e.data.find("full-queue") != std::string::npos)
			full_payload = e.data;
		if (e.data.find("ranked") != std::string::npos)
			ranked_payload = e.data;
	}
	ASSERT_TRUE(!full_payload.empty());
	ASSERT_TRUE(full_payload.find("\"remote_queue_rank\":null") != std::string::npos);
	// A real position is still a number.
	ASSERT_TRUE(!ranked_payload.empty());
	ASSERT_TRUE(ranked_payload.find("\"remote_queue_rank\":12") != std::string::npos);
}

TEST(EventDiff, ClientEventCarriesPartProgressPercent)
{
	CState state;
	// A 4-part file (3 * PARTSIZE + a byte) the peer is a source for, holding
	// 3 of its 4 chunks.
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 1;
		f.hash = "8b54a3c28b54a3c28b54a3c28b54a3c2";
		f.name = "four-parts.iso";
		f.size = kPartSizeBytes * 3 + 1;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &clients) {
		ClientSnapshot c;
		c.ecid = 7;
		c.client_name = "peer";
		c.download_file_hash = "8b54a3c28b54a3c28b54a3c28b54a3c2";
		c.available_parts = 3;
		c.has_available_parts = true;
		clients.emplace(c.ecid, c);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "client_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	// 3 of 4 parts. Asserting the prefix rather than the full double keeps
	// this independent of ostream's default precision.
	ASSERT_TRUE(payload.find("\"part_progress_percent\":75") != std::string::npos);
}

TEST(EventDiff, ClientEventNullsPartProgressPercentWithNoLinkedFile)
{
	// A peer that only downloads FROM us has no meaningful denominator, so
	// the field is null -- the same rule the REST row follows since #1160
	// section 1, where an unknown value is null rather than an absent key.
	// The -1 sentinel is in-process only and must never reach the wire.
	CState state;
	state.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &clients) {
		ClientSnapshot c;
		c.ecid = 8;
		c.client_name = "leech";
		clients.emplace(c.ecid, c);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "client_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"part_progress_percent\":null") != std::string::npos);
	ASSERT_TRUE(payload.find("-1") == std::string::npos);
}

// `media` is null, never absent, on a shared event whose file has no metadata.
// The event promises key parity with the /shared row, and that row reports the
// key unconditionally -- a subscriber diffing the two must not find `media` on
// one side only. This drifted once already: the REST writer moved to null while
// this one kept skipping the key, and only a live parity check caught it.
TEST(EventDiff, SharedEventNullsMediaWithNoMetadata)
{
	CState state;
	state.MutateShared([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 12;
		f.hash = "1212121212121212121212121212bbbb";
		f.name = "no-metadata.bin";
		f.size = kPartSizeBytes;
		f.is_shared = true;
		f.has_media = false;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "shared_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"media\":null") != std::string::npos);
}

// ...and the object itself when there is one, so the null above is the
// no-value answer rather than the writer having lost the field.
TEST(EventDiff, SharedEventCarriesMediaWhenPresent)
{
	CState state;
	state.MutateShared([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 13;
		f.hash = "1313131313131313131313131313cccc";
		f.name = "clip.mkv";
		f.size = kPartSizeBytes;
		f.is_shared = true;
		f.has_media = true;
		f.media.length_s = 5400;
		f.media.bitrate = 1500;
		f.media.codec = "h264";
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "shared_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"media\":{\"length_s\":5400") != std::string::npos);
	ASSERT_TRUE(payload.find("\"codec\":\"h264\"") != std::string::npos);
}

// Hashing progress on the shared side (issue #1054). amuled emits one tag kind
// per ECID, so a file that is both downloading and shared arrives only as
// EC_TAG_PARTFILE and its hashing progress lands in the download sub-block.
// SharedHashingProgress() is the fallback every shared-side consumer goes
// through; these pin that the SSE payload and the equality test use it too,
// since a shared row that never updates is the failure this hides behind.
TEST(EventDiff, SharedEventCarriesHashingProgressFromTheSharedSide)
{
	CState state;
	state.MutateShared([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 11;
		f.hash = "1111111111111111111111111111aaaa";
		f.name = "complete.iso";
		f.size = kPartSizeBytes * 4;
		f.is_shared = true;
		f.shared.hashing_progress = 2;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "shared_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"hashing_progress\":2") != std::string::npos);
}

TEST(EventDiff, SharedEventFallsBackToTheDownloadSideHashingProgress)
{
	// The shared partfile case: shared.hashing_progress stays 0 because the
	// tag never arrived on the KNOWNFILE variant, and the accessor reads
	// across to the download sub-block.
	CState state;
	state.MutateShared([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 12;
		f.hash = "2222222222222222222222222222bbbb";
		f.name = "in-progress.iso";
		f.size = kPartSizeBytes * 4;
		f.is_shared = true;
		f.is_downloading = true;
		f.download.hashing_progress = 3;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "shared_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"hashing_progress\":3") != std::string::npos);
}

TEST(EventDiff, SharedUpdatedFiresWhenOnlyHashingProgressMoved)
{
	// EqualShared compares through the accessor, so a hash advancing on the
	// download side of a shared partfile still pushes shared_updated. Comparing
	// the raw shared field would hold every tick of it back.
	CState state;
	state.MutateShared([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 13;
		f.hash = "3333333333333333333333333333cccc";
		f.name = "verifying.iso";
		f.size = kPartSizeBytes * 4;
		f.is_shared = true;
		f.is_downloading = true;
		f.download.hashing_progress = 1;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state); // cold start: shared_added
	DrainAll(bus);

	state.MutateShared([](FileMap &files) { files.find(13)->second.download.hashing_progress = 2; });
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "shared_updated")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"hashing_progress\":2") != std::string::npos);
}

TEST(EventDiff, DownloadUpdatedFiresWhenOnlyHashingProgressMoved)
{
	// The download side needs the same treatment: hashing_progress used to be
	// GET /downloads/{hash}-only and absent from EqualDownload, so a Verify
	// Local Data pass produced no download_updated at all.
	CState state;
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 14;
		f.hash = "4444444444444444444444444444dddd";
		f.name = "checking.iso";
		f.size = kPartSizeBytes * 4;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);
	DrainAll(bus);

	state.MutateDownloads([](FileMap &files) { files.find(14)->second.download.hashing_progress = 5; });
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "download_updated")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"hashing_progress\":5") != std::string::npos);
}

// A file leaving the map fires download_removed carrying its hash, and drops
// out of the baseline — the `gone` erase. Without it the baseline would grow
// without bound across a session and keep re-firing the removal every tick.
TEST(EventDiff, DownloadRemovedFiresWhenTheFileLeavesTheMap)
{
	CState state;
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 21;
		f.hash = "3333333333333333333333333333cccc";
		f.name = "gone.iso";
		f.size = kPartSizeBytes * 2;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state); // baseline: download_added
	DrainAll(bus);
	ASSERT_EQUALS(static_cast<std::size_t>(1), prev.files.size());

	// FileMap::erase is iterator-only — it keeps the hash index in step.
	state.MutateDownloads([](FileMap &files) { files.erase(files.find(21)); });
	EmitDiffsAndUpdate(bus, prev, state);

	int removed_events = 0;
	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "download_removed") {
			++removed_events;
			payload = e.data;
		}
	}
	ASSERT_EQUALS(1, removed_events);
	ASSERT_TRUE(payload.find("3333333333333333333333333333cccc") != std::string::npos);
	// Baseline entry erased, so a third tick stays silent.
	ASSERT_TRUE(prev.files.empty());
	const std::uint64_t cursor = bus.NewestId();
	EmitDiffsAndUpdate(bus, prev, state);
	ASSERT_TRUE(DrainSince(bus, cursor).empty());
}

// The share flag clearing on a file that stays in the map fires shared_removed
// without erasing the baseline entry: the ECID is still live, only that role
// ended.
TEST(EventDiff, SharedRemovedFiresWhenOnlyTheRoleFlagClears)
{
	CState state;
	state.MutateShared([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 22;
		f.hash = "4444444444444444444444444444dddd";
		f.name = "unshared.iso";
		f.size = kPartSizeBytes * 2;
		f.is_shared = true;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);
	DrainAll(bus);

	state.MutateShared([](FileMap &files) { files.find(22)->second.is_shared = false; });
	EmitDiffsAndUpdate(bus, prev, state);

	int shared_removed = 0, download_removed = 0;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "shared_removed")
			++shared_removed;
		if (e.name == "download_removed")
			++download_removed;
	}
	ASSERT_EQUALS(1, shared_removed);
	// The download role is untouched, so nothing tears down that side.
	ASSERT_EQUALS(0, download_removed);
	// Entry stays in the baseline, with the cleared flag written back.
	ASSERT_EQUALS(static_cast<std::size_t>(1), prev.files.size());
	ASSERT_TRUE(!prev.files.find(22)->second.is_shared);
}

// One ECID swapping roles in a single tick emits both families, removed first:
// a client tearing down its download slot must not see the shared_added for
// the same file arrive before the download_removed.
TEST(EventDiff, RoleFlipEmitsRemovedBeforeAdded)
{
	CState state;
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 23;
		f.hash = "5555555555555555555555555555eeee";
		f.name = "completed.iso";
		f.size = kPartSizeBytes * 2;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);
	DrainAll(bus);

	// The download finished: it stops being a download and starts being shared.
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot &f = files.find(23)->second;
		f.is_downloading = false;
		f.is_shared = true;
	});
	EmitDiffsAndUpdate(bus, prev, state);

	int removed_at = -1, added_at = -1, i = 0;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "download_removed")
			removed_at = i;
		if (e.name == "shared_added")
			added_at = i;
		++i;
	}
	ASSERT_TRUE(removed_at >= 0);
	ASSERT_TRUE(added_at >= 0);
	ASSERT_TRUE(removed_at < added_at);
}

// The write-back trap: a role flag flipping is itself enough to refresh the
// whole baseline entry, so fields the *other* role's predicate never compared
// cannot reach the payload stale. Here the download sub-block moves during the
// same tick that is_downloading goes false→true; the download_added must carry
// the new value, not the one the entry was parked with while it was shared-only.
TEST(EventDiff, RoleFlipRefreshesFieldsTheOtherRoleNeverCompared)
{
	CState state;
	state.MutateShared([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 24;
		f.hash = "6666666666666666666666666666ffff";
		f.name = "reshared.iso";
		f.size = kPartSizeBytes * 4;
		f.is_shared = true;
		f.download.size_done = 111;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state); // shared_added; download side never compared
	DrainAll(bus);

	state.MutateShared([](FileMap &files) {
		FileSnapshot &f = files.find(24)->second;
		f.is_downloading = true;
		f.download.size_done = 999;
	});
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "download_added")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("999") != std::string::npos);
	ASSERT_TRUE(payload.find("111") == std::string::npos);
	// And the baseline now carries it, so the next tick is silent.
	ASSERT_EQUALS(static_cast<std::uint64_t>(999), prev.files.find(24)->second.download.size_done);
}

// --- comments_updated payload ---------------------------------------
//
// EVENTS.md promises the payload is the GET /downloads/{hash}/comments body
// plus `hash`. It used to carry `hash` but NOT `kad_comment_search_running`,
// so a client that followed the document and fed the event into the view it
// built from the endpoint silently lost the in-flight-lookup flag -- exactly
// the flag it needs while a POST /downloads/{hash}/comments Kad lookup runs.
TEST(EventDiff, CommentsUpdatedIsASupersetOfTheRestBody)
{
	CState state;
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 41;
		f.hash = "5555555555555555555555555555eeee";
		f.name = "commented.iso";
		f.size = kPartSizeBytes * 2;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state); // cold start: download_added
	DrainAll(bus);

	// A Kad lookup is in flight AND a note has landed, so the event fires
	// with the flag still true -- the state a client is most likely to
	// render, and the one the missing key made unrepresentable.
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot &f = files.find(41)->second;
		f.download.kad_comment_searching = true;
		FileSnapshot::DownloadSide::SourceComment c;
		c.username = "alice";
		c.filename = "commented.iso";
		c.rating = 5;
		c.comment = "great quality";
		f.download.source_comments.push_back(c);
	});
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "comments_updated")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	// The endpoint's three keys...
	ASSERT_TRUE(payload.find("\"count\":1") != std::string::npos);
	ASSERT_TRUE(payload.find("\"kad_comment_search_running\":true") != std::string::npos);
	ASSERT_TRUE(payload.find("\"comments\":[") != std::string::npos);
	// ...plus the one the event adds, because nothing else in the frame
	// identifies the file.
	ASSERT_TRUE(payload.find("\"hash\":\"5555555555555555555555555555eeee\"") != std::string::npos);
	ASSERT_TRUE(payload.find("\"username\":\"alice\"") != std::string::npos);
}

// And the flag tracks false, so a client can see the lookup finish.
TEST(EventDiff, CommentsUpdatedReportsAnIdleKadLookup)
{
	CState state;
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 42;
		f.hash = "6666666666666666666666666666ffff";
		f.name = "quiet.iso";
		f.size = kPartSizeBytes;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);
	DrainAll(bus);

	state.MutateDownloads([](FileMap &files) {
		FileSnapshot &f = files.find(42)->second;
		FileSnapshot::DownloadSide::SourceComment c;
		c.username = "Kad user";
		c.rating = 4;
		f.download.source_comments.push_back(c);
	});
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "comments_updated")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"kad_comment_search_running\":false") != std::string::npos);
}

// The Kad lookup finishing is a comments_updated in its own right. The flag
// rides in the payload, so if EqualComments ignores it the true->false edge
// produces no event and a ?channels=comments subscriber's in-flight
// indicator never clears.
TEST(EventDiff, CommentsUpdatedFiresWhenOnlyTheKadFlagClears)
{
	CState state;
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 43;
		f.hash = "7777777777777777777777777777aaaa";
		f.name = "searching.iso";
		f.size = kPartSizeBytes;
		f.is_downloading = true;
		f.download.kad_comment_searching = true;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state); // cold start
	DrainAll(bus);

	// The lookup ends. No comment arrived, so source_comments is untouched
	// and ONLY the flag moves.
	state.MutateDownloads(
		[](FileMap &files) { files.find(43)->second.download.kad_comment_searching = false; });
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "comments_updated")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"kad_comment_search_running\":false") != std::string::npos);
}

// Cold start with a lookup already running. The mirror of
// CommentsUpdatedFiresWhenOnlyTheKadFlagClears: there the finished edge never
// arrived, here the starting state never arrives, and a ?channels=comments
// subscriber that joined after the download appeared would never learn a
// lookup was in flight.
TEST(EventDiff, CommentsUpdatedFiresWhenAFileArrivesMidKadLookup)
{
	CState state;
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 44;
		f.hash = "8888888888888888888888888888bbbb";
		f.name = "arrived-searching.iso";
		f.size = kPartSizeBytes;
		f.is_downloading = true;
		// No comments yet -- only the in-flight flag.
		f.download.kad_comment_searching = true;
		files.emplace(f.ecid, f);
	});

	CEventBus bus;
	LastSeenState prev;
	EmitDiffsAndUpdate(bus, prev, state);

	std::string payload;
	for (const auto &e : DrainAll(bus)) {
		if (e.name == "comments_updated")
			payload = e.data;
	}
	ASSERT_TRUE(!payload.empty());
	ASSERT_TRUE(payload.find("\"kad_comment_search_running\":true") != std::string::npos);
	ASSERT_TRUE(payload.find("\"count\":0") != std::string::npos);
}
