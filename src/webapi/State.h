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

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

	// Audio/video media metadata (issue #418). amuled emits it only for
	// probed files (GetMetaDataVer != 0), so `has_media` gates the
	// `media` object on the detail endpoints — omitted entirely when false.
	bool has_media = false;
	struct Media
	{
		std::uint32_t length_s = 0; // duration, seconds
		std::uint32_t bitrate = 0;
		std::string codec;
		std::string artist;
		std::string album;
		std::string title;
	} media;

	// EC_TAG_KNOWNFILE_FILENAME: a partfile's on-disk basename (e.g.
	// `001.part`), or a completed known file's directory path.
	// Interpreted per endpoint — `met_file` on /downloads/{hash},
	// `path` on /shared/{hash}.
	std::string knownfile_filename;

	// Download-side state — meaningful when `is_downloading` is true,
	// reset to default on the true→false transition (and never read
	// by `/downloads` when the flag is false).
	struct DownloadSide
	{
		std::uint64_t size_done = 0;
		std::uint64_t size_xfer = 0;
		std::uint32_t speed_bps = 0;
		std::string status; // "downloading" | "paused"
				    // | "completed" | "hashing" | ...
		// Download priority: "very_low" | "low" | "normal" | "high"
		// | "release" | "auto".
		std::string priority;
		bool priority_auto = false;
		std::uint32_t category = 0;
		double percent = 0.0;
		std::uint32_t sources_total = 0;
		std::uint32_t sources_not_current = 0;
		std::uint32_t sources_transferring = 0;
		std::uint32_t sources_a4af = 0;

		// Detail-only fields (GET /downloads/{hash}); the list endpoint
		// omits them. All decoded from tags CEC_PartFile_Tag already
		// emits under INC_UPDATE.
		std::uint32_t last_seen_complete = 0;    // unix ts; 0 = unknown
		std::uint32_t last_changed = 0;          // unix ts of last change
		std::uint32_t download_active_time = 0;  // seconds downloading
		std::uint16_t available_part_count = 0;  // parts across sources
		std::uint16_t hashing_progress = 0;      // part being hashed
		std::uint64_t lost_to_corruption = 0;    // bytes
		std::uint64_t gained_by_compression = 0; // bytes
		std::uint32_t saved_by_ich = 0;          // packets recovered by ICH
		std::uint32_t partmet_id = 0;            // numeric partfile id

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
		std::uint64_t xfer_session = 0;
		std::uint64_t xfer_total = 0;
		std::uint32_t requests_session = 0;
		std::uint32_t requests_total = 0;
		std::uint32_t accepts_session = 0;
		std::uint32_t accepts_total = 0;
		std::uint32_t complete_sources = 0;

		// Detail-only (GET /shared/{hash}). The complete-sources range
		// backs the desktop `< N` / `N - M` display; the scalar
		// `complete_sources` above stays as-is.
		std::uint16_t complete_sources_low = 0;
		std::uint16_t complete_sources_high = 0;
	} shared;
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
// (/downloads/{hash}/sources can filter by
// upload_file_hash matching the partfile.)
struct ClientSnapshot
{
	std::uint32_t ecid = 0;
	std::string client_name;
	std::string user_hash; // peer's user hash (32-char lowercase hex MD4)
	std::string ip;        // dotted-quad
	std::uint16_t port = 0;

	// Software identity. EC_TAG_CLIENT_SOFTWARE ships a numeric code
	// (SO_AMULE / SO_EMULE / etc); we decode it server-side into a
	// short label here so consumers don't need the lookup table.
	std::string software;         // "amule" | "emule" | "edonkey" | "mldonkey" | ...
	std::string software_version; // free-form string from EC_TAG_CLIENT_SOFT_VER_STR
	std::string os_info;          // free-form (CLIENT_OS_INFO)

