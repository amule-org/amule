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

#ifndef WEBAPI_STATE_H
#define WEBAPI_STATE_H

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// kNoPartPendingSentinel: the same shape of sentinel as
// kRemoteQueueFullSentinel below, for the two part indices on ClientSnapshot.
// It lives in that header rather than here because the predicates that test it
// are unit-tested standalone, and a constant defined twice is a constant that
// drifts.
#include "PartIndex.h"

// Cached snapshot of amuled state. One instance lives inside
// CamuleapiApp for the whole process; the refresher (wxApp thread)
// writes; the HTTP server (Boost.Asio thread) reads.
//
// **Concurrency model.** A single `std::shared_timed_mutex` guards
// every member field. The refresher takes it exclusive once per
// tick to swap each substruct (the swap is a `std::move`, never
// the EC roundtrip itself). HTTP read handlers take it shared,
// copy the relevant substruct, release, then serialise JSON
// outside the critical section — multiple clients stack with no
// per-handler bottleneck.

namespace webapi
{

// One per file in amuled's state — keyed by ECID. Each file may
// participate in either or both of two roles:
//
//   * `is_downloading` — the file is a partfile in `downloadqueue`
//     (still acquiring chunks). Drives `/downloads`. The walker that
//     populates this side consumes `EC_TAG_PARTFILE_*` children.
//   * `is_shared`      — the file is uploadable: a fully-completed
//     knownfile, OR a partfile with ≥1 chunk done (amuled flags via
//     `EC_TAG_PARTFILE_SHARED=true`). Drives `/shared`. Populated
//     from `EC_TAG_KNOWNFILE_*` children.
//
// Both flags can be true simultaneously for a partfile that's
// currently downloading AND uploading completed chunks.
//
// The unified-keyed-by-ECID design mirrors amulegui's
// `CKnownFilesRem::m_items_hash` (amule-remote-gui.cpp:1507). It
// avoids the "shared cache has a ghost row with empty hash" bug
// (see #201 review): on a partfile-becoming-shared tick the server's
// CValueMap suppresses `EC_TAG_PARTFILE_HASH` because it was sent on
// a prior partfile-walker tick, but the unified entry already has
// hash + name from the downloads walker, so the shared walker just
// flips `is_shared=true` and merges its own fields. No fallback.
//
// Role-specific state lives in sub-blocks. When a role transitions
// true→false (partfile completes → ECID dies; or shared partfile
// loses every chunk → `is_shared` flips off; or knownfile is un-
// shared), the refresher resets that side's sub-block to default so
// `/downloads` or `/shared` can never serve stale stats from a
// previous active period.
struct FileSnapshot
{
	// Identity / shared metadata (always populated).
	std::uint32_t ecid = 0;
	std::string hash; // 32-char hex MD4
	std::string name;
	std::string ed2k_link;
	std::uint64_t size = 0;

	bool is_downloading = false;
	bool is_shared = false;

	// File-level attributes carried by the base CKnownFile EC tags, so
	// they're available on both the download and shared detail endpoints.
	// Detail-only (the list endpoints don't emit them).
	std::string aich_hash;          // AICH master hash (hex); "" if none
	std::uint32_t queued_count = 0; // clients on this file's upload queue
	std::string comment;            // the user's own file comment
	std::int32_t rating = 0;        // the user's own rating, 0-5 (0 = unrated)

	// Audio/video media metadata (issue #418). amuled emits each field only
	// when that field has a value -- deliberately NOT gated on the aggregate
	// GetMetaDataVer(), which would send length 0 / bitrate 0 for a file that
	// probed to a codec and no duration, and a displayed zero is a claim where
	// absence is not. A zero / empty value is amuled saying the field is GONE,
	// which is how a re-probe's clear reaches us at all.
	//
	// `has_media` is therefore DERIVED from the fields below rather than being
	// something amuled sent: latching it on any tag arriving would report
	// media on a file whose every field has since been cleared. It gates the
	// `media` object on the detail endpoints -- omitted entirely when false.
	bool has_media = false;
	struct Media
	{
		std::uint32_t duration_seconds = 0; // duration, seconds
		std::uint32_t bitrate_kilobits_per_second = 0;
		std::string codec;
		std::string artist;
		std::string album;
		std::string title;
	} media;

	// The partfile's control-file basename (e.g. `001.part`), from
	// EC_TAG_KNOWNFILE_FILENAME. Meaningful only while the file is still
	// an incomplete partfile — once it completes, the daemon reuses that
	// EC tag to carry the directory path, so `part_file_name` on /downloads is
	// gated on the download status (empty once completed). See #417.
	std::string part_met_basename;

	// The file's on-disk directory, from EC_TAG_KNOWNFILE_PATH — the Temp
	// dir while downloading, the destination dir once completed. Always a
	// directory (never the `.part` basename), so it feeds an unambiguous
	// `path` on both /downloads/{hash} and /shared/{hash} (#417).
	std::string on_disk_dir;

	// Download-side state — meaningful when `is_downloading` is true,
	// reset to default on the true→false transition (and never read
	// by `/downloads` when the flag is false).
	struct DownloadSide
	{
		std::uint64_t completed_bytes = 0;
		std::uint64_t transferred_bytes = 0;
		std::uint32_t speed_bytes_per_second = 0;
		std::string status; // "downloading" | "paused"
				    // | "completed" | "hashing" | ...
		// Download priority: "very_low" | "low" | "normal" | "high"
		// | "release" | "auto".
		std::string priority;
		bool priority_auto = false;
		std::uint32_t category = 0;
		double percent = 0.0;
		std::uint32_t sources_total = 0;
		std::uint32_t sources_unavailable = 0;
		std::uint32_t sources_transferring = 0;
		std::uint32_t sources_a4af = 0;

		// Detail-only fields (GET /downloads/{hash}); the list endpoint
		// omits them. All decoded from tags CEC_PartFile_Tag already
		// emits under INC_UPDATE.
		std::uint32_t last_seen_complete_at = 0;       // unix ts; 0 = unknown
		std::uint32_t last_received_at = 0;            // unix ts of last change
		std::uint32_t active_seconds = 0;              // seconds downloading
		std::uint16_t available_part_count = 0;        // parts across sources
		std::uint16_t hashed_part_count = 0;           // parts hashed so far; 0 = idle
		std::uint64_t lost_to_corruption_bytes = 0;    // bytes
		std::uint64_t gained_by_compression_bytes = 0; // bytes
		std::uint32_t ich_recovered_packet_count = 0;  // packets recovered by ICH

		// Per-source comments/ratings (GET /downloads/{hash}/comments,
		// issue #419). Downloads-only — needs a live source list. A
		// `rating` of -1 means the source left a comment but no rating.
		struct SourceComment
		{
			std::string username;
			std::string filename;
			std::int32_t rating = 0;
			std::string comment;
		};
		std::vector<SourceComment> source_comments;

		// True while an on-demand Kad notes lookup is in flight on amuled for
		// this file (POST /downloads/{hash}/comments starts one; issue #434).
		// Lets clients poll GET .../comments until the search finishes.
		bool kad_comment_searching = false;

		// Source-reported filenames (GET /downloads/{hash}/filenames,
		// issue #420). amuled delta-encodes these keyed by a stable id
		// (new = name+count, count 0 = removed, else count update); the
		// refresher accumulates into this map across ticks. Reset with
		// the rest of DownloadSide when the download role drops.
		struct SourceName
		{
			std::string name;
			std::uint32_t count = 0;
		};
		std::map<std::uint32_t, SourceName> source_names;

		// A4AF (asked-for-another-file) source scheduling (issue #421).
		// `a4af_auto` is the auto-swap flag; `a4af_sources` are the client
		// ECIDs currently parked as A4AF sources for this download (full
		// list re-sent by amuled when it changes).
		bool a4af_auto = false;
		std::vector<std::uint32_t> a4af_sources;

		// Decoded per-part state, populated by the refresher's RLE
		// decoder pass on EC_TAG_PARTFILE_GAP_STATUS +
		// EC_TAG_PARTFILE_PART_STATUS. Both arrays are sized to
		// ceil(size / PARTSIZE) once a successful decode has landed;
		// the list endpoint omits them, the detail endpoint emits
		// `progress.parts: [{state, sources}, ...]` by walking them
		// in parallel.
		std::vector<std::uint64_t> decoded_gaps;
		std::vector<std::uint16_t> decoded_part_sources;
	} download;

	// Shared-side state — meaningful when `is_shared` is true,
	// reset on the true→false transition.
	struct SharedSide
	{
		// Upload priority level, distinct from the download-side value —
		// a partfile that is both downloading and shared carries two
		// independent priorities, so each side stores its own.
		std::string priority; // upload priority: "very_low" | "low"
				      // | "normal" | "high" | "release" | "auto"
		// Upload-side auto-priority flag, mirroring `download.priority_auto`;
		// says whether amuled is deriving it automatically from the queue.
		bool priority_auto = false;
		std::uint64_t uploaded_bytes_session = 0;
		std::uint64_t uploaded_bytes_total = 0;
		std::uint32_t request_count_session = 0;
		std::uint32_t request_count_total = 0;
		std::uint32_t accepted_request_count_session = 0;
		std::uint32_t accepted_request_count_total = 0;
		std::uint32_t complete_sources = 0;

		// Detail-only (GET /shared/{hash}). The complete-sources range
		// backs the desktop `< N` / `N - M` display; the scalar
		// `complete_sources` above stays as-is.
		std::uint16_t complete_sources_low = 0;
		std::uint16_t complete_sources_high = 0;

		// Per-part source availability backing the shared "Obtained
		// Parts" bar, decoded by the refresher from
		// EC_TAG_PARTFILE_PART_STATUS on the EC_TAG_KNOWNFILE tag.
		// Sized to ceil(size / PARTSIZE) once a decode has landed and
		// empty until then, which the detail endpoint reports by
		// omitting `parts` entirely -- "no data yet" and "no sources
		// for any part" stay distinguishable.
		//
		// A shared *partfile* never populates this: amuled emits it as
		// EC_TAG_PARTFILE only (one encoder per ECID), so its vector
		// lands in `download.decoded_part_sources` instead. The detail
		// writer falls back to that one.
		//
		// Values are the daemon's raw per-part source counts, clamped
		// to 255 by the RLE encoder's uint8 buffer.
		std::vector<std::uint16_t> decoded_part_sources;

		// Parts hashed so far by a pass running over this complete share --
		// Verify Local Data, or an AICH hashset rebuild -- decoded from
		// EC_TAG_KNOWNFILE_HASHED_PART_COUNT on the EC_TAG_KNOWNFILE tag.
		// A count, not an index: the tasks report part + 1. 0 means idle,
		// which both tasks restore when they finish or abort.
		//
		// A download that is also shared arrives as EC_TAG_PARTFILE, so its
		// progress lands in download.hashed_part_count and this stays 0 --
		// read both through SharedHashingProgress() rather than this field
		// directly, the same fallback decoded_part_sources needs.
		std::uint16_t hashing_progress = 0;

		// Live upload activity (issue #466), the upload-side analogue of
		// the download stats. `upload_speed_bytes_per_second` + `uploading_client_count` are
		// live (refresh every tick); `last_upload` / `shared_since` are
		// unix timestamps, 0 = unknown (a file that has never uploaded, or
		// a known.met entry that predates the feature).
		std::uint32_t upload_speed_bytes_per_second = 0;
		std::uint16_t uploading_client_count = 0;
		std::uint32_t last_upload = 0;
		std::uint32_t shared_since = 0;
	} shared;

