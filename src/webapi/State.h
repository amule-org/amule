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
#include <vector>


// Cached snapshot of amuled state, populated by the refresher (wxApp
// thread) and read by the HTTP server (Boost.Asio thread).
//
// **Concurrency model.** A single `std::shared_timed_mutex` guards
// every member field. The refresher takes it exclusive once per tick
// to swap each substruct; HTTP read handlers take it shared, copy
// the relevant substruct out, release immediately, and serialize the
// JSON outside the critical section. Multiple REST clients reading
// `/status` (or any other read endpoint) therefore stack with no
// per-handler bottleneck — the shared-lock contention is bounded by
// the µs-scale memcpy of a small POD substruct. The Refresher's
// exclusive acquisition blocks readers only for the duration of the
// `std::move`-swap, never across the EC roundtrip itself.
//
// Lifetime: one instance lives inside CamuleapiApp for the whole
// process. Refresher writes a complete snapshot under a single
// lock acquisition per tick; HTTP handlers copy the relevant
// substruct out under the same lock and then release it before
// JSON serialisation.
//
// Phase 4a populates only `Status`. Phase 4b+ adds Downloads,
// Uploads, Shared, Servers, Kad, Categories, Logs, Preferences,
// Stats, Search — each as a sibling struct here, each pulled
// during the same tick.

namespace webapi {


// One per partfile in the daemon's download queue. Identity is the
// 32-bit EC `ECID` (`ID()` on the tag), which the refresher uses as
// the map key for INC-mode delta application. `hash` is the
// (lowercase hex) MD4 used as the URL key on /downloads/{hash}.
//
// Field shapes mirror the reference branch's `/api/v0/downloads`
// wire contract minus `progress.parts` (deferred to Phase 4e per
// PLAN §8 — needs amule's stateful `RLE_Data` carried across ticks).
struct DownloadSnapshot {
	std::uint32_t ecid             = 0;
	std::string hash;            // 32-char hex MD4
	std::string name;
	std::string ed2k_link;
	std::uint64_t size              = 0;
	std::uint64_t size_done         = 0;   // bytes completed
	std::uint64_t size_xfer         = 0;   // bytes transferred this session
	std::uint32_t speed_bps         = 0;
	std::string   status;        // "downloading" | "paused" | "completed" | "hashing" | "erroneous" | "completing" | "allocating" | "waiting"
	std::string   priority;      // "low" | "normal" | "high" | "release" | "very_low" | "auto"
	bool          priority_auto     = false;
	std::uint32_t category          = 0;
	double        percent           = 0.0;
	// Source counts.
	std::uint32_t sources_total        = 0;
	std::uint32_t sources_not_current  = 0;
	std::uint32_t sources_transferring = 0;
	std::uint32_t sources_a4af         = 0;

	// Decoded per-part state, populated by the refresher's RLE
	// decoder pass on EC_TAG_PARTFILE_GAP_STATUS +
	// EC_TAG_PARTFILE_PART_STATUS. Both arrays are sized to
	// ceil(size / PARTSIZE) once a successful decode has landed;
	// the list endpoint omits them, the detail endpoint emits
	// `progress.parts: [{state, sources}, ...]` by walking them
	// in parallel.
	//
	// `gaps` are flat (start_byte, end_byte) uint64 pairs marking
	// byte-ranges still missing from the partfile (amule's
	// `CGapList::Encode/Decode` semantics). `part_sources` is one
	// uint16 per part counting how many sources currently have it.
	std::vector<std::uint64_t> decoded_gaps;
	std::vector<std::uint16_t> decoded_part_sources;
};


// One per peer (CUpDownClient) in the daemon's active client list.
// Populated from the EC_TAG_CLIENT subtree inside the GET_UPDATE
// response (Phase 4g consolidation — see RefresherTick.cpp).
//
// "Client" here is amule's bidirectional peer: a remote ed2k peer
// that's connected to us in EITHER role — uploader (we are downloading
// from them), uploadee (we are uploading to them), queue waiter,
// banned, etc. The cache holds ALL of them; consumer endpoints filter
// by role:
//   * /uploads → filter by upload_state == US_UPLOADING
//   * /clients → no filter, full set surfaced
// (Phase 5+ /downloads/{hash}/sources can filter by
//  requested_file_hash matching the partfile.)
struct ClientSnapshot {
	std::uint32_t ecid                  = 0;
	std::string   client_name;
	std::string   user_hash;           // peer's user hash (32-char lowercase hex MD4)
	std::string   ip;                  // dotted-quad
	std::uint16_t port                 = 0;

