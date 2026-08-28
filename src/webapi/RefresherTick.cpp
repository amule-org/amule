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

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace webapi
{

// One id-less EC_OP_SEARCH_RESULTS at EC_DETAIL_INC_UPDATE: the daemon answers
// with every result of every search it holds, incrementally.
//
// This replaced one EC_DETAIL_FULL roundtrip per active search, each carrying
// that search's whole result set on every tick whether or not anything had
// changed. With eliding, an unchanged result costs nothing on the wire and an
// idle or finished search costs nothing at all -- which is what makes polling
// finished searches affordable, and is why the caller no longer filters on
// ActiveSearchIds().
//
// No single-search fallback: amuleapi already advertises multi-search
// unconditionally and never checks whether the daemon supports it (App.cpp),
// so it has always required a matching daemon for search. EC_TAG_CAN_PARTIAL_SEARCH
// is advertised for every client of CRemoteConnect, so the daemon is already
// eliding for this connection.
// Serialises the two issuers of the search stream across the roundtrip AND the
// apply. SendRecvSerialized only orders the roundtrips: both callers then take
// the State lock separately, so a union reply and a FULL reply can still be
// applied in the opposite order to the one they were fetched in. That matters
// because the union's tombstones are one-shot -- the daemon drops an ECID from
// io_lastSentResultIds as it emits EC_TAG_FILE_REMOVED for it -- so a stale
// FULL landing after a tombstone re-inserts a row nothing will ever remove
// again. Held across the EC call, which is the same wait the EC worker already
// imposes; the State lock is always taken inside this one, never the reverse.
std::mutex g_search_stream_mtx;

SearchFetchOutcome FetchSearchResults(CamuleapiApp &app, CState &state)
{
	std::lock_guard<std::mutex> stream_lock(g_search_stream_mtx);
	std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_SEARCH_RESULTS, EC_DETAIL_INC_UPDATE));
	// Opt into result grouping (issue #431): the empty EC_TAG_SEARCH_PARENT
	// flag tells the responder to also emit each same-hash/different-name
	// child so the results list can nest them.
	req->AddTag(CECEmptyTag(EC_TAG_SEARCH_PARENT));
	const CECPacket *resp = app.SendRecvSerialized(req.get());
	if (!resp)
		return SearchFetchOutcome::EcFailed;
	state.MutateAllSearches([&](std::map<std::uint32_t, SearchSlot> &slots,
					std::map<std::uint32_t, std::uint32_t> &owner) {
		ApplySearchUnion(resp, slots, owner);
	});
	delete resp;
	return SearchFetchOutcome::Updated;
}

