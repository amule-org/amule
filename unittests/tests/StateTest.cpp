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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace muleunit;
using namespace webapi;

namespace
{
// Stand-ins for the role-filtered accessors CState no longer exposes; the
// assertions below just want a countable, indexable list.
std::vector<FileSnapshot> RoleView(const CState &s, bool FileSnapshot::*role)
{
	std::vector<FileSnapshot> out;
	s.WithFiles([&](const FileMap &files) {
		for (const auto &entry : files) {
			if (entry.second.*role)
				out.push_back(entry.second);
		}
	});
	return out;
}

std::vector<FileSnapshot> Downloads(const CState &s)
{
	return RoleView(s, &FileSnapshot::is_downloading);
}

std::vector<FileSnapshot> Shared(const CState &s)
{
	return RoleView(s, &FileSnapshot::is_shared);
}
} // namespace

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
	in.ed2k_high_id = true;
	in.ed2k_id = 1234567890u; // 210.2.150.73 packed LSB-first
	in.ed2k_public_ip = "210.2.150.73";
	in.download_overhead_bps = 8700;
	in.upload_overhead_bps = 1100;
	in.temp_free_bytes = 48318382080LL;
	in.incoming_free_bytes = -1; // unknown
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
	ASSERT_TRUE(out.ed2k_high_id);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1234567890u), out.ed2k_id);
	ASSERT_EQUALS(std::string("210.2.150.73"), out.ed2k_public_ip);
	ASSERT_EQUALS(static_cast<std::uint64_t>(8700), out.download_overhead_bps);
	ASSERT_EQUALS(static_cast<std::uint64_t>(1100), out.upload_overhead_bps);
	ASSERT_EQUALS(static_cast<std::int64_t>(48318382080LL), out.temp_free_bytes);
	// -1 must survive the round trip as -1: it is what the handler turns
	// into JSON null, and an unsigned slot would make it 1.8e19.
	ASSERT_EQUALS(static_cast<std::int64_t>(-1), out.incoming_free_bytes);
	ASSERT_FALSE(out.kad_firewalled);
	ASSERT_EQUALS(std::string("Some Server"), out.server_name);
	ASSERT_EQUALS(std::string("192.0.2.42"), out.server_ip);
	ASSERT_EQUALS(static_cast<std::uint32_t>(4242), out.server_port);
	ASSERT_EQUALS(static_cast<std::uint64_t>(12345), out.download_bps);
	ASSERT_EQUALS(static_cast<std::uint64_t>(6789), out.upload_bps);
	ASSERT_EQUALS(static_cast<std::uint32_t>(3), out.ul_queue_len);
	ASSERT_EQUALS(static_cast<std::uint32_t>(17), out.total_src_count);
}