	// True while the file is genuinely an incomplete partfile: still in the
	// download queue, and not the "finished but not yet cleared" state.
	//
	// The distinction matters because a completed download keeps
	// `is_downloading` set until the user clears it, while already being a
	// knownfile whose data sits in its destination directory -- so
	// `is_downloading` alone would misreport it. "completing" is incomplete
	// on purpose: the data is still in the temp directory until the move
	// finishes.
	bool IsIncompletePartfile() const { return is_downloading && download.status != "completed"; }
};

// One per peer (CUpDownClient) in the daemon's active client list.
// Populated from the EC_TAG_CLIENT subtree inside the GET_UPDATE
// response.
//
// "Client" here is amule's bidirectional peer: a remote ed2k peer
// that's connected to us in EITHER role — uploader (we are downloading
// from them), uploadee (we are uploading to them), queue waiter,
// banned, etc. The cache holds ALL of them; consumer endpoints filter
// by role:
//  * /uploads → filter by upload_state == US_UPLOADING
//  * /clients → no filter, full set surfaced
//  * /downloads/{hash}/clients and /shared/{hash}/clients → the peers
//    related to one file, selected by download_file_hash /
//    upload_file_hash (plus that download's A4AF sources), each row
//    carrying the direction as a `role` field.
/**
 * One peer from the daemon's credit store — GET /known_clients.
 *
 * Distinct from ClientSnapshot, which describes a peer we are connected to
 * *now*: this is keyed by user hash rather than ECID, survives the daemon
 * process that issued any ECID, and carries stored history rather than live
 * transfer state. A peer can appear in both, correlated by user_hash.
 *
 * Every field except the hash and the totals is optional. A record written
 * before the daemon kept per-peer metadata has no name, address or software,
 * and the daemon simply omits those tags rather than inventing values.
 */
struct KnownClientSnapshot
{
	std::string user_hash; // 32-char lowercase hex MD4; the identity
	std::string client_name;
	std::string ip; // dotted-quad; "" when the record predates metadata
	std::uint16_t port = 0;
	std::uint16_t kad_port = 0;
	std::string country_code;     // ISO 3166-1 alpha-2, resolved daemon-side
	std::string software;         // resolved name, e.g. "eMule"
	std::string software_version; // "v0.50.0"
	std::string source_origin;
	std::string obfuscation_state;
	std::uint64_t uploaded_bytes_total = 0;
	std::uint64_t downloaded_bytes_total = 0;
	std::time_t first_seen_at = 0; // 0 when the record carries no metadata
	std::time_t last_seen_at = 0;
	std::uint32_t session_count = 0;
	//! This peer is connected right now, correlated by user hash against the
	//! live client list. Never by ECID: those mean nothing across daemon
	//! restarts, while the hash is what the credit store is keyed on.
	bool online = false;
};

//! amuled substitutes this for the queue position when the peer's queue is
//! full (ECSpecialCoreTags.cpp: `IsRemoteQueueFull() ? 0xffff : rank`). It is a
//! sentinel, not a position: relayed as a number it reads as "position 65535",
//! so the REST and SSE writers emit null for it instead.
constexpr std::uint16_t kRemoteQueueFullSentinel = 0xffffu;

struct ClientSnapshot
{
	std::uint32_t ecid = 0;
	std::string client_name;
	std::string user_hash; // peer's user hash (32-char lowercase hex MD4)
	std::string ip;        // dotted-quad
	std::uint16_t port = 0;
	// ISO 3166-1 alpha-2 country code (lowercase), resolved by the daemon's
	// GeoIP from the peer IP (#439). "" when GeoIP is disabled/unsupported or
	// the IP does not resolve.
	std::string country_code;

	// Software identity. EC_TAG_CLIENT_SOFTWARE ships a numeric code
	// (SO_AMULE / SO_EMULE / etc); we decode it server-side into a
	// short label here so consumers don't need the lookup table.
	std::string software;         // "amule" | "emule" | "edonkey" | "mldonkey" | ...
	std::string software_version; // free-form string from EC_TAG_CLIENT_SOFT_VER_STR
	std::string reported_os;      // free-form (CLIENT_OS_INFO)

	// State machine values. We decode the raw US_*/DS_*/IS_* ints
	// into wire strings so consumers don't reach into amule's enums.
	std::string upload_state;   // "uploading" | "queued" | "banned" | "connecting" | "idle" | ...
	std::string download_state; // "downloading" | "queued" | "no_needed_parts" | ... | "idle"
	// Complete set, see ClientIdentStateName() in Refresher.cpp:
	std::string ident_state; // "not_available" | "id_needed" | "identified" | "id_failed" | "bad_guy" |
				 // "unknown"

	// File context — different per direction. Both correlators are
	// 32-char lowercase MD4 hashes resolved by the refresher from
	// EC_TAG_CLIENT_UPLOAD_FILE / EC_TAG_CLIENT_REQUEST_FILE (which
	// amuled ships as ECIDs) against the unified m_files map. Consumers
	// correlate against /downloads[].hash or /shared[].hash.
	//  * upload_file_hash: partfile this peer is downloading FROM
	//    us. Empty when not uploading to them, or when amuled's ECID
	//    didn't resolve to a known file this tick.
	//  * download_file_hash + download_file_name: file we are
	//    downloading FROM this peer + the filename the peer
	//    advertised (OP_REQFILENAMEANSWER). Empty when not in
	//    download role.
	//  * upload_file_name: partfile this peer is downloading FROM us,
	//    resolved locally (we know our own partfiles' names, unlike
	//    the peer-advertised download_file_name above). Empty when
	//    not in upload role, or when upload_file_hash didn't resolve.
	std::string upload_file_hash;   // EC_TAG_CLIENT_UPLOAD_FILE resolved
	std::string download_file_hash; // EC_TAG_CLIENT_REQUEST_FILE resolved
	std::string download_file_name; // EC_TAG_CLIENT_REMOTE_FILENAME
	std::string upload_file_name;   // resolved from upload_file_hash against m_files

	// Per-session transfer stats. CLIENT_UPLOAD_SESSION = bytes
	// uploaded TO this peer; PARTFILE_SIZE_XFER (when re-keyed on a
	// CLIENT_* tag) = bytes downloaded FROM this peer.
	std::uint64_t uploaded_bytes_session = 0;
	std::uint64_t downloaded_bytes_session = 0;
	std::uint64_t uploaded_bytes_total = 0;
	std::uint64_t downloaded_bytes_total = 0;
	std::uint32_t upload_speed_bytes_per_second = 0;
	std::uint32_t download_speed_bytes_per_second = 0;

	// Upload queue position (for peers in US_ONUPLOADQUEUE).
	// 0 when not queued.
	std::uint32_t upload_queue_position = 0;
	// Remote queue rank — our position in THE PEER's upload queue
	// (i.e. how many other ed2k clients they're going to upload to
	// before us). 0xFFFF when their queue is full.
	//! Carries amuled's queue-full sentinel as well as a real position; see
	//! kRemoteQueueFullSentinel.
	std::uint16_t remote_queue_position = 0;

	std::uint32_t score = 0; // EC_TAG_CLIENT_SCORE
	// Complete set, see ClientObfuscationName() in Refresher.cpp:
	std::string obfuscation_state; // "undefined" | "enabled" | "supported" | "not_supported" |
				       // "disabled" | "unknown"
	bool friend_slot = false;

	// --- Extra fields decoded off the INC_UPDATE wire (issue #422) ----
	// Originally detail-only. Five of them -- source_origin,
	// parts_offered_count, client_mod_name, view_shared_disabled and the derived
	// part_progress_percent -- were since promoted to the /clients list row
	// and the SSE client_* payloads (#995, #1015); the rest are still
	// serialized only by GET /clients/{ecid}.
	std::uint32_t ed2k_user_id = 0; // EC_TAG_CLIENT_USER_ID (hybrid eD2k id)
	bool high_id = false;           // derived: !IsLowID(ed2k_user_id)
	std::string server_ip;          // dotted-quad; "" when unknown/0
	std::uint16_t server_port = 0;
	std::string server_name;
	std::uint16_t kad_port = 0; // 0 => Kad not connected for this peer
	std::string source_origin;  // "server" | "kad" | "source_exchange" | "passive" | "link" | ...
	std::uint32_t parts_offered_count = 0; // count of parts the peer has (EC_TAG_CLIENT_AVAILABLE_PARTS)
	bool has_parts_offered_count = false;  // false => tag absent, emitted as null
	std::string client_mod_name;           // EC_TAG_CLIENT_MOD_VERSION
	bool view_shared_disabled = false;     // peer forbids viewing its shared files
	// Completeness of the linked download for this peer, as a percent
	// (parts_offered_count / file part count). < 0 => not computable (no
	// linked file / no part count). Derived rather than decoded: filled in
	// by ComputePartProgressPercent at every site that serializes a client
	// -- the list, the per-file rows, the detail object and the SSE
	// payload -- since it needs a second snapshot to resolve.
	double part_progress_percent = -1.0;

	// Per-part bitmaps, one bit per chunk of the file the relation names:
	// `part_status` is what the peer holds of the file WE PULL FROM IT,
	// `upload_part_status` what it holds of the file IT PULLS FROM US. A peer
	// doing both at once has two different files' bitmaps here, so a consumer
	// must pick by direction rather than assume they describe the same file.
	//
	// The core sends an EMPTY tag to mean "has every part" rather than
	// spending bytes on an all-ones buffer, and omits the tag entirely when
	// its length would disagree with the file's part count. `*_all` carries
	// the first case; the vector is then left empty and sized by whoever
	// renders it, which is the only place the part count is known.
	std::vector<bool> part_status;
	std::vector<bool> upload_part_status;
	bool part_status_all = false;
	bool upload_part_status_all = false;
	bool has_part_status = false;
	bool has_upload_part_status = false;
	// Indices into the download bitmap; absent from the wire means unchanged,
	// so the has_ flags distinguish "never reported" from "reported as 0".
	//
	// Both are indices into the *download* bitmap of the peer's request
	// file (ECSpecialCoreTags.cpp emits them inside the `pfile` guard, next
	// to EC_TAG_CLIENT_PART_STATUS), so they address one file, not the peer
	// as a whole. Surfaced by the per-file client rows only --
	// `next_requested_part_index` / `downloading_part_index` on
	// GET /downloads/{hash}/clients under `include_parts` -- which is why
	// no comparator sorts on them and no SSE payload carries them. Values
	// are relayed raw and validated by the serializer rather than here,
	// because only the endpoint knows the file: kNoPartPendingSentinel is
	// the core's "nothing pending" answer, any index past the file's part
	// count is unusable, the row needs the bitmap the index indexes, and
	// last_downloading_part is a stale 0 (BaseClient.cpp inits it so, and
	// the tag is sent unconditionally) unless download_state is
	// "downloading" -- all of which come out as null.
	std::uint16_t next_requested_part = 0;
	std::uint16_t last_downloading_part = 0;
	bool has_next_requested_part = false;
	bool has_last_downloading_part = false;

	// --- Detail-only fields (issue #423, new EC tags) ----------------
	bool is_friend = false;    // CUpDownClient::IsFriend(); distinct from friend_slot
	double credit_ratio = 0.0; // CUpDownClient::GetScoreRatio() ("DL/UP modifier")
};

// One per eD2k server in the configured server list. Identity is
// the EC ECID (stable per amuled process lifetime). Servers are
// fetched at full-state per refresher tick (`EC_OP_GET_SERVER_LIST`
// has no two-phase INC equivalent — see `ExternalConn.cpp:2023`),
// so the refresher rebuilds the whole map each cycle.
struct ServerSnapshot
{
	std::uint32_t ecid = 0;
	std::string name;
	std::string description;
	std::string version;
	std::string address;  // host:port form (canonical)
	std::uint32_t ip = 0; // host-byte-order IPv4
	std::uint16_t port = 0;
	// ISO 3166-1 alpha-2 country code (lowercase) of the server host,
	// resolved by the daemon's GeoIP (#440). "" when GeoIP is off/unresolved.
	std::string country_code;
	std::uint32_t ping_ms = 0;
	std::uint32_t failed_count = 0;
	std::uint32_t users = 0;
	std::uint32_t max_users = 0;
	std::uint32_t files = 0;
	// Per-user publishing limits the server advertises: below the soft limit a
	// client may publish every file, between soft and hard only its rarest,
	// above the hard limit nothing. Both arrive only once a UDP status reply
	// has come back, so 0 means "the server has not told us", not "the limit
	// is zero" -- the desktop renders that as a blank cell, as it does for
	// Users and Files.
	std::uint32_t soft_file_limit = 0;
	std::uint32_t hard_file_limit = 0;
	// Bitmasks of the eD2k wire capabilities the server announced, decoded to
	// booleans on the way out (ServerFlagNames.h). 0 likewise means "nothing
	// announced yet" rather than "supports nothing".
	std::uint32_t tcp_flags = 0;
	std::uint32_t udp_flags = 0;
	std::string priority; // "low" | "normal" | "high"
	bool is_static = false;
};

// /friends endpoint. The daemon ships the whole friends list inside every
// EC_OP_GET_UPDATE reply, so this is filled from the tick we already run and
// no endpoint here costs an extra roundtrip.
struct FriendSnapshot
{
	std::uint32_t ecid = 0;
	std::string name;
	std::string user_hash; // 32-char lowercase MD4, "" when added by ip:port
	// Dotted quad, "" for a zero IP -- rendered in the walker like
	// ClientSnapshot::ip, so nothing downstream has to know the wire encoding.
	std::string ip;
	std::uint16_t port = 0;
	std::uint32_t client_ecid = 0; // live peer this friend is linked to, 0 = offline
	// Reported by cores that serialize EC_TAG_FRIEND_FRIENDSLOT. An older
	// daemon omits the tag and this stays false, the same way is_friend and
	// credit_ratio degrade on /clients.
	bool friend_slot = false;
};

// /chats endpoints. One conversation with one peer, mirrored from the
// daemon's CChatSessionStore over EC_OP_GET_CHAT_SESSIONS.
//
// Keyed on the GUI_ID the wire already uses -- (ip << 16) | port -- which the
// REST layer renders as the readable "<ip>:<port>" conversation key. Stable
// across peer reconnects, unlike an ECID, and converts straight back to the
// GUI_ID the EC ops want.
struct ChatMessageSnapshot
{
	std::uint32_t id = 0; //!< monotonic per daemon process; a safe `since_id` cursor
	bool outgoing = false;
	std::uint32_t timestamp = 0; //!< unix seconds, stamped by the core
	std::string text;
};

struct ChatSessionSnapshot
{
	std::uint64_t gui_id = 0;
	std::string ip; //!< dotted quad, rendered in the walker like ClientSnapshot::ip
	std::uint16_t port = 0;
	std::string name;              //!< peer display name; "" when the core has none
	std::uint32_t client_ecid = 0; //!< live peer, 0 when offline
	std::uint32_t friend_ecid = 0; //!< friend entry, 0 when not a friend
	std::vector<ChatMessageSnapshot> messages;