	// Software identity. EC_TAG_CLIENT_SOFTWARE ships a numeric code
	// (SO_AMULE / SO_EMULE / etc); we decode it server-side into a
	// short label here so consumers don't need the lookup table.
	std::string   software;            // "amule" | "emule" | "edonkey" | "mldonkey" | ...
	std::string   software_version;    // free-form string from EC_TAG_CLIENT_SOFT_VER_STR
	std::string   os_info;             // free-form (CLIENT_OS_INFO)

	// State machine values. We decode the raw US_*/DS_*/IS_* ints
	// into wire strings so consumers don't reach into amule's enums.
	std::string   upload_state;        // "uploading" | "queued" | "banned" | "connecting" | "idle" | ...
	std::string   download_state;      // "downloading" | "onqueue" | "noneededparts" | ... | "idle"
	std::string   ident_state;         // "unknown" | "identified" | "bad_guy" | ...

	// File context — different per direction.
	//   * requested_file_hash: the partfile this peer is downloading
	//     FROM us (the upload-side identity). Empty when not
	//     uploading.
	//   * downloading_file_hash: the file we're downloading FROM this
	//     peer. Empty when not in download role.
	std::string   requested_file_hash;     // CLIENT_UPLOAD_FILE (ECID-resolved hash, hex)
	std::string   requested_file_name;     // PARTFILE_NAME of the requested file
	std::string   downloading_file_hash;   // CLIENT_REQUEST_FILE (ECID-resolved)

	// Per-session transfer stats. CLIENT_UPLOAD_SESSION = bytes
	// uploaded TO this peer; PARTFILE_SIZE_XFER (when re-keyed on a
	// CLIENT_* tag) = bytes downloaded FROM this peer.
	std::uint64_t xfer_up_session       = 0;
	std::uint64_t xfer_down_session     = 0;
	std::uint64_t xfer_up_total         = 0;
	std::uint64_t xfer_down_total       = 0;
	std::uint32_t upload_speed_bps      = 0;
	std::uint32_t download_speed_bps    = 0;

	// Upload queue position (for peers in US_ONUPLOADQUEUE).
	// 0 when not queued.
	std::uint32_t queue_waiting_position = 0;
	// Remote queue rank — our position in THE PEER's upload queue
	// (i.e. how many other ed2k clients they're going to upload to
	// before us). 0xFFFF when their queue is full.
	std::uint16_t remote_queue_rank      = 0;