	// State machine values. We decode the raw US_*/DS_*/IS_* ints
	// into wire strings so consumers don't reach into amule's enums.
	std::string upload_state;   // "uploading" | "queued" | "banned" | "connecting" | "idle" | ...
	std::string download_state; // "downloading" | "onqueue" | "noneededparts" | ... | "idle"
	std::string ident_state;    // "unknown" | "identified" | "bad_guy" | ...

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
	std::string upload_file_hash;   // EC_TAG_CLIENT_UPLOAD_FILE resolved
	std::string download_file_hash; // EC_TAG_CLIENT_REQUEST_FILE resolved
	std::string download_file_name; // EC_TAG_CLIENT_REMOTE_FILENAME

	// Per-session transfer stats. CLIENT_UPLOAD_SESSION = bytes
	// uploaded TO this peer; PARTFILE_SIZE_XFER (when re-keyed on a
	// CLIENT_* tag) = bytes downloaded FROM this peer.
	std::uint64_t xfer_up_session = 0;
	std::uint64_t xfer_down_session = 0;
	std::uint64_t xfer_up_total = 0;
	std::uint64_t xfer_down_total = 0;
	std::uint32_t upload_speed_bps = 0;
	std::uint32_t download_speed_bps = 0;

	// Upload queue position (for peers in US_ONUPLOADQUEUE).
	// 0 when not queued.
	std::uint32_t queue_waiting_position = 0;
	// Remote queue rank — our position in THE PEER's upload queue
	// (i.e. how many other ed2k clients they're going to upload to
	// before us). 0xFFFF when their queue is full.
	std::uint16_t remote_queue_rank = 0;

	std::uint32_t score = 0;        // EC_TAG_CLIENT_SCORE
	std::string obfuscation_status; // "none" | "supported" | "required"
	bool friend_slot = false;
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
	std::uint32_t ping_ms = 0;
	std::uint32_t failed = 0;
	std::uint32_t users = 0;
	std::uint32_t max_users = 0;
	std::uint32_t files = 0;
	std::string priority; // "low" | "normal" | "high"
	bool is_static = false;
};

// /kad endpoint. Single composite snapshot pulled from the STAT_REQ
// response we're already fetching for /status — saves a roundtrip
// since amuled's `EC_OP_STAT_REQ` at `EC_DETAIL_CMD` ships every
// `EC_TAG_STATS_KAD_*` we want here.
struct KadSnapshot
{
	std::string state; // "disabled" | "connecting" | "connected"
	bool firewalled = false;
	bool firewalled_udp = false;
	bool in_lan_mode = false;
	std::uint32_t users = 0;
	std::uint32_t files = 0;
	std::uint32_t nodes = 0;
	std::uint32_t indexed_sources = 0;
	std::uint32_t indexed_keywords = 0;
	std::uint32_t indexed_notes = 0;
	std::uint32_t indexed_load = 0;
	std::string ip; // dotted-quad
	// Buddy is the LowID-buddy state (for NAT-T peers). Most users see
	// "no_buddy"; networks behind aggressive NAT see "connected".
	std::string buddy_status; // "no_buddy" | "connecting" | "connected"
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
// `extra` holds the optional nested sub-value (the parenthetical "(total …)"
// some nodes carry) — at most one level deep, matching the EC encoding.
struct StatsTreeValue
{
	enum Kind
	{
		Num, // integer/istring/bytes/ishort/time/speed -> num (raw seconds/bytes/…)
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
	// value is not a sentinel; surfaced as an additive "enum" field so
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
	// Sample cadence in seconds. amuled's CStatsCollection uses 1s.
	// Used as the `interval` field in the response.
	std::uint32_t interval_seconds = 1;

	std::vector<std::uint32_t> download_bps;
	std::vector<std::uint32_t> upload_bps;
	std::vector<std::uint32_t> connections;
	std::vector<std::uint32_t> kad_nodes;

