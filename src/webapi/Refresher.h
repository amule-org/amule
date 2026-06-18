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

#ifndef WEBAPI_REFRESHER_H
#define WEBAPI_REFRESHER_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>


class CECPacket;
class CamuleapiApp;
class PartFileEncoderData;


namespace webapi {


class CState;


// Single tick of the EC poller. Issues every cached request (Phase 4a:
// EC_OP_STAT_REQ only), parses each response into a snapshot struct,
// writes it under CState's exclusive lock. Returns true on success,
// false if any EC roundtrip failed (caller flips
// CState::MarkTickFailure and leaves the stale data in place).
//
// Runs on the wxApp thread (the same thread CaMuleExternalConnector's
// CRemoteConnect uses for its socket I/O), so it can issue the EC
// roundtrip synchronously without any thread-marshalling. Phase 5+'s
// mutation handlers run on the HTTP threads and reach EC through a
// process-wide mutex around `CamuleapiApp::SendRecvMsg_v2`, so the
// refresher and the mutation paths share the same EC-traffic budget.
//
// Pure-function shape (takes app + state by reference, returns bool)
// so the tick body is unit-testable against a mock EC reply once we
// ship the EC fixture harness (Phase 4b candidate).
bool RefresherTick(CamuleapiApp &app, CState &state);


// Sub-tick helpers exposed for testing. The Refresher uses these
// internally; the unit test calls them against hand-crafted
// CECPacket fixtures to pin the EC-tag-to-State mapping without
// standing up a real amuled.

struct StatusSnapshot;
struct DownloadSnapshot;
struct ClientSnapshot;
struct SharedSnapshot;
struct ServerSnapshot;
struct KadSnapshot;
struct CategorySnapshot;
struct PreferencesSnapshot;

void ParseStatusFromPacket(const CECPacket *resp, StatusSnapshot &out);
// Kad detail rides the same STAT_REQ response — saves a roundtrip
// since amuled bundles `EC_TAG_STATS_KAD_*` into the standard CMD-
// level stats packet. /status calls ParseStatus then /kad calls
// this against the same packet pointer.
void ParseKadFromPacket   (const CECPacket *resp, KadSnapshot &out);

// Drain new amule-log lines from the STAT_REQ response. amule's EC
// server piggybacks them inside an `EC_TAG_STATS_LOGGER_MESSAGE`
// parent tag with child `EC_TAG_STRING` tags, but ONLY when the
// STAT_REQ was issued at `EC_DETAIL_FULL` (or INC_UPDATE). The
// refresher calls this on the same packet as ParseStatus / ParseKad,
// then `state.AppendAmuleLog(...)` to fold them into the cache.
void ParseAmuleLogFromPacket(const CECPacket *resp,
                             std::vector<std::string> &out_new_lines);

// `EC_OP_GET_PREFERENCES` response → flat prefs + bundled categories
// (the EC packet carries categories under `EC_TAG_PREFS_CATEGORIES`).
// One roundtrip populates both /preferences and /categories.
void ParsePreferencesFromPacket(const CECPacket *resp,
                                PreferencesSnapshot &out_prefs,
                                std::vector<CategorySnapshot> &out_cats);

// `EC_OP_GET_UPDATE` at `EC_DETAIL_INC_UPDATE` is the consolidated
// fetch that backs downloads + shared + servers in a single
// roundtrip. The response packet shape (from
// `Get_EC_Response_GetUpdate` at ExternalConn.cpp:869) is:
//   * top-level interleaved `EC_TAG_PARTFILE` (downloads) and
//     `EC_TAG_KNOWNFILE` (shared) — full identity is shipped on first
//     encounter, stat-only deltas on subsequent ticks. The valuemap
//     gating happens server-side so unchanged entries are simply
//     omitted from the response.
//   * top-level `EC_TAG_FILE_REMOVED` markers — explicit deletion
//     events for downloads OR shared (the encoder map is unified
//     server-side, so a single ECID maps to one of the two caches).
//   * `EC_TAG_SERVER` container with per-server `CEC_Server_Tag`
//     children — server list ships the FULL list every tick (no
//     FILE_REMOVED equivalent on the server side), with valuemap
//     suppression of unchanged per-server fields.
//   * `EC_TAG_CLIENT` container — peers; amuleapi IGNORES this in
//     favour of `EC_OP_GET_ULOAD_QUEUE` so /uploads stays bound to
//     the upload queue semantic (the GET_UPDATE clients block is
//     filtered server-side by the global
//     `TransmitOnlyUploadingClients` pref which would pollute
//     amuleweb/amulegui's view if we set it).
//   * `EC_TAG_FRIEND` container — ignored.
//
// Why one consolidated op instead of four substruct fetches: the
// per-substruct `EC_OP_GET_*` paths at `EC_DETAIL_UPDATE` strip
// identity from the response (ECSpecialCoreTags.cpp:244-246's
// early-return on `== EC_DETAIL_UPDATE`), which forced the two-phase
// (UPDATE → FULL) polling protocol the old refresher used. INC_UPDATE
// does NOT hit that early-return, so identity arrives in one shot —
// no Phase 2 EC roundtrip, no `require_partfile_hash_on_insert`
// defence, no `HasChildTags()` alive-marker defence. The wire-level
// races that motivated those defences (#713 and #808) only exist at
// `EC_DETAIL_UPDATE`.
//
// The three helpers below each iterate the same response once,
// filtering for their tag type. Called under three distinct CState
// mutator acquisitions per tick — matches the existing per-substruct
// lock-acquisition pattern (the snapshot_at is set after the whole
// tick succeeds, so cross-substruct consistency was already best-
// effort).

void ApplyGetUpdateToDownloads(
	const CECPacket *resp,
	std::map<std::uint32_t, DownloadSnapshot> &cache,
	std::map<std::uint32_t, PartFileEncoderData> &rle_state);

void ApplyGetUpdateToShared(
	const CECPacket *resp,
	std::map<std::uint32_t, SharedSnapshot>   &cache);

void ApplyGetUpdateToServers(
	const CECPacket *resp,
	std::map<std::uint32_t, ServerSnapshot>   &cache);


// /stats/tree (EC_OP_GET_STATSTREE response). Recursive walk —
// every EC_TAG_STATTREE_NODE that contains children becomes a
// branch; leaves get `children.empty()`. The top-level `root` is
// an unnamed container; we skip the root and emit its direct
// children as the visible tree (matches amuleweb's
// `am_load_stats_tree.php` behaviour).
struct StatsTreeNode;
void ParseStatsTreeFromPacket(const CECPacket *resp, StatsTreeNode &out);

// /stats/graphs/{graph} (EC_OP_GET_STATSGRAPHS response). amuled
// packs the four time-series (download/upload/connections+kad as
// two interleaved channels in EC_TAG_STATSGRAPH_DATA + a separate
// EC_TAG_STATSGRAPH_DATA_CONN) into byte blobs. Parser un-packs
// them into the four separate vectors of `StatsGraphs`.
struct StatsGraphs;
void ParseGraphsFromPacket(const CECPacket *resp, StatsGraphs &out);

// /search/results (EC_OP_SEARCH_RESULTS response). Full-state fetch
// per tick; like /servers, no INC path exists for the search list.
// Cache is keyed by ECID; cleared on each refresher tick before
// applying. Phase 5 will add POST /search to drive the underlying
// query lifecycle.
struct SearchResult;
void ApplySearchFull(const CECPacket *resp,
                     std::map<std::uint32_t, SearchResult> &cache);

// Two-phase polling protocol (mirrors `WebServer.h::DoRequery` in the
// reference branch).
//
// **Phase 1 — UPDATE.** Caller issues
// `CECPacket(op, EC_DETAIL_UPDATE)`. The server's `skip_unchanged_path`
// kicks in (when partial_update_active is true): only changed files
// produce tags, and deletions surface as EC_TAG_FILE_REMOVED markers.
// `ApplyUpdate` walks the response, applies the UPDATE-level deltas
// (stats only, no identity) to known ECIDs, removes ECIDs flagged
// with EC_TAG_FILE_REMOVED, and returns a `set<uint32_t>` of NEW
// ECIDs that need Phase 2 to populate their identity fields.
//
// When `partial_update_active=false`, the server falls back to
// alive-marker tags for unchanged files; `ApplyUpdate` bulk-deletes
// cache entries we didn't see in this Phase 1 response (the legacy
// "absence implies removed" semantics).
//
// **Phase 2 — FULL.** Caller builds `CECPacket(op)` (defaulting to
// EC_DETAIL_FULL) populated with `CECTag(item_tag, ecid)` for each
// new ECID, sends it, and passes the response to `ApplyFull`.
// ApplyFull inserts brand-new entries with full identity + initial
// stats.
//
// Defences against #713 (alive-marker for unknown ECID) and #808
// (UPDATE response carries change-only tags for a never-seen ECID
// → no identity) are inlined.

std::set<std::uint32_t> ApplyDownloadsUpdate(
	const CECPacket *resp,
	bool partial_update_active,
	std::map<std::uint32_t, DownloadSnapshot> &cache,
	std::map<std::uint32_t, PartFileEncoderData> &rle_state);

void ApplyDownloadsFull(
	const CECPacket *resp,
	std::map<std::uint32_t, DownloadSnapshot> &cache,
	std::map<std::uint32_t, PartFileEncoderData> &rle_state);

// `ApplyGetUpdateToClients` consumes the EC_TAG_CLIENT container
// from the consolidated GET_UPDATE response. The walker uses
// "seen-this-tick = keep, absent = evict" semantics: every alive
// client surfaces every tick via the outer per-client tag (CValueMap
// suppression operates on the tag's *children*, not on the entity
// itself), so cache entries not seen in this response are gone on
// amuled's side (peer disconnected, dropped from queue, banned).
void ApplyGetUpdateToClients(
	const CECPacket *resp,
	std::map<std::uint32_t, ClientSnapshot>   &cache);


}  // namespace webapi

#endif // WEBAPI_REFRESHER_H