	std::uint32_t score                  = 0;   // EC_TAG_CLIENT_SCORE
	std::string   obfuscation_status;    // "none" | "supported" | "required"
	bool          friend_slot            = false;
};


struct SharedSnapshot {
	std::uint32_t ecid             = 0;
	std::string   hash;
	std::string   name;
	std::string   ed2k_link;
	std::uint64_t size                = 0;
	std::uint64_t xfer_session        = 0;   // bytes uploaded this session
	std::uint64_t xfer_total          = 0;   // bytes uploaded lifetime
	std::uint32_t requests_session    = 0;
	std::uint32_t requests_total      = 0;
	std::uint32_t accepts_session     = 0;
	std::uint32_t accepts_total       = 0;
	std::uint32_t complete_sources    = 0;
	std::string   priority;     // "very_low" | "low" | "normal" | "high" | "release" | "auto"
};


// One per eD2k server in the configured server list. Identity is
// the EC ECID (stable per amuled process lifetime). Servers are
// fetched at full-state per refresher tick (`EC_OP_GET_SERVER_LIST`
// has no two-phase INC equivalent — see `ExternalConn.cpp:2023`),
// so the refresher rebuilds the whole map each cycle.
struct ServerSnapshot {
	std::uint32_t ecid              = 0;
	std::string   name;
	std::string   description;
	std::string   version;
	std::string   address;           // host:port form (canonical)
	std::uint32_t ip                = 0;    // host-byte-order IPv4
	std::uint16_t port              = 0;
	std::uint32_t ping_ms           = 0;
	std::uint32_t failed            = 0;
	std::uint32_t users             = 0;
	std::uint32_t max_users         = 0;
	std::uint32_t files             = 0;
	std::string   priority;          // "low" | "normal" | "high"
	bool          is_static         = false;
};


// /kad endpoint. Single composite snapshot pulled from the STAT_REQ
// response we're already fetching for /status — saves a roundtrip
// since amuled's `EC_OP_STAT_REQ` at `EC_DETAIL_CMD` ships every
// `EC_TAG_STATS_KAD_*` we want here.
struct KadSnapshot {
	std::string   state;           // "disabled" | "connecting" | "connected"
	bool          firewalled       = false;
	bool          firewalled_udp   = false;
	bool          in_lan_mode      = false;
	std::uint32_t users            = 0;
	std::uint32_t files            = 0;
	std::uint32_t nodes            = 0;
	std::uint32_t indexed_sources  = 0;
	std::uint32_t indexed_keywords = 0;
	std::uint32_t indexed_notes    = 0;
	std::uint32_t indexed_load     = 0;
	std::string   ip;              // dotted-quad
	// Buddy is the LowID-buddy state (for NAT-T peers). Most users see
	// "no_buddy"; networks behind aggressive NAT see "connected".
	std::string   buddy_status;    // "no_buddy" | "connecting" | "connected"
	std::string   buddy_ip;
	std::uint16_t buddy_port       = 0;
};


// One per download category (categories live in amuled's preferences;
// the EC packet bundles them under `EC_PREFS_CATEGORIES`). Index 0
// is the implicit "All" category. Refresher fetches the full set on
// each tick — categories rarely change but the cost is bounded by
// the typical 0-10 entry count.
struct CategorySnapshot {
	std::uint32_t index            = 0;
	std::string   name;
	std::string   path;
	std::string   comment;
	std::uint32_t color            = 0;
	std::uint8_t  priority_code    = 0;
	std::string   priority;        // human-readable (very_low/low/normal/high/release/auto)
};


// One node in the recursive stats tree (amuled's "Statistics" panel
// contents — counters, ratios, uptime, transfer aggregates, etc.).
// amule emits a flat-label representation: `GetDisplayString()`
// returns the rendered text (e.g. "Total bytes transferred: 12.3 GiB")
// rather than a typed value, so we pass it through as a single
// human-readable string and let clients render the tree verbatim.
struct StatsTreeNode {
	std::string label;
	std::vector<StatsTreeNode> children;
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
struct StatsGraphs {
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
	std::uint64_t session_upload_bytes   = 0;
	std::uint64_t session_kad_bytes      = 0;
};


// One result from a /search/results poll. Identity is the file's
// MD4 hash (search results carry it directly). amuled accumulates
// results in its `searchlist` singleton as search packets come in
// from servers/Kad; client polls EC_OP_SEARCH_RESULTS to drain.
//
// Phase 5 will add POST `/search` to start a query; Phase 4d ships
// the read endpoint so a client can poll an already-running search.
struct SearchResult {
	std::uint32_t ecid                  = 0;
	std::string   hash;             // 32-char hex MD4
	std::string   name;
	std::uint64_t size                  = 0;
	std::uint32_t source_count          = 0;
	std::uint32_t complete_source_count = 0;
	bool          already_have          = false;
	std::uint8_t  rating                = 0;
};


// `m_amule_log_lines` in CState holds the cached log surfaced via
// /logs/amule. amule's EC server piggybacks new log lines on
// STAT_REQ responses at `EC_DETAIL_FULL` (see `AddLoggerTag` in
// ExternalConn.cpp:700-715), using a per-EC-connection cursor
// (CLoggerAccess) so each call returns ONLY the lines emitted since
// the previous STAT_REQ from the same connection. The refresher
// appends those new lines to the vector; clients tail with
// `?tail=N`.
//
// **No cap on history.** Per operator preference, every line amuled
// ships stays in memory until amuleapi restarts. amule's typical
// log volume is bounded by operator habits (idle: ~tens of KB/day;
// busy: ~hundreds of KB/day). A future `DELETE /logs/amule`
// (Phase 5 mutation) will let operators truncate when needed; until
// then the vector grows monotonically.


// /logs/serverinfo. amule has no incremental EC op for this log
// (no equivalent of CLoggerAccess for ServerInfoLog), so the
// refresher fetches the entire string via EC_OP_GET_SERVERINFO each
// tick and the cache stores the latest snapshot. Server-info logs
// are small (a few KB at most — just server connection chatter), so
// the per-tick rebuild cost is negligible.
struct ServerInfoLog {
	std::string text;
};


// amuled preferences subset surfaced via /preferences. The amuled
// preferences corpus is enormous (every UI panel has its own
// section); for v0.1 we ship the common-case fields:
// nick, transfer limits, ports, connection toggles. Phase 4d / later
// can extend this if a real client reports needing more.
struct PreferencesSnapshot {
	// [General]
	std::string   nickname;
	std::string   user_hash;
	std::string   host_name;
	bool          check_new_version = false;