TEST(State, FileMapEmplaceFilesTheSnapshotUnderItsKey)
{
	// The clients walker resolves an amuled ECID with find() and then reads
	// the snapshot it gets back, so the key and FileSnapshot::ecid have to
	// agree. emplace() makes them agree instead of trusting the caller: a
	// snapshot carrying the wrong id would otherwise break /clients silently.
	CState s;
	s.MutateDownloads([](FileMap &cache) {
		FileSnapshot f;
		f.ecid = 999; // stale / wrong -- the key is what readers look up by
		f.hash = "cccc2222cccc2222cccc2222cccc2222";
		f.is_downloading = true;
		cache.emplace(42, std::move(f));
	});
	s.WithFiles([](const FileMap &files) {
		const auto it = files.find(42);
		ASSERT_TRUE(it != files.end());
		ASSERT_EQUALS(static_cast<std::uint32_t>(42), it->second.ecid);
		ASSERT_TRUE(files.find(999) == files.end());
		// The hash index is keyed off the same insert, so it agrees too.
		std::uint32_t by_hash = 0;
		ASSERT_TRUE(files.FindEcidByHash("cccc2222cccc2222cccc2222cccc2222", by_hash));
		ASSERT_EQUALS(static_cast<std::uint32_t>(42), by_hash);
	});
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
	const auto out = Downloads(s);
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

	const auto out = Downloads(s);
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

// FileSnapshot::IsIncompletePartfile() decides two things: whether
// /shared/{hash} reports `incomplete`, and whether "verify local data" is
// rejected as unsupported. Both want "genuinely still a partfile", which is
// not the same question as "is in the download queue".
TEST(State, IncompletePartfileIsFalseForAPureShare)
{
	FileSnapshot f;
	f.is_shared = true;
	f.is_downloading = false;
	// A file that was never downloaded here has no download side at all, so
	// the status string is empty rather than "completed".
	ASSERT_TRUE(f.download.status.empty());
	ASSERT_FALSE(f.IsIncompletePartfile());
}

TEST(State, IncompletePartfileIsTrueWhileDownloading)
{
	FileSnapshot f;
	f.is_downloading = true;
	f.download.status = "downloading";
	ASSERT_TRUE(f.IsIncompletePartfile());
}

// The case the flag exists for: a finished download stays in the queue, with
// is_downloading still set, until the user clears it -- but by then it is a
// knownfile whose data is in the destination directory. Reporting it as an
// incomplete partfile would be wrong, and would also reject a legitimate
// verify target.
TEST(State, IncompletePartfileIsFalseForCompletedButNotCleared)
{
	FileSnapshot f;
	f.is_downloading = true;
	f.download.status = "completed";
	ASSERT_FALSE(f.IsIncompletePartfile());
}

// A paused or stopped partfile is still an incomplete partfile: pausing
// changes whether it transfers, not whether its data is whole. Same for every
// other non-completed state the wire exposes, including "completing" -- the
// data lives in the temp directory until that move finishes.
TEST(State, IncompletePartfileIsTrueForEveryNonCompletedStatus)
{
	static const char *const kIncompleteStates[] = { "downloading",
		"paused",
		"stopped",
		"completing",
		"hashing",
		"waiting",
		"allocating",
		"erroneous",
		"insufficient_disk",
		"unknown" };
	for (const char *status : kIncompleteStates) {
		FileSnapshot f;
		f.is_downloading = true;
		f.download.status = status;
		ASSERT_TRUE(f.IsIncompletePartfile());
	}
}

// Only the exact wire string counts. The check is against "completed", not a
// prefix or a substring, so the neighbouring "completing" state must not be
// swept in with it.
TEST(State, IncompletePartfileDistinguishesCompletingFromCompleted)
{
	FileSnapshot completing;
	completing.is_downloading = true;
	completing.download.status = "completing";
	FileSnapshot completed;
	completed.is_downloading = true;
	completed.download.status = "completed";
	ASSERT_TRUE(completing.IsIncompletePartfile());
	ASSERT_FALSE(completed.IsIncompletePartfile());
}

// is_downloading is the gate: a shared knownfile never in the queue is
// complete whatever the download side happens to hold.
TEST(State, IncompletePartfileRequiresBeingInTheDownloadQueue)
{
	FileSnapshot f;
	f.is_shared = true;
	f.is_downloading = false;
	f.download.status = "downloading";
	ASSERT_FALSE(f.IsIncompletePartfile());
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
	ASSERT_EQUALS(static_cast<size_t>(1), Shared(s).size());
	ASSERT_EQUALS(std::string("shared.iso"), Shared(s)[0].name);
}

TEST(State, WriteKadAndPreferencesRoundtrip)
{
	CState s;

	KadSnapshot k;
	k.state = "connected";
	k.users = 12345;
	k.firewalled = true;
	k.public_ip = "1.2.3.4";
	k.node_id = "8f3a1c07d94b2e5a6018bb4c7f209d3e";
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
	// The two string fields were written but never read back, so a
	// rename could pass this test while dropping the value.
	ASSERT_EQUALS(std::string("1.2.3.4"), k_out.public_ip);
	ASSERT_EQUALS(std::string("8f3a1c07d94b2e5a6018bb4c7f209d3e"), k_out.node_id);

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

TEST(State, AmuleLogFromReportsTotalAndSlicesTheTail)
{
	// The per-tick log diff reads the size and the tail in one lock, so both
	// halves of that are contract: `total` is always the current line count,
	// and the returned slice starts at `first`.
	CState s;
	std::size_t total = 0;

	// Empty: no lines, and a total of zero rather than an unset out-param.
	ASSERT_EQUALS(static_cast<size_t>(0), s.AmuleLogFrom(0, total).size());
	ASSERT_EQUALS(static_cast<size_t>(0), total);

	s.AppendAmuleLog({ "a", "b", "c" });

	// first == 0: the whole buffer.
	total = 0;
	const auto all = s.AmuleLogFrom(0, total);
	ASSERT_EQUALS(static_cast<size_t>(3), all.size());
	ASSERT_EQUALS(static_cast<size_t>(3), total);
	ASSERT_EQUALS(std::string("a"), all[0]);
	ASSERT_EQUALS(std::string("c"), all[2]);

	// Mid-buffer: the tail from there on.
	total = 0;
	const auto tail = s.AmuleLogFrom(2, total);
	ASSERT_EQUALS(static_cast<size_t>(1), tail.size());
	ASSERT_EQUALS(std::string("c"), tail[0]);
	ASSERT_EQUALS(static_cast<size_t>(3), total);

	// first == total: caught up, nothing to publish, total still reported.
	total = 0;
	ASSERT_EQUALS(static_cast<size_t>(0), s.AmuleLogFrom(3, total).size());
	ASSERT_EQUALS(static_cast<size_t>(3), total);

	// first > total: past the end truncates to empty rather than reading out
	// of range. This is the shape the caller sees after a reset shrank the
	// buffer below the counter it was holding.
	total = 0;
	ASSERT_EQUALS(static_cast<size_t>(0), s.AmuleLogFrom(99, total).size());
	ASSERT_EQUALS(static_cast<size_t>(3), total);
}

TEST(State, AmuleLogFromAfterAResetReportsTheShrunkTotal)
{
	// ClearAmuleLog is the one path that shrinks the buffer. A caller still
	// holding the pre-reset count must get an empty slice and the new, smaller
	// total -- that pairing is what tells the log diff a truncation happened
	// rather than an append.
	CState s;
	s.AppendAmuleLog({ "one", "two", "three", "four" });
	std::size_t total = 0;
	s.AmuleLogFrom(0, total);
	ASSERT_EQUALS(static_cast<size_t>(4), total);

	s.ClearAmuleLog();
	total = 99;
	ASSERT_EQUALS(static_cast<size_t>(0), s.AmuleLogFrom(4, total).size());
	ASSERT_EQUALS(static_cast<size_t>(0), total);

	// And it appends from scratch afterwards.
	s.AppendAmuleLog({ "fresh" });
	total = 0;
	const auto after = s.AmuleLogFrom(0, total);
	ASSERT_EQUALS(static_cast<size_t>(1), after.size());
	ASSERT_EQUALS(std::string("fresh"), after[0]);
	ASSERT_EQUALS(static_cast<size_t>(1), total);
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
	g.active_uploads = { 4, 5, 6 };
	g.active_downloads = { 7, 8, 9 };
	g.max_points = 1800;
	g.session_download_bytes = 1024;
	g.session_upload_bytes = 256;
	g.session_kad_node_seconds = 4096;
	g.session_duration_seconds = 3600.0;
	s.WriteGraphs(g);

	const StatsGraphs out = s.Graphs();
	ASSERT_EQUALS(static_cast<std::uint32_t>(1), out.interval_seconds);
	ASSERT_EQUALS(static_cast<size_t>(3), out.download_bps.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(300), out.download_bps[2]);
	ASSERT_EQUALS(static_cast<std::uint32_t>(700), out.kad_nodes[2]);
	// The second data blob rides along point-aligned with the first.
	ASSERT_EQUALS(static_cast<size_t>(3), out.active_uploads.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(6), out.active_uploads[2]);
	ASSERT_EQUALS(static_cast<std::uint32_t>(9), out.active_downloads[2]);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1800), out.max_points);
	ASSERT_EQUALS(static_cast<std::uint64_t>(1024), out.session_download_bytes);
	ASSERT_EQUALS(static_cast<std::uint64_t>(4096), out.session_kad_node_seconds);
	ASSERT_EQUALS(3600.0, out.session_duration_seconds);
}

