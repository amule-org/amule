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

#include "Api.h"

#include "AmuleApiConfig.h"
#include "App.h"          // CamuleapiApp::SendRecvSerialized (Phase 4g lazy-fetch)
#include "Auth.h"
#include "ConstantTime.h"
#include "Etag.h"        // Phase 7 — ETag / If-None-Match
#include "JsonWriter.h"
#include "Jwt.h"
#include "PathPatterns.h"
#include "Refresher.h"    // ParseStatsTreeFromPacket / ParseGraphsFromPacket / ApplySearchFull
#include "State.h"

#include "Constants.h"   // PR_* download priority codes (Phase 5a)

#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECCodes.h>
#include <ec/cpp/ECSpecialTags.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <cctype>

// strncasecmp lives in <strings.h> on POSIX (glibc also exposes it
// via <string.h>, but musl/BSDs don't). Match the shim
// libwebcommon/HeaderParse.cpp ships.
#ifdef _WIN32
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#endif

#define PICOJSON_USE_INT64
#include "picojson.h"

#include "config.h"     // VERSION

#include "Types.h"     // uint8 (required by libs/common/MD5Sum.h)
#include <common/MD5Sum.h>

#include <wx/string.h>

#include <cstdio>
#include <ctime>


namespace {

void SplitPathAndQuery(const std::string &target,
                       std::string &path,
                       std::string &query)
{
	const size_t q = target.find('?');
	if (q == std::string::npos) {
		path  = target;
		query = std::string();
	} else {
		path  = target.substr(0, q);
		query = target.substr(q + 1);
	}
}


CHttpServer::Response ErrorResponse(unsigned status,
                                    const char *code,
                                    const char *message)
{
	CHttpServer::Response r;
	r.status       = status;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("error");
	  w.BeginObject();
	    w.Key("code");    w.ValueString(wxString::FromAscii(code));
	    w.Key("message"); w.ValueString(wxString::FromAscii(message));
	  w.EndObject();
	w.EndObject();
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}


// Common preamble for every auth-protected endpoint. Pulls the JWT
// out of either the Authorization header or the cookie, verifies it,
// rejects revoked tokens, and exposes the resulting VerifyResult.
// Returns 401 on any failure.
//
// Token precedence: Authorization header wins over the cookie when
// both are present. This mirrors the convention browsers and SDKs
// already converge on — a client that explicitly attached a bearer
// header signalled intent that overrides the implicit cookie.
struct AuthOutcome {
	bool                   ok    = false;
	CHttpServer::Response  rejection;
	CJwt::VerifyResult     verified;
};

AuthOutcome AuthenticateRequest(const CHttpServer::Request &req,
                                CJwt                       &jwt,
                                webapi::CRevocationSet     &revocations,
                                const std::string          &cookie_name)
{
	AuthOutcome out;

	std::string token;
	auto auth_it = req.headers.find("Authorization");
	if (auth_it == req.headers.end()) {
		// Case-tolerant fallback: HTTP header names are case-insensitive,
		// but Beast preserves whatever the client sent — so a lowercase
		// `authorization:` from a curl `-H` slips past the literal find.
		// Walk the map once to recover.
		for (const auto &h : req.headers) {
			if (h.first.size() == 13
			    && strncasecmp(h.first.c_str(), "Authorization", 13) == 0) {
				auth_it = req.headers.find(h.first);
				break;
			}
		}
	}
	if (auth_it != req.headers.end()) {
		token = webapi::ExtractBearerToken(auth_it->second);
	}
	if (token.empty()) {
		// No Authorization → fall through to the cookie. Browser-driven
		// session-cookie clients land here; bearer-only API clients
		// already have their token from the header path above.
		auto ck_it = req.headers.find("Cookie");
		if (ck_it == req.headers.end()) {
			for (const auto &h : req.headers) {
				if (h.first.size() == 6
				    && strncasecmp(h.first.c_str(), "Cookie", 6) == 0) {
					ck_it = req.headers.find(h.first);
					break;
				}
			}
		}
		if (ck_it != req.headers.end()) {
			token = webapi::ExtractCookieValue(ck_it->second, cookie_name);
		}
	}
	if (token.empty()) {
		out.rejection = ErrorResponse(401, "unauthorized",
			"missing bearer token or session cookie");
		return out;
	}
	if (!jwt.Verify(token, out.verified)) {
		out.rejection = ErrorResponse(401, "unauthorized",
			"invalid or expired token");
		return out;
	}
	if (revocations.IsRevoked(out.verified.jti)) {
		out.rejection = ErrorResponse(401, "unauthorized",
			"token has been revoked");
		return out;
	}
	out.ok = true;
	return out;
}


// `Set-Cookie: <name>=<value>; HttpOnly; SameSite=Strict; Path=/api/v0;
//              Max-Age=<lifetime>`
//
// Not `Secure`: amuleapi serves HTTP by design and the operator
// terminates TLS in front. Adding `Secure` would silently break the
// cookie path on every plain-HTTP deployment. Documented in
// QUICKSTART (Phase 10 packaging step).
std::string MakeSetCookie(const std::string &name,
                          const std::string &value,
                          std::time_t expires_at)
{
	const std::time_t now      = std::time(nullptr);
	// Boundary case: an already-expired `expires_at` produces
	// `Max-Age=0`, which makes the browser delete the cookie on
	// receipt (RFC 6265 §5.2.2). That's the right behaviour — issuing
	// an expired token's cookie shouldn't grant the client a working
	// session — so we emit it deliberately rather than clamping to
	// some positive minimum.
	const std::time_t lifetime = expires_at > now ? expires_at - now : 0;
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"%s=%s; HttpOnly; SameSite=Strict; Path=/api/v0; Max-Age=%lld",
		name.c_str(),
		value.c_str(),
		static_cast<long long>(lifetime));
	return buf;
}


// `Set-Cookie: <name>=; ... Max-Age=0` — invalidates whatever was
// set on a prior login. Used by /auth/logout.
std::string MakeClearCookie(const std::string &name)
{
	char buf[160];
	std::snprintf(buf, sizeof(buf),
		"%s=; HttpOnly; SameSite=Strict; Path=/api/v0; Max-Age=0",
		name.c_str());
	return buf;
}


// `amuleapi_token` namespacing keeps the cookie distinct from
// amuleweb's legacy `amule_token` so the two daemons can coexist
// behind the same host without a Set-Cookie tug-of-war.
const char *const kSessionCookieName = "amuleapi_token";


}  // namespace


CApiDispatcher::CApiDispatcher(const CAmuleApiConfig &config,
                               CJwt                  &jwt,
                               webapi::CState        &state,
                               CamuleapiApp          &app)
	: m_config(config),
	  m_jwt(jwt),
	  m_state(state),
	  m_app(app),
	  m_rateLimiter(webapi::CRateLimiter::Config{
		config.AuthCfg().login_failure_window_seconds,
		config.AuthCfg().login_failure_threshold,
		config.AuthCfg().login_lockout_seconds})
{
}


namespace {

// Case-tolerant header lookup. Beast preserves the wire-form casing
// the client sent, so a literal `req.headers.find("If-None-Match")`
// misses lowercased headers. Walks the map once on miss to recover.
std::string FindHeaderCaseInsensitive(
	const std::map<std::string, std::string> &headers,
	const std::string &name)
{
	auto it = headers.find(name);
	if (it != headers.end()) return it->second;
	for (const auto &h : headers) {
		if (h.first.size() == name.size()
		    && strncasecmp(h.first.c_str(), name.c_str(),
		                   name.size()) == 0) {
			return h.second;
		}
	}
	return std::string();
}


// Phase 9: resolve the CORS Origin echo for this request. Returns
// the verbatim Origin to put in `Access-Control-Allow-Origin`, or
// an empty string when CORS is disabled, the request had no Origin
// (same-origin browser navigation; non-browser caller), or the
// allowlist rejected the value. Wildcard semantics: `allow_cors=1`
// with an empty allowlist echoes the request's Origin verbatim,
// which is `*`-equivalent but cookie-auth-compatible (the literal
// `*` is incompatible with `Access-Control-Allow-Credentials: true`
// per CORS Fetch §3.2.5).
std::string ResolveCorsOrigin(const CHttpServer::Request &req,
                              const CAmuleApiConfig &cfg)
{
	if (!cfg.ServerCfg().allow_cors) return std::string();
	const std::string origin = FindHeaderCaseInsensitive(
		req.headers, "Origin");
	if (origin.empty()) return std::string();
	const auto &list = cfg.ServerCfg().cors_origin_allowlist;
	if (list.empty()) return origin;          // echo any origin
	for (const auto &allowed : list) {
		if (allowed == origin) return origin;
	}
	return std::string();
}


// Phase 9: stamp the resolved CORS headers onto a response. `Vary:
// Origin` is ALWAYS added when CORS is enabled (even on rejected
// origins) so intermediaries don't cache a cross-origin response
// against a same-origin cache key. The auth + content headers go
// on iff the origin was actually allowed.
void ApplyCorsHeaders(std::map<std::string, std::string> &headers,
                      const std::string &resolved_origin,
                      bool cors_enabled)
{
	if (!cors_enabled) return;
	headers["Vary"] = "Origin";
	if (resolved_origin.empty()) return;
	headers["Access-Control-Allow-Origin"]      = resolved_origin;
	headers["Access-Control-Allow-Credentials"] = "true";
	// Header names the client may read from `fetch().headers.get(...)`
	// — by default the Fetch spec only exposes the CORS-safelisted
	// response headers (Cache-Control, Content-Language, Content-Type,
	// Expires, Last-Modified, Pragma). amuleapi clients want to read
	// ETag for cache validation; SSE clients don't need this list.
	headers["Access-Control-Expose-Headers"]    = "ETag";
}

}  // namespace


CHttpServer::Response CApiDispatcher::Dispatch(const CHttpServer::Request &req)
{
	const bool cors_enabled    = m_config.ServerCfg().allow_cors;
	const std::string cors_org = ResolveCorsOrigin(req, m_config);

	// Phase 9: CORS preflight short-circuit. OPTIONS requests with
	// `Access-Control-Request-Method` are browser preflights — they
	// don't carry credentials and shouldn't run the auth gate or the
	// route handler. Reply with 204 and the CORS bundle (or 204 +
	// `Vary: Origin` only when the origin is rejected — the browser
	// blocks the subsequent real request).
	if (req.method == "OPTIONS"
	    && !FindHeaderCaseInsensitive(req.headers,
	                                  "Access-Control-Request-Method").empty()) {
		CHttpServer::Response pre;
		pre.status       = 204;
		pre.content_type.clear();
		ApplyCorsHeaders(pre.headers, cors_org, cors_enabled);
		if (!cors_org.empty()) {
			pre.headers["Access-Control-Allow-Methods"] =
				"GET, HEAD, POST, PATCH, DELETE, OPTIONS";
			// Headers actual requests may send. Authorization for
			// bearer; If-None-Match for ETag conditional GET (Phase 7);
			// Last-Event-ID for SSE replay (Phase 8c).
			pre.headers["Access-Control-Allow-Headers"] =
				"Authorization, Content-Type, If-None-Match, Last-Event-ID";
			pre.headers["Access-Control-Max-Age"] = "86400";
		}
		return pre;
	}

	// Phase 7: post-process every response with an ETag stamp +
	// `If-None-Match` → 304 swap, but only on GET/HEAD that come back
	// 200. Mutations (POST/PATCH/DELETE) and error paths are passed
	// through unchanged — there's no benefit to ETag-caching a 4xx
	// body, and a mutation's response carries the post-mutation
	// state which the client always wants delivered.
	CHttpServer::Response resp = DispatchToHandler(req);

	const bool is_safe_method = (req.method == "GET" || req.method == "HEAD");
	if (is_safe_method && resp.status == 200 && !resp.body.empty()) {
		const std::string etag = webcommon::Etag(resp.body);
		// RFC 7232 §2.3 — the header value MUST be quoted.
		resp.headers["ETag"] = "\"" + etag + "\"";

		const std::string inm = FindHeaderCaseInsensitive(
			req.headers, "If-None-Match");
		if (webcommon::IfNoneMatchHits(inm, etag)) {
			// 304 carries no body and no Content-Type, but the ETag
			// header IS preserved (RFC 7232 §4.1 — clients use it to
			// re-stamp the cached representation).
			resp.status = 304;
			resp.body.clear();
			resp.content_type.clear();
		}
		// HEAD never carries a body — the inner handler already shaped
		// the response body for the GET path; strip it now. The ETag
		// header is preserved so HEAD-based cache validators work.
		if (req.method == "HEAD") {
			resp.body.clear();
		}
	}

	// Phase 9: stamp CORS on every response (success and error paths)
	// so browsers can read the body in the 4xx/5xx case too.
	ApplyCorsHeaders(resp.headers, cors_org, cors_enabled);
	return resp;
}


CHttpServer::Response CApiDispatcher::DispatchToHandler(const CHttpServer::Request &req)
{
	std::string path, query;
	SplitPathAndQuery(req.target, path, query);

	if (path == "/api/v0/version") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed",
				"method not allowed on /api/v0/version");
		}
		return HandleVersion(req);
	}

	if (path == "/api/v0/auth/login") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /auth/login");
		}
		return HandleLogin(req);
	}

	if (path == "/api/v0/auth/logout") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /auth/logout");
		}
		return HandleLogout(req);
	}

	if (path == "/api/v0/auth/session") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed",
				"only GET on /auth/session");
		}
		return HandleSession(req);
	}

	if (path == "/api/v0/status") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed",
				"only GET on /status");
		}
		return HandleStatus(req);
	}

	if (path == "/api/v0/downloads") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleDownloads(req);
		}
		if (req.method == "POST") {
			// Phase 5a: add a download by ed2k link.
			return HandleDownloadAdd(req);
		}
		return ErrorResponse(405, "method_not_allowed",
			"only GET / HEAD / POST on /downloads");
	}

	// Phase 5b: bulk clear-completed.
	if (path == "/api/v0/downloads/clear_completed") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /downloads/clear_completed");
		}
		return HandleDownloadsClearCompleted(req);
	}

	// /uploads was retired in Phase 4g — /clients covers the full
	// peer surface (every upload_state, including queue waiters and
	// download peers); consumers filter client-side by upload_state.
	if (path == "/api/v0/clients") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed",
				"only GET on /clients");
		}
		return HandleClients(req);
	}

	if (path == "/api/v0/shared") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed",
				"only GET on /shared");
		}
		return HandleSharedList(req);
	}

	if (path == "/api/v0/shared/reload") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /shared/reload");
		}
		return HandleSharedReload(req);
	}

	if (path == "/api/v0/servers") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleServers(req);
		}
		if (req.method == "POST") {
			// Phase 5c — add a server by host:port.
			return HandleServerAdd(req);
		}
		return ErrorResponse(405, "method_not_allowed",
			"only GET / HEAD / POST on /servers");
	}

	if (path == "/api/v0/servers/update") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /servers/update");
		}
		return HandleServerUpdateFromUrl(req);
	}

	// Phase 5c — server connect & remove (single server by ECID).
	// Address-keyed aliases live in the same block — same handlers,
	// different lookup path. ECID forms are tried first because they
	// match a single-segment pattern that's cheaper to dispatch; the
	// address forms have a colon in the capture which the path pattern
	// captures as a single segment too.
	{
		static const auto server_connect =
			web_api_path::ParsePattern("/api/v0/servers/{ecid}/connect");
		static const auto server_one =
			web_api_path::ParsePattern("/api/v0/servers/{ecid}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(server_connect, path_segs, caps)) {
			if (req.method != "POST") {
				return ErrorResponse(405, "method_not_allowed",
					"only POST on /servers/{ecid}/connect");
			}
			// `{ecid}` capture also matches "<ip>:<port>" because the
			// path-pattern matcher is opaque-segment. Disambiguate
			// here: if the capture contains a colon, treat it as an
			// address-keyed alias.
			if (caps["ecid"].find(':') != std::string::npos) {
				return HandleServerConnectByAddress(req, caps["ecid"]);
			}
			return HandleServerConnect(req, caps["ecid"]);
		}
		if (web_api_path::Match(server_one, path_segs, caps)) {
			if (req.method != "DELETE") {
				return ErrorResponse(405, "method_not_allowed",
					"only DELETE on /servers/{ecid}");
			}
			if (caps["ecid"].find(':') != std::string::npos) {
				return HandleServerDeleteByAddress(req, caps["ecid"]);
			}
			return HandleServerDelete(req, caps["ecid"]);
		}
	}

	if (path == "/api/v0/kad") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed",
				"only GET on /kad");
		}
		return HandleKad(req);
	}

	// Phase 5e — connection control.
	if (path == "/api/v0/networks/connect") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /networks/connect");
		}
		return HandleNetworksConnect(req);
	}
	if (path == "/api/v0/networks/disconnect") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /networks/disconnect");
		}
		return HandleNetworksDisconnect(req);
	}
	if (path == "/api/v0/kad/connect") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /kad/connect");
		}
		return HandleKadConnect(req);
	}
	if (path == "/api/v0/kad/disconnect") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /kad/disconnect");
		}
		return HandleKadDisconnect(req);
	}
	if (path == "/api/v0/kad/bootstrap") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /kad/bootstrap");
		}
		return HandleKadBootstrap(req);
	}

	// Phase 5f — shared file priority PATCH.
	{
		static const auto shared_detail =
			web_api_path::ParsePattern("/api/v0/shared/{hash}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(shared_detail, path_segs, caps)) {
			if (req.method != "PATCH") {
				return ErrorResponse(405, "method_not_allowed",
					"only PATCH on /shared/{hash}");
			}
			return HandleSharedPatch(req, caps["hash"]);
		}
	}

	if (path == "/api/v0/categories") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleCategories(req);
		}
		if (req.method == "POST") {
			return HandleCategoryCreate(req);
		}
		return ErrorResponse(405, "method_not_allowed",
			"only GET / HEAD / POST on /categories");
	}

	// Phase 5g — single-category PATCH/DELETE.
	{
		static const auto category_one =
			web_api_path::ParsePattern("/api/v0/categories/{index}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(category_one, path_segs, caps)) {
			if (req.method == "PATCH") {
				return HandleCategoryUpdate(req, caps["index"]);
			}
			if (req.method == "DELETE") {
				return HandleCategoryDelete(req, caps["index"]);
			}
			return ErrorResponse(405, "method_not_allowed",
				"only PATCH / DELETE on /categories/{index}");
		}
	}

	if (path == "/api/v0/preferences") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandlePreferences(req);
		}
		if (req.method == "PATCH") {
			return HandlePreferencesPatch(req);
		}
		return ErrorResponse(405, "method_not_allowed",
			"only GET / HEAD / PATCH on /preferences");
	}

	if (path == "/api/v0/logs/amule") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleLogAmule(req);
		}
		if (req.method == "DELETE") {
			return HandleLogAmuleReset(req);
		}
		return ErrorResponse(405, "method_not_allowed",
			"only GET / HEAD / DELETE on /logs/amule");
	}

	if (path == "/api/v0/logs/serverinfo") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleLogServerinfo(req);
		}
		if (req.method == "DELETE") {
			return HandleLogServerinfoReset(req);
		}
		return ErrorResponse(405, "method_not_allowed",
			"only GET / HEAD / DELETE on /logs/serverinfo");
	}

	if (path == "/api/v0/stats/tree") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed",
				"only GET on /stats/tree");
		}
		return HandleStatsTree(req);
	}

	// Phase 6 — search.
	if (path == "/api/v0/search") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /search (use GET /search/results for results)");
		}
		return HandleSearchStart(req);
	}
	if (path == "/api/v0/search/stop") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed",
				"only POST on /search/stop");
		}
		return HandleSearchStop(req);
	}
	if (path == "/api/v0/search/results") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed",
				"only GET on /search/results");
		}
		return HandleSearchResults(req);
	}
	{
		static const auto search_download =
			web_api_path::ParsePattern("/api/v0/search/results/{hash}/download");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(search_download, path_segs, caps)) {
			if (req.method != "POST") {
				return ErrorResponse(405, "method_not_allowed",
					"only POST on /search/results/{hash}/download");
			}
			return HandleSearchDownload(req, caps["hash"]);
		}
	}

	// /stats/graphs/{graph} — path-pattern matches the four allowed
	// graph names ("download" / "upload" / "connections" / "kad").
	{
		static const auto graph_pattern =
			web_api_path::ParsePattern("/api/v0/stats/graphs/{graph}");
		const auto segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(graph_pattern, segs, caps)) {
			if (req.method != "GET" && req.method != "HEAD") {
				return ErrorResponse(405, "method_not_allowed",
					"only GET on /stats/graphs/{graph}");
			}
			return HandleStatsGraph(req, caps["graph"]);
		}
	}

	// /downloads/{hash} — single-resource detail (GET / HEAD) and the
	// Phase 5a mutation surface (PATCH for status/priority/category).
	// Phase 5b adds DELETE (clear-completed single).
	{
		static const auto download_detail =
			web_api_path::ParsePattern("/api/v0/downloads/{hash}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(download_detail, path_segs, caps)) {
			if (req.method == "GET" || req.method == "HEAD") {
				return HandleDownloadDetail(req, caps["hash"]);
			}
			if (req.method == "PATCH") {
				return HandleDownloadPatch(req, caps["hash"]);
			}
			if (req.method == "DELETE") {
				return HandleDownloadDelete(req, caps["hash"]);
			}
			return ErrorResponse(405, "method_not_allowed",
				"only GET / HEAD / PATCH / DELETE on /downloads/{hash}");
		}
	}

	return ErrorResponse(404, "not_found", "no such endpoint");
}


