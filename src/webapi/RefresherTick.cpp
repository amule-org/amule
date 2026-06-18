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

// Refresher orchestration — the per-tick loop body that issues EC
// requests via `CamuleapiApp::SendRecvSerialized`. Split from
// Refresher.cpp so the pure parser/applier code (`ApplyDownloads*`,
// `ApplyUploads*`, `ApplyShared*`, `ParseStatusFromPacket`) stays
// linkable from the unit tests without dragging the wxApp /
// ExternalConnector dependency tree in via App.h.

#include "Refresher.h"

#include "App.h"
#include "EventDiff.h"   // Phase 8b: SSE event emission from cache diffs
#include "State.h"

#include <ec/cpp/ECSpecialTags.h>
#include <ec/cpp/ECPacket.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>


namespace webapi {


namespace {

// Two-phase polling helper. Issues `op` at EC_DETAIL_UPDATE, applies
// the deltas to known entries, then if any new ECIDs surfaced fires
// a second EC_DETAIL_FULL roundtrip restricted to those ECIDs and
// inserts them.
//
// Bandwidth on a steady-state library: O(changed_files) — most ticks
// the Phase 2 packet is empty (no new files) and the second roundtrip
// is skipped entirely. On a cold-start tick: Phase 1 surfaces all
// files as "new" (cache is empty), Phase 2 ships all identities.
// Steady state from tick 2 onward.
template <class Snapshot,
          class UpdateFn,   // (resp, partial_update, cache) → set<ecid>
          class FullFn>     // (resp, cache) → void
bool TwoPhaseRefresh(CamuleapiApp &app,
                     CState &state,
                     ec_opcode_t op,
                     ec_tagname_t item_tag,
                     UpdateFn  apply_update,
                     FullFn    apply_full,
                     void (CState::*mutator)(
                         const std::function<void(std::map<std::uint32_t, Snapshot> &)>&))
{
	const bool partial_update_active = app.IsServerPartialUpdateActive();
	std::set<std::uint32_t> needs_full;

	// Phase 1 — UPDATE.
	{
		std::unique_ptr<CECPacket> req(new CECPacket(op, EC_DETAIL_UPDATE));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp) return false;
		(state.*mutator)([&](std::map<std::uint32_t, Snapshot> &cache) {
			needs_full = apply_update(resp, partial_update_active, cache);
		});
		delete resp;
	}

	// Phase 2 — FULL (only if Phase 1 surfaced new ECIDs).
	if (!needs_full.empty()) {
		// Default constructor uses EC_DETAIL_FULL, which is what the
		// server's CTagSet<uint32, item_tag> filter expects for the
		// "fetch full details for these ECIDs" path.
		std::unique_ptr<CECPacket> req(new CECPacket(op));
		for (std::uint32_t ecid : needs_full) {
			req->AddTag(CECTag(item_tag, ecid));
		}
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp) return false;
		(state.*mutator)([&](std::map<std::uint32_t, Snapshot> &cache) {
			apply_full(resp, cache);
		});
		delete resp;
	}
	return true;
}

}  // namespace