// Re-fetch ONE search at EC_DETAIL_FULL, bypassing the union entirely.
//
// The union is a stateful differential stream: the daemon records what it has
// sent this connection and then sends only changes, with no opcode for "send
// it all again". That makes it the wrong tool twice over, and this is the
// escape hatch for both.
//
// Seeding a newly discovered slot. The union responder walks
// GetKnownSearchIds() -- every search the core holds, including ones started
// in amulegui or the monolithic GUI. ApplySearchUnion drops results for a
// search it has no slot for, but the daemon has already marked those ECIDs
// delivered and seeded its valuemap, so on the next poll they are unchanged
// and elided: dropped once means dropped forever. A slot created later by
// discovery would stay empty. This fetch is what fills it.
//
// Serving the HTTP thread. The union must have exactly one issuer, or two
// replies can be applied out of order and leave a ghost row no later poll can
// clear. A FULL reply is self-contained, but it is not ordered against the
// union's one-shot tombstones, so this shares g_search_stream_mtx with the
// union rather than relying on idempotence alone.
//
// Re-seeding after a lost union reply, with `replace`. A merge cannot express
// a deletion, and a FULL reply carries no tombstones, so a plain merge would
// leave behind rows the daemon has since dropped. In replace mode the slot's
// results are swapped wholesale for what the daemon reports now.
SearchFetchOutcome FetchOneSearchFull(CamuleapiApp &app, CState &state, std::uint32_t search_id, bool replace)
{
	std::lock_guard<std::mutex> stream_lock(g_search_stream_mtx);
	std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_SEARCH_RESULTS, EC_DETAIL_FULL));
	req->AddTag(CECEmptyTag(EC_TAG_SEARCH_PARENT));
	req->AddTag(CECTag(EC_TAG_SEARCH_ID, search_id));
	const CECPacket *resp = app.SendRecvSerialized(req.get());
	if (!resp)
		return SearchFetchOutcome::EcFailed;
	SearchFetchOutcome outcome = SearchFetchOutcome::Updated;
	if (resp->GetTagByName(EC_TAG_SEARCH_EXPIRED)) {
		outcome = SearchFetchOutcome::Expired;
	} else {
		// Same merge the union uses. The per-search responder builds its tags
		// without a valuemap, so every field of every result is present and
		// the merge's absent-means-unchanged rule simply never fires. The
		// reply carries no EC_TAG_SEARCH_ID of its own, hence the explicit id.
		state.MutateAllSearches([&](std::map<std::uint32_t, SearchSlot> &slots,
						std::map<std::uint32_t, std::uint32_t> &owner) {
			ApplySearchFullReply(resp, slots, owner, search_id, replace);
		});
	}
	if (outcome == SearchFetchOutcome::Expired) {
		// Definitive answer, just not a useful one: the daemon no longer has
		// this search, so retirement owns the slot from here. Clearing the
		// flag stops it being re-requested every tick forever.
		state.ClearSearchResyncFlag(search_id);
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

	// /logs/server_info, /stats/tree, /stats/graphs/{graph} are NOT
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
	// Every slot the daemon could still speak for, finished ones included.
	// A finished search is never polled for progress -- there is none left to
	// report -- but it still has to be watched for EXPIRY, because the ring
	// evicting it is what tombstones its results, and a slot that is not
	// detached by then has them erased by the union below. That is the whole
	// point of detaching: keep the last-known results for late reads.
	//
	// Naming these ids is also what stops the eviction happening so soon.
	// The daemon touches its LRU for exactly the ids a client names, so
	// leaving finished searches out made them the least-recently-used and the
	// first victims of anyone's next search -- amuleapi stopped refreshing
	// precisely the searches it had decided to keep.
	const std::vector<std::uint32_t> attached_sids = state.AttachedSearchIds();
	// Nothing attached means nothing to ask about. Without this guard the
	// union would cost a roundtrip every tick forever on an idle daemon,
	// where the per-id loop below simply had nothing to iterate.
	if (app.IsServerSearchProgressUnionActive() && !attached_sids.empty()) {
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_SEARCH_PROGRESS));
		// Name the searches we track so the daemon bumps exactly those in its
		// LRU, matching what the per-id poll did.
		for (std::uint32_t sid : attached_sids) {
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

	// Per-search lifecycle, off the progress union fetched above (or one
	// roundtrip each against an older daemon). Runs BEFORE the results poll
	// below so an eviction is seen, and the slot frozen, while its results
	// are still there to keep -- see the `expired` branch.
	for (std::uint32_t sid : attached_sids) {
		std::uint32_t percent = 0;
		std::uint32_t lifecycle_state = 0;
		bool expired = false;
		// A finished slot is here for the expiry verdict only. Its progress
		// is terminal and must not be re-derived: AdvanceSearchProgress reads
		// a missing lifecycle tag as IDLE and would reset complete/percent
		// back to 0, turning a finished search into an idle one.
		const bool was_active =
			std::find(active_sids.begin(), active_sids.end(), sid) != active_sids.end();
		if (have_union) {
			// Already fetched above; absent means the daemon dropped it.
			const auto found = union_progress.find(sid);
			if (found == union_progress.end()) {
				expired = true;
			} else {
				percent = found->second.first;
				lifecycle_state = found->second.second;
			}
		} else if (was_active) {
			// No union on this daemon: fall back to one roundtrip per search,
			// and only for the active ones. Paying a roundtrip per FINISHED
			// slot every tick to learn about an eviction that may never come
			// is the trade the union makes cheap and this form does not.
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
			//
			// Detaching is what makes "keep the last-known results" true. The
			// same eviction makes the union emit an EC_TAG_FILE_REMOVED for
			// every one of this search's results, which would erase precisely
			// what is being kept -- so the slot is frozen here, before the
			// union below is fetched, and ApplySearchUnion then leaves it be.
			// Hence this loop running ahead of the results poll: the progress
			// union it reads was fetched separately, above, so the order is
			// free.
			state.DetachSearch(sid);
			SearchProgressSnapshot fin = state.SearchProgress(sid);
			fin.active = false;
			fin.complete = true;
			fin.percent = 100;
			state.WriteSearchProgress(sid, fin);
			continue;
		}
		if (!was_active) {
			// Not expired, and not active: nothing to advance. Leaving the
			// snapshot alone is what keeps a finished search finished.
			continue;
		}
		const SearchProgressSnapshot next =
			AdvanceSearchProgress(state.SearchProgress(sid), lifecycle_state, percent);
		state.WriteSearchProgress(sid, next);
	}

	// Results for every search in one roundtrip.
	//
	// Gated on there being any search at all, not on which ones are active. A
	// finished search still changes -- a hit gets downloaded, a Kad notes
	// lookup lands -- and now costs nothing to keep polling, which is what
	// lets those changes be seen rather than only appearing on a client's
	// next read. What is not worth paying for is the empty case: a daemon
	// holding no searches would otherwise take a roundtrip a second forever
	// to be told so.
	//
	// Deliberately NOT gated on SSE subscribers. The diff walk is (see
	// CEventBus's subscriber accounting), but the fetch cannot be: a REST
	// client polling GET /search/{id}/results reads this cache, and for an
	// active search nothing else refreshes it -- ClaimSearchRefresh covers
	// only slots that are not active. Skipping the fetch when nobody is
	// subscribed would hand that client frozen results.
	//
	// The gate asks about OUR slots -- specifically the ones the daemon could
	// still speak for, since a detached slot's search has already been
	// evicted core-side and polling for it would never return anything. It
	// does not ask about the daemon's searches, and the daemon never tells us
	// about one unasked: a slot exists only because
	// this process started the search or because a read discovered it
	// (RequireSearch -> DiscoverSearchIfHeldByCore, a one-off
	// EC_OP_SEARCH_LIST on a cache miss). A search begun in amulegui or the
	// monolithic GUI therefore leaves this false, and the union is not sent.
	//
	// Skipping the poll is not merely an optimisation there, it is the point.
	// ApplySearchUnion drops results for a search it has no slot for, and the
	// daemon marks them sent regardless, so polling with nothing to apply
	// them to would burn through a foreign search's results once and elide
	// them forever after. Neither discovery route runs through here:
	// GET /search goes straight to EC_OP_SEARCH_LIST every call, and a by-id
	// read seeds the slot in full via FetchOneSearchFull before this opens.
	// Anything the last failed union reply covered is unrecoverable from the
	// stream itself, so re-seed those slots in full before polling again.
	// Ordered after the retirement loop so a slot the daemon has dropped is
	// already detached by the time this runs, and SearchesNeedingResync skips
	// it: the ordering alone only makes the slot detached, it does not keep it
	// out of the list.
	for (std::uint32_t sid : state.SearchesNeedingResync()) {
		if (FetchOneSearchFull(app, state, sid, /*replace=*/true) == SearchFetchOutcome::EcFailed) {
			return false;
		}
	}
	// The same set the progress union above asked about, reused rather than
	// recomputed against a second spelling of the same !detached test. A slot
	// an HTTP thread created since is missed for this one tick and picked up
	// by the next, which is the poll interval either way.
	if (!attached_sids.empty()) {
		if (FetchSearchResults(app, state) == SearchFetchOutcome::EcFailed) {
			// The daemon commits its differential state while building the
			// reply, so what this one carried is already gone from its point
			// of view. Flag the slots for a full re-seed above on the next
			// tick; without it every result that reply covered would be
			// elided from every later poll.
			state.MarkAllSearchesNeedResync();
			// Same rule as every other step in the tick: a failed roundtrip
			// bails the whole tick rather than exposing a half-refreshed
			// cache.
			return false;
		}
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