CHttpServer::Response CApiDispatcher::HandleVersion(const CHttpServer::Request &)
{
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("name");          w.ValueString(wxT("amuleapi"));
	  w.Key("api_version");   w.ValueString(wxT("v0"));
	  w.Key("amule_version"); w.ValueString(wxString::FromAscii(VERSION));
	w.EndObject();
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}


CHttpServer::Response CApiDispatcher::HandleLogin(const CHttpServer::Request &req)
{
	const std::string &ip = req.remote_addr;

	// Rate-limit BEFORE we touch the credential path. A locked-out
	// IP burns no MD5 cycles and can't drive a side-channel that
	// would distinguish "lockout in effect" from "wrong password".
	const auto decision = m_rateLimiter.Check(ip);
	if (decision.locked_out) {
		CHttpServer::Response r = ErrorResponse(429, "rate_limited",
			"too many failed attempts; retry later");
		char retry_after[32];
		std::snprintf(retry_after, sizeof(retry_after), "%lld",
			static_cast<long long>(decision.retry_after_seconds));
		r.headers["Retry-After"] = retry_after;
		return r;
	}

	// Refuse early if amuleapi has no passwords configured at all —
	// otherwise every login would silently fail and the operator
	// debugging "why isn't login working" would think the JWT was
	// the problem.
	if (m_config.AdminPasswordMd5().empty()
	    && m_config.GuestPasswordMd5().empty()) {
		return ErrorResponse(503, "login_disabled",
			"amuleapi has no admin/guest password configured; "
			"set one via `amuleapi --set-admin-pass=<plain>`");
	}

	// Parse `{"password": "<plain>"}`. Anything else gets a 400.
	picojson::value v;
	const std::string err = picojson::parse(v, req.body);
	if (!err.empty() || !v.is<picojson::object>()) {
		return ErrorResponse(400, "bad_request",
			"body must be JSON object {\"password\": \"...\"}");
	}
	const auto &obj = v.get<picojson::object>();
	auto pw_it = obj.find("password");
	if (pw_it == obj.end() || !pw_it->second.is<std::string>()) {
		return ErrorResponse(400, "bad_request",
			"missing or non-string `password` field");
	}
	const wxString plain = wxString::FromUTF8(
		pw_it->second.get<std::string>().c_str());
	const std::string md5_hex(
		MD5Sum(plain).GetHash().utf8_str());

	// Compare against admin first, then guest. ConstantTimeEquals is
	// length-leaking by design (both sides are 32 hex chars, so length
	// is fixed) but byte-content equality is constant time.
	Role role = Role::GUEST;
	bool match = false;
	if (!m_config.AdminPasswordMd5().empty()
	    && webcommon::ConstantTimeEquals(md5_hex, m_config.AdminPasswordMd5())) {
		role  = Role::ADMIN;
		match = true;
	} else if (!m_config.GuestPasswordMd5().empty()
	           && webcommon::ConstantTimeEquals(md5_hex, m_config.GuestPasswordMd5())) {
		role  = Role::GUEST;
		match = true;
	}

	if (!match) {
		m_rateLimiter.NoteFailure(ip);
		return ErrorResponse(401, "invalid_credentials",
			"password does not match any configured role");
	}

	m_rateLimiter.NoteSuccess(ip);

	const CJwt::IssuedToken issued = m_jwt.Issue(role);

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	r.headers["Set-Cookie"] = MakeSetCookie(
		kSessionCookieName, issued.token, issued.expires_at);

	CJsonWriter w;
	w.BeginObject();
	  w.Key("token");           w.ValueString(wxString::FromUTF8(issued.token.c_str()));
	  w.Key("role");
	  w.ValueString(role == Role::ADMIN ? wxT("admin") : wxT("guest"));
	  w.Key("expires_at");      w.ValueString(wxString::FromUTF8(
	                              webapi::FormatIso8601Utc(issued.expires_at).c_str()));
	  w.Key("expires_at_unix"); w.ValueInt(static_cast<int64_t>(issued.expires_at));
	  w.Key("jti");             w.ValueString(wxString::FromUTF8(issued.jti.c_str()));
	w.EndObject();
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}


CHttpServer::Response CApiDispatcher::HandleLogout(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	// Add the jti to the revocation set with the JWT's own exp as the
	// TTL — once the token would have expired anyway, the GC drops
	// the entry.
	m_revocations.Revoke(a.verified.jti, a.verified.exp);

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	r.headers["Set-Cookie"] = MakeClearCookie(kSessionCookieName);

	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok"); w.ValueBool(true);
	w.EndObject();
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}


CHttpServer::Response CApiDispatcher::HandleSession(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";

	CJsonWriter w;
	w.BeginObject();
	  w.Key("role");
	  w.ValueString(a.verified.role == Role::ADMIN ? wxT("admin") : wxT("guest"));
	  w.Key("jti"); w.ValueString(wxString::FromUTF8(a.verified.jti.c_str()));
	  w.Key("exp");      w.ValueString(wxString::FromUTF8(
	                       webapi::FormatIso8601Utc(a.verified.exp).c_str()));
	  w.Key("exp_unix"); w.ValueInt(static_cast<int64_t>(a.verified.exp));
	w.EndObject();
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}


CHttpServer::Response CApiDispatcher::HandleStatus(const CHttpServer::Request &req)
{
	// Read endpoints: any authenticated role is enough (admin OR
	// guest). Phase 5+ mutating endpoints will gate on `admin` only.
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	// Until the refresher has completed at least one tick, the cache
	// is empty. Return 503 with a structured code so clients can
	// retry rather than guessing — saves a round of confused log-
	// reading when the daemon just came up.
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	// Single shared_lock for the whole composite read. Dashboard()
	// returns a (status, kad, snapshot_at, ec_connected) tuple in
	// one m_state lock acquisition, so a refresher tick cannot land
	// between sub-snapshots and produce an inconsistent rollup
	// (kad.network from tick N+1 while ed2k.* / speeds.* are from
	// tick N). Caller-side aliases keep the rest of the function
	// reading the same way the four-accessor version did.
	const webapi::CState::DashboardSnapshot d = m_state.Dashboard();
	const webapi::StatusSnapshot &s   = d.status;
	const webapi::KadSnapshot    &k   = d.kad;
	const std::time_t            ts  = d.snapshot_at;
	const bool                   ec  = d.ec_connected;

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";

	CJsonWriter w;
	w.BeginObject();
	  // Phase 7.1: snapshot_at + snapshot_at_unix were retired from
	  // every envelope response so the ETag/If-None-Match cache
	  // (Phase 7) actually gets cache hits on list endpoints.
	  // `ec_connected` is the dedicated staleness signal — it flips
	  // false when the refresher tick fails. Standard HTTP `Date:`
	  // header carries wall-clock for any consumer that needs it.
	  w.Key("ec_connected");     w.ValueBool(ec);
	  (void) ts;

	  w.Key("ed2k");
	  w.BeginObject();
	    w.Key("state");       w.ValueString(wxString::FromUTF8(s.ed2k_state.c_str()));
	    w.Key("low_id");      w.ValueBool(s.ed2k_lowid);
	    w.Key("server_name"); w.ValueString(wxString::FromUTF8(s.server_name.c_str()));
	    w.Key("server_ip");   w.ValueString(wxString::FromUTF8(s.server_ip.c_str()));
	    w.Key("server_port"); w.ValueInt(static_cast<int64_t>(s.server_port));
	  w.EndObject();

	  w.Key("kad");
	  w.BeginObject();
	    w.Key("state");      w.ValueString(wxString::FromUTF8(s.kad_state.c_str()));
	    w.Key("firewalled"); w.ValueBool(s.kad_firewalled);
	    // Network rollup — same numbers GET /kad serves under
	    // `network.{users,files,nodes}`. Surfaced here so /status
	    // is a one-call dashboard view (matches the RFC contract
	    // §4.1 `kad.network: {users, files}`; we ship `nodes` too
	    // because it costs nothing extra and the desktop GUI shows
	    // it in the same place). `k` was snapshotted at the top of
	    // the handler in the same shared_lock batch as `s`, so
	    // these counters describe the same refresher tick as
	    // ed2k.* / speeds.* above.
	    w.Key("network");
	    w.BeginObject();
	      w.Key("users"); w.ValueInt(static_cast<int64_t>(k.users));
	      w.Key("files"); w.ValueInt(static_cast<int64_t>(k.files));
	      w.Key("nodes"); w.ValueInt(static_cast<int64_t>(k.nodes));
	    w.EndObject();
	  w.EndObject();

	  w.Key("speeds");
	  w.BeginObject();
	    w.Key("download_bps"); w.ValueInt(static_cast<int64_t>(s.download_bps));
	    w.Key("upload_bps");   w.ValueInt(static_cast<int64_t>(s.upload_bps));
	  w.EndObject();

	  w.Key("queue");
	  w.BeginObject();
	    w.Key("upload_queue_length"); w.ValueInt(static_cast<int64_t>(s.ul_queue_len));
	    w.Key("total_source_count");  w.ValueInt(static_cast<int64_t>(s.total_src_count));
	  w.EndObject();
	  // Nickname is a /preferences field, not a /status one (Phase 4c).
	w.EndObject();

	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}