// An amuled predating EC_TAG_STATSGRAPH_DATA_CONN sends the first blob and
// not the second. The two extra series must come back empty rather than
// zero-filled: the handler keys "omit the JSON fields" off exactly that, so
// a consumer can tell "not reported" from "nothing was transferring".
TEST(State, WriteGraphsWithoutConnBlobLeavesExtraSeriesEmpty)
{
	CState s;
	StatsGraphs g;
	g.download_bps = { 100, 200, 300 };
	g.connections = { 1, 2, 3 };
	s.WriteGraphs(g);

	const StatsGraphs out = s.Graphs();
	ASSERT_EQUALS(static_cast<size_t>(3), out.connections.size());
	ASSERT_TRUE(out.active_uploads.empty());
	ASSERT_TRUE(out.active_downloads.empty());
}

TEST(State, SearchResultsRoundtripAndOrderByEcid)
{
	CState s;
	// Multi-search: a slot must exist before it can be mutated/read.
	const std::uint32_t sid = 1;
	s.MarkSearchStarted(sid, "global", "ubuntu");
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

	// std::map iterates ECID-ascending → Search() vector is sorted.
	const auto out = s.Search(sid);
	ASSERT_EQUALS(static_cast<size_t>(2), out.size());
	ASSERT_EQUALS(std::string("first-by-ecid.iso"), out[0].name);
	ASSERT_EQUALS(std::string("ascii-name.iso"), out[1].name);
	ASSERT_TRUE(out[0].already_have);
	ASSERT_FALSE(out[1].already_have);
	// The query rides on the slot, so a results read can report what was
	// searched for without a second lookup.
	ASSERT_EQUALS(std::string("ubuntu"), s.SearchQuery(sid));
	// Id 0 is not a search and never resolves to one.
	ASSERT_TRUE(s.Search(0).empty());
	ASSERT_FALSE(s.HasSearch(0));
}

