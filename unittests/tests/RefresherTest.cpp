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

#include "include/protocol/ed2k/ClientSoftware.h" // SO_* client-software enum

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
// A partfile that is BOTH downloading and shared carries two independent
// priorities: the download priority (EC_TAG_PARTFILE_PRIO, surfaced on
// /downloads) and the upload priority (EC_TAG_KNOWNFILE_PRIO, surfaced on
// /shared). They live in separate sub-blocks; the shared-walker pass must
// not clobber the download value written by the downloads-walker pass.
// (Regression: a single top-level snapshot `priority` field let the two
// overwrite each other — /downloads reported the upload level.)
// ----------------------------------------------------------------------

TEST(Refresher, BothFilePrioritiesAreIndependent)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	// Downloads pass: partfile ECID 77 at download priority PR_HIGH (=2,
	// Constants.h) → "high".
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(77));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, static_cast<std::uint8_t>(2)));
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	ASSERT_TRUE(cache.find(77) != cache.end());
	ASSERT_TRUE(cache.find(77)->second.is_downloading);
	ASSERT_EQUALS(std::string("high"), cache.find(77)->second.download.priority);

	// Shared pass: SAME ECID, shared flag on, upload priority PR_LOW (=0)
	// → "low". Must land in shared.priority and leave download.priority.
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(77));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(1)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_PRIO, static_cast<std::uint8_t>(0)));
		resp.AddTag(pf);
		ApplyGetUpdateToShared(&resp, cache);
	}

	const auto it = cache.find(77);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.is_downloading);
	ASSERT_TRUE(it->second.is_shared);
	ASSERT_EQUALS(std::string("high"), it->second.download.priority); // not clobbered
	ASSERT_EQUALS(std::string("low"), it->second.shared.priority);
}

// ----------------------------------------------------------------------
// A partfile that starts downloading BEFORE it shares. Its upload
// priority (EC_TAG_KNOWNFILE_PRIO) is emitted on early ticks while it is
// still download-only; amuled then CValueMap-suppresses the unchanged
// tag. When the file later flips shared, the shared walker never sees
// the priority tag again. The downloads walker must therefore latch the
// upload priority from the partfile tag, and the share-off reset must
// preserve it, so /shared reports a real level instead of "".
// (Regression: amule-org/amule#384 follow-up — empty shared `priority`.)
// ----------------------------------------------------------------------

TEST(Refresher, SharedPriorityLatchedBeforeSharingSurvivesSuppression)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	// Tick 1 — download-only. The partfile tag carries both priorities
	// (download PR_HIGH=2 → "high", upload PR_LOW=0 → "low") and an
	// explicit not-shared flag. Downloads walker runs first, then shared.
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(78));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, static_cast<std::uint8_t>(2)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_PRIO, static_cast<std::uint8_t>(0)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(0)));
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
		ApplyGetUpdateToShared(&resp, cache);
	}
	{
		const auto it = cache.find(78);
		ASSERT_TRUE(it != cache.end());
		ASSERT_TRUE(!it->second.is_shared);
		// Upload priority was latched by the downloads walker and NOT
		// wiped by the share-off reset.
		ASSERT_EQUALS(std::string("low"), it->second.shared.priority);
	}

	// Tick 2 — file flips shared, but the unchanged upload priority is
	// now suppressed (no EC_TAG_KNOWNFILE_PRIO child). Without the latch
	// the shared walker would leave shared.priority empty.
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(78));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(1)));
		resp.AddTag(pf);
		ApplyGetUpdateToShared(&resp, cache);
	}

	const auto it = cache.find(78);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.is_shared);
	ASSERT_EQUALS(std::string("high"), it->second.download.priority);
	ASSERT_EQUALS(std::string("low"), it->second.shared.priority); // not empty
}

// ----------------------------------------------------------------------
// Single-file detail decode (issue #417). The download-detail and
// shared-detail endpoints read these off the same snapshot the walkers
// build, so pin that the new tags land in the right sub-blocks.
// ----------------------------------------------------------------------