namespace {

// Compact helper — every read endpoint serialises its body the same
// way (build a wxString via CJsonWriter, then utf8_str into the
// response body). Hide the boilerplate behind a helper that takes a
// ready CJsonWriter.
void FinalizeJsonBody(CJsonWriter &w, CHttpServer::Response &r)
{
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
}


// Write a single download object. Used both inline (in the list
// endpoint, iterated) and as the body of the detail endpoint (bare,
// per Q3). The `include_envelope_keys` flag controls whether we
// emit the snapshot_at envelope around it — list mode wraps in its
// own envelope, detail mode is the bare object.
// PARTSIZE — the byte width of a single partfile chunk in amule
// (9.28 MB). Authoritative copy is in `protocol/ed2k/Constants.h`;
// duplicated here to avoid pulling that header into Api.cpp (which
// would cascade into the protocol-level types). amule has never
// changed PARTSIZE since the ed2k spec was frozen.
constexpr std::uint64_t kPartSize = 9728000ull;


// Render the per-part state array from the decoded gap list +
// per-part source counts. Algorithm cribbed from the reference REST
// branch's `EmitProgressParts` (WebServerApi.cpp:897-952):
//   - count = ceil(size / PARTSIZE)
//   - mark a part "has gap" if any byte-range in `gaps` covers it
//   - state = "complete"   (no gap) /
//             "incomplete" (gap + sources > 0) /
//             "missing"    (gap + zero sources)
// `gaps` is flat (start, end) uint64 pairs. Both inclusive on amule's
// side (CGapList::Encode semantics).
void WriteProgressParts(CJsonWriter &w, const webapi::DownloadSnapshot &d)
{
	w.Key("parts");
	w.BeginArray();
	if (d.size == 0) { w.EndArray(); return; }
	const std::uint64_t part_count = (d.size + kPartSize - 1) / kPartSize;
	std::vector<bool> has_gap(part_count, false);
	const std::size_t gap_pair_count = d.decoded_gaps.size() / 2;
	for (std::size_t g = 0; g < gap_pair_count; ++g) {
		const std::uint64_t gap_start = d.decoded_gaps[2 * g];
		const std::uint64_t gap_end   = d.decoded_gaps[2 * g + 1];
		const std::uint64_t start_idx = gap_start / kPartSize;
		const std::uint64_t end_idx   = gap_end   / kPartSize;
		for (std::uint64_t i = start_idx;
		     i <= end_idx && i < part_count; ++i) {
			has_gap[static_cast<std::size_t>(i)] = true;
		}
	}
	for (std::uint64_t i = 0; i < part_count; ++i) {
		const std::uint16_t sources =
			(static_cast<std::size_t>(i) < d.decoded_part_sources.size())
			? d.decoded_part_sources[static_cast<std::size_t>(i)]
			: static_cast<std::uint16_t>(0);
		const char *state =
			!has_gap[static_cast<std::size_t>(i)] ? "complete" :
			(sources > 0 ? "incomplete" : "missing");
		w.BeginObject();
		  w.Key("state");   w.ValueString(wxString::FromAscii(state));
		  w.Key("sources"); w.ValueInt(static_cast<int64_t>(sources));
		w.EndObject();
	}
	w.EndArray();
}


void WriteDownloadObject(CJsonWriter &w, const webapi::DownloadSnapshot &d,
                         bool include_parts = false)
{
	w.BeginObject();
	  w.Key("hash");            w.ValueString(wxString::FromUTF8(d.hash.c_str()));
	  w.Key("name");            w.ValueString(wxString::FromUTF8(d.name.c_str()));
	  w.Key("ed2k_link");       w.ValueString(wxString::FromUTF8(d.ed2k_link.c_str()));
	  w.Key("size");            w.ValueInt(static_cast<int64_t>(d.size));
	  w.Key("size_done");       w.ValueInt(static_cast<int64_t>(d.size_done));
	  w.Key("size_xfer");       w.ValueInt(static_cast<int64_t>(d.size_xfer));
	  w.Key("speed_bps");       w.ValueInt(static_cast<int64_t>(d.speed_bps));
	  w.Key("status");          w.ValueString(wxString::FromUTF8(d.status.c_str()));
	  w.Key("priority");        w.ValueString(wxString::FromUTF8(d.priority.c_str()));
	  w.Key("priority_auto");   w.ValueBool(d.priority_auto);
	  w.Key("category");        w.ValueInt(static_cast<int64_t>(d.category));
	  w.Key("sources");
	  w.BeginObject();
	    w.Key("total");        w.ValueInt(static_cast<int64_t>(d.sources_total));
	    w.Key("not_current");  w.ValueInt(static_cast<int64_t>(d.sources_not_current));
	    w.Key("transferring"); w.ValueInt(static_cast<int64_t>(d.sources_transferring));
	    w.Key("a4af");         w.ValueInt(static_cast<int64_t>(d.sources_a4af));
	  w.EndObject();
	  w.Key("progress");
	  w.BeginObject();
	    w.Key("percent"); w.ValueDouble(d.percent);
	    if (include_parts) {
	      WriteProgressParts(w, d);
	    }
	  w.EndObject();
	w.EndObject();
}


void WriteClientObject(CJsonWriter &w, const webapi::ClientSnapshot &c)
{
	w.BeginObject();
	  w.Key("ecid");                  w.ValueInt(static_cast<int64_t>(c.ecid));
	  w.Key("client_name");           w.ValueString(wxString::FromUTF8(c.client_name.c_str()));
	  w.Key("user_hash");             w.ValueString(wxString::FromUTF8(c.user_hash.c_str()));
	  w.Key("ip");                    w.ValueString(wxString::FromUTF8(c.ip.c_str()));
	  w.Key("port");                  w.ValueInt(static_cast<int64_t>(c.port));
	  w.Key("software");              w.ValueString(wxString::FromUTF8(c.software.c_str()));
	  w.Key("software_version");      w.ValueString(wxString::FromUTF8(c.software_version.c_str()));
	  w.Key("os_info");               w.ValueString(wxString::FromUTF8(c.os_info.c_str()));
	  w.Key("upload_state");          w.ValueString(wxString::FromUTF8(c.upload_state.c_str()));
	  w.Key("download_state");        w.ValueString(wxString::FromUTF8(c.download_state.c_str()));
	  w.Key("ident_state");           w.ValueString(wxString::FromUTF8(c.ident_state.c_str()));
	  w.Key("requested_file_name");   w.ValueString(wxString::FromUTF8(c.requested_file_name.c_str()));
	  w.Key("requested_file_hash");   w.ValueString(wxString::FromUTF8(c.requested_file_hash.c_str()));
	  w.Key("downloading_file_hash"); w.ValueString(wxString::FromUTF8(c.downloading_file_hash.c_str()));
	  w.Key("xfer");
	  w.BeginObject();
	    w.Key("up_session");     w.ValueInt(static_cast<int64_t>(c.xfer_up_session));
	    w.Key("down_session");   w.ValueInt(static_cast<int64_t>(c.xfer_down_session));
	    w.Key("up_total");       w.ValueInt(static_cast<int64_t>(c.xfer_up_total));
	    w.Key("down_total");     w.ValueInt(static_cast<int64_t>(c.xfer_down_total));
	  w.EndObject();
	  w.Key("upload_speed_bps");      w.ValueInt(static_cast<int64_t>(c.upload_speed_bps));
	  w.Key("download_speed_bps");    w.ValueInt(static_cast<int64_t>(c.download_speed_bps));
	  w.Key("queue_waiting_position"); w.ValueInt(static_cast<int64_t>(c.queue_waiting_position));
	  w.Key("remote_queue_rank");     w.ValueInt(static_cast<int64_t>(c.remote_queue_rank));
	  w.Key("score");                 w.ValueInt(static_cast<int64_t>(c.score));
	  w.Key("obfuscation_status");    w.ValueString(wxString::FromUTF8(c.obfuscation_status.c_str()));
	  w.Key("friend_slot");           w.ValueBool(c.friend_slot);
	w.EndObject();
}


void WriteSharedObject(CJsonWriter &w, const webapi::SharedSnapshot &s)
{
	w.BeginObject();
	  w.Key("hash");              w.ValueString(wxString::FromUTF8(s.hash.c_str()));
	  w.Key("name");              w.ValueString(wxString::FromUTF8(s.name.c_str()));
	  w.Key("ed2k_link");         w.ValueString(wxString::FromUTF8(s.ed2k_link.c_str()));
	  w.Key("size");              w.ValueInt(static_cast<int64_t>(s.size));
	  w.Key("priority");          w.ValueString(wxString::FromUTF8(s.priority.c_str()));
	  w.Key("complete_sources");  w.ValueInt(static_cast<int64_t>(s.complete_sources));
	  w.Key("xfer");
	  w.BeginObject();
	    w.Key("session"); w.ValueInt(static_cast<int64_t>(s.xfer_session));
	    w.Key("total");   w.ValueInt(static_cast<int64_t>(s.xfer_total));
	  w.EndObject();
	  w.Key("requests");
	  w.BeginObject();
	    w.Key("session"); w.ValueInt(static_cast<int64_t>(s.requests_session));
	    w.Key("total");   w.ValueInt(static_cast<int64_t>(s.requests_total));
	  w.EndObject();
	  w.Key("accepts");
	  w.BeginObject();
	    w.Key("session"); w.ValueInt(static_cast<int64_t>(s.accepts_session));
	    w.Key("total");   w.ValueInt(static_cast<int64_t>(s.accepts_total));
	  w.EndObject();
	w.EndObject();
}


// Helper for every list endpoint's envelope: snapshot_at +
// snapshot_at_unix + the list under its named key. ec_unavailable +
// 503 is also emitted here so each handler doesn't repeat the check.
template <class T, class WriterFn>
CHttpServer::Response ListResponse(const webapi::CState &state,
                                   const char           *plural_key,
                                   const std::vector<T> &items,
                                   WriterFn              write_item)
{
	if (!state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	// Phase 7.1: envelope responses dropped snapshot_at_* — they were
	// defeating the ETag cache (Phase 7) by churning the body bytes
	// every refresher tick. The ETag (Phase 7) is now the cache
	// validator; HTTP `Date:` is the wall-clock.
	CJsonWriter w;
	w.BeginObject();
	  w.Key(plural_key);
	  w.BeginArray();
	  for (const auto &item : items) write_item(w, item);
	  w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

}  // namespace


// ===================================================================
// Phase 5 mutation helpers — shared by every Handle{Resource}{Patch,Add,
// Delete} below. The contract is the one PLAN §14 4a established for
// every mutation handler:
//   1. AuthenticateRequest (bearer or cookie)
//   2. RequireAdmin (Phase 5 introduces the role gate; mutations are
//      admin-only)
//   3. Parse JSON body (mutations carry their payload in the body)
//   4. Build + send EC mutation packet via SendRecvSerialized
//   5. Check the response — EC_OP_NOOP means success, EC_OP_FAILED
//      carries an amuled-side rejection string we surface
//   6. Run RefresherTick inline on the same HTTP thread so the
//      response sees post-mutation state (vs. the next refresher tick
//      catching up ~1 s later)
//   7. Return the updated resource (or 201 / 204 per HTTP convention)
// ===================================================================

namespace {

// Admin role gate. Drop-in for the standard ` if (!a.ok) return
// a.rejection;` pattern; mutations chain ` if (auto r = RequireAdmin(a))
// return *r;` immediately after.
std::unique_ptr<CHttpServer::Response> RequireAdmin(const AuthOutcome &a)
{
	if (a.verified.role != Role::ADMIN) {
		return std::unique_ptr<CHttpServer::Response>(
			new CHttpServer::Response(
				ErrorResponse(403, "forbidden",
					"admin role required for this endpoint")));
	}
	return nullptr;
}


// JSON body parser. Returns true and populates `out` on success;
// false and populates `err` on failure. Mutations expect a JSON
// object at the root; non-object roots (array / string / number) are
// rejected with a clear error.
bool ParseJsonObjectBody(const std::string &body, picojson::value &out,
                         std::string &err)
{
	const std::string parse_err = picojson::parse(out, body);
	if (!parse_err.empty()) {
		err = "malformed JSON: " + parse_err;
		return false;
	}
	if (!out.is<picojson::object>()) {
		err = "request body must be a JSON object";
		return false;
	}
	return true;
}


// Surfaces the EC_OP_FAILED reply shape from amuled. The standard
// amuled failure response carries one or more EC_TAG_STRING children
// with the rejection message; we relay the first one to the client.
// Returns true if the response was an error (caller short-circuits);
// false on EC_OP_NOOP or any other "success" shape.
bool IsEcFailedResponse(const CECPacket *resp, std::string &out_msg)
{
	if (!resp) return false;
	if (resp->GetOpCode() != EC_OP_FAILED) return false;
	out_msg = "amuled rejected the operation";
	for (CECPacket::const_iterator it = resp->begin(); it != resp->end(); ++it) {
		const CECTag *t = &*it;
		if (t->GetTagName() == EC_TAG_STRING) {
			out_msg = std::string(t->GetStringData().utf8_str());
			break;
		}
	}
	return true;
}


// Map our wire-string priorities back to amule's PR_* encoding (the
// inverse of DownloadPriorityName in Refresher.cpp). Note: amule's
// PR_* values for DOWNLOADS only span LOW/NORMAL/HIGH/VERYHIGH/AUTO —
// no `very_low` (that's a shared/upload-side enum). `release` is the
// wire string for the highest priority (`PR_VERYHIGH`, raw code 3).
// `PR_AUTO=5` is the magic value amule's PartFile uses internally
// when the user picks "auto".
// Returns false if the wire string isn't a known download priority.
bool DownloadPriorityToCode(const std::string &name, std::uint8_t &out)
{
	if      (name == "low")      { out = PR_LOW;      return true; }
	else if (name == "normal")   { out = PR_NORMAL;   return true; }
	else if (name == "high")     { out = PR_HIGH;     return true; }
	else if (name == "release")  { out = PR_VERYHIGH; return true; }
	else if (name == "auto")     { out = PR_AUTO;     return true; }
	return false;
}


// MD4 hex string → CMD4Hash. Returns false if the string isn't 32
// lowercase-or-uppercase hex chars (we tolerate both cases; the
// Phase 4b route already lowercases what comes off the URL).
bool HashFromHex(const std::string &hex, CMD4Hash &out)
{
	if (hex.size() != 32) return false;
	return out.Decode(wxString::FromAscii(hex.c_str()));
}

}  // namespace


CHttpServer::Response CApiDispatcher::HandleDownloads(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	// /downloads filters status=="completed" out by default (Phase 4h).
	// amuled holds finished downloads in `m_completedDownloads` as a
	// separate "awaiting clear" list; surfacing them in /downloads
	// alongside in-progress files confuses consumers reading
	// "what's currently in the transfer queue." Opt back in with
	// `?include_completed=1`. The detail endpoint
	// `GET /downloads/{hash}` is unaffected — consumer asked for that
	// specific file. Phase 5+ may add an explicit clear-completed
	// mutation (deferred to v0.2 per Phase 4h decision).
	bool include_completed = false;
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos) query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("include_completed");
		if (it != qmap.end()) {
			const std::string &v = it->second;
			include_completed = (v == "1" || v == "true" || v == "yes");
		}
	}

	std::vector<webapi::DownloadSnapshot> downloads = m_state.Downloads();
	if (!include_completed) {
		downloads.erase(
			std::remove_if(downloads.begin(), downloads.end(),
				[](const webapi::DownloadSnapshot &d) {
					return d.status == "completed";
				}),
			downloads.end());
	}

	return ListResponse(m_state, "downloads", downloads,
		[](CJsonWriter &w, const webapi::DownloadSnapshot &d) {
			// List mode — omit `progress.parts` (Q2 + the per-list
			// shape: omitting parts keeps the list response compact,
			// detail endpoint is where parts ship).
			WriteDownloadObject(w, d, /*include_parts=*/false);
		});
}


CHttpServer::Response CApiDispatcher::HandleClients(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	// Optional `?filter=uploads | downloads | active` query parameter.
	// `uploads`   → peers actively transferring TO us (upload_state ==
	//               "uploading"). Subset that maps to the legacy
	//               amuleweb "Uploads" page.
	// `downloads` → peers we're actively pulling FROM (download_state
	//               == "downloading").
	// `active`    → union of the two; everything currently moving
	//               bytes either direction.
	// No filter → every peer the daemon knows about (default, v0.1
	// shape).
	std::string filter;
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos) query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("filter");
		if (it != qmap.end()) filter = it->second;
	}
	if (!filter.empty() && filter != "uploads" && filter != "downloads"
	    && filter != "active") {
		return ErrorResponse(400, "bad_request",
			"`filter` must be one of \"uploads\", \"downloads\", \"active\"");
	}

	auto clients = m_state.Clients();
	if (!filter.empty()) {
		auto matches = [&](const webapi::ClientSnapshot &c) {
			const bool up   = (c.upload_state   == "uploading");
			const bool down = (c.download_state == "downloading");
			if (filter == "uploads")   return up;
			if (filter == "downloads") return down;
			/* active */               return up || down;
		};
		clients.erase(
			std::remove_if(clients.begin(), clients.end(),
				[&](const webapi::ClientSnapshot &c) { return !matches(c); }),
			clients.end());
	}

	return ListResponse(m_state, "clients", clients, WriteClientObject);
}


CHttpServer::Response CApiDispatcher::HandleSharedList(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	return ListResponse(m_state, "shared", m_state.Shared(),
		WriteSharedObject);
}


CHttpServer::Response CApiDispatcher::HandleDownloadDetail(
	const CHttpServer::Request &req,
	const std::string          &hash)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	// Canonicalise the URL hash to lowercase for the lookup —
	// `FindDownload` matches against the State-side lowercase hash
	// we wrote during the refresher tick, so a client typing the
	// hash uppercase still hits.
	std::string needle = hash;
	std::transform(needle.begin(), needle.end(), needle.begin(),
		[](unsigned char c) { return std::tolower(c); });

	webapi::DownloadSnapshot d;
	if (!m_state.FindDownload(needle, d)) {
		return ErrorResponse(404, "not_found",
			"no download with that hash");
	}

	// Bare object per Q3: list endpoint envelopes, detail endpoint
	// is the resource itself. No `snapshot_at` here — clients that
	// need freshness metadata can read the list endpoint.
	//
	// `include_parts=true` adds `progress.parts: [...]` to the
	// response. List endpoint omits this — `parts` can be 100K+
	// entries for a multi-TiB download (Q2: no cap), which clients
	// don't need to walk through when paging the queue overview.
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteDownloadObject(w, d, /*include_parts=*/true);
	FinalizeJsonBody(w, r);
	return r;
}


// --- Phase 5a — download lifecycle mutations ---------------------------