	//! Highest id held here, 0 when empty.
	std::uint32_t LastMsgId() const { return messages.empty() ? 0 : messages.back().id; }

	//! The REST conversation key, "<ip>:<port>".
	std::string PeerKey() const { return ip + ":" + std::to_string(port); }

	//! Display name, falling back to the desktop's own rendering when the
	//! core has no nick for the peer (CChatSelector builds the same string).
	//! Shared by the list, the detail read and the SSE payload so a client
	//! never sees a blank name from one and a real one from another.
	std::string DisplayName() const
	{
		return name.empty() ? ("IP: " + ip + " Port: " + std::to_string(port)) : name;
	}
};

// Renders an IPv4 address that arrives LSB-first -- the layout
// EC_TAG_CLIENT_USER_IP, the Kad address tags and the eD2k id all share, and
// what Uint32toStringIP() renders on the desktop side. Lives here rather than
// in the refresher because the snapshot layer needs it too (a chat peer key
// is built from a GUI_ID, with no walker involved).
std::string IPv4ToDotted(std::uint32_t ip_lsb_first);

// Is this request target eligible for the response-ETag memo?
//
// OPT-IN. The memo key is (target, snapshot revision) and nothing else, which
// makes two demands on anything eligible; a route that fails either one gets
// answered 304 for content that has changed:
//
//  1. Its body must move only when the state moves, so the revision covers
//     it. Endpoints with their own TTL cache, an append-only mirror, a
//     refresh-on-read, or a live EC roundtrip per request do not qualify.
//  2. Its body must be identical for every caller. The key carries no
//     principal, so a per-caller document would share one validator between
//     whoever asked first.
//
// This was an exclusion list and it was wrong four times over, each time for
// a different reason. The eligible set is now spelled out instead, so the
// cost of overlooking a route is a wasted hash rather than a stale 304.
bool MemoizableTarget(const std::string &target);

// Whether a response may be read from, or written to, the ETag memo. Two
// independent conditions, both required:
//
//  1. MemoizableTarget(target) -- the opt-in eligibility above.
//  2. The snapshot revision did not move while the handler ran. The body is
//     serialized inside the handler under its own read lock, dropped before
//     the caller can sample again; if a write lands in that window the body
//     belongs to `rev_before` while the key would claim `rev_after`, and every
//     later hit serves a validator describing neither.
//
// Condition 2 guards a race, so nothing in a sequential test notices when it
// is removed. It lives here, as a decision separate from the dispatch that
// makes it, so that removing it fails a test instead of nothing.
bool MemoUsable(const std::string &target, std::uint64_t rev_before, std::uint64_t rev_after);

// Whether the dispatcher should hash the body and stamp its own ETag.
//
// `handler_set_etag` is the half worth spelling out: a handler that computed
// its own validator owns it, and stamping the body hash over the top hands out
// two different ETags for one resource depending on which branch answered.
// That is exactly what the static path did before -- it clears the body for
// HEAD, so only the GET reached the hashing branch.
bool ShouldStampEtag(bool is_safe_method, bool handler_set_etag, unsigned status, bool body_empty);

// The same key built straight from a GUI_ID, for paths that only have the id
// (a session that was closed is gone from the snapshot, so there is no
// ChatSessionSnapshot left to ask). GUI_ID is (ip << 16) | port.
std::string ChatPeerKeyFromGuiId(std::uint64_t gui_id);

// /kad endpoint. Single composite snapshot pulled from the STAT_REQ
// response we're already fetching for /status — saves a roundtrip
// since amuled's `EC_OP_STAT_REQ` at `EC_DETAIL_CMD` ships every
// `EC_TAG_STATS_KAD_*` we want here.
struct KadSnapshot
{
	std::string state; // "disabled" | "connecting" | "connected"
	// Our own 128-bit Kademlia node id, 32 lowercase hex chars.
	// Empty while Kad is not running -- amuled gates the
	// EC_TAG_KAD_ID sub-tag on Kademlia::CKademlia::IsRunning(),
	// which is exactly the condition `state == "disabled"` covers.
	// Unlike the session-scoped ECIDs and the server-assigned eD2k
	// id, this one is persisted (preferencesKad.dat) and survives
	// daemon restarts.
	std::string node_id;
	// Everything below is emitted as `null` unless Kad is CONNECTED, and the
	// `has_*` flags are what carries that -- one per JSON object rather than
	// one per field, since they share a single gate.
	//
	// These used to be plain values with a `0`/`false` default, so a
	// disconnected daemon answered with numbers that looked live. Measured on a
	// real node with Kad stopped: `nodes` reported 2 and `firewalled_tcp`
	// reported true -- claims about a network we are not on. `nodes` is the
	// worst, because it is the size of our OWN routing table rather than
	// anything network-wide, and contacts outlive the disconnect.
	//
	// REFERENCE.md's "Unknown values" rule is explicit that an unknown value is
	// `null` and "neither is ever spelled `0` or `-1`". A count for a network
	// we are not connected to is precisely unknown: `0` reads as "the network
	// is empty" and a stale figure reads as "this is current" -- both worse
	// than `null`, because both are syntactically valid answers a consumer will
	// happily plot.
	//
	// Safe as a default-false flag because RefresherTick builds a fresh
	// KadSnapshot every tick, so an ungated field keeps its default rather than
	// the previous tick's value.
	bool firewalled_tcp = false;
	bool has_firewalled_tcp = false;
	bool firewalled_udp = false;
	bool has_firewalled_udp = false;
	bool lan_mode = false;
	bool has_lan_mode = false;
	std::uint32_t users = 0;
	std::uint32_t files = 0;
	std::uint32_t nodes = 0;
	bool has_network = false; // gates users/files/nodes together
	std::uint32_t indexed_sources = 0;
	std::uint32_t indexed_keywords = 0;
	std::uint32_t indexed_notes = 0;
	std::uint32_t indexed_load = 0;
	bool has_indexed = false; // gates the four indexed_* together
	// Our externally-visible address as a remote Kad contact reported
	// it back, dotted-quad. Named to match the JSON and to keep it
	// apart from `buddy_ip` below, which is somebody else's. Empty
	// while Kad is not connected (amuled only ships the tag then),
	// and "0.0.0.0" while connected but not yet told our address by
	// any contact -- amuled sends GetPrefs()->GetIPAddress() as-is,
	// which is what the desktop panel renders too.
	std::string public_ip;
	// Buddy is the LowID-buddy state (for NAT-T peers). Most users see
	// "no_buddy"; networks behind aggressive NAT see "connected".
	// Defaulted rather than left empty: amuled ships
	// EC_TAG_STATS_BUDDY_STATUS only while Kad is connected, and both
	// the core (CClientList's `m_nBuddyStatus = Disconnected`) and
	// amulegui (GetTagByNameSafe()->GetInt() == 0 on an absent tag)
	// read that absence as `Disconnected`. An empty string would be a
	// fourth value outside the endpoint's own enum.
	std::string buddy_status = "no_buddy"; // "no_buddy" | "connecting" | "connected"
	bool has_buddy = false;                // gates buddy_status/ip/port together
	// The buddy's address, dotted-quad. "0.0.0.0"/0 while Kad is
	// connected with no buddy (amuled ships the tags as 0), empty
	// while Kad is not connected at all -- same "not known" split as
	// public_ip above. Only meaningful when buddy_status is
	// "connected".
	std::string buddy_ip;
	std::uint16_t buddy_port = 0;
};

// One per download category (categories live in amuled's preferences;
// the EC packet bundles them under `EC_PREFS_CATEGORIES`). Index 0
// is the implicit "All" category. Refresher fetches the full set on
// each tick — categories rarely change but the cost is bounded by
// the typical 0-10 entry count.
struct CategorySnapshot
{
	std::uint32_t index = 0;
	std::string name;
	std::string path;
	std::string comment;
	std::uint32_t color = 0;
	std::uint8_t priority_code = 0;
	std::string priority; // human-readable (very_low/low/normal/high/release/auto)
};

// One typed value carried by a stats-tree node. The EC packet transports
// the untranslated English label template plus one or more typed values
// (EC_TAG_STAT_NODE_VALUE), so the API exposes them structurally rather than
// flattening through GetDisplayString() (which translates and locale-formats
// in the amuleapi process). `type` is the EC value type as a stable lowercase
// string; the raw value lands in exactly one of num/dbl/str per `kind`.
// `extra` holds the optional nested sub-value — at most one level deep,
// matching the EC encoding. It is whatever the desktop prints in
// parentheses, which is three different quantities depending on the node:
// a percentage of the parent (CStatTreeItemCounterTmpl with stShowPercent),
// a packet count beside a byte total (CStatTreeItemPackets), or the all-time
// total beside a session figure (CStatTreeItemUlDlCounter). It is `type`-
// tagged, so a client formats it from `type` rather than from its position.
struct StatsTreeValue
{
	enum Kind
	{
		Num, // integer/bytes/time/speed -> num (raw seconds/bytes/…)
		Dbl, // double -> dbl
		Str  // string -> str (raw, untranslated English)
	};

	std::string type;
	Kind kind = Num;
	std::uint64_t num = 0;
	double dbl = 0.0;
	std::string str;
	// Locale-independent token for a well-known sentinel value
	// (EC_TAG_STAT_VALUE_ENUM, e.g. "never"/"not_available"). Empty when the
	// value is not a sentinel; surfaced as an additive "token" field so
	// clients need not match the English `value`/`str`.
	std::string enum_token;
	std::vector<StatsTreeValue> extra;
};

// One node in the recursive stats tree (amuled's "Statistics" panel
// contents — counters, ratios, uptime, transfer aggregates, etc.).
// `label` is the untranslated English template exactly as EC carries it
// (e.g. "Uptime: %s"); `values` are the typed raw values that fill it, so
// clients do their own formatting/localization. The API contract is English
// text + C-locale numbers, independent of the amuleapi/amuled --locale.
struct StatsTreeNode
{
	// The EC_TAG_STATTREE_CAPPING value this tree was fetched at. Only
	// meaningful on the root; carried so the unkeyed TTL cache can reject
	// an entry fetched at a different cap.
	std::uint8_t max_client_versions = 0;

	// Stable, untranslated machine key (EC_TAG_STAT_NODE_KEY). Empty when
	// the node carries no key; omitted from JSON in that case.
	std::string key;
	// Raw, untranslated machine value for data-labelled nodes (client
	// version / OS string), from EC_TAG_STAT_NODE_RAW. Empty when absent;
	// surfaced as "raw" so clients need not parse it out of `label`.
	std::string raw;
	std::string label;
	std::vector<StatsTreeValue> values;
	std::vector<StatsTreeNode> children;

