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

#include "config.h" // AMULEAPI_STATIC_DIR (compile-time install path)
#include "AmuleApiConfig.h"
#include "App.h"
#include "Auth.h"
#include "ConstantTime.h"
#include "Etag.h"
#include "JsonWriter.h"
#include "Jwt.h"
#include "PathPatterns.h"
#include "Refresher.h" // ParseStatsTreeFromPacket / ParseGraphsFromPacket / ApplySearchFull
#include "StaticFs.h"  // IsDir, ResolveWithinRoot
#include "State.h"

#include "Constants.h"

#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECCodes.h>
#include <ec/cpp/ECSpecialTags.h>

#include <wx/stdpaths.h>
#include <wx/filename.h>
#ifdef __WXMAC__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <wx/osx/core/cfstring.h>
#endif

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <set>
#include <sstream>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>

// strncasecmp lives in <strings.h> on POSIX (glibc also exposes it
// via <string.h>, but musl/BSDs don't). Match the shim
// libwebcommon/HeaderParse.cpp ships.
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#define PICOJSON_USE_INT64
#include "picojson.h"

#include "config.h" // VERSION

#include "Types.h" // uint8 (required by libs/common/MD5Sum.h)
#include <common/MD5Sum.h>

#include <wx/string.h>

#include <cstdio>
#include <ctime>

namespace
{

void SplitPathAndQuery(const std::string &target, std::string &path, std::string &query)
{
	const size_t q = target.find('?');
	if (q == std::string::npos) {
		path = target;
		query = std::string();
	} else {
		path = target.substr(0, q);
		query = target.substr(q + 1);
	}
}

CHttpServer::Response ErrorResponse(unsigned status, const char *code, const char *message)
{
	CHttpServer::Response r;
	r.status = status;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("error");
	w.BeginObject();
	w.Key("code");
	w.ValueString(wxString::FromAscii(code));
	w.Key("message");
	w.ValueString(wxString::FromAscii(message));
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
struct AuthOutcome
{
	bool ok = false;
	CHttpServer::Response rejection;
	CJwt::VerifyResult verified;
};

AuthOutcome AuthenticateRequest(const CHttpServer::Request &req,
	CJwt &jwt,
	webapi::CRevocationSet &revocations,
	const std::string &cookie_name)
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
			if (h.first.size() == 13 && strncasecmp(h.first.c_str(), "Authorization", 13) == 0) {
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
				if (h.first.size() == 6 && strncasecmp(h.first.c_str(), "Cookie", 6) == 0) {
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
		out.rejection = ErrorResponse(401, "unauthorized", "missing bearer token or session cookie");
		return out;
	}
	if (!jwt.Verify(token, out.verified)) {
		out.rejection = ErrorResponse(401, "unauthorized", "invalid or expired token");
		return out;
	}
	if (revocations.IsRevoked(out.verified.jti)) {
		out.rejection = ErrorResponse(401, "unauthorized", "token has been revoked");
		return out;
	}
	out.ok = true;
	return out;
}

// Wrapper that pipes AuthenticateRequest through a per-IP failure
// counter. Every 401 (missing token / bad sig / expired / revoked)
// counts against the calling IP; once the bucket fills the IP gets
// 429 with Retry-After until the lockout window expires. Pre-checks
// the bucket BEFORE Verify() so a locked-out IP can't burn CPU on
// MAC compares either. Used by every auth-protected handler — login
// keeps its own m_rateLimiter for the dedicated password-failure
// path.
AuthOutcome AuthenticateRequestRateLimited(const CHttpServer::Request &req,
	CJwt &jwt,
	webapi::CRevocationSet &revocations,
	webapi::CRateLimiter &limiter,
	const std::string &cookie_name)
{
	AuthOutcome out;
	const std::string &ip = req.remote_addr;

	const auto decision = limiter.Check(ip);
	if (decision.locked_out) {
		CHttpServer::Response r =
			ErrorResponse(429, "rate_limited", "too many failed auth attempts; retry later");
		char retry_after[32];
		std::snprintf(retry_after,
			sizeof(retry_after),
			"%lld",
			static_cast<long long>(decision.retry_after_seconds));
		r.headers["Retry-After"] = retry_after;
		out.rejection = std::move(r);
		return out;
	}

	out = AuthenticateRequest(req, jwt, revocations, cookie_name);
	if (out.ok) {
		limiter.NoteSuccess(ip);
	} else {
		limiter.NoteFailure(ip);
	}
	return out;
}

// `Set-Cookie: <name>=<value>; HttpOnly; SameSite=Strict; Path=/api/v0;
//             Max-Age=<lifetime>`
//
// No `Secure`: amuleapi serves HTTP by design (operator terminates
// TLS in front). Documented in QUICKSTART.
//
// Attributes shared by Set-Cookie (login) + clear-cookie (logout):
// RFC 6265 §5.3 requires (name, path, domain) match to delete, so
// one shared constant keeps the two paths from drifting.
const char *const kSessionCookieAttrs = "; HttpOnly; SameSite=Strict; Path=/api/v0";

std::string MakeSetCookie(const std::string &name, const std::string &value, std::time_t expires_at)
{
	const std::time_t now = std::time(nullptr);
	// Boundary case: an already-expired `expires_at` produces
	// `Max-Age=0`, which makes the browser delete the cookie on
	// receipt (RFC 6265 §5.2.2). That's the right behaviour — issuing
	// an expired token's cookie shouldn't grant the client a working
	// session — so we emit it deliberately rather than clamping to
	// some positive minimum.
	const std::time_t lifetime = expires_at > now ? expires_at - now : 0;
	// std::string instead of a fixed snprintf buffer. The previous
	// 256-byte buffer fit today's ~189-byte HS256 JWT plus the
	// attribute boilerplate with room to spare, but any future
	// payload extension (extra claim, longer secret, switch to a
	// longer alg) would silently truncate. std::string sizes
	// itself.
	std::string out;
	out.reserve(name.size() + value.size() + 80);
	out += name;
	out += '=';
	out += value;
	out += kSessionCookieAttrs;
	out += "; Max-Age=";
	out += std::to_string(static_cast<long long>(lifetime));
	return out;
}

// `Set-Cookie: <name>=; ... Max-Age=0` — invalidates whatever was
// set on a prior login. Used by /auth/logout. MUST use the same
// (name, path, domain) tuple as MakeSetCookie or the browser
// won't drop the original.
std::string MakeClearCookie(const std::string &name)
{
	std::string out;
	out.reserve(name.size() + 64);
	out += name;
	out += '=';
	out += kSessionCookieAttrs;
	out += "; Max-Age=0";
	return out;
}

// `amuleapi_token` namespacing keeps the cookie distinct from
// amuleweb's legacy `amule_token` so the two daemons can coexist
// behind the same host without a Set-Cookie tug-of-war.
const char *const kSessionCookieName = "amuleapi_token";

// Hard ceiling for individual static-asset reads. Frontend bundles are
// kilobytes to a few MB; 16 MiB is comfortable headroom while keeping a
// malformed StaticRoot pointing at /dev/zero or a multi-GB log file
// from exhausting daemon RAM.
constexpr std::size_t kStaticMaxFileBytes = 16 * 1024 * 1024;

// Map file extension to Content-Type. Unknown → application/octet-stream
// (no XSS amplification from a wrong-type response on an attacker-named
// file).
std::string StaticContentType(const std::string &path)
{
	const std::size_t dot = path.find_last_of('.');
	if (dot == std::string::npos)
		return "application/octet-stream";
	std::string ext = path.substr(dot + 1);
	for (char &c : ext)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (ext == "html" || ext == "htm")
		return "text/html; charset=utf-8";
	if (ext == "js" || ext == "mjs")
		return "text/javascript; charset=utf-8";
	if (ext == "css")
		return "text/css; charset=utf-8";
	if (ext == "json")
		return "application/json; charset=utf-8";
	if (ext == "svg")
		return "image/svg+xml";
	if (ext == "png")
		return "image/png";
	if (ext == "gif")
		return "image/gif";
	if (ext == "jpg" || ext == "jpeg")
		return "image/jpeg";
	if (ext == "ico")
		return "image/x-icon";
	if (ext == "webp")
		return "image/webp";
	if (ext == "woff2")
		return "font/woff2";
	if (ext == "woff")
		return "font/woff";
	if (ext == "ttf")
		return "font/ttf";
	if (ext == "map")
		return "application/json";
	if (ext == "txt")
		return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

// Slurp `fs_path` into `out`. Returns false if the path is not a
// regular file, exceeds kStaticMaxFileBytes, or any read error. `st`
// is populated on success so the caller can derive an ETag from
// mtime + size without re-stat'ing.
bool ReadStaticFile(const std::string &fs_path, std::string &out, struct stat &st)
{
	if (::stat(fs_path.c_str(), &st) != 0)
		return false;
	if (!S_ISREG(st.st_mode))
		return false;
	if (static_cast<std::size_t>(st.st_size) > kStaticMaxFileBytes)
		return false;
	std::ifstream f(fs_path.c_str(), std::ios::binary);
	if (!f.is_open())
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	if (f.bad())
		return false;
	out = ss.str();
	return true;
}

// "mtime-size" hex ETag — same shape nginx defaults to. Strong-form
// quoted per RFC 7232. Sufficient for the local-frontend case where
// the daemon and the file system are colocated and clock-sane.
std::string BuildStaticEtag(const struct stat &st)
{
	std::ostringstream oss;
	oss << '"' << std::hex << static_cast<std::uint64_t>(st.st_mtime) << '-'
	    << static_cast<std::uint64_t>(st.st_size) << '"';
	return oss.str();
}

// Resolve the default static directory when amuleapi.conf's
// [Server]/StaticRoot is empty. Mirrors amuleweb's GetTemplateDir
// (src/webserver/src/WebInterface.cpp): try the macOS .app bundle's
// Resources/ first (so an installed aMule.app surfaces the bundled
// frontend without a conf edit), then the compile-time install path
// from AMULEAPI_STATIC_DIR, then wxStandardPaths' platform-adjusted
// resource dir. Returns the first existing directory; empty if none.
std::string ResolveDefaultStaticDir()
{
	const std::string asset = "amuleapi-static";

#ifdef __WXMAC__
	// LaunchServices lookup for the installed aMule.app. Picks up the
	// bundled placeholder when the operator launched amuleapi from a
	// path-registered .app install.
	CFArrayRef urls = LSCopyApplicationURLsForBundleIdentifier(CFSTR("org.amule.aMule"), NULL);
	CFURLRef bundle_url = NULL;
	if (urls) {
		if (CFArrayGetCount(urls) > 0) {
			bundle_url = (CFURLRef)CFRetain(CFArrayGetValueAtIndex(urls, 0));
		}
		CFRelease(urls);
	}
	if (bundle_url) {
		CFBundleRef bundle = CFBundleCreate(NULL, bundle_url);
		CFRelease(bundle_url);
		if (bundle) {
			CFStringRef name =
				CFStringCreateWithCString(NULL, asset.c_str(), kCFStringEncodingUTF8);
			CFURLRef rsrc = CFBundleCopyResourceURL(bundle, name, NULL, NULL);
			CFRelease(name);
			CFRelease(bundle);
			if (rsrc) {
				CFURLRef abs = CFURLCopyAbsoluteURL(rsrc);
				CFRelease(rsrc);
				if (abs) {
					CFStringRef p = CFURLCopyFileSystemPath(abs, kCFURLPOSIXPathStyle);
					CFRelease(abs);
					std::string s = std::string(wxCFStringRef(p).AsString().utf8_str());
					if (webapi::IsDir(s))
						return s;
				}
			}
		}
	}
#endif // __WXMAC__

#ifdef AMULEAPI_STATIC_DIR
	if (webapi::IsDir(AMULEAPI_STATIC_DIR)) {
		return std::string(AMULEAPI_STATIC_DIR);
	}
#endif

	// wxStandardPaths fallback. Same platform adjustments amuleweb
	// applies for its `webserver/` lookup (WebInterface.cpp:211-225).
	wxString dir = wxStandardPaths::Get().GetResourcesDir();
#if defined(__WINDOWS__)
	// Installer layout: bin\amuleapi.exe + share\amule\amuleapi-static\.
	// wxStandardPaths returns the exe directory on Windows, so go up
	// one level and into the FHS-style share/amule/ tree.
	dir = wxFileName(dir, "..").GetFullPath();
	dir = wxFileName(dir, "share").GetFullPath();
	dir = wxFileName(dir, "amule").GetFullPath();
#elif !defined(__WXMAC__)
	dir = dir.BeforeLast(wxFileName::GetPathSeparator());
	dir = wxFileName(dir, "amule").GetFullPath();
#endif
	dir = wxFileName(dir, asset).GetFullPath();
	const std::string s(dir.utf8_str());
	if (webapi::IsDir(s))
		return s;
	return std::string();
}

} // namespace

CApiDispatcher::CApiDispatcher(
	const CAmuleApiConfig &config, CJwt &jwt, webapi::CState &state, CamuleapiApp &app)
: m_config(config)
, m_jwt(jwt)
, m_state(state)
, m_app(app)
, m_rateLimiter(webapi::CRateLimiter::Config{ config.AuthCfg().login_failure_window_seconds,
	  config.AuthCfg().login_failure_threshold,
	  config.AuthCfg().login_lockout_seconds })
,
// Generic-401 limiter: 30 failures within 60 s, 5-minute
// lockout. Hard-coded for now — operators have a separate
// knob for login-specific limits; the generic 401 cap is a
// crash-pad against credential-stuffing across all non-
// login endpoints and can become a config knob in 3.1 if
// anyone asks.
m_authRateLimiter(webapi::CRateLimiter::Config{ 60u, 30u, 300u })
{
}

namespace
{

// Case-tolerant header lookup. Beast preserves the wire-form casing
// the client sent, so a literal `req.headers.find("If-None-Match")`
// misses lowercased headers. Walks the map once on miss to recover.
std::string FindHeaderCaseInsensitive(
	const std::map<std::string, std::string> &headers, const std::string &name)
{
	auto it = headers.find(name);
	if (it != headers.end())
		return it->second;
	for (const auto &h : headers) {
		if (h.first.size() == name.size() &&
			strncasecmp(h.first.c_str(), name.c_str(), name.size()) == 0) {
			return h.second;
		}
	}
	return std::string();
}

// resolve the CORS Origin echo for this request. Returns
// the verbatim Origin to put in `Access-Control-Allow-Origin`, or
// an empty string when CORS is disabled, the request had no Origin
// (same-origin browser navigation; non-browser caller), or the
// allowlist rejected the value. Wildcard semantics: `allow_cors=1`
// with an empty allowlist echoes the request's Origin verbatim,
// which is `*`-equivalent but cookie-auth-compatible (the literal
// `*` is incompatible with `Access-Control-Allow-Credentials: true`
// per CORS Fetch §3.2.5).
std::string ResolveCorsOrigin(const CHttpServer::Request &req, const CAmuleApiConfig &cfg)
{
	if (!cfg.ServerCfg().allow_cors)
		return std::string();
	const std::string origin = FindHeaderCaseInsensitive(req.headers, "Origin");
	if (origin.empty())
		return std::string();
	const auto &list = cfg.ServerCfg().cors_origin_allowlist;
	if (list.empty())
		return origin; // echo any origin
	for (const auto &allowed : list) {
		if (allowed == origin)
			return origin;
	}
	return std::string();
}

// stamp the resolved CORS headers onto a response. `Vary:
// Origin` is ALWAYS added when CORS is enabled (even on rejected
// origins) so intermediaries don't cache a cross-origin response
// against a same-origin cache key. The auth + content headers go
// on iff the origin was actually allowed.
void ApplyCorsHeaders(
	std::map<std::string, std::string> &headers, const std::string &resolved_origin, bool cors_enabled)
{
	if (!cors_enabled)
		return;
	headers["Vary"] = "Origin";
	if (resolved_origin.empty())
		return;
	headers["Access-Control-Allow-Origin"] = resolved_origin;
	headers["Access-Control-Allow-Credentials"] = "true";
	// Header names the client may read from `fetch().headers.get(...)`
	// — by default the Fetch spec only exposes the CORS-safelisted
	// response headers (Cache-Control, Content-Language, Content-Type,
	// Expires, Last-Modified, Pragma). amuleapi clients want to read
	// ETag for cache validation; SSE clients don't need this list.
	headers["Access-Control-Expose-Headers"] = "ETag";
}

// Forward declaration so HandleLogin (which sits above the helper's
// definition) can share the depth-cap defence. The definition lives
// near the other mutation-body parsers further down the file.
bool ParseJsonObjectBody(const std::string &body, picojson::value &out, std::string &err);

} // namespace

CHttpServer::Response CApiDispatcher::Dispatch(const CHttpServer::Request &req)
{
	const bool cors_enabled = m_config.ServerCfg().allow_cors;
	const std::string cors_org = ResolveCorsOrigin(req, m_config);

	// CORS preflight short-circuit. OPTIONS requests with
	// `Access-Control-Request-Method` are browser preflights — they
	// don't carry credentials and shouldn't run the auth gate or the
	// route handler. Reply with 204 and the CORS bundle (or 204 +
	// `Vary: Origin` only when the origin is rejected — the browser
	// blocks the subsequent real request).
	if (req.method == "OPTIONS" &&
		!FindHeaderCaseInsensitive(req.headers, "Access-Control-Request-Method").empty()) {
		CHttpServer::Response pre;
		pre.status = 204;
		pre.content_type.clear();
		ApplyCorsHeaders(pre.headers, cors_org, cors_enabled);
		if (!cors_org.empty()) {
			pre.headers["Access-Control-Allow-Methods"] =
				"GET, HEAD, POST, PATCH, DELETE, OPTIONS";
			// Headers actual requests may send. Authorization for
			// bearer; If-None-Match for ETag conditional GET;
			// Last-Event-ID for SSE replay.
			pre.headers["Access-Control-Allow-Headers"] =
				"Authorization, Content-Type, If-None-Match, Last-Event-ID";
			pre.headers["Access-Control-Max-Age"] = "86400";
		}
		return pre;
	}

	// post-process every response with an ETag stamp +
	// `If-None-Match` → 304 swap, but only on GET/HEAD that come back
	// 200. Mutations (POST/PATCH/DELETE) and error paths are passed
	// through unchanged — there's no benefit to ETag-caching a 4xx
	// body, and a mutation's response carries the post-mutation
	// state which the client always wants delivered.
	CHttpServer::Response resp = DispatchToHandler(req);

	const bool is_safe_method = (req.method == "GET" || req.method == "HEAD");
	if (is_safe_method && resp.status == 200 && !resp.body.empty()) {
		// Snapshot-versioned memoization: skip the MD5 over the
		// (potentially multi-MB) body when the (target,
		// snapshot_at) tuple matches what we already hashed.
		const std::time_t snap = m_state.SnapshotAt();
		std::string etag;
		{
			std::lock_guard<std::mutex> g(m_etagCacheMu);
			auto it = m_etagCache.find(req.target);
			if (it != m_etagCache.end() && it->second.snapshot_at == snap && snap != 0) {
				etag = it->second.etag;
			}
		}
		if (etag.empty()) {
			etag = webcommon::Etag(resp.body);
			std::lock_guard<std::mutex> g(m_etagCacheMu);
			if (m_etagCache.size() >= kEtagCacheCapacity) {
				// Crude memory backstop. Real workload is a few
				// dozen unique targets; the wholesale clear is
				// cheaper than a real LRU machinery.
				m_etagCache.clear();
			}
			EtagCacheEntry e;
			e.snapshot_at = snap;
			e.etag = etag;
			m_etagCache[req.target] = std::move(e);
		}
		// RFC 7232 §2.3 — the header value MUST be quoted.
		resp.headers["ETag"] = "\"" + etag + "\"";

		const std::string inm = FindHeaderCaseInsensitive(req.headers, "If-None-Match");
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

	// stamp CORS on every response (success and error paths)
	// so browsers can read the body in the 4xx/5xx case too.
	ApplyCorsHeaders(resp.headers, cors_org, cors_enabled);
	return resp;
}

CHttpServer::Response CApiDispatcher::DispatchToHandler(const CHttpServer::Request &req)
{
	std::string path, query;
	SplitPathAndQuery(req.target, path, query);

	// Defence-in-depth: reject NUL / encoded NUL / encoded `..` /
	// literal `..` segments before routing. Today's byte-exact
	// routes 404 these requests organically, but adding a future
	// endpoint that admits path captures (file-by-name, log-by-
	// label, …) would silently inherit a traversal surface without
	// this gate.
	if (web_api_path::LooksMalicious(path)) {
		return ErrorResponse(400, "bad_request", "path contains a traversal/injection token");
	}

	if (path == "/api/v0/version") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(
				405, "method_not_allowed", "method not allowed on /api/v0/version");
		}
		return HandleVersion(req);
	}

	if (path == "/api/v0/auth/login") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed", "only POST on /auth/login");
		}
		return HandleLogin(req);
	}

	if (path == "/api/v0/auth/logout") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed", "only POST on /auth/logout");
		}
		return HandleLogout(req);
	}

	if (path == "/api/v0/auth/session") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed", "only GET on /auth/session");
		}
		return HandleSession(req);
	}

	if (path == "/api/v0/status") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed", "only GET on /status");
		}
		return HandleStatus(req);
	}

	if (path == "/api/v0/downloads") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleDownloads(req);
		}
		if (req.method == "POST") {
			// add a download by ed2k link.
			return HandleDownloadAdd(req);
		}
		if (req.method == "PATCH") {
			// bulk pause/resume/priority/category over {hashes:[...]}.
			return HandleDownloadsBulkPatch(req);
		}
		if (req.method == "DELETE") {
			// bulk cancel+remove over {hashes:[...]}.
			return HandleDownloadsBulkDelete(req);
		}
		return ErrorResponse(
			405, "method_not_allowed", "only GET / HEAD / POST / PATCH / DELETE on /downloads");
	}

	// bulk clear-completed.
	if (path == "/api/v0/downloads/clear_completed") {
		if (req.method != "POST") {
			return ErrorResponse(
				405, "method_not_allowed", "only POST on /downloads/clear_completed");
		}
		return HandleDownloadsClearCompleted(req);
	}

	// /uploads was retired in — /clients covers the full
	// peer surface (every upload_state, including queue waiters and
	// download peers); consumers filter client-side by upload_state.
	if (path == "/api/v0/clients") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed", "only GET on /clients");
		}
		return HandleClients(req);
	}

	if (path == "/api/v0/shared") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleSharedList(req);
		}
		if (req.method == "PATCH") {
			// bulk upload-priority PATCH over {hashes:[...], priority}.
			return HandleSharedBulkPatch(req);
		}
		return ErrorResponse(405, "method_not_allowed", "only GET / HEAD / PATCH on /shared");
	}

	if (path == "/api/v0/shared/reload") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed", "only POST on /shared/reload");
		}
		return HandleSharedReload(req);
	}

	if (path == "/api/v0/servers") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleServers(req);
		}
		if (req.method == "POST") {
			// add a server by host:port.
			return HandleServerAdd(req);
		}
		return ErrorResponse(405, "method_not_allowed", "only GET / HEAD / POST on /servers");
	}

	if (path == "/api/v0/servers/update") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed", "only POST on /servers/update");
		}
		return HandleServerUpdateFromUrl(req);
	}

	// server connect & remove (single server by ECID).
	// Address-keyed aliases live in the same block — same handlers,
	// different lookup path. ECID forms are tried first because they
	// match a single-segment pattern that's cheaper to dispatch; the
	// address forms have a colon in the capture which the path pattern
	// captures as a single segment too.
	{
		static const auto server_connect =
			web_api_path::ParsePattern("/api/v0/servers/{ecid}/connect");
		static const auto server_one = web_api_path::ParsePattern("/api/v0/servers/{ecid}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(server_connect, path_segs, caps)) {
			if (req.method != "POST") {
				return ErrorResponse(
					405, "method_not_allowed", "only POST on /servers/{ecid}/connect");
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
				return ErrorResponse(
					405, "method_not_allowed", "only DELETE on /servers/{ecid}");
			}
			if (caps["ecid"].find(':') != std::string::npos) {
				return HandleServerDeleteByAddress(req, caps["ecid"]);
			}
			return HandleServerDelete(req, caps["ecid"]);
		}
	}

	if (path == "/api/v0/kad") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed", "only GET on /kad");
		}
		return HandleKad(req);
	}

	// connection control.
	if (path == "/api/v0/networks/connect") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed", "only POST on /networks/connect");
		}
		return HandleNetworksConnect(req);
	}
	if (path == "/api/v0/networks/disconnect") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed", "only POST on /networks/disconnect");
		}
		return HandleNetworksDisconnect(req);
	}
	// /api/v0/kad/connect and /api/v0/kad/disconnect were dropped in
	// favour of /networks/{connect,disconnect} with `{"network":"kad"}`
	// — the two were strict aliases and the granular-selector form on
	// /networks/* makes the dedicated shortcut redundant.
	if (path == "/api/v0/kad/bootstrap") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed", "only POST on /kad/bootstrap");
		}
		return HandleKadBootstrap(req);
	}

	// shared file priority PATCH. `{hash}` is the lowercase 32-char hex
	// MD4 hash.
	{
		static const auto shared_detail = web_api_path::ParsePattern("/api/v0/shared/{hash}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(shared_detail, path_segs, caps)) {
			if (req.method != "PATCH") {
				return ErrorResponse(
					405, "method_not_allowed", "only PATCH on /shared/{hash}");
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
		return ErrorResponse(405, "method_not_allowed", "only GET / HEAD / POST on /categories");
	}

	// single-category PATCH/DELETE.
	{
		static const auto category_one = web_api_path::ParsePattern("/api/v0/categories/{index}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(category_one, path_segs, caps)) {
			if (req.method == "PATCH") {
				return HandleCategoryUpdate(req, caps["index"]);
			}
			if (req.method == "DELETE") {
				return HandleCategoryDelete(req, caps["index"]);
			}
			return ErrorResponse(
				405, "method_not_allowed", "only PATCH / DELETE on /categories/{index}");
		}
	}

	if (path == "/api/v0/preferences") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandlePreferences(req);
		}
		if (req.method == "PATCH") {
			return HandlePreferencesPatch(req);
		}
		return ErrorResponse(405, "method_not_allowed", "only GET / HEAD / PATCH on /preferences");
	}

	if (path == "/api/v0/logs/amule") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleLogAmule(req);
		}
		if (req.method == "DELETE") {
			return HandleLogAmuleReset(req);
		}
		return ErrorResponse(405, "method_not_allowed", "only GET / HEAD / DELETE on /logs/amule");
	}

	if (path == "/api/v0/logs/serverinfo") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleLogServerinfo(req);
		}
		if (req.method == "DELETE") {
			return HandleLogServerinfoReset(req);
		}
		return ErrorResponse(
			405, "method_not_allowed", "only GET / HEAD / DELETE on /logs/serverinfo");
	}

	if (path == "/api/v0/stats/tree") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed", "only GET on /stats/tree");
		}
		return HandleStatsTree(req);
	}

	// search.
	if (path == "/api/v0/search") {
		if (req.method != "POST") {
			return ErrorResponse(405,
				"method_not_allowed",
				"only POST on /search (use GET /search/results for results)");
		}
		return HandleSearchStart(req);
	}
	if (path == "/api/v0/search/stop") {
		if (req.method != "POST") {
			return ErrorResponse(405, "method_not_allowed", "only POST on /search/stop");
		}
		return HandleSearchStop(req);
	}
	if (path == "/api/v0/search/results") {
		if (req.method != "GET" && req.method != "HEAD") {
			return ErrorResponse(405, "method_not_allowed", "only GET on /search/results");
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
				return ErrorResponse(405,
					"method_not_allowed",
					"only POST on /search/results/{hash}/download");
			}
			return HandleSearchDownload(req, caps["hash"]);
		}
	}

	// /stats/graphs/{graph} — path-pattern matches the four allowed
	// graph names ("download" / "upload" / "connections" / "kad").
	{
		static const auto graph_pattern = web_api_path::ParsePattern("/api/v0/stats/graphs/{graph}");
		const auto segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(graph_pattern, segs, caps)) {
			if (req.method != "GET" && req.method != "HEAD") {
				return ErrorResponse(
					405, "method_not_allowed", "only GET on /stats/graphs/{graph}");
			}
			return HandleStatsGraph(req, caps["graph"]);
		}
	}

	// /downloads/{hash} — single-resource detail (GET / HEAD) and the
	// mutation surface (PATCH for status/priority/category, DELETE for
	// clear-completed single). `{hash}` is the lowercase 32-char hex
	// MD4 hash (dispatcher lower-cases input on the way in).
	{
		static const auto download_detail = web_api_path::ParsePattern("/api/v0/downloads/{hash}");
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
			return ErrorResponse(405,
				"method_not_allowed",
				"only GET / HEAD / PATCH / DELETE on /downloads/{hash}");
		}
	}

	// Static-frontend fallthrough. Anything that didn't match an
	// /api/v0/* route and is a safe-method request for a non-API path
	// is a candidate. ServeStaticFile is a no-op (404) when StaticRoot
	// is unset, so API-only deployments keep their historical
	// behaviour. Auth is intentionally NOT required here — the shell
	// itself is public; the API calls it makes still go through the
	// per-handler role gates.
	if ((req.method == "GET" || req.method == "HEAD") && path.compare(0, 5, "/api/") != 0) {
		return ServeStaticFile(req, path);
	}

	return ErrorResponse(404, "not_found", "no such endpoint");
}