CHttpServer::Response CApiDispatcher::HandleDownloadAdd(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	// Body shape (two forms — both accepted, exactly one required):
	//   {"ed2k_link": "ed2k://|file|...|/", "category": 0}    — singular
	//   {"links": ["ed2k://|file|...|/", ...], "category": 0} — array
	// `links` is the RFC §4.2 shape (PR #132); `ed2k_link` ships for
	// backwards compatibility with the v0.1.0 wire. Mixing both is a
	// 400.
	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	std::vector<std::string> links;
	{
		const auto it_single = obj.find("ed2k_link");
		const auto it_array  = obj.find("links");
		if (it_single != obj.end() && it_array != obj.end()) {
			return ErrorResponse(400, "bad_request",
				"send either `ed2k_link` (single) or `links` (array), "
				"not both");
		}
		if (it_single != obj.end()) {
			if (!it_single->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`ed2k_link` must be a string");
			}
			links.push_back(it_single->second.get<std::string>());
		} else if (it_array != obj.end()) {
			if (!it_array->second.is<picojson::array>()) {
				return ErrorResponse(400, "bad_request",
					"`links` must be an array of ed2k:// strings");
			}
			const auto &arr = it_array->second.get<picojson::array>();
			if (arr.empty()) {
				return ErrorResponse(400, "bad_request",
					"`links` must contain at least one entry");
			}
			links.reserve(arr.size());
			for (const auto &v : arr) {
				if (!v.is<std::string>()) {
					return ErrorResponse(400, "bad_request",
						"every entry in `links` must be a string");
				}
				links.push_back(v.get<std::string>());
			}
		} else {
			return ErrorResponse(400, "bad_request",
				"required field missing: send `ed2k_link` (string) or "
				"`links` (array of strings)");
		}
		for (const auto &link : links) {
			if (link.size() < 7 || link.compare(0, 7, "ed2k://") != 0) {
				return ErrorResponse(400, "bad_request",
					"every link must start with ed2k://");
			}
		}
	}
	std::uint8_t category = 0;
	{
		const auto it = obj.find("category");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request",
					"`category` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 255) {
				return ErrorResponse(400, "bad_request",
					"`category` must be in [0, 255]");
			}
			category = static_cast<std::uint8_t>(v);
		}
	}

	// Build one EC_OP_ADD_LINK packet per link. amuled's add-link op
	// is single-link-only on the wire; we batch at the HTTP layer so
	// clients only pay one round-trip. We accumulate accepted /
	// failed / disconnected-mid-batch into separate lists and report
	// the whole picture at the end — never short-circuit on an EC
	// blip mid-batch (an unconditional 503 would silently throw away
	// the links amuled already queued from earlier iterations).
	std::vector<std::string> accepted_links;
	std::vector<std::string> failed_links;
	std::vector<std::string> ec_disconnected_links;
	std::string first_error;
	for (const auto &link : links) {
		std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_ADD_LINK));
		CECTag link_tag(EC_TAG_STRING, wxString::FromUTF8(link.c_str()));
		link_tag.AddTag(CECTag(EC_TAG_PARTFILE_CAT, category));
		ec_req->AddTag(link_tag);
		const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
		if (!ec_resp) {
			ec_disconnected_links.push_back(link);
			if (first_error.empty()) {
				first_error = "EC roundtrip failed for ADD_LINK";
			}
			continue;
		}
		std::string ec_err_msg;
		const bool failed = IsEcFailedResponse(ec_resp, ec_err_msg);
		delete ec_resp;
		if (failed) {
			failed_links.push_back(link);
			if (first_error.empty()) first_error = ec_err_msg;
		} else {
			accepted_links.push_back(link);
		}
	}

	// Inline-refresh the cache so the response sees post-mutation
	// state. amuled's ADD_LINK is asynchronous (the partfile gets
	// allocated + hashed before it shows up in m_filelist), so the
	// new entry may not surface until the *next* tick — we'd still
	// return 202 Accepted with an empty resource. For now: refresh,
	// then return {ok: true} and leave the GET /downloads to surface
	// the new entry.
	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	// 202 (all clean), 207 (any rejection or mid-batch disconnect
	// with at least one accept), 503 (every link blocked by an EC
	// disconnect — the operator's amuled is unreachable and nothing
	// could land at all). 207 is per the partial-success convention
	// documented in QUICKSTART.
	const bool all_ok = failed_links.empty()
	                    && ec_disconnected_links.empty();
	const bool none_landed = accepted_links.empty();
	r.status = all_ok ? 202
	         : (none_landed && !ec_disconnected_links.empty())
	             ? 503
	             : 207;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");        w.ValueBool(all_ok);
	  w.Key("accepted");  w.ValueInt(static_cast<int64_t>(accepted_links.size()));
	  w.Key("failed");    w.ValueInt(static_cast<int64_t>(failed_links.size()));
	  w.Key("disconnected"); w.ValueInt(
	      static_cast<int64_t>(ec_disconnected_links.size()));
	  if (!accepted_links.empty()) {
	      w.Key("accepted_links");
	      w.BeginArray();
	      for (const auto &l : accepted_links) {
	          w.ValueString(wxString::FromUTF8(l.c_str()));
	      }
	      w.EndArray();
	  }
	  if (!failed_links.empty()) {
	      w.Key("failed_links");
	      w.BeginArray();
	      for (const auto &l : failed_links) {
	          w.ValueString(wxString::FromUTF8(l.c_str()));
	      }
	      w.EndArray();
	  }
	  if (!ec_disconnected_links.empty()) {
	      // Distinct from `failed_links` — amuled didn't reject these,
	      // it just wasn't there to receive them. Clients can retry
	      // this subset once /api/v0/status reports ec_connected=true.
	      w.Key("disconnected_links");
	      w.BeginArray();
	      for (const auto &l : ec_disconnected_links) {
	          w.ValueString(wxString::FromUTF8(l.c_str()));
	      }
	      w.EndArray();
	  }
	  if (!first_error.empty()) {
	      w.Key("first_error");
	      w.ValueString(wxString::FromUTF8(first_error.c_str()));
	  }
	  w.Key("message");
	  w.ValueString(wxString::FromUTF8(
		"link(s) accepted; new downloads will appear after amuled has "
		"allocated and hashed the partfiles (typically within one "
		"refresher tick)"));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleDownloadPatch(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	// Canonicalise the URL hash to lowercase before the cache lookup
	// (same shape as the GET detail handler).
	std::string needle = hash;
	std::transform(needle.begin(), needle.end(), needle.begin(),
		[](unsigned char c) { return std::tolower(c); });

	webapi::DownloadSnapshot d;
	if (!m_state.FindDownload(needle, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	CMD4Hash file_hash;
	if (!HashFromHex(needle, file_hash)) {
		// Should not happen — we already matched the cache by hash.
		return ErrorResponse(500, "internal_error",
			"failed to decode partfile hash");
	}

	// Each field present in the body fires one EC mutation. We
	// process them in a fixed order (status, priority, category) so
	// the wire effect is deterministic regardless of JSON key order.
	auto send_op = [&](ec_opcode_t op,
	                   bool has_inner, ec_tagname_t inner_name,
	                   std::uint8_t inner_value) -> CHttpServer::Response {
		std::unique_ptr<CECPacket> p(new CECPacket(op));
		CECTag hash_tag(EC_TAG_PARTFILE, file_hash);
		if (has_inner) {
			hash_tag.AddTag(CECTag(inner_name, inner_value));
		}
		p->AddTag(hash_tag);
		const CECPacket *r = m_app.SendRecvSerialized(p.get());
		if (!r) {
			return ErrorResponse(503, "ec_unavailable",
				"EC roundtrip failed");
		}
		std::string ec_err_msg;
		if (IsEcFailedResponse(r, ec_err_msg)) {
			delete r;
			return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
		}
		delete r;
		CHttpServer::Response ok;
		ok.status = 200;
		return ok;
	};

	bool any_change = false;

	// status: "paused" | "resumed"
	{
		const auto it = obj.find("status");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`status` must be one of \"paused\" or \"resumed\"");
			}
			const std::string &v = it->second.get<std::string>();
			ec_opcode_t op;
			if      (v == "paused")  op = EC_OP_PARTFILE_PAUSE;
			else if (v == "resumed") op = EC_OP_PARTFILE_RESUME;
			else {
				return ErrorResponse(400, "bad_request",
					"`status` must be one of \"paused\" or \"resumed\"");
			}
			auto err = send_op(op, /*has_inner=*/false,
				static_cast<ec_tagname_t>(0), 0);
			if (err.status >= 400) return err;
			any_change = true;
		}
	}

	// priority: "very_low"|"low"|"normal"|"high"|"release"|"auto"
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`priority` must be a wire-string enum");
			}
			std::uint8_t code = 0;
			if (!DownloadPriorityToCode(it->second.get<std::string>(), code)) {
				return ErrorResponse(400, "bad_request",
					"`priority` must be one of "
					"low, normal, high, release, auto");
			}
			auto err = send_op(EC_OP_PARTFILE_PRIO_SET, /*has_inner=*/true,
				EC_TAG_PARTFILE_PRIO, code);
			if (err.status >= 400) return err;
			any_change = true;
		}
	}

	// category: integer
	{
		const auto it = obj.find("category");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request",
					"`category` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 255) {
				return ErrorResponse(400, "bad_request",
					"`category` must be in [0, 255]");
			}
			auto err = send_op(EC_OP_PARTFILE_SET_CAT, /*has_inner=*/true,
				EC_TAG_PARTFILE_CAT, static_cast<std::uint8_t>(v));
			if (err.status >= 400) return err;
			any_change = true;
		}
	}

	if (!any_change) {
		return ErrorResponse(400, "bad_request",
			"request body must include at least one of "
			"`status`, `priority`, or `category`");
	}

	// Inline refresh so the response below sees post-mutation state.
	(void) RefresherTick(m_app, m_state);

	// Re-read the snapshot — fall back to the prior copy if the
	// cache evicted it between mutations and this read (vanishingly
	// rare; would mean amuled removed it between our SendRecv and
	// the refresh).
	webapi::DownloadSnapshot d_after;
	if (!m_state.FindDownload(needle, d_after)) d_after = d;

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteDownloadObject(w, d_after, /*include_parts=*/false);
	FinalizeJsonBody(w, r);
	return r;
}


// --- Phase 5b — clear completed / delete partfile ----------------------

CHttpServer::Response CApiDispatcher::HandleDownloadDelete(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	std::string needle = hash;
	std::transform(needle.begin(), needle.end(), needle.begin(),
		[](unsigned char c) { return std::tolower(c); });

	webapi::DownloadSnapshot d;
	if (!m_state.FindDownload(needle, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	// Route by status:
	//   * status == "completed" → EC_OP_CLEAR_COMPLETED (by ECID).
	//     The partfile already sits in `m_completedDownloads`; only
	//     the bulk-clear op reaches that list.
	//   * any other status → EC_OP_PARTFILE_DELETE (by hash). Targets
	//     active entries in `m_filelist`.
	std::unique_ptr<CECPacket> ec_req;
	if (d.status == "completed") {
		ec_req.reset(new CECPacket(EC_OP_CLEAR_COMPLETED));
		ec_req->AddTag(CECTag(EC_TAG_ECID, d.ecid));
	} else {
		CMD4Hash file_hash;
		if (!HashFromHex(needle, file_hash)) {
			return ErrorResponse(500, "internal_error",
				"failed to decode partfile hash");
		}
		ec_req.reset(new CECPacket(EC_OP_PARTFILE_DELETE));
		ec_req->AddTag(CECTag(EC_TAG_PARTFILE, file_hash));
	}

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for DELETE");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Inline refresh — the next GET /downloads must not show the
	// deleted entry. The cache eviction happens via FILE_REMOVED in
	// the GET_UPDATE response.
	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok"); w.ValueBool(true);
	  w.Key("hash"); w.ValueString(wxString::FromUTF8(needle.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleDownloadsClearCompleted(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	// Collect every cached download with status=="completed" — these
	// are the ECIDs that sit in amuled's m_completedDownloads list
	// awaiting an explicit clear (the Phase 4h status decode fix is
	// what makes this enumeration meaningful — completed entries used
	// to look like "paused" and would have been skipped).
	std::vector<std::uint32_t> ecids;
	std::vector<std::string>   hashes_cleared;
	for (const auto &d : m_state.Downloads()) {
		if (d.status == "completed") {
			ecids.push_back(d.ecid);
			hashes_cleared.push_back(d.hash);
		}
	}

	if (ecids.empty()) {
		// Nothing to do — return 200 with cleared:0 so consumers can
		// distinguish "no-op" from "amuled rejected" (both end up
		// with no visible change).
		CHttpServer::Response r;
		r.status       = 200;
		r.content_type = "application/json";
		CJsonWriter w;
		w.BeginObject();
		  w.Key("ok");      w.ValueBool(true);
		  w.Key("cleared"); w.ValueInt(0);
		w.EndObject();
		FinalizeJsonBody(w, r);
		return r;
	}

	// One EC roundtrip with all ECIDs (per amulegui's pattern at
	// amule-remote-gui.cpp:2238-2246).
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_CLEAR_COMPLETED));
	for (std::uint32_t ecid : ecids) {
		ec_req->AddTag(CECTag(EC_TAG_ECID, ecid));
	}
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for CLEAR_COMPLETED");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Inline refresh — the response below + the next GET both must
	// show the post-clear state.
	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");           w.ValueBool(true);
	  w.Key("cleared");      w.ValueInt(static_cast<int64_t>(ecids.size()));
	  w.Key("cleared_hashes");
	  w.BeginArray();
	  for (const auto &h : hashes_cleared) {
	    w.ValueString(wxString::FromUTF8(h.c_str()));
	  }
	  w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


// --- /servers, /kad, /categories, /preferences -------------------------

namespace {

void WriteServerObject(CJsonWriter &w, const webapi::ServerSnapshot &s)
{
	w.BeginObject();
	  // `ecid` is the URL key for /servers/{ecid}/connect and
	  // /servers/{ecid} (Phase 5c). Phase 4f intentionally surfaced
	  // it on /clients for the same reason; servers got it later.
	  w.Key("ecid");        w.ValueInt(static_cast<int64_t>(s.ecid));
	  w.Key("name");        w.ValueString(wxString::FromUTF8(s.name.c_str()));
	  w.Key("description"); w.ValueString(wxString::FromUTF8(s.description.c_str()));
	  w.Key("version");     w.ValueString(wxString::FromUTF8(s.version.c_str()));
	  w.Key("address");     w.ValueString(wxString::FromUTF8(s.address.c_str()));
	  w.Key("port");        w.ValueInt(static_cast<int64_t>(s.port));
	  w.Key("users");       w.ValueInt(static_cast<int64_t>(s.users));
	  w.Key("max_users");   w.ValueInt(static_cast<int64_t>(s.max_users));
	  w.Key("files");       w.ValueInt(static_cast<int64_t>(s.files));
	  w.Key("priority");    w.ValueString(wxString::FromUTF8(s.priority.c_str()));
	  w.Key("ping_ms");     w.ValueInt(static_cast<int64_t>(s.ping_ms));
	  w.Key("failed");      w.ValueInt(static_cast<int64_t>(s.failed));
	  w.Key("static");      w.ValueBool(s.is_static);
	w.EndObject();
}


void WriteCategoryObject(CJsonWriter &w, const webapi::CategorySnapshot &c)
{
	w.BeginObject();
	  w.Key("index");    w.ValueInt(static_cast<int64_t>(c.index));
	  w.Key("name");     w.ValueString(wxString::FromUTF8(c.name.c_str()));
	  w.Key("path");     w.ValueString(wxString::FromUTF8(c.path.c_str()));
	  w.Key("comment");  w.ValueString(wxString::FromUTF8(c.comment.c_str()));
	  w.Key("color");    w.ValueInt(static_cast<int64_t>(c.color));
	  w.Key("priority"); w.ValueString(wxString::FromUTF8(c.priority.c_str()));
	w.EndObject();
}

}  // namespace


CHttpServer::Response CApiDispatcher::HandleServers(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	return ListResponse(m_state, "servers", m_state.Servers(),
		WriteServerObject);
}


// --- Phase 5c — server lifecycle ---------------------------------------

namespace {

// Parse an integer ECID from a path capture. Returns false on
// negative, overflow, or non-digit content (the API expects positive
// 32-bit ECIDs from the URL).
bool ParseEcidPath(const std::string &s, std::uint32_t &out)
{
	if (s.empty()) return false;
	char *end = nullptr;
	const unsigned long v = std::strtoul(s.c_str(), &end, 10);
	if (end == s.c_str() || *end != '\0') return false;
	if (v > 0xFFFFFFFFu) return false;
	out = static_cast<std::uint32_t>(v);
	return true;
}


// Look up a server in the State cache by ECID. Returns false if
// no match — the handler then 404s.
bool FindServerByEcid(const webapi::CState &state, std::uint32_t ecid,
                      webapi::ServerSnapshot &out)
{
	const auto all = state.Servers();
	for (const auto &s : all) {
		if (s.ecid == ecid) { out = s; return true; }
	}
	return false;
}

}  // namespace


CHttpServer::Response CApiDispatcher::HandleServerAdd(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	std::string address;
	{
		const auto it = obj.find("address");
		if (it == obj.end() || !it->second.is<std::string>()) {
			return ErrorResponse(400, "bad_request",
				"required string field `address` is missing (\"host:port\")");
		}
		address = it->second.get<std::string>();
		const std::size_t colon = address.find(':');
		if (colon == std::string::npos || colon == 0
		    || colon == address.size() - 1) {
			return ErrorResponse(400, "bad_request",
				"`address` must be in \"host:port\" form");
		}
	}
	std::string name;
	{
		const auto it = obj.find("name");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`name` must be a string");
			}
			name = it->second.get<std::string>();
		}
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SERVER_ADD));
	ec_req->AddTag(CECTag(EC_TAG_SERVER_ADDRESS,
		wxString::FromUTF8(address.c_str())));
	if (!name.empty()) {
		ec_req->AddTag(CECTag(EC_TAG_SERVER_NAME,
			wxString::FromUTF8(name.c_str())));
	}

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for SERVER_ADD");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Inline refresh — the new server should be in the next /servers
	// response without waiting on the regular tick.
	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 201;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");      w.ValueBool(true);
	  w.Key("address"); w.ValueString(wxString::FromUTF8(address.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleServerConnect(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	std::uint32_t ecid = 0;
	if (!ParseEcidPath(ecid_str, ecid)) {
		return ErrorResponse(400, "bad_request",
			"path `{ecid}` must be a non-negative integer");
	}
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}
	webapi::ServerSnapshot srv;
	if (!FindServerByEcid(m_state, ecid, srv)) {
		return ErrorResponse(404, "not_found",
			"no server with that ECID in the current snapshot");
	}

	// EC_OP_SERVER_CONNECT routes through Get_EC_Response_Server,
	// which looks up the server by IPv4 lookup (ExternalConn.cpp:1266).
	// Build EC_TAG_SERVER with the IPv4 + port from our cache.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SERVER_CONNECT));
	ec_req->AddTag(CECTag(EC_TAG_SERVER, EC_IPv4_t(srv.ip, srv.port)));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for SERVER_CONNECT");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Connection state is observable via /status.ed2k.state — the
	// refresher tick will surface the change. Inline refresh so
	// /status reflects "connecting" immediately.
	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");   w.ValueBool(true);
	  w.Key("ecid"); w.ValueInt(static_cast<int64_t>(ecid));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleServerDelete(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	std::uint32_t ecid = 0;
	if (!ParseEcidPath(ecid_str, ecid)) {
		return ErrorResponse(400, "bad_request",
			"path `{ecid}` must be a non-negative integer");
	}
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}
	webapi::ServerSnapshot srv;
	if (!FindServerByEcid(m_state, ecid, srv)) {
		return ErrorResponse(404, "not_found",
			"no server with that ECID in the current snapshot");
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SERVER_REMOVE));
	ec_req->AddTag(CECTag(EC_TAG_SERVER, EC_IPv4_t(srv.ip, srv.port)));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for SERVER_REMOVE");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");   w.ValueBool(true);
	  w.Key("ecid"); w.ValueInt(static_cast<int64_t>(ecid));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleServerUpdateFromUrl(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();
	const auto it = obj.find("servers_url");
	if (it == obj.end() || !it->second.is<std::string>()) {
		return ErrorResponse(400, "bad_request",
			"required string field `servers_url` is missing");
	}
	const std::string &url = it->second.get<std::string>();
	if (url.empty()) {
		return ErrorResponse(400, "bad_request",
			"`servers_url` must not be empty");
	}
	// Light hygiene check — amuled will fetch this and bail if it's
	// nonsense, but rejecting obviously bad inputs at the API layer
	// gives a clearer error than the EC "amuled rejected" wrapper.
	if (url.compare(0, 7, "http://") != 0
	    && url.compare(0, 8, "https://") != 0) {
		return ErrorResponse(400, "bad_request",
			"`servers_url` must be an http:// or https:// URL");
	}

	std::unique_ptr<CECPacket> ec_req(
		new CECPacket(EC_OP_SERVER_UPDATE_FROM_URL));
	ec_req->AddTag(CECTag(EC_TAG_SERVERS_UPDATE_URL,
		wxString::FromUTF8(url.c_str())));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed");
	}
	std::string ec_err;
	if (IsEcFailedResponse(ec_resp, ec_err)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err.c_str());
	}
	delete ec_resp;

	// amuled streams the new server list into its CServerList
	// asynchronously over the next few ticks (download + parse + merge
	// in CServerList::UpdateServerMetFromURL). Run the inline
	// RefresherTick to grab whatever's already there, but the
	// `server_added` SSE events will continue to fire on subsequent
	// natural ticks as more entries land.
	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");          w.ValueBool(true);
	  w.Key("servers_url"); w.ValueString(wxString::FromUTF8(url.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


// Resolve "<ip>:<port>" from the URL into an ECID by walking the
// servers cache. Returns 0 on miss; the caller 404s.
static std::uint32_t ResolveServerEcidByAddress(
	const webapi::CState &state, const std::string &ip_port)
{
	const auto colon = ip_port.rfind(':');
	if (colon == std::string::npos) return 0;
	const std::string ip_str = ip_port.substr(0, colon);
	const std::string port_str = ip_port.substr(colon + 1);
	if (ip_str.empty() || port_str.empty()) return 0;
	char *end = nullptr;
	const unsigned long port = std::strtoul(port_str.c_str(), &end, 10);
	if (end == port_str.c_str() || *end != '\0'
	    || port == 0 || port > 0xFFFF) {
		return 0;
	}
	// Parse the IP — accept dotted-quad form OR a uint32 host-order
	// number that matches ServerSnapshot::ip. We compute both so we
	// can match either against what the cache holds.
	std::uint32_t ip_he = 0;
	{
		unsigned a_, b_, c_, d_;
		if (std::sscanf(ip_str.c_str(), "%u.%u.%u.%u",
		                &a_, &b_, &c_, &d_) == 4
		    && a_ <= 255 && b_ <= 255 && c_ <= 255 && d_ <= 255) {
			ip_he = (a_) | (b_ << 8) | (c_ << 16) | (d_ << 24);
		}
	}
	for (const auto &s : state.Servers()) {
		if (s.port == static_cast<std::uint16_t>(port)
		    && (s.ip == ip_he || s.address == ip_port)) {
			return s.ecid;
		}
	}
	return 0;
}


CHttpServer::Response CApiDispatcher::HandleServerConnectByAddress(
	const CHttpServer::Request &req, const std::string &ip_port)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}
	const std::uint32_t ecid =
		ResolveServerEcidByAddress(m_state, ip_port);
	if (ecid == 0) {
		return ErrorResponse(404, "not_found",
			"no server matches that ip:port");
	}
	// Delegate to the ECID-keyed handler; passing the resolved ECID as
	// a decimal string keeps the contract uniform.
	std::ostringstream os; os << ecid;
	return HandleServerConnect(req, os.str());
}


CHttpServer::Response CApiDispatcher::HandleServerDeleteByAddress(
	const CHttpServer::Request &req, const std::string &ip_port)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}
	const std::uint32_t ecid =
		ResolveServerEcidByAddress(m_state, ip_port);
	if (ecid == 0) {
		return ErrorResponse(404, "not_found",
			"no server matches that ip:port");
	}
	std::ostringstream os; os << ecid;
	return HandleServerDelete(req, os.str());
}


