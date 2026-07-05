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

#include "Refresher.h"
#include "State.h"

#include "RLE.h" // PartFileEncoderData for the rle_state arg

#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECTag.h>
#include <ec/cpp/ECCodes.h>

#include <cstdint>
#include <map>

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(Refresher)

// ----------------------------------------------------------------------
// EC_TAG_FILE_REMOVED — INC-protocol deletion marker. With GET_UPDATE
// + EC_DETAIL_INC_UPDATE the marker arrives in the consolidated response
// packet. Both ApplyGetUpdateToDownloads and ApplyGetUpdateToShared
// react to it (one will be a no-op for any given ECID since the
// server-side encoder map is unified across both surfaces, but the
// dispatch is per-walker).
// ----------------------------------------------------------------------

TEST(Refresher, FileRemovedErasesFromDownloads)
{
	// Pre-seed two downloads in the cache.
	FileMap cache;
	{
		FileSnapshot d;
		d.ecid = 42;
		d.hash = "aaaa0000aaaa0000aaaa0000aaaa0000";
		d.name = "doomed.iso";
		cache.emplace(42, d);
	}
	{
		FileSnapshot d;
		d.ecid = 99;
		d.hash = "bbbb1111bbbb1111bbbb1111bbbb1111";
		d.name = "survivor.iso";
		cache.emplace(99, d);
	}

	// Craft a GET_UPDATE response that contains a single
	// EC_TAG_FILE_REMOVED marker pointing at ECID 42.
	// The response packet's op code is what amuled emits per
	// ExternalConn.cpp:874 (EC_OP_SHARED_FILES); the walker doesn't
	// care, it iterates child tags.
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(42)));

	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	// Doomed download is gone.
	ASSERT_TRUE(cache.find(42) == cache.end());
	// Survivor is untouched — INC protocol uses explicit deletion
	// markers, never "absence implies removed".
	ASSERT_TRUE(cache.find(99) != cache.end());
	ASSERT_EQUALS(std::string("survivor.iso"), cache.find(99)->second.name);
}

TEST(Refresher, FileRemovedErasesFromShared)
{
	// Symmetric to FileRemovedErasesFromDownloads. The server-side
	// encoder map is unified across partfiles + sharedfiles, so a
	// FILE_REMOVED marker could target an ECID in either cache.
	// ApplyGetUpdateToShared evicts unconditionally; the eventual
	// cross-walker call in RefresherTick has both walkers fire on
	// the same response so the right cache loses the entry.
	FileMap cache;
	{
		FileSnapshot s;
		s.ecid = 33;
		s.hash = "1111aaaa1111aaaa1111aaaa1111aaaa";
		s.name = "shared-doomed.iso";
		cache.emplace(33, s);
	}

	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(33)));

	ApplyGetUpdateToShared(&resp, cache);

	ASSERT_TRUE(cache.find(33) == cache.end());
	ASSERT_TRUE(cache.empty());
}

TEST(Refresher, FileRemovedForUnknownEcidIsNoOp)
{
	// Cache contains a single known download.
	FileMap cache;
	{
		FileSnapshot d;
		d.ecid = 7;
		d.hash = "cccc2222cccc2222cccc2222cccc2222";
		d.name = "kept.iso";
		cache.emplace(7, d);
	}

	// Server emits a stale removal marker for ECID 9999 we've never
	// seen (race: server-side gen bumped between the two lookups).
	// Erasing a missing key must be a no-op.
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(9999)));

	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(7) != cache.end());
	ASSERT_EQUALS(std::string("kept.iso"), cache.find(7)->second.name);
}

// ----------------------------------------------------------------------
// Empty response (no churn since the last tick) — INC protocol's
// silent-skip semantics. Downloads + shared caches stay intact.
// ----------------------------------------------------------------------

