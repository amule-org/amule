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

#include "State.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <thread>
#include <vector>

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(State)

TEST(State, FreshHasNoSnapshot)
{
	CState s;
	ASSERT_FALSE(s.HasFirstSnapshot());
	ASSERT_FALSE(s.EcConnected());
	ASSERT_EQUALS(static_cast<std::time_t>(0), s.SnapshotAt());
}

TEST(State, MarkTickSuccessFlagsFreshness)
{
	CState s;
	const std::time_t before = std::time(nullptr);
	s.MarkTickSuccess();
	const std::time_t after = std::time(nullptr);

	ASSERT_TRUE(s.HasFirstSnapshot());
	ASSERT_TRUE(s.EcConnected());
	ASSERT_TRUE(s.SnapshotAt() >= before);
	ASSERT_TRUE(s.SnapshotAt() <= after);
}

TEST(State, MarkTickFailurePreservesSnapshotAt)
{
	CState s;
	s.MarkTickSuccess();
	const std::time_t first_snapshot_at = s.SnapshotAt();

	// Sleep a beat so a "snapshot_at = now" regression on
	// MarkTickFailure would visibly change the value.
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));

	s.MarkTickFailure();
	ASSERT_FALSE(s.EcConnected());
	// HasFirstSnapshot stays true — we have stale data, but data nonetheless.
	ASSERT_TRUE(s.HasFirstSnapshot());
	ASSERT_EQUALS(first_snapshot_at, s.SnapshotAt());
}

TEST(State, WriteStatusRoundtrip)
{
	CState s;
	StatusSnapshot in;
	in.ed2k_state = "connected";
	in.kad_state = "connecting";
	in.ed2k_lowid = true;
	in.kad_firewalled = false;
	in.server_name = "Some Server";
	in.server_ip = "192.0.2.42";
	in.server_port = 4242;
	in.download_bps = 12345;
	in.upload_bps = 6789;
	in.ul_queue_len = 3;
	in.total_src_count = 17;
	s.WriteStatus(in);

	const StatusSnapshot out = s.Status();
	ASSERT_EQUALS(std::string("connected"), out.ed2k_state);
	ASSERT_EQUALS(std::string("connecting"), out.kad_state);
	ASSERT_TRUE(out.ed2k_lowid);
	ASSERT_FALSE(out.kad_firewalled);
	ASSERT_EQUALS(std::string("Some Server"), out.server_name);
	ASSERT_EQUALS(std::string("192.0.2.42"), out.server_ip);
	ASSERT_EQUALS(static_cast<std::uint32_t>(4242), out.server_port);
	ASSERT_EQUALS(static_cast<std::uint64_t>(12345), out.download_bps);
	ASSERT_EQUALS(static_cast<std::uint64_t>(6789), out.upload_bps);
	ASSERT_EQUALS(static_cast<std::uint32_t>(3), out.ul_queue_len);
	ASSERT_EQUALS(static_cast<std::uint32_t>(17), out.total_src_count);
}

TEST(State, MutateDownloadsRoundtripAndFind)
{
	CState s;
	s.MutateDownloads([](FileMap &cache) {
		FileSnapshot a;
		a.ecid = 100;
		a.hash = "aaaa0000aaaa0000aaaa0000aaaa0000";
		a.name = "foo.iso";
		a.size = 1000;
		a.is_downloading = true;
		a.download.size_done = 250;
		a.download.priority = "high";
		a.download.status = "downloading";
		a.download.percent = 25.0;
		cache.emplace(a.ecid, a);

		FileSnapshot b;
		b.ecid = 200;
		b.hash = "bbbb1111bbbb1111bbbb1111bbbb1111";
		b.name = "bar.iso";
		b.is_downloading = true;
		cache.emplace(b.ecid, b);
	});

	// Both entries should be present in the vector view. Order is
	// unordered_map-bucket-defined (FileMap drops std::map's ECID
	// ordering), so look entries up by ECID instead of position.
	const auto out = s.Downloads();
	ASSERT_EQUALS(static_cast<size_t>(2), out.size());
	std::string foo_name, bar_name;
	for (const auto &f : out) {
		if (f.ecid == 100)
			foo_name = f.name;
		if (f.ecid == 200)
			bar_name = f.name;
	}
	ASSERT_EQUALS(std::string("foo.iso"), foo_name);
	ASSERT_EQUALS(std::string("bar.iso"), bar_name);

	// Hash lookup goes through FindDownload's linear scan; both hits
	// and misses must come back correctly.
	FileSnapshot found;
	ASSERT_TRUE(s.FindDownload("bbbb1111bbbb1111bbbb1111bbbb1111", found));
	ASSERT_EQUALS(std::string("bar.iso"), found.name);
	ASSERT_EQUALS(static_cast<std::uint32_t>(200), found.ecid);

	FileSnapshot miss;
	ASSERT_FALSE(s.FindDownload("0000000000000000000000000000000c", miss));
}