	// Raw numeric UL:DL ratio (download-per-upload) for the ratio node,
	// parsed from EC_TAG_STAT_NODE_RATIO[_TOTAL]. Present only on that node
	// and only when the daemon could compute it (both sides > 0); surfaced
	// as a "ratio" object so clients need not parse the composite string.
	bool has_ratio_session = false;
	double ratio_session = 0.0;
	bool has_ratio_total = false;
	double ratio_total = 0.0;
};

// Time-series data for /stats/graphs/{graph}. amuled keeps a
// circular buffer of uint32 samples per series at 1-sec cadence;
// the refresher pulls the most recent `kRefreshWindow` samples per
// tick and stores them here, with the most recent sample at
// `points.back()` corresponding to the snapshot wall-clock at
// `snapshot_at` (CState::SnapshotAt()).
//
// Four series fan out from a single `EC_OP_GET_STATSGRAPHS` packet
// (download, upload, connections, kad). Handler picks the one named
// in the `{graph}` path segment.
struct StatsGraphs
{
	// Seconds between samples, as actually requested of amuled via
	// EC_TAG_STATSGRAPH_SCALE. Reported as `interval_seconds` and used to
	// space the reconstructed timestamps, so it has to be the value that
	// was asked for rather than an assumption. Also the cache key: an
	// entry fetched at one interval must not answer a request for another.
	std::uint32_t interval_seconds = 1;

	std::vector<std::uint32_t> download_bytes_per_second;
	std::vector<std::uint32_t> upload_bytes_per_second;
	std::vector<std::uint32_t> connections;
	std::vector<std::uint32_t> kad_nodes;

	// Second data blob (EC_TAG_STATSGRAPH_DATA_CONN), point-aligned with
	// the four above and only meaningful on the connections graph. Empty
	// when the daemon predates the tag -- which is why these are reported
	// as absent rather than as zeros: "not reported" and "nothing was
	// transferring" are different answers.
	std::vector<std::uint32_t> active_uploads;
	std::vector<std::uint32_t> active_downloads;

	// Records amuled holds per resolution range (EC_TAG_STATSGRAPH_DEPTH).
	// Asking for more points than this makes the daemon repeat records
	// rather than fail, and no timestamps travel on the wire, so the
	// caller cannot spot it -- every series is truncated to this.
	std::uint32_t max_points = 1800;

	// Session running totals — reported alongside the time-series so the
	// panel can show "this session total" without a separate roundtrip.
	//
	// The daemon divides the two byte counters by 1024 before sending
	// (Statistics.cpp, RecordHistory), so these are scaled back on parse
	// and really are bytes, to 1 KiB granularity.
	std::uint64_t session_download_bytes = 0;
	std::uint64_t session_upload_bytes = 0;

	// NOT a transfer figure: the running per-second sum of the Kad node
	// count, i.e. node·seconds. Divided by session_duration_seconds it
	// gives the session-average node count, which is its only use.
	std::uint64_t session_kad_node_seconds = 0;

	// Daemon uptime in seconds at the newest point
	// (EC_TAG_STATSGRAPH_SESSION_TIMESPAN). Without it the three totals
	// above cannot be turned into averages. 0 when not reported.
	double session_duration_seconds = 0.0;
};

// One result from a /search/results poll. Identity is the file's
// MD4 hash. amuled accumulates results in its `searchlist`
// singleton as packets come in from servers/Kad; the client polls
// EC_OP_SEARCH_RESULTS to drain.
struct SearchResult
{
	std::uint32_t ecid = 0;
	std::string hash; // 32-char hex MD4
	std::string name;
	std::uint64_t size = 0;
	std::uint32_t source_count = 0;
	std::uint32_t complete_source_count = 0;
	bool already_downloaded = false;
	std::uint8_t rating = 0;
	// Download status of the result on this node (issue #429), a lowercase
	// string from the CSearchFile enum: "new" | "downloaded" | "queued" |
	// "canceled" | "queued_canceled".
	std::string status;
	// File-type token derived from the filename (like the shared-detail
	// `file_type`), e.g. "video"/"audio"; "" if the name has no extension.
	std::string type;
	// The folder this file sits in inside the browsed peer's share
	// (EC_TAG_SEARCHFILE_DIRECTORY). The core emits it for results filed
	// from a peer's shared-file list and for nothing else, so it is empty
	// on every ordinary server/Kad hit. Per-result rather than per-search:
	// two copies of one file in different folders of the same share group
	// under a single parent, each keeping its own folder, exactly as the
	// desktop's Directories column shows them.
	std::string directory;
	// Audio/video media metadata (issue #430), same shape as the file
	// detail endpoints' `media` object. `has_media` gates it, and like the
	// file-side flag above it is DERIVED from the field values rather than
	// from which tags arrived -- a hit carrying an FT_MEDIA_* tag whose value
	// is empty reads as no media, which is what the daemon means by sending
	// one. Omitted entirely when false (most remote results).
	bool has_media = false;
	struct Media
	{
		std::uint32_t duration_seconds = 0;
		std::uint32_t bitrate_kilobits_per_second = 0;
		std::string codec;
		std::string artist;
		std::string album;
		std::string title;
	} media;

	// Result grouping (issue #431): same-hash/same-size hits advertised
	// under different filenames. `parent_ecid`/`has_parent` are set on a
	// child during decode; the refresher then folds children into their
	// parent's `children` list and drops them from the top-level set, so
	// the API emits one parent row per hash+size with the alternative
	// names nested. `children` is empty for a hit seen under one name.
	std::uint32_t parent_ecid = 0;
	bool has_parent = false;
	struct Child
	{
		std::uint32_t ecid = 0;
		std::string name;
		std::string hash; // same as the parent's (that's why they group)
		std::uint32_t source_count = 0;
		std::uint32_t complete_source_count = 0;
		// Same meaning as the parent's `directory`, carried per child
		// because two copies in different folders group together.
		std::string directory;
	};
	std::vector<Child> children;

	// On-demand Kad community ratings/comments for this result (issue #434),
	// same shape as a download's `comments`. A search hit has no connected
	// sources, so these are purely the Kad notes. `kad_comment_searching` stays
	// true while a lookup started via POST /search/results/{hash}/comments is in
	// flight, so clients can poll until it finishes. `rating` -1 means a comment
	// with no rating.
	struct Comment
	{
		std::string username;
		std::string filename;
		std::int32_t rating = 0;
		std::string comment;
	};
	std::vector<Comment> comments;
	bool kad_comment_searching = false;
};

// Refresher-tracked lifecycle of the currently-active (or last-finished)
// search. The refresher reads EC_TAG_SEARCH_LIFECYCLE_STATE (added in the
// EC protocol cleanup landed earlier in this PR) and maps it directly
// here — no sentinel decode, no state machine, no defensive timeout.
struct SearchProgressSnapshot
{
	// True between POST /search and the daemon-reported finished state.
	// Drives whether the refresher keeps polling EC_OP_SEARCH_RESULTS +
	// EC_OP_SEARCH_PROGRESS.
	bool active = false;
	// "global" | "local" | "kad". Captured from POST /search's `type`
	// param. Surfaced in `search_progress` SSE so consumers can
	// distinguish which network produced the result set.
	std::string kind;
	std::uint32_t percent = 0; // 0..100, daemon-computed for every
				   // kind (global = real server-queue
				   // percent; Kad = cosmetic time-ramp;
				   // 100 on finished)
	bool complete = false;     // true exactly once on the lifecycle
				   // RUNNING → FINISHED edge
	// Monotonically-increasing per POST /search. MarkSearchStarted
	// bumps it; the refresher copies it through unchanged. EventDiff
	// treats a generation change as a guaranteed emit trigger so the
	// terminal `search_progress` frame can't be lost when a search
	// starts and finishes inside a single refresher tick (a race
	// against the tick boundary; see @ngosang's PR review).
	std::uint64_t generation = 0;
};

// The byte width of one partfile chunk. Authoritative copy is PARTSIZE in
// `protocol/ed2k/Constants.h`, which is deliberately NOT included here: that
// header is written against amule's legacy `uint64`/`uint32` typedefs from
// src/Types.h, and dragging those into the webapi layer (and into its unit
// tests) costs more than one restated number. amule has never changed
// PARTSIZE since the ed2k spec was frozen.
//
// One copy, in one place, is the point: the literal used to sit in Api.cpp
// beside six separately open-coded ceiling divisions.
constexpr std::uint64_t kPartSizeBytes = 9728000ull;

// Number of eD2k chunks a file of `size` bytes is split into: ceil(size /
// kPartSizeBytes), and 0 for an empty file.
//
// Matches CKnownFile::SetFileSize's m_iPartCount, which reaches the same
// answer the long way round (floor + 1, then decremented back on an exact
// multiple) -- worth knowing, because a mismatch here would silently trim
// or pad every per-part bitmap.
std::uint64_t PartCountForSize(std::uint64_t size);

// Parts hashed so far by a pass running over `f` as a complete share -- Verify
// Local Data, or an AICH hashset rebuild. 0 when no such pass is running.
//
// The fallback is why this is a function: amuled emits one tag kind per ECID,
// so a download that is also shared arrives as EC_TAG_PARTFILE and its
// progress lands on the download side. SharedPartSources in Api.cpp needs the
// identical fallback for the identical reason. Every shared-side consumer --
// the JSON writer, the SSE writer and the equality test -- goes through here,
// so all three agree about which file is hashing.
std::uint16_t SharedHashingProgress(const FileSnapshot &f);

// Fill in ClientSnapshot::part_progress_percent, which is derived rather
// than refreshed: it needs the part count of the file this peer is a source
// for, which lives in a different snapshot. Left at its < 0 sentinel when
// not computable, which is how the writers know to omit the field.
//
// Shared rather than owned by the REST layer because the SSE client payload
// has to carry the same value: EVENTS.md promises an `_updated` subscriber
// gets the full new state and never has to re-GET, and this field was the
// one exception to that.
class CState;
void ComputePartProgressPercent(const CState &state, ClientSnapshot &cli);

// One concurrent search's cached state: its results (keyed by result ECID)
// and its lifecycle progress. CState holds a map of these keyed by the
// daemon-allocated search_id (see m_searches).
struct SearchSlot
{
	// What the daemon last told us, flat: every result ECID it has sent for
	// this search, parents and grouped children alike, exactly as they
	// arrived. This is the merge target for the incremental union poll --
	// a diffed tag names one ECID and carries only the fields that moved, so
	// there has to be somewhere addressable by ECID to apply it to.
	std::map<std::uint32_t, SearchResult> raw;
	// The view every reader uses: `raw` with each child folded into its
	// parent's children[] and dropped from the top level, so the API serves
	// one row per hash+size. Rebuilt from `raw` after each merge rather than
	// merged into directly, which keeps every consumer (Search(),
	// FindSearchResultByHash, EventDiff) on exactly the shape it had before
	// incremental polling existed.
	std::map<std::uint32_t, SearchResult> results;
	SearchProgressSnapshot progress;
	// What was searched for, so a consumer reading one search's results
	// does not have to cross-reference GET /search for the string. For a
	// browse ("View Files") the daemon's name for the search is the peer's
	// nickname, not a query. Empty only for a slot seeded by discovery
	// before its name was observed.
	std::string query;
	// Wall-clock second this session started the search, for ranking the
	// entries GET /search returns: that listing comes straight off
	// EC_OP_SEARCH_LIST, which walks a std::map keyed by search_id and
	// carries no timestamp, so it arrives id-ascending -- and id order is
	// not recency, since Kad ids carry SEARCH_ID_KAD_MASK (0x80000000) and
	// therefore always sort above ed2k ones. 0 for a search this session did
	// not start (another client's, or one the daemon restored from disk),
	// which is unknowable here rather than zero-valued.
	std::time_t started_at = 0;
	// When this slot's results were last pulled from the daemon. The tick
	// refreshes active searches every second; a FINISHED search is never
	// polled again, so reads of it refresh on demand instead, coalesced by
	// this stamp. Default-constructed (epoch) means "never fetched", which
	// is always stale.
	std::chrono::steady_clock::time_point last_fetch{};
	// The daemon no longer holds this search: its EC ring evicted it, or it
	// was closed core-side. Set by the tick when EC_OP_SEARCH_PROGRESS comes
	// back expired, before that same tick applies the results union -- which
	// would otherwise tombstone away exactly the results the retirement path
	// means to keep for late reads, since the union emits EC_TAG_FILE_REMOVED
	// for every result of an evicted search.
	//
	// A detached slot is frozen: ApplySearchUnion skips it for merges and
	// tombstones alike, since nothing about it can change any more. It is
	// also the preferred eviction victim, because it is the only kind whose
	// eviction costs nothing -- the daemon has nothing left to re-send.
	bool detached = false;
	// Set when a union roundtrip failed against a live socket, and cleared
	// once this slot has been re-seeded in full.
	//
	// The daemon commits its differential state while BUILDING the reply --
	// Get_EC_Response_Search_Results_Union swaps io_lastSentResultIds and
	// writes the valuemap before the packet reaches the socket -- so a reply
	// we never apply is not re-sent, it is lost. Every later poll elides the
	// results it covered. Nothing else recovers them: ResetLists deliberately
	// keeps the search state (wiping it would resync nothing), and
	// ClaimSearchRefresh refuses an active slot because the tick is supposed
	// to be covering it. This flag is what turns that dead end into one FULL
	// re-seed on the next tick.
	bool needs_resync = false;
	// Insertion order, for oldest-first eviction. Not started_at: that is 0
	// for a discovered slot, which would make every adopted search tie for
	// oldest and evict in map order.
	std::uint64_t seq = 0;
};

// `m_amule_log_lines` in CState caches /logs/amule. amule's EC
// server piggybacks new lines on STAT_REQ at `EC_DETAIL_FULL` (see
// `AddLoggerTag` in ExternalConn.cpp:700-715) via a per-EC-connection
// cursor (CLoggerAccess) — each call returns ONLY lines emitted
// since the previous STAT_REQ from the same connection. Clients
// tail with `?tail=N`.
//
// **No cap on history.** Per operator preference, every line stays
// in memory until amuleapi restarts; log volume is bounded by
// operator habits (idle ~tens of KB/day; busy ~hundreds).

// /logs/server_info. amule has no incremental EC op for this log
// (no equivalent of CLoggerAccess for ServerInfoLog), so the
// refresher fetches the entire string via EC_OP_GET_SERVERINFO each
// tick and the cache stores the latest snapshot. Server-info logs
// are small (a few KB at most — just server connection chatter), so
// the per-tick rebuild cost is negligible.
struct ServerInfoLog
{
	std::string text;
};

// amuled preferences subset surfaced via /preferences. The amuled
// preferences corpus is enormous (every UI panel has its own
// section); for v0.1 we ship the common-case fields:
// nick, transfer limits, ports, connection toggles. / later
// can extend this if a real client reports needing more.
struct PreferencesSnapshot
{
	// [General]
	std::string nickname;
	std::string user_hash;
	std::string daemon_host_name;
	bool version_check_enabled = false;
	// Capability: the connected daemon is built with ENABLE_VERSION_CHECK
	// (emits EC_TAG_GENERAL_VERSION_CHECK_AVAILABLE). False for OS-package
	// or pre-3.1 daemons; combined with version_check_enabled to decide whether
	// update checking is active. See /version's "update" object.
	bool version_check_available = false;