TEST(Refresher, EmptyResponseLeavesCachesIntact)
{
	FileMap downloads;
	{
		FileSnapshot d;
		d.ecid = 1;
		d.name = "alpha";
		downloads.emplace(1, d);
	}
	FileMap shared;
	{
		FileSnapshot s;
		s.ecid = 2;
		s.name = "beta";
		shared.emplace(2, s);
	}

	CECPacket resp(EC_OP_SHARED_FILES);
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	ApplyGetUpdateToDownloads(&resp, downloads, rle_state);
	ApplyGetUpdateToShared(&resp, shared);

	// INC protocol: empty response means "no changes since last tick".
	// Cache stays intact — no bulk-delete fallback needed.
	ASSERT_EQUALS(static_cast<size_t>(1), downloads.size());
	ASSERT_EQUALS(static_cast<size_t>(1), shared.size());
}

// ----------------------------------------------------------------------
// Mixed top-level dispatch — one GET_UPDATE response carries both
// EC_TAG_PARTFILE and EC_TAG_KNOWNFILE at the same level. The two
// walkers must each consume only their own tag type without
// cross-contaminating the other cache.
// ----------------------------------------------------------------------

TEST(Refresher, MixedTopLevelDispatchedByTagName)
{
	FileMap downloads;
	FileMap shared;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	// One partfile (ECID 10) — should land in downloads only.
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(10)));
	// One sharedfile (ECID 20) — should land in shared only.
	resp.AddTag(CECTag(EC_TAG_KNOWNFILE, static_cast<std::uint32_t>(20)));
	// A FILE_REMOVED marker (ECID 99) — erases from both walkers'
	// caches; since neither was pre-seeded, it's a no-op for both.
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(99)));

	ApplyGetUpdateToDownloads(&resp, downloads, rle_state);
	ApplyGetUpdateToShared(&resp, shared);

	// Downloads walker captured ECID 10 only — NOT ECID 20 (that
	// belongs to shared) and NOT ECID 99 (that's the FILE_REMOVED).
	ASSERT_EQUALS(static_cast<size_t>(1), downloads.size());
	ASSERT_TRUE(downloads.find(10) != downloads.end());
	ASSERT_TRUE(downloads.find(20) == downloads.end());

	// Shared walker captured ECID 20 only.
	ASSERT_EQUALS(static_cast<size_t>(1), shared.size());
	ASSERT_TRUE(shared.find(20) != shared.end());
	ASSERT_TRUE(shared.find(10) == shared.end());
}

// ----------------------------------------------------------------------
// Shared partfile dispatch — amuled's /shared surface is the union
// of completed knownfiles AND partfiles with `IsShared()=true`
// (i.e. ≥1 chunk completed → uploadable). GET_UPDATE ships partfiles
// as EC_TAG_PARTFILE with a child `EC_TAG_PARTFILE_SHARED` bool.
// The shared walker has to consume both top-level tag types and gate
// partfile inclusion on the flag.
// ----------------------------------------------------------------------

TEST(Refresher, SharedPartfileWithFlagTrueLandsInShared)
{
	FileMap cache;
	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(50));
		// IsShared==true: this partfile has ≥1 chunk and is currently
		// uploadable. The shared walker should pick it up.
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(1)));
		resp.AddTag(pf);
	}

	// PARTFILE_HASH is CValueMap-suppressed on the partfile-to-shared
	// transition tick — supply identity via the downloads-cache fallback,
	// which is how the live code recovers it.
	std::map<std::uint32_t, std::pair<std::string, std::string>> fallback;
	fallback[50] = std::make_pair(
		std::string("aaaa3333aaaa3333aaaa3333aaaa3333"), std::string("shared-test.iso"));
	ApplyGetUpdateToShared(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(50) != cache.end());
	ASSERT_EQUALS(static_cast<std::uint32_t>(50), cache.find(50)->second.ecid);
}

TEST(Refresher, UnsharedPartfileSkippedFromShared)
{
	// PARTFILE arrives with EC_TAG_PARTFILE_SHARED=false. The
	// shared walker must NOT insert it — the file is in the download
	// queue but has zero chunks completed, so no peer can request it.
	FileMap cache;
	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(60));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(0)));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToShared(&resp, cache);

	ASSERT_TRUE(cache.empty());
}

