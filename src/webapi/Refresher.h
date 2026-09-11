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
#include <ctime> // std::time_t -- needed for AdvanceSearchProgress
#include <map>
#include <string>
#include <vector>

class CECPacket;
class CEC_SearchFile_Tag;
class CamuleapiApp;
class PartFileEncoderData;

namespace webapi
{

class CState;

// Single tick of the EC poller. Issues every cached request, parses each response into a snapshot
// struct, writes it under CState's exclusive lock. Returns true on success, false if any EC
// roundtrip failed (the caller flips CState::MarkTickFailure and leaves stale data in place).
//
// Runs on the wxApp thread (the same thread CRemoteConnect uses for its socket I/O), so it can
// issue the EC roundtrip synchronously without thread-marshalling. Mutation handlers on the HTTP
// threads reach EC through a process-wide mutex around `CamuleapiApp::SendRecvMsg_v2`, so refresher
// and mutations share the same EC-traffic budget.
//
// Pure-function shape (app + state by reference, returns bool) so the tick body is unit-testable
// against a mock EC reply.
bool RefresherTick(CamuleapiApp &app, CState &state);

// Outcome of one EC_OP_SEARCH_RESULTS fetch for a single search.
enum class SearchFetchOutcome
{
	Updated,  //!< results merged into the slot
	Expired,  //!< the daemon no longer holds this search (EC_TAG_SEARCH_EXPIRED)
	EcFailed, //!< the roundtrip failed; the slot is untouched
};

// Pull one search's full result set from the daemon and merge it into that search's slot. Shared by
// the per-tick poll of ACTIVE searches and by the on-demand refresh the read paths use for a
// FINISHED one, so both issue the identical request and feed the identical applier.
SearchFetchOutcome FetchSearchResults(CamuleapiApp &app, CState &state);

// Re-fetch one search at EC_DETAIL_FULL. The union has no resync opcode, so this is how a newly
// discovered slot is filled and how a request handler refreshes without becoming a second issuer of
// the stateful stream. See the definition.
SearchFetchOutcome FetchOneSearchFull(
	CamuleapiApp &app, CState &state, std::uint32_t search_id, bool replace = false);

// Single-threaded SSE diff emission. Called ONLY from the wxApp refresher loop after a successful
// RefresherTick, so the LastSeenState walk is single-writer. Inline-from-HTTP RefresherTick call
// sites deliberately skip it -- SSE subscribers see the post-mutation diff on the next tick.
void EmitDiffsForEventBus(CamuleapiApp &app, const CState &state);

// Bring the diff baseline up to the current state without publishing. Used on the first tick after
// diffs were skipped, which would otherwise emit one event per record; whoever subscribed during
// the gap gets `resync` instead.
void PrimeDiffBaseline(CamuleapiApp &app, const CState &state);

// Sub-tick helpers exposed for testing. The Refresher uses these internally; the unit test calls
// them against hand-crafted CECPacket fixtures to pin the EC-tag-to-State mapping without standing
// up a real amuled.

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
// Kad detail rides the same STAT_REQ response -- amuled bundles EC_TAG_STATS_KAD_* into the
// standard CMD-level stats packet, so /status calls ParseStatus and /kad calls this against the
// same packet pointer.
void ParseKadFromPacket(const CECPacket *resp, KadSnapshot &out);

// Drain new amule-log lines from the STAT_REQ response. amule's EC server piggybacks them inside an
// EC_TAG_STATS_LOGGER_MESSAGE parent tag with child EC_TAG_STRING tags, but ONLY when the STAT_REQ
// was issued at EC_DETAIL_FULL (or INC_UPDATE).
void ParseAmuleLogFromPacket(const CECPacket *resp, std::vector<std::string> &out_new_lines);

// Union EC_OP_SEARCH_PROGRESS response -> {search id: {percent, lifecycle state}} for every search
// the daemon reported.
//
// Returns false unless the reply really is an EC_OP_SEARCH_PROGRESS, and the caller MUST honour
// that: absence from a union reply is how a search is learnt to have expired, so treating an
// unparseable reply as an empty union would retire every search the caller is tracking in one pass.
// An EC_OP_FAILED must therefore fall back to per-id polling, not be read as "all gone".
//
// An *empty* union with the right opcode is NOT malformed -- it is the daemon legitimately saying
// it holds none of the searches asked about.
bool ParseSearchProgressUnion(
	const CECPacket *resp, std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> &out);

// EC_OP_GET_PREFERENCES response -> flat prefs + bundled categories (the packet carries categories
// under EC_TAG_PREFS_CATEGORIES), so one roundtrip populates both /preferences and /categories.
void ParsePreferencesFromPacket(
	const CECPacket *resp, PreferencesSnapshot &out_prefs, std::vector<CategorySnapshot> &out_cats);

// Locale-independent file-type token from a filename: the desktop's own category label
// (GetFiletypeByName, untranslated) lowercased -- "audio", "video", "cd-images", "any". Reuses the
// GUI categorization rather than duplicating the extension table. One owner, because this lived
// twice character for character and the two keys could drift apart silently.
std::string FileTypeToken(const std::string &name);

// EC_OP_GET_UPDATE at EC_DETAIL_INC_UPDATE is the consolidated fetch backing downloads + shared +
// servers in a single roundtrip. Response shape:
//  * top-level interleaved EC_TAG_PARTFILE (downloads) and EC_TAG_KNOWNFILE (shared) -- full
//    identity on first encounter, stat-only deltas on later ticks via the server-side valuemap.
//  * top-level EC_TAG_FILE_REMOVED markers for both caches (the encoder map is unified
//    server-side).
//  * EC_TAG_SERVER container -- full list every tick, valuemap-suppressed unchanged fields.
//  * EC_TAG_CLIENT container -- consumed into /clients. Filtered server-side by the global
//    TransmitOnlyUploadingClients pref; /uploads stays bound to the upload-queue semantic via
//    EC_OP_GET_ULOAD_QUEUE.
//  * EC_TAG_FRIEND container -- consumed into /friends.
//
// Why INC_UPDATE instead of per-substruct UPDATE: the per-substruct paths at EC_DETAIL_UPDATE
// strip identity via an early return in ECSpecialCoreTags.cpp, forcing a second FULL-detail
// roundtrip. INC_UPDATE does not hit that, so identity arrives in one shot.

// Merges download-walker state (EC_TAG_PARTFILE children) into the unified file map. Sets
// `is_downloading=true` on touched entries and writes only the download sub-block. FILE_REMOVED
// clears the download role, and drops the entry entirely if `is_shared` was also false.
void ApplyGetUpdateToDownloads(
	const CECPacket *resp, FileMap &cache, std::map<std::uint32_t, PartFileEncoderData> &rle_state);

// Merges shared-walker state (EC_TAG_KNOWNFILE / EC_TAG_PARTFILE with the SHARED flag) into the
// same unified map. Sets `is_shared=true`, updates the shared sub-block, and clears the shared role
// on PARTFILE_SHARED=false / FILE_REMOVED.
//
// `rle_state` is the same per-ECID decoder map the downloads walker uses, and deliberately so:
// amuled keeps exactly one encoder per ECID (CFileEncoderMap::UpdateEncoders) and emits it as
// EC_TAG_PARTFILE or EC_TAG_KNOWNFILE, never both, so a single decoder map mirrors the daemon's
// encoder set 1:1. The downloads walker already evicts on EC_TAG_FILE_REMOVED, which is the only
// way an ECID's encoder is torn down and rebuilt.
void ApplyGetUpdateToShared(
	const CECPacket *resp, FileMap &cache, std::map<std::uint32_t, PartFileEncoderData> &rle_state);

void ApplyGetUpdateToServers(const CECPacket *resp, std::map<std::uint32_t, ServerSnapshot> &cache);

// Consumes the EC_TAG_FRIEND container the daemon appends to every
// EC_OP_GET_UPDATE reply, so /friends is served from the tick we already run.
void ApplyGetUpdateToFriends(const CECPacket *resp, std::map<std::uint32_t, FriendSnapshot> &cache);

// EC_OP_CHAT_SESSIONS -> the /chats snapshot. `cursor` is read as the value sent with the request
// and written back with the store's current last id.
//
// The reply is the daemon's COMPLETE session set, so this replaces the vector rather than merging:
// a session missing from a reply was closed, and that absence is the only signal a close produces.
// Messages arrive incrementally.
void ApplyChatSessions(const CECPacket *resp,
	std::vector<ChatSessionSnapshot> &cache,
	std::uint32_t &cursor,
	std::vector<ChatSessionSnapshot> &out_new_messages,
	std::vector<std::uint64_t> &out_closed);

// ed2k server priority, both directions. The SRV_PR_* wire values are not monotone (NORMAL=0,
// HIGH=1, LOW=2), so callers must never assume a name's position in a list is its code.
const char *ServerPriorityName(std::uint32_t prio_code);
bool ServerPriorityCode(const std::string &name, std::uint32_t &out_code);

// /stats/tree (EC_OP_GET_STATSTREE response). Recursive walk -- every EC_TAG_STATTREE_NODE with
// children becomes a branch. The top-level root is an unnamed container, skipped so its direct
// children are the visible tree.
struct StatsTreeNode;
void ParseStatsTreeFromPacket(const CECPacket *resp, StatsTreeNode &out);

// /stats/graphs/{graph} (EC_OP_GET_STATSGRAPHS response). amuled packs the four time-series into
// byte blobs (two interleaved channels in EC_TAG_STATSGRAPH_DATA plus a separate
// EC_TAG_STATSGRAPH_DATA_CONN).
struct StatsGraphs;
void ParseGraphsFromPacket(const CECPacket *resp, StatsGraphs &out);

// /search/results (EC_OP_SEARCH_RESULTS response). Full-state fetch per tick;
// like /servers, no INC path exists for the search list. Keyed by ECID.
struct SearchResult;
struct SearchSlot;
// Merge one EC_TAG_SEARCHFILE onto a result, writing only the fields the tag carries. Exposed for
// the unit tests, which pin the absent-means-unchanged contract field by field.
void MergeSearchResultTag(const CEC_SearchFile_Tag *sf, SearchResult &r);

// Rebuild the folded view from the flat merge target: children nested into
// their parents, orphans promoted.
void RebuildFoldedResults(
	const std::map<std::uint32_t, SearchResult> &raw, std::map<std::uint32_t, SearchResult> &out);

// Apply one incremental multi-search union reply across every slot, keeping the ECID -> search_id
// index in step. See the definition for why absence cannot mean deletion here. `default_sid`
// attributes tags that carry no EC_TAG_SEARCH_ID of their own: zero for the union reply, where a
// missing id means "already known"; set for a per-search reply, which never stamps one.
void ApplySearchUnion(const CECPacket *resp,
	std::map<std::uint32_t, SearchSlot> &slots,
	std::map<std::uint32_t, std::uint32_t> &owner,
	std::uint32_t default_sid = 0);

// Apply one per-search EC_DETAIL_FULL reply to a single slot.
//
// `replace` swaps the slot's results for what the daemon reports now instead of merging onto them,
// which is what a re-seed after a lost union reply needs: a FULL reply carries no tombstones, so a
// merge cannot express a row the daemon has dropped. It is also the only mode that discharges
// SearchSlot::needs_resync -- clearing it after a merge would strand them.
void ApplySearchFullReply(const CECPacket *resp,
	std::map<std::uint32_t, SearchSlot> &slots,
	std::map<std::uint32_t, std::uint32_t> &owner,
	std::uint32_t search_id,
	bool replace);

// Search-progress derivation from the EC_TAG_SEARCH_LIFECYCLE_* tags. `lifecycle_state` is the
// uint8 enum value (0=idle, 1=running, 2=finished). `pct_now` is the daemon's unified 0..100 for
// every search kind, passed straight through with no per-kind masking. Pure function: no I/O, no
// globals.
struct SearchProgressSnapshot;
SearchProgressSnapshot AdvanceSearchProgress(
	const SearchProgressSnapshot &prev, std::uint32_t lifecycle_state, std::uint32_t pct_now);

// `ApplyGetUpdateToClients` consumes the EC_TAG_CLIENT container from the consolidated GET_UPDATE
// response, with "seen-this-tick = keep, absent = evict" semantics: every alive client surfaces
// every tick via the outer per-client tag, since CValueMap suppression operates on the tag's
// *children*.
//
// `files` lets the walker resolve EC_TAG_CLIENT_UPLOAD_FILE / EC_TAG_CLIENT_REQUEST_FILE (raw
// amuled ECIDs) into MD4 hashes at walker time. It is the live map, so pass it via
// CState::MutateClientsWithFiles AFTER the downloads/shared walkers have run on the same tick.
void ApplyGetUpdateToClients(
	const CECPacket *resp, std::map<std::uint32_t, ClientSnapshot> &cache, const FileMap &files);

} // namespace webapi

#endif // WEBAPI_REFRESHER_H
