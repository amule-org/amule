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
// Refresher orchestration -- the per-tick loop body that issues EC
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

// One id-less EC_OP_SEARCH_RESULTS at EC_DETAIL_INC_UPDATE: the daemon answers with every result of
// every search it holds, incrementally. This replaced one EC_DETAIL_FULL roundtrip per active
// search, each carrying that search's whole result set every tick whether or not anything had
// changed. With eliding, an unchanged result costs nothing on the wire and an idle or finished
// search nothing at all -- which makes polling finished searches affordable, and is why the caller
// no longer filters on ActiveSearchIds().
//
// No single-search fallback: amuleapi advertises multi-search unconditionally and never checks
// whether the daemon supports it (App.cpp), so it has always required a matching daemon for search.
// EC_TAG_CAN_PARTIAL_SEARCH is advertised for every client of CRemoteConnect, so the daemon is
// already eliding for this connection.
//
// The mutex serialises the two issuers of the search stream across the roundtrip AND the apply.
// SendRecvSerialized only orders the roundtrips; both callers then take the State lock separately,
// so a union reply and a FULL reply can still be applied in the opposite order to the one they were
// fetched in. The union's tombstones are one-shot -- the daemon drops an ECID from
// io_lastSentResultIds as it emits EC_TAG_FILE_REMOVED for it -- so a stale FULL landing after a
// tombstone re-inserts a row nothing will ever remove. Held across the EC call, the same wait the
// EC worker already imposes; the State lock is always taken inside this one.
std::mutex g_search_stream_mtx;

SearchFetchOutcome FetchSearchResults(CamuleapiApp &app, CState &state)
{
	std::lock_guard<std::mutex> stream_lock(g_search_stream_mtx);
	std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_SEARCH_RESULTS, EC_DETAIL_INC_UPDATE));
	// Opt into result grouping (issue #431): the empty EC_TAG_SEARCH_PARENT flag tells the
	// responder to also emit each same-hash/different-name child so the results list can nest
	// them.
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
// The union is a stateful differential stream: the daemon records what it has sent this connection
// and then sends only changes, with no opcode for "send it all again". This is the escape hatch,
// needed three ways.
//
// Seeding a newly discovered slot: the union responder walks every search the core holds,
// ApplySearchUnion drops results for a search it has no slot for, and the daemon has already marked
// those ECIDs delivered -- dropped once means dropped forever, so a slot created later by discovery
// would stay empty.
//
// Serving the HTTP thread: the union must have exactly one issuer, or two replies can be applied
// out of order and leave a ghost row no later poll can clear. A FULL reply is self-contained but
// not ordered against the union's one-shot tombstones, so this shares g_search_stream_mtx.
//
// Re-seeding after a lost union reply, with `replace`: a merge cannot express a deletion and a FULL
// reply carries no tombstones, so replace mode swaps the slot's results wholesale.
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
		// Same merge the union uses. The per-search responder builds its tags without a
		// valuemap, so every field is present and the merge's absent-means-unchanged rule
		// never fires. The reply carries no EC_TAG_SEARCH_ID of its own, hence the explicit
		// id.
		state.MutateAllSearches([&](std::map<std::uint32_t, SearchSlot> &slots,
						std::map<std::uint32_t, std::uint32_t> &owner) {
			ApplySearchFullReply(resp, slots, owner, search_id, replace);
		});
	}
	if (outcome == SearchFetchOutcome::Expired) {
		// Definitive answer, just not a useful one: the daemon no longer has this search,
		// so retirement owns the slot. Clearing the flag stops it being re-requested every
		// tick.
		state.ClearSearchResyncFlag(search_id);
	}
	delete resp;
	return outcome;
}