	// [Connection]
	std::uint32_t max_upload_kibibytes_per_second = 0;
	std::uint32_t max_download_kibibytes_per_second = 0;
	std::uint32_t upload_slot_min_kibibytes_per_second = 0;
	std::uint16_t tcp_port = 0;
	std::uint16_t udp_port = 0;
	// Positive sense: true = the extended UDP port (Kad / global search) is on.
	// The EC layer carries the opposite (EC_TAG_CONN_UDP_DISABLE); the API
	// inverts on read and write.
	bool extended_udp_port_enabled = true;
	std::uint32_t max_sources_per_file_count = 0;
	std::uint32_t max_connection_count = 0;
	bool autoconnect = false;
	bool reconnect_on_connection_loss = false;
	bool ed2k_enabled = false;
	bool kad_enabled = false;
	// Bind the daemon's listening sockets to this local IP (empty = any).
	// Applied on next daemon start, same as the desktop control.
	std::string bind_address;
	// Bind to a named network interface (empty = any); daemon-side name.
	std::string bind_interface;
	// Proxy the daemon routes P2P + HTTP through. proxy_password is
	// write-only (accepted on PATCH, never surfaced here / on GET).
	bool proxy_enabled = false;
	// Serialized enum "socks5" / "socks4" / "http" / "socks4a" (#655): the
	// EC layer carries the wire ints 0..3, the API spells them out so a
	// client needs no magic-number table. Empty for CProxyType PROXY_NONE
	// (-1, the "no proxy configured" state the daemon always serializes),
	// which has no enum string and cannot be set back over PATCH --
	// proxy_enabled is the off switch.
	std::string proxy_type;
	std::string proxy_host;
	std::uint16_t proxy_port = 0;
	bool proxy_auth = false;
	std::string proxy_user;
	// UPnP. upnp_enabled toggles router forwarding of the P2P ports (which
	// are tcp_port / udp_port themselves); upnp_control_point_port is the UPnP control
	// point's own local port (0 = auto), not a forwarded port. upnp_supported
	// is a read-only capability: the daemon advertises whether it was built
	// with UPnP (false on a core built -DENABLE_UPNP=OFF).
	bool upnp_supported = false;
	bool upnp_enabled = false;
	std::uint16_t upnp_control_point_port = 0;

	// --- Extended EC-carried categories (issue #437) -----------------
	// Every field below maps 1:1 to an EC tag the daemon already
	// serializes in CEC_Prefs_Packet and applies in Apply(); the webapi
	// just requests the wider selection bitmask and plumbs them through.
	// Nested sub-structs mirror the nested JSON the endpoint emits.

	// [Directories] EC_TAG_PREFS_DIRECTORIES
	struct DirectoriesPrefs
	{
		std::string incoming_path;
		std::string temp_path;
		std::vector<std::string> shared_paths;
		bool share_hidden = false;
		bool rescan_on_startup = false;
		bool follow_symlinks = false;
		std::string exclude_patterns;
		bool exclude_patterns_use_regex = false;
	} directories;

	// [Files] EC_TAG_PREFS_FILES
	struct FilesPrefs
	{
		bool ich_enabled = false;
		bool trust_unverified_aich_hashes = false;
		bool add_new_downloads_paused = false;
		bool new_downloads_auto_priority = false;
		bool new_shared_files_auto_priority = false;
		bool prioritize_first_last_chunks = false;
		bool on_finished_start_next_paused = false;
		bool on_finished_start_next_in_same_category = false;
		bool save_sources_for_rare_files = false;
		bool preallocate_full_file_size = false;
		// Memory-mapped file I/O (#565). mmap_supported is a read-only daemon
		// capability (mirrors upnp_supported): true only when the core was built
		// with mmap support. mmap_enabled is the runtime preference and is only
		// accepted on PATCH when mmap_supported is true.
		bool mmap_supported = false;
		bool mmap_enabled = false;
		bool stop_on_low_disk_space = false;
		std::uint32_t min_free_space_mebibytes = 0;
		// Positive sense (#655): true = part files are created sparse, so
		// blocks are allocated on demand. The core stores exactly this
		// (s_createFilesSparse, default on); it is only the EC layer that
		// carries the negation, as EC_TAG_FILES_CREATE_NORMAL present ==
		// "not sparse". The schema's `invert` column undoes that on both
		// the read and the write path. Only does real work when the core
		// runs on Windows -- on POSIX both branches create the part file
		// identically.
		bool create_sparse_files = true;
		bool on_finished_start_next_alphabetically = false;
		bool endgame_mode_enabled = false;
		// Media metadata (issue #140): probe shared files with ffprobe to
		// advertise length/bitrate/codec. Empty path = daemon auto-detect.
		bool media_metadata_enabled = false;
		std::string ffprobe_path;
	} files;

	// [Servers] EC_TAG_PREFS_SERVERS
	struct ServersPrefs
	{
		bool remove_dead_servers = false;
		std::uint32_t dead_server_retry_count = 0;
		bool update_list_at_startup = false;
		bool update_list_from_server = false;
		bool update_list_from_client = false;
		bool server_priority_system_enabled = false;
		bool smart_lowid_check_enabled = false;
		bool safe_server_connect_enabled = false;
		bool autoconnect_static_servers_only = false;
		bool manual_servers_high_priority = false;
		std::string update_url;
	} servers;

	// [Security] EC_TAG_PREFS_SECURITY
	struct SecurityPrefs
	{
		// Serialized enum "everybody" / "friends" / "nobody" (#655). The EC
		// layer carries the 3-state int (EC_TAG_SECURITY_CAN_SEE_SHARES /
		// s_iSeeShares: 0 = everybody, 1 = friends only, 2 = nobody); the
		// API spells it out, and the name says it is not a yes/no question.
		std::string shared_files_visibility = "everybody";
		bool ipfilter_clients_enabled = false;
		bool ipfilter_servers_enabled = false;
		bool ipfilter_auto_update = false;
		std::string ipfilter_update_url;
		std::uint32_t ipfilter_min_access_level = 0;
		bool ipfilter_include_lan_ips = false;
		bool secure_identification_enabled = false;
		bool protocol_obfuscation_enabled = false;
		bool obfuscation_requested = false;
		bool obfuscation_required = false;
		bool reject_spoofed_source_ips = false;
		bool system_ipfilter_enabled = false;
	} security;

	// [MessageFilter] EC_TAG_PREFS_MESSAGEFILTER
	struct MessageFilterPrefs
	{
		bool enabled = false;
		bool filter_all_messages = false;
		bool accept_from_friends_only = false;
		bool accept_from_known_clients_only = false;
		bool filter_by_keyword = false;
		std::string keywords;
		bool log_filtered_messages = false;
		bool filter_comments = false;
		std::string comment_keywords;
	} message_filter;

	// [RemoteControls] EC_TAG_PREFS_REMOTECTRL. Passwords are
	// write-only (set via PATCH, never serialized here).
	//
	// Two unrelated remote-control subsystems live under one EC category, so
	// the JSON nests them (#655) instead of prefixing every field:
	// remote_controls.webserver.{...} / remote_controls.amuleapi.{...}. Both
	// sub-objects still pack into the single EC_TAG_PREFS_REMOTECTRL group on
	// the write path -- the nesting is an API-shape choice, not an EC one.
	struct RemoteControlsPrefs
	{
		struct WebserverPrefs
		{
			bool enabled = false;
			std::uint32_t port = 0;
			bool gzip_enabled = false;
			std::uint32_t refresh_seconds = 0;
			// `template_name`, not `template`: the JSON key was renamed to
			// match the member, which could never be `template` (C++ keyword).
			std::string template_name;
			bool guest_enabled = false;
		} webserver;
		struct AmuleApiPrefs
		{
			bool enabled = false;
			std::uint32_t port = 0;
			std::string bind_address;
		} amuleapi;
	} remote_controls;

	// [OnlineSignature] EC_TAG_PREFS_ONLINESIG
	struct OnlineSignaturePrefs
	{
		bool enabled = false;
		std::string directory;
		std::uint32_t update_frequency_seconds = 0;
	} online_signature;

	// [advanced] (EC group: CORETWEAKS)
	struct AdvancedPrefs
	{
		std::uint32_t max_new_connections_per_5_seconds = 0;
		bool verbose_logging = false;
		std::uint32_t file_buffer_bytes = 0;
		std::uint32_t max_upload_queue_client_count = 0;
		std::uint32_t server_keepalive_timeout_minutes = 0;
		std::uint32_t kad_max_concurrent_source_search_count = 0;
		std::uint32_t kad_source_reask_minutes = 0;
		std::uint32_t source_reask_minutes = 0;
	} advanced;

	// [kad] (EC group: KADEMLIA)
	struct KadPrefs
	{
		std::string update_url;
	} kad;

