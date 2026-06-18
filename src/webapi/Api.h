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
#include "State.h"        // ServerInfoLog / StatsTreeNode / StatsGraphs / SearchResult
#include "TtlCache.h"     // Phase 4g lazy-fetch coalescer

#include <ctime>
#include <map>
#include <utility>


class CAmuleApiConfig;
class CamuleapiApp;
class CJwt;

namespace webapi { class CState; }


// Request dispatcher for the `/api/v0/*` surface. Lives between the
// transport (CHttpServer) and the per-endpoint handlers. Phase 2
// added /api/v0/version (stateless); Phase 3 wires the auth surface,
// so the dispatcher now owns the CJwt instance, the revocation set,
// and the rate limiter.
//
// References (not copies) of the config + jwt machinery: the App
// constructs them once at startup and outlives every Request.
// CRevocationSet + CRateLimiter live by-value inside the dispatcher
// because they're amuleapi-owned state with no external consumer.

class CApiDispatcher {
public:
	CApiDispatcher(const CAmuleApiConfig &config,
	               CJwt                  &jwt,
	               webapi::CState        &state,
	               CamuleapiApp          &app);

	CHttpServer::Response Dispatch(const CHttpServer::Request &req);

	// Phase 8 streaming entry point. Called by HttpServer when the
	// streaming_resolver matches `/api/v0/events`. The handler runs
	// the SSE loop until the writer goes dead (peer disconnect) or
	// returns voluntarily.
	void DispatchEvents(const CHttpServer::Request &req,
	                    CHttpServer::Writer &writer,
	                    unsigned &http_status,
	                    std::string &content_type,
	                    std::map<std::string, std::string> &response_headers);

private:
	// Inner routing — picks the right Handle*() based on path/method,
	// returns a fully-formed response. The public Dispatch wraps this
	// with the Phase 7 ETag stamp + If-None-Match → 304 conversion
	// (GET/HEAD on a 200 only).
	CHttpServer::Response DispatchToHandler(const CHttpServer::Request &req);
public:

