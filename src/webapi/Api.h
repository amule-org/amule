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

#ifndef WEBAPI_API_H
#define WEBAPI_API_H

#include <memory>
#include <string>

#include "Auth.h"
#include "HttpServer.h"
#include "Jwt.h"   // CJwt::VerifyResult, by value in AuthOutcome
#include "State.h" // ServerInfoLog / StatsTreeNode / StatsGraphs / SearchResult
#include "TtlCache.h"

#include <ec/cpp/ECCodes.h> // ec_opcode_t, for the shared search-op sender

#include <ctime>
#include <map>
#include <utility>

class CECTag;
class CAmuleApiConfig;
class CamuleapiApp;
class CJsonWriter;

// Result of authenticating one request: either `ok` with the verified
// claims, or a ready-to-send rejection.
struct AuthOutcome
{
	bool ok = false;
	CHttpServer::Response rejection;
	CJwt::VerifyResult verified;
};

namespace webapi
{
class CState;
}

// Request dispatcher for the `/api/v0/*` surface. Lives between the transport
// (CHttpServer) and the per-endpoint handlers. Owns the CJwt instance, the
// revocation set, and the rate limiter.
//
// Config + jwt are held by reference: the App constructs them once at startup
// and outlives every Request.

class CApiDispatcher
{
public:
	CApiDispatcher(CAmuleApiConfig &config, CJwt &jwt, webapi::CState &state, CamuleapiApp &app);

	CHttpServer::Response Dispatch(const CHttpServer::Request &req);

	// Streaming entry point, called by HttpServer when the streaming_resolver
	// matches `/api/v0/events`. Runs the SSE loop until the writer goes dead
	// (peer disconnect) or it returns voluntarily.
	void DispatchEvents(const CHttpServer::Request &req,
		CHttpServer::Writer &writer,
		unsigned &http_status,
		std::string &content_type,
		std::map<std::string, std::string> &response_headers);

	// Synchronous preflight for the SSE path. Runs on the I/O thread BEFORE the
	// worker thread is spawned and BEFORE the streaming-session budget is
	// touched. boost::none admits; a 401/403/429 short-circuits without a slot.
	boost::optional<CHttpServer::Response> PreflightEvents(const CHttpServer::Request &req);
	// CORS for transport-built replies (408 / 413 / 431); see the .cpp.
	void StampCorsForTransport(
		std::map<std::string, std::string> &headers, const std::string &origin_header);

private:
	// Inner routing -- picks the right Handle*() based on path/method. The public
	// Dispatch wraps this with the ETag stamp and the If-None-Match -> 304
	// conversion (GET/HEAD on a 200 only).
	CHttpServer::Response DispatchToHandler(const CHttpServer::Request &req);

public:
	// Test-visible accessors: the auth-state containers are exposed so AuthTest
	// can drive the rate-limit and revocation paths without an HTTP server.
	webapi::CRevocationSet &Revocations() { return m_revocations; }
	webapi::CRateLimiter &RateLimiter() { return m_rateLimiter; }

private:
	CHttpServer::Response HandleVersion(const CHttpServer::Request &);
	CHttpServer::Response HandleVersionCheck(const CHttpServer::Request &);
	CHttpServer::Response HandleLogin(const CHttpServer::Request &);
	CHttpServer::Response HandleLogout(const CHttpServer::Request &);
	CHttpServer::Response HandleSession(const CHttpServer::Request &);
	CHttpServer::Response HandleAuthPasswords(const CHttpServer::Request &);
	CHttpServer::Response HandleAuthPasswordsPatch(const CHttpServer::Request &);

	// The one way every protected handler authenticates: bearer/cookie lookup,
	// signature and expiry, per-IP failure limiter, revocation list, and the
	// rule that a token older than the last credential change is dead.
	AuthOutcome Authenticate(const CHttpServer::Request &req);