	// [geoip] (EC group: IP2COUNTRY, #440). The daemon only emits
	// this category on a GeoIP-capable build, so an absent category leaves
	// `supported` false (mirrors version_check_available: a capability the
	// connected daemon advertises, not a stored setting). `source` is the
	// serialized enum "dbip" / "maxmind" / "custom" (next-download
	// selector). The trailing status fields are read-only daemon state.
	// `maxmind_license` round-trips plainly — it is a config string the
	// core already serializes and the desktop GeoIP panel shows, not a
	// masked password like the [RemoteControls] ones.
	struct GeoipPrefs
	{
		bool supported = false;
		bool enabled = false;
		std::string source; // "dbip" / "maxmind" / "custom"
		std::string custom_update_url;
		std::string maxmind_license;
		bool auto_update = false;
		std::string loaded_source;
		std::string db_path;
		bool db_loaded = false;
		bool download_in_progress = false;
		std::string last_update_status;
	} geoip;
};

struct StatusSnapshot
{
	// "connected" / "connecting" / "disconnected" — the literal
	// string the API returns. Done at parse time rather than
	// emit time so the snapshot is self-describing (debug-dump-
	// friendly) and the emit path stays one-liner trivial.
	std::string ed2k_state = "disconnected";
	std::string kad_state = "disabled";

	// Nickname is intentionally NOT a /status field — it lives in the
	// preferences EC namespace, not the STAT_REQ response. amuleapi
	// surfaces it via /api/v0/preferences where it belongs
	// semantically (it's a user-edited value, not a connection-state
	// observation). Same call /status that PHP's am_status template
	// makes.

	// Server the daemon is currently connected to (eD2k only — Kad
	// has no equivalent). Empty when ed2k_state != "connected".
	std::string server_name;
	std::string server_ip;
	std::uint32_t server_port = 0;

	// True when our eD2k id is a HighID (>= HIGHEST_LOWID_ED2K_KAD). False
	// for a LowID *and* whenever we are not connected at all -- there is no
	// id then, so gate on ed2k_state == "connected" before reading this as
	// a firewall verdict. Positive sense so it matches the peer-side
	// high_id on /clients/{ecid}, and so the disconnected case does not
	// read as an alarming "low id".
	bool ed2k_high_id = false;

	// Our eD2k id as assigned by the connected server. 0 when not
	// connected; the 0xffffffff "connect in flight" sentinel is normalized
	// to 0 rather than surfaced. Packed LSB-first, unlike the peer-side
	// ed2k_user_id on CUpDownClient, which byte-swaps a HighID.
	std::uint32_t ed2k_user_id = 0;

	// Our public IPv4 in dotted-quad form, derived from ed2k_user_id when that
	// is a HighID -- a HighID *is* the address. Empty for a LowID or while
	// disconnected, where no address exists. Formatted here rather than in
	// the handler, matching every other address in these snapshots.
	std::string ed2k_public_ip;
	// True when Kad is running but firewalled for TCP. The verdict is a
	// vote: two peers must confirm reachability over an incoming TCP
	// connection before it clears. Distinct from the UDP test, which is
	// a different mechanism -- see KadSnapshot::firewalled_udp.
	bool kad_firewalled_tcp = false;
	// `null` unless Kad is connected: IsKadFirewalled() reads a connstate bit
	// that survives the disconnect, so this answered `true` on a stopped Kad.
	bool has_kad_firewalled_tcp = false;

	// Unix timestamp of the most recent connect (amule-org/amule#174),
	// from EC_TAG_CONNSTATE's optional {ED2K,KAD}_CONNECTED_SINCE
	// sub-tags. 0 when not connected -- gate on ed2k_state/kad_state
	// rather than trust a 0 timestamp alone.
	std::uint64_t ed2k_connected_since = 0;
	std::uint64_t kad_connected_since = 0;

	// Bytes per second (NOT kB) so the field name matches the wire
	// units throughout. Clients that want kB/s do the divide.
	std::uint64_t download_bytes_per_second = 0;
	std::uint64_t upload_bytes_per_second = 0;

	// Protocol/control-traffic overhead, bytes/second. ADDITIVE to the two
	// rates above rather than a subset of them -- amuled keeps them as
	// separate counters and the desktop renders them as a second figure in
	// parentheses. 0 when the daemon reports nothing.
	std::uint64_t download_overhead_bytes_per_second = 0;
	std::uint64_t upload_overhead_bytes_per_second = 0;

	// Aggregate counts pulled by the same EC_OP_STATS round-trip.
	std::uint32_t ul_queue_len = 0;
	std::uint32_t total_src_count = 0;

	// Free bytes on the filesystems holding the part files and the finished
	// downloads. SIGNED, with -1 meaning "the daemon has no figure" -- the
	// state before CFreeSpaceThread publishes its first sample, and the
	// permanent state of a directory it cannot stat.
	//
	// amuled's FREE_SPACE_UNKNOWN is -1 and the EC serializer casts it
	// straight to uint64, so the wire carries 0xFFFFFFFFFFFFFFFF. Storing
	// that unsigned and emitting it would report 17 exabytes free -- the
	// exact opposite of the truth -- so it is read back as a signed -1 and
	// serialised as JSON null.
	std::int64_t temp_free_bytes = -1;
	std::int64_t incoming_free_bytes = -1;

	// ed2k network-wide totals (all connected servers). Surfaced in
	// /status as ed2k.network.{users,files} — symmetric with
	// kad.network.{users,files,nodes} on KadSnapshot. Populated from
	// EC_TAG_STATS_ED2K_{USERS,FILES}, present in the same
	// EC_OP_STAT_REQ response we already parse.
	//
	// `null` unless eD2k is connected. These are summed over the whole known
	// SERVER LIST rather than the server we are attached to, and nothing zeroes
	// them on disconnect: measured on a real node, a disconnected daemon kept
	// reporting the identical figures it had while connected, indefinitely. A
	// consumer cannot tell that from live data, which is what makes it worse
	// than a `0`.
	std::uint32_t ed2k_users = 0;
	std::uint32_t ed2k_files = 0;
	bool has_ed2k_network = false; // gates ed2k_users/ed2k_files together

	// Version-check result, relayed on the same EC_OP_STATS round-trip
	// (EC_TAG_GENERAL_VERSION_CHECK_*). done == a check has completed;
	// latest is the release string; outdated == a newer release exists;
	// timestamp is the unix time the check completed. Surfaced in
	// /version's "update" object. Absent (done == false) on daemons that
	// have not checked yet or were built without ENABLE_VERSION_CHECK.
	bool version_check_done = false;
	bool version_check_outdated = false;
	std::string version_check_latest;
	std::uint64_t version_check_timestamp = 0;
};

// ECID-keyed file map + hash→ECID index in lockstep. The index is
// maintained inline on every emplace/erase so the obvious lookup
// directions both stay O(1) avg without a per-tick rebuild pass:
//  * ECID → entry via std::unordered_map::find (file_map[]).
//  * 32-char hex MD4 hash + role → ECID via FindDownloadEcidByHash /
//    FindSharedEcidByHash (one index per role).
//
// One index per role, not one overall, because a hash does NOT name a single
// file here. A part file and the completed copy someone dropped into a shared
// folder are two amuled objects with two ECIDs and the same hash, and this map
// holds both. aMule keeps them apart by having a list per role
// (CKnownFileList and CSharedFileList are each hash-keyed and de-duplicate on
// insert, so neither ever holds two files under one hash); this map merges the
// roles, so it has to carry the role in the key instead. With a single index
// whichever entry was filed last won the slot and the other became
// unreachable while still sitting in the map (#1161, reported as #1157).
//
// Walkers reach in via find()/emplace()/erase()/begin()/end() — the
// same surface they had when this was a raw std::map<uint32_t,
// FileSnapshot>&. The wrapper intercepts the two mutations that move
// hashes around and keeps the index consistent.
//
// Invariant: `hash`, `is_downloading` and `is_shared` are what the indexes are
// built from, so they are not assigned through the iterator -- go through
// SetHash() / SetDownloading() / SetShared(), which re-file the entry. A raw
// assignment compiles and silently desyncs the index.
//
// Invariant: an entry's key IS its FileSnapshot::ecid, enforced by emplace()
// rather than left to the caller. A snapshot filed under one id but carrying
// another silently breaks every reader that resolves via find() and then
// trusts what it gets back -- /clients' file hashes among them.
class FileMap
{
public:
	using map_type = std::unordered_map<std::uint32_t, FileSnapshot>;
	using iterator = map_type::iterator;
	using const_iterator = map_type::const_iterator;

	iterator find(std::uint32_t ecid) { return m_files.find(ecid); }
	const_iterator find(std::uint32_t ecid) const { return m_files.find(ecid); }
	iterator begin() { return m_files.begin(); }
	const_iterator begin() const { return m_files.begin(); }
	iterator end() { return m_files.end(); }
	const_iterator end() const { return m_files.end(); }
	std::size_t size() const { return m_files.size(); }
	bool empty() const { return m_files.empty(); }

	// By-value param so callers can pass either an lvalue (copies) or
	// rvalue (moves) with the same call site — std::unordered_map's
	// variadic emplace is too liberal for our index-keeping discipline.
	std::pair<iterator, bool> emplace(std::uint32_t ecid, FileSnapshot f)
	{
		// The key wins -- readers resolve by it. See the invariant above.
		f.ecid = ecid;
		auto r = m_files.emplace(ecid, std::move(f));
		if (r.second) {
			Reindex(r.first);
		}
		return r;
	}

	//! Assign `hash` on an existing entry and re-file it. The refresher can
	//! learn a hash after the insert (a partfile frame with HASH suppressed,
	//! then a knownfile frame carrying it), and the index has to follow.
	void SetHash(iterator it, std::string hash)
	{
		if (it == m_files.end() || it->second.hash == hash)
			return;
		DropRows(it->first, it->second.hash);
		it->second.hash = std::move(hash);
		Reindex(it);
	}

	//! Add or remove the downloading role, keeping that role's index in step.
	void SetDownloading(iterator it, bool on)
	{
		if (it == m_files.end() || it->second.is_downloading == on)
			return;
		it->second.is_downloading = on;
		Reindex(it);
	}

	//! Add or remove the shared role, keeping that role's index in step.
	void SetShared(iterator it, bool on)
	{
		if (it == m_files.end() || it->second.is_shared == on)
			return;
		it->second.is_shared = on;
		Reindex(it);
	}

	iterator erase(iterator it)
	{
		DropRows(it->first, it->second.hash);
		return m_files.erase(it);
	}

	void clear()
	{
		m_files.clear();
		m_download_by_hash.clear();
		m_shared_by_hash.clear();
	}

	//! Resolve a hash to the entry carrying the DOWNLOADING role, ignoring a
	//! share that happens to have the same content.
	bool FindDownloadEcidByHash(const std::string &hash, std::uint32_t &out) const
	{
		return Lookup(m_download_by_hash, hash, out);
	}

	//! Resolve a hash to the entry carrying the SHARED role.
	bool FindSharedEcidByHash(const std::string &hash, std::uint32_t &out) const
	{
		return Lookup(m_shared_by_hash, hash, out);
	}

private:
	using index_type = std::unordered_map<std::string, std::uint32_t>;

	static bool Lookup(const index_type &idx, const std::string &hash, std::uint32_t &out)
	{
		auto it = idx.find(hash);
		if (it == idx.end())
			return false;
		out = it->second;
		return true;
	}

	//! Point `idx[hash]` at `ecid` when the role applies, and clear the row
	//! when it no longer does -- but only if it still names this entry, since
	//! the other entry sharing this hash may own the row.
	static void FileRow(index_type &idx, const std::string &hash, std::uint32_t ecid, bool applies)
	{
		auto it = idx.find(hash);
		if (applies) {
			idx[hash] = ecid;
		} else if (it != idx.end() && it->second == ecid) {
			idx.erase(it);
		}
	}

	//! Re-file one entry into whichever role indexes its current flags call
	//! for. Cheap and idempotent, so callers can just call it after a change.
	void Reindex(iterator it)
	{
		if (it->second.hash.empty())
			return;
		FileRow(m_download_by_hash, it->second.hash, it->first, it->second.is_downloading);
		FileRow(m_shared_by_hash, it->second.hash, it->first, it->second.is_shared);
	}

	//! Remove every row naming `ecid` under `hash`, in both roles.
	void DropRows(std::uint32_t ecid, const std::string &hash)
	{
		if (hash.empty())
			return;
		FileRow(m_download_by_hash, hash, ecid, false);
		FileRow(m_shared_by_hash, hash, ecid, false);
	}

	map_type m_files;
	index_type m_download_by_hash;
	index_type m_shared_by_hash;
};

// One State instance per amuleapi process. The mutex protects every
// member field; refresh swaps the whole struct under it, handlers
// read the substructs they need under it.
class CState
{
public:
	// True once the refresher has completed at least one successful
	// tick. Until then, the /status endpoint returns 503 with
	// `ec_unavailable` so clients can tell "amuleapi is up but amuled
	// isn't responding" apart from a hard 5xx.
	bool HasFirstSnapshot() const;