TEST(Refresher, DownloadDetailTagsDecodeIntoSnapshot)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(101));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_LAST_SEEN_COMP, static_cast<std::uint32_t>(1700000000)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_LAST_RECV, static_cast<std::uint32_t>(1700000123)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_DOWNLOAD_ACTIVE, static_cast<std::uint32_t>(3600)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_AVAILABLE_PARTS, static_cast<std::uint16_t>(12)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_HASHED_PART_COUNT, static_cast<std::uint16_t>(3)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_LOST_CORRUPTION, static_cast<std::uint64_t>(9728000)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_GAINED_COMPRESSION, static_cast<std::uint64_t>(4096)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_SAVED_ICH, static_cast<std::uint32_t>(7)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_PARTMETID, static_cast<std::uint32_t>(42)));
	// Base CKnownFile tags carried on the partfile tag too.
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_ON_QUEUE, static_cast<std::uint32_t>(5)));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_AICH_MASTERHASH, std::string("ABCDEF0123")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_FILENAME, std::string("042.part")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_PATH, std::string("/home/me/.aMule/Temp")));
	resp.AddTag(pf);

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	const auto it = cache.find(101);
	ASSERT_TRUE(it != cache.end());
	const auto &d = it->second;
	ASSERT_EQUALS(static_cast<std::uint32_t>(1700000000), d.download.last_seen_complete);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1700000123), d.download.last_changed);
	ASSERT_EQUALS(static_cast<std::uint32_t>(3600), d.download.download_active_time);
	ASSERT_EQUALS(static_cast<std::uint16_t>(12), d.download.available_part_count);
	ASSERT_EQUALS(static_cast<std::uint16_t>(3), d.download.hashing_progress);
	ASSERT_EQUALS(static_cast<std::uint64_t>(9728000), d.download.lost_to_corruption);
	ASSERT_EQUALS(static_cast<std::uint64_t>(4096), d.download.gained_by_compression);
	ASSERT_EQUALS(static_cast<std::uint32_t>(7), d.download.saved_by_ich);
	ASSERT_EQUALS(static_cast<std::uint32_t>(42), d.download.partmet_id);
	ASSERT_EQUALS(static_cast<std::uint32_t>(5), d.queued_count);
	ASSERT_EQUALS(std::string("ABCDEF0123"), d.aich_hash);
	ASSERT_EQUALS(std::string("042.part"), d.part_met_basename);
	ASSERT_EQUALS(std::string("/home/me/.aMule/Temp"), d.on_disk_dir);
}

TEST(Refresher, SharedDetailTagsDecodeIntoSnapshot)
{
	FileMap cache;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag kf(EC_TAG_KNOWNFILE, static_cast<std::uint32_t>(202));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES, static_cast<std::uint16_t>(8)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES_LOW, static_cast<std::uint16_t>(5)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES_HIGH, static_cast<std::uint16_t>(11)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_ON_QUEUE, static_cast<std::uint32_t>(9)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_AICH_MASTERHASH, std::string("FEDCBA9876")));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_FILENAME, std::string("/home/me/Incoming")));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_PATH, std::string("/home/me/Incoming")));
	// Live upload activity (issue #466).
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_UPLOAD_SPEED, static_cast<std::uint32_t>(51200)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_UPLOADING_COUNT, static_cast<std::uint16_t>(3)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_LAST_UPLOAD, static_cast<std::uint32_t>(1700000500)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_SHARED_SINCE, static_cast<std::uint32_t>(1699000000)));
	resp.AddTag(kf);

	ApplyGetUpdateToShared(&resp, cache);

	const auto it = cache.find(202);
	ASSERT_TRUE(it != cache.end());
	const auto &s = it->second;
	ASSERT_EQUALS(static_cast<std::uint16_t>(5), s.shared.complete_sources_low);
	ASSERT_EQUALS(static_cast<std::uint16_t>(11), s.shared.complete_sources_high);
	ASSERT_EQUALS(static_cast<std::uint32_t>(9), s.queued_count);
	ASSERT_EQUALS(std::string("FEDCBA9876"), s.aich_hash);
	// Completed known file → the directory path arrives on its own tag
	// (the write layer maps an incomplete shared partfile to "[PartFile]").
	ASSERT_EQUALS(std::string("/home/me/Incoming"), s.on_disk_dir);
	// Upload activity (issue #466) decodes into the shared sub-block.
	ASSERT_EQUALS(static_cast<std::uint32_t>(51200), s.shared.upload_speed_bps);
	ASSERT_EQUALS(static_cast<std::uint16_t>(3), s.shared.uploading_count);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1700000500), s.shared.last_upload);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1699000000), s.shared.shared_since);
}