	// Test-visible accessors; the auth-state containers are exposed
	// so AuthTest can drive the rate-limit and revocation paths
	// without standing up a full HTTP server.
	webapi::CRevocationSet &Revocations()  { return m_revocations; }
	webapi::CRateLimiter   &RateLimiter() { return m_rateLimiter; }

private:
	CHttpServer::Response HandleVersion (const CHttpServer::Request &);
	CHttpServer::Response HandleLogin   (const CHttpServer::Request &);
	CHttpServer::Response HandleLogout  (const CHttpServer::Request &);
	CHttpServer::Response HandleSession (const CHttpServer::Request &);
	CHttpServer::Response HandleStatus  (const CHttpServer::Request &);
	CHttpServer::Response HandleDownloads      (const CHttpServer::Request &);
	CHttpServer::Response HandleDownloadDetail (const CHttpServer::Request &,
	                                            const std::string &hash);
	// Phase 5a — download lifecycle mutations.
	CHttpServer::Response HandleDownloadAdd    (const CHttpServer::Request &);
	CHttpServer::Response HandleDownloadPatch  (const CHttpServer::Request &,
	                                            const std::string &hash);
	// Phase 5b — clear completed downloads.
	CHttpServer::Response HandleDownloadDelete (const CHttpServer::Request &,
	                                            const std::string &hash);
	CHttpServer::Response HandleDownloadsClearCompleted(const CHttpServer::Request &);
	// Phase 5c — server lifecycle.
	CHttpServer::Response HandleServerAdd      (const CHttpServer::Request &);
	CHttpServer::Response HandleServerConnect  (const CHttpServer::Request &,
	                                            const std::string &ecid_str);
	CHttpServer::Response HandleServerDelete   (const CHttpServer::Request &,
	                                            const std::string &ecid_str);
	// Refresh the server list from a `server.met` URL — operator-
	// curated server-list update, same EC op the desktop GUI's "Update
	// from URL" button uses.
	CHttpServer::Response HandleServerUpdateFromUrl(const CHttpServer::Request &);
	// Address-keyed aliases that resolve {ip}:{port} to the ECID and
	// delegate to HandleServerConnect / HandleServerDelete. Lets
	// clients work without first having to GET /servers to learn the
	// ECID for a known address.
	CHttpServer::Response HandleServerConnectByAddress(
		const CHttpServer::Request &, const std::string &ip_port);
	CHttpServer::Response HandleServerDeleteByAddress(
		const CHttpServer::Request &, const std::string &ip_port);
	// Phase 5d — preferences PATCH.
	CHttpServer::Response HandlePreferencesPatch(const CHttpServer::Request &);
	// Phase 5e — connection control.
	CHttpServer::Response HandleNetworksConnect (const CHttpServer::Request &);
	CHttpServer::Response HandleNetworksDisconnect(const CHttpServer::Request &);
	CHttpServer::Response HandleKadConnect      (const CHttpServer::Request &);
	CHttpServer::Response HandleKadDisconnect   (const CHttpServer::Request &);
	CHttpServer::Response HandleKadBootstrap    (const CHttpServer::Request &);
	// Phase 5f — shared file priority PATCH.
	CHttpServer::Response HandleSharedPatch     (const CHttpServer::Request &,
	                                             const std::string &hash);
	// Rescan shared directories — amuled re-walks the configured share
	// roots and re-publishes whatever's there. Parameterless EC op
	// (EC_OP_SHAREDFILES_RELOAD).
	CHttpServer::Response HandleSharedReload    (const CHttpServer::Request &);
	// Phase 5g — categories CRUD.
	CHttpServer::Response HandleCategoryCreate  (const CHttpServer::Request &);
	CHttpServer::Response HandleCategoryUpdate  (const CHttpServer::Request &,
	                                             const std::string &index_str);
	CHttpServer::Response HandleCategoryDelete  (const CHttpServer::Request &,
	                                             const std::string &index_str);
	// Phase 6 — search.
	CHttpServer::Response HandleSearchStart    (const CHttpServer::Request &);
	CHttpServer::Response HandleSearchStop     (const CHttpServer::Request &);
	CHttpServer::Response HandleSearchDownload (const CHttpServer::Request &,
	                                            const std::string &hash);
	CHttpServer::Response HandleClients        (const CHttpServer::Request &);
	CHttpServer::Response HandleSharedList     (const CHttpServer::Request &);
	CHttpServer::Response HandleServers        (const CHttpServer::Request &);
	CHttpServer::Response HandleKad            (const CHttpServer::Request &);
	CHttpServer::Response HandleCategories     (const CHttpServer::Request &);
	CHttpServer::Response HandlePreferences    (const CHttpServer::Request &);
	CHttpServer::Response HandleLogAmule       (const CHttpServer::Request &);
	CHttpServer::Response HandleLogServerinfo  (const CHttpServer::Request &);
	// Log reset mutations. Both clear the corresponding buffer on
	// amuled's side via the EC_OP_RESET_LOG / EC_OP_CLEAR_SERVERINFO
	// opcodes and invalidate / clear amuleapi's local mirror so the
	// next GET reflects the post-reset state immediately (the
	// refresher's incremental append-only path can't shrink the
	// amule-log cache, and the server-info lazy cache would otherwise
	// keep serving stale text until its TTL elapses).
	CHttpServer::Response HandleLogAmuleReset      (const CHttpServer::Request &);
	CHttpServer::Response HandleLogServerinfoReset (const CHttpServer::Request &);
	CHttpServer::Response HandleStatsTree      (const CHttpServer::Request &);
	CHttpServer::Response HandleStatsGraph     (const CHttpServer::Request &,
	                                            const std::string &graph);
	CHttpServer::Response HandleSearchResults  (const CHttpServer::Request &);

	const CAmuleApiConfig    &m_config;
	CJwt                     &m_jwt;
	webapi::CState           &m_state;
	CamuleapiApp             &m_app;
	webapi::CRevocationSet    m_revocations;
	webapi::CRateLimiter      m_rateLimiter;

	// Lazy-fetch TTL caches (Phase 4g). Each cache stores the
	// snapshot value PLUS the wall-clock time at fetch so handlers
	// can render `snapshot_at` against the actual freshness, not the
	// refresher's tick boundary. TTL coalesces concurrent burst reads
	// (1 s default; per Phase 4g design call). Fetcher lambdas
	// acquire `m_app.m_ec_mtx` AFTER the cache's own mutex — single
	// flight: a second concurrent miss waits on the cache mutex and
	// reads the just-stored value.
	using TtlPair_StatsTree  = std::pair<webapi::StatsTreeNode,                     std::time_t>;
	using TtlPair_StatsGraphs= std::pair<webapi::StatsGraphs,                       std::time_t>;
	using TtlPair_ServerInfo = std::pair<webapi::ServerInfoLog,                     std::time_t>;
	using TtlPair_Search     = std::pair<std::map<std::uint32_t, webapi::SearchResult>, std::time_t>;
	webapi::CTtlCache<TtlPair_StatsTree>    m_stats_tree_cache;
	webapi::CTtlCache<TtlPair_StatsGraphs>  m_stats_graphs_cache;
	webapi::CTtlCache<TtlPair_ServerInfo>   m_server_info_cache;
	webapi::CTtlCache<TtlPair_Search>       m_search_cache;
};


#endif // WEBAPI_API_H