	// Session running totals — single uint64 per series, reported
	// alongside the time-series so the panel can show "this session
	// total" without needing a separate roundtrip.
	std::uint64_t session_download_bytes = 0;
	std::uint64_t session_upload_bytes = 0;
	std::uint64_t session_kad_bytes = 0;
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
	bool already_have = false;
	std::uint8_t rating = 0;
	// Download status of the result on this node (issue #429), a lowercase
	// string from the CSearchFile enum: "new" | "downloaded" | "queued" |
	// "canceled" | "queued_canceled".
	std::string status;
	// File-type token derived from the filename (like the shared-detail
	// `file_type`), e.g. "video"/"audio"; "" if the name has no extension.
	std::string type;
	// Audio/video media metadata (issue #430), same shape as the file
	// detail endpoints' `media` object. `has_media` gates it — omitted
	// when the hit carries no FT_MEDIA_* tags (most remote results).
	bool has_media = false;
	struct Media
	{
		std::uint32_t length_s = 0;
		std::uint32_t bitrate = 0;
		std::string codec;
		std::string artist;
		std::string album;
		std::string title;
	} media;
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

// /logs/serverinfo. amule has no incremental EC op for this log
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
	std::string host_name;
	bool check_new_version = false;
	// Capability: the connected daemon is built with ENABLE_VERSION_CHECK
	// (emits EC_TAG_GENERAL_VERSION_CHECK_AVAILABLE). False for OS-package
	// or pre-3.1 daemons; combined with check_new_version to decide whether
	// update checking is active. See /version's "update" object.
	bool version_check_available = false;

	// [Connection]
	std::uint32_t max_upload_kbps = 0;
	std::uint32_t max_download_kbps = 0;
	std::uint32_t max_upload_cap_kbps = 0;
	std::uint32_t max_download_cap_kbps = 0;
	std::uint32_t slot_allocation = 0;
	std::uint16_t tcp_port = 0;
	std::uint16_t udp_port = 0;
	bool udp_disabled = false;
	std::uint32_t max_sources_per_file = 0;
	std::uint32_t max_connections = 0;
	bool autoconnect = false;
	bool reconnect = false;
	bool network_ed2k = false;
	bool network_kad = false;
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

	// True when the daemon is connected to ed2k but in LowID mode
	// (NAT'd, can't accept incoming). adds NAT-T affordances
	// that change this calculus; until then the field maps 1:1 to the
	// EC CONNSTATE bit.
	bool ed2k_lowid = false;
	// True when Kad is running but firewalled.
	bool kad_firewalled = false;

	// Bytes per second (NOT kB) so the field name matches the wire
	// units throughout. Clients that want kB/s do the divide.
	std::uint64_t download_bps = 0;
	std::uint64_t upload_bps = 0;

	// Aggregate counts pulled by the same EC_OP_STATS round-trip.
	std::uint32_t ul_queue_len = 0;
	std::uint32_t total_src_count = 0;

	// ed2k network-wide totals (all connected servers). Surfaced in
	// /status as ed2k.network.{users,files} — symmetric with
	// kad.network.{users,files,nodes} on KadSnapshot. Populated from
	// EC_TAG_STATS_ED2K_{USERS,FILES}, present in the same
	// EC_OP_STAT_REQ response we already parse.
	std::uint32_t ed2k_users = 0;
	std::uint32_t ed2k_files = 0;

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
//  * 32-char hex MD4 hash → ECID via FindEcidByHash (index[]).
//
// Walkers reach in via find()/emplace()/erase()/begin()/end() — the
// same surface they had when this was a raw std::map<uint32_t,
// FileSnapshot>&. The wrapper intercepts the two mutations that move
// hashes around and keeps the index consistent.
//
// Invariant: a FileSnapshot's `hash` is content-derived and never
// changes once set. Walkers MUST NOT reassign `hash` via the iterator
// (the index would desync). Set hash before emplace, never after.
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
		auto r = m_files.emplace(ecid, std::move(f));
		if (r.second && !r.first->second.hash.empty()) {
			m_hash_to_ecid[r.first->second.hash] = ecid;
		}
		return r;
	}