// Comment/rating (issue #419): the user's own comment+rating land at the
// top level; the per-source EC_TAG_PARTFILE_COMMENTS container decodes
// into download.source_comments (4 index-grouped children per source,
// rating -1 = unrated).
TEST(Refresher, CommentRatingAndSourceCommentsDecode)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(303));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_COMMENT, std::string("my own note")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_RATING, static_cast<std::uint32_t>(4)));
	CECEmptyTag comments(EC_TAG_PARTFILE_COMMENTS);
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("alice")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("movie.mkv")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, static_cast<std::uint64_t>(5)));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("great quality")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("bob")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("film.avi")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, static_cast<std::uint64_t>(-1))); // unrated
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("no rating here")));
	pf.AddTag(comments);
	resp.AddTag(pf);

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	const auto it = cache.find(303);
	ASSERT_TRUE(it != cache.end());
	ASSERT_EQUALS(std::string("my own note"), it->second.comment);
	ASSERT_EQUALS(4, static_cast<int>(it->second.rating));
	ASSERT_EQUALS(static_cast<size_t>(2), it->second.download.source_comments.size());
	ASSERT_EQUALS(std::string("alice"), it->second.download.source_comments[0].username);
	ASSERT_EQUALS(std::string("movie.mkv"), it->second.download.source_comments[0].filename);
	ASSERT_EQUALS(5, static_cast<int>(it->second.download.source_comments[0].rating));
	ASSERT_EQUALS(-1, static_cast<int>(it->second.download.source_comments[1].rating));
	ASSERT_EQUALS(std::string("no rating here"), it->second.download.source_comments[1].comment);
}

// Source-reported filenames (issue #420) are delta-encoded by amuled and
// accumulated across ticks: tick 1 adds two names; tick 2 removes one
// (COUNTS=0) and updates the other's count (COUNTS-only child).
TEST(Refresher, SourceNamesDeltaAccumulate)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(404));
		CECEmptyTag names(EC_TAG_PARTFILE_SOURCE_NAMES);
		CECTag c1(EC_TAG_PARTFILE_SOURCE_NAMES, static_cast<std::uint32_t>(1));
		c1.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES, std::string("Movie.mkv")));
		c1.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, static_cast<std::uint32_t>(7)));
		names.AddTag(c1);
		CECTag c2(EC_TAG_PARTFILE_SOURCE_NAMES, static_cast<std::uint32_t>(2));
		c2.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES, std::string("movie.avi")));
		c2.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, static_cast<std::uint32_t>(2)));
		names.AddTag(c2);
		pf.AddTag(names);
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	{
		const auto it = cache.find(404);
		ASSERT_TRUE(it != cache.end());
		ASSERT_EQUALS(static_cast<size_t>(2), it->second.download.source_names.size());
		ASSERT_EQUALS(std::string("Movie.mkv"), it->second.download.source_names[1].name);
		ASSERT_EQUALS(static_cast<std::uint32_t>(7), it->second.download.source_names[1].count);
	}

	// Tick 2: remove id 2 (count 0), bump id 1's count to 9 (no name).
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(404));
		CECEmptyTag names(EC_TAG_PARTFILE_SOURCE_NAMES);
		CECTag rem(EC_TAG_PARTFILE_SOURCE_NAMES, static_cast<std::uint32_t>(2));
		rem.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, static_cast<std::uint32_t>(0)));
		names.AddTag(rem);
		CECTag upd(EC_TAG_PARTFILE_SOURCE_NAMES, static_cast<std::uint32_t>(1));
		upd.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, static_cast<std::uint32_t>(9)));
		names.AddTag(upd);
		pf.AddTag(names);
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	const auto it = cache.find(404);
	ASSERT_TRUE(it != cache.end());
	ASSERT_EQUALS(static_cast<size_t>(1), it->second.download.source_names.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(9), it->second.download.source_names[1].count);
	ASSERT_TRUE(it->second.download.source_names.find(2) == it->second.download.source_names.end());
}

// A4AF (issue #421): the auto flag decodes into download.a4af_auto and
// the EC_TAG_PARTFILE_A4AF_SOURCES container's EC_TAG_ECID children into
// the a4af_sources list (full replace when present).
TEST(Refresher, A4afAutoAndSourcesDecode)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(505));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_A4AFAUTO, true));
	CECEmptyTag a4af(EC_TAG_PARTFILE_A4AF_SOURCES);
	a4af.AddTag(CECTag(EC_TAG_ECID, static_cast<std::uint32_t>(1234)));
	a4af.AddTag(CECTag(EC_TAG_ECID, static_cast<std::uint32_t>(5678)));
	pf.AddTag(a4af);
	resp.AddTag(pf);

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	const auto it = cache.find(505);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.download.a4af_auto);
	ASSERT_EQUALS(static_cast<size_t>(2), it->second.download.a4af_sources.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(1234), it->second.download.a4af_sources[0]);
	ASSERT_EQUALS(static_cast<std::uint32_t>(5678), it->second.download.a4af_sources[1]);
}