CHttpServer::Response CApiDispatcher::ServeStaticFile(
	const CHttpServer::Request &req, const std::string &url_path)
{
	// Resolve once per process. Conf override wins; otherwise we walk
	// the install-path discovery chain. `m_static_root_resolved`
	// guards against re-walking on every request (the answer is
	// stable for the daemon's lifetime — operators editing
	// amuleapi.conf at runtime restart the daemon).
	if (!m_static_root_resolved) {
		m_static_root_cache = m_config.ServerCfg().static_root;
		if (m_static_root_cache.empty()) {
			m_static_root_cache = ResolveDefaultStaticDir();
		}
		m_static_root_resolved = true;
	}
	const std::string &root = m_static_root_cache;
	if (root.empty()) {
		// API-only deployment AND nothing on disk to fall back to.
		return ErrorResponse(404, "not_found", "no such endpoint");
	}

	// Map "/" → SPA entry. Strip leading slash so the join is relative;
	// `LooksMalicious` (run at the top of DispatchToHandler) already
	// rejected NUL / encoded NUL / encoded `..` / literal `..` segments.
	std::string rel =
		(url_path == "/" || url_path.empty()) ? std::string("index.html") : url_path.substr(1);

	std::string fs_path;
	struct stat st
	{
	};
	std::string body;
	bool found = webapi::ResolveWithinRoot(root, rel, fs_path) && ReadStaticFile(fs_path, body, st);

	// SPA fallback: an extension-less path that didn't resolve is
	// treated as a client-side route and served the entry document so
	// a deep-linked reload still boots the app. Paths that look like
	// an asset (carry an extension) 404 honestly so a missing JS/CSS
	// failure is visible rather than masked by an HTML response.
	if (!found && rel.find('.') == std::string::npos) {
		if (webapi::ResolveWithinRoot(root, "index.html", fs_path) &&
			ReadStaticFile(fs_path, body, st)) {
			rel = "index.html";
			found = true;
		}
	}

	if (!found) {
		return ErrorResponse(404, "not_found", "no such file");
	}

	const std::string etag = BuildStaticEtag(st);

	// Conditional GET: client sent If-None-Match → 304 with no body
	// when the ETag matches. ETag is mtime+size, so a rebuild of the
	// frontend invalidates without manual cache-busting.
	auto inm = req.headers.find("If-None-Match");
	if (inm != req.headers.end() && inm->second == etag) {
		CHttpServer::Response r;
		r.status = 304;
		r.headers["ETag"] = etag;
		return r;
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = StaticContentType(rel);
	r.body = (req.method == "HEAD") ? std::string() : std::move(body);
	r.headers["ETag"] = etag;
	return r;
}

CHttpServer::Response CApiDispatcher::HandleVersion(const CHttpServer::Request &)
{
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("name");
	w.ValueString(wxT("amuleapi"));
	w.Key("api_version");
	w.ValueString(wxT("v0"));
	w.Key("amule_version");
	w.ValueString(wxString::FromAscii(VERSION));
	// Version of the connected amuled, from the EC handshake. Empty
	// string when EC is not (yet) connected or the daemon predates the
	// EC_TAG_SERVER_VERSION tag. Distinct from amule_version above,
	// which is amuleapi's own build version.
	w.Key("daemon_version");
	w.ValueString(m_app.GetDaemonVersion());
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
		CHttpServer::Response r =
			ErrorResponse(429, "rate_limited", "too many failed attempts; retry later");
		char retry_after[32];
		std::snprintf(retry_after,
			sizeof(retry_after),
			"%lld",
			static_cast<long long>(decision.retry_after_seconds));
		r.headers["Retry-After"] = retry_after;
		return r;
	}

	// Refuse early if amuleapi has no passwords configured at all —
	// otherwise every login would silently fail and the operator
	// debugging "why isn't login working" would think the JWT was
	// the problem.
	if (m_config.AdminPasswordMd5().empty() && m_config.GuestPasswordMd5().empty()) {
		return ErrorResponse(503,
			"login_disabled",
			"amuleapi has no admin/guest password configured; "
			"set one via `amuleapi --set-admin-pass=<plain>`");
	}

	// Parse `{"password": "<plain>"}`. Anything else gets a 400.
	// Route through ParseJsonObjectBody so the pre-auth login path
	// shares the same depth-cap defence the rest of the body
	// parses get; without it a deeply-nested `{"a":{"a":...}}` would
	// blow the worker thread's stack via picojson's recursive
	// descent — and login is reachable unauthenticated.
	picojson::value v;
	std::string err;
	if (!ParseJsonObjectBody(req.body, v, err)) {
		return ErrorResponse(400, "bad_request", "body must be JSON object {\"password\": \"...\"}");
	}
	const auto &obj = v.get<picojson::object>();
	auto pw_it = obj.find("password");
	if (pw_it == obj.end() || !pw_it->second.is<std::string>()) {
		return ErrorResponse(400, "bad_request", "missing or non-string `password` field");
	}
	const wxString plain = wxString::FromUTF8(pw_it->second.get<std::string>().c_str());
	const std::string md5_hex(MD5Sum(plain).GetHash().utf8_str());

	// Compare against admin first, then guest. ConstantTimeEquals is
	// length-leaking by design (both sides are 32 hex chars, so length
	// is fixed) but byte-content equality is constant time.
	Role role = Role::GUEST;
	bool match = false;
	if (!m_config.AdminPasswordMd5().empty() &&
		webcommon::ConstantTimeEquals(md5_hex, m_config.AdminPasswordMd5())) {
		role = Role::ADMIN;
		match = true;
	} else if (!m_config.GuestPasswordMd5().empty() &&
		   webcommon::ConstantTimeEquals(md5_hex, m_config.GuestPasswordMd5())) {
		role = Role::GUEST;
		match = true;
	}

	if (!match) {
		m_rateLimiter.NoteFailure(ip);
		return ErrorResponse(
			401, "invalid_credentials", "password does not match any configured role");
	}

	m_rateLimiter.NoteSuccess(ip);

	const CJwt::IssuedToken issued = m_jwt.Issue(role);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	r.headers["Set-Cookie"] = MakeSetCookie(kSessionCookieName, issued.token, issued.expires_at);

	// Default (cookie-auth, browser): the HttpOnly+SameSite cookie
	// carries the token. Echoing it into the JSON body would defeat
	// HttpOnly — any XSS that `fetch('/auth/login', ...)` could read
	// the body and exfiltrate the bearer. So the default response is
	// deliberately token-less.
	//
	// Bearer opt-in (SDK / curl / no cookie jar): client passes
	// `Accept: application/jwt` or `?type=bearer` to get the bearer
	// shape — `token`, `expires_at`, `expires_at_unix`, `jti`. The
	// cookie still ships; bearer clients can ignore it.
	bool wants_bearer = false;
	{
		const std::string accept = FindHeaderCaseInsensitive(req.headers, "Accept");
		if (accept.find("application/jwt") != std::string::npos) {
			wants_bearer = true;
		}
		if (!wants_bearer) {
			std::string q;
			const std::size_t qpos = req.target.find('?');
			if (qpos != std::string::npos)
				q = req.target.substr(qpos + 1);
			const auto qmap = web_api_path::ParseQuery(q);
			const auto it = qmap.find("type");
			if (it != qmap.end() && it->second == "bearer") {
				wants_bearer = true;
			}
		}
	}

	CJsonWriter w;
	w.BeginObject();
	if (wants_bearer) {
		w.Key("token");
		w.ValueString(wxString::FromUTF8(issued.token.c_str()));
	}
	w.Key("role");
	w.ValueString(role == Role::ADMIN ? wxT("admin") : wxT("guest"));
	w.Key("expires_at");
	w.ValueString(wxString::FromUTF8(webapi::FormatIso8601Utc(issued.expires_at).c_str()));
	w.Key("expires_at_unix");
	w.ValueInt(static_cast<int64_t>(issued.expires_at));
	if (wants_bearer) {
		w.Key("jti");
		w.ValueString(wxString::FromUTF8(issued.jti.c_str()));
	}
	w.EndObject();
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}