TEST(Refresher, SharedPartfileTransitionsOutClearsSharedRole)
{
	// Pre-seed a shared partfile in cache (was sharing on previous
	// ticks). Now the operator paused / stopped it: the next tick
	// emits EC_TAG_PARTFILE_SHARED=false. The walker must clear the
	// is_shared role (and reset the shared sub-block so /shared can't
	// surface stale upload stats). The entry itself stays in the
	// unified map — entity-level eviction is FILE_REMOVED's job.
	FileMap cache;
	{
		FileSnapshot s;
		s.ecid = 70;
		s.hash = "dddd4444dddd4444dddd4444dddd4444";
		s.name = "was-sharing.iso";
		s.is_shared = true;
		s.shared.xfer_session = 99; // stale stat to verify the reset
		cache.emplace(70, s);
	}
	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(70));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(0)));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToShared(&resp, cache);

	ASSERT_TRUE(cache.find(70) != cache.end());
	ASSERT_TRUE(!cache.find(70)->second.is_shared);
	// Stale upload stats from the prior sharing period must be cleared
	// so /shared can never re-surface them.
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), cache.find(70)->second.shared.xfer_session);
}

TEST(Refresher, SuppressedSharedFlagPreservesCachedPartfile)
{
	// CValueMap suppresses the EC_TAG_PARTFILE_SHARED tag when the
	// value matches the previous frame. For a cached partfile that
	// was previously shared, the absence of the flag means "still
	// shared" — the walker must keep it and apply stat deltas.
	FileMap cache;
	{
		FileSnapshot s;
		s.ecid = 80;
		s.hash = "eeee5555eeee5555eeee5555eeee5555";
		s.name = "still-sharing.iso";
		cache.emplace(80, s);
	}
	CECPacket resp(EC_OP_SHARED_FILES);
	// PARTFILE with no EC_TAG_PARTFILE_SHARED child — flag suppressed.
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(80)));

	ApplyGetUpdateToShared(&resp, cache);

	ASSERT_TRUE(cache.find(80) != cache.end());
	ASSERT_EQUALS(std::string("still-sharing.iso"), cache.find(80)->second.name);
}

TEST(Refresher, SuppressedSharedFlagSkipsUnknownPartfile)
{
	// Mirror of the previous test: a PARTFILE with the SHARED flag
	// suppressed AND no prior cache entry means "we have no signal
	// that this is shared." Don't insert blindly — wait for the next
	// tick that flips the state to emit the flag.
	FileMap cache;
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(90)));

	ApplyGetUpdateToShared(&resp, cache);

	ASSERT_TRUE(cache.empty());
}

// ----------------------------------------------------------------------
// New ECID arrives in one tick with identity baked in — the whole
// point of the GET_UPDATE consolidation. INC_UPDATE doesn't hit the
// EC_DETAIL_UPDATE early-return at ECSpecialCoreTags.cpp:244-246, so
// HASH / NAME / SIZE are shipped on first encounter; no second
// roundtrip needed.
// ----------------------------------------------------------------------

TEST(Refresher, NewPartfileInsertedInOneTick)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	// Craft a partfile tag with just the ECID. The walker dispatches
	// on tag name, calls TagHashLower + MergePartFileTag — both of
	// which gracefully tolerate an absent child set. After the
	// walker runs, ECID 55 is in the cache with default-init fields.
	// (In production a real CEC_PartFile_Tag at INC_UPDATE always
	// carries the full identity child set; this test pins the bare-
	// minimum insertion path.)
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(55)));

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	// The new ECID landed — no needed.
	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(55) != cache.end());
	ASSERT_EQUALS(static_cast<std::uint32_t>(55), cache.find(55)->second.ecid);
}

// ----------------------------------------------------------------------
// /servers — GET_UPDATE wraps per-server tags in an EC_TAG_SERVER
// container at top level. Walker iterates INTO the container and
// merges per-ECID; cache entries not seen in the response get evicted
// because the server side has no FILE_REMOVED equivalent for servers
// (the container always carries the full current list).
// ----------------------------------------------------------------------