// Media metadata (issue #418): the six FT_MEDIA_* EC tags decode into the
// `media` sub-struct and set `has_media`; a file with no media tags keeps
// has_media=false (so the API omits the `media` object).
TEST(Refresher, MediaMetadataDecode)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(606));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_LENGTH, static_cast<std::uint32_t>(5400)));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_BITRATE, static_cast<std::uint32_t>(1500)));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_CODEC, std::string("h264")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_ARTIST, std::string("Some Artist")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_ALBUM, std::string("Some Album")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_TITLE, std::string("Some Title")));
	resp.AddTag(pf);
	// A second file with no media tags stays has_media=false.
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(607)));

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	const auto it = cache.find(606);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.has_media);
	ASSERT_EQUALS(static_cast<std::uint32_t>(5400), it->second.media.length_s);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1500), it->second.media.bitrate);
	ASSERT_EQUALS(std::string("h264"), it->second.media.codec);
	ASSERT_EQUALS(std::string("Some Artist"), it->second.media.artist);
	ASSERT_EQUALS(std::string("Some Album"), it->second.media.album);
	ASSERT_EQUALS(std::string("Some Title"), it->second.media.title);

	const auto it2 = cache.find(607);
	ASSERT_TRUE(it2 != cache.end());
	ASSERT_TRUE(!it2->second.has_media);
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
		// #440 server host country ISO code resolved daemon-side.
		srv.AddTag(CECTag(EC_TAG_SERVER_COUNTRY, wxString::FromUTF8("de")));
		container.AddTag(srv);
		resp.AddTag(container);
	}

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(42) != cache.end());
	ASSERT_TRUE(cache.find(9999) == cache.end()); // evicted
	ASSERT_EQUALS(static_cast<std::uint32_t>(1234), cache[42].users);
	ASSERT_EQUALS(std::string("de"), cache[42].country_code);
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

TEST(Refresher, StatusDecodeStoppedNonCompleteReportsStopped)
{
	// A download that's stopped but NOT yet completed (user hit Stop
	// mid-transfer) surfaces as the distinct wire status "stopped":
	// stop = pause + drop all sources + reset the Kad source search,
	// and clients need to tell it apart from a plain "paused" (which
	// keeps its sources). PS_COMPLETE / PS_COMPLETING still take
	// priority over the stopped flag — see the two tests above.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(103));
		// PS_READY = 0 (transferring). User stopped it.
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(0)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, true));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	ASSERT_TRUE(cache.find(103) != cache.end());
	ASSERT_EQUALS(std::string("stopped"), cache.find(103)->second.download.status);
}

TEST(Refresher, StatusDecodePausedNotStoppedReportsPaused)
{
	// The complement: a paused file that is NOT stopped (PS_PAUSED with
	// the stopped flag clear) keeps its sources and reports "paused",
	// distinct from the "stopped" state above. Pins that the "stopped"
	// wire status is gated on EC_TAG_PARTFILE_STOPPED, not on PS_PAUSED.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(104));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(7 /* PS_PAUSED */)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, false));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	ASSERT_TRUE(cache.find(104) != cache.end());
	ASSERT_EQUALS(std::string("paused"), cache.find(104)->second.download.status);
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

// Search result status + type (issue #429): EC_TAG_PARTFILE_STATUS decodes
// to the lowercase status string, and `type` is derived from the filename.
TEST(Refresher, SearchResultStatusAndTypeDecode)
{
	std::map<std::uint32_t, SearchResult> cache;
	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("cool.movie.mkv")));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint32_t>(2))); // QUEUED
	resp.AddTag(sf);
	// A second result with no status tag defaults to "new"; a .mp3 → audio.
	CECTag sf2(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(71));
	sf2.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("song.mp3")));
	sf2.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(4)));
	resp.AddTag(sf2);

	ApplySearchFull(&resp, cache);

	const auto it = cache.find(70);
	ASSERT_TRUE(it != cache.end());
	ASSERT_EQUALS(std::string("queued"), it->second.status);
	// GetFiletypeByName's label lowercased — "videos", same tokens as the
	// shared-detail file_type (issue #417), not a bespoke "video".
	ASSERT_EQUALS(std::string("videos"), it->second.type);

	const auto it2 = cache.find(71);
	ASSERT_TRUE(it2 != cache.end());
	ASSERT_EQUALS(std::string("new"), it2->second.status);
	ASSERT_EQUALS(std::string("audio"), it2->second.type);
}

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

