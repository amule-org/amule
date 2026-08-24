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

// Outcome of one EC_OP_SEARCH_RESULTS fetch for a single search.
enum class SearchFetchOutcome
{
	Updated,  //!< results merged into the slot
	Expired,  //!< the daemon no longer holds this search (EC_TAG_SEARCH_EXPIRED)
	EcFailed, //!< the roundtrip failed; the slot is untouched
};

// Pull one search's full result set from the daemon and merge it into that
// search's slot. Shared by the per-tick poll of ACTIVE searches and by the
// on-demand refresh the read paths use for a FINISHED one, so both issue the
// identical request (EC_DETAIL_FULL plus the EC_TAG_SEARCH_PARENT grouping
// flag) and feed the identical applier — a results list read through either
// route has the same shape.
SearchFetchOutcome FetchSearchResults(CamuleapiApp &app, CState &state, std::uint32_t search_id);

// Single-threaded SSE diff emission. Called ONLY from the wxApp
// refresher loop after a successful RefresherTick so that the
// LastSeenState walk (which mutates app.LastSeenForEvents()) is
// single-writer. Inline-from-HTTP RefresherTick call sites
// deliberately skip it — SSE subscribers see the post-mutation
// diff on the next 1-second tick instead of immediately.
void EmitDiffsForEventBus(CamuleapiApp &app, const CState &state);

// Bring the diff baseline up to the current state without publishing. Used on
// the first tick after diffs were skipped, which would otherwise emit one event
// per record; whoever subscribed during the gap gets `resync` instead.
void PrimeDiffBaseline(CamuleapiApp &app, const CState &state);

// Sub-tick helpers exposed for testing. The Refresher uses these
// internally; the unit test calls them against hand-crafted
// CECPacket fixtures to pin the EC-tag-to-State mapping without
// standing up a real amuled.

struct StatusSnapshot;
struct FileSnapshot;
struct ClientSnapshot;
struct FriendSnapshot;
struct ChatSessionSnapshot;
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

// Union `EC_OP_SEARCH_PROGRESS` response → {search id: {percent, lifecycle
// state}} for every search the daemon reported.
//
// Returns false unless the reply really is an `EC_OP_SEARCH_PROGRESS`, and the
// caller MUST honour that: absence from a union reply is how a search is
// learnt to have expired, so treating a reply this could not parse as an empty
// union would retire every search the caller is tracking in one pass --
// active=false, terminal snapshot, a final search_progress SSE frame to every
// subscriber. The per-id form this replaces cannot do that: it expires only on
// an explicit `EC_TAG_SEARCH_EXPIRED`, and an unexpected reply just leaves the
// values alone. An `EC_OP_FAILED` (the daemon has ~30 paths that emit one)
// must therefore fall back to per-id polling, not be read as "all gone".
//
// An *empty* union with the right opcode is NOT malformed -- it is the daemon
// legitimately saying it holds none of the searches asked about -- so the
// opcode, not the child count, is the discriminator.
bool ParseSearchProgressUnion(
	const CECPacket *resp, std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> &out);

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
// `rle_state` is the same per-ECID decoder map the downloads walker
// uses, and deliberately so: amuled keeps exactly one encoder per ECID
// (CFileEncoderMap::UpdateEncoders) and emits it as EC_TAG_PARTFILE or
// EC_TAG_KNOWNFILE, never both, so a single decoder map mirrors the
// daemon's encoder set 1:1. This walker feeds only the KNOWNFILE half;
// the downloads walker feeds the PARTFILE half and already evicts on
// EC_TAG_FILE_REMOVED, which is the only way an ECID's encoder is torn
// down and rebuilt (a completing download keeps its CPartFile_Encoder
// until CDownloadQueue::ClearCompleted renews its ECID outright).
void ApplyGetUpdateToShared(
	const CECPacket *resp, FileMap &cache, std::map<std::uint32_t, PartFileEncoderData> &rle_state);

void ApplyGetUpdateToServers(const CECPacket *resp, std::map<std::uint32_t, ServerSnapshot> &cache);

// Consumes the EC_TAG_FRIEND container the daemon appends to every
// EC_OP_GET_UPDATE reply (ExternalConn.cpp, "Add friends"), so /friends is
// served from the tick we already run rather than a roundtrip of its own.
void ApplyGetUpdateToFriends(const CECPacket *resp, std::map<std::uint32_t, FriendSnapshot> &cache);

// EC_OP_CHAT_SESSIONS -> the /chats snapshot. `cursor` is read as the value
// sent with the request and written back with the store's current last id.
//
// The reply is the daemon's COMPLETE session set, so this replaces the vector
// rather than merging into it: a session missing from a reply was closed (by
// another client, or evicted), and that absence is the only signal a close
// produces. Messages, by contrast, arrive incrementally -- only those newer
// than the cursor -- so they are appended to the session they belong to,
// carrying over what the previous tick already held.
void ApplyChatSessions(const CECPacket *resp,
	std::vector<ChatSessionSnapshot> &cache,
	std::uint32_t &cursor,
	std::vector<ChatSessionSnapshot> &out_new_messages,
	std::vector<std::uint64_t> &out_closed);

// ed2k server priority, both directions. The SRV_PR_* wire values are not
// monotone (NORMAL=0, HIGH=1, LOW=2), so callers must never assume a name's
// position in a list is its code. ServerPriorityCode returns false for an
// unknown name, leaving out_code untouched.
const char *ServerPriorityName(std::uint32_t prio_code);
bool ServerPriorityCode(const std::string &name, std::uint32_t &out_code);

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
// `files` lets the walker resolve EC_TAG_CLIENT_UPLOAD_FILE /
// EC_TAG_CLIENT_REQUEST_FILE (raw amuled ECIDs) into MD4 hashes at
// walker time, so ClientSnapshot can surface the hash directly. It is
// the live map, keyed by the same ECIDs, so pass it via
// CState::MutateClientsWithFiles AFTER the downloads/shared walkers
// have run on the same tick. Empty map = correlator hashes stay empty
// (matches "not currently transferring" semantics).
void ApplyGetUpdateToClients(
	const CECPacket *resp, std::map<std::uint32_t, ClientSnapshot> &cache, const FileMap &files);

} // namespace webapi

#endif // WEBAPI_REFRESHER_H