	void BeginSession(
		const CHttpServer::Request &req, Role role, CHttpServer::Response &r, CJsonWriter &w);
	CHttpServer::Response HandleStatus(const CHttpServer::Request &);
	CHttpServer::Response HandleDownloads(const CHttpServer::Request &);
	// `key` accepts the lowercase 32-char hex hash OR the decimal ECID.
	CHttpServer::Response HandleDownloadDetail(const CHttpServer::Request &, const std::string &key);
	// per-source comments/ratings list. `key` = 32-char MD4 hash.
	CHttpServer::Response HandleDownloadComments(const CHttpServer::Request &, const std::string &key);
	CHttpServer::Response HandleDownloadCommentsKadSearch(
		const CHttpServer::Request &, const std::string &key);
	// source-reported filenames + counts. `key` = 32-char MD4 hash.
	CHttpServer::Response HandleDownloadFilenames(const CHttpServer::Request &, const std::string &key);
	// A4AF swap action (POST). `key` = 32-char MD4 hash; the source rows come
	// from GET /downloads/{hash}/clients, which flags each one with `a4af`.
	CHttpServer::Response HandleDownloadA4afAction(const CHttpServer::Request &, const std::string &key);
	// download lifecycle mutations.
	CHttpServer::Response HandleDownloadAdd(const CHttpServer::Request &);
	CHttpServer::Response HandleDownloadPatch(const CHttpServer::Request &, const std::string &key);
	// clear completed downloads.
	CHttpServer::Response HandleDownloadDelete(const CHttpServer::Request &, const std::string &key);
	CHttpServer::Response HandleDownloadsClearCompleted(const CHttpServer::Request &);
	// bulk mutations over a `hashes` array, per-item `results` envelope (#358).
	CHttpServer::Response HandleDownloadsBulkPatch(const CHttpServer::Request &);
	CHttpServer::Response HandleDownloadsBulkDelete(const CHttpServer::Request &);
	// server lifecycle.
	CHttpServer::Response HandleServerAdd(const CHttpServer::Request &);
	CHttpServer::Response HandleFriends(const CHttpServer::Request &);
	CHttpServer::Response HandleFriendAdd(const CHttpServer::Request &);
	CHttpServer::Response HandleFriendRemove(const CHttpServer::Request &, const std::string &ecid_str);
	CHttpServer::Response HandleFriendPatch(const CHttpServer::Request &, const std::string &ecid_str);
	CHttpServer::Response HandleFriendBrowse(const CHttpServer::Request &, const std::string &ecid_str);
	CHttpServer::Response HandleServerConnect(const CHttpServer::Request &, const std::string &ecid_str);
	CHttpServer::Response HandleServerDelete(const CHttpServer::Request &, const std::string &ecid_str);
	CHttpServer::Response HandleServerPatch(const CHttpServer::Request &, const std::string &ecid_str);
	// Refresh the server list from a `server.met` URL -- the same EC op the
	// desktop GUI's "Update from URL" button uses.
	CHttpServer::Response HandleServerUpdateFromUrl(const CHttpServer::Request &);
	// Address-keyed aliases that resolve {ip}:{port} to the ECID and delegate to
	// HandleServerConnect / HandleServerDelete, so clients need not GET /servers
	// first to learn the ECID for a known address.
	CHttpServer::Response HandleServerConnectByAddress(
		const CHttpServer::Request &, const std::string &ip_port);
	CHttpServer::Response HandleServerPatchByAddress(
		const CHttpServer::Request &, const std::string &ip_port);
	CHttpServer::Response HandleServerDeleteByAddress(
		const CHttpServer::Request &, const std::string &ip_port);
	// preferences PATCH.
	CHttpServer::Response HandlePreferencesPatch(const CHttpServer::Request &);
	// connection control.
	CHttpServer::Response HandleNetworksConnect(const CHttpServer::Request &);
	CHttpServer::Response HandleNetworksDisconnect(const CHttpServer::Request &);
	CHttpServer::Response HandleKadBootstrap(const CHttpServer::Request &);
	CHttpServer::Response HandleKadUpdateFromUrl(const CHttpServer::Request &);
	// IP-filter actions: re-read the on-disk filter files, and download a fresh
	// ipfilter.dat from a URL. The update's URL is optional -- it falls back to
	// the configured security.ipfilter_update_url.
	CHttpServer::Response HandleIpfilterReload(const CHttpServer::Request &);
	CHttpServer::Response HandleIpfilterUpdate(const CHttpServer::Request &);
	// POST /geoip/update -- fetch a fresh GeoIP database now. An action, so it is
	// a route rather than a write-only boolean inside PATCH /preferences.
	CHttpServer::Response HandleGeoipUpdate(const CHttpServer::Request &);
	// single shared-file detail (GET / HEAD). `key` = 32-char MD4 hash.
	CHttpServer::Response HandleSharedDetail(const CHttpServer::Request &, const std::string &key);
	// shared file priority PATCH. `key` = hash OR ECID.
	CHttpServer::Response HandleSharedPatch(const CHttpServer::Request &, const std::string &key);
	// bulk shared-priority PATCH over a `hashes` array (#358).
	CHttpServer::Response HandleSharedBulkPatch(const CHttpServer::Request &);
	// Re-hash a shared file against its on-disk data (EC_OP_VERIFY_LOCAL_DATA).
	// amuled schedules the task and answers immediately, so this is accepted.
	CHttpServer::Response HandleSharedVerify(const CHttpServer::Request &, const std::string &key);
	// The bytes of one completed shared file (GET / HEAD). The only handler that
	// answers with a streamed file window rather than a buffered body: it
	// resolves and containment-checks the path itself. Honours a single byte
	// Range (206), ignores multi-range sets per RFC 7233 3.1, refuses partfiles.
	CHttpServer::Response HandleSharedContent(const CHttpServer::Request &, const std::string &key);