TEST(State, MultiSearchSlotsAreIndependentAndAddressable)
{
	CState s;
	// No search yet: nothing is known and reads are empty.
	ASSERT_FALSE(s.HasSearch(42));
	ASSERT_TRUE(s.Search(42).empty());

	// Two concurrent searches, each with its own result.
	s.MarkSearchStarted(10, "global", "ten");
	s.MutateSearch(10, [](std::map<std::uint32_t, SearchResult> &cache) {
		SearchResult r;
		r.ecid = 1;
		r.name = "in-ten.iso";
		cache.emplace(r.ecid, r);
	});
	s.MarkSearchStarted(20, "kad", "twenty");
	s.MutateSearch(20, [](std::map<std::uint32_t, SearchResult> &cache) {
		SearchResult r;
		r.ecid = 2;
		r.name = "in-twenty.iso";
		cache.emplace(r.ecid, r);
	});

	// Each id addresses only its own results — no cross-contamination, and
	// no implicit target that could make an unaddressed read mean either.
	ASSERT_EQUALS(static_cast<size_t>(1), s.Search(10).size());
	ASSERT_EQUALS(std::string("in-ten.iso"), s.Search(10).at(0).name);
	ASSERT_EQUALS(std::string("in-twenty.iso"), s.Search(20).at(0).name);
	ASSERT_TRUE(s.HasSearch(10));
	ASSERT_TRUE(s.HasSearch(20));
	ASSERT_FALSE(s.HasSearch(30));

	// Progress kind and query are per-slot.
	ASSERT_EQUALS(std::string("global"), s.SearchProgress(10).kind);
	ASSERT_EQUALS(std::string("kad"), s.SearchProgress(20).kind);
	ASSERT_EQUALS(std::string("ten"), s.SearchQuery(10));
	ASSERT_EQUALS(std::string("twenty"), s.SearchQuery(20));

	// Freeing one search leaves the other completely alone — the property
	// that makes DELETE /search/{id} safe to issue from one tab.
	s.CloseSearch(20);
	ASSERT_FALSE(s.HasSearch(20));
	ASSERT_TRUE(s.HasSearch(10));
	ASSERT_TRUE(s.Search(20).empty());
	ASSERT_EQUALS(static_cast<size_t>(1), s.Search(10).size());
	ASSERT_EQUALS(std::string("in-ten.iso"), s.Search(10).at(0).name);
}

TEST(State, SearchStartedAtStampsOnlyOurOwnSearches)
{
	// GET /search has no recency signal of its own: the daemon returns its
	// searches id-ascending and ships no timestamp, and id order is not
	// recency because Kad ids carry a high-bit mask and always sort above
	// ed2k ones. `started_at` is what a client ranks by instead -- and it
	// exists only for searches THIS session started.
	CState s;
	s.MarkSearchStarted(5, "global", "ubuntu");
	const std::time_t ours = s.SearchStartedAt(5);
	ASSERT_TRUE(ours != 0);

	// A search another client started, adopted through discovery, has no
	// start time we could know. 0 means "unknown", which is why the REST
	// layer omits the key rather than emitting a 1970 timestamp.
	s.MarkSearchDiscovered(2147483651u, "kad", "debian", /*active=*/true, /*complete=*/false);
	ASSERT_TRUE(s.HasSearch(2147483651u));
	ASSERT_EQUALS(static_cast<std::time_t>(0), s.SearchStartedAt(2147483651u));

	// An id nobody holds reports 0 too; HasSearch is what separates them.
	ASSERT_EQUALS(static_cast<std::time_t>(0), s.SearchStartedAt(999));
	ASSERT_FALSE(s.HasSearch(999));
}