CHttpServer::Response CApiDispatcher::HandleLogout(const CHttpServer::Request &req)
{
	// Generic-401 cap applies to logout too — repeat 401s here are a
	// credential-stuffing signal even on the idempotent path. Locked-
	// out IPs short-circuit before any MAC compare.
	const std::string &ip = req.remote_addr;
	{
		const auto decision = m_authRateLimiter.Check(ip);
		if (decision.locked_out) {
			CHttpServer::Response r = ErrorResponse(
				429, "rate_limited", "too many failed auth attempts; retry later");
			char retry_after[32];
			std::snprintf(retry_after,
				sizeof(retry_after),
				"%lld",
				static_cast<long long>(decision.retry_after_seconds));
			r.headers["Retry-After"] = retry_after;
			return r;
		}
	}

	// Logout is idempotent. A revoked-but-not-yet-expired token
	// should still get a 200 noop — the operation it requested has
	// already happened. Without this, a browser tab that does
	// `fetch('/auth/logout', ...)` twice in quick succession (page
	// reload during the request, double-tap on the menu, etc.) sees
	// a 401 on the second attempt and renders a confusing "session
	// expired" toast. Inline a softer flow than AuthenticateRequest:
	// reject only on bad-sig/expired/missing, treat revoked as a
	// noop.
	std::string token;
	auto auth_it = req.headers.find("Authorization");
	if (auth_it == req.headers.end()) {
		for (const auto &h : req.headers) {
			if (h.first.size() == 13 && strncasecmp(h.first.c_str(), "Authorization", 13) == 0) {
				auth_it = req.headers.find(h.first);
				break;
			}
		}
	}
	if (auth_it != req.headers.end()) {
		token = webapi::ExtractBearerToken(auth_it->second);
	}
	if (token.empty()) {
		auto ck_it = req.headers.find("Cookie");
		if (ck_it == req.headers.end()) {
			for (const auto &h : req.headers) {
				if (h.first.size() == 6 && strncasecmp(h.first.c_str(), "Cookie", 6) == 0) {
					ck_it = req.headers.find(h.first);
					break;
				}
			}
		}
		if (ck_it != req.headers.end()) {
			token = webapi::ExtractCookieValue(ck_it->second, kSessionCookieName);
		}
	}
	if (token.empty()) {
		m_authRateLimiter.NoteFailure(ip);
		return ErrorResponse(401, "unauthorized", "missing bearer token or session cookie");
	}
	CJwt::VerifyResult v;
	if (!m_jwt.Verify(token, v)) {
		m_authRateLimiter.NoteFailure(ip);
		return ErrorResponse(401, "unauthorized", "invalid or expired token");
	}
	// Already revoked → 200 noop (don't re-revoke, don't re-emit a
	// clear-cookie that might race with the browser's own delete).
	if (!m_revocations.IsRevoked(v.jti)) {
		// Add the jti to the revocation set with the JWT's own exp as
		// the TTL — once the token would have expired anyway, the GC
		// drops the entry.
		m_revocations.Revoke(v.jti, v.exp);
	}
	m_authRateLimiter.NoteSuccess(ip);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	r.headers["Set-Cookie"] = MakeClearCookie(kSessionCookieName);

	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.EndObject();
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSession(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";

	CJsonWriter w;
	w.BeginObject();
	w.Key("role");
	w.ValueString(a.verified.role == Role::ADMIN ? wxT("admin") : wxT("guest"));
	w.Key("jti");
	w.ValueString(wxString::FromUTF8(a.verified.jti.c_str()));
	w.Key("exp");
	w.ValueString(wxString::FromUTF8(webapi::FormatIso8601Utc(a.verified.exp).c_str()));
	w.Key("exp_unix");
	w.ValueInt(static_cast<int64_t>(a.verified.exp));
	w.EndObject();
	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}

CHttpServer::Response CApiDispatcher::HandleStatus(const CHttpServer::Request &req)
{
	// Read endpoints: any authenticated role is enough (admin OR
	// guest). mutating endpoints will gate on `admin` only.
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	// Until the refresher has completed at least one tick, the cache
	// is empty. Return 503 with a structured code so clients can
	// retry rather than guessing — saves a round of confused log-
	// reading when the daemon just came up.
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	// Single shared_lock for the whole composite read. Dashboard()
	// returns a (status, kad, snapshot_at, ec_connected) tuple in
	// one m_state lock acquisition, so a refresher tick cannot land
	// between sub-snapshots and produce an inconsistent rollup
	// (kad.network from tick N+1 while ed2k.* / speeds.* are from
	// tick N). Caller-side aliases keep the rest of the function
	// reading the same way the four-accessor version did.
	const webapi::CState::DashboardSnapshot d = m_state.Dashboard();
	const webapi::StatusSnapshot &s = d.status;
	const webapi::KadSnapshot &k = d.kad;
	const std::time_t ts = d.snapshot_at;
	const bool ec = d.ec_connected;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";

	CJsonWriter w;
	w.BeginObject();
	// snapshot_at + snapshot_at_unix were retired from
	// every envelope response so the ETag/If-None-Match cache
	// actually gets cache hits on list endpoints.
	// `ec_connected` is the dedicated staleness signal — it flips
	// false when the refresher tick fails. Standard HTTP `Date:`
	// header carries wall-clock for any consumer that needs it.
	w.Key("ec_connected");
	w.ValueBool(ec);
	(void)ts;

	w.Key("ed2k");
	w.BeginObject();
	w.Key("state");
	w.ValueString(wxString::FromUTF8(s.ed2k_state.c_str()));
	w.Key("low_id");
	w.ValueBool(s.ed2k_lowid);
	w.Key("server_name");
	w.ValueString(wxString::FromUTF8(s.server_name.c_str()));
	w.Key("server_ip");
	w.ValueString(wxString::FromUTF8(s.server_ip.c_str()));
	w.Key("server_port");
	w.ValueInt(static_cast<int64_t>(s.server_port));
	// Network rollup, symmetric with kad.network below. Aggregate
	// user + file counts across all connected ed2k servers, taken
	// from the same EC_OP_STAT_REQ response the kad counters ride
	// on — no extra round-trip.
	w.Key("network");
	w.BeginObject();
	w.Key("users");
	w.ValueInt(static_cast<int64_t>(s.ed2k_users));
	w.Key("files");
	w.ValueInt(static_cast<int64_t>(s.ed2k_files));
	w.EndObject();
	w.EndObject();

	w.Key("kad");
	w.BeginObject();
	w.Key("state");
	w.ValueString(wxString::FromUTF8(s.kad_state.c_str()));
	w.Key("firewalled");
	w.ValueBool(s.kad_firewalled);
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
	w.Key("users");
	w.ValueInt(static_cast<int64_t>(k.users));
	w.Key("files");
	w.ValueInt(static_cast<int64_t>(k.files));
	w.Key("nodes");
	w.ValueInt(static_cast<int64_t>(k.nodes));
	w.EndObject();
	w.EndObject();

	w.Key("speeds");
	w.BeginObject();
	w.Key("download_bps");
	w.ValueInt(static_cast<int64_t>(s.download_bps));
	w.Key("upload_bps");
	w.ValueInt(static_cast<int64_t>(s.upload_bps));
	w.EndObject();

	w.Key("queue");
	w.BeginObject();
	w.Key("upload_queue_length");
	w.ValueInt(static_cast<int64_t>(s.ul_queue_len));
	w.Key("total_source_count");
	w.ValueInt(static_cast<int64_t>(s.total_src_count));
	w.EndObject();
	// Nickname is a /preferences field, not a /status one.
	w.EndObject();

	const wxString js = w.GetBuffer();
	const wxScopedCharBuffer ub = js.utf8_str();
	r.body.assign(ub.data(), ub.length());
	return r;
}

namespace
{

// Compact helper — every read endpoint serialises its body the same
// way (build a wxString via CJsonWriter, then utf8_str into the
// response body). Hide the boilerplate behind a helper that takes a
// ready CJsonWriter.
void FinalizeJsonBody(CJsonWriter &w, CHttpServer::Response &r)
{
	const wxString &js = w.GetBuffer();
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
//  - count = ceil(size / PARTSIZE)
//  - mark a part "has gap" if any byte-range in `gaps` covers it
//  - state = "complete"   (no gap) /
//            "incomplete" (gap + sources > 0) /
//            "missing"    (gap + zero sources)
// `gaps` is flat (start, end) uint64 pairs. Both inclusive on amule's
// side (CGapList::Encode semantics).
void WriteProgressParts(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	w.Key("parts");
	w.BeginArray();
	if (f.size == 0) {
		w.EndArray();
		return;
	}
	const std::uint64_t part_count = (f.size + kPartSize - 1) / kPartSize;
	std::vector<bool> has_gap(part_count, false);
	const auto &gaps = f.download.decoded_gaps;
	const std::size_t gap_pair_count = gaps.size() / 2;
	for (std::size_t g = 0; g < gap_pair_count; ++g) {
		const std::uint64_t gap_start = gaps[2 * g];
		const std::uint64_t gap_end = gaps[2 * g + 1];
		const std::uint64_t start_idx = gap_start / kPartSize;
		const std::uint64_t end_idx = gap_end / kPartSize;
		for (std::uint64_t i = start_idx; i <= end_idx && i < part_count; ++i) {
			has_gap[static_cast<std::size_t>(i)] = true;
		}
	}
	const auto &part_sources = f.download.decoded_part_sources;
	for (std::uint64_t i = 0; i < part_count; ++i) {
		const std::uint16_t sources = (static_cast<std::size_t>(i) < part_sources.size())
						      ? part_sources[static_cast<std::size_t>(i)]
						      : static_cast<std::uint16_t>(0);
		const char *state = !has_gap[static_cast<std::size_t>(i)]
					    ? "complete"
					    : (sources > 0 ? "incomplete" : "missing");
		w.BeginObject();
		w.Key("state");
		w.ValueString(wxString::FromAscii(state));
		w.Key("sources");
		w.ValueInt(static_cast<int64_t>(sources));
		w.EndObject();
	}
	w.EndArray();
}

void WriteDownloadObject(CJsonWriter &w, const webapi::FileSnapshot &f, bool include_parts = false)
{
	w.BeginObject();
	w.Key("hash");
	w.ValueString(wxString::FromUTF8(f.hash.c_str()));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(f.name.c_str()));
	w.Key("ed2k_link");
	w.ValueString(wxString::FromUTF8(f.ed2k_link.c_str()));
	w.Key("size");
	w.ValueInt(static_cast<int64_t>(f.size));
	w.Key("size_done");
	w.ValueInt(static_cast<int64_t>(f.download.size_done));
	w.Key("size_xfer");
	w.ValueInt(static_cast<int64_t>(f.download.size_xfer));
	w.Key("speed_bps");
	w.ValueInt(static_cast<int64_t>(f.download.speed_bps));
	w.Key("status");
	w.ValueString(wxString::FromUTF8(f.download.status.c_str()));
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(f.priority.c_str()));
	w.Key("priority_auto");
	w.ValueBool(f.download.priority_auto);
	w.Key("category");
	w.ValueInt(static_cast<int64_t>(f.download.category));
	w.Key("sources");
	w.BeginObject();
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(f.download.sources_total));
	w.Key("not_current");
	w.ValueInt(static_cast<int64_t>(f.download.sources_not_current));
	w.Key("transferring");
	w.ValueInt(static_cast<int64_t>(f.download.sources_transferring));
	w.Key("a4af");
	w.ValueInt(static_cast<int64_t>(f.download.sources_a4af));
	w.EndObject();
	w.Key("progress");
	w.BeginObject();
	w.Key("percent");
	w.ValueDouble(f.download.percent);
	if (include_parts) {
		WriteProgressParts(w, f);
	}
	w.EndObject();
	w.EndObject();
}

void WriteClientObject(CJsonWriter &w, const webapi::ClientSnapshot &c)
{
	w.BeginObject();
	w.Key("client_ecid");
	w.ValueInt(static_cast<int64_t>(c.ecid));
	w.Key("client_name");
	w.ValueString(wxString::FromUTF8(c.client_name.c_str()));
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(c.user_hash.c_str()));
	w.Key("ip");
	w.ValueString(wxString::FromUTF8(c.ip.c_str()));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(c.port));
	w.Key("software");
	w.ValueString(wxString::FromUTF8(c.software.c_str()));
	w.Key("software_version");
	w.ValueString(wxString::FromUTF8(c.software_version.c_str()));
	w.Key("os_info");
	w.ValueString(wxString::FromUTF8(c.os_info.c_str()));
	w.Key("upload_state");
	w.ValueString(wxString::FromUTF8(c.upload_state.c_str()));
	w.Key("download_state");
	w.ValueString(wxString::FromUTF8(c.download_state.c_str()));
	w.Key("ident_state");
	w.ValueString(wxString::FromUTF8(c.ident_state.c_str()));
	w.Key("download_file_name");
	w.ValueString(wxString::FromUTF8(c.download_file_name.c_str()));
	w.Key("upload_file_hash");
	w.ValueString(wxString::FromUTF8(c.upload_file_hash.c_str()));
	w.Key("download_file_hash");
	w.ValueString(wxString::FromUTF8(c.download_file_hash.c_str()));
	w.Key("xfer");
	w.BeginObject();
	w.Key("up_session");
	w.ValueInt(static_cast<int64_t>(c.xfer_up_session));
	w.Key("down_session");
	w.ValueInt(static_cast<int64_t>(c.xfer_down_session));
	w.Key("up_total");
	w.ValueInt(static_cast<int64_t>(c.xfer_up_total));
	w.Key("down_total");
	w.ValueInt(static_cast<int64_t>(c.xfer_down_total));
	w.EndObject();
	w.Key("upload_speed_bps");
	w.ValueInt(static_cast<int64_t>(c.upload_speed_bps));
	w.Key("download_speed_bps");
	w.ValueInt(static_cast<int64_t>(c.download_speed_bps));
	w.Key("queue_waiting_position");
	w.ValueInt(static_cast<int64_t>(c.queue_waiting_position));
	w.Key("remote_queue_rank");
	w.ValueInt(static_cast<int64_t>(c.remote_queue_rank));
	w.Key("score");
	w.ValueInt(static_cast<int64_t>(c.score));
	w.Key("obfuscation_status");
	w.ValueString(wxString::FromUTF8(c.obfuscation_status.c_str()));
	w.Key("friend_slot");
	w.ValueBool(c.friend_slot);
	w.EndObject();
}

void WriteSharedObject(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	w.BeginObject();
	w.Key("hash");
	w.ValueString(wxString::FromUTF8(f.hash.c_str()));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(f.name.c_str()));
	w.Key("ed2k_link");
	w.ValueString(wxString::FromUTF8(f.ed2k_link.c_str()));
	w.Key("size");
	w.ValueInt(static_cast<int64_t>(f.size));
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(f.priority.c_str()));
	w.Key("priority_auto");
	w.ValueBool(f.shared.priority_auto);
	w.Key("complete_sources");
	w.ValueInt(static_cast<int64_t>(f.shared.complete_sources));
	w.Key("xfer");
	w.BeginObject();
	w.Key("session");
	w.ValueInt(static_cast<int64_t>(f.shared.xfer_session));
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(f.shared.xfer_total));
	w.EndObject();
	w.Key("requests");
	w.BeginObject();
	w.Key("session");
	w.ValueInt(static_cast<int64_t>(f.shared.requests_session));
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(f.shared.requests_total));
	w.EndObject();
	w.Key("accepts");
	w.BeginObject();
	w.Key("session");
	w.ValueInt(static_cast<int64_t>(f.shared.accepts_session));
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(f.shared.accepts_total));
	w.EndObject();
	w.EndObject();
}

// --- List pagination + sorting (issue #357) ---------------------------
// Server-side window shared by every list endpoint. `limit` (capped at
// 500), `offset`, `sort` (an endpoint-defined field) and `order`
// (asc|desc). Omitting `limit` returns the full set, preserving the
// pre-#357 behaviour; `total`, `offset` and `limit` metadata are always
// emitted so a paging consumer can size its requests.
struct ListParams
{
	bool has_limit = false;
	std::size_t limit = 0;
	std::size_t offset = 0;
	std::string sort; // empty = unsorted (native snapshot order)
	bool desc = false;
};

// Endpoint-specific sortable fields: field name -> ascending comparator.
// A vector (not a map) so the definition site reads as an ordered list
// and an unknown `sort` value is simply a lookup miss -> 400.
template <class T>
using ListComparators = std::vector<std::pair<std::string, std::function<bool(const T &, const T &)>>>;

std::unique_ptr<CHttpServer::Response> BadRequestPtr(const char *message)
{
	return std::make_unique<CHttpServer::Response>(ErrorResponse(400, "bad_request", message));
}

// Parse ?limit/&offset/&sort/&order from a raw query string. `limit` is
// clamped to 500; a non-numeric/oversized limit or offset, and a bad
// `order`, are 400s. `sort` is validated later against the endpoint's
// comparator table (BuildListWindow).
std::unique_ptr<CHttpServer::Response> ParseListParams(const std::string &query, ListParams &out)
{
	const auto qmap = web_api_path::ParseQuery(query);
	auto parseCount = [](const std::string &s, std::size_t &v) -> bool {
		if (s.empty() || s.size() > 9) // > ~1e9 is nonsense for these lists
			return false;
		std::size_t val = 0;
		for (char c : s) {
			if (c < '0' || c > '9')
				return false;
			val = val * 10 + static_cast<std::size_t>(c - '0');
		}
		v = val;
		return true;
	};
	const auto limit_it = qmap.find("limit");
	if (limit_it != qmap.end()) {
		std::size_t v = 0;
		if (!parseCount(limit_it->second, v))
			return BadRequestPtr("`limit` must be a non-negative integer");
		out.has_limit = true;
		out.limit = std::min<std::size_t>(v, 500);
	}
	const auto offset_it = qmap.find("offset");
	if (offset_it != qmap.end()) {
		std::size_t v = 0;
		if (!parseCount(offset_it->second, v))
			return BadRequestPtr("`offset` must be a non-negative integer");
		out.offset = v;
	}
	const auto order_it = qmap.find("order");
	if (order_it != qmap.end()) {
		if (order_it->second == "asc")
			out.desc = false;
		else if (order_it->second == "desc")
			out.desc = true;
		else
			return BadRequestPtr("`order` must be \"asc\" or \"desc\"");
	}
	const auto sort_it = qmap.find("sort");
	if (sort_it != qmap.end())
		out.sort = sort_it->second;
	return nullptr;
}

// Stable-sort the full set (if `params.sort` is set) then slice to the
// requested window. `out_window` is filled with pointers into `items`
// (no element copies) and `out_total` with the pre-slice count. Returns
// a 400 when `params.sort` is set but absent from `comparators`.
template <class T>
std::unique_ptr<CHttpServer::Response> BuildListWindow(const std::vector<T> &items,
	const ListParams &params,
	const ListComparators<T> &comparators,
	std::vector<const T *> &out_window,
	std::size_t &out_total)
{
	out_total = items.size();
	std::vector<const T *> ptrs;
	ptrs.reserve(items.size());
	for (const auto &it : items)
		ptrs.push_back(&it);

	if (!params.sort.empty()) {
		auto c = std::find_if(comparators.begin(), comparators.end(), [&](const auto &p) {
			return p.first == params.sort;
		});
		if (c == comparators.end())
			return BadRequestPtr("unknown `sort` field for this endpoint");
		const auto &cmp = c->second;
		std::stable_sort(ptrs.begin(), ptrs.end(), [&](const T *a, const T *b) {
			return params.desc ? cmp(*b, *a) : cmp(*a, *b);
		});
	}

	const std::size_t begin = std::min(params.offset, out_total);
	const std::size_t end = params.has_limit ? std::min(begin + params.limit, out_total) : out_total;
	out_window.assign(ptrs.begin() + begin, ptrs.begin() + end);
	return nullptr;
}

// Emit the `total` / `offset` / `limit` pagination metadata. `limit`
// echoes the effective page size (the actual number returned when the
// caller omitted `limit`).
void WritePageMeta(CJsonWriter &w, std::size_t total, const ListParams &params, std::size_t returned)
{
	w.Key("total");
	w.ValueUInt(total);
	w.Key("offset");
	w.ValueUInt(params.offset);
	w.Key("limit");
	w.ValueUInt(params.has_limit ? params.limit : returned);
}

// Extract the raw query string from a request target ("/x?a=1" -> "a=1").
std::string QueryOf(const CHttpServer::Request &req)
{
	std::string path, query;
	SplitPathAndQuery(req.target, path, query);
	return query;
}

// Helper for every list endpoint's envelope: the list under its named
// key plus #357 pagination metadata. ec_unavailable + 503 is emitted here
// so each handler doesn't repeat the check.
template <class T, class WriterFn>
CHttpServer::Response ListResponse(const webapi::CState &state,
	const char *plural_key,
	const std::vector<T> &items,
	WriterFn write_item,
	const ListParams &params = ListParams(),
	const ListComparators<T> &comparators = ListComparators<T>())
{
	if (!state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}
	std::vector<const T *> window;
	std::size_t total = 0;
	if (auto err = BuildListWindow(items, params, comparators, window, total))
		return *err;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	// envelope responses dropped snapshot_at_* — they were
	// defeating the ETag cache by churning the body bytes
	// every refresher tick. The ETag is now the cache
	// validator; HTTP `Date:` is the wall-clock.
	CJsonWriter w;
	w.BeginObject();
	w.Key(plural_key);
	w.BeginArray();
	for (const T *item : window)
		write_item(w, *item);
	w.EndArray();
	WritePageMeta(w, total, params, window.size());
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

} // namespace

// ===================================================================
// Mutation helpers — shared by every Handle{Resource}{Patch,Add,
// Delete} below. Every mutation handler follows:
//  1. AuthenticateRequest (bearer or cookie)
//  2. RequireAdmin (mutations are admin-only)
//  3. Parse JSON body
//  4. Send EC mutation packet via SendRecvSerialized
//  5. EC_OP_NOOP = success; EC_OP_FAILED carries amuled's rejection
//  6. Run RefresherTick inline on the HTTP thread so the response
//     sees post-mutation state (vs. next refresher tick ~1 s later)
//  7. Return the updated resource (or 201 / 204 per HTTP convention)
// ===================================================================