CHttpServer::Response CApiDispatcher::HandleCategories(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	return ListResponse(m_state, "categories", m_state.Categories(),
		WriteCategoryObject);
}


CHttpServer::Response CApiDispatcher::HandleKad(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	const webapi::KadSnapshot k = m_state.Kad();
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  // Bare object (Q3 — Kad is a single resource, not a list).
	  w.Key("state");            w.ValueString(wxString::FromUTF8(k.state.c_str()));
	  w.Key("firewalled");       w.ValueBool(k.firewalled);
	  w.Key("firewalled_udp");   w.ValueBool(k.firewalled_udp);
	  w.Key("in_lan_mode");      w.ValueBool(k.in_lan_mode);
	  w.Key("ip");               w.ValueString(wxString::FromUTF8(k.ip.c_str()));
	  w.Key("network");
	  w.BeginObject();
	    w.Key("users"); w.ValueInt(static_cast<int64_t>(k.users));
	    w.Key("files"); w.ValueInt(static_cast<int64_t>(k.files));
	    w.Key("nodes"); w.ValueInt(static_cast<int64_t>(k.nodes));
	  w.EndObject();
	  w.Key("indexed");
	  w.BeginObject();
	    w.Key("sources");  w.ValueInt(static_cast<int64_t>(k.indexed_sources));
	    w.Key("keywords"); w.ValueInt(static_cast<int64_t>(k.indexed_keywords));
	    w.Key("notes");    w.ValueInt(static_cast<int64_t>(k.indexed_notes));
	    w.Key("load");     w.ValueInt(static_cast<int64_t>(k.indexed_load));
	  w.EndObject();
	  w.Key("buddy");
	  w.BeginObject();
	    w.Key("status"); w.ValueString(wxString::FromUTF8(k.buddy_status.c_str()));
	    w.Key("ip");     w.ValueString(wxString::FromUTF8(k.buddy_ip.c_str()));
	    w.Key("port");   w.ValueInt(static_cast<int64_t>(k.buddy_port));
	  w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


namespace {

// `?tail=N` parser. Returns 0 if the query is absent / unparseable;
// the caller's contract is "0 means return everything". Negative or
// non-numeric values clamp to 0.
std::size_t ParseTailParam(const std::string &query)
{
	const auto qmap = web_api_path::ParseQuery(query);
	const auto it = qmap.find("tail");
	if (it == qmap.end()) return 0;
	const long n = std::atol(it->second.c_str());
	if (n <= 0) return 0;
	// Cap at 100k lines so a bogus `?tail=2147483647` doesn't try to
	// serialise the entire wxString through the JSON escaper.
	const long capped = std::min<long>(n, 100000);
	return static_cast<std::size_t>(capped);
}


// Return a copy of `all` containing at most `tail` trailing lines.
// `tail == 0` means "all lines" (no tailing).
std::vector<std::string> SliceTail(const std::vector<std::string> &all,
                                   std::size_t tail)
{
	if (tail == 0 || all.size() <= tail) return all;
	return std::vector<std::string>(
		all.begin() + (all.size() - tail), all.end());
}


// For a single-string log (e.g. /logs/serverinfo), `?tail=N` slices
// at line boundaries from the END so the first line of the response
// is always whole. tail=0 returns the input verbatim.
std::string TailString(const std::string &text, std::size_t tail_lines)
{
	if (tail_lines == 0 || text.empty()) return text;
	// Walk backwards counting newlines until we've found `tail_lines`
	// of them; whatever's after the last seen newline becomes the
	// response.
	std::size_t pos = text.size();
	std::size_t seen = 0;
	while (pos > 0 && seen < tail_lines) {
		--pos;
		if (text[pos] == '\n') ++seen;
	}
	// Advance past the leading '\n' so the response doesn't start
	// with a blank line.
	if (pos < text.size() && text[pos] == '\n') ++pos;
	return text.substr(pos);
}

}  // namespace


namespace {

void WriteStatsNode(CJsonWriter &w, const webapi::StatsTreeNode &n)
{
	w.BeginObject();
	  w.Key("label"); w.ValueString(wxString::FromUTF8(n.label.c_str()));
	  w.Key("children");
	  w.BeginArray();
	  for (const auto &c : n.children) WriteStatsNode(w, c);
	  w.EndArray();
	w.EndObject();
}


// Render an array of (t, value) points walking backwards from
// snapshot_at. Earliest sample sits at points[start] and corresponds
// to `snapshot_at - (samples.size()-1)*interval`; most recent sits
// at `snapshot_at`.
void WritePointArray(CJsonWriter &w,
                     const std::vector<std::uint32_t> &samples,
                     std::time_t snapshot_at,
                     std::uint32_t interval,
                     std::size_t max_width)
{
	w.BeginArray();
	if (samples.empty()) { w.EndArray(); return; }
	const std::size_t start = (max_width > 0 && samples.size() > max_width)
		? samples.size() - max_width : 0;
	for (std::size_t i = start; i < samples.size(); ++i) {
		const std::time_t t = snapshot_at
			- static_cast<std::time_t>(
				(samples.size() - 1 - i) * interval);
		w.BeginObject();
		  w.Key("t");        w.ValueString(wxString::FromUTF8(
		                       webapi::FormatIso8601Utc(t).c_str()));
		  w.Key("t_unix");   w.ValueInt(static_cast<int64_t>(t));
		  w.Key("value");    w.ValueInt(static_cast<int64_t>(samples[i]));
		w.EndObject();
	}
	w.EndArray();
}


void WriteSearchObject(CJsonWriter &w, const webapi::SearchResult &r)
{
	w.BeginObject();
	  w.Key("hash");         w.ValueString(wxString::FromUTF8(r.hash.c_str()));
	  w.Key("name");         w.ValueString(wxString::FromUTF8(r.name.c_str()));
	  w.Key("size");         w.ValueInt(static_cast<int64_t>(r.size));
	  w.Key("sources");
	  w.BeginObject();
	    w.Key("total");      w.ValueInt(static_cast<int64_t>(r.source_count));
	    w.Key("complete");   w.ValueInt(static_cast<int64_t>(r.complete_source_count));
	  w.EndObject();
	  w.Key("already_have"); w.ValueBool(r.already_have);
	  w.Key("rating");       w.ValueInt(static_cast<int64_t>(r.rating));
	w.EndObject();
}

}  // namespace


CHttpServer::Response CApiDispatcher::HandleStatsTree(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	// Phase 4g lazy-fetch with 1 s TTL coalescing. The fetcher runs
	// the EC roundtrip under m_app's m_ec_mtx (SendRecvSerialized);
	// concurrent burst reads serialize on m_stats_tree_cache's mutex
	// and the second waiter reads the just-stored value.
	auto pair = m_stats_tree_cache.GetOrFetch(
		std::chrono::milliseconds(1000),
		[this]() -> TtlPair_StatsTree {
			std::unique_ptr<CECPacket> req_ec(
				new CECPacket(EC_OP_GET_STATSTREE, EC_DETAIL_WEB));
			req_ec->AddTag(CECTag(EC_TAG_STATTREE_CAPPING,
				static_cast<std::uint8_t>(0)));
			const CECPacket *resp = m_app.SendRecvSerialized(req_ec.get());
			webapi::StatsTreeNode tree;
			std::time_t ts = 0;
			if (resp) {
				webapi::ParseStatsTreeFromPacket(resp, tree);
				ts = std::time(nullptr);
				delete resp;
			}
			return TtlPair_StatsTree(std::move(tree), ts);
		});

	if (pair.second == 0) {
		return ErrorResponse(503, "ec_unavailable",
			"EC fetch failed for stats tree; amuled may be disconnected");
	}

	const webapi::StatsTreeNode &root = pair.first;
	const std::time_t ts = pair.second;

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	// Phase 7.1: snapshot_at retired in favour of the ETag (Phase 7)
	// as the cache validator. The TtlPair_StatsTree still tracks the
	// fetched-at time internally (drives the 1 s TTL coalescer) — it
	// just isn't surfaced any more.
	(void) ts;
	CJsonWriter w;
	w.BeginObject();
	  w.Key("nodes");
	  w.BeginArray();
	  for (const auto &child : root.children) WriteStatsNode(w, child);
	  w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleStatsGraph(
	const CHttpServer::Request &req, const std::string &graph)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	// Validate the graph name BEFORE fetching — saves an EC roundtrip
	// on tab-complete typos hitting /stats/graphs/<bogus>.
	const char *unit = nullptr;
	if      (graph == "download")    { unit = "bps"; }
	else if (graph == "upload")      { unit = "bps"; }
	else if (graph == "connections") { unit = "count"; }
	else if (graph == "kad")         { unit = "count"; }
	else {
		return ErrorResponse(404, "not_found",
			"unknown graph; expected one of: download, upload, connections, kad");
	}

	// Lazy-fetch the full 4-series graph bundle (one EC call serves
	// all 4 named graphs, so the cache shares across concurrent
	// requests for different graph names).
	auto pair = m_stats_graphs_cache.GetOrFetch(
		std::chrono::milliseconds(1000),
		[this]() -> TtlPair_StatsGraphs {
			std::unique_ptr<CECPacket> req_ec(new CECPacket(EC_OP_GET_STATSGRAPHS));
			req_ec->AddTag(CECTag(EC_TAG_STATSGRAPH_SCALE,
				static_cast<std::uint16_t>(1)));
			req_ec->AddTag(CECTag(EC_TAG_STATSGRAPH_WIDTH,
				static_cast<std::uint16_t>(1800)));
			const CECPacket *resp = m_app.SendRecvSerialized(req_ec.get());
			webapi::StatsGraphs g;
			std::time_t ts = 0;
			if (resp) {
				webapi::ParseGraphsFromPacket(resp, g);
				ts = std::time(nullptr);
				delete resp;
			}
			return TtlPair_StatsGraphs(std::move(g), ts);
		});

	if (pair.second == 0) {
		return ErrorResponse(503, "ec_unavailable",
			"EC fetch failed for stats graphs; amuled may be disconnected");
	}

	const webapi::StatsGraphs &g = pair.first;
	const std::vector<std::uint32_t> *series = nullptr;
	if      (graph == "download")    { series = &g.download_bps; }
	else if (graph == "upload")      { series = &g.upload_bps;   }
	else if (graph == "connections") { series = &g.connections;  }
	else /* kad */                   { series = &g.kad_nodes;    }

	// ?width=N — clamp the sample count returned. 0 / absent means
	// "everything we have" (up to the 1800-sample window we ask for).
	std::string query;
	const std::size_t q = req.target.find('?');
	if (q != std::string::npos) query = req.target.substr(q + 1);
	std::size_t width = 0;
	{
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("width");
		if (it != qmap.end()) {
			const long n = std::atol(it->second.c_str());
			if (n > 0) width = static_cast<std::size_t>(std::min<long>(n, 1800));
		}
	}

	const std::time_t ts = pair.second;
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("graph");            w.ValueString(wxString::FromUTF8(graph.c_str()));
	  w.Key("unit");             w.ValueString(wxString::FromUTF8(unit));
	  w.Key("interval_seconds"); w.ValueInt(static_cast<int64_t>(g.interval_seconds));
	  // Phase 7.1: snapshot_at retired from the response. WritePointArray
	  // still consumes `ts` to compute per-point timestamps (anchoring
	  // the time-series backwards from the fetch wall-clock).
	  w.Key("points");
	  WritePointArray(w, *series, ts, g.interval_seconds, width);
	  // Session totals tag along — clients showing "this session: X GB
	  // down" don't need a separate roundtrip.
	  w.Key("session");
	  w.BeginObject();
	    w.Key("download_bytes"); w.ValueInt(static_cast<int64_t>(g.session_download_bytes));
	    w.Key("upload_bytes");   w.ValueInt(static_cast<int64_t>(g.session_upload_bytes));
	    w.Key("kad_bytes");      w.ValueInt(static_cast<int64_t>(g.session_kad_bytes));
	  w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleSearchResults(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	// Lazy-fetch via TtlCache. Phase 5+ POST /search will call
	// m_search_cache.Invalidate() so the next GET sees fresh results
	// without waiting for the TTL to expire.
	auto pair = m_search_cache.GetOrFetch(
		std::chrono::milliseconds(1000),
		[this]() -> TtlPair_Search {
			std::unique_ptr<CECPacket> req_ec(
				new CECPacket(EC_OP_SEARCH_RESULTS, EC_DETAIL_FULL));
			const CECPacket *resp = m_app.SendRecvSerialized(req_ec.get());
			std::map<std::uint32_t, webapi::SearchResult> results;
			std::time_t ts = 0;
			if (resp) {
				webapi::ApplySearchFull(resp, results);
				ts = std::time(nullptr);
				delete resp;
			}
			return TtlPair_Search(std::move(results), ts);
		});

	if (pair.second == 0) {
		return ErrorResponse(503, "ec_unavailable",
			"EC fetch failed for search results; amuled may be disconnected");
	}

	std::vector<webapi::SearchResult> results_vec;
	results_vec.reserve(pair.first.size());
	for (const auto &kv : pair.first) results_vec.push_back(kv.second);

	const std::time_t ts = pair.second;
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	// Phase 7.1: snapshot_at retired. ETag (Phase 7) is the cache
	// validator; POST /search invalidates m_search_cache so the next
	// GET's body changes when amuled has fresh results — ETag tracks
	// that change.
	(void) ts;
	CJsonWriter w;
	w.BeginObject();
	  w.Key("results");
	  w.BeginArray();
	  for (const auto &item : results_vec) WriteSearchObject(w, item);
	  w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleLogAmule(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	// Extract the query string from the raw target (request.target is
	// the literal URI, e.g. "/api/v0/logs/amule?tail=200").
	std::string path, query;
	const size_t q = req.target.find('?');
	if (q != std::string::npos) {
		query = req.target.substr(q + 1);
	}
	const std::size_t tail = ParseTailParam(query);
	const auto all = m_state.AmuleLog();
	const auto sliced = SliceTail(all, tail);

	// Bare object (Q3): single resource, no list envelope.
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("lines");
	  w.BeginArray();
	  for (const auto &line : sliced) {
	    w.ValueString(wxString::FromUTF8(line.c_str()));
	  }
	  w.EndArray();
	  // Operator-debug metadata: total cached + how many we returned.
	  // Lets a client paging through history know what it missed.
	  w.Key("total_cached");  w.ValueInt(static_cast<int64_t>(all.size()));
	  w.Key("returned");      w.ValueInt(static_cast<int64_t>(sliced.size()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleLogAmuleReset(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_RESET_LOG));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed");
	}
	std::string ec_err;
	if (IsEcFailedResponse(ec_resp, ec_err)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err.c_str());
	}
	delete ec_resp;

	// Drop the in-process mirror. The refresher's append-only path
	// (AppendAmuleLog) can't shrink the cache, and EmitDiffsAndUpdate
	// already treats a size decrease as a silent truncation
	// (EventDiff.cpp's `amule_log.size() < prev.amule_log_count`
	// branch), so no spurious log_appended event fires on the next
	// tick.
	m_state.ClearAmuleLog();

	CHttpServer::Response r;
	r.status       = 204;
	r.content_type.clear();
	return r;
}


CHttpServer::Response CApiDispatcher::HandleLogServerinfo(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;

	std::string query;
	const size_t q = req.target.find('?');
	if (q != std::string::npos) {
		query = req.target.substr(q + 1);
	}
	const std::size_t tail = ParseTailParam(query);

	// Lazy-fetch via TtlCache. EC_OP_GET_SERVERINFO ships one
	// EC_TAG_STRING with the whole accumulated text — amuled rotates
	// it server-side so the size stays bounded.
	auto pair = m_server_info_cache.GetOrFetch(
		std::chrono::milliseconds(1000),
		[this]() -> TtlPair_ServerInfo {
			std::unique_ptr<CECPacket> req_ec(
				new CECPacket(EC_OP_GET_SERVERINFO));
			const CECPacket *resp = m_app.SendRecvSerialized(req_ec.get());
			webapi::ServerInfoLog log;
			std::time_t ts = 0;
			if (resp) {
				if (const CECTag *t = resp->GetFirstTagSafe()) {
					if (t->GetTagName() == EC_TAG_STRING) {
						log.text = std::string(t->GetStringData().utf8_str());
					}
				}
				ts = std::time(nullptr);
				delete resp;
			}
			return TtlPair_ServerInfo(std::move(log), ts);
		});

	if (pair.second == 0) {
		return ErrorResponse(503, "ec_unavailable",
			"EC fetch failed for server info; amuled may be disconnected");
	}

	const webapi::ServerInfoLog &log = pair.first;
	const std::string text = TailString(log.text, tail);

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("text");          w.ValueString(wxString::FromUTF8(text.c_str()));
	  // The total length lets a client decide whether to re-poll
	  // with a smaller `?tail=` for incremental display.
	  w.Key("total_bytes");   w.ValueInt(static_cast<int64_t>(log.text.size()));
	  w.Key("returned_bytes"); w.ValueInt(static_cast<int64_t>(text.size()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleLogServerinfoReset(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	std::unique_ptr<CECPacket> ec_req(
		new CECPacket(EC_OP_CLEAR_SERVERINFO));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed");
	}
	std::string ec_err;
	if (IsEcFailedResponse(ec_resp, ec_err)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err.c_str());
	}
	delete ec_resp;

	// Lazy cache for /logs/serverinfo would otherwise return stale
	// text until its 1 s TTL expires; force the next GET to re-fetch.
	m_server_info_cache.Invalidate();

	CHttpServer::Response r;
	r.status       = 204;
	r.content_type.clear();
	return r;
}


CHttpServer::Response CApiDispatcher::HandlePreferences(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	const webapi::PreferencesSnapshot p = m_state.Preferences();
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("general");
	  w.BeginObject();
	    w.Key("nickname");          w.ValueString(wxString::FromUTF8(p.nickname.c_str()));
	    w.Key("user_hash");         w.ValueString(wxString::FromUTF8(p.user_hash.c_str()));
	    w.Key("host_name");         w.ValueString(wxString::FromUTF8(p.host_name.c_str()));
	    w.Key("check_new_version"); w.ValueBool(p.check_new_version);
	  w.EndObject();
	  w.Key("connection");
	  w.BeginObject();
	    w.Key("max_upload_kbps");       w.ValueInt(static_cast<int64_t>(p.max_upload_kbps));
	    w.Key("max_download_kbps");     w.ValueInt(static_cast<int64_t>(p.max_download_kbps));
	    w.Key("max_upload_cap_kbps");   w.ValueInt(static_cast<int64_t>(p.max_upload_cap_kbps));
	    w.Key("max_download_cap_kbps"); w.ValueInt(static_cast<int64_t>(p.max_download_cap_kbps));
	    w.Key("slot_allocation");       w.ValueInt(static_cast<int64_t>(p.slot_allocation));
	    w.Key("tcp_port");              w.ValueInt(static_cast<int64_t>(p.tcp_port));
	    w.Key("udp_port");              w.ValueInt(static_cast<int64_t>(p.udp_port));
	    w.Key("udp_disabled");          w.ValueBool(p.udp_disabled);
	    w.Key("max_sources_per_file");  w.ValueInt(static_cast<int64_t>(p.max_sources_per_file));
	    w.Key("max_connections");       w.ValueInt(static_cast<int64_t>(p.max_connections));
	    w.Key("autoconnect");           w.ValueBool(p.autoconnect);
	    w.Key("reconnect");             w.ValueBool(p.reconnect);
	    w.Key("network_ed2k");          w.ValueBool(p.network_ed2k);
	    w.Key("network_kad");           w.ValueBool(p.network_kad);
	  w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


// --- Phase 5d — PATCH /preferences -------------------------------------

namespace {

// Helpers that pull (& validate) optional fields from a JSON object.
// Each returns true and writes `out` if the field is present and the
// right shape; returns false on absence. On wrong shape, writes
// `err_label` for the caller to relay to the client and returns false
// as well (errors take priority via the err_label out-param).
struct PrefsParseError {
	bool        is_error = false;
	std::string message;
};

// EC_OP_SET_PREFERENCES requires EC_DETAIL_FULL so the daemon honors
// boolean tags (CEC_Prefs_Packet::Apply checks
// `use_tag = (GetDetailLevel() == EC_DETAIL_FULL)` before calling
// ApplyBoolean). FULL is also what amulegui sends.
}  // namespace

CHttpServer::Response CApiDispatcher::HandlePreferencesPatch(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	// Body shape: { "general": {...}, "connection": {...} } — both
	// sub-objects optional, all fields within optional. Mirrors the
	// /preferences GET shape (Phase 4c) so a typical client read-
	// modify-write workflow doesn't have to translate between schemas.
	const picojson::object *general_obj    = nullptr;
	const picojson::object *connection_obj = nullptr;
	{
		const auto it = obj.find("general");
		if (it != obj.end()) {
			if (!it->second.is<picojson::object>()) {
				return ErrorResponse(400, "bad_request",
					"`general` must be an object");
			}
			general_obj = &it->second.get<picojson::object>();
		}
	}
	{
		const auto it = obj.find("connection");
		if (it != obj.end()) {
			if (!it->second.is<picojson::object>()) {
				return ErrorResponse(400, "bad_request",
					"`connection` must be an object");
			}
			connection_obj = &it->second.get<picojson::object>();
		}
	}

	if (general_obj == nullptr && connection_obj == nullptr) {
		return ErrorResponse(400, "bad_request",
			"request body must include at least one of `general` or "
			"`connection` sub-objects");
	}

	// Build the SET_PREFERENCES packet at EC_DETAIL_FULL (required for
	// boolean fields — Apply() gates ApplyBoolean on detail==FULL).
	std::unique_ptr<CECPacket> ec_req(
		new CECPacket(EC_OP_SET_PREFERENCES, EC_DETAIL_FULL));

	auto add_uint = [](CECTag &group, ec_tagname_t name,
	                   std::uint32_t v) {
		group.AddTag(CECTag(name, v));
	};
	auto add_bool = [](CECTag &group, ec_tagname_t name, bool v) {
		group.AddTag(CECTag(name, static_cast<std::uint8_t>(v ? 1 : 0)));
	};

	bool any_change = false;

	// --- General sub-object. -----------------------------------
	if (general_obj) {
		CECTag general(EC_TAG_PREFS_GENERAL, static_cast<std::uint32_t>(0));
		bool any_general = false;
		{
			const auto it = general_obj->find("nickname");
			if (it != general_obj->end()) {
				if (!it->second.is<std::string>()) {
					return ErrorResponse(400, "bad_request",
						"`general.nickname` must be a string");
				}
				const std::string &v = it->second.get<std::string>();
				general.AddTag(CECTag(EC_TAG_USER_NICK,
					wxString::FromUTF8(v.c_str())));
				any_general = true;
			}
		}
		{
			const auto it = general_obj->find("check_new_version");
			if (it != general_obj->end()) {
				if (!it->second.is<bool>()) {
					return ErrorResponse(400, "bad_request",
						"`general.check_new_version` must be a bool");
				}
				add_bool(general, EC_TAG_GENERAL_CHECK_NEW_VERSION,
					it->second.get<bool>());
				any_general = true;
			}
		}
		if (any_general) {
			ec_req->AddTag(general);
			any_change = true;
		}
	}

	// --- Connection sub-object. --------------------------------
	if (connection_obj) {
		CECTag connection(EC_TAG_PREFS_CONNECTIONS, static_cast<std::uint32_t>(0));
		bool any_conn = false;

		// Helper for "uint field" — repeats for each numeric pref.
		auto take_uint = [&](const char *key, ec_tagname_t name,
		                     std::uint32_t max) -> CHttpServer::Response {
			const auto it = connection_obj->find(key);
			if (it == connection_obj->end()) {
				CHttpServer::Response ok;
				ok.status = 0;   // sentinel: not present
				return ok;
			}
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request",
					"connection field must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > max) {
				return ErrorResponse(400, "bad_request",
					"connection field out of range");
			}
			add_uint(connection, name, static_cast<std::uint32_t>(v));
			any_conn = true;
			CHttpServer::Response ok;
			ok.status = 200;
			return ok;
		};
		auto take_bool = [&](const char *key, ec_tagname_t name) -> CHttpServer::Response {
			const auto it = connection_obj->find(key);
			if (it == connection_obj->end()) {
				CHttpServer::Response ok;
				ok.status = 0;
				return ok;
			}
			if (!it->second.is<bool>()) {
				return ErrorResponse(400, "bad_request",
					"connection field must be a bool");
			}
			add_bool(connection, name, it->second.get<bool>());
			any_conn = true;
			CHttpServer::Response ok;
			ok.status = 200;
			return ok;
		};

		// Uints — kbps caps in [0, 1_000_000_000], ports in [0, 65535].
		const std::uint32_t kbps_max = 1000000000u;
		auto r1 = take_uint("max_upload_kbps",       EC_TAG_CONN_MAX_UL,            kbps_max);
		if (r1.status >= 400) return r1;
		auto r2 = take_uint("max_download_kbps",     EC_TAG_CONN_MAX_DL,            kbps_max);
		if (r2.status >= 400) return r2;
		auto r3 = take_uint("max_upload_cap_kbps",   EC_TAG_CONN_UL_CAP,            kbps_max);
		if (r3.status >= 400) return r3;
		auto r4 = take_uint("max_download_cap_kbps", EC_TAG_CONN_DL_CAP,            kbps_max);
		if (r4.status >= 400) return r4;
		auto r5 = take_uint("slot_allocation",       EC_TAG_CONN_SLOT_ALLOCATION,   65535);
		if (r5.status >= 400) return r5;
		auto r6 = take_uint("tcp_port",              EC_TAG_CONN_TCP_PORT,          65535);
		if (r6.status >= 400) return r6;
		auto r7 = take_uint("udp_port",              EC_TAG_CONN_UDP_PORT,          65535);
		if (r7.status >= 400) return r7;
		auto r8 = take_uint("max_sources_per_file",  EC_TAG_CONN_MAX_FILE_SOURCES,  65535);
		if (r8.status >= 400) return r8;
		auto r9 = take_uint("max_connections",       EC_TAG_CONN_MAX_CONN,          65535);
		if (r9.status >= 400) return r9;

		// Bools.
		auto rb1 = take_bool("udp_disabled",  EC_TAG_CONN_UDP_DISABLE);
		if (rb1.status >= 400) return rb1;
		auto rb2 = take_bool("autoconnect",   EC_TAG_CONN_AUTOCONNECT);
		if (rb2.status >= 400) return rb2;
		auto rb3 = take_bool("reconnect",     EC_TAG_CONN_RECONNECT);
		if (rb3.status >= 400) return rb3;
		auto rb4 = take_bool("network_ed2k",  EC_TAG_NETWORK_ED2K);
		if (rb4.status >= 400) return rb4;
		auto rb5 = take_bool("network_kad",   EC_TAG_NETWORK_KADEMLIA);
		if (rb5.status >= 400) return rb5;

		if (any_conn) {
			ec_req->AddTag(connection);
			any_change = true;
		}
	}

	if (!any_change) {
		return ErrorResponse(400, "bad_request",
			"request body did not include any known pref fields");
	}

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for SET_PREFERENCES");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Inline refresh — the GET below + the next /preferences must
	// reflect the post-mutation state without waiting on the regular
	// tick.
	(void) RefresherTick(m_app, m_state);

	// Return the updated /preferences shape so consumers can confirm
	// what landed without a follow-up GET.
	const webapi::PreferencesSnapshot p = m_state.Preferences();
	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("general");
	  w.BeginObject();
	    w.Key("nickname");          w.ValueString(wxString::FromUTF8(p.nickname.c_str()));
	    w.Key("user_hash");         w.ValueString(wxString::FromUTF8(p.user_hash.c_str()));
	    w.Key("host_name");         w.ValueString(wxString::FromUTF8(p.host_name.c_str()));
	    w.Key("check_new_version"); w.ValueBool(p.check_new_version);
	  w.EndObject();
	  w.Key("connection");
	  w.BeginObject();
	    w.Key("max_upload_kbps");       w.ValueInt(static_cast<int64_t>(p.max_upload_kbps));
	    w.Key("max_download_kbps");     w.ValueInt(static_cast<int64_t>(p.max_download_kbps));
	    w.Key("max_upload_cap_kbps");   w.ValueInt(static_cast<int64_t>(p.max_upload_cap_kbps));
	    w.Key("max_download_cap_kbps"); w.ValueInt(static_cast<int64_t>(p.max_download_cap_kbps));
	    w.Key("slot_allocation");       w.ValueInt(static_cast<int64_t>(p.slot_allocation));
	    w.Key("tcp_port");              w.ValueInt(static_cast<int64_t>(p.tcp_port));
	    w.Key("udp_port");              w.ValueInt(static_cast<int64_t>(p.udp_port));
	    w.Key("udp_disabled");          w.ValueBool(p.udp_disabled);
	    w.Key("max_sources_per_file");  w.ValueInt(static_cast<int64_t>(p.max_sources_per_file));
	    w.Key("max_connections");       w.ValueInt(static_cast<int64_t>(p.max_connections));
	    w.Key("autoconnect");           w.ValueBool(p.autoconnect);
	    w.Key("reconnect");             w.ValueBool(p.reconnect);
	    w.Key("network_ed2k");          w.ValueBool(p.network_ed2k);
	    w.Key("network_kad");           w.ValueBool(p.network_kad);
	  w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


// --- Phase 5e — connection control -------------------------------------

namespace {

// Issue a single-shot mutation EC packet (no body), check the
// response, run RefresherTick inline, return a standard
// `{ok: true, message?: "..."}` response. Used by every connection-
// control endpoint where the EC op is parameterless.
CHttpServer::Response SimpleConnControlOp(
	CamuleapiApp &app, webapi::CState &state,
	ec_opcode_t op, unsigned http_status)
{
	std::unique_ptr<CECPacket> ec_req(new CECPacket(op));
	const CECPacket *ec_resp = app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	// amuled's CONNECT/DISCONNECT return EC_OP_STRINGS with a status
	// message. We surface the message verbatim so consumers see what
	// amuled would have shown in its UI.
	std::string message;
	if (ec_resp) {
		for (CECPacket::const_iterator it = ec_resp->begin();
		     it != ec_resp->end(); ++it) {
			const CECTag *t = &*it;
			if (t->GetTagName() == EC_TAG_STRING) {
				if (!message.empty()) message += "; ";
				message += std::string(t->GetStringData().utf8_str());
			}
		}
	}
	delete ec_resp;

	(void) RefresherTick(app, state);

	CHttpServer::Response r;
	r.status       = http_status;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");      w.ValueBool(true);
	  if (!message.empty()) {
		  w.Key("message"); w.ValueString(wxString::FromUTF8(message.c_str()));
	  }
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

}  // namespace


CHttpServer::Response CApiDispatcher::HandleNetworksConnect(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;
	return SimpleConnControlOp(m_app, m_state, EC_OP_CONNECT, 202);
}


CHttpServer::Response CApiDispatcher::HandleNetworksDisconnect(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	// Optional `{"network": "ed2k" | "kad" | "both"}` selector. Default
	// "both" (preserves the original parameterless contract). Empty
	// body is fine — that's the most common shape and matches the v0
	// contract callers built against.
	std::string network = "both";
	if (!req.body.empty()) {
		picojson::value root;
		std::string parse_err;
		if (!ParseJsonObjectBody(req.body, root, parse_err)) {
			return ErrorResponse(400, "bad_request", parse_err.c_str());
		}
		const auto &obj = root.get<picojson::object>();
		const auto it = obj.find("network");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`network` must be one of \"ed2k\", \"kad\", \"both\"");
			}
			network = it->second.get<std::string>();
			if (network != "ed2k" && network != "kad" && network != "both") {
				return ErrorResponse(400, "bad_request",
					"`network` must be one of \"ed2k\", \"kad\", \"both\"");
			}
		}
	}

	if (network == "ed2k") {
		return SimpleConnControlOp(m_app, m_state,
			EC_OP_SERVER_DISCONNECT, 200);
	}
	if (network == "kad") {
		return SimpleConnControlOp(m_app, m_state,
			EC_OP_KAD_STOP, 200);
	}
	// "both": amuled's EC_OP_DISCONNECT short-circuits to both
	// SERVER_DISCONNECT and KAD_STOP in one EC roundtrip.
	return SimpleConnControlOp(m_app, m_state, EC_OP_DISCONNECT, 200);
}


CHttpServer::Response CApiDispatcher::HandleKadConnect(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;
	return SimpleConnControlOp(m_app, m_state, EC_OP_KAD_START, 202);
}


CHttpServer::Response CApiDispatcher::HandleKadDisconnect(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;
	return SimpleConnControlOp(m_app, m_state, EC_OP_KAD_STOP, 200);
}


CHttpServer::Response CApiDispatcher::HandleKadBootstrap(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	// Body: {"ip": "1.2.3.4" | <uint32 host-order>, "port": <uint16>}.
	// Accept the IP either as a dotted-quad string (friendly) OR as
	// a uint32 (matches the EC tag's wire shape directly).
	std::uint32_t ip_he = 0;
	{
		const auto it = obj.find("ip");
		if (it == obj.end()) {
			return ErrorResponse(400, "bad_request",
				"required field `ip` is missing");
		}
		if (it->second.is<std::string>()) {
			// Dotted-quad string. Parse with strtoul on each octet.
			const std::string &s = it->second.get<std::string>();
			unsigned a_, b_, c_, d_;
			if (std::sscanf(s.c_str(), "%u.%u.%u.%u",
			                &a_, &b_, &c_, &d_) != 4
			    || a_ > 255 || b_ > 255 || c_ > 255 || d_ > 255) {
				return ErrorResponse(400, "bad_request",
					"`ip` must be a dotted-quad IPv4 address or a "
					"host-order uint32");
			}
			ip_he = (a_) | (b_ << 8) | (c_ << 16) | (d_ << 24);
		} else if (it->second.is<double>()) {
			const double v = it->second.get<double>();
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request",
					"`ip` uint32 out of range");
			}
			ip_he = static_cast<std::uint32_t>(v);
		} else {
			return ErrorResponse(400, "bad_request",
				"`ip` must be a string or number");
		}
	}
	std::uint16_t port = 0;
	{
		const auto it = obj.find("port");
		if (it == obj.end() || !it->second.is<double>()) {
			return ErrorResponse(400, "bad_request",
				"required numeric field `port` is missing");
		}
		const double v = it->second.get<double>();
		if (v < 0 || v > 65535) {
			return ErrorResponse(400, "bad_request",
				"`port` must be in [0, 65535]");
		}
		port = static_cast<std::uint16_t>(v);
	}

	std::unique_ptr<CECPacket> ec_req(
		new CECPacket(EC_OP_KAD_BOOTSTRAP_FROM_IP));
	ec_req->AddTag(CECTag(EC_TAG_BOOTSTRAP_IP, ip_he));
	ec_req->AddTag(CECTag(EC_TAG_BOOTSTRAP_PORT, port));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for KAD_BOOTSTRAP_FROM_IP");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");   w.ValueBool(true);
	  w.Key("ip");   w.ValueInt(static_cast<int64_t>(ip_he));
	  w.Key("port"); w.ValueInt(static_cast<int64_t>(port));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