TEST(State, MutateDownloadsDecodedRleFieldsRoundtrip)
{
	// `decoded_gaps` + `decoded_part_sources` are populated by the
	// refresher's stateful RLE decoder pass. CState just
	// stores and surfaces them; this test pins that the per-part
	// arrays survive the MutateDownloads → Downloads()/FindDownload
	// roundtrip with element-level fidelity. Regression would manifest
	// as `progress.parts` being empty or wrong-sized on the wire.
	CState s;
	s.MutateDownloads([](FileMap &cache) {
		FileSnapshot a;
		a.ecid = 42;
		a.hash = "dddd3333dddd3333dddd3333dddd3333";
		a.name = "with-rle.iso";
		a.size = 9728000ull * 3; // exactly 3 parts
		a.is_downloading = true;
		// One gap covering byte ranges 100..200 and 9728000..9800000:
		// the first lies entirely in part 0, the second entirely in
		// part 1.
		a.download.decoded_gaps = { 100ull, 200ull, 9728000ull, 9800000ull };
		// Three parts with source counts [5, 0, 7].
		a.download.decoded_part_sources = { 5, 0, 7 };
		cache.emplace(a.ecid, a);
	});

	const auto out = s.Downloads();
	ASSERT_EQUALS(static_cast<size_t>(1), out.size());
	ASSERT_EQUALS(static_cast<size_t>(4), out[0].download.decoded_gaps.size());
	ASSERT_EQUALS(static_cast<std::uint64_t>(100), out[0].download.decoded_gaps[0]);
	ASSERT_EQUALS(static_cast<std::uint64_t>(200), out[0].download.decoded_gaps[1]);
	ASSERT_EQUALS(static_cast<std::uint64_t>(9728000), out[0].download.decoded_gaps[2]);
	ASSERT_EQUALS(static_cast<std::uint64_t>(9800000), out[0].download.decoded_gaps[3]);
	ASSERT_EQUALS(static_cast<size_t>(3), out[0].download.decoded_part_sources.size());
	ASSERT_EQUALS(static_cast<std::uint16_t>(5), out[0].download.decoded_part_sources[0]);
	ASSERT_EQUALS(static_cast<std::uint16_t>(0), out[0].download.decoded_part_sources[1]);
	ASSERT_EQUALS(static_cast<std::uint16_t>(7), out[0].download.decoded_part_sources[2]);

	// FindDownload returns the same surface (used by the detail
	// endpoint, which is the only path that emits progress.parts).
	FileSnapshot via_find;
	ASSERT_TRUE(s.FindDownload("dddd3333dddd3333dddd3333dddd3333", via_find));
	ASSERT_EQUALS(static_cast<size_t>(4), via_find.download.decoded_gaps.size());
	ASSERT_EQUALS(static_cast<size_t>(3), via_find.download.decoded_part_sources.size());
	ASSERT_EQUALS(static_cast<std::uint16_t>(7), via_find.download.decoded_part_sources[2]);
}