	// Wall-clock at which the last successful tick completed. Used
	// to populate `snapshot_at` / `snapshot_at_unix` on every list
	// response.
	std::time_t SnapshotAt() const;

	// True iff the most recent tick succeeded. False after a tick
	// failed (EC timeout / disconnect); the refresher keeps the
	// stale snapshot for clients but flips this flag.
	bool EcConnected() const;

	StatusSnapshot Status() const;
	KadSnapshot Kad() const;
	// One-shot snapshot of the four scalars /api/v0/status composes
	// from. Taken under a single shared_lock so the four pieces
	// describe the same refresher tick — no risk of `status` and
	// `kad` straddling a tick boundary.
	struct DashboardSnapshot
	{
		StatusSnapshot status;
		KadSnapshot kad;
		std::time_t snapshot_at = 0;
		bool ec_connected = false;
	};
	DashboardSnapshot Dashboard() const;
	PreferencesSnapshot Preferences() const;
	// Full snapshot of the amule log lines (oldest-first). API
	// handlers slice the tail before serialising via the
	// `?tail=N` query param.
	std::vector<std::string> AmuleLog() const;
	//! The lines from `first` on, plus the current total, under one lock. A
	//! `first` past the end gives an empty tail, which with `total` is how the
	//! caller sees a truncation. Copies nothing when nothing was appended,
	//! which the per-tick log diff needs and AmuleLog() cannot do.
	//! `generation`, when asked for, is read under the same lock as the window
	//! and is bumped by every ClearAmuleLog(). Size alone cannot detect a
	//! clear: a buffer cleared and refilled past its old length between two
	//! ticks looks like growth, and the diff would then publish a mid-buffer
	//! slice as a tail. Taking it here rather than from a second call is what
	//! keeps a concurrent DELETE from tearing the pair.
	std::vector<std::string> AmuleLogFrom(
		std::size_t first, std::size_t &total, std::uint64_t *generation = nullptr) const;
	ServerInfoLog ServerInfo() const;

	// Flat list views. Reads the ECID-keyed map under shared_lock and
	// returns a copy of the snapshot values in unordered_map iteration
	// order — bucket-dependent, NOT stable across ticks. Consumers
	// that want a specific order sort on their side (by name / date /
	// progress / etc.).
	//
	// The role-filtered views are gone: no reader wanted whole snapshots copied
	// out. Filter `m_files` on the role flag inside WithFiles() instead.
	//! Read the unified file map in place, under the shared lock. Same contract
	//! as WithKnownClients: the callback must not call back into CState, nor
	//! retain the reference. For readers wanting a few fields per file, or
	//! pointers to sort and page -- a FileSnapshot is 848 bytes plus a heap
	//! allocation per string, so copying the collection out is never cheap.
	// Re-entrancy detector for the callback accessors below.
	//
	// Every WithX() / MutateX() runs a caller-supplied callback while holding
	// m_mu. Calling back into the same CState from inside one takes that
	// non-recursive std::shared_timed_mutex a second time: undefined
	// behaviour, and in practice a hang -- immediately on the exclusive
	// paths, and on the shared ones as soon as a writer is queued, because
	// the implementation stops admitting new readers to avoid starving it.
	//
	// The rule was documentation only, so a violation surfaced as a wedged
	// daemon under load rather than as a test failure -- the callbacks are
	// pure today, but nothing stopped the next edit from reaching for
	// m_state. Enforced in Release as well as Debug, and aborting rather
	// than returning, for the reason the Session dtor gives in
	// HttpServer.cpp: assert() is stripped by NDEBUG, and the alternative
	// here is not a wrong answer but a process that stops responding with
	// nothing in the log. The thread would deadlock on the next line
	// regardless; this way it leaves a message and a core instead.
	//
	// Constructed BEFORE the lock, deliberately. A re-entrant call blocks on
	// the mutex it can never obtain, so a guard placed after the lock would
	// never run for the one case it exists to catch.
	class ReentryGuard
	{
	public:
		explicit ReentryGuard(const CState *self);
		~ReentryGuard();
		ReentryGuard(const ReentryGuard &) = delete;
		ReentryGuard &operator=(const ReentryGuard &) = delete;

	private:
		const CState *m_prev;
	};

	template <class F> void WithFiles(F &&fn) const
	{
		const ReentryGuard guard(this);
		std::shared_lock<std::shared_timed_mutex> lock(m_mu);
		fn(static_cast<const FileMap &>(m_files));
	}

	// Full peer list (all upload_state values, including queue
	// waiters, idle peers, and banned). Backs /clients.
	// Consumers query /clients and filter by role on their side rather
	// than asking for a pre-filtered upload view.
	std::vector<ClientSnapshot> Clients() const;

	// --- Known clients (the daemon's credit store) -------------------------
	//
	// Fetched once, then maintained: the refresher folds every tick's live
	// peers in, so the store stays current without ever being re-read. What
	// only a refetch could give is the expiry prune the core applies at its
	// own startup, and that cannot happen underneath us: HandleEcConnectionLost
	// shuts amuleapi down the moment the EC socket drops, so the process never
	// attaches to a second core. The store therefore lives for the life of the
	// process, with no invalidation path -- deliberately, since the obvious
	// place to add one (ResetLists, which fires on a failed tick against a live
	// socket) would refetch the whole store for a single null tick.
	//
	// Held rather than copied out: this is the whole store, tens of thousands
	// of records, so a by-value accessor would cost more per request than the
	// EC roundtrip it saves. Readers run under the shared lock instead.
	bool KnownClientsLoaded() const;
	//! Install the first fetch and reconcile it against the current peers.
	void SetKnownClients(std::vector<KnownClientSnapshot> &&rows);
	/**
	 * Fold this tick's peers into the store.
	 *
	 * A record whose peer is not connected cannot change -- credit totals only
	 * move during a transfer, last-seen only at disconnect -- so this touches
	 * only the connected ones, of which there are at most MaxConnections. No-op
	 * until the store has been loaded, so a daemon nobody asks about never
	 * pays for it.
	 *
	 * The store only grows between fetches: a peer met here is added and never
	 * removed, because a record leaving the daemon's own store means expiry,
	 * which it applies at its startup and we pick up on the next fetch. Growth
	 * is one row per distinct peer met, bounded by real traffic and reset with
	 * everything else by ResetLists().
	 */
	void ReconcileKnownClients();
	//! Read the store under the shared lock. The callback must not call back
	//! into CState, and must not retain the reference.
	template <class F> void WithKnownClients(F &&fn) const
	{
		const ReentryGuard guard(this);
		std::shared_lock<std::shared_timed_mutex> lock(m_mu);
		fn(static_cast<const std::vector<KnownClientSnapshot> &>(m_known_clients));
	}

	std::vector<ServerSnapshot> Servers() const;
	std::vector<FriendSnapshot> Friends() const;
	// Chat sessions, most-recently-active first (the daemon's own order).
	std::vector<ChatSessionSnapshot> Chats() const;
	// The resume cursor for the next EC_OP_GET_CHAT_SESSIONS poll: the
	// highest message id this snapshot holds. Advanced from the reply even
	// when nothing came back, so evicted ids are not re-requested forever.
	std::uint32_t ChatCursor() const;
	// Results / progress for one search. Every caller names a concrete
	// daemon-allocated id — there is no implicit "current search" — and an
	// unknown id yields an empty list / idle progress. HasSearch
	// distinguishes "unknown id" (404) from "known but empty".
	std::vector<SearchResult> Search(std::uint32_t search_id) const;
	SearchProgressSnapshot SearchProgress(std::uint32_t search_id) const;
	// True if search_id names a live slot. Used for the 404 on an id that
	// was never started, was freed, or expired.
	bool HasSearch(std::uint32_t search_id) const;
	// What this search was started with; empty for an unknown id, or for a
	// discovered slot whose name the daemon had not reported yet.
	std::string SearchQuery(std::uint32_t search_id) const;
	// When this session started the search, or 0 when it did not start it
	// (see SearchSlot::started_at). An unknown id reports 0 too; a caller
	// that must tell those apart checks HasSearch first.
	std::time_t SearchStartedAt(std::uint32_t search_id) const;
	// Slots the refresher must still poll (progress.active).
	std::vector<std::uint32_t> ActiveSearchIds() const;
	// Every slot the daemon could still speak for: attached, active or not.
	// The tick polls THIS set for expiry, not ActiveSearchIds(), because a
	// finished search is exactly the one the daemon's ring drops first and
	// the one whose results we are keeping. Naming them in the progress
	// union also refreshes their LRU entry daemon-side, which is what the
	// daemon means by "the searches a client still has open".
	//
	// Empty is also the gate for both union polls: with no attached slot
	// there is nothing the daemon could answer about, and sending either
	// request would be a roundtrip a second that can never return anything.
	// This replaced a separate HasAnySearch() predicate -- the same
	// !detached test spelled a second way, which is what drifts -- and the
	// vector it was introduced to avoid is now built for the progress union
	// regardless.
	std::vector<std::uint32_t> AttachedSearchIds() const;
	// Every live slot id, for the SSE per-search diff.
	std::vector<std::uint32_t> AllSearchIds() const;
	// Find a result carrying this (already-lowercased) hash across ALL open
	// searches — the hash-keyed comments endpoints are search-agnostic. The
	// parent owns any fetched Kad notes, so it matches ahead of its children.
	// `owner_search_id` reports which slot the hit came from, so the caller
	// can refresh that slot before reading (see ClaimSearchRefresh).
	bool FindSearchResultByHash(
		const std::string &hash_hex, SearchResult &out, std::uint32_t *owner_search_id) const;
	// Take the right to refresh one search's results from the daemon, for
	// the read paths that must not serve a frozen snapshot.
	//
	// Returns true (and stamps the slot as fetched NOW) only when the slot
	// exists, is NOT active, and its last fetch is older than `ttl`. Active
	// slots are excluded because the tick already refreshes them every
	// second. Stamping before the fetch rather than after is deliberate: it
	// is what makes this a claim, so two concurrent readers of the same
	// finished search issue one EC roundtrip between them, not two.
	bool ClaimSearchRefresh(std::uint32_t search_id, std::chrono::milliseconds ttl);

	// Categories aren't ECID-keyed (they come in via the
	// preferences packet as an indexed array); keep them as a plain
	// vector copied out under the shared lock.
	std::vector<CategorySnapshot> Categories() const;

	// /stats/tree returns the recursive tree as a single bare object.
	StatsTreeNode StatsTree() const;
	// /stats/graphs/{graph} reads one series out of the bundle.
	StatsGraphs Graphs() const;

	// Look up a single file by 32-char hex hash, then check the role.
	// Returns true on hit + role match, false on miss; on miss `out`
	// is left untouched. Used by /downloads/{hash} (download role) and
	// /shared/{hash} (shared role) — both inspect the same m_files map.
	bool FindDownload(const std::string &hash_hex, FileSnapshot &out) const;
	// Part count of the download with this hash, or 0 when there is none (or
	// it has no size yet). Exists so a per-client loop can ask the cheap
	// question without FindDownload's full FileSnapshot copy -- strings plus
	// the gap / source / comment vectors, per source, per tick.
	std::uint64_t DownloadPartCount(const std::string &hash_hex) const;
	bool FindShared(const std::string &hash_hex, FileSnapshot &out) const;

	// ECID-keyed counterparts. Used internally — there is no
	// /downloads/{ecid} or /shared/{ecid} path; the wire surface is
	// hash-only. CClientList::ApplyGetUpdate also reaches in here when
	// resolving EC_TAG_CLIENT_UPLOAD_FILE.
	bool FindDownloadByEcid(std::uint32_t ecid, FileSnapshot &out) const;
	bool FindSharedByEcid(std::uint32_t ecid, FileSnapshot &out) const;

