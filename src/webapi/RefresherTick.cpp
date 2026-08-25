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

namespace webapi
{

SearchFetchOutcome FetchSearchResults(CamuleapiApp &app, CState &state, std::uint32_t search_id)
{
	std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_SEARCH_RESULTS, EC_DETAIL_FULL));
	// Opt into result grouping (issue #431): the empty EC_TAG_SEARCH_PARENT
	// flag tells the FULL responder to also emit each same-hash/different-name
	// child so the results list can nest them.
	req->AddTag(CECEmptyTag(EC_TAG_SEARCH_PARENT));
	req->AddTag(CECTag(EC_TAG_SEARCH_ID, search_id));
	const CECPacket *resp = app.SendRecvSerialized(req.get());
	if (!resp)
		return SearchFetchOutcome::EcFailed;
	SearchFetchOutcome outcome = SearchFetchOutcome::Updated;
	if (resp->GetTagByName(EC_TAG_SEARCH_EXPIRED)) {
		outcome = SearchFetchOutcome::Expired;
	} else {
		state.MutateSearch(search_id,
			[&](std::map<std::uint32_t, SearchResult> &cache) { ApplySearchFull(resp, cache); });
	}
	delete resp;
	return outcome;
}

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
	// FULL or INC_UPDATE (ExternalConn.cpp:722-730). FULL is also what
	// carries STATS_UP_OVERHEAD / STATS_DOWN_OVERHEAD and the two
	// free-space tags, which /status reports — so this is load-bearing
	// now, not a harmless over-request: dropping it to CMD would silently
	// empty those fields along with the log channel. STATS_BANNED_COUNT,
	// STATS_TOTAL_*_BYTES and STATS_SHARED_FILE_COUNT still arrive
	// unconsumed.
	{
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_STAT_REQ, EC_DETAIL_FULL));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp)
			return false;
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
	// The response also carries EC_TAG_CLIENT (filtered server-side by
	// `TransmitOnlyUploadingClients`) and EC_TAG_FRIEND containers. Both are
	// consumed below, into /clients and /friends respectively — /uploads
	// stays bound to the upload-queue semantic via EC_OP_GET_ULOAD_QUEUE.
	//
	// Six exclusive acquisitions in this block: five Mutate calls (downloads,
	// shared, servers, friends, clients+files) plus ReconcileKnownClients at
	// the end — snapshot_at is set after the whole tick succeeds; per-substruct
	// atomicity was already best-effort.
	{
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_GET_UPDATE, EC_DETAIL_INC_UPDATE));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp)
			return false;
		auto &rle = app.PartfileRleStateRequireStateWriteLock();

		// Snapshot the cache's pre-tick ECID set so we can evict
		// rle_state entries for any partfile that gets removed during
		// the walk (the walker erases from rle_state on FILE_REMOVED,
		// but we also want to cover the case where ApplyGetUpdate*
		// itself evicts in some future hardening path).
		std::set<std::uint32_t> ecids_before;
		state.MutateDownloads([&](FileMap &cache) {
			for (const auto &kv : cache) {
				if (kv.second.is_downloading)
					ecids_before.insert(kv.first);
			}
			ApplyGetUpdateToDownloads(resp, cache, rle);
			// Evict RLE state for ECIDs that no longer carry the
			// downloading role after the apply. The walker handles
			// FILE_REMOVED already; this is defence in depth.
			for (auto ecid : ecids_before) {
				auto it = cache.find(ecid);
				if (it == cache.end() || !it->second.is_downloading) {
					rle.erase(ecid);
				}
			}
		});

		// Shared walker reads + writes the same unified m_files map.
		// No more dl_identity_fallback compose: when the shared walker
		// sees a partfile whose hash was CValueMap-suppressed, the
		// entry in `cache` already carries hash + name from the
		// downloads walker above. See FileSnapshot in State.h for the
		// shared-storage rationale.
		// Same `rle` map: the shared walker decodes the availability blob on
		// EC_TAG_KNOWNFILE tags, the downloads walker the EC_TAG_PARTFILE
		// ones, and amuled emits exactly one of the two per ECID. The
		// eviction sweep above only ever touches ECIDs that were downloading,
		// so it cannot drop a knownfile's decoder state.
		state.MutateShared([&](FileMap &cache) { ApplyGetUpdateToShared(resp, cache, rle); });

		state.MutateServers([&](std::map<std::uint32_t, ServerSnapshot> &cache) {
			ApplyGetUpdateToServers(resp, cache);
		});

		state.MutateFriends([&](std::map<std::uint32_t, FriendSnapshot> &cache) {
			ApplyGetUpdateToFriends(resp, cache);
		});

		// /clients — every alive peer in theApp->clientlist (download
		// sources, upload slots, queue waiters, etc.). The walker turns each
		// peer's file ECID into an MD4 hash as it goes — the wire contract is
		// hash-only — so it needs the map the downloads/shared walkers just
		// wrote: one acquisition hands it both.
		state.MutateClientsWithFiles(
			[&](std::map<std::uint32_t, ClientSnapshot> &cache, const FileMap &files) {
				ApplyGetUpdateToClients(resp, cache, files);
			});
		delete resp;

		// Fold this tick's peers into the known-clients store, so it stays
		// current from the update we already have rather than being re-read.
		// A no-op until something has asked for /known_clients, so a daemon
		// nobody queries never pays for it; after that it costs one hash
		// lookup per connected peer, bounded by MaxConnections.
		state.ReconcileKnownClients();
	}

	// /chats — one roundtrip carrying the cursor from the previous tick, so
	// the daemon replies with the session list plus only the messages we do
	// not have. Gated on the capability and skipped entirely otherwise: a
	// daemon predating the chat ops reaches the unknown-opcode branch of
	// ProcessRequest2(), which asserts rather than answering EC_OP_FAILED.
	if (app.IsServerChatActive()) {
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_GET_CHAT_SESSIONS));
		const std::uint32_t cursor = state.ChatCursor();
		if (cursor) {
			req->AddTag(CECTag(EC_TAG_CHAT_MSG_ID, cursor));
		}
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp)
			return false;
		// Collected under the write lock, published after it: emitting SSE
		// frames from inside the lambda would hold CState exclusively across
		// the event bus.
		std::vector<webapi::ChatSessionSnapshot> new_messages;
		std::vector<std::uint64_t> closed;
		state.MutateChats([&](std::vector<webapi::ChatSessionSnapshot> &cache, std::uint32_t &cur) {
			ApplyChatSessions(resp, cache, cur, new_messages, closed);
		});
		delete resp;
		PublishChatEvents(app.EventBus(), new_messages, closed);
	}

	// /logs/serverinfo, /stats/tree, /stats/graphs/{graph} are NOT
	// fetched per-tick — they're lazy-fetched on first GET via
	// CTtlCache (1 s TTL coalesces burst reads). HTTP handlers in
	// Api.cpp drive their own EC roundtrips under m_ec_mtx. Per-tick
	// refresh would have been pure waste when nothing is listening.

	// Searches this session never started itself (another EC client, or
	// the monolithic GUI) are NOT discovered here per-tick -- that would
	// pay an EC_OP_SEARCH_LIST roundtrip every tick forever to serve
	// something that happens rarely. Instead, HandleSearchResults in
	// Api.cpp does a one-off EC_OP_SEARCH_LIST check on a cache miss and
	// seeds the slot via MarkSearchDiscovered right there; from the next
	// tick on, the loop below picks it up like any other active search.

	// /search/results — poll each ACTIVE search independently (amuleapi runs
	// several at once). POST /search seeds a slot with active=true; the
	// daemon's per-id EC_TAG_SEARCH_LIFECYCLE_STATE tells us when to flip it
	// back. amuleapi pins a daemon carrying the lifecycle tags, so we read them
	// directly with no sentinel-decode fallback. Each request addresses its
	// search by EC_TAG_SEARCH_ID; a search evicted from the daemon's ring comes
	// back as EC_TAG_SEARCH_EXPIRED, which we resolve to a terminal snapshot.
	// Progress for every active search in ONE roundtrip when the daemon
	// advertises the union: an id-less EC_OP_SEARCH_PROGRESS answers with one
	// child per search it holds. Matters more here than in amuleGUI because
	// SendRecvSerialized is synchronous and process-wide mutexed, so N searches
	// meant N serialized round trips inside a single tick. Absence from the
	// union is the daemon saying it no longer holds that search, which is the
	// same verdict the per-id form reports as EC_TAG_SEARCH_EXPIRED.
	std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> union_progress;
	bool have_union = false;
	const std::vector<std::uint32_t> active_sids = state.ActiveSearchIds();
	// Nothing active means nothing to ask about. Without this guard the union
	// would cost a roundtrip every tick forever on an idle daemon, where the
	// per-id loop below simply had nothing to iterate.
	if (app.IsServerSearchProgressUnionActive() && !active_sids.empty()) {
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_SEARCH_PROGRESS));
		// Name the searches we track so the daemon bumps exactly those in its
		// LRU, matching what the per-id poll did.
		for (std::uint32_t sid : active_sids) {
			req->AddTag(CECTag(EC_TAG_SEARCH_ID, sid));
		}
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp)
			return false;
		// Only trust a reply that really is a union. Anything else -- an
		// EC_OP_FAILED, a packet with no search children -- must fall through
		// to the per-id polling below rather than be read as an empty union:
		// absence is the expiry signal, so a misread would retire every
		// tracked search in one pass. Slower, but correct.
		have_union = ParseSearchProgressUnion(resp, union_progress);
		delete resp;
	}

	for (std::uint32_t sid : active_sids) {
		std::uint32_t percent = 0;
		std::uint32_t lifecycle_state = 0;
		bool expired = false;
		switch (FetchSearchResults(app, state, sid)) {
		case SearchFetchOutcome::EcFailed:
			// Same rule as every other step in the tick: a failed roundtrip
			// bails the whole tick rather than exposing a half-refreshed cache.
			return false;
		case SearchFetchOutcome::Expired:
			expired = true;
			break;
		case SearchFetchOutcome::Updated:
			break;
		}
		if (!expired && have_union) {
			// Already fetched above; absent means the daemon dropped it.
			const auto found = union_progress.find(sid);
			if (found == union_progress.end()) {
				expired = true;
			} else {
				percent = found->second.first;
				lifecycle_state = found->second.second;
			}
		} else if (!expired) {
			std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_SEARCH_PROGRESS));
			req->AddTag(CECTag(EC_TAG_SEARCH_ID, sid));
			const CECPacket *resp = app.SendRecvSerialized(req.get());
			if (resp) {
				if (resp->GetTagByName(EC_TAG_SEARCH_EXPIRED)) {
					expired = true;
				} else {
					// Unified 0..100 the daemon computes for every kind
					// (global = real, Kad = cosmetic ramp, finished = 100).
					if (const CECTag *t =
							resp->GetTagByName(EC_TAG_SEARCH_LIFECYCLE_PERCENT)) {
						percent = static_cast<std::uint32_t>(t->GetInt());
					}
					if (const CECTag *t =
							resp->GetTagByName(EC_TAG_SEARCH_LIFECYCLE_STATE)) {
						lifecycle_state = static_cast<std::uint32_t>(t->GetInt());
					}
				}
				delete resp;
			}
		}
		if (expired) {
			// The daemon evicted this search (its ring is capped). Retire the
			// slot as finished + inactive so we stop polling it but keep the
			// last-known results for late reads; the terminal state also drives
			// a final search_progress SSE frame for subscribers.
			SearchProgressSnapshot fin = state.SearchProgress(sid);
			fin.active = false;
			fin.complete = true;
			fin.percent = 100;
			state.WriteSearchProgress(sid, fin);
			continue;
		}
		const SearchProgressSnapshot next =
			AdvanceSearchProgress(state.SearchProgress(sid), lifecycle_state, percent);
		state.WriteSearchProgress(sid, next);
	}

	// /preferences + /categories — one EC roundtrip populates both.
	// Selection bitmask requests every category the endpoint exposes
	// (issue #437 widened this from GENERAL|CONNECTIONS to all EC-
	// carried groups). Using the named enums (rather than hex literals)
	// so a future bit shuffle in ECCodes.h doesn't silently zero out a
	// section — bit-positional bugs here are hard to spot in JSON
	// (empty defaults look like "0 KB/s" not "field not requested").
	// STATISTICS is intentionally omitted (its serialize block is empty
	// — the 0x1B* tags carry live graph data, not stored prefs).
	{
		const std::uint32_t selection =
			EC_PREFS_CATEGORIES | EC_PREFS_GENERAL | EC_PREFS_CONNECTIONS | EC_PREFS_DIRECTORIES |
			EC_PREFS_FILES | EC_PREFS_SERVERS | EC_PREFS_SECURITY | EC_PREFS_MESSAGEFILTER |
			EC_PREFS_REMOTECONTROLS | EC_PREFS_ONLINESIG | EC_PREFS_CORETWEAKS |
			EC_PREFS_KADEMLIA | EC_PREFS_IP2COUNTRY;
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_GET_PREFERENCES));
		req->AddTag(CECTag(EC_TAG_SELECT_PREFS, selection));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp)
			return false;
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
	//
	// The ETag memo key rides on this, so it has to be bumped HERE and
	// not in MarkTickSuccess: the background loop calls that, but the
	// mutating handlers refresh inline and never do, so a mutation moved
	// the body while the key stood still and the next conditional GET was
	// answered 304 for content that had just changed.
	state.BumpSnapshotRevision();
	return true;
}

void EmitDiffsForEventBus(CamuleapiApp &app, const CState &state)
{
	// Sole writer of `app.LastSeenForEvents()`. ONLY the wxApp
	// refresher loop calls this; HTTP-server inline RefresherTick
	// call sites do NOT.
	EmitDiffsAndUpdate(app.EventBus(), app.LastSeenForEvents(), state);
}

void PrimeDiffBaseline(CamuleapiApp &app, const CState &state)
{
	// Same walk into a bus nobody reads -- diverting the events is what keeps
	// this short, instead of a `publish` flag through every emitter.
	CEventBus scratch(CEventBus::kMinCapacity);
	EmitDiffsAndUpdate(scratch, app.LastSeenForEvents(), state);
}

} // namespace webapi