TEST(State, MutateClientsAndSharedRoundtrip)
{
	CState s;
	// m_clients is the unified peer cache (all upload_state
	// values). /clients endpoint surfaces the full set; consumers
	// filter by role on their side.
	s.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &cache) {
		ClientSnapshot c;
		c.ecid = 10;
		c.client_name = "peer-1";
		c.upload_state = "uploading";
		c.upload_speed_bps = 1234;
		cache.emplace(c.ecid, c);
	});
	ASSERT_EQUALS(static_cast<size_t>(1), s.Clients().size());
	ASSERT_EQUALS(std::string("peer-1"), s.Clients()[0].client_name);
	ASSERT_EQUALS(std::string("uploading"), s.Clients()[0].upload_state);

	s.MutateShared([](FileMap &cache) {
		FileSnapshot x;
		x.ecid = 20;
		x.hash = "ffff2222ffff2222ffff2222ffff2222";
		x.name = "shared.iso";
		x.size = 4096;
		x.is_shared = true;
		x.shared.priority = "normal";
		cache.emplace(x.ecid, x);
	});
	ASSERT_EQUALS(static_cast<size_t>(1), s.Shared().size());
	ASSERT_EQUALS(std::string("shared.iso"), s.Shared()[0].name);
}

TEST(State, WriteKadAndPreferencesRoundtrip)
{
	CState s;

	KadSnapshot k;
	k.state = "connected";
	k.users = 12345;
	k.firewalled = true;
	k.ip = "1.2.3.4";
	s.WriteKad(k);

	PreferencesSnapshot p;
	p.nickname = "tester";
	p.user_hash = "deadbeefdeadbeefdeadbeefdeadbeef";
	p.tcp_port = 4662;
	p.udp_port = 4672;
	p.network_ed2k = true;
	s.WritePreferences(p);

	std::vector<CategorySnapshot> cats;
	{
		CategorySnapshot c;
		c.index = 0;
		c.name = "All";
		c.priority = "auto";
		cats.push_back(c);
	}
	{
		CategorySnapshot c;
		c.index = 1;
		c.name = "Movies";
		c.path = "/tmp/movies";
		c.priority = "high";
		cats.push_back(c);
	}
	s.WriteCategories(cats);

	const auto k_out = s.Kad();
	ASSERT_EQUALS(std::string("connected"), k_out.state);
	ASSERT_EQUALS(static_cast<std::uint32_t>(12345), k_out.users);
	ASSERT_TRUE(k_out.firewalled);

	const auto p_out = s.Preferences();
	ASSERT_EQUALS(std::string("tester"), p_out.nickname);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4662), p_out.tcp_port);
	ASSERT_TRUE(p_out.network_ed2k);
	ASSERT_FALSE(p_out.network_kad);

	const auto c_out = s.Categories();
	ASSERT_EQUALS(static_cast<size_t>(2), c_out.size());
	ASSERT_EQUALS(std::string("All"), c_out[0].name);
	ASSERT_EQUALS(std::string("Movies"), c_out[1].name);
}

TEST(State, WriteServersRoundtripAndOrder)
{
	CState s;
	s.MutateServers([](std::map<std::uint32_t, ServerSnapshot> &cache) {
		ServerSnapshot a;
		a.ecid = 200;
		a.name = "second-by-ecid";
		cache.emplace(a.ecid, a);

		ServerSnapshot b;
		b.ecid = 100;
		b.name = "first-by-ecid";
		cache.emplace(b.ecid, b);
	});

	// std::map iterates ECID-ascending — the Servers() vector view
	// inherits that ordering so the wire response is stable across
	// refresher ticks.
	const auto out = s.Servers();
	ASSERT_EQUALS(static_cast<size_t>(2), out.size());
	ASSERT_EQUALS(std::string("first-by-ecid"), out[0].name);
	ASSERT_EQUALS(std::string("second-by-ecid"), out[1].name);
}

TEST(State, AppendAmuleLogUncappedHistory)
{
	// Per-operator preference: amule log history is uncapped. Pushing
	// thousands of lines must NOT trigger any trimming — operators
	// rely on the full record being available for triage. A future
	// `DELETE /logs/amule` mutation is the only intentional truncation
	// path; until that lands, history grows monotonically.
	CState s;
	{
		std::vector<std::string> first_batch;
		for (int i = 0; i < 1000; ++i) {
			first_batch.push_back("first-" + std::to_string(i));
		}
		s.AppendAmuleLog(std::move(first_batch));
	}
	ASSERT_EQUALS(static_cast<size_t>(1000), s.AmuleLog().size());

	{
		std::vector<std::string> second_batch;
		for (int i = 0; i < 1000; ++i) {
			second_batch.push_back("second-" + std::to_string(i));
		}
		s.AppendAmuleLog(std::move(second_batch));
	}
	const auto out = s.AmuleLog();
	ASSERT_EQUALS(static_cast<size_t>(2000), out.size());
	// Oldest-first preserved.
	ASSERT_EQUALS(std::string("first-0"), out[0]);
	ASSERT_EQUALS(std::string("first-999"), out[999]);
	ASSERT_EQUALS(std::string("second-0"), out[1000]);
	ASSERT_EQUALS(std::string("second-999"), out[1999]);
}