	// Static-frontend fallthrough. Resolves `url_path` under
	// ServerCfg().static_root and returns the file with a content-type derived
	// from its extension. 404 when static serving is disabled, the file is
	// absent, or the resolved path escapes static_root (realpath containment).
	// Falls back to index.html for extension-less paths so SPA deep links work.
	// Never requires auth -- the shell is public, its API calls still gated.
	CHttpServer::Response ServeStaticFile(const CHttpServer::Request &, const std::string &url_path);
	// Country-flag artwork for `country_code`. `url_path` must be
	// "/flags/<cc>.png" with two lowercase ASCII letters, or
	// "/flags/unknown.png" for the "??" placeholder; the bytes come from the
	// embedded icon table's "flag_<cc>" entry, so nothing touches the file
	// system. 404 for any other shape and for codes the famfamfam set has no
	// artwork for. Never requires auth -- the artwork is public either way.
	CHttpServer::Response ServeCountryFlag(const CHttpServer::Request &, const std::string &url_path);
	// Rescan shared directories -- amuled re-walks the configured share roots.
	// Parameterless EC op (EC_OP_SHAREDFILES_RELOAD).
	CHttpServer::Response HandleSharedReload(const CHttpServer::Request &);

	// Re-extract media metadata: the whole share, or one file by hash. Both
	// answer 202 -- the probes are queued and the response says how many.
	CHttpServer::Response HandleSharedMediaRefresh(const CHttpServer::Request &);
	CHttpServer::Response HandleSharedMediaRefreshOne(
		const CHttpServer::Request &, const std::string &hash);
	CHttpServer::Response HandleSharedDirectories(const CHttpServer::Request &);
	CHttpServer::Response HandleSharedDirectoriesPut(const CHttpServer::Request &);
	CHttpServer::Response HandleSharedDirectoriesAdd(const CHttpServer::Request &);
	CHttpServer::Response HandleSharedDirectoriesDelete(const CHttpServer::Request &);
	// categories CRUD.
	CHttpServer::Response HandleCategoryCreate(const CHttpServer::Request &);
	CHttpServer::Response HandleHealth(const CHttpServer::Request &req);
	CHttpServer::Response HandleCategoryOne(
		const CHttpServer::Request &req, const std::string &index_str);
	CHttpServer::Response HandleCategoryUpdate(
		const CHttpServer::Request &, const std::string &index_str);
	CHttpServer::Response HandleCategoryDelete(
		const CHttpServer::Request &, const std::string &index_str);
	// search.
	// Cache miss on a search_id: ask the core once (EC_OP_SEARCH_LIST) whether
	// it holds this id anyway -- a search another client, or the monolithic GUI,
	// started. Seeds the local slot and returns true when found. Shared by every
	// endpoint that addresses a search by id, so none of them can 404 on an id
	// GET /api/v0/search just enumerated. Deliberately a one-off roundtrip on
	// the miss, not a per-tick poll: discovery is rare.
	bool DiscoverSearchIfHeldByCore(std::uint32_t search_id);
	// Resolve a path-supplied search_id to a live slot, seeding it from the core
	// on a cache miss. Every search-scoped handler starts with this, so they
	// cannot disagree on what a 404 means.
	std::unique_ptr<CHttpServer::Response> RequireSearch(std::uint32_t search_id);
	// Refresh one search's cached results if it is finished and its last fetch
	// has aged out. The tick only polls ACTIVE searches, so without this a Kad
	// notes lookup started on one of its hits would never report back, and a hit
	// downloaded from it would keep claiming `status: "new"`.
	void RefreshSearchIfStale(std::uint32_t search_id);
	CHttpServer::Response HandleSearchList(const CHttpServer::Request &);
	CHttpServer::Response HandleSearchStart(const CHttpServer::Request &);
	CHttpServer::Response HandleSearchStop(const CHttpServer::Request &, std::uint32_t search_id);
	// DELETE /search/{id}: stop it AND free it, daemon-side and locally.
	CHttpServer::Response HandleSearchClose(const CHttpServer::Request &, std::uint32_t search_id);
	// POST /search/{id}/more: the desktop "More" button, Kad-only.
	CHttpServer::Response HandleSearchMore(const CHttpServer::Request &, std::uint32_t search_id);
	// One addressed EC exchange for stop / close / more: they differ only in
	// opcode, the close flag and the success status. `out_more_reaskable`, when
	// given, receives the EC_TAG_SEARCH_MORE_REASKABLE bit as 1 or 0, or stays
	// -1 when the daemon sent no such tag -- which is what an older daemon looks
	// like and must never be read as "exhausted".
	CHttpServer::Response SendSearchOp(ec_opcode_t opcode,
		std::uint32_t search_id,
		bool close,
		int success_status,
		int *out_more_reaskable = nullptr);
	CHttpServer::Response HandleSearchDownload(const CHttpServer::Request &, const std::string &hash);
	CHttpServer::Response HandleSearchComments(const CHttpServer::Request &, const std::string &hash);
	CHttpServer::Response HandleSearchCommentsKadSearch(
		const CHttpServer::Request &, const std::string &hash);
	CHttpServer::Response HandleClients(const CHttpServer::Request &);
	// Both per-file peer routes; `require_downloading` picks which collection
	// the hash must belong to (downloads vs shared).
	CHttpServer::Response HandleFileClients(
		const CHttpServer::Request &, const std::string &key, bool require_downloading);
	CHttpServer::Response HandleClientDetail(const CHttpServer::Request &, const std::string &ecid_str);
	CHttpServer::Response HandleKnownClients(const CHttpServer::Request &);