bool RefresherTick(CamuleapiApp &app, CState &state)
{
	// Four EC ops per tick (one or two roundtrips per op under the
	// two-phase polling protocol). Each takes m_ec_mtx via
	// SendRecvSerialized; failures on any one bail the whole tick so
	// the cache stays internally consistent (we never expose
	// partially-refreshed snapshots — old data is preferable to a
	// download list that disagrees with the status `total_src_count`).
	//
	// Order: STAT_REQ first because it's the cheapest probe; if EC
	// went away between ticks, STAT_REQ catches it before we burn
	// roundtrips on the larger queries.
	//
	// Detail level:
	//   * STAT_REQ uses EC_DETAIL_CMD — UPDATE on STAT_REQ returns
	//     nothing useful (ExternalConn.cpp:774 just `break`s).
	//   * Queue/shared use the two-phase EC_DETAIL_UPDATE polling
	//     protocol: Phase 1 fetches stat deltas + EC_TAG_FILE_REMOVED
	//     markers, Phase 2 (only when new ECIDs are seen) fetches
	//     full identity for those. Bandwidth is O(changed_files)
	//     per tick after cold-start. Documented inline at the
	//     TwoPhaseRefresh helper.

	// /status + /kad + /logs/amule (new amule log lines) all share one
	// STAT_REQ packet.
	//
	// Detail level promoted CMD → FULL because amuled only piggybacks
	// `EC_TAG_STATS_LOGGER_MESSAGE` (the incremental-log channel) at
	// FULL or INC_UPDATE — see ExternalConn.cpp:722-730. Promoting
	// from CMD to FULL also tacks on a few stat-overhead extras
	// (STATS_UP_OVERHEAD, STATS_DOWN_OVERHEAD, STATS_BANNED_COUNT,
	// STATS_TOTAL_SENT_BYTES, STATS_TOTAL_RECEIVED_BYTES,
	// STATS_SHARED_FILE_COUNT). amuleapi's StatusSnapshot doesn't
	// surface those yet; harmless overhead until a future
	// Phase carves them out into /status.
	{
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_STAT_REQ, EC_DETAIL_FULL));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp) return false;
		StatusSnapshot s;
		ParseStatusFromPacket(resp, s);
		state.WriteStatus(std::move(s));
		KadSnapshot k;
		ParseKadFromPacket(resp, k);
		state.WriteKad(std::move(k));
		std::vector<std::string> new_log_lines;
		ParseAmuleLogFromPacket(resp, new_log_lines);
		if (!new_log_lines.empty()) {
			state.AppendAmuleLog(std::move(new_log_lines));
		}
		delete resp;
	}

	// /downloads + /shared + /servers — single GET_UPDATE roundtrip at
	// EC_DETAIL_INC_UPDATE. Replaces the previous Phase 4b/4c per-
	// substruct fetches (GET_DLOAD_QUEUE + GET_SHARED_FILES +
	// GET_SERVER_LIST, each with a two-phase Phase 1/2 split). The
	// consolidated op is what amulegui uses; the response packet
	// interleaves EC_TAG_PARTFILE / EC_TAG_KNOWNFILE / EC_TAG_FILE_REMOVED
	// at the top level + an EC_TAG_SERVER container at the same level.
	//
	// Why INC_UPDATE works in one tick where UPDATE needed two:
	// `CEC_SharedFile_Tag` / `CEC_PartFile_Tag` constructors short-
	// circuit identity (NAME / HASH / SIZE / ED2K_LINK) only when
	// `detail_level == EC_DETAIL_UPDATE` (ECSpecialCoreTags.cpp:244-
	// 246). INC_UPDATE doesn't trip that early-return, so first-
	// encounter tags carry full identity — no Phase 2 EC roundtrip
	// needed, no #808 ghost defence, no #713 alive-marker defence.
	// (Both are wire-level races that only exist at EC_DETAIL_UPDATE.)
	//
	// The response also carries an EC_TAG_CLIENT container (peers) and
	// EC_TAG_FRIEND container which amuleapi ignores — /uploads stays
	// bound to the upload-queue semantic via EC_OP_GET_ULOAD_QUEUE
	// below (the GET_UPDATE clients block is filtered server-side by
	// the global `TransmitOnlyUploadingClients` pref, which would
	// pollute amuleweb/amulegui's view if we flipped it).
	//
	// Three Mutate calls under three separate lock acquisitions — same
	// cross-substruct consistency model the previous refresher used
	// (snapshot_at marks tick completion, not per-substruct atomicity).
	{
		std::unique_ptr<CECPacket> req(
			new CECPacket(EC_OP_GET_UPDATE, EC_DETAIL_INC_UPDATE));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp) return false;
		auto &rle = app.PartfileRleState();

		// Snapshot the cache's pre-tick ECID set so we can evict
		// rle_state entries for any partfile that gets removed during
		// the walk (the walker erases from rle_state on FILE_REMOVED,
		// but we also want to cover the case where ApplyGetUpdate*
		// itself evicts in some future hardening path).
		std::set<std::uint32_t> ecids_before;
		state.MutateDownloads(
			[&](std::map<std::uint32_t, DownloadSnapshot> &cache) {
				for (const auto &kv : cache) ecids_before.insert(kv.first);
				ApplyGetUpdateToDownloads(resp, cache, rle);
				// Evict RLE state for ECIDs no longer in cache after
				// the apply. The walker handles FILE_REMOVED already;
				// this is defence in depth.
				for (auto ecid : ecids_before) {
					if (cache.find(ecid) == cache.end()) rle.erase(ecid);
				}
			});

		state.MutateShared(
			[&](std::map<std::uint32_t, SharedSnapshot> &cache) {
				ApplyGetUpdateToShared(resp, cache);
			});

		state.MutateServers(
			[&](std::map<std::uint32_t, ServerSnapshot> &cache) {
				ApplyGetUpdateToServers(resp, cache);
			});

		// /clients — every alive peer in theApp->clientlist (download
		// sources, upload slots, queue waiters, etc.). Replaces the
		// prior `EC_OP_GET_ULOAD_QUEUE` two-phase call (Phase 4g
		// consolidation). The CLIENT subtree is also gated by the
		// `TransmitOnlyUploadingClients` server-side pref, which we
		// rely on being false (the amuled default) for the full peer
		// surface; if an operator flipped it amuleapi just sees the
		// US_UPLOADING subset (still correct, narrower).
		state.MutateClients(
			[&](std::map<std::uint32_t, ClientSnapshot> &cache) {
				ApplyGetUpdateToClients(resp, cache);
			});
		delete resp;
	}

	// /logs/serverinfo, /stats/tree, /stats/graphs/{graph},
	// /search/results — RETIRED FROM THE PER-TICK LOOP in Phase 4g.
	// These are now lazy-fetched on first GET, coalesced via a 1 s
	// TTL cache (CTtlCache in TtlCache.h). The HTTP handlers in
	// Api.cpp drive their own EC roundtrips under m_ec_mtx.
	//
	// Why lazy: server-info / stats-tree / stats-graphs / search-
	// results are read-on-demand UIs (diagnostic views, dashboard
	// tiles, search panel). Per-tick refresh was pure waste when
	// nothing is listening — /search/results in particular sat empty
	// every tick until POST /search drove a query.
	//
	// 1 s TTL is enough to coalesce concurrent dashboard reads (a
	// page render hitting /stats/graphs/{download,upload,kad,
	// connections} four times in 100ms triggers one EC roundtrip,
	// not four) without making the data stale.
	//
	// SearchResult / ServerInfoLog / StatsTreeNode / StatsGraphs
	// types and their related parse helpers (ParseStatsTreeFromPacket,
	// ParseGraphsFromPacket, ApplySearchFull) stay on CState +
	// Refresher.cpp for the lazy-fetch handlers to call directly.

	// /preferences + /categories — one EC roundtrip populates both.
	// Selection bitmask: CATEGORIES (0x01) + GENERAL (0x02) +
	// CONNECTIONS (0x04). Using the named enums (rather than hex
	// literals) so a future bit shuffle in ECCodes.h doesn't
	// silently zero out a section — bit-positional bugs here are
	// hard to spot in JSON (empty defaults look like "0 KB/s" not
	// "field not requested").
	{
		const std::uint32_t selection =
			EC_PREFS_CATEGORIES | EC_PREFS_GENERAL | EC_PREFS_CONNECTIONS;
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_GET_PREFERENCES));
		req->AddTag(CECTag(EC_TAG_SELECT_PREFS, selection));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp) return false;
		PreferencesSnapshot p;
		std::vector<CategorySnapshot> cats;
		ParsePreferencesFromPacket(resp, p, cats);
		state.WritePreferences(std::move(p));
		state.WriteCategories(std::move(cats));
		delete resp;
	}

	// Phase 8b: walk the prior-vs-current state diff and publish
	// typed SSE events for each change (download_added/_updated/
	// _removed, shared_*, server_*, client_*, status_changed). Runs
	// here — at the END of a successful tick — so events fire
	// against a fully-coherent snapshot. The bus is internally
	// thread-safe; SSE subscribers drain from their own threads.
	webapi::EmitDiffsAndUpdate(app.EventBus(),
	                           app.LastSeenForEvents(),
	                           state);

	return true;
}


}  // namespace webapi