// Search result media metadata (issue #430): the EC_TAG_KNOWNFILE_MEDIA_*
// tags (present only for hits known/probed locally) decode into the
// SearchResult media sub-struct and set has_media; a hit with none stays
// has_media=false so the API omits the `media` object.
TEST(Refresher, SearchResultMediaDecode)
{
	std::map<std::uint32_t, SearchResult> cache;
	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(80));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("show.s01e01.mkv")));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(999)));
	sf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_LENGTH, static_cast<std::uint32_t>(1320)));
	sf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_BITRATE, static_cast<std::uint32_t>(2500)));
	sf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_CODEC, std::string("h264")));
	resp.AddTag(sf);
	// A second hit with no media tags stays has_media=false.
	CECTag sf2(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(81));
	sf2.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("nomedia.bin")));
	sf2.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(4)));
	resp.AddTag(sf2);

	ApplySearchFull(&resp, cache);

	const auto it = cache.find(80);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.has_media);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1320), it->second.media.length_s);
	ASSERT_EQUALS(static_cast<std::uint32_t>(2500), it->second.media.bitrate);
	ASSERT_EQUALS(std::string("h264"), it->second.media.codec);

	const auto it2 = cache.find(81);
	ASSERT_TRUE(it2 != cache.end());
	ASSERT_TRUE(!it2->second.has_media);
}

// --- #431: result grouping folds same-hash/diff-name children --------
//
// A parent plus two children (each carrying EC_TAG_SEARCH_PARENT) must
// collapse to a single top-level result with the two alternative names
// nested in children[]; the child ECIDs must not remain top-level.
TEST(Refresher, SearchResultGroupingFoldsChildren)
{
	std::map<std::uint32_t, SearchResult> cache;
	CECPacket resp(EC_OP_SEARCH_RESULTS);

	CECTag parent(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(100));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("best.mkv")));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(30)));
	resp.AddTag(parent);

	CECTag c1(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(101));
	c1.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("alt.name.mkv")));
	c1.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	c1.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(10)));
	c1.AddTag(CECTag(EC_TAG_SEARCH_PARENT, static_cast<std::uint32_t>(100)));
	resp.AddTag(c1);

	CECTag c2(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(102));
	c2.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("third.mkv")));
	c2.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	c2.AddTag(CECTag(EC_TAG_SEARCH_PARENT, static_cast<std::uint32_t>(100)));
	resp.AddTag(c2);

	ApplySearchFull(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	const auto it = cache.find(100);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(cache.find(101) == cache.end());
	ASSERT_TRUE(cache.find(102) == cache.end());
	ASSERT_EQUALS(static_cast<size_t>(2), it->second.children.size());
	// map iterates ecid-ascending, so 101 folds before 102.
	ASSERT_EQUALS(std::string("alt.name.mkv"), it->second.children[0].name);
	ASSERT_EQUALS(static_cast<std::uint32_t>(101), it->second.children[0].ecid);
	ASSERT_EQUALS(static_cast<std::uint32_t>(10), it->second.children[0].source_count);
	ASSERT_EQUALS(std::string("third.mkv"), it->second.children[1].name);
}

// --- #359: peer software_version must be locale-independent ----------
//
// The daemon formats the version string with gettext, so an unidentified
// client yields _("Unknown") -- "Desconocido" on a Spanish daemon. amuleapi
// is a separate process and can't reverse the translation, so the refresher
// keys off the numeric software code (locale-independent) and emits the
// lowercase "unknown" sentinel instead of the translated string.