	// --- Chat -------------------------------------------------
	// All six gate on m_app.IsServerChatActive() and answer 503 ec_unsupported
	// otherwise: a daemon predating the chat ops asserts on the unknown opcode
	// rather than answering EC_OP_FAILED, so the request must never go out.
	CHttpServer::Response HandleChats(const CHttpServer::Request &);
	CHttpServer::Response HandleChatMessages(const CHttpServer::Request &, const std::string &peer);
	CHttpServer::Response HandleChatSend(const CHttpServer::Request &, const std::string &peer);
	CHttpServer::Response HandleChatClose(const CHttpServer::Request &, const std::string &peer);
	// Shared body of all three send forms; `target` is the EC tag naming the
	// recipient (GUI_ID, client ECID or friend ECID).
	CHttpServer::Response SendChatMessageTo(const CHttpServer::Request &, const CECTag &target);
	// Address a conversation by friend / peer ECID instead of ip:port. The friend
	// form is the one that reaches an OFFLINE friend, through their stored address.
	CHttpServer::Response HandleFriendMessageSend(const CHttpServer::Request &, const std::string &ecid);
	CHttpServer::Response HandleClientMessageSend(const CHttpServer::Request &, const std::string &ecid);
	// POST /clients/{ecid}/shared_files -- browse a peer's shared file list
	// ("View Files"). Returns a search_id addressed like any search: results via
	// GET /search/{id}/results, progress + SSE via the standard machinery.
	CHttpServer::Response HandleClientBrowse(const CHttpServer::Request &, const std::string &ecid_str);
	// Shared by the /clients and /friends browse routes; `by_friend` selects
	// which sub-tag addresses the target.
	CHttpServer::Response HandleBrowse(
		const CHttpServer::Request &, const std::string &ecid_str, bool by_friend);
	CHttpServer::Response HandleSharedList(const CHttpServer::Request &);
	CHttpServer::Response HandleServers(const CHttpServer::Request &);
	CHttpServer::Response HandleKad(const CHttpServer::Request &);
	CHttpServer::Response HandleCategories(const CHttpServer::Request &);
	CHttpServer::Response HandlePreferences(const CHttpServer::Request &);
	CHttpServer::Response HandleLogAmule(const CHttpServer::Request &);
	CHttpServer::Response HandleLogServerinfo(const CHttpServer::Request &);
	// Log reset mutations. Both clear the corresponding buffer on amuled's side
	// and invalidate amuleapi's local mirror so the next GET reflects the
	// post-reset state: the refresher's append-only path cannot shrink the
	// amule-log cache, and the server-info lazy cache would serve stale text.
	CHttpServer::Response HandleLogAmuleReset(const CHttpServer::Request &);
	CHttpServer::Response HandleLogServerinfoReset(const CHttpServer::Request &);
	CHttpServer::Response HandleStatsTree(const CHttpServer::Request &);
	CHttpServer::Response HandleStatsGraph(const CHttpServer::Request &, const std::string &graph);
	CHttpServer::Response HandleSearchResults(const CHttpServer::Request &, std::uint32_t search_id);