namespace
{

// Admin role gate. Drop-in for the standard ` if (!a.ok) return
// a.rejection;` pattern; mutations chain ` if (auto r = RequireAdmin(a))
// return *r;` immediately after.
std::unique_ptr<CHttpServer::Response> RequireAdmin(const AuthOutcome &a)
{
	if (a.verified.role != Role::ADMIN) {
		return std::unique_ptr<CHttpServer::Response>(new CHttpServer::Response(
			ErrorResponse(403, "forbidden", "admin role required for this endpoint")));
	}
	return nullptr;
}

// JSON body parser. Returns true on success; false + `err` on
// failure. Non-object roots are rejected.
//
// Pre-parse depth cap. picojson uses unbounded recursive descent for
// `_parse_array` / `_parse_object` — a `{"a":{"a":...}}` body deep
// enough blows the worker thread stack. 32 openers is past anything
// legitimate (bodies are flat lists of scalars). Mirrors CJwt::Verify.
bool ParseJsonObjectBody(const std::string &body, picojson::value &out, std::string &err)
{
	constexpr std::size_t kMaxJsonOpeners = 32;
	std::size_t openers = 0;
	for (char c : body) {
		if (c == '{' || c == '[') {
			if (++openers > kMaxJsonOpeners) {
				err = "JSON nesting too deep";
				return false;
			}
		}
	}
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
	if (!resp)
		return false;
	if (resp->GetOpCode() != EC_OP_FAILED)
		return false;
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
	if (name == "low") {
		out = PR_LOW;
		return true;
	} else if (name == "normal") {
		out = PR_NORMAL;
		return true;
	} else if (name == "high") {
		out = PR_HIGH;
		return true;
	} else if (name == "release") {
		out = PR_VERYHIGH;
		return true;
	} else if (name == "auto") {
		out = PR_AUTO;
		return true;
	}
	return false;
}

// MD4 hex string → CMD4Hash. Returns false if the string isn't 32
// lowercase-or-uppercase hex chars (we tolerate both cases; the
// route already lowercases what comes off the URL).
bool HashFromHex(const std::string &hex, CMD4Hash &out)
{
	if (hex.size() != 32)
		return false;
	return out.Decode(wxString::FromAscii(hex.c_str()));
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleDownloads(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	// /downloads filters status=="completed" out by default.
	// amuled holds finished downloads in `m_completedDownloads` as a
	// separate "awaiting clear" list; surfacing them in /downloads
	// alongside in-progress files confuses consumers reading
	// "what's currently in the transfer queue." Opt back in with
	// `?include_completed=1`. The detail endpoint
	// `GET /downloads/{hash}` is unaffected — consumer asked for that
	// specific file. may add an explicit clear-completed
	// mutation.
	bool include_completed = false;
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos)
			query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("include_completed");
		if (it != qmap.end()) {
			const std::string &v = it->second;
			include_completed = (v == "1" || v == "true" || v == "yes");
		}
	}

	std::vector<webapi::FileSnapshot> downloads = m_state.Downloads();
	if (!include_completed) {
		downloads.erase(std::remove_if(downloads.begin(),
					downloads.end(),
					[](const webapi::FileSnapshot &d) {
						return d.download.status == "completed";
					}),
			downloads.end());
	}

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	static const ListComparators<webapi::FileSnapshot> kComps = {
		{ "name",
			[](const webapi::FileSnapshot &a, const webapi::FileSnapshot &b) {
				return a.name < b.name;
			} },
		{ "size",
			[](const webapi::FileSnapshot &a, const webapi::FileSnapshot &b) {
				return a.size < b.size;
			} },
		{ "progress",
			[](const webapi::FileSnapshot &a, const webapi::FileSnapshot &b) {
				return a.download.percent < b.download.percent;
			} },
		{ "speed",
			[](const webapi::FileSnapshot &a, const webapi::FileSnapshot &b) {
				return a.download.speed_bps < b.download.speed_bps;
			} },
		{ "status",
			[](const webapi::FileSnapshot &a, const webapi::FileSnapshot &b) {
				return a.download.status < b.download.status;
			} },
	};
	return ListResponse(
		m_state,
		"downloads",
		downloads,
		[](CJsonWriter &w, const webapi::FileSnapshot &d) {
			// List mode — omit `progress.parts` (Q2 + the per-list
			// shape: omitting parts keeps the list response compact,
			// detail endpoint is where parts ship).
			WriteDownloadObject(w, d, /*include_parts=*/false);
		},
		params,
		kComps);
}

CHttpServer::Response CApiDispatcher::HandleClients(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	// Optional `?filter=uploads | downloads | active` query parameter.
	// `uploads`   → peers actively transferring TO us (upload_state ==
	//              "uploading"). Subset that maps to the legacy
	//              amuleweb "Uploads" page.
	// `downloads` → peers we're actively pulling FROM (download_state
	//              == "downloading").
	// `active`    → union of the two; everything currently moving
	//              bytes either direction.
	// No filter → every peer the daemon knows about (default, v0.1
	// shape).
	std::string filter;
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos)
			query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("filter");
		if (it != qmap.end())
			filter = it->second;
	}
	if (!filter.empty() && filter != "uploads" && filter != "downloads" && filter != "active") {
		return ErrorResponse(
			400, "bad_request", "`filter` must be one of \"uploads\", \"downloads\", \"active\"");
	}

	auto clients = m_state.Clients();
	if (!filter.empty()) {
		auto matches = [&](const webapi::ClientSnapshot &c) {
			const bool up = (c.upload_state == "uploading");
			const bool down = (c.download_state == "downloading");
			if (filter == "uploads")
				return up;
			if (filter == "downloads")
				return down;
			/* active */ return up || down;
		};
		clients.erase(std::remove_if(clients.begin(),
				      clients.end(),
				      [&](const webapi::ClientSnapshot &c) { return !matches(c); }),
			clients.end());
	}

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	static const ListComparators<webapi::ClientSnapshot> kComps = {
		{ "name",
			[](const webapi::ClientSnapshot &a, const webapi::ClientSnapshot &b) {
				return a.client_name < b.client_name;
			} },
		{ "software",
			[](const webapi::ClientSnapshot &a, const webapi::ClientSnapshot &b) {
				return a.software < b.software;
			} },
	};
	return ListResponse(m_state, "clients", clients, WriteClientObject, params, kComps);
}

CHttpServer::Response CApiDispatcher::HandleSharedList(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	static const ListComparators<webapi::FileSnapshot> kComps = {
		{ "name",
			[](const webapi::FileSnapshot &a, const webapi::FileSnapshot &b) {
				return a.name < b.name;
			} },
		{ "size",
			[](const webapi::FileSnapshot &a, const webapi::FileSnapshot &b) {
				return a.size < b.size;
			} },
	};
	return ListResponse(m_state, "shared", m_state.Shared(), WriteSharedObject, params, kComps);
}

CHttpServer::Response CApiDispatcher::HandleDownloadDetail(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	// {hash} is the 32-char lowercase-hex MD4. URL is case-tolerant;
	// State writes lowercase, so we down-case the capture before the
	// O(1) m_hash_to_ecid lookup.
	webapi::FileSnapshot d;
	std::string needle = key;
	std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
		return std::tolower(c);
	});
	if (!m_state.FindDownload(needle, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
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
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteDownloadObject(w, d, /*include_parts=*/true);
	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// --- Bulk mutation results (issue #358) -------------------------------
// Every bulk mutation (POST /downloads, PATCH/DELETE /downloads,
// PATCH /shared) reports one entry per input item under a unified
// `results` array, so a client that submits N items learns the fate of
// each without parallel arrays or a first-error-only summary.
struct BulkItem
{
	std::string id; // the item key: ed2k link or MD4 hash
	bool ok = false;
	int http = 200;      // per-item semantic status; used only to aggregate
	std::string code;    // error code   (when !ok)
	std::string message; // error message (when !ok)
};

BulkItem BulkOk(const std::string &id)
{
	BulkItem b;
	b.id = id;
	b.ok = true;
	return b;
}

BulkItem BulkErr(const std::string &id, int http, const char *code, const std::string &message)
{
	BulkItem b;
	b.id = id;
	b.ok = false;
	b.http = http;
	b.code = code;
	b.message = message;
	return b;
}

// Emit `{"results":[{"id","ok"[,"error":{"code","message"}]}]}`. Aggregate
// status: every item ok -> `all_ok_status`; every item failed because the
// daemon was unreachable (503) -> 503; any other mix -> 207 Multi-Status.
CHttpServer::Response BulkResultsResponse(const std::vector<BulkItem> &items, int all_ok_status)
{
	bool all_ok = true;
	bool all_unreachable = !items.empty();
	for (const auto &it : items) {
		if (!it.ok) {
			all_ok = false;
			if (it.http != 503)
				all_unreachable = false;
		} else {
			all_unreachable = false;
		}
	}

	CHttpServer::Response r;
	r.status = all_ok ? all_ok_status : (all_unreachable ? 503 : 207);
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("results");
	w.BeginArray();
	for (const auto &it : items) {
		w.BeginObject();
		w.Key("id");
		w.ValueString(wxString::FromUTF8(it.id.c_str()));
		w.Key("ok");
		w.ValueBool(it.ok);
		if (!it.ok) {
			w.Key("error");
			w.BeginObject();
			w.Key("code");
			w.ValueString(wxString::FromUTF8(it.code.c_str()));
			w.Key("message");
			w.ValueString(wxString::FromUTF8(it.message.c_str()));
			w.EndObject();
		}
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// Extract a non-empty `hashes` string array (max 500) from a parsed body.
// On any shape problem fills `err` with a 400 and returns false.
bool ParseBulkHashes(const picojson::object &obj, std::vector<std::string> &out, CHttpServer::Response &err)
{
	const auto it = obj.find("hashes");
	if (it == obj.end() || !it->second.is<picojson::array>()) {
		err = ErrorResponse(400, "bad_request", "`hashes` must be an array of 32-char hex strings");
		return false;
	}
	const auto &arr = it->second.get<picojson::array>();
	if (arr.empty()) {
		err = ErrorResponse(400, "bad_request", "`hashes` must contain at least one entry");
		return false;
	}
	if (arr.size() > 500) {
		err = ErrorResponse(400, "bad_request", "`hashes` may contain at most 500 entries");
		return false;
	}
	out.clear();
	out.reserve(arr.size());
	for (const auto &v : arr) {
		if (!v.is<std::string>()) {
			err = ErrorResponse(400, "bad_request", "`hashes` entries must be strings");
			return false;
		}
		out.push_back(v.get<std::string>());
	}
	return true;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleDownloadAdd(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// Body shape (two forms — both accepted, exactly one required):
	//  {"ed2k_link": "ed2k://|file|...|/", "category": 0}    — singular
	//  {"links": ["ed2k://|file|...|/", ...], "category": 0} — array
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
		const auto it_array = obj.find("links");
		if (it_single != obj.end() && it_array != obj.end()) {
			return ErrorResponse(400,
				"bad_request",
				"send either `ed2k_link` (single) or `links` (array), "
				"not both");
		}
		if (it_single != obj.end()) {
			if (!it_single->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request", "`ed2k_link` must be a string");
			}
			links.push_back(it_single->second.get<std::string>());
		} else if (it_array != obj.end()) {
			if (!it_array->second.is<picojson::array>()) {
				return ErrorResponse(
					400, "bad_request", "`links` must be an array of ed2k://strings");
			}
			const auto &arr = it_array->second.get<picojson::array>();
			if (arr.empty()) {
				return ErrorResponse(
					400, "bad_request", "`links` must contain at least one entry");
			}
			links.reserve(arr.size());
			for (const auto &v : arr) {
				if (!v.is<std::string>()) {
					return ErrorResponse(400,
						"bad_request",
						"every entry in `links` must be a string");
				}
				links.push_back(v.get<std::string>());
			}
		} else {
			return ErrorResponse(400,
				"bad_request",
				"required field missing: send `ed2k_link` (string) or "
				"`links` (array of strings)");
		}
		for (const auto &link : links) {
			if (link.size() < 7 || link.compare(0, 7, "ed2k://") != 0) {
				return ErrorResponse(
					400, "bad_request", "every link must start with ed2k://");
			}
		}
	}
	std::uint8_t category = 0;
	{
		const auto it = obj.find("category");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(
					400, "bad_request", "`category` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 255) {
				return ErrorResponse(400, "bad_request", "`category` must be in [0, 255]");
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
	// Unified per-item envelope (#358): one `results` entry per submitted
	// link, keyed by the link itself. amuleapi is not yet shipped, so this
	// deliberately replaces the previous ok/accepted/failed counter shape
	// rather than extending it -- there are no released clients to break.
	std::vector<BulkItem> results;
	results.reserve(links.size());
	for (const auto &link : links) {
		std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_ADD_LINK));
		CECTag link_tag(EC_TAG_STRING, wxString::FromUTF8(link.c_str()));
		link_tag.AddTag(CECTag(EC_TAG_PARTFILE_CAT, category));
		ec_req->AddTag(link_tag);
		const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
		if (!ec_resp) {
			results.push_back(
				BulkErr(link, 503, "ec_unavailable", "EC roundtrip failed for ADD_LINK"));
			continue;
		}
		std::string ec_err_msg;
		if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
			delete ec_resp;
			results.push_back(BulkErr(link, 400, "amuled_rejected", ec_err_msg));
			continue;
		}
		delete ec_resp;
		results.push_back(BulkOk(link));
	}

	// Inline-refresh the cache so the response sees post-mutation
	// state. amuled's ADD_LINK is asynchronous (the partfile gets
	// allocated + hashed before it shows up in m_filelist), so the
	// new entry may not surface until the *next* tick — we'd still
	// return 202 Accepted with an empty resource. For now: refresh,
	// then return {ok: true} and leave the GET /downloads to surface
	// the new entry.
	(void)RefresherTick(m_app, m_state);

	// All accepted -> 202 (async: amuled allocates + hashes the partfile
	// before it surfaces in GET /downloads, typically within one tick); a
	// mix -> 207; every link blocked by an EC disconnect -> 503.
	return BulkResultsResponse(results, 202);
}

CHttpServer::Response CApiDispatcher::HandleDownloadPatch(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	webapi::FileSnapshot d;
	std::string needle = key;
	std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
		return std::tolower(c);
	});
	if (!m_state.FindDownload(needle, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	// Downstream EC ops still address by MD4 hash — read it back off
	// the snapshot we just resolved.
	CMD4Hash file_hash;
	if (!HashFromHex(d.hash, file_hash)) {
		return ErrorResponse(500, "internal_error", "failed to decode partfile hash");
	}

	// Each field present in the body fires one EC mutation. We
	// process them in a fixed order (status, priority, category) so
	// the wire effect is deterministic regardless of JSON key order.
	auto send_op = [&](ec_opcode_t op,
			       bool has_inner,
			       ec_tagname_t inner_name,
			       std::uint8_t inner_value) -> CHttpServer::Response {
		std::unique_ptr<CECPacket> p(new CECPacket(op));
		CECTag hash_tag(EC_TAG_PARTFILE, file_hash);
		if (has_inner) {
			hash_tag.AddTag(CECTag(inner_name, inner_value));
		}
		p->AddTag(hash_tag);
		const CECPacket *r = m_app.SendRecvSerialized(p.get());
		if (!r) {
			return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed");
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
				return ErrorResponse(400,
					"bad_request",
					"`status` must be one of \"paused\" or \"resumed\"");
			}
			const std::string &v = it->second.get<std::string>();
			ec_opcode_t op;
			if (v == "paused")
				op = EC_OP_PARTFILE_PAUSE;
			else if (v == "resumed")
				op = EC_OP_PARTFILE_RESUME;
			else {
				return ErrorResponse(400,
					"bad_request",
					"`status` must be one of \"paused\" or \"resumed\"");
			}
			auto err = send_op(op, /*has_inner=*/false, static_cast<ec_tagname_t>(0), 0);
			if (err.status >= 400)
				return err;
			any_change = true;
		}
	}

	// priority: "very_low"|"low"|"normal"|"high"|"release"|"auto"
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(
					400, "bad_request", "`priority` must be a wire-string enum");
			}
			std::uint8_t code = 0;
			if (!DownloadPriorityToCode(it->second.get<std::string>(), code)) {
				return ErrorResponse(400,
					"bad_request",
					"`priority` must be one of "
					"low, normal, high, release, auto");
			}
			auto err = send_op(
				EC_OP_PARTFILE_PRIO_SET, /*has_inner=*/true, EC_TAG_PARTFILE_PRIO, code);
			if (err.status >= 400)
				return err;
			any_change = true;
		}
	}

	// category: integer
	{
		const auto it = obj.find("category");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(
					400, "bad_request", "`category` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 255) {
				return ErrorResponse(400, "bad_request", "`category` must be in [0, 255]");
			}
			auto err = send_op(EC_OP_PARTFILE_SET_CAT,
				/*has_inner=*/true,
				EC_TAG_PARTFILE_CAT,
				static_cast<std::uint8_t>(v));
			if (err.status >= 400)
				return err;
			any_change = true;
		}
	}

	if (!any_change) {
		return ErrorResponse(400,
			"bad_request",
			"request body must include at least one of "
			"`status`, `priority`, or `category`");
	}

	// Inline refresh so the response below sees post-mutation state.
	(void)RefresherTick(m_app, m_state);

	// Re-read the snapshot — fall back to the prior copy if the
	// cache evicted it between mutations and this read (vanishingly
	// rare; would mean amuled removed it between our SendRecv and
	// the refresh).
	webapi::FileSnapshot d_after;
	if (!m_state.FindDownload(d.hash, d_after))
		d_after = d;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteDownloadObject(w, d_after, /*include_parts=*/false);
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleDownloadDelete(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	webapi::FileSnapshot d;
	std::string needle = key;
	std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
		return std::tolower(c);
	});
	if (!m_state.FindDownload(needle, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	// DELETE only handles ACTIVE downloads (anything not "completed").
	// Completed entries live in amuled's m_completedDownloads
	// staging list, and the only EC op that touches that list is
	// EC_OP_CLEAR_COMPLETED — which doesn't delete the on-disk file
	// from Incoming, it just acks the post-completion notification.
	// Conflating the two under one verb confused operators who
	// reasonably expected DELETE to remove a file from disk. Route
	// the completed case through POST /downloads/clear_completed
	// (which accepts an optional {hash} body for per-entry clears)
	// so the verb-vs-disk-semantic mapping stays unambiguous.
	if (d.download.status == "completed") {
		return ErrorResponse(409,
			"completed_use_clear_completed",
			"DELETE only removes active downloads (deletes .part/.met "
			"files from disk). Use POST /downloads/clear_completed "
			"with optional {\"hash\":\"...\"} body to clear a completed "
			"entry's post-completion notification — the file in the "
			"Incoming directory is NEVER removed via this API.");
	}

	CMD4Hash file_hash;
	if (!HashFromHex(d.hash, file_hash)) {
		return ErrorResponse(500, "internal_error", "failed to decode partfile hash");
	}
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_PARTFILE_DELETE));
	ec_req->AddTag(CECTag(EC_TAG_PARTFILE, file_hash));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for DELETE");
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
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("hash");
	w.ValueString(wxString::FromUTF8(d.hash.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleDownloadsClearCompleted(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	// Two shapes share this endpoint:
	//  * no body (or all-whitespace body) → bulk clear every
	//    completed entry. Original shape.
	//  * `{"hash": "<md4hex>"}` → clear that single completed entry.
	//    Hash must currently match a download with status=="completed";
	//    active / unknown hashes return 404.
	// The response envelope is identical in both branches so a client
	// that wraps the call doesn't need to fork on its own input.
	std::string target_hash;
	bool body_has_content = false;
	for (char c : req.body) {
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
			body_has_content = true;
			break;
		}
	}
	if (body_has_content) {
		picojson::value root;
		std::string parse_err;
		if (!ParseJsonObjectBody(req.body, root, parse_err)) {
			return ErrorResponse(400, "bad_request", parse_err.c_str());
		}
		const auto &obj = root.get<picojson::object>();
		const auto it_hash = obj.find("hash");
		if (it_hash != obj.end()) {
			if (!it_hash->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request", "`hash` must be a string");
			}
			target_hash = it_hash->second.get<std::string>();
			std::transform(target_hash.begin(),
				target_hash.end(),
				target_hash.begin(),
				[](unsigned char c) { return std::tolower(c); });
		}
		// Future-proof: silently ignore unknown keys rather than 400
		// so adding a flag later (e.g. {"hash": "...", "force": true})
		// doesn't break old clients.
	}

	// Collect target ECID(s). For the by-hash form, only one entry;
	// for the bulk form, every cached download with status=="completed".
	std::vector<std::uint32_t> ecids;
	std::vector<std::string> hashes_cleared;
	if (!target_hash.empty()) {
		webapi::FileSnapshot d;
		if (!m_state.FindDownload(target_hash, d)) {
			return ErrorResponse(404, "not_found", "no download with that hash");
		}
		if (d.download.status != "completed") {
			return ErrorResponse(409,
				"not_completed",
				"target download exists but is not in the completed "
				"staging list (status != \"completed\"). To remove an "
				"active partfile, use DELETE /downloads/{hash}.");
		}
		ecids.push_back(d.ecid);
		hashes_cleared.push_back(d.hash);
	} else {
		for (const auto &d : m_state.Downloads()) {
			if (d.download.status == "completed") {
				ecids.push_back(d.ecid);
				hashes_cleared.push_back(d.hash);
			}
		}
	}

	if (ecids.empty()) {
		// Nothing to do — return 200 with cleared:0 so consumers can
		// distinguish "no-op" from "amuled rejected" (both end up
		// with no visible change).
		CHttpServer::Response r;
		r.status = 200;
		r.content_type = "application/json";
		CJsonWriter w;
		w.BeginObject();
		w.Key("ok");
		w.ValueBool(true);
		w.Key("cleared");
		w.ValueInt(0);
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
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for CLEAR_COMPLETED");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Inline refresh — the response below + the next GET both must
	// show the post-clear state.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("cleared");
	w.ValueInt(static_cast<int64_t>(ecids.size()));
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

namespace
{

void WriteServerObject(CJsonWriter &w, const webapi::ServerSnapshot &s)
{
	w.BeginObject();
	// `ecid` is the URL key for /servers/{ecid}/connect and
	// /servers/{ecid}. intentionally surfaced
	// it on /clients for the same reason; servers got it later.
	w.Key("ecid");
	w.ValueInt(static_cast<int64_t>(s.ecid));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(s.name.c_str()));
	w.Key("description");
	w.ValueString(wxString::FromUTF8(s.description.c_str()));
	w.Key("version");
	w.ValueString(wxString::FromUTF8(s.version.c_str()));
	w.Key("address");
	w.ValueString(wxString::FromUTF8(s.address.c_str()));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(s.port));
	w.Key("users");
	w.ValueInt(static_cast<int64_t>(s.users));
	w.Key("max_users");
	w.ValueInt(static_cast<int64_t>(s.max_users));
	w.Key("files");
	w.ValueInt(static_cast<int64_t>(s.files));
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(s.priority.c_str()));
	w.Key("ping_ms");
	w.ValueInt(static_cast<int64_t>(s.ping_ms));
	w.Key("failed");
	w.ValueInt(static_cast<int64_t>(s.failed));
	w.Key("static");
	w.ValueBool(s.is_static);
	w.EndObject();
}

void WriteCategoryObject(CJsonWriter &w, const webapi::CategorySnapshot &c)
{
	w.BeginObject();
	w.Key("index");
	w.ValueInt(static_cast<int64_t>(c.index));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(c.name.c_str()));
	w.Key("path");
	w.ValueString(wxString::FromUTF8(c.path.c_str()));
	w.Key("comment");
	w.ValueString(wxString::FromUTF8(c.comment.c_str()));
	w.Key("color");
	w.ValueInt(static_cast<int64_t>(c.color));
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(c.priority.c_str()));
	w.EndObject();
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleServers(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	static const ListComparators<webapi::ServerSnapshot> kComps = {
		{ "name",
			[](const webapi::ServerSnapshot &a, const webapi::ServerSnapshot &b) {
				return a.name < b.name;
			} },
		{ "users",
			[](const webapi::ServerSnapshot &a, const webapi::ServerSnapshot &b) {
				return a.users < b.users;
			} },
		{ "ping",
			[](const webapi::ServerSnapshot &a, const webapi::ServerSnapshot &b) {
				return a.ping_ms < b.ping_ms;
			} },
		{ "files",
			[](const webapi::ServerSnapshot &a, const webapi::ServerSnapshot &b) {
				return a.files < b.files;
			} },
	};
	return ListResponse(m_state, "servers", m_state.Servers(), WriteServerObject, params, kComps);
}