// Build a one-client GET_UPDATE response: an EC_TAG_CLIENT container with a
// single child client carrying the software code and (optionally) a version
// string. Mirrors the amuled-side EC_TAG_CLIENT shape.
static void PutOneClient(CECPacket &resp,
	std::uint32_t ecid,
	std::uint32_t soft,
	const char *ver_str /* nullptr = omit the version tag */)
{
	CECTag container(EC_TAG_CLIENT, static_cast<std::uint32_t>(0));
	CECTag cli(EC_TAG_CLIENT, ecid);
	cli.AddTag(CECTag(EC_TAG_CLIENT_SOFTWARE, soft));
	if (ver_str)
		cli.AddTag(CECTag(EC_TAG_CLIENT_SOFT_VER_STR, wxString::FromUTF8(ver_str)));
	container.AddTag(cli);
	resp.AddTag(container);
}

TEST(Refresher, ClientVersionUnknownYieldsLocaleIndependentSentinel)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	std::map<std::uint32_t, std::string> no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	// Unidentified client + a translated version string, as a non-English
	// daemon would ship it. Must NOT leak into the API response.
	PutOneClient(resp, 7, static_cast<std::uint32_t>(SO_UNKNOWN), "Desconocido");
	ApplyGetUpdateToClients(&resp, cache, no_files);
	ASSERT_TRUE(cache.find(7) != cache.end());
	ASSERT_EQUALS(std::string("unknown"), cache[7].software_version);
}

TEST(Refresher, ClientVersionKnownStringPassesThrough)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	std::map<std::uint32_t, std::string> no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	PutOneClient(resp, 8, static_cast<std::uint32_t>(SO_AMULE), "aMule 2.3.3");
	ApplyGetUpdateToClients(&resp, cache, no_files);
	ASSERT_TRUE(cache.find(8) != cache.end());
	ASSERT_EQUALS(std::string("aMule 2.3.3"), cache[8].software_version);
}

TEST(Refresher, ClientKnownButNoVersionStringFallsBackToSentinel)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	std::map<std::uint32_t, std::string> no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	// Known software, but the daemon shipped no version string at all.
	PutOneClient(resp, 9, static_cast<std::uint32_t>(SO_EMULE), nullptr);
	ApplyGetUpdateToClients(&resp, cache, no_files);
	ASSERT_TRUE(cache.find(9) != cache.end());
	ASSERT_EQUALS(std::string("unknown"), cache[9].software_version);
}