	// Not const: the credential endpoints write through the config, and verifying
	// re-reads amuleapi-passwords (so a change made elsewhere takes effect
	// without a restart) and upgrades a stale-cost record after a match.
	CAmuleApiConfig &m_config;
	CJwt &m_jwt;
	webapi::CState &m_state;
	CamuleapiApp &m_app;
	webapi::CRevocationSet m_revocations;

	// Cached resolution of the static-frontend root. `[Server]/StaticRoot` wins;
	// an empty conf value falls back to the install-path discovery chain.
	// Resolved on first ServeStaticFile call, then memoized for the daemon's
	// lifetime -- editing the conf at runtime needs a restart.
	mutable std::string m_static_root_cache;
	mutable std::once_flag m_static_root_once;

	// ETag memoization keyed on (request target, snapshot version). Every 200
	// GET/HEAD runs MD5 over the whole body for the ETag, and on a 10K-file
	// daemon /downloads is multi-MB, which is the dominant CPU cost of the
	// safe-method path. On overflow the cache is cleared wholesale -- the bound
	// is a memory backstop, not a working-set estimate.
	mutable std::mutex m_etagCacheMu;
	struct EtagCacheEntry
	{
		// Refresh revision, not a timestamp; see CApiDispatcher::Dispatch.
		std::uint64_t snapshot_rev = 0;
		std::string etag;
	};
	std::map<std::string, EtagCacheEntry> m_etagCache;
	static constexpr std::size_t kEtagCacheCapacity = 512;
	// Login-specific failure counter, with tight thresholds from the operator's
	// `[Auth]/Login*` config: humans typing passwords rarely fail >5 times in
	// 60 s, so a tight cap is the right shape for password-guessing defence.
	webapi::CRateLimiter m_rateLimiter;
	// Generic 401 failure counter -- logout, session, events, and every mutation
	// endpoint. Looser than login because a misconfigured CI runner or a tab
	// whose cookie just expired should not lock the user out, but a
	// credential-stuffing attempt burning through stolen bearer tokens DOES
	// need a brake. Default 30 failures in 60 s -> 5 min lockout.
	webapi::CRateLimiter m_authRateLimiter;

	// Lazy-fetch TTL caches. Each stores the snapshot value PLUS the wall-clock
	// time at fetch, so handlers render `snapshot_at` against actual freshness
	// rather than the refresher's tick boundary. The TTL coalesces concurrent
	// burst reads, and the fetcher lambdas acquire m_app.m_ec_mtx AFTER the
	// cache's own mutex -- single flight, so a second miss reads the stored value.
	using TtlPair_StatsTree = std::pair<webapi::StatsTreeNode, std::time_t>;
	using TtlPair_StatsGraphs = std::pair<webapi::StatsGraphs, std::time_t>;
	using TtlPair_ServerInfo = std::pair<webapi::ServerInfoLog, std::time_t>;
	webapi::CTtlCache<TtlPair_StatsTree> m_stats_tree_cache;
	webapi::CTtlCache<TtlPair_StatsGraphs> m_stats_graphs_cache;
	webapi::CTtlCache<TtlPair_ServerInfo> m_server_info_cache;
	// /known_clients is NOT cached here: its store is fetched once on first
	// request and then maintained by the refresher, so there is nothing to
	// expire. /search/results is not either -- the refresher owns the polling
	// while a search is active.
};

#endif // WEBAPI_API_H