namespace
{

// Parse an integer ECID from a path capture. Returns false on
// negative, overflow, or non-digit content (the API expects positive
// 32-bit ECIDs from the URL).
bool ParseEcidPath(const std::string &s, std::uint32_t &out)
{
	if (s.empty())
		return false;
	char *end = nullptr;
	// strtoull (not strtoul) because `unsigned long` is 32-bit on
	// Windows — there the `v > 0xFFFFFFFFu` overflow guard below
	// would be a tautology and an out-of-range path-segment like
	// `99999999999` would saturate to ULONG_MAX = 0xFFFFFFFF, then
	// silently match an actual ECID 0xFFFFFFFF. strtoull is 64-bit
	// everywhere so the cap is meaningful regardless of platform.
	errno = 0;
	const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
	if (end == s.c_str() || *end != '\0')
		return false;
	if (errno == ERANGE)
		return false;
	if (v > 0xFFFFFFFFull)
		return false;
	out = static_cast<std::uint32_t>(v);
	return true;
}

// Look up a server in the State cache by ECID. Returns false if
// no match — the handler then 404s.
bool FindServerByEcid(const webapi::CState &state, std::uint32_t ecid, webapi::ServerSnapshot &out)
{
	const auto all = state.Servers();
	for (const auto &s : all) {
		if (s.ecid == ecid) {
			out = s;
			return true;
		}
	}
	return false;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleServerAdd(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

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
			return ErrorResponse(400,
				"bad_request",
				"required string field `address` is missing (\"host:port\")");
		}
		address = it->second.get<std::string>();
		const std::size_t colon = address.find(':');
		if (colon == std::string::npos || colon == 0 || colon == address.size() - 1) {
			return ErrorResponse(400, "bad_request", "`address` must be in \"host:port\" form");
		}
	}
	std::string name;
	{
		const auto it = obj.find("name");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request", "`name` must be a string");
			}
			name = it->second.get<std::string>();
		}
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SERVER_ADD));
	ec_req->AddTag(CECTag(EC_TAG_SERVER_ADDRESS, wxString::FromUTF8(address.c_str())));
	if (!name.empty()) {
		ec_req->AddTag(CECTag(EC_TAG_SERVER_NAME, wxString::FromUTF8(name.c_str())));
	}

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SERVER_ADD");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Inline refresh — the new server should be in the next /servers
	// response without waiting on the regular tick.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 201;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("address");
	w.ValueString(wxString::FromUTF8(address.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleServerConnect(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint32_t ecid = 0;
	if (!ParseEcidPath(ecid_str, ecid)) {
		return ErrorResponse(400, "bad_request", "path `{ecid}` must be a non-negative integer");
	}
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}
	webapi::ServerSnapshot srv;
	if (!FindServerByEcid(m_state, ecid, srv)) {
		return ErrorResponse(404, "not_found", "no server with that ECID in the current snapshot");
	}

	// EC_OP_SERVER_CONNECT routes through Get_EC_Response_Server,
	// which looks up the server by IPv4 lookup (ExternalConn.cpp:1266).
	// Build EC_TAG_SERVER with the IPv4 + port from our cache.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SERVER_CONNECT));
	ec_req->AddTag(CECTag(EC_TAG_SERVER, EC_IPv4_t(srv.ip, srv.port)));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SERVER_CONNECT");
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
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("ecid");
	w.ValueInt(static_cast<int64_t>(ecid));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleServerDelete(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint32_t ecid = 0;
	if (!ParseEcidPath(ecid_str, ecid)) {
		return ErrorResponse(400, "bad_request", "path `{ecid}` must be a non-negative integer");
	}
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}
	webapi::ServerSnapshot srv;
	if (!FindServerByEcid(m_state, ecid, srv)) {
		return ErrorResponse(404, "not_found", "no server with that ECID in the current snapshot");
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SERVER_REMOVE));
	ec_req->AddTag(CECTag(EC_TAG_SERVER, EC_IPv4_t(srv.ip, srv.port)));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SERVER_REMOVE");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("ecid");
	w.ValueInt(static_cast<int64_t>(ecid));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleServerUpdateFromUrl(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();
	const auto it = obj.find("servers_url");
	if (it == obj.end() || !it->second.is<std::string>()) {
		return ErrorResponse(400, "bad_request", "required string field `servers_url` is missing");
	}
	const std::string &url = it->second.get<std::string>();
	if (url.empty()) {
		return ErrorResponse(400, "bad_request", "`servers_url` must not be empty");
	}
	// Light hygiene check — amuled will fetch this and bail if it's
	// nonsense, but rejecting obviously bad inputs at the API layer
	// gives a clearer error than the EC "amuled rejected" wrapper.
	if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
		return ErrorResponse(400, "bad_request", "`servers_url` must be an http://or https://URL");
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SERVER_UPDATE_FROM_URL));
	ec_req->AddTag(CECTag(EC_TAG_SERVERS_UPDATE_URL, wxString::FromUTF8(url.c_str())));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed");
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
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("servers_url");
	w.ValueString(wxString::FromUTF8(url.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// Resolve "<ip>:<port>" from the URL into an ECID by walking the
// servers cache. Returns 0 on miss; the caller 404s.
static std::uint32_t ResolveServerEcidByAddress(const webapi::CState &state, const std::string &ip_port)
{
	const auto colon = ip_port.rfind(':');
	if (colon == std::string::npos)
		return 0;
	const std::string ip_str = ip_port.substr(0, colon);
	const std::string port_str = ip_port.substr(colon + 1);
	if (ip_str.empty() || port_str.empty())
		return 0;
	char *end = nullptr;
	const unsigned long port = std::strtoul(port_str.c_str(), &end, 10);
	if (end == port_str.c_str() || *end != '\0' || port == 0 || port > 0xFFFF) {
		return 0;
	}
	// Parse the IP — accept dotted-quad form OR a uint32 host-order
	// number that matches ServerSnapshot::ip. We compute both so we
	// can match either against what the cache holds.
	std::uint32_t ip_he = 0;
	{
		unsigned a_, b_, c_, d_;
		if (std::sscanf(ip_str.c_str(), "%u.%u.%u.%u", &a_, &b_, &c_, &d_) == 4 && a_ <= 255 &&
			b_ <= 255 && c_ <= 255 && d_ <= 255) {
			ip_he = (a_) | (b_ << 8) | (c_ << 16) | (d_ << 24);
		}
	}
	// Require an IPv4-shaped address: drop the s.address string
	// fallback. The fallback was a convenience for hostname-form
	// URLs (e.g. `/api/v0/servers/donkey.example.com:4242/connect`),
	// but it admits an UNINTENDED match too — `s.address` is
	// populated from amuled's wire-form "name" tag, which can be
	// a hostname OR a synthetic display string ("Eserver No.1");
	// a DELETE-by-address with a colliding label could remove the
	// wrong row. IP+port exact match has no such ambiguity.
	if (ip_he == 0)
		return 0;
	for (const auto &s : state.Servers()) {
		if (s.port == static_cast<std::uint16_t>(port) && s.ip == ip_he) {
			return s.ecid;
		}
	}
	return 0;
}

CHttpServer::Response CApiDispatcher::HandleServerConnectByAddress(
	const CHttpServer::Request &req, const std::string &ip_port)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}
	const std::uint32_t ecid = ResolveServerEcidByAddress(m_state, ip_port);
	if (ecid == 0) {
		return ErrorResponse(404, "not_found", "no server matches that ip:port");
	}
	// Delegate to the ECID-keyed handler; passing the resolved ECID as
	// a decimal string keeps the contract uniform.
	std::ostringstream os;
	os << ecid;
	return HandleServerConnect(req, os.str());
}

CHttpServer::Response CApiDispatcher::HandleServerDeleteByAddress(
	const CHttpServer::Request &req, const std::string &ip_port)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}
	const std::uint32_t ecid = ResolveServerEcidByAddress(m_state, ip_port);
	if (ecid == 0) {
		return ErrorResponse(404, "not_found", "no server matches that ip:port");
	}
	std::ostringstream os;
	os << ecid;
	return HandleServerDelete(req, os.str());
}

CHttpServer::Response CApiDispatcher::HandleCategories(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	// amuled's EC suppresses the whole `EC_TAG_PREFS_CATEGORIES`
	// block when no custom categories exist, and starts including
	// index 0 once the first custom one is added. Faithful at the
	// wire layer, but a client iterating /categories expecting at
	// least the default has to special-case the empty case. Inject
	// a synthetic index-0 entry when missing so clients see the same
	// shape regardless of category count. The defaults mirror what
	// amuled emits for category 0 itself: empty title/path/comment,
	// color 0, priority_code PR_LOW (the amuled default for
	// `defaultcat->prio` in CPreferences::LoadCats).
	std::vector<webapi::CategorySnapshot> cats = m_state.Categories();
	bool has_zero = false;
	for (const auto &c : cats) {
		if (c.index == 0) {
			has_zero = true;
			break;
		}
	}
	if (!has_zero) {
		webapi::CategorySnapshot d;
		d.index = 0;
		d.priority_code = 0; // PR_LOW (matches amuled default)
		d.priority = "low";
		cats.insert(cats.begin(), std::move(d));
	}
	return ListResponse(m_state, "categories", cats, WriteCategoryObject);
}

CHttpServer::Response CApiDispatcher::HandleKad(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	const webapi::KadSnapshot k = m_state.Kad();
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	// Bare object (Q3 — Kad is a single resource, not a list).
	w.Key("state");
	w.ValueString(wxString::FromUTF8(k.state.c_str()));
	w.Key("firewalled");
	w.ValueBool(k.firewalled);
	w.Key("firewalled_udp");
	w.ValueBool(k.firewalled_udp);
	w.Key("in_lan_mode");
	w.ValueBool(k.in_lan_mode);
	w.Key("ip");
	w.ValueString(wxString::FromUTF8(k.ip.c_str()));
	w.Key("network");
	w.BeginObject();
	w.Key("users");
	w.ValueInt(static_cast<int64_t>(k.users));
	w.Key("files");
	w.ValueInt(static_cast<int64_t>(k.files));
	w.Key("nodes");
	w.ValueInt(static_cast<int64_t>(k.nodes));
	w.EndObject();
	w.Key("indexed");
	w.BeginObject();
	w.Key("sources");
	w.ValueInt(static_cast<int64_t>(k.indexed_sources));
	w.Key("keywords");
	w.ValueInt(static_cast<int64_t>(k.indexed_keywords));
	w.Key("notes");
	w.ValueInt(static_cast<int64_t>(k.indexed_notes));
	w.Key("load");
	w.ValueInt(static_cast<int64_t>(k.indexed_load));
	w.EndObject();
	w.Key("buddy");
	w.BeginObject();
	w.Key("status");
	w.ValueString(wxString::FromUTF8(k.buddy_status.c_str()));
	w.Key("ip");
	w.ValueString(wxString::FromUTF8(k.buddy_ip.c_str()));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(k.buddy_port));
	w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// `?tail=N` parser. Returns 0 if the query is absent / unparseable;
// the caller's contract is "0 means return everything". Negative or
// non-numeric values clamp to 0.
std::size_t ParseTailParam(const std::string &query)
{
	const auto qmap = web_api_path::ParseQuery(query);
	const auto it = qmap.find("tail");
	if (it == qmap.end())
		return 0;
	const long n = std::atol(it->second.c_str());
	if (n <= 0)
		return 0;
	// Cap at 100k lines so a bogus `?tail=2147483647` doesn't try to
	// serialise the entire wxString through the JSON escaper.
	const long capped = std::min<long>(n, 100000);
	return static_cast<std::size_t>(capped);
}

// Return a copy of `all` containing at most `tail` trailing lines.
// `tail == 0` means "all lines" (no tailing).
std::vector<std::string> SliceTail(const std::vector<std::string> &all, std::size_t tail)
{
	if (tail == 0 || all.size() <= tail)
		return all;
	return std::vector<std::string>(all.begin() + (all.size() - tail), all.end());
}

// For a single-string log (e.g. /logs/serverinfo), `?tail=N` slices
// at line boundaries from the END so the first line of the response
// is always whole. tail=0 returns the input verbatim.
std::string TailString(const std::string &text, std::size_t tail_lines)
{
	if (tail_lines == 0 || text.empty())
		return text;
	// Walk backwards counting newlines until we've found `tail_lines`
	// of them; whatever's after the last seen newline becomes the
	// response.
	std::size_t pos = text.size();
	std::size_t seen = 0;
	while (pos > 0 && seen < tail_lines) {
		--pos;
		if (text[pos] == '\n')
			++seen;
	}
	// Advance past the leading '\n' so the response doesn't start
	// with a blank line.
	if (pos < text.size() && text[pos] == '\n')
		++pos;
	return text.substr(pos);
}

} // namespace