TEST(Refresher, ServersFromContainerMergesByEcid)
{
	std::map<std::uint32_t, ServerSnapshot> cache;
	// Pre-seed an entry that should disappear: a server the operator
	// removed from amuled between ticks (it won't show up in the new
	// response's SERVER container).
	{
		ServerSnapshot s;
		s.ecid = 9999;
		s.name = "removed";
		cache.emplace(9999, s);
	}

	// Build a SERVER container with one per-server child (ECID 42).
	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag container(EC_TAG_SERVER, static_cast<std::uint32_t>(0));
		// One per-server child tag inside the container — same
		// EC_TAG_SERVER name (the walker disambiguates by depth, not
		// by name). Minimum tags needed for the merge to populate
		// the snapshot.
		CECTag srv(EC_TAG_SERVER, static_cast<std::uint32_t>(42));
		srv.AddTag(CECTag(EC_TAG_SERVER_USERS, static_cast<std::uint32_t>(1234)));
		container.AddTag(srv);
		resp.AddTag(container);
	}

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(42) != cache.end());
	ASSERT_TRUE(cache.find(9999) == cache.end()); // evicted
	ASSERT_EQUALS(static_cast<std::uint32_t>(1234), cache[42].users);
}

TEST(Refresher, ServersEmptyContainerEmptiesCache)
{
	std::map<std::uint32_t, ServerSnapshot> cache;
	cache.emplace(1, ServerSnapshot{});
	cache.emplace(2, ServerSnapshot{});

	// Empty SERVER container (operator removed every server). Every
	// pre-seeded entry is "not seen this tick" → evicted.
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_SERVER, static_cast<std::uint32_t>(0)));

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_TRUE(cache.empty());
}

TEST(Refresher, ServersNoContainerLeavesCacheAlone)
{
	// Defensive: if a response is missing the SERVER container
	// entirely (which production amuled never does — it always
	// emits the container even when empty), the walker leaves the
	// cache untouched. Better than wiping on an unexpected wire
	// shape.
	std::map<std::uint32_t, ServerSnapshot> cache;
	cache.emplace(7, ServerSnapshot{});

	CECPacket resp(EC_OP_SHARED_FILES);
	// No EC_TAG_SERVER container in the response.

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(7) != cache.end());
}

// ----------------------------------------------------------------------
// RLE state map — cleaned up alongside the cache when a partfile
// gets evicted via FILE_REMOVED. Without the cleanup, the decoder's
// internal buffer (~200 KB per partfile on TB-class files) would
// slowly leak.
// ----------------------------------------------------------------------

TEST(Refresher, RleStateErasedAlongsideFileRemoved)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	{
		FileSnapshot d;
		d.ecid = 77;
		d.hash = "aaaa0000aaaa0000aaaa0000aaaa0000";
		d.name = "doomed.iso";
		cache.emplace(77, d);
		// Simulate a previous tick having allocated a decoder for ECID 77.
		rle_state.emplace(77, PartFileEncoderData{});
	}

	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(77)));

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(77) == cache.end());
	ASSERT_TRUE(rle_state.find(77) == rle_state.end());
}

TEST(Refresher, RleStatePreservedForKnownEntryAcrossTick)
{
	// A partfile already in cache should KEEP its RLE state across a
	// tick that brings no new info. The decoder relies on its buffer
	// surviving from the prior tick.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	{
		FileSnapshot d;
		d.ecid = 5;
		d.hash = "bbbb1111bbbb1111bbbb1111bbbb1111";
		d.name = "stable.iso";
		cache.emplace(5, d);
		rle_state.emplace(5, PartFileEncoderData{});
	}

	// A no-op response (no PARTFILE tags, no FILE_REMOVED). Nothing
	// should churn.
	CECPacket resp(EC_OP_SHARED_FILES);
	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(5) != cache.end());
	ASSERT_TRUE(rle_state.find(5) != rle_state.end());
}

// ----------------------------------------------------------------------
// /stats/tree — recursive walk strips the root container and surfaces
// its children at the top level. Crafted as a hand-built CECTag tree.
// ----------------------------------------------------------------------