TEST(State, WriteServerInfoRoundtrip)
{
	CState s;
	ServerInfoLog in;
	in.text = "server hello\nline 2\nline 3\n";
	s.WriteServerInfo(in);

	const auto out = s.ServerInfo();
	ASSERT_EQUALS(in.text, out.text);

	// Overwrite semantics: a second write replaces (it doesn't
	// append) — ServerInfoLog is amuled's full-snapshot text, not
	// an incremental cursor.
	ServerInfoLog replacement;
	replacement.text = "totally different\n";
	s.WriteServerInfo(replacement);
	ASSERT_EQUALS(std::string("totally different\n"), s.ServerInfo().text);
}

TEST(State, WriteStatsTreeRoundtripRecursive)
{
	CState s;
	StatsTreeNode root;
	root.label = "root";
	{
		StatsTreeNode child;
		child.label = "Transfer";
		{
			StatsTreeNode grand;
			grand.label = "Total bytes transferred: 12.3 GiB";
			child.children.push_back(grand);
		}
		root.children.push_back(child);
	}
	{
		StatsTreeNode sib;
		sib.label = "Connection";
		root.children.push_back(sib);
	}
	s.WriteStatsTree(root);

	const StatsTreeNode out = s.StatsTree();
	ASSERT_EQUALS(std::string("root"), out.label);
	ASSERT_EQUALS(static_cast<size_t>(2), out.children.size());
	ASSERT_EQUALS(std::string("Transfer"), out.children[0].label);
	ASSERT_EQUALS(static_cast<size_t>(1), out.children[0].children.size());
	ASSERT_EQUALS(std::string("Total bytes transferred: 12.3 GiB"), out.children[0].children[0].label);
	ASSERT_EQUALS(std::string("Connection"), out.children[1].label);
}

TEST(State, WriteGraphsRoundtripAllSeries)
{
	CState s;
	StatsGraphs g;
	g.interval_seconds = 1;
	g.download_bps = { 100, 200, 300 };
	g.upload_bps = { 10, 20, 30 };
	g.connections = { 1, 2, 3 };
	g.kad_nodes = { 500, 600, 700 };
	g.session_download_bytes = 1024;
	g.session_upload_bytes = 256;
	g.session_kad_bytes = 4096;
	s.WriteGraphs(g);

	const StatsGraphs out = s.Graphs();
	ASSERT_EQUALS(static_cast<std::uint32_t>(1), out.interval_seconds);
	ASSERT_EQUALS(static_cast<size_t>(3), out.download_bps.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(300), out.download_bps[2]);
	ASSERT_EQUALS(static_cast<std::uint32_t>(700), out.kad_nodes[2]);
	ASSERT_EQUALS(static_cast<std::uint64_t>(1024), out.session_download_bytes);
	ASSERT_EQUALS(static_cast<std::uint64_t>(4096), out.session_kad_bytes);
}

TEST(State, SearchResultsRoundtripAndOrderByEcid)
{
	CState s;
	// Multi-search: a slot must exist before it can be mutated/read. Seed one
	// (its id becomes the current search) and populate it.
	const std::uint32_t sid = 1;
	s.MarkSearchStarted(sid, "global");
	s.MutateSearch(sid, [](std::map<std::uint32_t, SearchResult> &cache) {
		SearchResult a;
		a.ecid = 50;
		a.hash = "aaaa0000aaaa0000aaaa0000aaaa0000";
		a.name = "ascii-name.iso";
		a.size = 10000;
		a.source_count = 12;
		cache.emplace(a.ecid, a);

		SearchResult b;
		b.ecid = 25;
		b.hash = "bbbb1111bbbb1111bbbb1111bbbb1111";
		b.name = "first-by-ecid.iso";
		b.size = 7000;
		b.complete_source_count = 5;
		b.already_have = true;
		cache.emplace(b.ecid, b);
	});

	// std::map iterates ECID-ascending → Search() vector is sorted. Read by
	// explicit id and via the no-id default (0 == current search) — both hit
	// the same slot.
	for (std::uint32_t query : { sid, std::uint32_t{ 0 } }) {
		const auto out = s.Search(query);
		ASSERT_EQUALS(static_cast<size_t>(2), out.size());
		ASSERT_EQUALS(std::string("first-by-ecid.iso"), out[0].name);
		ASSERT_EQUALS(std::string("ascii-name.iso"), out[1].name);
		ASSERT_TRUE(out[0].already_have);
		ASSERT_FALSE(out[1].already_have);
	}
}