	// [Connection]
	std::uint32_t max_upload_kbps        = 0;
	std::uint32_t max_download_kbps      = 0;
	std::uint32_t max_upload_cap_kbps    = 0;
	std::uint32_t max_download_cap_kbps  = 0;
	std::uint32_t slot_allocation        = 0;
	std::uint16_t tcp_port               = 0;
	std::uint16_t udp_port               = 0;
	bool          udp_disabled           = false;
	std::uint32_t max_sources_per_file   = 0;
	std::uint32_t max_connections        = 0;
	bool          autoconnect            = false;
	bool          reconnect              = false;
	bool          network_ed2k           = false;
	bool          network_kad            = false;
};


struct StatusSnapshot {
	// "connected" / "connecting" / "disconnected" — the literal
	// string the API returns. Done at parse time rather than
	// emit time so the snapshot is self-describing (debug-dump-
	// friendly) and the emit path stays one-liner trivial.
	std::string ed2k_state    = "disconnected";
	std::string kad_state     = "disabled";

	// Nickname is intentionally NOT a /status field — it lives in the
	// preferences EC namespace, not the STAT_REQ response. amuleapi
	// surfaces it via /api/v0/preferences (Phase 4c) where it belongs
	// semantically (it's a user-edited value, not a connection-state
	// observation). Same call /status that PHP's am_status template
	// makes.

	// Server the daemon is currently connected to (eD2k only — Kad
	// has no equivalent). Empty when ed2k_state != "connected".
	std::string server_name;
	std::string server_ip;
	std::uint32_t server_port = 0;

	// True when the daemon is connected to ed2k but in LowID mode
	// (NAT'd, can't accept incoming). Phase 7+ adds NAT-T affordances
	// that change this calculus; until then the field maps 1:1 to the
	// EC CONNSTATE bit.
	bool        ed2k_lowid    = false;
	// True when Kad is running but firewalled.
	bool        kad_firewalled = false;

	// Bytes per second (NOT kB) so the field name matches the wire
	// units throughout. Clients that want kB/s do the divide.
	std::uint64_t download_bps = 0;
	std::uint64_t upload_bps   = 0;

	// Aggregate counts pulled by the same EC_OP_STATS round-trip.
	std::uint32_t ul_queue_len     = 0;
	std::uint32_t total_src_count  = 0;
};


// One State instance per amuleapi process. The mutex protects every
// member field; refresh swaps the whole struct under it, handlers
// read the substructs they need under it.
class CState {
public:
	// True once the refresher has completed at least one successful
	// tick. Until then, the /status endpoint returns 503 with
	// `ec_unavailable` so clients can tell "amuleapi is up but amuled
	// isn't responding" apart from a hard 5xx.
	bool             HasFirstSnapshot() const;

	// Wall-clock at which the last successful tick completed. Used
	// to populate `snapshot_at` / `snapshot_at_unix` on every list
	// response.
	std::time_t      SnapshotAt() const;

	// True iff the most recent tick succeeded. False after a tick
	// failed (EC timeout / disconnect); the refresher keeps the
	// stale snapshot for clients but flips this flag.
	bool             EcConnected() const;

	StatusSnapshot                  Status()      const;
	KadSnapshot                     Kad()         const;
	PreferencesSnapshot             Preferences() const;
	// Full snapshot of the amule log lines (oldest-first). API
	// handlers slice the tail before serialising via the
	// `?tail=N` query param.
	std::vector<std::string>        AmuleLog()    const;
	ServerInfoLog                   ServerInfo()  const;

	// Flat list views. Reads the ECID-keyed map under shared_lock and
	// returns a copy of the snapshot values, ordered by ECID.
	// Insertion order isn't preserved by std::map, but ECID ordering
	// is deterministic per-client-session and matches the EC server's
	// own iteration order, so the API surface stays consistent across
	// refresher ticks.
	std::vector<DownloadSnapshot> Downloads() const;

	// Full peer list (all upload_state values, including queue
	// waiters, idle peers, and banned). Backs /clients (Phase 4g).
	// The legacy /uploads endpoint is retired — consumers query
	// /clients and filter by role on their side.
	std::vector<ClientSnapshot>   Clients()   const;

	std::vector<SharedSnapshot>   Shared()    const;
	std::vector<ServerSnapshot>   Servers()   const;
	std::vector<SearchResult>     Search()    const;

	// Categories aren't ECID-keyed (they come in via the
	// preferences packet as an indexed array); keep them as a plain
	// vector copied out under the shared lock.
	std::vector<CategorySnapshot> Categories() const;