namespace
{

void WriteStatsValue(CJsonWriter &w, const webapi::StatsTreeValue &v)
{
	w.BeginObject();
	w.Key("type");
	w.ValueString(wxString::FromUTF8(v.type.c_str()));
	w.Key("value");
	switch (v.kind) {
	case webapi::StatsTreeValue::Num:
		w.ValueUInt(v.num);
		break;
	case webapi::StatsTreeValue::Dbl:
		w.ValueDouble(v.dbl);
		break;
	case webapi::StatsTreeValue::Str:
		w.ValueString(wxString::FromUTF8(v.str.c_str()));
		break;
	}
	// Optional nested sub-value (the parenthetical "(total …)" some nodes carry).
	if (!v.extra.empty()) {
		w.Key("extra");
		WriteStatsValue(w, v.extra.front());
	}
	w.EndObject();
}

void WriteStatsNode(CJsonWriter &w, const webapi::StatsTreeNode &n)
{
	w.BeginObject();
	// Stable machine key, when the daemon provides one (omitted otherwise
	// so a legacy daemon simply yields no "key" field).
	if (!n.key.empty()) {
		w.Key("key");
		w.ValueString(wxString::FromUTF8(n.key.c_str()));
	}
	w.Key("label");
	w.ValueString(wxString::FromUTF8(n.label.c_str()));
	w.Key("values");
	w.BeginArray();
	for (const auto &v : n.values)
		WriteStatsValue(w, v);
	w.EndArray();
	// Raw numeric UL:DL ratio (download-per-upload), for the ratio node only.
	// Emitted when the daemon provided at least one component; each field is
	// present only when computable, so a legacy daemon yields no "ratio" key.
	if (n.has_ratio_session || n.has_ratio_total) {
		w.Key("ratio");
		w.BeginObject();
		if (n.has_ratio_session) {
			w.Key("session");
			w.ValueDouble(n.ratio_session);
		}
		if (n.has_ratio_total) {
			w.Key("total");
			w.ValueDouble(n.ratio_total);
		}
		w.EndObject();
	}
	w.Key("children");
	w.BeginArray();
	for (const auto &c : n.children)
		WriteStatsNode(w, c);
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
	if (samples.empty()) {
		w.EndArray();
		return;
	}
	const std::size_t start =
		(max_width > 0 && samples.size() > max_width) ? samples.size() - max_width : 0;
	for (std::size_t i = start; i < samples.size(); ++i) {
		const std::time_t t =
			snapshot_at - static_cast<std::time_t>((samples.size() - 1 - i) * interval);
		w.BeginObject();
		w.Key("t");
		w.ValueString(wxString::FromUTF8(webapi::FormatIso8601Utc(t).c_str()));
		w.Key("t_unix");
		w.ValueInt(static_cast<int64_t>(t));
		w.Key("value");
		w.ValueInt(static_cast<int64_t>(samples[i]));
		w.EndObject();
	}
	w.EndArray();
}

void WriteSearchObject(CJsonWriter &w, const webapi::SearchResult &r)
{
	w.BeginObject();
	w.Key("hash");
	w.ValueString(wxString::FromUTF8(r.hash.c_str()));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(r.name.c_str()));
	w.Key("size");
	w.ValueInt(static_cast<int64_t>(r.size));
	w.Key("sources");
	w.BeginObject();
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(r.source_count));
	w.Key("complete");
	w.ValueInt(static_cast<int64_t>(r.complete_source_count));
	w.EndObject();
	w.Key("already_have");
	w.ValueBool(r.already_have);
	w.Key("rating");
	w.ValueInt(static_cast<int64_t>(r.rating));
	w.EndObject();
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleStatsTree(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	// lazy-fetch with 1 s TTL coalescing. The fetcher runs
	// the EC roundtrip under m_app's m_ec_mtx (SendRecvSerialized);
	// concurrent burst reads serialize on m_stats_tree_cache's mutex
	// and the second waiter reads the just-stored value.
	auto pair =
		m_stats_tree_cache.GetOrFetch(std::chrono::milliseconds(1000), [this]() -> TtlPair_StatsTree {
			std::unique_ptr<CECPacket> req_ec(new CECPacket(EC_OP_GET_STATSTREE, EC_DETAIL_WEB));
			req_ec->AddTag(CECTag(EC_TAG_STATTREE_CAPPING, static_cast<std::uint8_t>(0)));
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
		return ErrorResponse(
			503, "ec_unavailable", "EC fetch failed for stats tree; amuled may be disconnected");
	}

	const webapi::StatsTreeNode &root = pair.first;
	const std::time_t ts = pair.second;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	// snapshot_at retired in favour of the ETag
	// as the cache validator. The TtlPair_StatsTree still tracks the
	// fetched-at time internally (drives the 1 s TTL coalescer) — it
	// just isn't surfaced any more.
	(void)ts;
	CJsonWriter w;
	w.BeginObject();
	w.Key("nodes");
	w.BeginArray();
	for (const auto &child : root.children)
		WriteStatsNode(w, child);
	w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleStatsGraph(
	const CHttpServer::Request &req, const std::string &graph)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	// Validate the graph name BEFORE fetching — saves an EC roundtrip
	// on tab-complete typos hitting /stats/graphs/<bogus>.
	const char *unit = nullptr;
	if (graph == "download") {
		unit = "bps";
	} else if (graph == "upload") {
		unit = "bps";
	} else if (graph == "connections") {
		unit = "count";
	} else if (graph == "kad") {
		unit = "count";
	} else {
		return ErrorResponse(404,
			"not_found",
			"unknown graph; expected one of: download, upload, connections, kad");
	}

	// Lazy-fetch the full 4-series graph bundle (one EC call serves
	// all 4 named graphs, so the cache shares across concurrent
	// requests for different graph names).
	auto pair = m_stats_graphs_cache.GetOrFetch(
		std::chrono::milliseconds(1000), [this]() -> TtlPair_StatsGraphs {
			std::unique_ptr<CECPacket> req_ec(new CECPacket(EC_OP_GET_STATSGRAPHS));
			req_ec->AddTag(CECTag(EC_TAG_STATSGRAPH_SCALE, static_cast<std::uint16_t>(1)));
			req_ec->AddTag(CECTag(EC_TAG_STATSGRAPH_WIDTH, static_cast<std::uint16_t>(1800)));
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
		return ErrorResponse(503,
			"ec_unavailable",
			"EC fetch failed for stats graphs; amuled may be disconnected");
	}

	const webapi::StatsGraphs &g = pair.first;
	const std::vector<std::uint32_t> *series = nullptr;
	if (graph == "download") {
		series = &g.download_bps;
	} else if (graph == "upload") {
		series = &g.upload_bps;
	} else if (graph == "connections") {
		series = &g.connections;
	} else /* kad */ {
		series = &g.kad_nodes;
	}

	// ?width=N — clamp the sample count returned. 0 / absent means
	// "everything we have" (up to the 1800-sample window we ask for).
	std::string query;
	const std::size_t q = req.target.find('?');
	if (q != std::string::npos)
		query = req.target.substr(q + 1);
	std::size_t width = 0;
	{
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("width");
		if (it != qmap.end()) {
			const long n = std::atol(it->second.c_str());
			if (n > 0)
				width = static_cast<std::size_t>(std::min<long>(n, 1800));
		}
	}

	const std::time_t ts = pair.second;
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("graph");
	w.ValueString(wxString::FromUTF8(graph.c_str()));
	w.Key("unit");
	w.ValueString(wxString::FromUTF8(unit));
	w.Key("interval_seconds");
	w.ValueInt(static_cast<int64_t>(g.interval_seconds));
	// snapshot_at retired from the response. WritePointArray
	// still consumes `ts` to compute per-point timestamps (anchoring
	// the time-series backwards from the fetch wall-clock).
	w.Key("points");
	WritePointArray(w, *series, ts, g.interval_seconds, width);
	// Session totals tag along — clients showing "this session: X GB
	// down" don't need a separate roundtrip.
	w.Key("session");
	w.BeginObject();
	w.Key("download_bytes");
	w.ValueInt(static_cast<int64_t>(g.session_download_bytes));
	w.Key("upload_bytes");
	w.ValueInt(static_cast<int64_t>(g.session_upload_bytes));
	w.Key("kad_bytes");
	w.ValueInt(static_cast<int64_t>(g.session_kad_bytes));
	w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSearchResults(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

	// Read straight from the refresher-maintained state. POST /search
	// flips the active flag; RefresherTick polls amuled while active and
	// reads the daemon's unambiguous EC_TAG_SEARCH_LIFECYCLE_* tags
	// (state + unified percent) — see RefresherTick.cpp + SearchList.cpp.
	// The state stores the normalized (kind, percent, complete, active);
	// no further interpretation here. The `progress` object carries the
	// same state/kind/percent as the `search_progress` SSE event (the
	// event additionally ships a results count, since it has no results
	// array beside it).
	const std::vector<webapi::SearchResult> results_vec = m_state.Search();
	const webapi::SearchProgressSnapshot progress = m_state.SearchProgress();

	// #357 pagination/sort. This endpoint keeps its own envelope (the
	// `progress` object rides alongside `results`), so it can't call
	// ListResponse, but it shares the window + page-meta helpers.
	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	static const ListComparators<webapi::SearchResult> kComps = {
		{ "name",
			[](const webapi::SearchResult &a, const webapi::SearchResult &b) {
				return a.name < b.name;
			} },
		{ "size",
			[](const webapi::SearchResult &a, const webapi::SearchResult &b) {
				return a.size < b.size;
			} },
		{ "sources",
			[](const webapi::SearchResult &a, const webapi::SearchResult &b) {
				return a.source_count < b.source_count;
			} },
		{ "rating",
			[](const webapi::SearchResult &a, const webapi::SearchResult &b) {
				return a.rating < b.rating;
			} },
	};
	std::vector<const webapi::SearchResult *> window;
	std::size_t total = 0;
	if (auto err = BuildListWindow(results_vec, params, kComps, window, total))
		return *err;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("results");
	w.BeginArray();
	for (const webapi::SearchResult *item : window)
		WriteSearchObject(w, *item);
	w.EndArray();
	WritePageMeta(w, total, params, window.size());
	// Mirrors the `search_progress` SSE event field-for-field. `state`
	// is canonical and encodes the full lifecycle (running / finished /
	// idle), so we don't also emit redundant `active` / `complete`
	// booleans — consumers derive them from `state` and read the same
	// shape whether they poll here or subscribe to the stream.
	w.Key("progress");
	w.BeginObject();
	w.Key("state");
	w.ValueString(wxString::FromAscii(progress.complete ? "finished"
					  : progress.active ? "running"
							    : "idle"));
	w.Key("kind");
	w.ValueString(wxString::FromUTF8(progress.kind.c_str()));
	w.Key("percent");
	w.ValueInt(static_cast<int64_t>(progress.percent));
	w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleLogAmule(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

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
	r.status = 200;
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
	w.Key("total_cached");
	w.ValueInt(static_cast<int64_t>(all.size()));
	w.Key("returned");
	w.ValueInt(static_cast<int64_t>(sliced.size()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleLogAmuleReset(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_RESET_LOG));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed");
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
	r.status = 204;
	r.content_type.clear();
	return r;
}

CHttpServer::Response CApiDispatcher::HandleLogServerinfo(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;

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
		std::chrono::milliseconds(1000), [this]() -> TtlPair_ServerInfo {
			std::unique_ptr<CECPacket> req_ec(new CECPacket(EC_OP_GET_SERVERINFO));
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
		return ErrorResponse(
			503, "ec_unavailable", "EC fetch failed for server info; amuled may be disconnected");
	}

	const webapi::ServerInfoLog &log = pair.first;
	const std::string text = TailString(log.text, tail);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("text");
	w.ValueString(wxString::FromUTF8(text.c_str()));
	// The total length lets a client decide whether to re-poll
	// with a smaller `?tail=` for incremental display.
	w.Key("total_bytes");
	w.ValueInt(static_cast<int64_t>(log.text.size()));
	w.Key("returned_bytes");
	w.ValueInt(static_cast<int64_t>(text.size()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleLogServerinfoReset(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_CLEAR_SERVERINFO));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed");
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
	r.status = 204;
	r.content_type.clear();
	return r;
}

CHttpServer::Response CApiDispatcher::HandlePreferences(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	const webapi::PreferencesSnapshot p = m_state.Preferences();
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("general");
	w.BeginObject();
	w.Key("nickname");
	w.ValueString(wxString::FromUTF8(p.nickname.c_str()));
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(p.user_hash.c_str()));
	w.Key("host_name");
	w.ValueString(wxString::FromUTF8(p.host_name.c_str()));
	w.Key("check_new_version");
	w.ValueBool(p.check_new_version);
	w.EndObject();
	w.Key("connection");
	w.BeginObject();
	w.Key("max_upload_kbps");
	w.ValueInt(static_cast<int64_t>(p.max_upload_kbps));
	w.Key("max_download_kbps");
	w.ValueInt(static_cast<int64_t>(p.max_download_kbps));
	w.Key("max_upload_cap_kbps");
	w.ValueInt(static_cast<int64_t>(p.max_upload_cap_kbps));
	w.Key("max_download_cap_kbps");
	w.ValueInt(static_cast<int64_t>(p.max_download_cap_kbps));
	w.Key("slot_allocation");
	w.ValueInt(static_cast<int64_t>(p.slot_allocation));
	w.Key("tcp_port");
	w.ValueInt(static_cast<int64_t>(p.tcp_port));
	w.Key("udp_port");
	w.ValueInt(static_cast<int64_t>(p.udp_port));
	w.Key("udp_disabled");
	w.ValueBool(p.udp_disabled);
	w.Key("max_sources_per_file");
	w.ValueInt(static_cast<int64_t>(p.max_sources_per_file));
	w.Key("max_connections");
	w.ValueInt(static_cast<int64_t>(p.max_connections));
	w.Key("autoconnect");
	w.ValueBool(p.autoconnect);
	w.Key("reconnect");
	w.ValueBool(p.reconnect);
	w.Key("network_ed2k");
	w.ValueBool(p.network_ed2k);
	w.Key("network_kad");
	w.ValueBool(p.network_kad);
	w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// Helpers that pull (& validate) optional fields from a JSON object.
// Each returns true and writes `out` if the field is present and the
// right shape; returns false on absence. On wrong shape, writes
// `err_label` for the caller to relay to the client and returns false
// as well (errors take priority via the err_label out-param).
struct PrefsParseError
{
	bool is_error = false;
	std::string message;
};

// EC_OP_SET_PREFERENCES requires EC_DETAIL_FULL so the daemon honors
// boolean tags (CEC_Prefs_Packet::Apply checks
// `use_tag = (GetDetailLevel() == EC_DETAIL_FULL)` before calling
// ApplyBoolean). FULL is also what amulegui sends.
} // namespace

CHttpServer::Response CApiDispatcher::HandlePreferencesPatch(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	// Body shape: { "general": {...}, "connection": {...} } — both
	// sub-objects optional, all fields within optional. Mirrors the
	// /preferences GET shape so a typical client read-
	// modify-write workflow doesn't have to translate between schemas.
	const picojson::object *general_obj = nullptr;
	const picojson::object *connection_obj = nullptr;
	{
		const auto it = obj.find("general");
		if (it != obj.end()) {
			if (!it->second.is<picojson::object>()) {
				return ErrorResponse(400, "bad_request", "`general` must be an object");
			}
			general_obj = &it->second.get<picojson::object>();
		}
	}
	{
		const auto it = obj.find("connection");
		if (it != obj.end()) {
			if (!it->second.is<picojson::object>()) {
				return ErrorResponse(400, "bad_request", "`connection` must be an object");
			}
			connection_obj = &it->second.get<picojson::object>();
		}
	}

	if (general_obj == nullptr && connection_obj == nullptr) {
		return ErrorResponse(400,
			"bad_request",
			"request body must include at least one of `general` or "
			"`connection` sub-objects");
	}

	// Build the SET_PREFERENCES packet at EC_DETAIL_FULL (required for
	// boolean fields — Apply() gates ApplyBoolean on detail==FULL).
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SET_PREFERENCES, EC_DETAIL_FULL));

	auto add_uint = [](CECTag &group, ec_tagname_t name, std::uint32_t v) {
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
					return ErrorResponse(
						400, "bad_request", "`general.nickname` must be a string");
				}
				const std::string &v = it->second.get<std::string>();
				general.AddTag(CECTag(EC_TAG_USER_NICK, wxString::FromUTF8(v.c_str())));
				any_general = true;
			}
		}
		{
			const auto it = general_obj->find("check_new_version");
			if (it != general_obj->end()) {
				if (!it->second.is<bool>()) {
					return ErrorResponse(400,
						"bad_request",
						"`general.check_new_version` must be a bool");
				}
				add_bool(general, EC_TAG_GENERAL_CHECK_NEW_VERSION, it->second.get<bool>());
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
		auto take_uint =
			[&](const char *key, ec_tagname_t name, std::uint32_t max) -> CHttpServer::Response {
			const auto it = connection_obj->find(key);
			if (it == connection_obj->end()) {
				CHttpServer::Response ok;
				ok.status = 0; // sentinel: not present
				return ok;
			}
			if (!it->second.is<double>()) {
				return ErrorResponse(400,
					"bad_request",
					"connection field must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > max) {
				return ErrorResponse(400, "bad_request", "connection field out of range");
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
				return ErrorResponse(400, "bad_request", "connection field must be a bool");
			}
			add_bool(connection, name, it->second.get<bool>());
			any_conn = true;
			CHttpServer::Response ok;
			ok.status = 200;
			return ok;
		};

		// Uints — kbps caps in [0, 1_000_000_000], ports in [0, 65535].
		const std::uint32_t kbps_max = 1000000000u;
		auto r1 = take_uint("max_upload_kbps", EC_TAG_CONN_MAX_UL, kbps_max);
		if (r1.status >= 400)
			return r1;
		auto r2 = take_uint("max_download_kbps", EC_TAG_CONN_MAX_DL, kbps_max);
		if (r2.status >= 400)
			return r2;
		auto r3 = take_uint("max_upload_cap_kbps", EC_TAG_CONN_UL_CAP, kbps_max);
		if (r3.status >= 400)
			return r3;
		auto r4 = take_uint("max_download_cap_kbps", EC_TAG_CONN_DL_CAP, kbps_max);
		if (r4.status >= 400)
			return r4;
		auto r5 = take_uint("slot_allocation", EC_TAG_CONN_SLOT_ALLOCATION, 65535);
		if (r5.status >= 400)
			return r5;
		auto r6 = take_uint("tcp_port", EC_TAG_CONN_TCP_PORT, 65535);
		if (r6.status >= 400)
			return r6;
		auto r7 = take_uint("udp_port", EC_TAG_CONN_UDP_PORT, 65535);
		if (r7.status >= 400)
			return r7;
		auto r8 = take_uint("max_sources_per_file", EC_TAG_CONN_MAX_FILE_SOURCES, 65535);
		if (r8.status >= 400)
			return r8;
		auto r9 = take_uint("max_connections", EC_TAG_CONN_MAX_CONN, 65535);
		if (r9.status >= 400)
			return r9;

		// Bools.
		auto rb1 = take_bool("udp_disabled", EC_TAG_CONN_UDP_DISABLE);
		if (rb1.status >= 400)
			return rb1;
		auto rb2 = take_bool("autoconnect", EC_TAG_CONN_AUTOCONNECT);
		if (rb2.status >= 400)
			return rb2;
		auto rb3 = take_bool("reconnect", EC_TAG_CONN_RECONNECT);
		if (rb3.status >= 400)
			return rb3;
		auto rb4 = take_bool("network_ed2k", EC_TAG_NETWORK_ED2K);
		if (rb4.status >= 400)
			return rb4;
		auto rb5 = take_bool("network_kad", EC_TAG_NETWORK_KADEMLIA);
		if (rb5.status >= 400)
			return rb5;

		if (any_conn) {
			ec_req->AddTag(connection);
			any_change = true;
		}
	}

	if (!any_change) {
		return ErrorResponse(
			400, "bad_request", "request body did not include any known pref fields");
	}

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SET_PREFERENCES");
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
	(void)RefresherTick(m_app, m_state);

	// Return the updated /preferences shape so consumers can confirm
	// what landed without a follow-up GET.
	const webapi::PreferencesSnapshot p = m_state.Preferences();
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("general");
	w.BeginObject();
	w.Key("nickname");
	w.ValueString(wxString::FromUTF8(p.nickname.c_str()));
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(p.user_hash.c_str()));
	w.Key("host_name");
	w.ValueString(wxString::FromUTF8(p.host_name.c_str()));
	w.Key("check_new_version");
	w.ValueBool(p.check_new_version);
	w.EndObject();
	w.Key("connection");
	w.BeginObject();
	w.Key("max_upload_kbps");
	w.ValueInt(static_cast<int64_t>(p.max_upload_kbps));
	w.Key("max_download_kbps");
	w.ValueInt(static_cast<int64_t>(p.max_download_kbps));
	w.Key("max_upload_cap_kbps");
	w.ValueInt(static_cast<int64_t>(p.max_upload_cap_kbps));
	w.Key("max_download_cap_kbps");
	w.ValueInt(static_cast<int64_t>(p.max_download_cap_kbps));
	w.Key("slot_allocation");
	w.ValueInt(static_cast<int64_t>(p.slot_allocation));
	w.Key("tcp_port");
	w.ValueInt(static_cast<int64_t>(p.tcp_port));
	w.Key("udp_port");
	w.ValueInt(static_cast<int64_t>(p.udp_port));
	w.Key("udp_disabled");
	w.ValueBool(p.udp_disabled);
	w.Key("max_sources_per_file");
	w.ValueInt(static_cast<int64_t>(p.max_sources_per_file));
	w.Key("max_connections");
	w.ValueInt(static_cast<int64_t>(p.max_connections));
	w.Key("autoconnect");
	w.ValueBool(p.autoconnect);
	w.Key("reconnect");
	w.ValueBool(p.reconnect);
	w.Key("network_ed2k");
	w.ValueBool(p.network_ed2k);
	w.Key("network_kad");
	w.ValueBool(p.network_kad);
	w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// Issue a single-shot mutation EC packet (no body), check the
// response, run RefresherTick inline, return a standard
// `{ok: true, message?: "..."}` response. Used by every connection-
// control endpoint where the EC op is parameterless.
CHttpServer::Response SimpleConnControlOp(
	CamuleapiApp &app, webapi::CState &state, ec_opcode_t op, unsigned http_status)
{
	std::unique_ptr<CECPacket> ec_req(new CECPacket(op));
	const CECPacket *ec_resp = app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed");
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
		for (CECPacket::const_iterator it = ec_resp->begin(); it != ec_resp->end(); ++it) {
			const CECTag *t = &*it;
			if (t->GetTagName() == EC_TAG_STRING) {
				if (!message.empty())
					message += "; ";
				message += std::string(t->GetStringData().utf8_str());
			}
		}
	}
	delete ec_resp;

	(void)RefresherTick(app, state);

	CHttpServer::Response r;
	r.status = http_status;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	if (!message.empty()) {
		w.Key("message");
		w.ValueString(wxString::FromUTF8(message.c_str()));
	}
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleNetworksConnect(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// Optional `{"network": "ed2k" | "kad" | "both"}` selector — same
	// shape as /networks/disconnect. Default "both" preserves the
	// original parameterless contract (every connector-aware client
	// kept working when this body was added).
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
				return ErrorResponse(400,
					"bad_request",
					"`network` must be one of \"ed2k\", \"kad\", \"both\"");
			}
			network = it->second.get<std::string>();
			if (network != "ed2k" && network != "kad" && network != "both") {
				return ErrorResponse(400,
					"bad_request",
					"`network` must be one of \"ed2k\", \"kad\", \"both\"");
			}
		}
	}

	if (network == "ed2k") {
		return SimpleConnControlOp(m_app, m_state, EC_OP_SERVER_CONNECT, 202);
	}
	if (network == "kad") {
		return SimpleConnControlOp(m_app, m_state, EC_OP_KAD_START, 202);
	}
	return SimpleConnControlOp(m_app, m_state, EC_OP_CONNECT, 202);
}