bool RefresherTick(CamuleapiApp &app, CState &state)
{
	// Per-tick budget: a few EC ops via SendRecvSerialized (m_ec_mtx-serialised). Any failure
	// bails the whole tick so the cache stays internally consistent. STAT_REQ runs first as the
	// cheapest probe: an EC drop between ticks is caught before larger queries burn roundtrips.

	// /status + /kad + /logs/amule share one STAT_REQ packet. Detail level CMD -> FULL because
	// amuled only piggybacks EC_TAG_STATS_LOGGER_MESSAGE at FULL or INC_UPDATE. FULL also
	// carries STATS_UP_OVERHEAD / STATS_DOWN_OVERHEAD and the two free-space tags, so dropping
	// to CMD would silently empty those fields too.
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

	// /downloads + /shared + /servers in a single GET_UPDATE roundtrip at EC_DETAIL_INC_UPDATE.
	// The response packet shape and the "why INC_UPDATE works in one tick" rationale are
	// documented next to ApplyGetUpdateToDownloads in Refresher.h.
	//
	// The response also carries EC_TAG_CLIENT (filtered server-side by
	// TransmitOnlyUploadingClients) and EC_TAG_FRIEND containers, consumed below into /clients
	// and /friends -- /uploads stays bound to the upload-queue semantic via
	// EC_OP_GET_ULOAD_QUEUE.
	{
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_GET_UPDATE, EC_DETAIL_INC_UPDATE));
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp)
			return false;
		auto &rle = app.PartfileRleStateRequireStateWriteLock();

		// Snapshot the cache's pre-tick ECID set so rle_state entries can be
		// evicted for any partfile removed during the walk.
		std::set<std::uint32_t> ecids_before;
		state.MutateDownloads([&](FileMap &cache) {
			for (const auto &kv : cache) {
				if (kv.second.is_downloading)
					ecids_before.insert(kv.first);
			}
			ApplyGetUpdateToDownloads(resp, cache, rle);
			// Evict RLE state for ECIDs that no longer carry the downloading role.
			// The walker handles FILE_REMOVED already; this is defence in depth.
			for (auto ecid : ecids_before) {
				auto it = cache.find(ecid);
				if (it == cache.end() || !it->second.is_downloading) {
					rle.erase(ecid);
				}
			}
		});

		// Shared walker reads and writes the same unified m_files map: a partfile whose
		// hash was CValueMap-suppressed already carries hash + name from the downloads
		// walker above.
		//
		// Same `rle` map: the shared walker decodes the availability blob on
		// EC_TAG_KNOWNFILE tags and the downloads walker the EC_TAG_PARTFILE ones, and
		// amuled emits exactly one per ECID. The eviction sweep above only touches ECIDs
		// that were downloading.
		state.MutateShared([&](FileMap &cache) { ApplyGetUpdateToShared(resp, cache, rle); });

		state.MutateServers([&](std::map<std::uint32_t, ServerSnapshot> &cache) {
			ApplyGetUpdateToServers(resp, cache);
		});

		state.MutateFriends([&](std::map<std::uint32_t, FriendSnapshot> &cache) {
			ApplyGetUpdateToFriends(resp, cache);
		});

		// /clients -- every alive peer in theApp->clientlist. The walker turns each peer's
		// file ECID into an MD4 hash as it goes, the wire contract being hash-only, so it
		// needs the map the downloads/shared walkers just wrote: one acquisition hands it
		// both.
		state.MutateClientsWithFiles(
			[&](std::map<std::uint32_t, ClientSnapshot> &cache, const FileMap &files) {
				ApplyGetUpdateToClients(resp, cache, files);
			});
		delete resp;

		// Fold this tick's peers into the known-clients store, so it stays current from the
		// update we already have rather than being re-read. A no-op until something asks
		// for /known_clients; after that it costs one hash lookup per connected peer.
		state.ReconcileKnownClients();
	}

	// /chats -- one roundtrip carrying the cursor from the previous tick, so the daemon replies
	// with the session list plus only the messages we do not have. Gated on the capability: a
	// daemon predating the chat ops reaches ProcessRequest2()'s unknown-opcode branch, which
	// asserts.
	if (app.IsServerChatActive()) {
		std::unique_ptr<CECPacket> req(new CECPacket(EC_OP_GET_CHAT_SESSIONS));
		const std::uint32_t cursor = state.ChatCursor();
		if (cursor) {
			req->AddTag(CECTag(EC_TAG_CHAT_MSG_ID, cursor));
		}
		const CECPacket *resp = app.SendRecvSerialized(req.get());
		if (!resp)
			return false;
		// Collected under the write lock, published after it: emitting SSE frames
		// from inside the lambda would hold CState exclusively across the bus.
		std::vector<webapi::ChatSessionSnapshot> new_messages;
		std::vector<std::uint64_t> closed;
		state.MutateChats([&](std::vector<webapi::ChatSessionSnapshot> &cache, std::uint32_t &cur) {
			ApplyChatSessions(resp, cache, cur, new_messages, closed);
		});
		delete resp;
		PublishChatEvents(app.EventBus(), new_messages, closed);
	}

	// /logs/server_info, /stats/tree and /stats/graphs/{graph} are lazy-fetched on first GET
	// via CTtlCache (a 1 s TTL coalesces burst reads), under m_ec_mtx, not polled per tick.

	// Searches this session never started are NOT discovered per tick -- that would pay an
	// EC_OP_SEARCH_LIST roundtrip every tick forever. HandleSearchResults does a one-off check
	// on a cache miss and seeds the slot; from the next tick the loop below picks it up.

	// /search/results -- poll each ACTIVE search independently. POST /search seeds a slot with
	// active=true; the daemon's per-id EC_TAG_SEARCH_LIFECYCLE_STATE says when to flip it back.
	// A search evicted from the daemon's ring comes back as EC_TAG_SEARCH_EXPIRED, resolved to
	// a terminal snapshot.
	//
	// Progress for every active search comes in ONE roundtrip when the daemon advertises the
	// union. That matters more here than in amuleGUI because SendRecvSerialized is synchronous
	// and process-wide mutexed, so N searches meant N serialized roundtrips inside a single
	// tick.
	std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> union_progress;
	bool have_union = false;
	const std::vector<std::uint32_t> active_sids = state.ActiveSearchIds();
	// Every slot the daemon could still speak for, finished ones included. A finished search is
	// never polled for progress, but it still has to be watched for EXPIRY: the ring evicting
	// it is what tombstones its results, and a slot not detached by then has them erased by the
	// union below. Naming these ids also delays the eviction -- the daemon touches its LRU for
	// exactly the ids a client names, so leaving finished searches out made them the first
	// victims of anyone's next search.
	const std::vector<std::uint32_t> attached_sids = state.AttachedSearchIds();
	// Nothing attached means nothing to ask about. Without this guard the union
	// would cost a roundtrip every tick forever on an idle daemon.
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
		// Only trust a reply that really is a union. Anything else (an EC_OP_FAILED, a
		// packet with no search children) must fall through to the per-id polling below:
		// absence is the expiry signal, so reading it as an empty union would retire every
		// tracked search.
		have_union = ParseSearchProgressUnion(resp, union_progress);
		delete resp;
	}

	// Per-search lifecycle, off the progress union above. Runs BEFORE the results poll so an
	// eviction is seen, and the slot frozen, while its results are still there.
	for (std::uint32_t sid : attached_sids) {
		std::uint32_t percent = 0;
		std::uint32_t lifecycle_state = 0;
		bool expired = false;
		// A finished slot is here for the expiry verdict only. Its progress is
		// terminal and must not be re-derived: AdvanceSearchProgress reads a
		// missing lifecycle tag as IDLE and would reset complete/percent to 0.
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
			// No union on this daemon: one roundtrip per search, and only the active
			// ones. A roundtrip per FINISHED slot every tick, for an eviction that may
			// never come, is the trade the union makes cheap and this form does not.
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
			// The daemon evicted this search (its ring is capped). Retire the slot as
			// finished + inactive so we stop polling it but keep the last-known results
			// for late reads; the terminal state also drives a final search_progress
			// SSE frame. Detaching is what makes "keep the last-known results" true:
			// the same eviction makes the union emit an EC_TAG_FILE_REMOVED for every
			// one of this search's results. Hence this loop running ahead of the
			// results poll.
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

	// Anything the last failed union reply covered is unrecoverable from the stream itself, so
	// re-seed those slots in full before polling again. Ordered after the retirement loop so a
	// slot the daemon has dropped is already detached and SearchesNeedingResync skips it.
	for (std::uint32_t sid : state.SearchesNeedingResync()) {
		if (FetchOneSearchFull(app, state, sid, /*replace=*/true) == SearchFetchOutcome::EcFailed) {
			return false;
		}
	}
	// Results for every search in one roundtrip.
	//
	// Gated on there being any attached search at all, not on which ones are active: a finished
	// search still changes -- a hit gets downloaded, a Kad notes lookup lands -- and costs
	// nothing to keep polling. What is not worth paying for is the empty case.
	//
	// Deliberately NOT gated on SSE subscribers, unlike the diff walk: a REST client polling
	// GET /search/{id}/results reads this cache, and for an active search nothing else
	// refreshes it (ClaimSearchRefresh covers only inactive slots), so skipping the fetch would
	// hand that client frozen results.
	//
	// The gate asks about OUR slots, not the daemon's searches. Skipping the poll when we hold
	// none is the point, not an optimisation: ApplySearchUnion drops results for a search it
	// has no slot for and the daemon marks them sent regardless, so polling with nothing to
	// apply them to would burn through a foreign search's results once and elide them forever
	// after.
	//
	// The set is the same one the progress union asked about, reused rather than recomputed
	// against a second spelling of the same !detached test. A slot an HTTP thread created since
	// is picked up by the next tick.
	if (!attached_sids.empty()) {
		if (FetchSearchResults(app, state) == SearchFetchOutcome::EcFailed) {
			// The daemon commits its differential state while building the reply, so
			// what this one carried is already gone from its point of view. Flag the
			// slots for a full re-seed above on the next tick; without it every
			// result that reply covered would be elided from every later poll.
			state.MarkAllSearchesNeedResync();
			// Same rule as every other step in the tick: a failed roundtrip bails the
			// whole tick rather than exposing a half-refreshed cache.
			return false;
		}
	}

	// /preferences + /categories -- one EC roundtrip populates both. The selection bitmask
	// requests every category the endpoint exposes, spelled with the named enums rather than
	// hex literals so a future bit shuffle in ECCodes.h cannot silently zero out a section.
	// STATISTICS is omitted: its serialize block is empty, the 0x1B* tags being live graph
	// data.
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

	// EmitDiffsAndUpdate is intentionally NOT called here. Mutation handlers invoke
	// RefresherTick() inline on HTTP threads so the response sees post-mutation state, and
	// LastSeenState has no internal lock -- concurrent std::map mutation from the wxApp
	// refresher loop and an HTTP thread is UB.
	//
	// The ETag memo key rides on this, so it must be bumped HERE, not in MarkTickSuccess: only
	// the background loop calls that, so a mutation moved the body while the key stood still
	// and the next conditional GET was answered 304.
	state.BumpSnapshotRevision();
	return true;
}

void EmitDiffsForEventBus(CamuleapiApp &app, const CState &state)
{
	// Sole writer of `app.LastSeenForEvents()`. ONLY the wxApp refresher loop
	// calls this; the HTTP-server inline RefresherTick call sites do NOT.
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