TEST(State, DiscoveredSearchKeepsTheDaemonsLifecycleState)
{
	// Discovery used to seed every adopted search as active regardless of
	// what the daemon said. POST /search/{id}/more gates on exactly that,
	// so a FINISHED search adopted this way was accepted and answered 202
	// for a request amuled turns into a no-op.
	CState s;
	s.MarkSearchDiscovered(42, "kad", "debian", /*active=*/false, /*complete=*/true);
	const auto finished = s.SearchProgress(42);
	ASSERT_TRUE(!finished.active);
	ASSERT_TRUE(finished.complete);
	ASSERT_EQUALS(std::string("kad"), finished.kind);

	// A still-running one is seeded running, so the tick keeps polling it.
	s.MarkSearchDiscovered(43, "global", "ubuntu", /*active=*/true, /*complete=*/false);
	const auto running = s.SearchProgress(43);
	ASSERT_TRUE(running.active);
	ASSERT_TRUE(!running.complete);

	// A slot seeded inactive is not polled by the tick, so the read paths
	// have to be able to refresh it on demand -- otherwise adopting a
	// finished search would show an empty result list forever.
	ASSERT_TRUE(s.ClaimSearchRefresh(42, std::chrono::milliseconds(1000)));
	ASSERT_FALSE(s.ClaimSearchRefresh(43, std::chrono::milliseconds(1000)));
}

TEST(State, DiscoveredFinishedSearchReportsFullPercent)
{
	// A slot discovered as finished is never polled -- it is not in
	// ActiveSearchIds() -- so the percent has to be seeded at discovery or it
	// stays 0 for the life of the slot, contradicting the "finished" state
	// carried in the same envelope. A finished search is 100 by definition.
	CState s;
	s.MarkSearchDiscovered(2147483660u, "kad", "harry", /*active=*/false, /*complete=*/true);
	const auto finished = s.SearchProgress(2147483660u);
	ASSERT_TRUE(finished.complete);
	ASSERT_EQUALS(static_cast<std::uint32_t>(100), finished.percent);
}

TEST(State, DiscoveredRunningSearchKeepsZeroPercentAndIsPolled)
{
	// The running case must not be faked to 100: it is in ActiveSearchIds(),
	// so the very next tick overwrites the percent with the daemon's real
	// one. Seeding anything but 0 here would flash a wrong number for a tick.
	CState s;
	s.MarkSearchDiscovered(44, "kad", "ubuntu", /*active=*/true, /*complete=*/false);
	const auto running = s.SearchProgress(44);
	ASSERT_TRUE(!running.complete);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), running.percent);

	const auto active = s.ActiveSearchIds();
	ASSERT_TRUE(std::find(active.begin(), active.end(), 44u) != active.end());
}

TEST(State, DiscoveredSearchPrefersTheDaemonsReportedPercent)
{
	// Once the daemon reports a percent on its listing, that number wins over
	// the one derived from the lifecycle state -- which is the whole point of
	// carrying it: a running search adopted mid-ramp shows its real progress
	// from first sight instead of 0 until the next tick.
	CState s;
	s.MarkSearchDiscovered(50, "kad", "ubuntu", /*active=*/true, /*complete=*/false, 62);
	ASSERT_EQUALS(static_cast<std::uint32_t>(62), s.SearchProgress(50).percent);

	// A finished one likewise takes what the daemon says rather than assuming.
	s.MarkSearchDiscovered(51, "kad", "debian", /*active=*/false, /*complete=*/true, 100);
	ASSERT_EQUALS(static_cast<std::uint32_t>(100), s.SearchProgress(51).percent);
}

TEST(State, DiscoveredSearchFallsBackWhenTheDaemonReportsNoPercent)
{
	// -1 is "the listing carried no percent", i.e. a daemon older than the
	// tag. The derived fallback has to survive: finished is 100, running is 0
	// and gets corrected by the tick.
	CState s;
	s.MarkSearchDiscovered(52, "kad", "harry", /*active=*/false, /*complete=*/true, -1);
	ASSERT_EQUALS(static_cast<std::uint32_t>(100), s.SearchProgress(52).percent);

	s.MarkSearchDiscovered(53, "kad", "potter", /*active=*/true, /*complete=*/false, -1);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.SearchProgress(53).percent);
}

TEST(State, DiscoveryDoesNotStompAnAlreadyKnownSearchsPercent)
{
	// Re-discovery of a slot this session already tracks must leave its
	// accumulated progress alone; only the query is filled in. Seeding the
	// percent must not have opened a path that overwrites a real one.
	// The slot has to exist first: WriteSearchProgress only updates a slot
	// that is already there, so seeding it needs MarkSearchStarted (this
	// session started the search) before the progress write.
	CState s;
	s.MarkSearchStarted(45, "kad", "ubuntu");
	SearchProgressSnapshot p;
	p.active = true;
	p.complete = false;
	p.percent = 37;
	p.kind = "kad";
	s.WriteSearchProgress(45, p);
	ASSERT_EQUALS(static_cast<std::uint32_t>(37), s.SearchProgress(45).percent);

	// Re-discovery of that same id must leave the real percent alone.
	s.MarkSearchDiscovered(45, "kad", "named-later", /*active=*/false, /*complete=*/true);
	ASSERT_EQUALS(static_cast<std::uint32_t>(37), s.SearchProgress(45).percent);
}