	// INC-mode delta application. The refresher takes the unique_lock
	// once per EC roundtrip, then calls a callback with a mutable
	// reference to the unified ECID-keyed map; the callback walks the
	// EC response and upserts/removes individual entries plus role
	// flags. One unique acquisition per tick rather than N — reader
	// latency stays bounded by the parse loop, not by N independent
	// acquisitions. Both MutateDownloads and MutateShared operate on
	// the SAME m_files map; the callback decides which role to flip.
	// MutateDownloads/Shared hand out the FileMap wrapper, which keeps
	// its internal hash→ECID index in sync on every emplace/erase.
	void MutateDownloads(const std::function<void(FileMap &)> &fn);
	void MutateShared(const std::function<void(FileMap &)> &fn);
	//! Clients only. Reach for MutateClientsWithFiles below when the callback
	//! also needs the file map, rather than pairing this with WithFiles: that
	//! pair is two acquisitions where one does, which is what the clients
	//! walker was moved off.
	void MutateClients(const std::function<void(std::map<std::uint32_t, ClientSnapshot> &)> &fn);
	//! Clients plus a read-only view of the file map, under the one acquisition
	//! this was already taking. The clients walker resolves ECIDs against the
	//! files but cannot fetch them itself: m_mu is a single non-recursive
	//! mutex, so reaching back into CState from inside the callback trips
	//! ReentryGuard. Second argument carries the same borrow contract as
	//! WithFiles -- valid for the call only, never retained.
	void MutateClientsWithFiles(
		const std::function<void(std::map<std::uint32_t, ClientSnapshot> &, const FileMap &)> &fn);
	void MutateServers(const std::function<void(std::map<std::uint32_t, ServerSnapshot> &)> &fn);
	void MutateFriends(const std::function<void(std::map<std::uint32_t, FriendSnapshot> &)> &fn);
	// The walker replaces the whole session vector each tick rather than
	// merging: the daemon's reply IS the complete set, and a session missing
	// from it was closed (that absence is exactly how every client learns of
	// a close, so merging would resurrect closed conversations).
	void MutateChats(
		const std::function<void(std::vector<ChatSessionSnapshot> &, std::uint32_t &cursor)> &fn);
	// Mutate one search's result map (the refresher's per-tick full-fetch
	// overwrite). No-op if the id names no live slot.
	void MutateSearch(std::uint32_t search_id,
		const std::function<void(std::map<std::uint32_t, SearchResult> &)> &fn);
	// Mutate every search slot and the ECID->search_id index together, under
	// one exclusive lock. The incremental union poll needs both: a diffed tag
	// names no search, so the index resolves it, and a result that moves or
	// disappears has to leave the map and the index in step. Handing out both
	// halves to one callback is what makes that atomic.
	void MutateAllSearches(const std::function<void(std::map<std::uint32_t, SearchSlot> &,
			std::map<std::uint32_t, std::uint32_t> &)> &fn);

	// Wholesale reset paths. Called by the refresher after a
	// MarkTickFailure → MarkTickSuccess transition (the server's
	// CValueMap was reset on reconnect; stale entries that vanished
	// during the disconnect window would otherwise live forever in
	// the cache).
	void ResetLists();

	// Refresher-side write paths.
	void WriteStatus(StatusSnapshot s);
	void WriteKad(KadSnapshot k);
	void WritePreferences(PreferencesSnapshot p);
	void WriteCategories(std::vector<CategorySnapshot> c);
	// Append one or more new amule-log lines to the ring; trims oldest
	// entries when capacity is exceeded. Called once per refresher
	// tick with the lines drained from EC_TAG_STATS_LOGGER_MESSAGE.
	void AppendAmuleLog(std::vector<std::string> new_lines);
	// Drop every cached amule-log line. Called by DELETE /logs/amule
	// after the EC_OP_RESET_LOG roundtrip — the refresher only appends
	// (it has no equivalent of "shrink to amuled's current count"), so
	// the in-process cache MUST be cleared explicitly or the next GET
	// will keep returning the pre-reset lines. The next refresher tick
	// resumes appending from amuled's now-empty buffer.
	void ClearAmuleLog();
	void WriteServerInfo(ServerInfoLog s);
	// Called by POST /search with the daemon-allocated search_id. Creates (or
	// resets) that search's slot: clears its results and marks it active=true
	// with the requested `kind` and `query`. The refresher then polls
	// EC_OP_SEARCH_RESULTS / _PROGRESS for it each tick, mapping
	// EC_TAG_SEARCH_LIFECYCLE_STATE into `complete` / `active`.
	void MarkSearchStarted(std::uint32_t search_id, const std::string &kind, const std::string &query);
	// Seeds a slot for a search this session did NOT start itself -- a
	// search another client, or the monolithic GUI, started. Called
	// on-demand from the read paths on a cache miss, after a one-off
	// EC_OP_SEARCH_LIST confirms the core actually holds it (deliberately
	// NOT a per-tick refresher poll: that would pay an EC roundtrip every
	// tick, forever, to serve something that happens rarely). Unlike
	// MarkSearchStarted it is idempotent -- a search already known
	// (self-started or previously discovered) is left untouched rather than
	// having its accumulated results/progress reset.
	// `active` / `complete` come from the lifecycle state the same
	// EC_OP_SEARCH_LIST entry carries. Seeding them rather than assuming
	// "active" matters because callers gate on them: POST /search/{id}/more
	// rejects a finished search, and would otherwise accept one for the tick
	// or so it took to be corrected. A slot seeded inactive is not polled by
	// the tick, which is fine -- the read paths refresh an inactive slot on
	// demand (ClaimSearchRefresh).
	//
	// `reported_percent` is the daemon's own percent for this search when the
	// EC_OP_SEARCH_LIST entry carried one, and -1 when it did not. A daemon
	// older than that tag reports nothing, and the fallback then has to be
	// derived: 100 for a finished search (true by definition), 0 for a
	// running one (the tick corrects it within a second, and inventing a
	// number would flash a wrong one meanwhile).
	/**
	 * Mark a slot as no longer backed by the daemon. Freezes its results:
	 * see SearchSlot::detached. Idempotent; a no-op for an unknown id.
	 */
	void DetachSearch(std::uint32_t search_id);

	// Flag every slot the daemon could still speak for as needing a full
	// re-seed. Called when a union roundtrip failed: we cannot know what that
	// reply carried, and the daemon will not send it again.
	void MarkAllSearchesNeedResync();
	// Ids awaiting that re-seed, oldest slot first. Drained by the tick.
	// Detached slots are excluded, matching MarkAllSearchesNeedResync: one
	// detached after it was flagged has nothing left to re-seed.
	std::vector<std::uint32_t> SearchesNeedingResync() const;
	// Clears the flag for one slot without re-seeding it, for when the daemon
	// answers that the search is gone.
	void ClearSearchResyncFlag(std::uint32_t search_id);

	void MarkSearchDiscovered(std::uint32_t search_id,
		const std::string &kind,
		const std::string &query,
		bool active,
		bool complete,
		int reported_percent = -1);
	// Refresher-side write path for one search's progress snapshot.
	void WriteSearchProgress(std::uint32_t search_id, SearchProgressSnapshot s);
	// Drop a search's slot entirely: DELETE /search/{id}, or the refresher
	// observing the daemon evicted it (EC_TAG_SEARCH_EXPIRED).
	void CloseSearch(std::uint32_t search_id);
	void WriteStatsTree(StatsTreeNode t);
	void WriteGraphs(StatsGraphs g);
	void MarkTickSuccess();
	// Monotonic counter advanced by every successful refresh, from the
	// background loop and from the inline refreshes that mutating handlers
	// run. `snapshot_at` cannot play this role: it is whole seconds, so two
	// refreshes inside one second are indistinguishable, and it is stamped
	// only by the loop, so a mutation could change a body while it stood
	// still -- which answered the next conditional GET with 304 for content
	// that had just changed. Anything keyed on "has the state moved" wants
	// this, not the timestamp.
	void BumpSnapshotRevision();
	std::uint64_t SnapshotRevision() const;
	void MarkTickFailure();

private:
	mutable std::shared_timed_mutex m_mu;
	// Which CState this thread is currently inside a callback of, so the
	// guard can tell "re-entered the same instance" (a deadlock) from
	// "touched a different one" (harmless -- a different mutex).
	static thread_local const CState *t_in_callback;

	bool m_has_first_snapshot = false;
	bool m_ec_connected = false;
	std::time_t m_snapshot_at = 0;
	std::uint64_t m_snapshot_rev = 0;

	StatusSnapshot m_status;
	KadSnapshot m_kad;
	PreferencesSnapshot m_preferences;
	std::vector<CategorySnapshot> m_categories;
	// Unified ECID-keyed file map. A single entry may participate in
	// the /downloads view (`is_downloading`), the /shared view
	// (`is_shared`), or both. See FileSnapshot's header comment for
	// why the two views share storage. FileMap also owns a hash→ECID
	// index, maintained inline on every emplace/erase so /downloads/{hash}
	// + /shared/{hash} lookups stay O(1) avg without a per-tick rebuild.
	FileMap m_files;

	std::map<std::uint32_t, ClientSnapshot> m_clients;
	// See the Known clients block above. m_known_loaded distinguishes "loaded
	// and genuinely empty" from "never fetched".
	std::vector<KnownClientSnapshot> m_known_clients;
	// Indices, deliberately, not pointers: the reconcile appends while it
	// iterates, and a reallocation would dangle anything holding addresses.
	// Rows are only ever appended for the same reason -- an erase would
	// invalidate every index past it.
	//
	// Keyed on the same lowercase hex the live snapshot uses (both go through
	// CMD4Hash::Encode().Lower()); a case mismatch would silently give every
	// peer a second row.
	std::map<std::string, std::size_t> m_known_of_hash;
	bool m_known_loaded = false;
	//! Rows currently flagged online, so a peer that left is found without
	//! walking the store.
	std::set<std::size_t> m_known_online;
	//! MUST be called with m_mu held for writing.
	void ReconcileKnownClientsLocked();
	std::map<std::uint32_t, ServerSnapshot> m_servers;
	std::map<std::uint32_t, FriendSnapshot> m_friends;
	std::vector<ChatSessionSnapshot> m_chats;
	std::uint32_t m_chat_cursor = 0;
	std::vector<std::string> m_amule_log_lines;
	std::uint64_t m_amule_log_generation = 0;
	ServerInfoLog m_server_info;
	StatsTreeNode m_stats_tree;
	StatsGraphs m_graphs;
	// Multi-search: amuleapi runs several searches at once, each addressed on
	// the REST surface by its daemon-allocated search_id. One slot per search
	// holds that search's results (by result ECID) and its lifecycle progress.
	std::map<std::uint32_t, SearchSlot> m_searches;
	// Result ECID -> owning search_id, mirroring m_searches' result maps.
	//
	// Required by the incremental union poll: the daemon sends
	// EC_TAG_SEARCH_ID only the first time it tells us about a result, and
	// EC_TAG_FILE_REMOVED carries the bare ECID, so every later tag has to be
	// attributed from here. Maintained inside the same locked mutation that
	// writes the result maps -- an index that can drift from the map it
	// mirrors is how #1028 leaked keys a reload could not heal.
	std::map<std::uint32_t, std::uint32_t> m_resultOwner;
	//! Monotonic source for SearchSlot::seq.
	std::uint64_t m_search_seq = 0;
	// Ceiling on retained slots. A client that never DELETEs its searches,
	// or one watching a busy monolithic GUI, would otherwise accumulate a
	// slot and its whole result map per search for the life of the process:
	// the daemon's own kMaxEcSearches bounds only the searches it holds for
	// *this* connection, and detached slots outlive even those.
	//
	// Eviction is recoverable, which is why a cap is affordable at all: a
	// later read of an evicted id misses the cache, re-discovers it via
	// EC_OP_SEARCH_LIST and re-seeds it in full through FetchOneSearchFull.
	// Detached slots are preferred as victims anyway, since for those the
	// daemon has nothing left to re-send and nothing is lost.
	//
	// A soft cap, not a hard bound: an active slot is never a victim, so a
	// burst of more than this many concurrent searches sits above the cap
	// until they finish. That is deliberate -- evicting a search still being
	// polled would drop results the daemon has already marked delivered.
	static constexpr std::size_t kMaxSearchSlots = 64;
	// Trim m_searches back to kMaxSearchSlots, and drop the evicted slots'
	// entries from m_resultOwner with them. Caller holds the write lock.
	//
	// `exempt_id` is never chosen as a victim: callers evict straight after
	// inserting, and with every other slot active the freshly-inserted one is
	// the only eligible victim, so it would evict what it just seeded.
	void EvictSurplusSearchSlotsLocked(std::uint32_t exempt_id = 0);
};

} // namespace webapi

#endif // WEBAPI_STATE_H