// --- #422: detail-only client fields decode --------------------------
//
// The section-B fields ride the INC_UPDATE client tag and are captured
// into ClientSnapshot by MergeClientTag; the detail endpoint serializes
// them. Verify each decodes, HighID/LowID derives from the hybrid id,
// and EC_TAG_CLIENT_FROM maps to the stable origin token.
TEST(Refresher, ClientDetailFieldsDecode)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	std::map<std::uint32_t, std::string> no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag container(EC_TAG_CLIENT, static_cast<std::uint32_t>(0));

	// A HighID peer carrying the full detail-only set.
	CECTag hi(EC_TAG_CLIENT, static_cast<std::uint32_t>(50));
	hi.AddTag(CECTag(EC_TAG_CLIENT_USER_ID, static_cast<std::uint32_t>(0x04030201)));
	// Host-order IPv4; FormatClientIpv4 renders LSB-first => 127.0.0.1.
	hi.AddTag(CECTag(EC_TAG_CLIENT_SERVER_IP, static_cast<std::uint32_t>(0x0100007F)));
	hi.AddTag(CECTag(EC_TAG_CLIENT_SERVER_PORT, static_cast<std::uint16_t>(4242)));
	hi.AddTag(CECTag(EC_TAG_CLIENT_SERVER_NAME, wxString::FromUTF8("test-server")));
	hi.AddTag(CECTag(EC_TAG_CLIENT_KAD_PORT, static_cast<std::uint16_t>(4672)));
	hi.AddTag(CECTag(EC_TAG_CLIENT_FROM, static_cast<std::uint64_t>(3))); // SF_KADEMLIA
	hi.AddTag(CECTag(EC_TAG_PARTFILE_NAME, wxString::FromUTF8("upload.iso")));
	hi.AddTag(CECTag(EC_TAG_CLIENT_AVAILABLE_PARTS, static_cast<std::uint32_t>(7)));
	hi.AddTag(CECTag(EC_TAG_CLIENT_MOD_VERSION, wxString::FromUTF8("mod-x")));
	hi.AddTag(CECTag(EC_TAG_CLIENT_DISABLE_VIEW_SHARED, true));
	// #423 friend status + DL/UP modifier.
	hi.AddTag(CECTag(EC_TAG_CLIENT_IS_FRIEND, true));
	hi.AddTag(CECTag(EC_TAG_CLIENT_SCORE_RATIO, static_cast<double>(2.5)));
	// #439 peer country ISO code resolved daemon-side.
	hi.AddTag(CECTag(EC_TAG_CLIENT_COUNTRY, wxString::FromUTF8("de")));
	container.AddTag(hi);

	// A LowID peer (hybrid id < 0x1000000) with no section-B tags.
	CECTag lo(EC_TAG_CLIENT, static_cast<std::uint32_t>(51));
	lo.AddTag(CECTag(EC_TAG_CLIENT_USER_ID, static_cast<std::uint32_t>(1234)));
	container.AddTag(lo);

	resp.AddTag(container);
	ApplyGetUpdateToClients(&resp, cache, no_files);

	const auto it = cache.find(50);
	ASSERT_TRUE(it != cache.end());
	const ClientSnapshot &cs = it->second;
	ASSERT_EQUALS(static_cast<std::uint32_t>(0x04030201), cs.user_id_hybrid);
	ASSERT_TRUE(cs.high_id);
	ASSERT_EQUALS(std::string("127.0.0.1"), cs.server_ip);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4242), cs.server_port);
	ASSERT_EQUALS(std::string("test-server"), cs.server_name);
	ASSERT_EQUALS(std::string("de"), cs.country_code);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4672), cs.kad_port);
	ASSERT_EQUALS(std::string("kad"), cs.source_origin);
	ASSERT_EQUALS(std::string("upload.iso"), cs.upload_file_name);
	ASSERT_TRUE(cs.has_available_parts);
	ASSERT_EQUALS(static_cast<std::uint32_t>(7), cs.available_parts);
	ASSERT_EQUALS(std::string("mod-x"), cs.mod_version);
	ASSERT_TRUE(cs.view_shared_disabled);
	ASSERT_TRUE(cs.is_friend);
	ASSERT_TRUE(cs.dl_up_modifier > 2.4 && cs.dl_up_modifier < 2.6);

	const auto it2 = cache.find(51);
	ASSERT_TRUE(it2 != cache.end());
	ASSERT_TRUE(!it2->second.high_id);
	ASSERT_TRUE(!it2->second.has_available_parts);
	// #423 fields absent on the wire => defaults preserved.
	ASSERT_TRUE(!it2->second.is_friend);
	ASSERT_TRUE(it2->second.dl_up_modifier == 0.0);
}