TEST(State, SearchRefreshIsClaimedOncePerTtlAndOnlyWhenIdle)
{
	// The read paths refresh a FINISHED search on demand, because the tick
	// only polls active ones. ClaimSearchRefresh is what stops that from
	// turning every GET into an EC roundtrip, and what keeps two concurrent
	// readers of the same search to one roundtrip between them.
	CState s;
	const auto kTtl = std::chrono::milliseconds(1000);

	// Unknown id: nothing to refresh.
	ASSERT_FALSE(s.ClaimSearchRefresh(99, kTtl));

	// A running search is refreshed by the tick every second already, so the
	// read path must not add traffic on top of it.
	s.MarkSearchStarted(7, "global", "still running");
	ASSERT_FALSE(s.ClaimSearchRefresh(7, kTtl));

	// Once finished it has never been fetched by this path, so the first
	// caller gets the claim...
	SearchProgressSnapshot done = s.SearchProgress(7);
	done.active = false;
	done.complete = true;
	s.WriteSearchProgress(7, done);
	ASSERT_TRUE(s.ClaimSearchRefresh(7, kTtl));
	// ...and the next one inside the TTL does not, because the claim stamped
	// the slot rather than waiting for the fetch to finish.
	ASSERT_FALSE(s.ClaimSearchRefresh(7, kTtl));
	// A zero TTL means "always stale", which is how the test distinguishes
	// "coalesced" from "never refreshes again".
	ASSERT_TRUE(s.ClaimSearchRefresh(7, std::chrono::milliseconds(0)));
}

TEST(State, BrowseRidesSearchMachinery)
{
	// A "View Files" browse (POST /clients/{ecid}/shared_files) is filed
	// under a search_id with kind "browse" and its files land in the same
	// per-slot result cache as a query search — the refresher, /search/results
	// and the SSE search channel all treat it identically. Lock that in: the
	// browse kind is preserved per-slot and its results address only its own id.
	CState s;
	s.MarkSearchStarted(17, "browse", "SomePeerNick");
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
	// A browse's "query" is the peer whose share is listed, which is what the
	// daemon names the search and what GET /search reports for it.
	ASSERT_EQUALS(std::string("SomePeerNick"), s.SearchQuery(17));
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
	ASSERT_EQUALS(static_cast<size_t>(1), Downloads(s).size());
	ASSERT_EQUALS(static_cast<size_t>(1), s.Clients().size());
	ASSERT_EQUALS(static_cast<size_t>(1), Shared(s).size());

	s.ResetLists();
	ASSERT_EQUALS(static_cast<size_t>(0), Downloads(s).size());
	ASSERT_EQUALS(static_cast<size_t>(0), s.Clients().size());
	ASSERT_EQUALS(static_cast<size_t>(0), Shared(s).size());
}

// The callback accessors run caller code while holding m_mu, which is not
// recursive. These pin the two halves of that contract that can be checked
// without deadlocking the test: a nested call on a DIFFERENT instance is a
// different mutex and must stay legal, and the guard must unwind so a second
// sequential call still works. Re-entering the SAME instance aborts by
// design, so it is not exercised here -- see CState::ReentryGuard.
TEST(State, CallbackOnADifferentStateInstanceIsAllowed)
{
	CState a;
	CState b;
	a.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 1;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});

	// b's callback reads a: a different CState, so a different mutex, and
	// nothing to deadlock on. The guard must not confuse the two.
	std::size_t seen = 0;
	b.MutateClients([&a, &seen](std::map<std::uint32_t, ClientSnapshot> &) {
		a.WithFiles([&seen](const FileMap &files) { seen = files.size(); });
	});
	ASSERT_EQUALS(static_cast<size_t>(1), seen);
}

TEST(State, CallbackGuardUnwindsSoLaterCallsStillWork)
{
	CState s;
	s.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 7;
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});
	std::size_t first = 0, second = 0;
	s.WithFiles([&first](const FileMap &files) { first = files.size(); });
	s.WithFiles([&second](const FileMap &files) { second = files.size(); });
	ASSERT_EQUALS(static_cast<size_t>(1), first);
	ASSERT_EQUALS(static_cast<size_t>(1), second);

	// And an exception escaping a callback must not leave the guard set.
	try {
		s.WithFiles([](const FileMap &) { throw std::runtime_error("boom"); });
	} catch (const std::runtime_error &) {
		// expected
	}
	std::size_t after = 0;
	s.WithFiles([&after](const FileMap &files) { after = files.size(); });
	ASSERT_EQUALS(static_cast<size_t>(1), after);
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