// --- Phase 5f — shared file priority PATCH -----------------------------

namespace {

// Inverse of SharedPriorityName in Refresher.cpp. Wire form mirrors
// the /shared[].priority enum: bare priorities ("low", "normal",
// "high", "release", "very_low", "auto") + their *_auto variants
// (encoded by amule as `prio + 10`). Returns false on unknown enum.
bool SharedPriorityToCode(const std::string &name, std::uint8_t &out)
{
	if      (name == "very_low")      { out = PR_VERY_LOW;                 return true; }
	else if (name == "very_low_auto") { out = PR_VERY_LOW + 10;            return true; }
	else if (name == "low")           { out = PR_LOW;                      return true; }
	else if (name == "low_auto")      { out = PR_LOW + 10;                 return true; }
	else if (name == "normal")        { out = PR_NORMAL;                   return true; }
	else if (name == "normal_auto")   { out = PR_NORMAL + 10;              return true; }
	else if (name == "high")          { out = PR_HIGH;                     return true; }
	else if (name == "high_auto")     { out = PR_HIGH + 10;                return true; }
	else if (name == "release")       { out = PR_VERYHIGH;                 return true; }
	else if (name == "release_auto")  { out = PR_VERYHIGH + 10;            return true; }
	else if (name == "auto")          { out = PR_AUTO;                     return true; }
	return false;
}

}  // namespace