	iterator erase(iterator it)
	{
		if (!it->second.hash.empty()) {
			auto hit = m_hash_to_ecid.find(it->second.hash);
			// Defence: only clear the index slot if it still points at
			// this ECID. A later emplace with the same hash but a
			// different ECID could have rewired the slot already.
			if (hit != m_hash_to_ecid.end() && hit->second == it->first) {
				m_hash_to_ecid.erase(hit);
			}
		}
		return m_files.erase(it);
	}

	void clear()
	{
		m_files.clear();
		m_hash_to_ecid.clear();
	}

	bool FindEcidByHash(const std::string &hash, std::uint32_t &out) const
	{
		auto it = m_hash_to_ecid.find(hash);
		if (it == m_hash_to_ecid.end())
			return false;
		out = it->second;
		return true;
	}

private:
	map_type m_files;
	std::unordered_map<std::string, std::uint32_t> m_hash_to_ecid;
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
	ServerInfoLog ServerInfo() const;

	// Flat list views. Reads the ECID-keyed map under shared_lock and
	// returns a copy of the snapshot values in unordered_map iteration
	// order — bucket-dependent, NOT stable across ticks. Consumers
	// that want a specific order sort on their side (by name / date /
	// progress / etc.).
	//
	// `Downloads()` filters `m_files` by `is_downloading`, `Shared()`
	// filters by `is_shared`. Both views consult the same underlying
	// unified map — see FileSnapshot above for the role-flag model.
	std::vector<FileSnapshot> Downloads() const;
	std::vector<FileSnapshot> Shared() const;
	// Unfiltered view used by EventDiff to compute role-flag
	// transitions. Not surfaced on the REST API.
	std::vector<FileSnapshot> Files() const;

	// Full peer list (all upload_state values, including queue
	// waiters, idle peers, and banned). Backs /clients.
	// The legacy /uploads endpoint is retired — consumers query
	// /clients and filter by role on their side.
	std::vector<ClientSnapshot> Clients() const;

	std::vector<ServerSnapshot> Servers() const;
	std::vector<SearchResult> Search() const;
	SearchProgressSnapshot SearchProgress() const;

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
	void MutateClients(const std::function<void(std::map<std::uint32_t, ClientSnapshot> &)> &fn);
	void MutateServers(const std::function<void(std::map<std::uint32_t, ServerSnapshot> &)> &fn);
	void MutateSearch(const std::function<void(std::map<std::uint32_t, SearchResult> &)> &fn);

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
	// Called by POST /search. Wipes m_search, sets m_search_progress
	// to active=true with the requested `kind`. The refresher takes
	// over from there, mapping EC_TAG_SEARCH_LIFECYCLE_STATE into
	// `complete` / `active` on each tick.
	void MarkSearchStarted(const std::string &kind);
	// Refresher-side write path for the search progress snapshot.
	void WriteSearchProgress(SearchProgressSnapshot s);
	void WriteStatsTree(StatsTreeNode t);
	void WriteGraphs(StatsGraphs g);
	void MarkTickSuccess();
	void MarkTickFailure();

private:
	mutable std::shared_timed_mutex m_mu;

	bool m_has_first_snapshot = false;
	bool m_ec_connected = false;
	std::time_t m_snapshot_at = 0;

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
	std::map<std::uint32_t, ServerSnapshot> m_servers;
	std::vector<std::string> m_amule_log_lines;
	ServerInfoLog m_server_info;
	StatsTreeNode m_stats_tree;
	StatsGraphs m_graphs;
	std::map<std::uint32_t, SearchResult> m_search;
	SearchProgressSnapshot m_search_progress;
};

} // namespace webapi

#endif // WEBAPI_STATE_H