TEST(Refresher, StatusDecodeCompleteOverridesStopped)
{
	// A completed download in amuled sits in `m_completedDownloads`
	// with EC_TAG_PARTFILE_STOPPED set true. The decoder used to
	// short-circuit on `stopped` and report "paused" — masking the
	// PS_COMPLETE state from /downloads consumers (and breaking the
	// status=="completed" filter). PS_COMPLETE (and
	// PS_COMPLETING) must take priority over the stopped flag.
	//
	// PS_COMPLETE = 9 (Constants.h). Crafting a partfile tag with
	// PS_STATUS=9 + STOPPED=true exercises the merge path through
	// ApplyGetUpdateToDownloads.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(101));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(9 /* PS_COMPLETE */)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, true));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(101) != cache.end());
	ASSERT_EQUALS(std::string("completed"), cache.find(101)->second.download.status);
}

TEST(Refresher, StatusDecodeCompletingOverridesStopped)
{
	// Same shape, PS_COMPLETING (=8) takes priority over stopped too
	// — covers the in-flight finalization race where the cache is
	// being moved from m_filelist to m_completedDownloads.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(102));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(8 /* PS_COMPLETING */)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, true));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	ASSERT_TRUE(cache.find(102) != cache.end());
	ASSERT_EQUALS(std::string("completing"), cache.find(102)->second.download.status);
}

TEST(Refresher, StatusDecodeStoppedNonCompleteStaysPaused)
{
	// Sanity check the other direction: a download that's stopped
	// but NOT yet completed (user paused mid-transfer) must still
	// report "paused" — the fix only carves out
	// PS_COMPLETE/PS_COMPLETING.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(103));
		// PS_READY = 0 (transferring). User paused it.
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(0)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, true));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	ASSERT_TRUE(cache.find(103) != cache.end());
	ASSERT_EQUALS(std::string("paused"), cache.find(103)->second.download.status);
}

TEST(Refresher, ParseStatsTreeStripsRootAndRecursesChildren)
{
	// Build:
	//  root
	//  ├── Transfer
	//  │   └── Total bytes ...
	//  └── Connection
	CECPacket resp(EC_OP_STATSTREE);
	CECTag root(EC_TAG_STATTREE_NODE, wxString("root-container-label-discarded"));
	{
		CECTag transfer(EC_TAG_STATTREE_NODE, wxString("Transfer"));
		// Nodes may carry a stable machine key; Connection below omits it
		// to exercise the "no key" path.
		transfer.AddTag(CECTag(EC_TAG_STAT_NODE_KEY, wxString("transfer")));
		{
			CECTag total(EC_TAG_STATTREE_NODE, wxString("Total bytes transferred: 12.3 GiB"));
			// Raw numeric ratios ride along as distinctly-named double tags.
			total.AddTag(CECTag(EC_TAG_STAT_NODE_RATIO, static_cast<double>(2.5)));
			total.AddTag(CECTag(EC_TAG_STAT_NODE_RATIO_TOTAL, static_cast<double>(3.5)));
			transfer.AddTag(total);
		}
		root.AddTag(transfer);
	}
	{
		CECTag conn(EC_TAG_STATTREE_NODE, wxString("Connection"));
		root.AddTag(conn);
	}
	resp.AddTag(root);

	StatsTreeNode out;
	ParseStatsTreeFromPacket(&resp, out);

	// The root container itself is discarded; we expose its 2 children
	// (Transfer + Connection) as top-level nodes.
	ASSERT_TRUE(out.label.empty());
	ASSERT_EQUALS(static_cast<size_t>(2), out.children.size());
	// Transfer subtree.
	ASSERT_EQUALS(std::string("Transfer"), out.children[0].label);
	// Stable machine key is parsed when present...
	ASSERT_EQUALS(std::string("transfer"), out.children[0].key);
	ASSERT_EQUALS(static_cast<size_t>(1), out.children[0].children.size());
	ASSERT_EQUALS(std::string("Total bytes transferred: 12.3 GiB"), out.children[0].children[0].label);
	// ...and empty when the node omits the tag (Transfer's child + Connection).
	ASSERT_TRUE(out.children[0].children[0].key.empty());
	// Raw numeric ratios are parsed from the distinctly-named double tags.
	ASSERT_TRUE(out.children[0].children[0].has_ratio_session);
	ASSERT_TRUE(out.children[0].children[0].ratio_session > 2.49 &&
		    out.children[0].children[0].ratio_session < 2.51);
	ASSERT_TRUE(out.children[0].children[0].has_ratio_total);
	ASSERT_TRUE(out.children[0].children[0].ratio_total > 3.49 &&
		    out.children[0].children[0].ratio_total < 3.51);
	// Nodes without the ratio tags report neither.
	ASSERT_TRUE(!out.children[0].has_ratio_session);
	ASSERT_TRUE(!out.children[0].has_ratio_total);
	// Connection is a leaf at this depth.
	ASSERT_EQUALS(std::string("Connection"), out.children[1].label);
	ASSERT_TRUE(out.children[1].key.empty());
	ASSERT_EQUALS(static_cast<size_t>(0), out.children[1].children.size());
}