CHttpServer::Response CApiDispatcher::HandleNetworksDisconnect(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

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
				return ErrorResponse(400,
					"bad_request",
					"`network` must be one of \"ed2k\", \"kad\", \"both\"");
			}
			network = it->second.get<std::string>();
			if (network != "ed2k" && network != "kad" && network != "both") {
				return ErrorResponse(400,
					"bad_request",
					"`network` must be one of \"ed2k\", \"kad\", \"both\"");
			}
		}
	}

	if (network == "ed2k") {
		return SimpleConnControlOp(m_app, m_state, EC_OP_SERVER_DISCONNECT, 200);
	}
	if (network == "kad") {
		return SimpleConnControlOp(m_app, m_state, EC_OP_KAD_STOP, 200);
	}
	// "both": amuled's EC_OP_DISCONNECT short-circuits to both
	// SERVER_DISCONNECT and KAD_STOP in one EC roundtrip.
	return SimpleConnControlOp(m_app, m_state, EC_OP_DISCONNECT, 200);
}

// HandleKadConnect / HandleKadDisconnect were removed — strict
// aliases of HandleNetworksConnect / HandleNetworksDisconnect with
// `{"network":"kad"}`. The Kad bootstrap handler below is genuinely
// distinct (single-contact bootstrap from an explicit IP+port) and
// stays.

CHttpServer::Response CApiDispatcher::HandleKadBootstrap(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

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
			return ErrorResponse(400, "bad_request", "required field `ip` is missing");
		}
		if (it->second.is<std::string>()) {
			// Dotted-quad string. Parse with strtoul on each octet.
			const std::string &s = it->second.get<std::string>();
			unsigned a_, b_, c_, d_;
			if (std::sscanf(s.c_str(), "%u.%u.%u.%u", &a_, &b_, &c_, &d_) != 4 || a_ > 255 ||
				b_ > 255 || c_ > 255 || d_ > 255) {
				return ErrorResponse(400,
					"bad_request",
					"`ip` must be a dotted-quad IPv4 address or a "
					"host-order uint32");
			}
			ip_he = (a_) | (b_ << 8) | (c_ << 16) | (d_ << 24);
		} else if (it->second.is<double>()) {
			const double v = it->second.get<double>();
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request", "`ip` uint32 out of range");
			}
			ip_he = static_cast<std::uint32_t>(v);
		} else {
			return ErrorResponse(400, "bad_request", "`ip` must be a string or number");
		}
	}
	std::uint16_t port = 0;
	{
		const auto it = obj.find("port");
		if (it == obj.end() || !it->second.is<double>()) {
			return ErrorResponse(400, "bad_request", "required numeric field `port` is missing");
		}
		const double v = it->second.get<double>();
		if (v < 0 || v > 65535) {
			return ErrorResponse(400, "bad_request", "`port` must be in [0, 65535]");
		}
		port = static_cast<std::uint16_t>(v);
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_KAD_BOOTSTRAP_FROM_IP));
	ec_req->AddTag(CECTag(EC_TAG_BOOTSTRAP_IP, ip_he));
	ec_req->AddTag(CECTag(EC_TAG_BOOTSTRAP_PORT, port));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for KAD_BOOTSTRAP_FROM_IP");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("ip");
	w.ValueInt(static_cast<int64_t>(ip_he));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(port));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// Inverse of SharedPriorityName in Refresher.cpp. Wire form mirrors
// the /shared[].priority enum: bare upload priorities plus "auto".
// Setting "auto" hands level selection to amuled (it derives the level
// from the upload queue and reports it back as `priority` + a true
// `priority_auto`); to pin a fixed level, send the bare name. The
// combined "*_auto" strings are intentionally NOT accepted as input --
// "auto" is the level the daemon computes, so a caller can't pin it.
// Returns false on unknown enum.
bool SharedPriorityToCode(const std::string &name, std::uint8_t &out)
{
	if (name == "very_low") {
		out = PR_VERY_LOW;
		return true;
	} else if (name == "low") {
		out = PR_LOW;
		return true;
	} else if (name == "normal") {
		out = PR_NORMAL;
		return true;
	} else if (name == "high") {
		out = PR_HIGH;
		return true;
	} else if (name == "release") {
		out = PR_VERYHIGH;
		return true;
	} else if (name == "auto") {
		out = PR_AUTO;
		return true;
	}
	return false;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleSharedPatch(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	webapi::FileSnapshot s;
	std::string needle = key;
	std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
		return std::tolower(c);
	});
	if (!m_state.FindShared(needle, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	const auto pit = obj.find("priority");
	if (pit == obj.end()) {
		return ErrorResponse(400, "bad_request", "request body must include `priority`");
	}
	if (!pit->second.is<std::string>()) {
		return ErrorResponse(400, "bad_request", "`priority` must be a wire-string enum");
	}
	std::uint8_t code = 0;
	if (!SharedPriorityToCode(pit->second.get<std::string>(), code)) {
		return ErrorResponse(400,
			"bad_request",
			"`priority` must be one of "
			"very_low, low, normal, high, release, auto");
	}

	CMD4Hash file_hash;
	if (!HashFromHex(s.hash, file_hash)) {
		return ErrorResponse(500, "internal_error", "failed to decode file hash");
	}
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SHARED_SET_PRIO));
	CECTag hash_tag(EC_TAG_PARTFILE, file_hash);
	hash_tag.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, code));
	ec_req->AddTag(hash_tag);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SHARED_SET_PRIO");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void)RefresherTick(m_app, m_state);

	// Re-read post-mutation. Fall back to prior copy if evicted.
	webapi::FileSnapshot s_after = s;
	(void)m_state.FindShared(s.hash, s_after);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteSharedObject(w, s_after);
	FinalizeJsonBody(w, r);
	return r;
}

// --- Bulk mutations (issue #358) -------------------------------------
// PATCH/DELETE /downloads and PATCH /shared take a `hashes` array and
// apply the same op to each, reporting per-item outcomes under `results`
// (see BulkResultsResponse). Best-effort per item -- each hash is an
// independent EC roundtrip, so a mid-batch failure doesn't abort the
// rest. One RefresherTick runs after the whole batch. All-ok is 200
// (the mutations complete synchronously, unlike the async POST /downloads
// add which is 202); a mix is 207 Multi-Status; an all-unreachable batch
// collapses to 503.

CHttpServer::Response CApiDispatcher::HandleDownloadsBulkPatch(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err))
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	const auto &obj = root.get<picojson::object>();

	std::vector<std::string> hashes;
	CHttpServer::Response bad;
	if (!ParseBulkHashes(obj, hashes, bad))
		return bad;

	// Validate the patch ONCE -- the same op list applies to every hash, so
	// a malformed patch is a 400 for the whole request; per-hash problems
	// (not found, amuled rejection) surface per item. Fixed order
	// (status, priority, category) keeps the wire effect deterministic.
	struct PatchOp
	{
		ec_opcode_t op;
		bool has_inner;
		ec_tagname_t inner_name;
		std::uint8_t inner_value;
	};
	std::vector<PatchOp> ops;
	{
		const auto it = obj.find("status");
		if (it != obj.end()) {
			if (!it->second.is<std::string>())
				return ErrorResponse(400,
					"bad_request",
					"`status` must be one of \"paused\" or \"resumed\"");
			const std::string &v = it->second.get<std::string>();
			if (v == "paused")
				ops.push_back(
					{ EC_OP_PARTFILE_PAUSE, false, static_cast<ec_tagname_t>(0), 0 });
			else if (v == "resumed")
				ops.push_back(
					{ EC_OP_PARTFILE_RESUME, false, static_cast<ec_tagname_t>(0), 0 });
			else
				return ErrorResponse(400,
					"bad_request",
					"`status` must be one of \"paused\" or \"resumed\"");
		}
	}
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>())
				return ErrorResponse(
					400, "bad_request", "`priority` must be a wire-string enum");
			std::uint8_t code = 0;
			if (!DownloadPriorityToCode(it->second.get<std::string>(), code))
				return ErrorResponse(400,
					"bad_request",
					"`priority` must be one of low, normal, high, release, auto");
			ops.push_back({ EC_OP_PARTFILE_PRIO_SET, true, EC_TAG_PARTFILE_PRIO, code });
		}
	}
	{
		const auto it = obj.find("category");
		if (it != obj.end()) {
			if (!it->second.is<double>())
				return ErrorResponse(
					400, "bad_request", "`category` must be a non-negative integer");
			const double v = it->second.get<double>();
			if (v < 0 || v > 255)
				return ErrorResponse(400, "bad_request", "`category` must be in [0, 255]");
			ops.push_back({ EC_OP_PARTFILE_SET_CAT,
				true,
				EC_TAG_PARTFILE_CAT,
				static_cast<std::uint8_t>(v) });
		}
	}
	if (ops.empty())
		return ErrorResponse(400,
			"bad_request",
			"request body must include at least one of `status`, `priority`, or `category`");

	std::vector<BulkItem> results;
	results.reserve(hashes.size());
	for (const std::string &raw : hashes) {
		std::string needle = raw;
		std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		webapi::FileSnapshot d;
		if (!m_state.FindDownload(needle, d)) {
			results.push_back(BulkErr(raw, 404, "not_found", "no download with that hash"));
			continue;
		}
		CMD4Hash file_hash;
		if (!HashFromHex(d.hash, file_hash)) {
			results.push_back(
				BulkErr(raw, 500, "internal_error", "failed to decode partfile hash"));
			continue;
		}
		bool item_ok = true;
		for (const PatchOp &pop : ops) {
			std::unique_ptr<CECPacket> p(new CECPacket(pop.op));
			CECTag hash_tag(EC_TAG_PARTFILE, file_hash);
			if (pop.has_inner)
				hash_tag.AddTag(CECTag(pop.inner_name, pop.inner_value));
			p->AddTag(hash_tag);
			const CECPacket *ec_resp = m_app.SendRecvSerialized(p.get());
			if (!ec_resp) {
				results.push_back(BulkErr(raw, 503, "ec_unavailable", "EC roundtrip failed"));
				item_ok = false;
				break;
			}
			std::string ec_err;
			if (IsEcFailedResponse(ec_resp, ec_err)) {
				delete ec_resp;
				results.push_back(BulkErr(raw, 400, "amuled_rejected", ec_err));
				item_ok = false;
				break;
			}
			delete ec_resp;
		}
		if (item_ok)
			results.push_back(BulkOk(raw));
	}
	(void)RefresherTick(m_app, m_state);
	return BulkResultsResponse(results, 200);
}

CHttpServer::Response CApiDispatcher::HandleDownloadsBulkDelete(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err))
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	const auto &obj = root.get<picojson::object>();

	std::vector<std::string> hashes;
	CHttpServer::Response bad;
	if (!ParseBulkHashes(obj, hashes, bad))
		return bad;

	std::vector<BulkItem> results;
	results.reserve(hashes.size());
	for (const std::string &raw : hashes) {
		std::string needle = raw;
		std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		webapi::FileSnapshot d;
		if (!m_state.FindDownload(needle, d)) {
			results.push_back(BulkErr(raw, 404, "not_found", "no download with that hash"));
			continue;
		}
		// Same guard as the single-item DELETE: completed entries are not
		// removable here (use POST /downloads/clear_completed).
		if (d.download.status == "completed") {
			results.push_back(BulkErr(raw,
				409,
				"completed_use_clear_completed",
				"DELETE only removes active downloads; use POST "
				"/downloads/clear_completed to clear a completed entry"));
			continue;
		}
		CMD4Hash file_hash;
		if (!HashFromHex(d.hash, file_hash)) {
			results.push_back(
				BulkErr(raw, 500, "internal_error", "failed to decode partfile hash"));
			continue;
		}
		std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_PARTFILE_DELETE));
		ec_req->AddTag(CECTag(EC_TAG_PARTFILE, file_hash));
		const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
		if (!ec_resp) {
			results.push_back(
				BulkErr(raw, 503, "ec_unavailable", "EC roundtrip failed for DELETE"));
			continue;
		}
		std::string ec_err;
		if (IsEcFailedResponse(ec_resp, ec_err)) {
			delete ec_resp;
			results.push_back(BulkErr(raw, 400, "amuled_rejected", ec_err));
			continue;
		}
		delete ec_resp;
		results.push_back(BulkOk(raw));
	}
	(void)RefresherTick(m_app, m_state);
	return BulkResultsResponse(results, 200);
}

CHttpServer::Response CApiDispatcher::HandleSharedBulkPatch(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err))
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	const auto &obj = root.get<picojson::object>();

	std::vector<std::string> hashes;
	CHttpServer::Response bad;
	if (!ParseBulkHashes(obj, hashes, bad))
		return bad;

	// `priority` required + validated once for the whole batch.
	const auto pit = obj.find("priority");
	if (pit == obj.end())
		return ErrorResponse(400, "bad_request", "request body must include `priority`");
	if (!pit->second.is<std::string>())
		return ErrorResponse(400, "bad_request", "`priority` must be a wire-string enum");
	std::uint8_t code = 0;
	if (!SharedPriorityToCode(pit->second.get<std::string>(), code))
		return ErrorResponse(400,
			"bad_request",
			"`priority` must be one of very_low, low, normal, high, release, auto");

	std::vector<BulkItem> results;
	results.reserve(hashes.size());
	for (const std::string &raw : hashes) {
		std::string needle = raw;
		std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		webapi::FileSnapshot s;
		if (!m_state.FindShared(needle, s)) {
			results.push_back(BulkErr(raw, 404, "not_found", "no shared file with that hash"));
			continue;
		}
		CMD4Hash file_hash;
		if (!HashFromHex(s.hash, file_hash)) {
			results.push_back(BulkErr(raw, 500, "internal_error", "failed to decode file hash"));
			continue;
		}
		std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SHARED_SET_PRIO));
		CECTag hash_tag(EC_TAG_PARTFILE, file_hash);
		hash_tag.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, code));
		ec_req->AddTag(hash_tag);
		const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
		if (!ec_resp) {
			results.push_back(BulkErr(
				raw, 503, "ec_unavailable", "EC roundtrip failed for SHARED_SET_PRIO"));
			continue;
		}
		std::string ec_err;
		if (IsEcFailedResponse(ec_resp, ec_err)) {
			delete ec_resp;
			results.push_back(BulkErr(raw, 400, "amuled_rejected", ec_err));
			continue;
		}
		delete ec_resp;
		results.push_back(BulkOk(raw));
	}
	(void)RefresherTick(m_app, m_state);
	return BulkResultsResponse(results, 200);
}

CHttpServer::Response CApiDispatcher::HandleSharedReload(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	// EC_OP_SHAREDFILES_RELOAD: amuled re-walks every configured share
	// root and re-publishes the contents. Synchronous on amuled's side
	// but bounded by I/O over the share tree — typical small libraries
	// complete in well under a second. Inline RefresherTick re-pulls
	// the shared-files cache so SSE subscribers see `shared_added` /
	// `_removed` events for the delta before the response lands.
	return SimpleConnControlOp(m_app, m_state, EC_OP_SHAREDFILES_RELOAD, 202);
}