// --- MemoizableTarget -----------------------------------------------
//
// The response-ETag memo is keyed on (target, snapshot revision). Eligibility
// is opt-in: this used to be an exclusion list and it was wrong four separate
// times -- a bare collection the prefixes never matched, a live EC roundtrip
// nobody listed, a per-principal document, and a key that froze while bodies
// moved. Inverting it makes an oversight cost a wasted hash instead of a 304
// for changed content.

// The two collections the memo exists for: the multi-MB bodies where skipping
// an MD5 is worth anything.
// --- MemoUsable / ShouldStampEtag -----------------------------------
//
// Both guard something a sequential test cannot observe: MemoUsable's second
// condition guards a write landing while a handler serializes, and
// ShouldStampEtag's `handler_set_etag` guards the dispatcher stamping over a
// validator a handler already owns. Deleting either used to leave the whole
// suite green, which is the same as having no guard at all -- these tests
// exist to go red when that happens.

// The revision moving across the handler is what disqualifies the response:
// the body belongs to rev_before while the key would claim rev_after.
TEST(State, MemoUsableRejectsAMovedRevision)
{
	ASSERT_TRUE(MemoUsable("/api/v0/downloads", 7, 7));
	ASSERT_TRUE(!MemoUsable("/api/v0/downloads", 7, 8));
	// Direction does not matter -- any inequality means the body cannot be
	// attributed to a revision.
	ASSERT_TRUE(!MemoUsable("/api/v0/downloads", 8, 7));
	ASSERT_TRUE(MemoUsable("/api/v0/shared?limit=10", 3, 3));
	ASSERT_TRUE(!MemoUsable("/api/v0/shared?limit=10", 3, 4));
}

// Both conditions are required, so an ineligible target stays ineligible even
// with a perfectly stable revision, and vice versa.
TEST(State, MemoUsableNeedsBothConditions)
{
	ASSERT_TRUE(!MemoUsable("/api/v0/auth/session", 5, 5));
	ASSERT_TRUE(!MemoUsable("/api/v0/status", 5, 5));
	ASSERT_TRUE(!MemoUsable("/api/v0/auth/session", 5, 6));
	// Revision 0 is the pre-first-tick value; eligibility does not depend
	// on the number, only on it holding still. The caller separately
	// refuses to serve a memo entry stamped 0.
	ASSERT_TRUE(MemoUsable("/api/v0/downloads", 0, 0));
}

// A handler that computed its own ETag owns it. Stamping over the top is what
// gave the static path two validators for one resource, since it clears the
// body for HEAD and only the GET reached the hashing branch.
TEST(State, ShouldStampEtagLeavesAHandlerValidatorAlone)
{
	ASSERT_TRUE(ShouldStampEtag(true, false, 200, false));
	ASSERT_TRUE(!ShouldStampEtag(true, true, 200, false));
}

// The other three terms: unsafe methods carry post-mutation state the client
// always wants delivered, non-200s are not worth a validator, and an empty
// body has nothing to hash.
TEST(State, ShouldStampEtagOnlyForSafe200sWithABody)
{
	ASSERT_TRUE(!ShouldStampEtag(false, false, 200, false));
	ASSERT_TRUE(!ShouldStampEtag(true, false, 404, false));
	ASSERT_TRUE(!ShouldStampEtag(true, false, 304, true));
	ASSERT_TRUE(!ShouldStampEtag(true, false, 200, true));
}

TEST(State, MemoizableTargetCoversTheTwoBigCollections)
{
	ASSERT_TRUE(MemoizableTarget("/api/v0/downloads"));
	ASSERT_TRUE(MemoizableTarget("/api/v0/shared"));
	// A query string picks a page, not a different resource.
	ASSERT_TRUE(MemoizableTarget("/api/v0/downloads?limit=10&offset=20"));
	ASSERT_TRUE(MemoizableTarget("/api/v0/shared?sort=name&order=desc"));
}