// --- #437: extended EC preference categories decode ------------------
//
// Covers both boolean encodings the core serializer uses: value tags
// (share_hidden/exclude_regex/can_see_shares -> GetInt()!=0) and bare
// presence tags (ich_enabled/use_secident -> tag present == true), plus
// ints, strings, and the directories.shared string array.
TEST(Refresher, PreferencesExtendedCategoriesDecode)
{
	CECPacket resp(EC_OP_SET_PREFERENCES);

	CECEmptyTag dir(EC_TAG_PREFS_DIRECTORIES);
	dir.AddTag(CECTag(EC_TAG_DIRECTORIES_INCOMING, wxString::FromUTF8("/inc")));
	dir.AddTag(CECTag(EC_TAG_DIRECTORIES_TEMP, wxString::FromUTF8("/tmp")));
	CECTag shared(EC_TAG_DIRECTORIES_SHARED, static_cast<std::uint32_t>(2));
	shared.AddTag(CECTag(EC_TAG_STRING, wxString::FromUTF8("/a")));
	shared.AddTag(CECTag(EC_TAG_STRING, wxString::FromUTF8("/b")));
	dir.AddTag(shared);
	dir.AddTag(CECTag(EC_TAG_DIRECTORIES_SHARE_HIDDEN, true)); // value-encoded bool
	dir.AddTag(CECTag(EC_TAG_DIRECTORIES_EXCLUDE_REGEX, true));
	resp.AddTag(dir);

	CECEmptyTag files(EC_TAG_PREFS_FILES);
	files.AddTag(CECEmptyTag(EC_TAG_FILES_ICH_ENABLED)); // presence == true
	files.AddTag(CECTag(EC_TAG_FILES_MIN_FREE_SPACE, static_cast<std::uint32_t>(512)));
	resp.AddTag(files);

	CECEmptyTag srv(EC_TAG_PREFS_SERVERS);
	srv.AddTag(CECTag(EC_TAG_SERVERS_DEAD_SERVER_RETRIES, static_cast<std::uint16_t>(5)));
	srv.AddTag(CECTag(EC_TAG_SERVERS_UPDATE_URL, wxString::FromUTF8("http://srv")));
	resp.AddTag(srv);

	CECEmptyTag sec(EC_TAG_PREFS_SECURITY);
	sec.AddTag(CECTag(EC_TAG_SECURITY_CAN_SEE_SHARES, true)); // value-encoded bool
	sec.AddTag(CECTag(EC_TAG_IPFILTER_LEVEL, static_cast<std::uint32_t>(100)));
	sec.AddTag(CECEmptyTag(EC_TAG_SECURITY_USE_SECIDENT)); // presence == true
	resp.AddTag(sec);

	CECEmptyTag cw(EC_TAG_PREFS_CORETWEAKS);
	cw.AddTag(CECTag(EC_TAG_CORETW_MAX_CONN_PER_FIVE, static_cast<std::uint32_t>(200)));
	cw.AddTag(CECTag(EC_TAG_CORETW_KAD_REASK_MS, static_cast<std::uint32_t>(1800000)));
	resp.AddTag(cw);

	CECEmptyTag kad(EC_TAG_PREFS_KADEMLIA);
	kad.AddTag(CECTag(EC_TAG_KADEMLIA_UPDATE_URL, wxString::FromUTF8("http://nodes")));
	resp.AddTag(kad);

	CECEmptyTag ip2c(EC_TAG_PREFS_IP2COUNTRY);
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_SUPPORTED, true));  // value-encoded bool
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_ENABLED, true));    // value-encoded bool
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_SOURCE, (uint8)1)); // MaxMind
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_CUSTOM_URL, wxString::FromUTF8("http://geo")));
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_MAXMIND_LICENSE, wxString::FromUTF8("LICKEY")));
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_DB_LOADED, true)); // value-encoded bool
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_LOADED_SOURCE, wxString::FromUTF8("maxmind")));
	resp.AddTag(ip2c);

	PreferencesSnapshot p;
	std::vector<CategorySnapshot> cats;
	ParsePreferencesFromPacket(&resp, p, cats);

	ASSERT_EQUALS(std::string("/inc"), p.directories.incoming);
	ASSERT_EQUALS(std::string("/tmp"), p.directories.temp);
	ASSERT_EQUALS(static_cast<size_t>(2), p.directories.shared.size());
	ASSERT_EQUALS(std::string("/a"), p.directories.shared[0]);
	ASSERT_TRUE(p.directories.share_hidden);
	ASSERT_TRUE(p.directories.exclude_regex);
	ASSERT_TRUE(!p.directories.auto_rescan); // absent -> false

	ASSERT_TRUE(p.files.ich_enabled);
	ASSERT_TRUE(!p.files.aich_trust); // absent presence tag -> false
	ASSERT_EQUALS(static_cast<std::uint32_t>(512), p.files.min_free_space_mb);

	ASSERT_EQUALS(static_cast<std::uint32_t>(5), p.servers.dead_server_retries);
	ASSERT_EQUALS(std::string("http://srv"), p.servers.update_url);

	ASSERT_TRUE(p.security.can_see_shares);
	ASSERT_EQUALS(static_cast<std::uint32_t>(100), p.security.ipfilter_level);
	ASSERT_TRUE(p.security.use_secident);
	ASSERT_TRUE(!p.security.obfuscation_required); // absent -> false

	ASSERT_EQUALS(static_cast<std::uint32_t>(200), p.core_tweaks.max_conn_per_five);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1800000), p.core_tweaks.kad_reask_ms);
	ASSERT_EQUALS(std::string("http://nodes"), p.kademlia.update_url);

	ASSERT_TRUE(p.ip2country.supported);
	ASSERT_TRUE(p.ip2country.enabled);
	ASSERT_EQUALS(std::string("maxmind"), p.ip2country.source); // uint8 1 -> "maxmind"
	ASSERT_EQUALS(std::string("http://geo"), p.ip2country.custom_url);
	ASSERT_EQUALS(std::string("LICKEY"), p.ip2country.maxmind_license);
	ASSERT_TRUE(!p.ip2country.auto_update); // absent -> false
	ASSERT_TRUE(p.ip2country.db_loaded);
	ASSERT_EQUALS(std::string("maxmind"), p.ip2country.loaded_source);
	ASSERT_TRUE(!p.ip2country.downloading); // absent -> false
}