namespace
{

// Parse a uint8 index from a URL capture. Categories are 0..255 (the
// EC tag stores them as uint8). Returns false on overflow, negative,
// or non-digit content.
bool ParseCategoryIndex(const std::string &s, std::uint8_t &out)
{
	if (s.empty())
		return false;
	char *end = nullptr;
	const unsigned long v = std::strtoul(s.c_str(), &end, 10);
	if (end == s.c_str() || *end != '\0')
		return false;
	if (v > 255)
		return false;
	out = static_cast<std::uint8_t>(v);
	return true;
}

// Inverse of CategoryPriorityName (Refresher.cpp's ParseCategoryTag).
// Categories use the SAME priority enum as downloads, including the
// `+10` auto-flag offset. Maps wire strings to PR_* codes.
bool CategoryPriorityToCode(const std::string &name, std::uint8_t &out)
{
	if (name == "very_low") {
		out = PR_VERY_LOW;
		return true;
	} else if (name == "low") {
		out = PR_LOW;
		return true;
	} else if (name == "normal") {
		out = PR_NORMAL;
		return true;
	} else if (name == "high") {
		out = PR_HIGH;
		return true;
	} else if (name == "release") {
		out = PR_VERYHIGH;
		return true;
	} else if (name == "auto") {
		out = PR_AUTO;
		return true;
	}
	return false;
}

// Build the CEC_Category_Tag-shaped tag amuled expects. The shape is:
//  parent tag EC_TAG_CATEGORY with the index as the int payload,
//  nested children:
//    EC_TAG_CATEGORY_TITLE   (string, "name" in our API)
//    EC_TAG_CATEGORY_PATH    (string, "path")
//    EC_TAG_CATEGORY_COMMENT (string, "comment")
//    EC_TAG_CATEGORY_COLOR   (uint32)
//    EC_TAG_CATEGORY_PRIO    (uint8)
//
// For CREATE the index is `0xFFFFFFFF` (sentinel: amuled assigns the
// next free slot). For UPDATE we pass the actual index. For DELETE
// the tag is just `(EC_TAG_CATEGORY, index)` — no children needed.
CECTag BuildCategoryTag(std::uint32_t index,
	const std::string &name,
	const std::string &path,
	const std::string &comment,
	std::uint32_t color,
	std::uint8_t prio)
{
	CECTag t(EC_TAG_CATEGORY, index);
	t.AddTag(CECTag(EC_TAG_CATEGORY_TITLE, wxString::FromUTF8(name.c_str())));
	t.AddTag(CECTag(EC_TAG_CATEGORY_PATH, wxString::FromUTF8(path.c_str())));
	t.AddTag(CECTag(EC_TAG_CATEGORY_COMMENT, wxString::FromUTF8(comment.c_str())));
	t.AddTag(CECTag(EC_TAG_CATEGORY_COLOR, color));
	t.AddTag(CECTag(EC_TAG_CATEGORY_PRIO, prio));
	return t;
}

// Helper to extract optional name/path/comment/color/priority from a
// JSON object. Populates the out-params; returns an error response
// on shape violations. The `is_create` flag enables required-field
// enforcement: CREATE needs a name, UPDATE/PATCH treat all as
// optional.
struct CategoryFields
{
	std::string name;
	std::string path;
	std::string comment;
	std::uint32_t color = 0;
	std::uint8_t prio = PR_NORMAL;
	bool has_name = false;
	bool has_path = false;
	bool has_comment = false;
	bool has_color = false;
	bool has_prio = false;
};

CHttpServer::Response ParseCategoryFields(const picojson::object &obj, CategoryFields &out)
{
	auto get_string = [&obj](const char *key, std::string &dst, bool &has) -> CHttpServer::Response {
		const auto it = obj.find(key);
		if (it == obj.end()) {
			CHttpServer::Response ok;
			ok.status = 0;
			return ok;
		}
		if (!it->second.is<std::string>()) {
			return ErrorResponse(400, "bad_request", "category field must be a string");
		}
		dst = it->second.get<std::string>();
		has = true;
		CHttpServer::Response ok;
		ok.status = 200;
		return ok;
	};

	auto r1 = get_string("name", out.name, out.has_name);
	if (r1.status >= 400)
		return r1;
	auto r2 = get_string("path", out.path, out.has_path);
	if (r2.status >= 400)
		return r2;
	auto r3 = get_string("comment", out.comment, out.has_comment);
	if (r3.status >= 400)
		return r3;
	{
		const auto it = obj.find("color");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400, "bad_request", "`color` must be a uint32");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request", "`color` out of range");
			}
			out.color = static_cast<std::uint32_t>(v);
			out.has_color = true;
		}
	}
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(
					400, "bad_request", "`priority` must be a wire-string enum");
			}
			if (!CategoryPriorityToCode(it->second.get<std::string>(), out.prio)) {
				return ErrorResponse(400,
					"bad_request",
					"`priority` must be one of low, normal, high, "
					"release, very_low, auto");
			}
			out.has_prio = true;
		}
	}
	CHttpServer::Response ok;
	ok.status = 200;
	return ok;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleCategoryCreate(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	CategoryFields f;
	auto err = ParseCategoryFields(obj, f);
	if (err.status >= 400)
		return err;
	if (!f.has_name || f.name.empty()) {
		return ErrorResponse(400, "bad_request", "required string field `name` is missing");
	}

	// CREATE: index sentinel is 0xFFFFFFFF — amuled assigns the next
	// free slot and returns NOOP on success.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_CREATE_CATEGORY));
	ec_req->AddTag(BuildCategoryTag(0xFFFFFFFFu, f.name, f.path, f.comment, f.color, f.prio));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for CREATE_CATEGORY");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void)RefresherTick(m_app, m_state);

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
	r.status = 201;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("name");
	w.ValueString(wxString::FromUTF8(f.name.c_str()));
	if (created_index >= 0) {
		w.Key("index");
		w.ValueInt(static_cast<int64_t>(created_index));
	}
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleCategoryUpdate(
	const CHttpServer::Request &req, const std::string &index_str)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint8_t idx = 0;
	if (!ParseCategoryIndex(index_str, idx)) {
		return ErrorResponse(400, "bad_request", "path `{index}` must be a uint8 in [0, 255]");
	}
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
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
		return ErrorResponse(404, "not_found", "no category with that index");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	CategoryFields f;
	auto err = ParseCategoryFields(obj, f);
	if (err.status >= 400)
		return err;

	const std::string name = f.has_name ? f.name : current.name;
	const std::string path = f.has_path ? f.path : current.path;
	const std::string comment = f.has_comment ? f.comment : current.comment;
	const std::uint32_t color = f.has_color ? f.color : current.color;
	const std::uint8_t prio = f.has_prio ? f.prio : current.priority_code;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_UPDATE_CATEGORY));
	ec_req->AddTag(BuildCategoryTag(idx, name, path, comment, color, prio));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for UPDATE_CATEGORY");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void)RefresherTick(m_app, m_state);

	// Return the post-mutation category object.
	webapi::CategorySnapshot after = current;
	for (const auto &c : m_state.Categories()) {
		if (static_cast<std::uint8_t>(c.index) == idx) {
			after = c;
			break;
		}
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteCategoryObject(w, after);
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleCategoryDelete(
	const CHttpServer::Request &req, const std::string &index_str)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint8_t idx = 0;
	if (!ParseCategoryIndex(index_str, idx)) {
		return ErrorResponse(400, "bad_request", "path `{index}` must be a uint8 in [0, 255]");
	}
	if (!m_state.HasFirstSnapshot()) {
		return ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
	}
	// Index 0 is the implicit "All" category — amuled treats deleting
	// it as illegal. Reject before the EC roundtrip.
	if (idx == 0) {
		return ErrorResponse(400, "bad_request", "cannot delete the default (index=0) category");
	}
	bool found = false;
	for (const auto &c : m_state.Categories()) {
		if (static_cast<std::uint8_t>(c.index) == idx) {
			found = true;
			break;
		}
	}
	if (!found) {
		return ErrorResponse(404, "not_found", "no category with that index");
	}

	// CEC_Category_Tag CMD-detail shape: just `(EC_TAG_CATEGORY, idx)`,
	// no children (amule-remote-gui.cpp:1043 uses `CEC_Category_Tag(cat,
	// EC_DETAIL_CMD)`). We replicate that with a bare CECTag.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_DELETE_CATEGORY));
	ec_req->AddTag(CECTag(EC_TAG_CATEGORY, static_cast<std::uint32_t>(idx)));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for DELETE_CATEGORY");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void)RefresherTick(m_app, m_state);

	// amuled renumbers every download's category on delete (files at the
	// deleted index reset to 0, files above it shift down by one), but it
	// mutates m_category directly in CPartFile::RemoveCategory without
	// flagging the partfile dirty, so the change is never echoed back over
	// the incremental EC feed (EC_DETAIL_INC_UPDATE) that RefresherTick
	// consumes. Mirror the renumber into our cached snapshot ourselves —
	// exactly as amulegui does in CDownQueueRem::ResetCatParts — otherwise
	// downloads keep the stale (now-deleted) index and the next-created
	// category silently re-adopts them.
	m_state.MutateDownloads([idx](webapi::FileMap &files) {
		for (auto &kv : files) {
			webapi::FileSnapshot &f = kv.second;
			if (!f.is_downloading) {
				continue;
			}
			if (f.download.category == idx) {
				f.download.category = 0;
			} else if (f.download.category > idx) {
				f.download.category -= 1;
			}
		}
	});

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("index");
	w.ValueInt(static_cast<int64_t>(idx));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// Map wire-string search types to amule's EC_SEARCH_TYPE enum.
// "local" / "global" / "kad" matches amulegui's UI labels +
// amule-remote-gui.cpp:2406-2410's switch.
bool SearchTypeFromString(const std::string &s, std::uint8_t &out)
{
	if (s == "local") {
		out = EC_SEARCH_LOCAL;
		return true;
	} else if (s == "global") {
		out = EC_SEARCH_GLOBAL;
		return true;
	} else if (s == "kad") {
		out = EC_SEARCH_KAD;
		return true;
	}
	return false;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleSearchStart(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	// Body shape:
	//  { "query": "...", required string
	//    "type":  "local" | "global" | "kad" (default "global"),
	//    "file_type":  string (optional, amule file-type label),
	//    "extension":  string (optional, e.g. "mkv"),
	//    "min_size":   uint64 bytes (optional, default 0),
	//    "max_size":   uint64 bytes (optional, default 0 = no cap),
	//    "min_avail":  uint32 (optional, default 0) }
	std::string query;
	{
		const auto it = obj.find("query");
		if (it == obj.end() || !it->second.is<std::string>()) {
			return ErrorResponse(400, "bad_request", "required string field `query` is missing");
		}
		query = it->second.get<std::string>();
		if (query.empty()) {
			return ErrorResponse(400, "bad_request", "`query` must be non-empty");
		}
	}

	std::uint8_t search_type = EC_SEARCH_GLOBAL;
	std::string search_kind = "global"; // mirrors the input string for state.MarkSearchStarted
	{
		const auto it = obj.find("type");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400,
					"bad_request",
					"`type` must be one of \"local\", \"global\", \"kad\"");
			}
			search_kind = it->second.get<std::string>();
			if (!SearchTypeFromString(search_kind, search_type)) {
				return ErrorResponse(400,
					"bad_request",
					"`type` must be one of \"local\", \"global\", \"kad\"");
			}
		}
	}

	std::string file_type;
	{
		const auto it = obj.find("file_type");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request", "`file_type` must be a string");
			}
			file_type = it->second.get<std::string>();
		}
	}
	std::string extension;
	{
		const auto it = obj.find("extension");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request", "`extension` must be a string");
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
				return ErrorResponse(400,
					"bad_request",
					"`min_size` must be a non-negative integer (bytes)");
			}
			const double v = it->second.get<double>();
			if (v < 0)
				return ErrorResponse(400, "bad_request", "`min_size` must be >= 0");
			min_size = static_cast<std::uint64_t>(v);
		}
	}
	{
		const auto it = obj.find("max_size");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400,
					"bad_request",
					"`max_size` must be a non-negative integer (bytes; 0 = no cap)");
			}
			const double v = it->second.get<double>();
			if (v < 0)
				return ErrorResponse(400, "bad_request", "`max_size` must be >= 0");
			max_size = static_cast<std::uint64_t>(v);
		}
	}
	{
		const auto it = obj.find("min_avail");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(
					400, "bad_request", "`min_avail` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request", "`min_avail` out of range");
			}
			min_avail = static_cast<std::uint32_t>(v);
		}
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SEARCH_START));
	ec_req->AddTag(CEC_Search_Tag(wxString::FromUTF8(query.c_str()),
		static_cast<EC_SEARCH_TYPE>(search_type),
		wxString::FromUTF8(file_type.c_str()),
		wxString::FromUTF8(extension.c_str()),
		min_avail,
		min_size,
		max_size));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SEARCH_START");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Hand the lifecycle off to the refresher: it wipes the results
	// cache, marks active=true, captures the kind, and starts polling
	// EC_OP_SEARCH_RESULTS + _PROGRESS every tick until the state
	// machine in RefresherTick infers completion. Drops the prior
	// per-GET TtlCache fetch (the refresher is the single fetcher
	// now — needed so SSE search_result_added / search_progress fire
	// on the same delta the polling consumer would observe).
	m_state.MarkSearchStarted(search_kind);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("query");
	w.ValueString(wxString::FromUTF8(query.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSearchStop(const CHttpServer::Request &req)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SEARCH_STOP));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SEARCH_STOP");
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
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSearchDownload(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// Canonicalise the URL hash to lowercase.
	std::string needle = hash;
	std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
		return std::tolower(c);
	});

	CMD4Hash file_hash;
	if (!HashFromHex(needle, file_hash)) {
		return ErrorResponse(400, "bad_request", "`{hash}` must be a 32-char hex MD4");
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
				return ErrorResponse(
					400, "bad_request", "`category` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (v < 0 || v > 255) {
				return ErrorResponse(400, "bad_request", "`category` must be in [0, 255]");
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
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for DOWNLOAD_SEARCH_RESULT");
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
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("hash");
	w.ValueString(wxString::FromUTF8(needle.c_str()));
	w.Key("category");
	w.ValueInt(static_cast<int64_t>(category));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// SSE runs on a worker thread the HTTP server spawns per connection.
// Auth is enforced in PreflightEvents (synchronous, before head
// write and worker spawn); failures use the regular JSON error
// envelope. The 15 s heartbeat is a `: keepalive\n\n` SSE comment
// (RFC 6202) — proxies and many browsers drop idle TCP after ~30 s.
//
// DispatchStreaming reads head out-params ONCE before writing, so
// one function here sets the head AND runs the drain loop.
boost::optional<CHttpServer::Response> CApiDispatcher::PreflightEvents(const CHttpServer::Request &req)
{
	// Same bearer/cookie check the live handler used to do, but run
	// on the I/O thread BEFORE a worker thread is spawned and BEFORE
	// the 32-slot SSE budget is touched. Unauth/locked-out peers
	// get a normal request/response 401/429 and never reach the
	// streaming path; the slot stays free for legitimate
	// subscribers.
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok) {
		return a.rejection;
	}
	return boost::none;
}

void CApiDispatcher::DispatchEvents(const CHttpServer::Request &req,
	CHttpServer::Writer &writer,
	unsigned &http_status,
	std::string &content_type,
	std::map<std::string, std::string> &response_headers)
{
	// Auth ran inside PreflightEvents on the I/O thread before this
	// worker spawned, so we can assume an authenticated principal
	// here. Re-running Verify on the worker thread would just burn
	// one HMAC compare per connection for no security gain.
	auto a = AuthenticateRequestRateLimited(
		req, m_jwt, m_revocations, m_authRateLimiter, kSessionCookieName);
	if (!a.ok) {
		// Defence in depth — if PreflightEvents was bypassed for any
		// reason (test harness, future routing change) we still
		// reject here, just not as cheaply.
		http_status = a.rejection.status;
		content_type = "application/json";
		writer.Write(a.rejection.body);
		return;
	}
	// SSE doesn't need admin role — reads are guest-friendly. The
	// channel multiplexes every event type clients want to subscribe
	// to. Admin-gated mutations don't ship over SSE; SSE is a read-
	// only push.

	http_status = 200;
	content_type = "text/event-stream";
	response_headers["Cache-Control"] = "no-cache";
	response_headers["X-Accel-Buffering"] = "no"; // disable nginx buffering

	// CORS on the SSE response too. EventSource sends
	// `Origin` and reads only the standard CORS bundle for credentialed
	// cross-origin streams. No Expose-Headers needed (SSE clients don't
	// read response headers programmatically).
	{
		const std::string cors_org = ResolveCorsOrigin(req, m_config);
		ApplyCorsHeaders(response_headers, cors_org, m_config.ServerCfg().allow_cors);
	}
	// Also disable Connection: keep-alive override — chunked +
	// streaming requires the default. (HttpServer adds chunked
	// transfer-encoding automatically.)

	// Initial reassurance chunk so the client knows the channel is
	// open. Some browser EventSource impls don't fire `onopen` until
	// at least one chunk lands.
	if (!writer.Write(": connected\n\n"))
		return;

	// Optional `?channels=<csv>` query: limit the event types
	// delivered to a comma-separated subset. The mapping from
	// EventBus event name → channel is prefix-based:
	//  download_*  → "downloads"
	//  shared_*    → "shared"
	//  server_*    → "servers"
	//  client_*    → "clients"
	//  status_*    → "status"
	//  log_*       → "logs"
	// The synthetic per-subscriber `resync` event is ALWAYS
	// delivered regardless of filter — its purpose is to signal a
	// cache invalidation the client cannot opt out of.
	// Unknown channel names in the query are silently ignored (allow
	// forward-compatibility with future event families).
	std::set<std::string> channel_filter;
	bool channels_set = false;
	{
		// Cap unique channel tokens at 32 (six today + headroom)
		// so a 1 MB `channels=` query can't build a 1M-entry set in
		// the SSE worker.
		constexpr std::size_t kMaxChannelTokens = 32;
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos)
			query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("channels");
		if (it != qmap.end() && !it->second.empty()) {
			channels_set = true;
			std::string cur;
			bool overflowed = false;
			auto insert_token = [&](std::string &&s) {
				if (channel_filter.size() >= kMaxChannelTokens) {
					overflowed = true;
					return;
				}
				channel_filter.insert(std::move(s));
			};
			for (char c : it->second) {
				if (c == ',') {
					if (!cur.empty())
						insert_token(std::move(cur));
					cur.clear();
					if (overflowed)
						break;
				} else {
					cur.push_back(c);
				}
			}
			if (!overflowed && !cur.empty())
				insert_token(std::move(cur));
		}
	}
	auto event_channel = [](const std::string &name) -> std::string {
		// Event naming convention: every bus event MUST contain at
		// least one underscore — the prefix before the first `_`
		// identifies the channel. The only no-underscore name on
		// the wire is `resync`, which bypasses this filter entirely
		// (synthetic per-subscriber, never via EventBus::Publish).
		// Future bare-token events need explicit channel mapping or
		// must always bypass like `resync`.
		const auto us = name.find('_');
		if (us == std::string::npos)
			return name;
		const std::string prefix = name.substr(0, us);
		if (prefix == "download")
			return "downloads";
		if (prefix == "shared")
			return "shared";
		if (prefix == "server")
			return "servers";
		if (prefix == "client")
			return "clients";
		if (prefix == "status")
			return "status";
		if (prefix == "log")
			return "logs";
		if (prefix == "search")
			return "search";
		return prefix;
	};
	auto event_passes_filter = [&](const std::string &name) {
		if (!channels_set)
			return true;
		return channel_filter.count(event_channel(name)) > 0;
	};

	// Drain blocks up to the heartbeat interval (15 s); on timeout we
	// emit `: keepalive` so the connection stays warm.
	//
	// `since_id` resolution per RFC 6202 §4 reconnect:
	//  - absent / unparseable → start from NewestId (events fired
	//    AFTER connect only)
	//  - in-range (parsed+1 >= OldestId) → resume from `parsed`; the
	//    first Drain returns the missed range immediately
	//  - gap (parsed+1 < OldestId) → events evicted before this
	//    client read them; emit `resync` (reason=gap) so the client
	//    invalidates + re-GETs REST collections, then start from
	//    NewestId
	//  - parsed > NewestId → stale id from a prior daemon process
	//    (ids reset to 1 on restart); emit `resync` (reason=restart)
	//    and start from NewestId.
	std::uint64_t since_id;
	const std::string lei = FindHeaderCaseInsensitive(req.headers, "Last-Event-ID");
	const std::uint64_t newest = m_app.EventBus().NewestId();
	const std::uint64_t oldest = m_app.EventBus().OldestId();
	if (lei.empty()) {
		since_id = newest;
	} else {
		char *end = nullptr;
		const unsigned long long parsed = std::strtoull(lei.c_str(), &end, 10);
		if (end == lei.c_str() || *end != '\0') {
			since_id = newest;
		} else if (parsed > newest) {
			// Per-subscriber synthetic event — not on the bus. id is
			// the current newest so the client's EventSource resumes
			// from there on the next reconnect (no resync loop).
			std::ostringstream frame;
			frame << "event: resync\n"
			      << "id: " << newest << "\n"
			      << "data: {\"reason\":\"restart\",\"since_id\":"
			      << static_cast<std::uint64_t>(parsed) << ",\"newest_id\":" << newest << "}\n\n";
			if (!writer.Write(frame.str()))
				return;
			since_id = newest;
		} else if (oldest == 0 || parsed + 1 >= oldest) {
			since_id = static_cast<std::uint64_t>(parsed);
		} else {
			std::ostringstream frame;
			frame << "event: resync\n"
			      << "id: " << newest << "\n"
			      << "data: {\"reason\":\"gap\",\"since_id\":"
			      << static_cast<std::uint64_t>(parsed) << ",\"newest_id\":" << newest << "}\n\n";
			if (!writer.Write(frame.str()))
				return;
			since_id = newest;
		}
	}
	// Heartbeat is wall-clock driven, not Drain-timeout driven —
	// a busy bus + `?channels=` that filters every drained event
	// would otherwise leave the wire silent (Drain returns
	// immediately, loop swallows + re-enters, keepalive never
	// fires). NAT/proxies/EventSource clients drop idle TCP after
	// ~30–60 s, so emit `: keepalive` whenever last-write falls
	// behind the 15 s budget.
	const auto heartbeat_interval = std::chrono::seconds(15);
	auto last_write_at = std::chrono::steady_clock::now();
	std::vector<webapi::Event> drained;
	while (writer.Alive()) {
		// Shutdown poll. The Shutdown() flag is set by the App on
		// OnExit, and Drain() returns immediately when it's
		// observed. If the daemon is going down, drop this client
		// cleanly so the dispatcher reset() doesn't race a worker
		// still holding `m_app` references.
		if (m_app.EventBus().IsShutdown())
			break;
		drained.clear();
		const std::uint64_t new_high = m_app.EventBus().Drain(since_id, heartbeat_interval, drained);
		if (!writer.Alive())
			break;
		if (m_app.EventBus().IsShutdown())
			break;

		// Live-path gap detection. Reconnect handler above only
		// catches gaps at session start; once running, a burst that
		// fills + evicts the ring between Drains would silently drop
		// the missed range. Check OldestId after each Drain — on
		// cursor fall-off emit a typed resync and restart at newest.
		const std::uint64_t oldest_now = m_app.EventBus().OldestId();
		const std::uint64_t newest_now = m_app.EventBus().NewestId();
		if (oldest_now > 0 && since_id + 1 < oldest_now) {
			std::ostringstream gap_frame;
			gap_frame << "event: resync\n"
				  << "id: " << newest_now << "\n"
				  << "data: {\"reason\":\"gap\",\"since_id\":" << since_id
				  << ",\"newest_id\":" << newest_now << "}\n\n";
			if (!writer.Write(gap_frame.str()))
				break;
			last_write_at = std::chrono::steady_clock::now();
			since_id = newest_now;
			// Drop the events the Drain returned — the client is
			// about to re-fetch the REST collections (that's the
			// `resync` contract) so any partial pre-resync events
			// would be confusing noise.
			continue;
		}

		// Apply ?channels= filter before emission. We still advance
		// since_id over EVERY drained event (filtered or not) so the
		// client doesn't re-see them on reconnect; reconnect replay is
		// id-based, not channel-based.
		std::ostringstream frame;
		bool wrote_any = false;
		for (const auto &ev : drained) {
			if (!event_passes_filter(ev.name))
				continue;
			// SSE frame:  event: <name>\nid: <id>\ndata: <data>\n\n
			// Per RFC 6202 §4 `data:` lines are single-line; our JSON
			// payloads never contain literal newlines (EventDiff
			// escapes them), so one `data:` line per event suffices.
			//
			// `ev.name` is NOT escaped — every event name on the bus
			// is a server-controlled compile-time literal. A future
			// publisher taking a name from external input MUST
			// sanitize CR/LF/`\0` at its call site.
			frame << "event: " << ev.name << "\n"
			      << "id: " << ev.id << "\n"
			      << "data: " << ev.data << "\n\n";
			wrote_any = true;
		}
		if (wrote_any) {
			if (!writer.Write(frame.str()))
				break;
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
				if (!writer.Write(": keepalive\n\n"))
					break;
				last_write_at = now;
			}
		}
	}
}