// Everything else hashes per request. Each of these was a live bug at some
// point in this PR's history, and under an opt-in rule none of them can
// recur: the self-refreshing ones, the live-EC ones, and the per-principal
// one are all simply absent from the eligible set.
TEST(State, MemoizableTargetExcludesEverythingElse)
{
	// own TTL caches / append-only mirror / refresh-on-read
	ASSERT_TRUE(!MemoizableTarget("/api/v0/stats/tree"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/stats/graphs/download_speed?width=3"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/logs/amule"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/logs/serverinfo"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/search/7/results"));
	// live EC roundtrip per read, and the bare collection a trailing-slash
	// prefix could never match
	ASSERT_TRUE(!MemoizableTarget("/api/v0/search"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/share_directories"));
	// per-principal: one key cannot describe two callers' documents
	ASSERT_TRUE(!MemoizableTarget("/api/v0/auth/session"));
	// snapshot-backed, but not worth a memo -- and absent by default
	ASSERT_TRUE(!MemoizableTarget("/api/v0/status"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/clients"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/servers"));
}

// A sub-resource of an eligible collection is NOT itself eligible: it is a
// different body, and /shared/directories in particular is a live EC read
// that an "everything under /shared" rule would have swept back in.
TEST(State, MemoizableTargetDoesNotExtendToSubResources)
{
	ASSERT_TRUE(!MemoizableTarget("/api/v0/downloads/8b54a3c2"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/downloads/8b54a3c2/clients"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/shared/8b54a3c2"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/share_directories"));
	// and no prefix bleed onto a neighbour that merely starts the same
	ASSERT_TRUE(!MemoizableTarget("/api/v0/downloads_archive"));
	ASSERT_TRUE(!MemoizableTarget("/api/v0/sharedfiles"));
}

// --- Snapshot revision ----------------------------------------------
//
// The ETag memo is keyed on this, not on snapshot_at. snapshot_at cannot
// serve: it counts whole seconds, so two refreshes inside one second are
// indistinguishable, and it is stamped only by the background loop, so the
// inline refreshes that mutating handlers run never moved it. A mutation
// therefore changed a body while the key stood still, and the next
// conditional GET was answered 304 for content that had just changed.
TEST(State, SnapshotRevisionAdvancesOnEveryBump)
{
	CState state;
	const std::uint64_t r0 = state.SnapshotRevision();
	state.BumpSnapshotRevision();
	const std::uint64_t r1 = state.SnapshotRevision();
	ASSERT_TRUE(r1 != r0);
	state.BumpSnapshotRevision();
	ASSERT_TRUE(state.SnapshotRevision() != r1);
}

// Two bumps inside the same wall-clock second must still be distinguishable
// -- the precise case a time_t key collapses.
TEST(State, SnapshotRevisionSeparatesTwoBumpsInOneSecond)
{
	CState state;
	std::vector<std::uint64_t> seen;
	for (int i = 0; i < 5; ++i) {
		state.BumpSnapshotRevision();
		seen.push_back(state.SnapshotRevision());
	}
	for (std::size_t i = 1; i < seen.size(); ++i) {
		ASSERT_TRUE(seen[i] != seen[i - 1]);
	}
}

// Every writer of a memoized body advances the key, and it is the WRITER
// that does it rather than its callers. That is the whole point: the key was
// advanced from the outside three times and missed a path each time -- the
// inline refreshes mutating handlers run, and then a tick that failed partway
// after it had already written. A writer cannot forget that it wrote.
TEST(State, MutatingDownloadsAdvancesTheRevision)
{
	CState state;
	const std::uint64_t before = state.SnapshotRevision();
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 1;
		f.hash = "1111111111111111111111111111aaaa";
		f.is_downloading = true;
		files.emplace(f.ecid, f);
	});
	ASSERT_TRUE(state.SnapshotRevision() != before);
}

TEST(State, MutatingSharedAdvancesTheRevision)
{
	CState state;
	const std::uint64_t before = state.SnapshotRevision();
	state.MutateShared([](FileMap &) {});
	ASSERT_TRUE(state.SnapshotRevision() != before);
}

// The failure path specifically. A tick that dies partway still calls
// ResetLists on the way back, and that wipe is as much a body change as any
// mutation -- it is where the key used to freeze while the bodies moved, so
// a whole EC outage was served 304 against the pre-failure validator.
TEST(State, ResetListsAdvancesTheRevision)
{
	CState state;
	state.MutateDownloads([](FileMap &files) {
		FileSnapshot f;
		f.ecid = 2;
		f.hash = "2222222222222222222222222222bbbb";
		files.emplace(f.ecid, f);
	});
	const std::uint64_t before = state.SnapshotRevision();
	state.ResetLists();
	ASSERT_TRUE(state.SnapshotRevision() != before);
}

// MarkTickSuccess stamps the timestamp; it deliberately does NOT advance the
// revision, because RefresherTick owns that and the background loop calls
// both. Pinning it so a future edit does not quietly double-count.
TEST(State, MarkTickSuccessDoesNotAdvanceTheRevision)
{
	CState state;
	const std::uint64_t before = state.SnapshotRevision();
	state.MarkTickSuccess();
	ASSERT_EQUALS(before, state.SnapshotRevision());
}