// ----------------------------------------------------------------------
// AdvanceSearchProgress — maps EC_TAG_SEARCH_LIFECYCLE_STATE +
// EC_TAG_SEARCH_LIFECYCLE_PERCENT into (percent, complete, active).
// Trusts the daemon's flags; the percent is the daemon's unified 0..100
// for every kind (global = real, Kad = cosmetic ramp), so amuleapi no
// longer masks it per-kind — it just passes it through and clamps.
// ----------------------------------------------------------------------

namespace
{

webapi::SearchProgressSnapshot MakeActive(const std::string &kind)
{
	webapi::SearchProgressSnapshot s;
	s.active = true;
	s.kind = kind;
	return s;
}

constexpr std::uint32_t LIFECYCLE_IDLE = 0;
constexpr std::uint32_t LIFECYCLE_RUNNING = 1;
constexpr std::uint32_t LIFECYCLE_FINISHED = 2;

} // namespace

TEST(Refresher, SearchProgressRunningCarriesPercentForGlobal)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("global");
	s = AdvanceSearchProgress(s, LIFECYCLE_RUNNING, /*pct=*/42);
	ASSERT_TRUE(s.active);
	ASSERT_TRUE(!s.complete);
	ASSERT_EQUALS(static_cast<uint32_t>(42), s.percent);
}

TEST(Refresher, SearchProgressRunningPassesThroughKadRamp)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("kad");
	// The daemon synthesises a cosmetic time-ramp for Kad and ships it in
	// EC_TAG_SEARCH_LIFECYCLE_PERCENT, so amuleapi no longer masks Kad to
	// 0 — it passes the daemon value straight through.
	s = AdvanceSearchProgress(s, LIFECYCLE_RUNNING, /*pct=*/37);
	ASSERT_TRUE(s.active);
	ASSERT_EQUALS(static_cast<uint32_t>(37), s.percent);
}

TEST(Refresher, SearchProgressRunningClampsPercentAbove100)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("global");
	// The daemon's percent tag is 0..100, but stay defensive: any value
	// above 100 is clamped rather than surfaced raw to consumers.
	s = AdvanceSearchProgress(s, LIFECYCLE_RUNNING, /*pct=*/250);
	ASSERT_TRUE(s.active);
	ASSERT_EQUALS(static_cast<uint32_t>(100), s.percent);
}

TEST(Refresher, SearchProgressFinishedSetsComplete)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("global");
	s = AdvanceSearchProgress(s, LIFECYCLE_FINISHED, /*pct=*/0);
	ASSERT_TRUE(!s.active);
	ASSERT_TRUE(s.complete);
	ASSERT_EQUALS(static_cast<uint32_t>(100), s.percent);
}

TEST(Refresher, SearchProgressIdleZeroesOutGracefully)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("kad");
	// Refresher shouldn't call us with state=IDLE (it gates on active
	// being true on entry), but stay defensive: flip both flags off.
	s = AdvanceSearchProgress(s, LIFECYCLE_IDLE, /*pct=*/0);
	ASSERT_TRUE(!s.active);
	ASSERT_TRUE(!s.complete);
	ASSERT_EQUALS(static_cast<uint32_t>(0), s.percent);
}