CHttpServer::Response CApiDispatcher::HandleSharedPatch(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	// Canonicalise the URL hash to lowercase.
	std::string needle = hash;
	std::transform(needle.begin(), needle.end(), needle.begin(),
		[](unsigned char c) { return std::tolower(c); });

	// Look up the entry in the /shared cache (the per-tick Refresher
	// snapshot). 404 if unknown.
	webapi::SharedSnapshot s;
	bool found = false;
	for (const auto &x : m_state.Shared()) {
		if (x.hash == needle) { s = x; found = true; break; }
	}
	if (!found) {
		return ErrorResponse(404, "not_found",
			"no shared file with that hash");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	const auto pit = obj.find("priority");
	if (pit == obj.end()) {
		return ErrorResponse(400, "bad_request",
			"request body must include `priority`");
	}
	if (!pit->second.is<std::string>()) {
		return ErrorResponse(400, "bad_request",
			"`priority` must be a wire-string enum");
	}
	std::uint8_t code = 0;
	if (!SharedPriorityToCode(pit->second.get<std::string>(), code)) {
		return ErrorResponse(400, "bad_request",
			"`priority` must be one of "
			"very_low, low, normal, high, release, auto "
			"(and their *_auto variants)");
	}

	CMD4Hash file_hash;
	if (!HashFromHex(needle, file_hash)) {
		return ErrorResponse(500, "internal_error",
			"failed to decode file hash");
	}
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SHARED_SET_PRIO));
	CECTag hash_tag(EC_TAG_PARTFILE, file_hash);
	hash_tag.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, code));
	ec_req->AddTag(hash_tag);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for SHARED_SET_PRIO");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void) RefresherTick(m_app, m_state);

	// Re-read post-mutation. Fall back to prior copy if evicted.
	webapi::SharedSnapshot s_after = s;
	for (const auto &x : m_state.Shared()) {
		if (x.hash == needle) { s_after = x; break; }
	}

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteSharedObject(w, s_after);
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleSharedReload(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;
	// EC_OP_SHAREDFILES_RELOAD: amuled re-walks every configured share
	// root and re-publishes the contents. Synchronous on amuled's side
	// but bounded by I/O over the share tree — typical small libraries
	// complete in well under a second. Inline RefresherTick re-pulls
	// the shared-files cache so SSE subscribers see `shared_added` /
	// `_removed` events for the delta before the response lands.
	return SimpleConnControlOp(m_app, m_state,
		EC_OP_SHAREDFILES_RELOAD, 202);
}


// --- Phase 5g — categories CRUD ---------------------------------------

namespace {

// Parse a uint8 index from a URL capture. Categories are 0..255 (the
// EC tag stores them as uint8). Returns false on overflow, negative,
// or non-digit content.
bool ParseCategoryIndex(const std::string &s, std::uint8_t &out)
{
	if (s.empty()) return false;
	char *end = nullptr;
	const unsigned long v = std::strtoul(s.c_str(), &end, 10);
	if (end == s.c_str() || *end != '\0') return false;
	if (v > 255) return false;
	out = static_cast<std::uint8_t>(v);
	return true;
}


// Inverse of CategoryPriorityName (Refresher.cpp's ParseCategoryTag).
// Categories use the SAME priority enum as downloads, including the
// `+10` auto-flag offset. Maps wire strings to PR_* codes.
bool CategoryPriorityToCode(const std::string &name, std::uint8_t &out)
{
	if      (name == "very_low") { out = PR_VERY_LOW;       return true; }
	else if (name == "low")      { out = PR_LOW;            return true; }
	else if (name == "normal")   { out = PR_NORMAL;         return true; }
	else if (name == "high")     { out = PR_HIGH;           return true; }
	else if (name == "release")  { out = PR_VERYHIGH;       return true; }
	else if (name == "auto")     { out = PR_AUTO;           return true; }
	return false;
}


// Build the CEC_Category_Tag-shaped tag amuled expects. The shape is:
//   parent tag EC_TAG_CATEGORY with the index as the int payload,
//   nested children:
//     EC_TAG_CATEGORY_TITLE   (string, "name" in our API)
//     EC_TAG_CATEGORY_PATH    (string, "path")
//     EC_TAG_CATEGORY_COMMENT (string, "comment")
//     EC_TAG_CATEGORY_COLOR   (uint32)
//     EC_TAG_CATEGORY_PRIO    (uint8)
//
// For CREATE the index is `0xFFFFFFFF` (sentinel: amuled assigns the
// next free slot). For UPDATE we pass the actual index. For DELETE
// the tag is just `(EC_TAG_CATEGORY, index)` — no children needed.
CECTag BuildCategoryTag(std::uint32_t index,
                        const std::string &name,
                        const std::string &path,
                        const std::string &comment,
                        std::uint32_t color,
                        std::uint8_t  prio)
{
	CECTag t(EC_TAG_CATEGORY, index);
	t.AddTag(CECTag(EC_TAG_CATEGORY_TITLE,   wxString::FromUTF8(name.c_str())));
	t.AddTag(CECTag(EC_TAG_CATEGORY_PATH,    wxString::FromUTF8(path.c_str())));
	t.AddTag(CECTag(EC_TAG_CATEGORY_COMMENT, wxString::FromUTF8(comment.c_str())));
	t.AddTag(CECTag(EC_TAG_CATEGORY_COLOR,   color));
	t.AddTag(CECTag(EC_TAG_CATEGORY_PRIO,    prio));
	return t;
}


// Helper to extract optional name/path/comment/color/priority from a
// JSON object. Populates the out-params; returns an error response
// on shape violations. The `is_create` flag enables required-field
// enforcement: CREATE needs a name, UPDATE/PATCH treat all as
// optional.
struct CategoryFields {
	std::string  name;
	std::string  path;
	std::string  comment;
	std::uint32_t color = 0;
	std::uint8_t  prio  = PR_NORMAL;
	bool          has_name    = false;
	bool          has_path    = false;
	bool          has_comment = false;
	bool          has_color   = false;
	bool          has_prio    = false;
};


CHttpServer::Response ParseCategoryFields(const picojson::object &obj,
                                          CategoryFields &out)
{
	auto get_string = [&obj](const char *key, std::string &dst,
	                         bool &has) -> CHttpServer::Response {
		const auto it = obj.find(key);
		if (it == obj.end()) {
			CHttpServer::Response ok; ok.status = 0; return ok;
		}
		if (!it->second.is<std::string>()) {
			return ErrorResponse(400, "bad_request",
				"category field must be a string");
		}
		dst = it->second.get<std::string>();
		has = true;
		CHttpServer::Response ok; ok.status = 200; return ok;
	};

	auto r1 = get_string("name",    out.name,    out.has_name);    if (r1.status >= 400) return r1;
	auto r2 = get_string("path",    out.path,    out.has_path);    if (r2.status >= 400) return r2;
	auto r3 = get_string("comment", out.comment, out.has_comment); if (r3.status >= 400) return r3;
	{
		const auto it = obj.find("color");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request",
					"`color` must be a uint32");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request",
					"`color` out of range");
			}
			out.color = static_cast<std::uint32_t>(v);
			out.has_color = true;
		}
	}
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`priority` must be a wire-string enum");
			}
			if (!CategoryPriorityToCode(it->second.get<std::string>(),
			                            out.prio)) {
				return ErrorResponse(400, "bad_request",
					"`priority` must be one of low, normal, high, "
					"release, very_low, auto");
			}
			out.has_prio = true;
		}
	}
	CHttpServer::Response ok; ok.status = 200; return ok;
}

}  // namespace


CHttpServer::Response CApiDispatcher::HandleCategoryCreate(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	CategoryFields f;
	auto err = ParseCategoryFields(obj, f);
	if (err.status >= 400) return err;
	if (!f.has_name || f.name.empty()) {
		return ErrorResponse(400, "bad_request",
			"required string field `name` is missing");
	}

	// CREATE: index sentinel is 0xFFFFFFFF — amuled assigns the next
	// free slot and returns NOOP on success.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_CREATE_CATEGORY));
	ec_req->AddTag(BuildCategoryTag(0xFFFFFFFFu, f.name, f.path,
		f.comment, f.color, f.prio));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for CREATE_CATEGORY");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void) RefresherTick(m_app, m_state);

	// Look up the newly-created category by name to get its assigned
	// index. (amuled's CREATE returns NOOP without the index; we have
	// to scan the cache.) Fall back to 201 with no index if we can't
	// find it — shouldn't happen but keeps the surface honest.
	int created_index = -1;
	for (const auto &c : m_state.Categories()) {
		if (c.name == f.name) {
			created_index = static_cast<int>(c.index);
			break;
		}
	}

	CHttpServer::Response r;
	r.status       = 201;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");    w.ValueBool(true);
	  w.Key("name");  w.ValueString(wxString::FromUTF8(f.name.c_str()));
	  if (created_index >= 0) {
		  w.Key("index"); w.ValueInt(static_cast<int64_t>(created_index));
	  }
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleCategoryUpdate(
	const CHttpServer::Request &req, const std::string &index_str)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	std::uint8_t idx = 0;
	if (!ParseCategoryIndex(index_str, idx)) {
		return ErrorResponse(400, "bad_request",
			"path `{index}` must be a uint8 in [0, 255]");
	}
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}

	// Find the existing category — we need its current values for any
	// field the PATCH body doesn't override (CEC_Category_Tag is
	// not delta-friendly; we always send the full tag).
	webapi::CategorySnapshot current;
	bool found = false;
	for (const auto &c : m_state.Categories()) {
		if (static_cast<std::uint8_t>(c.index) == idx) {
			current = c;
			found = true;
			break;
		}
	}
	if (!found) {
		return ErrorResponse(404, "not_found",
			"no category with that index");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	CategoryFields f;
	auto err = ParseCategoryFields(obj, f);
	if (err.status >= 400) return err;

	const std::string name    = f.has_name    ? f.name    : current.name;
	const std::string path    = f.has_path    ? f.path    : current.path;
	const std::string comment = f.has_comment ? f.comment : current.comment;
	const std::uint32_t color = f.has_color   ? f.color   : current.color;
	const std::uint8_t  prio  = f.has_prio    ? f.prio    : current.priority_code;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_UPDATE_CATEGORY));
	ec_req->AddTag(BuildCategoryTag(idx, name, path, comment, color, prio));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for UPDATE_CATEGORY");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void) RefresherTick(m_app, m_state);

	// Return the post-mutation category object.
	webapi::CategorySnapshot after = current;
	for (const auto &c : m_state.Categories()) {
		if (static_cast<std::uint8_t>(c.index) == idx) {
			after = c;
			break;
		}
	}

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteCategoryObject(w, after);
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleCategoryDelete(
	const CHttpServer::Request &req, const std::string &index_str)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	std::uint8_t idx = 0;
	if (!ParseCategoryIndex(index_str, idx)) {
		return ErrorResponse(400, "bad_request",
			"path `{index}` must be a uint8 in [0, 255]");
	}
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(503, "ec_unavailable",
			"amuleapi has not received its first EC snapshot yet");
	}
	// Index 0 is the implicit "All" category — amuled treats deleting
	// it as illegal. Reject before the EC roundtrip.
	if (idx == 0) {
		return ErrorResponse(400, "bad_request",
			"cannot delete the default (index=0) category");
	}
	bool found = false;
	for (const auto &c : m_state.Categories()) {
		if (static_cast<std::uint8_t>(c.index) == idx) { found = true; break; }
	}
	if (!found) {
		return ErrorResponse(404, "not_found",
			"no category with that index");
	}

	// CEC_Category_Tag CMD-detail shape: just `(EC_TAG_CATEGORY, idx)`,
	// no children (amule-remote-gui.cpp:1043 uses `CEC_Category_Tag(cat,
	// EC_DETAIL_CMD)`). We replicate that with a bare CECTag.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_DELETE_CATEGORY));
	ec_req->AddTag(CECTag(EC_TAG_CATEGORY,
		static_cast<std::uint32_t>(idx)));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for DELETE_CATEGORY");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");    w.ValueBool(true);
	  w.Key("index"); w.ValueInt(static_cast<int64_t>(idx));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