	// /stats/tree returns the recursive tree as a single bare object.
	StatsTreeNode                 StatsTree()  const;
	// /stats/graphs/{graph} reads one series out of the bundle.
	StatsGraphs                   Graphs()     const;

	// Look up a single download by 32-char hex hash, copied out under
	// the shared lock. Returns true on hit, false on miss; on miss
	// `out` is left untouched. Used by /downloads/{hash}.
	bool             FindDownload(const std::string &hash_hex,
	                              DownloadSnapshot &out) const;

	// INC-mode delta application. The refresher takes the unique_lock
	// once per substruct's EC roundtrip, then calls a callback with a
	// mutable reference to the ECID-keyed map; the callback walks the
	// EC response and upserts/removes individual entries. One unique
	// acquisition per tick rather than N — reader latency stays
	// bounded by the parse loop, not by N independent acquisitions.
	using DownloadMutator = void (*)(std::map<std::uint32_t, DownloadSnapshot> &);
	using ClientMutator   = void (*)(std::map<std::uint32_t, ClientSnapshot>   &);
	using SharedMutator   = void (*)(std::map<std::uint32_t, SharedSnapshot>   &);
	using SearchMutator   = void (*)(std::map<std::uint32_t, SearchResult>     &);
	// Mutator versions taking a non-capturing std::function so callers
	// can close over the EC packet without templating CState.
	void             MutateDownloads(const std::function<
	                  void(std::map<std::uint32_t, DownloadSnapshot> &)> &fn);
	void             MutateClients  (const std::function<
	                  void(std::map<std::uint32_t, ClientSnapshot>   &)> &fn);
	void             MutateShared   (const std::function<
	                  void(std::map<std::uint32_t, SharedSnapshot>   &)> &fn);
	void             MutateServers  (const std::function<
	                  void(std::map<std::uint32_t, ServerSnapshot>   &)> &fn);
	void             MutateSearch   (const std::function<
	                  void(std::map<std::uint32_t, SearchResult>     &)> &fn);

	// Wholesale reset paths. Called by the refresher after a
	// MarkTickFailure → MarkTickSuccess transition (the server's
	// CValueMap was reset on reconnect; stale entries that vanished
	// during the disconnect window would otherwise live forever in
	// the cache).
	void             ResetLists();

	// Refresher-side write paths.
	void             WriteStatus(StatusSnapshot s);
	void             WriteKad(KadSnapshot k);
	void             WritePreferences(PreferencesSnapshot p);
	void             WriteCategories(std::vector<CategorySnapshot> c);
	// Append one or more new amule-log lines to the ring; trims oldest
	// entries when capacity is exceeded. Called once per refresher
	// tick with the lines drained from EC_TAG_STATS_LOGGER_MESSAGE.
	void             AppendAmuleLog(std::vector<std::string> new_lines);
	// Drop every cached amule-log line. Called by DELETE /logs/amule
	// after the EC_OP_RESET_LOG roundtrip — the refresher only appends
	// (it has no equivalent of "shrink to amuled's current count"), so
	// the in-process cache MUST be cleared explicitly or the next GET
	// will keep returning the pre-reset lines. The next refresher tick
	// resumes appending from amuled's now-empty buffer.
	void             ClearAmuleLog();
	void             WriteServerInfo(ServerInfoLog s);
	void             WriteStatsTree(StatsTreeNode t);
	void             WriteGraphs(StatsGraphs g);
	void             MarkTickSuccess();
	void             MarkTickFailure();

private:
	mutable std::shared_timed_mutex m_mu;

	bool             m_has_first_snapshot = false;
	bool             m_ec_connected       = false;
	std::time_t      m_snapshot_at        = 0;

	StatusSnapshot                                  m_status;
	KadSnapshot                                     m_kad;
	PreferencesSnapshot                             m_preferences;
	std::vector<CategorySnapshot>                   m_categories;
	std::map<std::uint32_t, DownloadSnapshot>       m_downloads;
	std::map<std::uint32_t, ClientSnapshot>         m_clients;
	std::map<std::uint32_t, SharedSnapshot>         m_shared;
	std::map<std::uint32_t, ServerSnapshot>         m_servers;
	std::vector<std::string>                        m_amule_log_lines;
	ServerInfoLog                                   m_server_info;
	StatsTreeNode                                   m_stats_tree;
	StatsGraphs                                     m_graphs;
	std::map<std::uint32_t, SearchResult>           m_search;
};


}  // namespace webapi

#endif // WEBAPI_STATE_H
