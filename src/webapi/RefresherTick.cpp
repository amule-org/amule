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
#include "EventDiff.h"
#include "State.h"

#include <ec/cpp/ECSpecialTags.h>
#include <ec/cpp/ECPacket.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>


namespace webapi {


bool RefresherTick(CamuleapiApp &app, CState &state)
{
	// Per-tick budget: a few EC ops via SendRecvSerialized
	// (m_ec_mtx-serialised). Any failure bails the whole tick so the
	// cache stays internally consistent — we never expose
	// partially-refreshed snapshots. STAT_REQ runs first because
	// it's the cheapest probe: if EC dropped between ticks, STAT_REQ
	// catches it before we burn roundtrips on the larger queries.

	// /status + /kad + /logs/amule share one STAT_REQ packet.
	//
	// Detail level CMD → FULL because amuled only piggybacks
	// `EC_TAG_STATS_LOGGER_MESSAGE` (the incremental-log channel) at
	// FULL or INC_UPDATE (ExternalConn.cpp:722-730). FULL also adds
	// a few stat-overhead extras (STATS_UP_OVERHEAD,
	// STATS_DOWN_OVERHEAD, STATS_BANNED_COUNT, STATS_TOTAL_*_BYTES,
	// STATS_SHARED_FILE_COUNT) that StatusSnapshot doesn't yet
	// surface — harmless overhead.
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

	// /downloads + /shared + /servers in a single GET_UPDATE roundtrip
	// at EC_DETAIL_INC_UPDATE. Replaces an earlier per-substruct
	// fetch (GET_DLOAD_QUEUE + GET_SHARED_FILES + GET_SERVER_LIST,
	// each with its own UPDATE+FULL two-pass split). Response packet
	// shape and the "why INC_UPDATE works in one tick" rationale
	// (identity short-circuit at EC_DETAIL_UPDATE only) are documented
	// next to ApplyGetUpdateToDownloads in Refresher.h.
	//
	// The response also carries EC_TAG_CLIENT (filtered server-side
	// by `TransmitOnlyUploadingClients`) and EC_TAG_FRIEND containers,
	// both of which we ignore — /uploads stays bound to the upload-
	// queue semantic via EC_OP_GET_ULOAD_QUEUE below.
	//
	// Three Mutate calls under three separate lock acquisitions —
	// snapshot_at is set after the whole tick succeeds; per-substruct
	// atomicity was already best-effort.
	{
		std::unique_ptr<CECPacket> req(
			new CECPacket(EC_OP_GET_UPDATE, EC_DETAIL_INC_UPDATE));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp) return false;
		auto &rle = app.PartfileRleStateRequireStateWriteLock();

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

		// Snapshot post-update download identity (hash, name) per ECID
		// so ApplyGetUpdateToShared can recover identity on a partfile-
		// becoming-shared tick where PARTFILE_HASH is suppressed.
		std::map<std::uint32_t, std::pair<std::string, std::string>>
			dl_identity_fallback;
		for (const auto &d : state.Downloads()) {
			dl_identity_fallback.emplace(d.ecid,
				std::make_pair(d.hash, d.name));
		}

		state.MutateShared(
			[&](std::map<std::uint32_t, SharedSnapshot> &cache) {
				ApplyGetUpdateToShared(resp, cache, dl_identity_fallback);
			});

		state.MutateServers(
			[&](std::map<std::uint32_t, ServerSnapshot> &cache) {
				ApplyGetUpdateToServers(resp, cache);
			});

		// /clients — every alive peer in theApp->clientlist (download
		// sources, upload slots, queue waiters, etc.). Replaces the
		// prior `EC_OP_GET_ULOAD_QUEUE` two-pass call. The CLIENT
		// subtree is also gated by the `TransmitOnlyUploadingClients`
		// server-side pref, which we rely on being false (the amuled
		// default) for the full peer surface; if an operator flipped
		// it amuleapi just sees the US_UPLOADING subset (still
		// correct, narrower).
		state.MutateClients(
			[&](std::map<std::uint32_t, ClientSnapshot> &cache) {
				ApplyGetUpdateToClients(resp, cache);
			});
		delete resp;
	}

	// /logs/serverinfo, /stats/tree, /stats/graphs/{graph} are NOT
	// fetched per-tick — they're lazy-fetched on first GET via
	// CTtlCache (1 s TTL coalesces burst reads). HTTP handlers in
	// Api.cpp drive their own EC roundtrips under m_ec_mtx. Per-tick
	// refresh would have been pure waste when nothing is listening.

	// /search/results — polled per-tick only WHILE a search is active.
	// POST /search flips state.SearchProgress().active = true; the
	// state machine below decides when to flip it back. amuled's
	// EC_TAG_SEARCH_STATUS overloads `raw=0` ("no search" / "Kad in
	// progress" / "global queue not yet populated" / "global finished
	// — m_searchInProgress just flipped off") and `raw=100` ("global
	// queue temporarily empty at start" / "global all done"), so we
	// can't trust the raw value in isolation — see SearchList.cpp:
	// GetSearchProgress for the upstream encoding.
	if (state.SearchProgress().active) {
		std::uint32_t raw_now = 0;
		bool got_progress = false;
		{
			std::unique_ptr<CECPacket> req(
				new CECPacket(EC_OP_SEARCH_RESULTS, EC_DETAIL_FULL));
			const CECPacket *resp = app.SendRecvSerialized(req.get());
			if (!resp) return false;
			state.MutateSearch(
				[&](std::map<std::uint32_t, SearchResult> &cache) {
					ApplySearchFull(resp, cache);
				});
			delete resp;
		}
		{
			std::unique_ptr<CECPacket> req(
				new CECPacket(EC_OP_SEARCH_PROGRESS));
			const CECPacket *resp = app.SendRecvSerialized(req.get());
			if (resp) {
				if (const CECTag *t = resp->GetTagByName(EC_TAG_SEARCH_STATUS)) {
					raw_now = static_cast<std::uint32_t>(t->GetInt());
					got_progress = true;
				}
				delete resp;
			}
		}
		// Missing EC_TAG_SEARCH_STATUS treated as raw=0 — the state
		// machine's defensive timeout branch finalizes the lifecycle
		// either way.
		(void) got_progress;
		const SearchProgressSnapshot next = AdvanceSearchProgress(
			state.SearchProgress(),
			raw_now,
			state.Search().size(),
			std::time(nullptr));
		state.WriteSearchProgress(next);
	}

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

	// EmitDiffsAndUpdate is intentionally NOT called here. Mutation
	// handlers invoke RefresherTick() inline on HTTP threads so the
	// response sees post-mutation state, and LastSeenState has no
	// internal lock — concurrent std::map mutation from the wxApp
	// refresher loop and an HTTP thread is UB. Only the wxApp loop
	// in App.cpp calls EmitDiffsForEventBus() (below) after a
	// successful tick; HTTP callers skip it and SSE subscribers see
	// the diff on the next natural 1 s tick.
	return true;
}


void EmitDiffsForEventBus(CamuleapiApp &app, const CState &state)
{
	// Sole writer of `app.LastSeenForEvents()`. ONLY the wxApp
	// refresher loop calls this; HTTP-server inline RefresherTick
	// call sites do NOT.
	EmitDiffsAndUpdate(app.EventBus(),
	                   app.LastSeenForEvents(),
	                   state);
}


}  // namespace webapi