TEST(State, MultiSearchSlotsAreIndependentAndAddressable)
{
	CState s;
	// No search yet: current is 0, nothing is known, reads are empty.
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.CurrentSearchId());
	ASSERT_FALSE(s.HasSearch(42));
	ASSERT_TRUE(s.Search(0).empty());

	// Two concurrent searches, each with its own result.
	s.MarkSearchStarted(10, "global");
	s.MutateSearch(10, [](std::map<std::uint32_t, SearchResult> &cache) {
		SearchResult r;
		r.ecid = 1;
		r.name = "in-ten.iso";
		cache.emplace(r.ecid, r);
	});
	s.MarkSearchStarted(20, "kad");
	s.MutateSearch(20, [](std::map<std::uint32_t, SearchResult> &cache) {
		SearchResult r;
		r.ecid = 2;
		r.name = "in-twenty.iso";
		cache.emplace(r.ecid, r);
	});

	// Most-recently-started search is current; no-id resolves to it.
	ASSERT_EQUALS(static_cast<std::uint32_t>(20), s.CurrentSearchId());
	ASSERT_EQUALS(std::string("in-twenty.iso"), s.Search(0).at(0).name);

	// Each id addresses only its own results — no cross-contamination.
	ASSERT_EQUALS(static_cast<size_t>(1), s.Search(10).size());
	ASSERT_EQUALS(std::string("in-ten.iso"), s.Search(10).at(0).name);
	ASSERT_EQUALS(std::string("in-twenty.iso"), s.Search(20).at(0).name);
	ASSERT_TRUE(s.HasSearch(10));
	ASSERT_TRUE(s.HasSearch(20));
	ASSERT_FALSE(s.HasSearch(30));

	// Progress kind is per-slot.
	ASSERT_EQUALS(std::string("global"), s.SearchProgress(10).kind);
	ASSERT_EQUALS(std::string("kad"), s.SearchProgress(20).kind);

	// Closing the current search drops its slot and clears current (no reliable
	// "next newest" once it is gone).
	s.CloseSearch(20);
	ASSERT_FALSE(s.HasSearch(20));
	ASSERT_TRUE(s.HasSearch(10));
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.CurrentSearchId());
	ASSERT_TRUE(s.Search(20).empty());
	ASSERT_EQUALS(static_cast<size_t>(1), s.Search(10).size());
}

TEST(State, BrowseRidesSearchMachinery)
{
	// A "View Files" browse (POST /clients/{ecid}/shared_files) is filed
	// under a search_id with kind "browse" and its files land in the same
	// per-slot result cache as a query search — the refresher, /search/results
	// and the SSE search channel all treat it identically. Lock that in: the
	// browse kind is preserved per-slot and its results address only its own id.
	CState s;
	s.MarkSearchStarted(17, "browse");
	s.MutateSearch(17, [](std::map<std::uint32_t, SearchResult> &cache) {
		SearchResult r;
		r.ecid = 1;
		r.name = "peer-shared.iso";
		cache.emplace(r.ecid, r);
	});
	ASSERT_TRUE(s.HasSearch(17));
	ASSERT_EQUALS(std::string("browse"), s.SearchProgress(17).kind);
	ASSERT_EQUALS(static_cast<size_t>(1), s.Search(17).size());
	ASSERT_EQUALS(std::string("peer-shared.iso"), s.Search(17).at(0).name);
}