// --- Phase 6 — search ---------------------------------------------------

namespace {

// Map wire-string search types to amule's EC_SEARCH_TYPE enum.
// "local" / "global" / "kad" matches amulegui's UI labels +
// amule-remote-gui.cpp:2406-2410's switch.
bool SearchTypeFromString(const std::string &s, std::uint8_t &out)
{
	if      (s == "local")  { out = EC_SEARCH_LOCAL;  return true; }
	else if (s == "global") { out = EC_SEARCH_GLOBAL; return true; }
	else if (s == "kad")    { out = EC_SEARCH_KAD;    return true; }
	return false;
}

}  // namespace


CHttpServer::Response CApiDispatcher::HandleSearchStart(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	// Body shape:
	//   { "query": "...", required string
	//     "type":  "local" | "global" | "kad" (default "global"),
	//     "file_type":  string (optional, amule file-type label),
	//     "extension":  string (optional, e.g. "mkv"),
	//     "min_size":   uint64 bytes (optional, default 0),
	//     "max_size":   uint64 bytes (optional, default 0 = no cap),
	//     "min_avail":  uint32 (optional, default 0) }
	std::string query;
	{
		const auto it = obj.find("query");
		if (it == obj.end() || !it->second.is<std::string>()) {
			return ErrorResponse(400, "bad_request",
				"required string field `query` is missing");
		}
		query = it->second.get<std::string>();
		if (query.empty()) {
			return ErrorResponse(400, "bad_request",
				"`query` must be non-empty");
		}
	}

	std::uint8_t search_type = EC_SEARCH_GLOBAL;
	{
		const auto it = obj.find("type");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`type` must be one of \"local\", \"global\", \"kad\"");
			}
			if (!SearchTypeFromString(it->second.get<std::string>(),
			                          search_type)) {
				return ErrorResponse(400, "bad_request",
					"`type` must be one of \"local\", \"global\", \"kad\"");
			}
		}
	}

	std::string file_type;
	{
		const auto it = obj.find("file_type");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`file_type` must be a string");
			}
			file_type = it->second.get<std::string>();
		}
	}
	std::string extension;
	{
		const auto it = obj.find("extension");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request",
					"`extension` must be a string");
			}
			extension = it->second.get<std::string>();
		}
	}
	std::uint64_t min_size = 0;
	std::uint64_t max_size = 0;
	std::uint32_t min_avail = 0;
	{
		const auto it = obj.find("min_size");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request",
					"`min_size` must be a non-negative integer (bytes)");
			}
			const double v = it->second.get<double>();
			if (v < 0) return ErrorResponse(400, "bad_request",
				"`min_size` must be >= 0");
			min_size = static_cast<std::uint64_t>(v);
		}
	}
	{
		const auto it = obj.find("max_size");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request",
					"`max_size` must be a non-negative integer (bytes; 0 = no cap)");
			}
			const double v = it->second.get<double>();
			if (v < 0) return ErrorResponse(400, "bad_request",
				"`max_size` must be >= 0");
			max_size = static_cast<std::uint64_t>(v);
		}
	}
	{
		const auto it = obj.find("min_avail");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request",
					"`min_avail` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request",
					"`min_avail` out of range");
			}
			min_avail = static_cast<std::uint32_t>(v);
		}
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SEARCH_START));
	ec_req->AddTag(CEC_Search_Tag(
		wxString::FromUTF8(query.c_str()),
		static_cast<EC_SEARCH_TYPE>(search_type),
		wxString::FromUTF8(file_type.c_str()),
		wxString::FromUTF8(extension.c_str()),
		min_avail,
		min_size,
		max_size));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for SEARCH_START");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Invalidate the lazy /search/results cache so the next GET sees
	// the fresh query's results (amuled's searchlist gets cleared
	// server-side on SEARCH_START; if the cached snapshot pre-dates
	// this mutation, returning it would be misleadingly stale).
	// This is exactly the use case Phase 4g's CTtlCache::Invalidate
	// was carved out for.
	m_search_cache.Invalidate();

	CHttpServer::Response r;
	r.status       = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");    w.ValueBool(true);
	  w.Key("query"); w.ValueString(wxString::FromUTF8(query.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleSearchStop(
	const CHttpServer::Request &req)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SEARCH_STOP));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for SEARCH_STOP");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// /search/results stays valid after stop — amuled keeps the
	// accumulated results until the next SEARCH_START. No cache
	// invalidation needed; consumers polling /search/results see
	// the same set they were just looking at.

	CHttpServer::Response r;
	r.status       = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok"); w.ValueBool(true);
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


CHttpServer::Response CApiDispatcher::HandleSearchDownload(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) return a.rejection;
	if (auto rej = RequireAdmin(a)) return *rej;

	// Canonicalise the URL hash to lowercase.
	std::string needle = hash;
	std::transform(needle.begin(), needle.end(), needle.begin(),
		[](unsigned char c) { return std::tolower(c); });

	CMD4Hash file_hash;
	if (!HashFromHex(needle, file_hash)) {
		return ErrorResponse(400, "bad_request",
			"`{hash}` must be a 32-char hex MD4");
	}

	// Optional body: {"category": uint8}. amulegui's
	// CDownQueueRem::AddSearchToDownload defaults to category 0
	// when none is supplied; we mirror that. The body itself is
	// optional — clients that don't care about category POST with
	// no body and get the default download path.
	std::uint8_t category = 0;
	if (!req.body.empty()) {
		picojson::value root;
		std::string parse_err;
		if (!ParseJsonObjectBody(req.body, root, parse_err)) {
			return ErrorResponse(400, "bad_request", parse_err.c_str());
		}
		const auto &obj = root.get<picojson::object>();
		const auto it = obj.find("category");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request",
					"`category` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 255) {
				return ErrorResponse(400, "bad_request",
					"`category` must be in [0, 255]");
			}
			category = static_cast<std::uint8_t>(v);
		}
	}

	// amuled accepts the result hash as the partfile-tag's int
	// payload (matches amule-remote-gui.cpp:2230). amuled looks up
	// the hash in its searchlist; if not present, returns FAILED.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_DOWNLOAD_SEARCH_RESULT));
	CECTag hash_tag(EC_TAG_PARTFILE, file_hash);
	hash_tag.AddTag(CECTag(EC_TAG_PARTFILE_CAT, category));
	ec_req->AddTag(hash_tag);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable",
			"EC roundtrip failed for DOWNLOAD_SEARCH_RESULT");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Inline refresh so /downloads sees the new partfile (subject
	// to amuled's async allocate-and-hash; same caveat as POST
	// /downloads — the partfile surfaces within 1-2 ticks).
	(void) RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status       = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	  w.Key("ok");       w.ValueBool(true);
	  w.Key("hash");     w.ValueString(wxString::FromUTF8(needle.c_str()));
	  w.Key("category"); w.ValueInt(static_cast<int64_t>(category));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}


// --- Phase 8a — Server-Sent Events: streaming /events endpoint ---------
//
// The SSE channel runs on a worker thread that the HTTP server's
// streaming pipeline spawns per connection. Authentication is enforced
// here, before the head is written; auth failures take the same wire
// shape as regular GET endpoints (401 / 403 JSON body).
//
// Phase 8a ships just the connection lifecycle + 15 s heartbeat. The
// heartbeat is a SSE-comment line (`: keepalive\n\n`) per RFC 6202
// — clients ignore comment lines, but intermediate proxies (and many
// browser SSE clients) need periodic traffic to keep the connection
// alive. Phase 8b layers event generation on top.
//
// The "two-call dance" mentioned in HttpServer.cpp's DispatchStreaming
// is collapsed here: this single function sets the response head AND
// runs the loop. The HTTP server captures the head out-params before
// writing the head, but reads them only ONCE — so we set them at the
// top and the loop body just calls writer.Write.
void CApiDispatcher::DispatchEvents(
	const CHttpServer::Request &req,
	CHttpServer::Writer &writer,
	unsigned &http_status,
	std::string &content_type,
	std::map<std::string, std::string> &response_headers)
{
	// Authenticate via the same bearer/cookie path as every other
	// handler. SSE doesn't have a "401 body" shape; if the auth
	// fails, we return a synthetic SSE event so the client sees a
	// clear failure rather than a silent close. The HTTP status is
	// still 401, so well-behaved clients react.
	auto a = AuthenticateRequest(req, m_jwt, m_revocations,
		kSessionCookieName);
	if (!a.ok) {
		http_status = a.rejection.status;
		content_type = "application/json";
		// Single chunk with the standard error body, then return —
		// the streaming pipeline will write the head with the
		// 401/403 status and the chunk as the body, then close.
		writer.Write(a.rejection.body);
		return;
	}
	// SSE doesn't need admin role — reads are guest-friendly. The
	// channel multiplexes every event type clients want to subscribe
	// to. Phase 5+'s admin-gated mutations don't ship over SSE; SSE
	// is a read-only push.

	http_status   = 200;
	content_type  = "text/event-stream";
	response_headers["Cache-Control"]    = "no-cache";
	response_headers["X-Accel-Buffering"] = "no";  // disable nginx buffering

	// Phase 9: CORS on the SSE response too. EventSource sends
	// `Origin` and reads only the standard CORS bundle for credentialed
	// cross-origin streams. No Expose-Headers needed (SSE clients don't
	// read response headers programmatically).
	{
		const std::string cors_org = ResolveCorsOrigin(req, m_config);
		ApplyCorsHeaders(response_headers, cors_org,
			m_config.ServerCfg().allow_cors);
	}
	// Also disable Connection: keep-alive override — chunked +
	// streaming requires the default. (HttpServer adds chunked
	// transfer-encoding automatically.)

	// Initial reassurance chunk so the client knows the channel is
	// open. Some browser EventSource impls don't fire `onopen` until
	// at least one chunk lands.
	if (!writer.Write(": connected\n\n")) return;

	// Optional `?channels=<csv>` query: limit the event types
	// delivered to a comma-separated subset. The mapping from
	// EventBus event name → channel is prefix-based:
	//   download_*  → "downloads"
	//   shared_*    → "shared"
	//   server_*    → "servers"
	//   client_*    → "clients"
	//   status_*    → "status"
	//   log_*       → "logs"
	// The synthetic per-subscriber `resync` event is ALWAYS
	// delivered regardless of filter — its purpose is to signal a
	// cache invalidation the client cannot opt out of.
	// Unknown channel names in the query are silently ignored (allow
	// forward-compatibility with future event families).
	std::set<std::string> channel_filter;
	bool channels_set = false;
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos) query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("channels");
		if (it != qmap.end() && !it->second.empty()) {
			channels_set = true;
			std::string cur;
			for (char c : it->second) {
				if (c == ',') {
					if (!cur.empty()) channel_filter.insert(cur);
					cur.clear();
				} else {
					cur.push_back(c);
				}
			}
			if (!cur.empty()) channel_filter.insert(cur);
		}
	}
	auto event_channel = [](const std::string &name) -> std::string {
		// Event naming convention: every bus event name MUST contain
		// at least one underscore — the prefix before the first `_`
		// identifies the channel. Names without an underscore would
		// collapse to "themselves" as their own channel, which a
		// client filter wouldn't anticipate and could silently drop
		// them. Today the only no-underscore name on the wire is
		// `resync`, which is always emitted before this filter
		// applies (it's a synthetic per-subscriber event published
		// directly to the SSE writer, never via EventBus::Publish).
		// If a future bus event ever ships as a bare token, either
		// give it an explicit channel mapping in the switch below
		// or have it always bypass the filter the way `resync`
		// does.
		const auto us = name.find('_');
		if (us == std::string::npos) return name;
		const std::string prefix = name.substr(0, us);
		if (prefix == "download") return "downloads";
		if (prefix == "shared")   return "shared";
		if (prefix == "server")   return "servers";
		if (prefix == "client")   return "clients";
		if (prefix == "status")   return "status";
		if (prefix == "log")      return "logs";
		return prefix;
	};
	auto event_passes_filter = [&](const std::string &name) {
		if (!channels_set) return true;
		return channel_filter.count(event_channel(name)) > 0;
	};

	// Drain events from the EventBus. The drain blocks up to the
	// heartbeat interval (15 s) waiting for new events; if nothing
	// arrives in that window, fall through to a `: keepalive`
	// comment so the connection stays warm.
	//
	// Initial `since_id` resolution honours the `Last-Event-ID`
	// header per RFC 6202 §4 reconnect contract:
	//   - absent / unparseable → start from NewestId (only events
	//     fired AFTER connect)
	//   - in-range (parsed+1 >= OldestId) → start from parsed; the
	//     first Drain returns immediately with the missed range
	//   - gap (parsed+1 < OldestId) → events evicted from the ring
	//     before this client could read them. Emit a typed `resync`
	//     event so the client invalidates its cache and re-GETs the
	//     REST collections, then start fresh from NewestId
	//   - parsed > NewestId → stale id from a prior daemon process
	//     (ids are per-process — they reset on restart). Same
	//     `resync` event with reason=restart, then start fresh.
	std::uint64_t since_id;
	const std::string lei = FindHeaderCaseInsensitive(req.headers,
		"Last-Event-ID");
	const std::uint64_t newest = m_app.EventBus().NewestId();
	const std::uint64_t oldest = m_app.EventBus().OldestId();
	if (lei.empty()) {
		since_id = newest;
	} else {
		char *end = nullptr;
		const unsigned long long parsed = std::strtoull(lei.c_str(),
			&end, 10);
		if (end == lei.c_str() || *end != '\0') {
			since_id = newest;
		} else if (parsed > newest) {
			// Per-subscriber synthetic event — not on the bus. id is
			// the current newest so the client's EventSource resumes
			// from there on the next reconnect (no resync loop).
			std::ostringstream frame;
			frame << "event: resync\n"
			      << "id: "    << newest << "\n"
			      << "data: {\"reason\":\"restart\",\"since_id\":"
			      << static_cast<std::uint64_t>(parsed)
			      << ",\"newest_id\":" << newest << "}\n\n";
			if (!writer.Write(frame.str())) return;
			since_id = newest;
		} else if (oldest == 0 || parsed + 1 >= oldest) {
			since_id = static_cast<std::uint64_t>(parsed);
		} else {
			std::ostringstream frame;
			frame << "event: resync\n"
			      << "id: "    << newest << "\n"
			      << "data: {\"reason\":\"gap\",\"since_id\":"
			      << static_cast<std::uint64_t>(parsed)
			      << ",\"newest_id\":" << newest << "}\n\n";
			if (!writer.Write(frame.str())) return;
			since_id = newest;
		}
	}
	// Heartbeat is wall-clock driven, not Drain-timeout driven. A
	// busy bus paired with `?channels=` that filters every drained
	// event would otherwise leave the wire silent for arbitrary
	// stretches: each Drain returns immediately (events are pending),
	// the loop swallows them all, advances since_id, and re-enters
	// Drain — never giving keepalive a chance to fire. NAT / load
	// balancers / EventSource clients typically drop idle TCP
	// connections after 30–60 s of silence, so we keep tabs on the
	// last byte written and emit `: keepalive` whenever it falls
	// behind the 15 s budget, regardless of which loop branch we
	// just took.
	const auto heartbeat_interval = std::chrono::seconds(15);
	auto last_write_at = std::chrono::steady_clock::now();
	std::vector<webapi::Event> drained;
	while (writer.Alive()) {
		drained.clear();
		const std::uint64_t new_high = m_app.EventBus().Drain(
			since_id, heartbeat_interval, drained);
		if (!writer.Alive()) break;
		// Apply ?channels= filter before emission. We still advance
		// since_id over EVERY drained event (filtered or not) so the
		// client doesn't re-see them on reconnect; reconnect replay is
		// id-based, not channel-based.
		std::ostringstream frame;
		bool wrote_any = false;
		for (const auto &ev : drained) {
			if (!event_passes_filter(ev.name)) continue;
			// Emit one SSE frame per event:
			//   event: <name>\nid: <id>\ndata: <data>\n\n
			// Per RFC 6202 §4: `data:` lines are single-line. Our
			// JSON payloads never contain literal newlines (the
			// EventDiff serializer escapes them), so one `data:`
			// line per event is sufficient.
			//
			// `ev.name` is NOT escaped here. Every event name on the
			// bus is a server-controlled compile-time literal
			// ("download_added", "shared_updated", ...) emitted by
			// `EventBus::Publish` from EventDiff.cpp. If a future
			// publisher ever takes a name from external input, that
			// publisher MUST sanitize CR/LF/`\0` at the call site —
			// otherwise this frame writer would corrupt the SSE
			// stream framing.
			frame << "event: " << ev.name << "\n"
			      << "id: "    << ev.id   << "\n"
			      << "data: "  << ev.data << "\n\n";
			wrote_any = true;
		}
		if (wrote_any) {
			if (!writer.Write(frame.str())) break;
			last_write_at = std::chrono::steady_clock::now();
			since_id = new_high;
		} else {
			if (!drained.empty()) {
				// Every drained event got filtered out — advance the
				// cursor silently so the next Drain doesn't re-read
				// them.
				since_id = new_high;
			}
			// drained.empty() (Drain hit its timeout with nothing
			// new) OR all-events-filtered-out (the channel-filter
			// drop). In either case, emit a heartbeat IFF we
			// haven't written anything in the heartbeat window.
			const auto now = std::chrono::steady_clock::now();
			if (now - last_write_at >= heartbeat_interval) {
				if (!writer.Write(": keepalive\n\n")) break;
				last_write_at = now;
			}
		}
	}
}
