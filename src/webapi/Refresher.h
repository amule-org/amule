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
#include <ctime> // std::time_t — needed for AdvanceSearchProgress
#include <map>
#include <string>
#include <vector>

class CECPacket;
class CamuleapiApp;
class PartFileEncoderData;

namespace webapi
{

class CState;

// Single tick of the EC poller. Issues every cached request, parses
// each response into a snapshot struct, writes it under CState's
// exclusive lock. Returns true on success, false if any EC roundtrip
// failed (caller flips CState::MarkTickFailure and leaves stale
// data in place).
//
// Runs on the wxApp thread (same thread CRemoteConnect uses for its
// socket I/O), so it can issue the EC roundtrip synchronously without
// thread-marshalling. Mutation handlers on the HTTP threads reach EC
// through a process-wide mutex around `CamuleapiApp::SendRecvMsg_v2`,
// so refresher + mutations share the same EC-traffic budget.
//
// Pure-function shape (app + state by reference, returns bool) so the
// tick body is unit-testable against a mock EC reply.
bool RefresherTick(CamuleapiApp &app, CState &state);

// Single-threaded SSE diff emission. Called ONLY from the wxApp
// refresher loop after a successful RefresherTick so that the
// LastSeenState walk (which mutates app.LastSeenForEvents()) is
// single-writer. Inline-from-HTTP RefresherTick call sites
// deliberately skip it — SSE subscribers see the post-mutation
// diff on the next 1-second tick instead of immediately.
void EmitDiffsForEventBus(CamuleapiApp &app, const CState &state);

// Sub-tick helpers exposed for testing. The Refresher uses these
// internally; the unit test calls them against hand-crafted
// CECPacket fixtures to pin the EC-tag-to-State mapping without
// standing up a real amuled.

struct StatusSnapshot;
struct FileSnapshot;
struct ClientSnapshot;
struct ServerSnapshot;
struct KadSnapshot;
struct CategorySnapshot;
struct PreferencesSnapshot;
class FileMap;

void ParseStatusFromPacket(const CECPacket *resp, StatusSnapshot &out);
// Kad detail rides the same STAT_REQ response — saves a roundtrip
// since amuled bundles `EC_TAG_STATS_KAD_*` into the standard CMD-
// level stats packet. /status calls ParseStatus then /kad calls
// this against the same packet pointer.
void ParseKadFromPacket(const CECPacket *resp, KadSnapshot &out);

// Drain new amule-log lines from the STAT_REQ response. amule's EC
// server piggybacks them inside an `EC_TAG_STATS_LOGGER_MESSAGE`
// parent tag with child `EC_TAG_STRING` tags, but ONLY when the
// STAT_REQ was issued at `EC_DETAIL_FULL` (or INC_UPDATE). The
// refresher calls this on the same packet as ParseStatus / ParseKad,
// then `state.AppendAmuleLog(...)` to fold them into the cache.
void ParseAmuleLogFromPacket(const CECPacket *resp, std::vector<std::string> &out_new_lines);

// `EC_OP_GET_PREFERENCES` response → flat prefs + bundled categories
// (the EC packet carries categories under `EC_TAG_PREFS_CATEGORIES`).
// One roundtrip populates both /preferences and /categories.
void ParsePreferencesFromPacket(
	const CECPacket *resp, PreferencesSnapshot &out_prefs, std::vector<CategorySnapshot> &out_cats);

// `EC_OP_GET_UPDATE` at `EC_DETAIL_INC_UPDATE` is the consolidated
// fetch backing downloads + shared + servers in a single roundtrip.
// Response shape (ExternalConn.cpp:869):
//  * top-level interleaved `EC_TAG_PARTFILE` (downloads) and
//    `EC_TAG_KNOWNFILE` (shared) — full identity on first encounter,
//    stat-only deltas on subsequent ticks via server-side valuemap.
//  * top-level `EC_TAG_FILE_REMOVED` markers for both caches (the
//    encoder map is unified server-side).
//  * `EC_TAG_SERVER` container — full list every tick, valuemap-
//    suppressed unchanged per-server fields.
//  * `EC_TAG_CLIENT` container — IGNORED in favour of
//    `EC_OP_GET_ULOAD_QUEUE` so /uploads stays bound to the upload-
//    queue semantic (the GET_UPDATE clients block is filtered by
//    the global `TransmitOnlyUploadingClients` pref which would
//    pollute amuleweb/amulegui's view).
//  * `EC_TAG_FRIEND` container — IGNORED.
//
// Why INC_UPDATE instead of per-substruct UPDATE: the per-substruct
// paths at `EC_DETAIL_UPDATE` strip identity (ECSpecialCoreTags.cpp:
// 244-246's early-return), forcing the second FULL-detail roundtrip
// the old refresher used. INC_UPDATE doesn't hit that early-return —
// identity arrives in one shot, no follow-up needed, no #713 / #808
// defences (those wire-level races only exist at EC_DETAIL_UPDATE).
//
// The three helpers below each iterate the same response once,
// filtering for their tag type. Called under three distinct CState
// mutator acquisitions; snapshot_at is set after the whole tick
// succeeds, so cross-substruct consistency is best-effort.

// Merges download-walker state (EC_TAG_PARTFILE children) into the
// unified file map. Sets `is_downloading=true` on touched entries,
// writes only the download sub-block. FILE_REMOVED clears the
// download role (and drops the entry entirely if `is_shared` was
// also false). See FileSnapshot in State.h for the unified-map
// rationale.
void ApplyGetUpdateToDownloads(
	const CECPacket *resp, FileMap &cache, std::map<std::uint32_t, PartFileEncoderData> &rle_state);

// Merges shared-walker state (EC_TAG_KNOWNFILE / EC_TAG_PARTFILE with
// SHARED flag) into the same unified map. Sets `is_shared=true`,
// updates the shared sub-block, and clears the shared role on
// PARTFILE_SHARED=false / FILE_REMOVED. The dl_identity_fallback
// parameter is gone: when the shared walker sees a partfile whose
// hash tag was CValueMap-suppressed, the entry already carries hash
// + name from the downloads walker on the same tick (same unified
// map), so the shared walker just flips its flag and merges its own
// fields. No fallback hop needed.
void ApplyGetUpdateToShared(const CECPacket *resp, FileMap &cache);

void ApplyGetUpdateToServers(const CECPacket *resp, std::map<std::uint32_t, ServerSnapshot> &cache);

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
// applying.
struct SearchResult;
void ApplySearchFull(const CECPacket *resp, std::map<std::uint32_t, SearchResult> &cache);

// Search-progress derivation from the EC_TAG_SEARCH_LIFECYCLE_* tags.
// `lifecycle_state` is the uint8 enum value (0=idle, 1=running,
// 2=finished). `pct_now` is the EC_TAG_SEARCH_LIFECYCLE_PERCENT value —
// the daemon's unified 0..100 for every search kind (global = real,
// Kad = cosmetic ramp, finished = 100), passed straight through (no
// per-kind masking). Pure function: no I/O, no globals — RefresherTest
// exercises every branch without standing up a daemon.
struct SearchProgressSnapshot;
SearchProgressSnapshot AdvanceSearchProgress(
	const SearchProgressSnapshot &prev, std::uint32_t lifecycle_state, std::uint32_t pct_now);

// `ApplyGetUpdateToClients` consumes the EC_TAG_CLIENT container
// from the consolidated GET_UPDATE response. The walker uses
// "seen-this-tick = keep, absent = evict" semantics: every alive
// client surfaces every tick via the outer per-client tag (CValueMap
// suppression operates on the tag's *children*, not on the entity
// itself), so cache entries not seen in this response are gone on
// amuled's side (peer disconnected, dropped from queue, banned).
// `file_hash_by_ecid` lets the walker resolve EC_TAG_CLIENT_UPLOAD_FILE
// / EC_TAG_CLIENT_REQUEST_FILE (raw amuled ECIDs) into MD4 hashes
// at walker time, so ClientSnapshot can surface the hash directly.
// Build it from the unified file map AFTER the downloads/shared
// walkers have run on the same tick. Empty map = correlator hashes
// stay empty (matches "not currently transferring" semantics).
void ApplyGetUpdateToClients(const CECPacket *resp,
	std::map<std::uint32_t, ClientSnapshot> &cache,
	const std::map<std::uint32_t, std::string> &file_hash_by_ecid);

} // namespace webapi

#endif // WEBAPI_REFRESHER_H