TEST(State, ResetListsLeavesLogsAlone)
{
	// Logs survive an EC reconnect on purpose — the operator can see
	// "EC disconnected at HH:MM" alongside earlier traffic. ResetLists
	// must not nuke either log buffer.
	CState s;
	s.AppendAmuleLog({ "persistent line" });
	s.WriteServerInfo({ "persistent server info" });
	s.ResetLists();
	ASSERT_EQUALS(static_cast<size_t>(1), s.AmuleLog().size());
	ASSERT_EQUALS(std::string("persistent server info"), s.ServerInfo().text);
}

TEST(State, ResetListsClearsAll)
{
	CState s;
	s.MutateDownloads([](FileMap &cache) {
		FileSnapshot d;
		d.ecid = 1;
		d.name = "a";
		d.is_downloading = true;
		cache.emplace(1, d);
	});
	s.MutateClients([](std::map<std::uint32_t, ClientSnapshot> &cache) {
		ClientSnapshot c;
		c.ecid = 1;
		c.client_name = "b";
		cache.emplace(1, c);
	});
	s.MutateShared([](FileMap &cache) {
		// Same ECID; sets is_shared on the existing entry rather than
		// creating a new map slot, matching the unified-map model.
		auto it = cache.find(1);
		if (it == cache.end()) {
			FileSnapshot x;
			x.ecid = 1;
			x.name = "c";
			x.is_shared = true;
			cache.emplace(1, x);
		} else {
			it->second.is_shared = true;
		}
	});
	ASSERT_EQUALS(static_cast<size_t>(1), s.Downloads().size());
	ASSERT_EQUALS(static_cast<size_t>(1), s.Clients().size());
	ASSERT_EQUALS(static_cast<size_t>(1), s.Shared().size());

	s.ResetLists();
	ASSERT_EQUALS(static_cast<size_t>(0), s.Downloads().size());
	ASSERT_EQUALS(static_cast<size_t>(0), s.Clients().size());
	ASSERT_EQUALS(static_cast<size_t>(0), s.Shared().size());
}

TEST(State, ConcurrentReadersDontTearSnapshot)
{
	// Spin up 4 readers + 1 writer for 100ms. The writer churns
	// distinct snapshot values; readers verify they always observe
	// a *self-consistent* snapshot (the four numeric fields below
	// are written under one unique_lock, so a shared_lock reader
	// must see them all from the same generation). A teared read
	// would manifest as a mismatched (download_bps, upload_bps)
	// pair, which we then assert against.

	CState s;
	std::atomic<bool> stop{ false };
	std::atomic<int> observed{ 0 };
	std::atomic<int> torn{ 0 };

	std::thread writer([&] {
		std::uint64_t gen = 1;
		while (!stop.load()) {
			StatusSnapshot v;
			v.download_bps = gen;
			v.upload_bps = gen * 2;
			v.ul_queue_len = static_cast<std::uint32_t>(gen & 0xffffffff);
			v.total_src_count = static_cast<std::uint32_t>(gen & 0xffffffff);
			s.WriteStatus(v);
			++gen;
		}
	});

	std::vector<std::thread> readers;
	for (int i = 0; i < 4; ++i) {
		readers.emplace_back([&] {
			while (!stop.load()) {
				StatusSnapshot r = s.Status();
				observed.fetch_add(1);
				// Invariants enforced by the writer's single
				// unique_lock acquisition: upload_bps == 2 *
				// download_bps; ul_queue_len == total_src_count.
				if (r.upload_bps != 2 * r.download_bps)
					torn.fetch_add(1);
				if (r.ul_queue_len != r.total_src_count)
					torn.fetch_add(1);
			}
		});
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	stop.store(true);
	writer.join();
	for (auto &t : readers)
		t.join();

	// Sanity: the loop actually exercised the contention path. A bar
	// of `> 0` passes with a single observation, which a debug build
	// or an over-loaded CI runner could plausibly produce — leaving
	// the tear-detection harness inactive while the test still
	// reports green. Require a meaningful number of reads instead;
	// even a slow runner does ~10K reads per shared_lock-protected
	// field in 100ms (single uncontended read is sub-microsecond),
	// and torn-read detection needs many reads to catch the
	// boundary anyway.
	ASSERT_TRUE(observed.load() > 1000);
	// And no read saw a torn snapshot.
	ASSERT_EQUALS(0, torn.load());
}
