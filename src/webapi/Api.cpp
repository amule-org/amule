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
#include "JsonDepthScan.h" // webapi::JsonNestingWithinLimit

#include "ClientTagNames.h" // Needed for the shared client-tag token decoders
// Shared server capability-bit tables, decoded to JSON
#include "ServerFlagNames.h"

#include "config.h" // AMULEAPI_STATIC_DIR (compile-time install path)
#include "AmuleApiConfig.h"
#include "App.h"
#include "Auth.h"
#include "ConstantTime.h"
#include "Etag.h"
#include "JsonWriter.h"

#include <mutex> // serialises the shared-directory read-modify-write
#include "Jwt.h"
#include "PathPatterns.h"
#include "Refresher.h" // ParseStatsTreeFromPacket / ParseGraphsFromPacket / ApplySearchFull
#include "StaticFs.h"  // IsDir, ResolveWithinRoot
#include <cstring>
#include <map>

#include "PrefsSchema.h"
#include "SearchJson.h" // WriteSearchResultFields, shared with the SSE payload
#include "State.h"

#include "Constants.h"
#include "OtherFunctions.h" // GetFiletypeByName for the shared file_type token
#include <common/Path.h>    // CPath
#include <icon_data.h>      // amule_find_icon — country flags for GET /flags/{code}.png

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

#include "PicoJson_Inc.h"

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

// Serialise the writer's buffer into the response body. Every JSON response in
// this file goes through here -- open-coding it is how nine handlers came to
// hold a stale copy of the conversion.
// Emit `key` as a number, or as null when the value is not known.
//
// Six sites spelled this out as a four-line if/else, which is how one response
// family came to carry four different spellings of "I don't know". The rule is
// in REFERENCE.md under `Unknown values`; this is the one place that implements
// it, so a seventh nullable field cannot quietly pick -1 or 0 instead.
void WriteIntOrNull(CJsonWriter &w, const char *key, bool known, std::int64_t value)
{
	w.Key(key);
	if (known)
		w.ValueInt(value);
	else
		w.ValueNull();
}

void FinalizeJsonBody(CJsonWriter &w, CHttpServer::Response &r)
{
	// The writer already holds UTF-8, so this is a move, not a convert+copy.
	// Never route it through wxString: amuleapi calls neither setlocale nor
	// wxLocale, so it runs in the "C" locale whatever LANG says, and
	// wxString's std::string ctor decodes with the locale -- turning a body
	// with any non-ASCII byte into an empty one.
	r.body = w.TakeBuffer();
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
	FinalizeJsonBody(w, r);
	return r;
}

// 405 with the `Allow` header RFC 9110 §15.5.6 requires ("The origin server
// MUST generate an Allow header field in a 405 response containing a list of
// the target resource's currently supported methods"). The accepted methods
// were already spelled out in the human-readable message at every rejection
// site; generic tooling and capability discovery read the header instead, so
// the same list is now emitted both ways. `allow` is the machine-readable
// form -- comma-separated, in RFC order -- and includes HEAD wherever GET is
// served, which several of the messages omit.
CHttpServer::Response MethodNotAllowed(const char *allow, const char *message)
{
	CHttpServer::Response r = ErrorResponse(405, "method_not_allowed", message);
	r.headers["Allow"] = allow;
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
AuthOutcome AuthenticateRequest(const CHttpServer::Request &req,
	CJwt &jwt,
	webapi::CRevocationSet &revocations,
	const std::string &cookie_name,
	std::time_t credentials_changed_at)
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
	// A password change ends the sessions the old password opened —
	// otherwise rotating a leaked password would leave whoever leaked it
	// logged in for up to a day. The cutoff is the credential file's own
	// mtime, so this holds however the change was made: over REST, from
	// the amuleapi CLI, from aMule's preferences dialog, or pushed to
	// amuled from amulegui. It also survives a restart, because the
	// timestamp is a property of the file rather than of this process.
	if (credentials_changed_at > 0 && out.verified.iat < credentials_changed_at) {
		out.rejection =
			ErrorResponse(401, "unauthorized", "credentials changed; please sign in again");
		return out;
	}
	out.ok = true;
	return out;
}

// Admin role gate. Drop-in for the standard ` if (!a.ok) return
// a.rejection;` pattern; callers chain ` if (auto r = RequireAdmin(a))
// return *r;` immediately after. Used by every mutation handler and by
// the /auth/passwords pair.
std::unique_ptr<CHttpServer::Response> RequireAdmin(const AuthOutcome &a)
{
	if (a.verified.role != Role::ADMIN) {
		return std::make_unique<CHttpServer::Response>(
			ErrorResponse(403, "forbidden", "admin role required for this endpoint"));
	}
	return nullptr;
}

// First-snapshot gate, the counterpart to RequireAdmin above and used the
// same way: ` if (auto r = RequireSnapshot(m_state)) return *r;`.
//
// Until the first EC snapshot lands there is nothing to answer from, and
// every handler that touches cached state has to say so identically -- the
// status, the code and the sentence are part of the API contract, not local
// wording. Thirty handlers had spelled the same four lines out by hand.
std::unique_ptr<CHttpServer::Response> RequireSnapshot(const webapi::CState &state)
{
	if (!state.HasFirstSnapshot()) {
		return std::make_unique<CHttpServer::Response>(ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet"));
	}
	return nullptr;
}

// The URL carries a file hash in whatever case the caller typed; the snapshot
// keys everything lowercase. Canonicalising was open-coded at seventeen call
// sites as the same four lines, so it lives here now -- and the two lookups
// that always follow it come with it, since "lower-case then find" is the
// whole operation every one of those sites wanted.
std::string LowerHexKey(const std::string &key)
{
	std::string out = key;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return out;
}

bool FindDownloadByKey(const webapi::CState &state, const std::string &key, webapi::FileSnapshot &out)
{
	return state.FindDownload(LowerHexKey(key), out);
}

bool FindSharedByKey(const webapi::CState &state, const std::string &key, webapi::FileSnapshot &out)
{
	return state.FindShared(LowerHexKey(key), out);
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
	const std::string &cookie_name,
	std::time_t credentials_changed_at)
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

	out = AuthenticateRequest(req, jwt, revocations, cookie_name, credentials_changed_at);
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

CApiDispatcher::CApiDispatcher(CAmuleApiConfig &config, CJwt &jwt, webapi::CState &state, CamuleapiApp &app)
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
m_authRateLimiter(webapi::CRateLimiter::Config{ config.AuthCfg().token_failure_window_seconds,
	config.AuthCfg().token_failure_threshold,
	config.AuthCfg().token_lockout_seconds })
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
	AppendHeaderToken(headers, "Vary", "Origin");
	if (resolved_origin.empty())
		return;
	headers["Access-Control-Allow-Origin"] = resolved_origin;
	headers["Access-Control-Allow-Credentials"] = "true";
	// Header names the client may read from `fetch().headers.get(...)`
	// — by default the Fetch spec only exposes the CORS-safelisted
	// response headers (Cache-Control, Content-Language, Content-Type,
	// Expires, Last-Modified, Pragma). amuleapi clients want to read
	// ETag for cache validation; SSE clients don't need this list.
	headers["Access-Control-Expose-Headers"] = "ETag, Allow, Retry-After";
}

// The Kad `network` rollup, byte-identical on GET /status (nested
// under `kad`) and on GET /kad. Both read the same KadSnapshot from
// the same Dashboard() acquisition, so there is one set of counters
// and one place that names them.
void WriteKadNetworkObject(CJsonWriter &w, const webapi::KadSnapshot &k)
{
	w.Key("network");
	w.BeginObject();
	w.Key("users");
	w.ValueInt(static_cast<int64_t>(k.users));
	w.Key("files");
	w.ValueInt(static_cast<int64_t>(k.files));
	w.Key("nodes");
	w.ValueInt(static_cast<int64_t>(k.nodes));
	w.EndObject();
}

// The {id} path segment of every search-scoped route. Accepts a plain
// non-negative decimal that fits a uint32 and is not 0.
//
// Zero is rejected rather than treated as "no search": it used to be the
// sentinel that meant "whichever search this session started last", and
// letting it through would quietly resurrect exactly the implicit-target
// behaviour these routes exist to remove. A non-numeric segment is rejected
// for the same reason -- it must never fall back to some other search.
const char *const kBadSearchIdMessage = "`{id}` must be a positive decimal search_id (see GET /search)";

bool ParseSearchIdSegment(const std::string &seg, std::uint32_t &out)
{
	if (seg.empty() || seg.size() > 10)
		return false;
	std::uint64_t v = 0;
	for (const char c : seg) {
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + static_cast<std::uint64_t>(c - '0');
	}
	if (v == 0 || v > 0xFFFFFFFFull)
		return false;
	out = static_cast<std::uint32_t>(v);
	return true;
}

// Forward declaration so HandleLogin (which sits above the helper's
// definition) can share the depth-cap defence. The definition lives
// near the other mutation-body parsers further down the file.
bool ParseJsonObjectBody(const std::string &body, picojson::value &out, std::string &err);

} // namespace

// Did the caller present credentials at all? Mirrors what Authenticate()
// accepts -- an Authorization header, or the session cookie the WebUI uses --
// without verifying them: an invalid or expired credential still means the
// response was computed for a specific caller and must not be shared.
bool RequestCarriesCredentials(const CHttpServer::Request &req, const std::string &cookie_name)
{
	if (!FindHeaderCaseInsensitive(req.headers, "Authorization").empty()) {
		return true;
	}
	const std::string cookie = FindHeaderCaseInsensitive(req.headers, "Cookie");
	if (cookie.empty()) {
		return false;
	}
	return !webapi::ExtractCookieValue(cookie, cookie_name).empty();
}

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
				"GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS";
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
	// Sampled BEFORE the handler runs, and again after. The memo pairs an
	// ETag with a revision, and the body is serialized inside the handler
	// under its own read lock which it has dropped by the time we get here
	// -- so reading the revision only afterwards can pair etag(old body)
	// with the NEW revision if a write lands in that window, and every
	// later hit then serves a validator that describes neither. Two
	// samples make the window observable: if anything moved, this response
	// is not attributable to a revision and is simply not memoized.
	const std::uint64_t rev_before = m_state.SnapshotRevision();
	CHttpServer::Response resp = DispatchToHandler(req);
	const std::uint64_t rev_after = m_state.SnapshotRevision();

	const bool is_safe_method = (req.method == "GET" || req.method == "HEAD");
	// A handler that computed its own validator owns it. Stamping the
	// body hash over the top would hand out two different ETags for one
	// resource depending on which branch answered -- which is what the
	// static path did, since it clears the body for HEAD and so only the
	// GET reached the hashing branch below.
	const bool handler_set_etag = (resp.headers.find("ETag") != resp.headers.end());
	if (webapi::ShouldStampEtag(is_safe_method, handler_set_etag, resp.status, resp.body.empty())) {
		// Skip the MD5 over a (potentially multi-MB) body when
		// nothing has changed since we last hashed it. The key is
		// (target, snapshot revision): the revision is advanced by
		// every writer of an eligible body, so unlike a timestamp it
		// cannot stand still through a mutation, cannot collapse two
		// changes inside one second, and does not depend on any
		// caller remembering to stamp it.
		//
		// Eligibility is opt-in -- see MemoizableTarget -- and covers
		// only /downloads and /shared, which is where skipping a hash
		// is worth anything. Everything else hashes per request and is
		// immune to a mispaired validator by construction.
		//
		// Revision stability is the other half, and MemoUsable carries
		// it: without it the key says which revision was current when
		// we looked, not which one this body came from.
		const std::uint64_t snap = rev_after;
		const bool memoizable = webapi::MemoUsable(req.target, rev_before, rev_after);
		std::string etag;
		if (memoizable) {
			std::lock_guard<std::mutex> g(m_etagCacheMu);
			auto it = m_etagCache.find(req.target);
			if (it != m_etagCache.end() && it->second.snapshot_rev == snap && snap != 0) {
				etag = it->second.etag;
			}
		}
		if (etag.empty()) {
			etag = webcommon::Etag(resp.body);
			if (memoizable) {
				std::lock_guard<std::mutex> g(m_etagCacheMu);
				if (m_etagCache.size() >= kEtagCacheCapacity) {
					// Crude memory backstop. Real workload is a few
					// dozen unique targets; the wholesale clear is
					// cheaper than a real LRU machinery.
					m_etagCache.clear();
				}
				EtagCacheEntry e;
				e.snapshot_rev = snap;
				e.etag = etag;
				m_etagCache[req.target] = std::move(e);
			}
		}
		// RFC 7232 §2.3 — the header value MUST be quoted.
		// Which representation is this validator naming? The transport
		// appends the coding when it compresses, but a 304 carries no
		// body for it to compress, so the answer has to be worked out
		// here -- while the body is still present to measure -- and it
		// has to be the SAME answer. A client that cached the gzip form
		// and gets the identity ETag back can never match its stored
		// response again.
		const bool coded = WillCompressBody(
			AcceptsGzip(FindHeaderCaseInsensitive(req.headers, "Accept-Encoding")),
			resp.body.size(),
			resp.content_type,
			resp.headers.find("Content-Encoding") != resp.headers.end());
		const std::string wire_etag = webcommon::WithCodingSuffix(etag, coded);
		resp.headers["ETag"] = "\"" + wire_etag + "\"";

		// Against wire_etag, which names the representation THIS request
		// selected. Matching either coding would defeat the suffix
		// entirely: a client holding the gzip bytes and asking for
		// identity would be told its copy is current, and one holding
		// identity bytes would be handed a 304 stamped with the gzip
		// validator, matching nothing it stored.
		const std::string inm = FindHeaderCaseInsensitive(req.headers, "If-None-Match");
		if (webcommon::IfNoneMatchHits(inm, wire_etag)) {
			// 304 carries no body and no Content-Type, but the ETag
			// header IS preserved (RFC 7232 §4.1 — clients use it to
			// re-stamp the cached representation).
			resp.status = 304;
			resp.body.clear();
			resp.content_type.clear();
		}
	}
	// HEAD carries no content, on ANY status. The strip used to live
	// inside the 200-only block above, so every HEAD that ended in 4xx or
	// 5xx shipped the JSON error envelope as content -- content RFC 9110
	// §9.3.2 says must not be there, and bytes a client that correctly
	// stops reading after the headers leaves in the socket to corrupt the
	// next response on a keep-alive connection. It is not stripped here
	// either: the transport writes headers only, so Content-Length still
	// reports what the equivalent GET would return.

	// A response produced for a caller who presented credentials is that
	// caller's, and must not be stored where another caller can be served
	// it. Authenticate() takes a bearer token OR a session cookie, and the
	// browser WebUI uses the cookie -- so these requests carry no
	// Authorization header, and RFC 9111 3.5's shared-cache prohibition
	// never engages on them. Without an explicit expiry a shared cache may
	// keep a 200 under heuristic freshness, and with Cookie absent from
	// Vary two users share a cache key.
	//
	// Stamped here rather than in each handler so a new authenticated
	// route cannot forget it, and only when the caller actually presented
	// credentials: an unauthenticated public probe like /version stays
	// cacheable and keeps its conditional GET. A handler that set its own
	// policy -- the static assets' public, no-cache, the country flags'
	// public, max-age, /auth/session's private, no-store, the SSE
	// no-cache -- is left alone.
	//
	// `private` and NOT `no-store`: no-store forbids the client's OWN
	// cache, so no browser would ever hold an entry to revalidate and no
	// authenticated route would ever see an If-None-Match -- which throws
	// away the whole point of the validator and memo machinery here, and
	// makes the WebUI re-transfer the full download list on every load. It
	// also lands on 304s, telling a cache to drop the entry it has just
	// been told is good. `private` alone is what answers the concern:
	// RFC 9111 3.5 stops a SHARED cache storing it, which is the cache
	// that could serve one user's list to another.
	if (RequestCarriesCredentials(req, kSessionCookieName) &&
		resp.headers.find("Cache-Control") == resp.headers.end()) {
		resp.headers["Cache-Control"] = "private";
		// Vary is the half that matters to a cache which stores anyway.
		AppendHeaderToken(resp.headers, "Vary", "Cookie");
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

	// `/api/v0/status/` and `/api/v0/status` name the same resource, so
	// route them the same way. Confined to the API prefix on purpose: the
	// static fallthrough below maps a path onto a filesystem, where a
	// trailing slash is a directory rather than a spelling.
	if (path.compare(0, 5, "/api/") == 0) {
		path = web_api_path::StripTrailingSlash(path);
	}

	if (path == "/api/v0/health") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET / HEAD on /health");
		}
		return HandleHealth(req);
	}

	if (path == "/api/v0/version") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "method not allowed on /api/v0/version");
		}
		return HandleVersion(req);
	}

	if (path == "/api/v0/version/check") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /api/v0/version/check");
		}
		return HandleVersionCheck(req);
	}

	if (path == "/api/v0/auth/login") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /auth/login");
		}
		return HandleLogin(req);
	}

	if (path == "/api/v0/auth/logout") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /auth/logout");
		}
		return HandleLogout(req);
	}

	if (path == "/api/v0/auth/session") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET on /auth/session");
		}
		return HandleSession(req);
	}

	if (path == "/api/v0/auth/passwords") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleAuthPasswords(req);
		}
		if (req.method == "PATCH") {
			return HandleAuthPasswordsPatch(req);
		}
		return MethodNotAllowed("GET, HEAD, PATCH", "only GET or PATCH on /auth/passwords");
	}

	// /events reaches the dispatcher only on a method the streaming
	// resolver declined, since it diverts GET and HEAD before this point.
	// Without an arm here the request fell through to the catch-all 404,
	// so the one endpoint a client is most likely to probe reported that
	// it does not exist -- and it was the only route to escape the Allow
	// sweep, while the docs promise the header on every 405.
	if (path == "/api/v0/events") {
		return MethodNotAllowed("GET, HEAD", "only GET / HEAD on /events");
	}

	if (path == "/api/v0/status") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET on /status");
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
		return MethodNotAllowed("GET, HEAD, POST, PATCH, DELETE",
			"only GET / HEAD / POST / PATCH / DELETE on /downloads");
	}

	// bulk clear-completed.
	if (path == "/api/v0/downloads_clear_completed") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /downloads/clear_completed");
		}
		return HandleDownloadsClearCompleted(req);
	}

	// /clients is the whole peer surface: every upload_state, including
	// queue waiters and download peers. Consumers filter client-side by
	// upload_state rather than asking for a pre-filtered view.
	if (path == "/api/v0/clients") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET on /clients");
		}
		return HandleClients(req);
	}

	// /known_clients — the daemon's credit store, every peer it has ever
	// exchanged data with. A separate resource rather than a sub-path of
	// /clients: these are keyed by user hash, outlive the daemon process that
	// would have issued an ECID, and carry stored history instead of live
	// transfer state. Matched before /clients/{ecid} regardless, since that
	// pattern accepts any single segment.
	if (path == "/api/v0/known_clients") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET on /known_clients");
		}
		return HandleKnownClients(req);
	}

	// /clients/{ecid} — single-peer detail (issue #422). GET/HEAD only;
	// {ecid} is the EC connection id (unique per live connection).
	{
		static const auto client_detail = web_api_path::ParsePattern("/api/v0/clients/{ecid}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(client_detail, path_segs, caps)) {
			if (req.method == "GET" || req.method == "HEAD") {
				return HandleClientDetail(req, caps["ecid"]);
			}
			return MethodNotAllowed("GET, HEAD", "only GET / HEAD on /clients/{ecid}");
		}
	}

	// /clients/{ecid}/shared_files — browse ("View Files") the peer's shared
	// file list (#399). POST starts the browse and returns a search_id.
	{
		static const auto client_browse =
			web_api_path::ParsePattern("/api/v0/clients/{ecid}/shared_files");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(client_browse, path_segs, caps)) {
			if (req.method == "POST") {
				return HandleClientBrowse(req, caps["ecid"]);
			}
			return MethodNotAllowed("POST", "only POST on /clients/{ecid}/shared_files");
		}
	}

	if (path == "/api/v0/shared") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleSharedList(req);
		}
		if (req.method == "PATCH") {
			// bulk upload-priority PATCH over {hashes:[...], priority}.
			return HandleSharedBulkPatch(req);
		}
		return MethodNotAllowed("GET, HEAD, PATCH", "only GET / HEAD / PATCH on /shared");
	}

	if (path == "/api/v0/shared_reload") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /shared/reload");
		}
		return HandleSharedReload(req);
	}

	// /shared/media/refresh — re-extract media metadata for the whole share.
	// Literal path, so it has to be matched before the /shared/{hash} patterns
	// below or "media" would be captured as a hash.
	if (path == "/api/v0/shared/media/refresh") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /shared/media/refresh");
		}
		return HandleSharedMediaRefresh(req);
	}

	// /shared/directories — the configured share roots, as opposed to /shared
	// which lists the files those roots produced. Literal path, so it has to be
	// matched before the /shared/{hash} patterns below or "directories" would be
	// captured as a hash.
	if (path == "/api/v0/share_directories") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleSharedDirectories(req);
		}
		if (req.method == "PUT") {
			return HandleSharedDirectoriesPut(req);
		}
		if (req.method == "POST") {
			return HandleSharedDirectoriesAdd(req);
		}
		if (req.method == "DELETE") {
			return HandleSharedDirectoriesDelete(req);
		}
		return MethodNotAllowed("GET, HEAD, POST, PUT, DELETE",
			"only GET / HEAD / PUT / POST / DELETE on /shared/directories");
	}

	if (path == "/api/v0/servers") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleServers(req);
		}
		if (req.method == "POST") {
			// add a server by host:port.
			return HandleServerAdd(req);
		}
		return MethodNotAllowed("GET, HEAD, POST", "only GET / HEAD / POST on /servers");
	}

	if (path == "/api/v0/friends") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleFriends(req);
		}
		if (req.method == "POST") {
			return HandleFriendAdd(req);
		}
		return MethodNotAllowed("GET, HEAD, POST", "only GET / HEAD / POST on /friends");
	}

	// One friend by ECID: remove, set the friend slot, or browse their shared
	// files. The browse form is checked first, same ordering rationale as the
	// server routes above.
	{
		static const auto friend_browse =
			web_api_path::ParsePattern("/api/v0/friends/{ecid}/shared_files");
		static const auto friend_one = web_api_path::ParsePattern("/api/v0/friends/{ecid}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(friend_browse, path_segs, caps)) {
			if (req.method != "POST") {
				return MethodNotAllowed("POST", "only POST on /friends/{ecid}/shared_files");
			}
			return HandleFriendBrowse(req, caps["ecid"]);
		}
		if (web_api_path::Match(friend_one, path_segs, caps)) {
			if (req.method == "DELETE") {
				return HandleFriendRemove(req, caps["ecid"]);
			}
			if (req.method == "PATCH") {
				return HandleFriendPatch(req, caps["ecid"]);
			}
			return MethodNotAllowed("PATCH, DELETE", "only DELETE / PATCH on /friends/{ecid}");
		}
	}

	if (path == "/api/v0/chats") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleChats(req);
		}
		return MethodNotAllowed("GET, HEAD", "only GET / HEAD on /chats");
	}

	// One conversation, keyed on "<ip>:<port>". The messages sub-resource is
	// matched first, same ordering rationale as the server / friend routes:
	// the longer pattern would otherwise be shadowed.
	{
		static const auto chat_messages = web_api_path::ParsePattern("/api/v0/chats/{peer}/messages");
		static const auto chat_one = web_api_path::ParsePattern("/api/v0/chats/{peer}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(chat_messages, path_segs, caps)) {
			if (req.method == "GET" || req.method == "HEAD") {
				return HandleChatMessages(req, caps["peer"]);
			}
			if (req.method == "POST") {
				return HandleChatSend(req, caps["peer"]);
			}
			return MethodNotAllowed(
				"GET, HEAD, POST", "only GET / HEAD / POST on /chats/{peer}/messages");
		}
		if (web_api_path::Match(chat_one, path_segs, caps)) {
			if (req.method == "DELETE") {
				return HandleChatClose(req, caps["peer"]);
			}
			return MethodNotAllowed("DELETE", "only DELETE on /chats/{peer}");
		}
	}

	// Address a conversation by ECID instead of ip:port, for a caller holding
	// a peer or friend row and no wish to compose the key. The friend form is
	// the one that reaches an OFFLINE friend.
	{
		static const auto friend_messages =
			web_api_path::ParsePattern("/api/v0/friends/{ecid}/messages");
		static const auto client_messages =
			web_api_path::ParsePattern("/api/v0/clients/{ecid}/messages");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(friend_messages, path_segs, caps)) {
			if (req.method != "POST") {
				return MethodNotAllowed("POST", "only POST on /friends/{ecid}/messages");
			}
			return HandleFriendMessageSend(req, caps["ecid"]);
		}
		if (web_api_path::Match(client_messages, path_segs, caps)) {
			if (req.method != "POST") {
				return MethodNotAllowed("POST", "only POST on /clients/{ecid}/messages");
			}
			return HandleClientMessageSend(req, caps["ecid"]);
		}
	}

	if (path == "/api/v0/servers_update") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /servers/update");
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
		static const auto server_addr_connect =
			web_api_path::ParsePattern("/api/v0/servers/by-address/{address}/connect");
		static const auto server_addr_one =
			web_api_path::ParsePattern("/api/v0/servers/by-address/{address}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		// The address form has its own path rather than sharing {ecid}.
		// One capture with two identity domains, disambiguated by sniffing
		// for a colon, is a dispatch rule invisible from outside -- and it
		// forecloses ever accepting an IPv6 literal here, since those are
		// all colons.
		if (web_api_path::Match(server_addr_connect, path_segs, caps)) {
			if (req.method != "POST") {
				return MethodNotAllowed(
					"POST", "only POST on /servers/by-address/{address}/connect");
			}
			return HandleServerConnectByAddress(req, caps["address"]);
		}
		if (web_api_path::Match(server_addr_one, path_segs, caps)) {
			if (req.method != "DELETE" && req.method != "PATCH") {
				return MethodNotAllowed("PATCH, DELETE",
					"only DELETE / PATCH on /servers/by-address/{address}");
			}
			return req.method == "PATCH" ? HandleServerPatchByAddress(req, caps["address"])
						     : HandleServerDeleteByAddress(req, caps["address"]);
		}
		if (web_api_path::Match(server_connect, path_segs, caps)) {
			if (req.method != "POST") {
				return MethodNotAllowed("POST", "only POST on /servers/{ecid}/connect");
			}
			return HandleServerConnect(req, caps["ecid"]);
		}
		if (web_api_path::Match(server_one, path_segs, caps)) {
			if (req.method != "DELETE" && req.method != "PATCH") {
				return MethodNotAllowed(
					"PATCH, DELETE", "only DELETE / PATCH on /servers/{ecid}");
			}
			return req.method == "PATCH" ? HandleServerPatch(req, caps["ecid"])
						     : HandleServerDelete(req, caps["ecid"]);
		}
	}

	if (path == "/api/v0/kad") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET on /kad");
		}
		return HandleKad(req);
	}

	// connection control.
	if (path == "/api/v0/networks/connect") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /networks/connect");
		}
		return HandleNetworksConnect(req);
	}
	if (path == "/api/v0/networks/disconnect") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /networks/disconnect");
		}
		return HandleNetworksDisconnect(req);
	}
	// /api/v0/kad/connect and /api/v0/kad/disconnect were dropped in
	// favour of /networks/{connect,disconnect} with `{"network":"kad"}`
	// — the two were strict aliases and the granular-selector form on
	// /networks/* makes the dedicated shortcut redundant.
	if (path == "/api/v0/kad/update") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /kad/update");
		}
		return HandleKadUpdateFromUrl(req);
	}

	if (path == "/api/v0/kad/bootstrap") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /kad/bootstrap");
		}
		return HandleKadBootstrap(req);
	}

	// IP filter actions. The IP-filter *settings* are ordinary
	// preferences; these two are the standalone operations behind the
	// desktop Security page's "Reload List" and "Update now" buttons.
	if (path == "/api/v0/ipfilter/reload") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /ipfilter/reload");
		}
		return HandleIpfilterReload(req);
	}

	if (path == "/api/v0/ipfilter/update") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /ipfilter/update");
		}
		return HandleIpfilterUpdate(req);
	}

	// re-hash one shared file against its on-disk data. Matched before the
	// single-segment `/shared/{hash}` pattern below purely for locality —
	// the two can't collide, this one carries an extra path segment.
	{
		static const auto shared_media_refresh =
			web_api_path::ParsePattern("/api/v0/shared/{hash}/media/refresh");
		static const auto shared_verify = web_api_path::ParsePattern("/api/v0/shared/{hash}/verify");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(shared_media_refresh, path_segs, caps)) {
			if (req.method != "POST") {
				return MethodNotAllowed("POST", "only POST on /shared/{hash}/media/refresh");
			}
			return HandleSharedMediaRefreshOne(req, caps["hash"]);
		}
		if (web_api_path::Match(shared_verify, path_segs, caps)) {
			if (req.method != "POST") {
				return MethodNotAllowed("POST", "only POST on /shared/{hash}/verify");
			}
			return HandleSharedVerify(req, caps["hash"]);
		}
	}

	// peers of one shared file. Same rows as /clients, selected by hash and
	// carrying their relation to the file; matched before `/shared/{hash}`.
	{
		static const auto shared_clients =
			web_api_path::ParsePattern("/api/v0/shared/{hash}/clients");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(shared_clients, path_segs, caps)) {
			if (req.method != "GET" && req.method != "HEAD") {
				return MethodNotAllowed(
					"GET, HEAD", "only GET / HEAD on /shared/{hash}/clients");
			}
			return HandleFileClients(req, caps["hash"], /*require_downloading=*/false);
		}
	}

	// shared file priority PATCH. `{hash}` is the lowercase 32-char hex
	// MD4 hash.
	{
		static const auto shared_detail = web_api_path::ParsePattern("/api/v0/shared/{hash}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(shared_detail, path_segs, caps)) {
			if (req.method == "GET" || req.method == "HEAD") {
				return HandleSharedDetail(req, caps["hash"]);
			}
			if (req.method != "PATCH") {
				return MethodNotAllowed(
					"GET, HEAD, PATCH", "only GET / HEAD / PATCH on /shared/{hash}");
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
		return MethodNotAllowed("GET, HEAD, POST", "only GET / HEAD / POST on /categories");
	}

	// single-category PATCH/DELETE.
	{
		static const auto category_one = web_api_path::ParsePattern("/api/v0/categories/{index}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(category_one, path_segs, caps)) {
			if (req.method == "GET" || req.method == "HEAD") {
				return HandleCategoryOne(req, caps["index"]);
			}
			if (req.method == "PATCH") {
				return HandleCategoryUpdate(req, caps["index"]);
			}
			if (req.method == "DELETE") {
				return HandleCategoryDelete(req, caps["index"]);
			}
			return MethodNotAllowed("GET, HEAD, PATCH, DELETE",
				"only GET / PATCH / DELETE on /categories/{index}");
		}
	}

	if (path == "/api/v0/preferences") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandlePreferences(req);
		}
		if (req.method == "PATCH") {
			return HandlePreferencesPatch(req);
		}
		return MethodNotAllowed("GET, HEAD, PATCH", "only GET / HEAD / PATCH on /preferences");
	}

	if (path == "/api/v0/logs/amule") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleLogAmule(req);
		}
		if (req.method == "DELETE") {
			return HandleLogAmuleReset(req);
		}
		return MethodNotAllowed("GET, HEAD, DELETE", "only GET / HEAD / DELETE on /logs/amule");
	}

	if (path == "/api/v0/logs/serverinfo") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleLogServerinfo(req);
		}
		if (req.method == "DELETE") {
			return HandleLogServerinfoReset(req);
		}
		return MethodNotAllowed("GET, HEAD, DELETE", "only GET / HEAD / DELETE on /logs/serverinfo");
	}

	if (path == "/api/v0/stats/tree") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET on /stats/tree");
		}
		return HandleStatsTree(req);
	}

	// search. Every search-scoped operation names its search in the path;
	// there is no implicit "current search" to fall back to.
	if (path == "/api/v0/search") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleSearchList(req);
		}
		if (req.method != "POST") {
			return MethodNotAllowed("GET, HEAD, POST",
				"only GET or POST on /search (GET lists searches, POST starts one; "
				"read one search at GET /search/{id}/results)");
		}
		return HandleSearchStart(req);
	}

	// The two hash-addressed routes are matched before anything capturing
	// {id}. They cannot actually collide (different segment counts, and {id}
	// is numeric), but ordering them first keeps that independent of the
	// matcher's internals. Both are deliberately search-AGNOSTIC: the daemon
	// resolves a download by hash against its whole search list, and a Kad
	// note fetched for a hash is fanned out to every result carrying it.
	// Nesting them under {id} would advertise a scoping that does not exist.
	{
		static const auto search_download =
			web_api_path::ParsePattern("/api/v0/search/results/{hash}/download");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(search_download, path_segs, caps)) {
			if (req.method != "POST") {
				return MethodNotAllowed(
					"POST", "only POST on /search/results/{hash}/download");
			}
			return HandleSearchDownload(req, caps["hash"]);
		}
	}

	// /search/results/{hash}/comments — community ratings/comments for a search
	// result (issue #434). POST triggers an on-demand Kad NOTES lookup; GET
	// returns the notes retrieved so far plus the running flag. The result's
	// `comments` also ride the results list, but this per-result path mirrors
	// /downloads/{hash}/comments for polling a single hash. Matched before
	// /search/results/{hash}/download (distinct trailing segment).
	{
		static const auto search_comments =
			web_api_path::ParsePattern("/api/v0/search/results/{hash}/comments");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(search_comments, path_segs, caps)) {
			if (req.method == "POST") {
				return HandleSearchCommentsKadSearch(req, caps["hash"]);
			}
			if (req.method != "GET" && req.method != "HEAD") {
				return MethodNotAllowed("GET, HEAD, POST",
					"only GET / HEAD / POST on /search/results/{hash}/comments");
			}
			return HandleSearchComments(req, caps["hash"]);
		}
	}

	// /search/{id} — DELETE stops the search AND frees it (results included).
	{
		static const auto search_one = web_api_path::ParsePattern("/api/v0/search/{id}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(search_one, path_segs, caps)) {
			std::uint32_t sid = 0;
			if (!ParseSearchIdSegment(caps["id"], sid)) {
				return ErrorResponse(400, "bad_request", kBadSearchIdMessage);
			}
			if (req.method != "DELETE") {
				return MethodNotAllowed("DELETE",
					"only DELETE on /search/{id} (read its results at "
					"GET /search/{id}/results)");
			}
			return HandleSearchClose(req, sid);
		}
	}

	// /search/{id}/{action} — results / stop / more.
	{
		static const auto search_action = web_api_path::ParsePattern("/api/v0/search/{id}/{action}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(search_action, path_segs, caps)) {
			std::uint32_t sid = 0;
			if (!ParseSearchIdSegment(caps["id"], sid)) {
				return ErrorResponse(400, "bad_request", kBadSearchIdMessage);
			}
			const std::string &action = caps["action"];
			if (action == "results") {
				if (req.method != "GET" && req.method != "HEAD") {
					return MethodNotAllowed(
						"GET, HEAD", "only GET / HEAD on /search/{id}/results");
				}
				return HandleSearchResults(req, sid);
			}
			if (action == "stop") {
				if (req.method != "POST") {
					return MethodNotAllowed("POST", "only POST on /search/{id}/stop");
				}
				return HandleSearchStop(req, sid);
			}
			if (action == "more") {
				if (req.method != "POST") {
					return MethodNotAllowed("POST", "only POST on /search/{id}/more");
				}
				return HandleSearchMore(req, sid);
			}
			return ErrorResponse(
				404, "not_found", "unknown search action (expected results, stop or more)");
		}
	}

	// /stats/graphs/{graph} — path-pattern matches the four allowed
	// graph names ("download_speed" / "upload_speed" / "connections" /
	// "kad_nodes" -- HandleStatsGraph rejects anything else).
	{
		static const auto graph_pattern = web_api_path::ParsePattern("/api/v0/stats/graphs/{graph}");
		const auto segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(graph_pattern, segs, caps)) {
			if (req.method != "GET" && req.method != "HEAD") {
				return MethodNotAllowed("GET, HEAD", "only GET on /stats/graphs/{graph}");
			}
			return HandleStatsGraph(req, caps["graph"]);
		}
	}

	// /downloads/{hash}/comments — per-source comments/ratings list
	// (issue #419). Downloads-only: needs a live source list. Matched
	// before /downloads/{hash} (more segments).
	{
		static const auto dl_comments =
			web_api_path::ParsePattern("/api/v0/downloads/{hash}/comments");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(dl_comments, path_segs, caps)) {
			if (req.method == "POST") {
				// POST triggers an on-demand Kad NOTES lookup; retrieved
				// community ratings/comments then appear on GET (issue #434).
				return HandleDownloadCommentsKadSearch(req, caps["hash"]);
			}
			if (req.method != "GET" && req.method != "HEAD") {
				return MethodNotAllowed("GET, HEAD, POST",
					"only GET / HEAD / POST on /downloads/{hash}/comments");
			}
			return HandleDownloadComments(req, caps["hash"]);
		}
	}

	// /downloads/{hash}/filenames — source-reported filenames + counts
	// (issue #420). Downloads-only.
	{
		static const auto dl_filenames =
			web_api_path::ParsePattern("/api/v0/downloads/{hash}/filenames");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(dl_filenames, path_segs, caps)) {
			if (req.method != "GET" && req.method != "HEAD") {
				return MethodNotAllowed(
					"GET, HEAD", "only GET / HEAD on /downloads/{hash}/filenames");
			}
			return HandleDownloadFilenames(req, caps["hash"]);
		}
	}

	// /downloads/{hash}/a4af — A4AF source list (GET) + swap actions
	// (POST). Downloads-only (issue #421).
	{
		static const auto dl_a4af = web_api_path::ParsePattern("/api/v0/downloads/{hash}/a4af");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(dl_a4af, path_segs, caps)) {
			if (req.method == "POST") {
				return HandleDownloadA4afAction(req, caps["hash"]);
			}
			// POST only. A4AF sources are rows of /downloads/{hash}/clients,
			// carrying the whole peer object rather than a bare ECID, and
			// `a4af_auto` is on the download detail object, so there is
			// nothing for a GET here to add. The swap actions have no
			// equivalent on those routes, and this reply is still the A4AF
			// view.
			return MethodNotAllowed("POST", "only POST on /downloads/{hash}/a4af");
		}
	}

	// sources and A4AF rows of one partfile. Matched before the bare
	// `/downloads/{hash}` pattern, which accepts any single segment.
	{
		static const auto dl_clients = web_api_path::ParsePattern("/api/v0/downloads/{hash}/clients");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(dl_clients, path_segs, caps)) {
			if (req.method != "GET" && req.method != "HEAD") {
				return MethodNotAllowed(
					"GET, HEAD", "only GET / HEAD on /downloads/{hash}/clients");
			}
			return HandleFileClients(req, caps["hash"], /*require_downloading=*/true);
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
			return MethodNotAllowed("GET, HEAD, PATCH, DELETE",
				"only GET / HEAD / PATCH / DELETE on /downloads/{hash}");
		}
	}

	// Country-flag artwork. Sits ahead of the static fallthrough so the
	// route answers identically whether or not StaticRoot is set — the
	// bytes are compiled in, not read from disk. Deliberately outside
	// /api/v0/: it is an image an <img src> points at, not a JSON
	// resource, and it carries no per-installation data.
	if (path.compare(0, 7, "/flags/") == 0) {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET / HEAD on /flags/{code}.png");
		}
		return ServeCountryFlag(req, path);
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
	// the install-path discovery chain. std::call_once because handlers
	// now run concurrently on the HTTP worker pool — a plain lazy bool
	// would race the string assignment on the first concurrent requests.
	// The answer is stable for the daemon's lifetime (operators editing
	// amuleapi.conf at runtime restart the daemon).
	std::call_once(m_static_root_once, [this]() {
		m_static_root_cache = m_config.ServerCfg().static_root;
		if (m_static_root_cache.empty()) {
			m_static_root_cache = ResolveDefaultStaticDir();
		}
	});
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
	// Case-insensitive: Beast preserves the client's wire casing, so a
	// lowercase `if-none-match` -- what an HTTP/2-shaped client library
	// produces -- used to miss here and silently lose conditional GET.
	// Through the shared matcher, not a string compare: the header may be
	// `*`, a comma-separated list, or a weak `W/"..."` validator (which is
	// what an nginx in front of us emits once it gzips). The outer layer
	// used to supply that grammar for the GET path; now that it steps aside
	// whenever a handler set its own ETag, this path has to speak it.
	const std::string inm_val = FindHeaderCaseInsensitive(req.headers, "If-None-Match");
	// A 304 has no body for the transport to compress, so which
	// representation this validator names is decided here, while the body
	// is still present to measure -- and the comparison uses that same
	// value, so a client is only told "unchanged" about the representation
	// it actually asked for.
	const bool coded =
		WillCompressBody(AcceptsGzip(FindHeaderCaseInsensitive(req.headers, "Accept-Encoding")),
			body.size(),
			StaticContentType(rel),
			/*already_encoded=*/false);
	// Through the shared helper, which takes the quoted form this path
	// carries as readily as the bare form the API path does. Three separate
	// hand-rolled spellings of "put the suffix on" is how the transport
	// ended up with one that could not take it back off again.
	const std::string wire_static_etag = webcommon::WithCodingSuffix(etag, coded);
	const std::string wire_static_bare =
		(wire_static_etag.size() >= 2 && wire_static_etag.front() == '"' &&
			wire_static_etag.back() == '"')
			? wire_static_etag.substr(1, wire_static_etag.size() - 2)
			: wire_static_etag;
	if (webcommon::IfNoneMatchHits(inm_val, wire_static_bare)) {
		CHttpServer::Response r;
		r.status = 304;
		// Response::content_type defaults to application/json, which on a
		// 304 for an HTML or CSS asset is simply wrong. Cleared rather
		// than corrected: a 304 carries no content, the transport omits
		// the header when it is empty, and this matches what the API 304
		// path already does.
		r.content_type.clear();
		r.headers["ETag"] = wire_static_etag;
		// Same policy the 200 carries, or a cache is told the shell is
		// private on the very response that confirms its copy is good.
		r.headers["Cache-Control"] = "public, no-cache";
		return r;
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = StaticContentType(rel);
	// The body is kept for HEAD too. The transport writes headers only,
	// so nothing reaches the wire, and keeping it is what lets
	// Content-Length report the real size instead of 0 -- and what stops
	// HEAD and GET disagreeing about this resource's validator, since the
	// outer layer skips stamping whenever a handler set its own ETag.
	r.body = std::move(body);
	r.headers["ETag"] = etag;
	// The WebUI shell is the same bytes for everyone, so it carries its own
	// policy rather than inheriting the authenticated default -- without
	// this it was stamped private and re-fetched per session.
	//
	// no-cache, NOT a max-age. These filenames carry no content hash:
	// index.html, app.js and app.css keep their names across a rebuild. A
	// freshness lifetime therefore buys nothing that the ETag does not
	// already give -- and costs correctness, because must-revalidate only
	// governs what a cache may do once the entry is ALREADY stale
	// (RFC 9111 5.2.2.2). Inside the lifetime the browser serves the copy
	// it has without asking, so an upgraded daemon would keep handing out
	// the old shell for the rest of the window, and since each asset
	// expires on its own clock a new shell can be paired with an old
	// bundle -- breakage that looks like a bug rather than a stale page.
	//
	// no-cache means revalidate every time, not "do not store": the copy
	// stays in the cache and an unchanged bundle costs one conditional GET
	// answered 304 with no body. Reserve a long max-age for content-hashed
	// filenames, where the URL changes when the bytes do.
	//
	// public is load-bearing alongside it, not decoration. RFC 9111 3.5
	// bars a shared cache from reusing a response to a request that
	// carried an AUTHORIZATION HEADER unless the response carries one of
	// public, must-revalidate or s-maxage, and no-cache is not on that
	// list. The browser WebUI is not the case that engages it -- it
	// authenticates by cookie, which is exactly why the credential stamp
	// above needs Vary: Cookie instead. A bearer-token client fetching the
	// same shell is: its request does carry the header. Without public, a
	// proxy in front of the daemon must then hold a per-token copy of a
	// file identical for every caller. It changes nothing for a client
	// that is not a shared cache: no-cache still forces revalidation on
	// every use.
	r.headers["Cache-Control"] = "public, no-cache";
	return r;
}

CHttpServer::Response CApiDispatcher::ServeCountryFlag(
	const CHttpServer::Request &, const std::string &url_path)
{
	// Exact shape only: "/flags/" + name + ".png".
	static const std::string kPrefix = "/flags/";
	static const std::string kSuffix = ".png";
	if (url_path.size() <= kPrefix.size() + kSuffix.size() ||
		url_path.compare(0, kPrefix.size(), kPrefix) != 0 ||
		url_path.compare(url_path.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
		return ErrorResponse(404, "not_found", "no such flag");
	}
	const std::string code =
		url_path.substr(kPrefix.size(), url_path.size() - kPrefix.size() - kSuffix.size());

	// Two lowercase ASCII letters — the shape `country_code` arrives in
	// — plus the one literal name the set ships alongside the alpha-2
	// files: "unknown", the "??" placeholder CCountryFlags falls back
	// to for an empty or unrecognised code, offered here so a frontend
	// can match the desktop instead of inventing its own.
	//
	// The art id is built by concatenation, so this whitelist is what
	// stops a crafted code from naming a non-flag entry in the shared
	// icon table ("/flags/../amule.png" and friends — LooksMalicious
	// already rejects those upstream, but the lookup must not depend
	// on that).
	const bool is_alpha2 =
		code.size() == 2 && code[0] >= 'a' && code[0] <= 'z' && code[1] >= 'a' && code[1] <= 'z';
	if (!is_alpha2 && code != "unknown") {
		return ErrorResponse(404, "not_found", "no such flag");
	}

	// The famfamfam set covers 248 of the ~300 assignable alpha-2
	// codes, and GeoIP can resolve one it has no artwork for, so a
	// well-formed miss is a normal 404 rather than an error.
	const struct AMuleIconEntry *icon = amule_find_icon(("flag_" + code).c_str());
	if (!icon || icon->png_data == nullptr || icon->png_len == 0) {
		return ErrorResponse(404, "not_found", "no such flag");
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "image/png";
	// The ETag / If-None-Match -> 304 swap is applied to every 200
	// GET/HEAD by Dispatch(), and a HEAD is written as headers only by the
	// transport rather than stripped here, so this handler only has to
	// produce the bytes. (Dispatch no longer strips the body: keeping it
	// is what lets Content-Length report what a GET would return.)
	r.body.assign(reinterpret_cast<const char *>(icon->png_data), icon->png_len);
	// The artwork is compiled in: it can only change with a new build,
	// and a peer list is a page full of <img> tags pointing here. A day
	// of freshness turns those into cache hits instead of one
	// conditional request per distinct country per reload, while still
	// bounding how long an upgraded daemon keeps serving stale art.
	r.headers["Cache-Control"] = "public, max-age=86400";
	return r;
}

// GET /health. The surface had no liveness probe, so `/version` was being used
// as one -- a document whose body changes over time, whose name says something
// else, and which reported the daemon's update state to an unauthenticated
// caller.
//
// Liveness, not readiness: always 200 while the HTTP server is answering, so a
// container or load-balancer probe never restarts a healthy process just
// because amuled went away. Readiness is in the body instead, where a caller
// that wants it can key on the two flags without the status code moving under
// a caller that does not.
//
// No EC roundtrip. amuleapi serialises EC through one worker, so a probe that
// waited on the daemon could block behind an unrelated slow mutation and time
// out at the read deadline, reporting the service as down when it is merely
// busy. Both flags are process-local reads.
CHttpServer::Response CApiDispatcher::HandleHealth(const CHttpServer::Request &)
{
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("status");
	w.ValueString(wxT("ok"));
	w.Key("ec_connected");
	w.ValueBool(m_state.EcConnected());
	w.Key("snapshot");
	w.ValueBool(m_state.HasFirstSnapshot());
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleVersion(const CHttpServer::Request &req)
{
	// The identity fields stay unauthenticated: this is the liveness/version
	// probe, and a probe has to work before anyone holds a token. The `update`
	// block does not, because it reports whether THIS daemon is running an
	// outdated build, and that is not something an unauthenticated caller on a
	// deliberately reachable interface should learn. A client that shows an
	// "update available" banner is already authenticated when it does.
	const auto auth = Authenticate(req);

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

	// Update-availability, relayed from the connected daemon (never checked
	// by amuleapi itself). All fields are English / C-locale per the API
	// contract. When the daemon can't check -- built without
	// ENABLE_VERSION_CHECK, the check_new_version pref off, or a pre-3.1
	// daemon that emits none of these tags -- check_enabled is false and a
	// client should show nothing. update_available / last_checked are null
	// until a check has completed.
	if (auth.ok) {
		const auto prefs = m_state.Preferences();
		const auto status = m_state.Status();
		const bool check_enabled = prefs.version_check_available && prefs.check_new_version;
		const bool checked = status.version_check_done;
		w.Key("update");
		w.BeginObject();
		w.Key("check_enabled");
		w.ValueBool(check_enabled);
		w.Key("checked");
		w.ValueBool(checked);
		w.Key("latest_version");
		w.ValueString(wxString::FromUTF8(status.version_check_latest.c_str()));
		w.Key("update_available");
		if (checked) {
			w.ValueBool(status.version_check_outdated);
		} else {
			w.ValueNull();
		}
		WriteIntOrNull(w,
			"last_checked",
			checked && status.version_check_timestamp > 0,
			static_cast<std::int64_t>(status.version_check_timestamp));
		w.EndObject();
	}
	w.EndObject();
	FinalizeJsonBody(w, r);
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

	// Admin first, then guest. The comparison itself is constant time
	// inside webcommon; what is visible from outside is the PBKDF2 cost,
	// which is why the rate limiter above runs first. This is also the
	// point where amuleapi picks up a password another process wrote —
	// aMule's preferences dialog, or amuled applying an EC push from
	// amulegui — and where a record predating the current KDF cost gets
	// upgraded. Empty roles skip the KDF entirely, so the
	// nothing-configured case below costs nothing to reach.
	Role role = Role::GUEST;
	const CAmuleApiConfig::MatchedRole matched = m_config.VerifyPassword(md5_hex);
	if (matched == CAmuleApiConfig::MatchedRole::Admin) {
		role = Role::ADMIN;
	}

	if (matched == CAmuleApiConfig::MatchedRole::None) {
		// Distinguish "nothing is configured" from "wrong password" —
		// otherwise every login silently fails and the operator
		// debugging "why isn't login working" suspects the JWT. Read
		// after VerifyPassword, which is what refreshes it from disk.
		// A misconfiguration is not a failed guess, so it does not
		// count against the rate limiter.
		if (!m_config.HasAnyCredential()) {
			return ErrorResponse(503,
				"login_disabled",
				"amuleapi has no admin/guest password configured; "
				"set one via `amuleapi --set-admin-pass=<plain>`");
		}
		m_rateLimiter.NoteFailure(ip);
		return ErrorResponse(
			401, "invalid_credentials", "password does not match any configured role");
	}

	m_rateLimiter.NoteSuccess(ip);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";

	CJsonWriter w;
	w.BeginObject();
	BeginSession(req, role, r, w);
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// Issues a session for `role`, attaches the cookie to `r`, and writes the
// standard session fields into the object `w` is currently building.
//
// Shared by /auth/login and by the password change, which re-issues so
// that changing a password does not sign the caller out of the request
// they are in the middle of making.
void CApiDispatcher::BeginSession(
	const CHttpServer::Request &req, Role role, CHttpServer::Response &r, CJsonWriter &w)
{
	const CJwt::IssuedToken issued = m_jwt.Issue(role);
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
	FinalizeJsonBody(w, r);
	return r;
}

AuthOutcome CApiDispatcher::Authenticate(const CHttpServer::Request &req)
{
	return AuthenticateRequestRateLimited(req,
		m_jwt,
		m_revocations,
		m_authRateLimiter,
		kSessionCookieName,
		m_config.CredentialsChangedAt());
}

CHttpServer::Response CApiDispatcher::HandleSession(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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
	// Per-principal document: a shared cache must not hand one caller
	// another's session. Our own ETag memo excludes this target for the
	// same reason; this is the half that binds anything in front of us.
	r.headers["Cache-Control"] = "private, no-store";
	FinalizeJsonBody(w, r);
	return r;
}

// GET /auth/passwords — what is configured, never the credentials
// themselves. Admin-only: whether a guest account exists is not something
// a guest session needs to know.
CHttpServer::Response CApiDispatcher::HandleAuthPasswords(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto r = RequireAdmin(a))
		return *r;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";

	CJsonWriter w;
	w.BeginObject();
	w.Key("admin_set");
	w.ValueBool(!m_config.AdminCredential().empty());
	w.Key("guest_enabled");
	w.ValueBool(!m_config.GuestCredential().empty());
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// PATCH /auth/passwords — change the admin password, and turn the guest
// role on/off or change its password.
//
// `current_password` is mandatory even though the caller already holds an
// admin token: a stolen token should not be enough to lock the real
// operator out of their own daemon. It goes through the same rate limiter
// as /auth/login, so this is not a softer place to guess passwords.
//
// Fields are omitted rather than nulled to mean "leave alone" — see
// webcommon::CredentialChange, which every entry point shares.
CHttpServer::Response CApiDispatcher::HandleAuthPasswordsPatch(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto r = RequireAdmin(a))
		return *r;

	const std::string &ip = req.remote_addr;
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

	picojson::value v;
	std::string err;
	if (!ParseJsonObjectBody(req.body, v, err)) {
		return ErrorResponse(400, "bad_request", "body must be a JSON object");
	}
	const auto &obj = v.get<picojson::object>();

	auto string_field = [&obj](const char *name, std::string &out, bool &present) -> bool {
		auto it = obj.find(name);
		present = (it != obj.end());
		if (!present)
			return true;
		if (!it->second.is<std::string>())
			return false;
		out = it->second.get<std::string>();
		return true;
	};

	std::string current, admin_new, guest_new;
	bool has_current = false, has_admin = false, has_guest = false;
	if (!string_field("current_password", current, has_current) ||
		!string_field("admin_password", admin_new, has_admin) ||
		!string_field("guest_password", guest_new, has_guest)) {
		return ErrorResponse(400, "bad_request", "password fields must be strings");
	}
	if (!has_current) {
		return ErrorResponse(400, "bad_request", "`current_password` is required");
	}

	bool guest_enabled = !m_config.GuestCredential().empty();
	bool has_guest_enabled = false;
	{
		auto it = obj.find("guest_enabled");
		has_guest_enabled = (it != obj.end());
		if (has_guest_enabled) {
			if (!it->second.is<bool>()) {
				return ErrorResponse(400, "bad_request", "`guest_enabled` must be a boolean");
			}
			guest_enabled = it->second.get<bool>();
		}
	}
	// Setting a guest password while switching the role off is
	// contradictory; guessing at which half was meant would silently do
	// the wrong one.
	if (has_guest && !guest_new.empty() && has_guest_enabled && !guest_enabled) {
		return ErrorResponse(400,
			"bad_request",
			"`guest_password` cannot be set together with `guest_enabled: false`");
	}
	// A guest password on its own means "turn guest on with this".
	if (has_guest && !guest_new.empty() && !has_guest_enabled) {
		guest_enabled = true;
	}
	if (!has_admin && !has_guest && !has_guest_enabled) {
		return ErrorResponse(400, "bad_request", "nothing to change");
	}
	// There is no way to clear the admin password; an admin-less daemon
	// bound to a routable address would answer to nobody.
	if (has_admin && admin_new.empty()) {
		return ErrorResponse(400,
			"bad_request",
			"`admin_password` cannot be empty; the admin role cannot be removed");
	}

	const std::string current_md5(
		MD5Sum(wxString::FromUTF8(current.c_str())).GetHash().Lower().utf8_str());
	if (m_config.VerifyPassword(current_md5) != CAmuleApiConfig::MatchedRole::Admin) {
		m_rateLimiter.NoteFailure(ip);
		return ErrorResponse(
			403, "invalid_credentials", "`current_password` is not the admin password");
	}
	m_rateLimiter.NoteSuccess(ip);

	webcommon::CredentialChange change;
	change.guest_enabled = guest_enabled;
	if (has_admin) {
		change.admin_md5 = std::string(
			MD5Sum(wxString::FromUTF8(admin_new.c_str())).GetHash().Lower().utf8_str());
	}
	if (has_guest && !guest_new.empty()) {
		change.guest_md5 = std::string(
			MD5Sum(wxString::FromUTF8(guest_new.c_str())).GetHash().Lower().utf8_str());
	}

	std::string apply_err;
	if (!webcommon::ApplyCredentialChange(
		    std::string(m_config.ConfigDir().utf8_str()), change, apply_err)) {
		return ErrorResponse(500, "internal_error", apply_err.c_str());
	}

	// Pull the new records into memory now, so the response below reports
	// the state that was just written rather than the state before it.
	m_config.ReloadCredentials();

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";

	CJsonWriter w;
	w.BeginObject();
	w.Key("admin_set");
	w.ValueBool(!m_config.AdminCredential().empty());
	w.Key("guest_enabled");
	w.ValueBool(!m_config.GuestCredential().empty());
	// Writing the file invalidated every token issued before it, this
	// caller's included. Re-issue theirs in the same response so the
	// operator who changed the password stays signed in while everyone
	// else is signed out — which is the point of changing it.
	w.Key("other_sessions_revoked");
	w.ValueBool(true);
	BeginSession(req, Role::ADMIN, r, w);
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleStatus(const CHttpServer::Request &req)
{
	// Read endpoints: any authenticated role is enough (admin OR
	// guest). mutating endpoints will gate on `admin` only.
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// Until the refresher has completed at least one tick, the cache
	// is empty. Return 503 with a structured code so clients can
	// retry rather than guessing — saves a round of confused log-
	// reading when the daemon just came up.
	if (auto r = RequireSnapshot(m_state))
		return *r;

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
	// No snapshot timestamp in the envelope: a value that moves every tick
	// changes the body bytes and defeats the ETag, so list endpoints would
	// never see a cache hit. `ec_connected` is the staleness signal, flipping
	// false when the refresher tick fails, and the HTTP `Date:` header
	// carries wall-clock for any consumer that needs it.
	w.Key("ec_connected");
	w.ValueBool(ec);
	(void)ts;

	w.Key("ed2k");
	w.BeginObject();
	w.Key("state");
	w.ValueString(wxString::FromUTF8(s.ed2k_state.c_str()));
	// Positive sense, matching the peer-side high_id on /clients/{ecid}.
	// False for a LowID and while disconnected alike, so read it together
	// with `state` before treating it as a firewall verdict.
	w.Key("high_id");
	w.ValueBool(s.ed2k_high_id);
	// Our server-assigned id; a HighID (>= 16777216) is our public address
	// packed LSB-first, which is where public_ip comes from. Both are 0 /
	// empty while disconnected.
	w.Key("id");
	w.ValueInt(static_cast<int64_t>(s.ed2k_id));
	w.Key("public_ip");
	w.ValueString(wxString::FromUTF8(s.ed2k_public_ip.c_str()));
	// 0 when not connected -- gate on ed2k.state, not on this being nonzero.
	w.Key("connected_since");
	w.ValueInt(static_cast<int64_t>(s.ed2k_connected_since));
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
	// 0 when not connected -- gate on kad.state, not on this being nonzero.
	w.Key("connected_since");
	w.ValueInt(static_cast<int64_t>(s.kad_connected_since));
	// Network rollup — same numbers GET /kad serves under
	// `network.{users,files,nodes}`, written by the same helper so
	// the two endpoints cannot drift. Surfaced here so /status is a
	// one-call dashboard view (matches the RFC contract §4.1
	// `kad.network: {users, files}`; we ship `nodes` too because it
	// costs nothing extra and the desktop GUI shows it in the same
	// place). `k` was snapshotted at the top of the handler in the
	// same shared_lock batch as `s`, so these counters describe the
	// same refresher tick as ed2k.* / speeds.* above.
	WriteKadNetworkObject(w, k);
	w.EndObject();

	w.Key("speeds");
	w.BeginObject();
	w.Key("download_bps");
	w.ValueInt(static_cast<int64_t>(s.download_bps));
	w.Key("upload_bps");
	w.ValueInt(static_cast<int64_t>(s.upload_bps));
	// Additive to the two above, not a subset: amuled counts protocol
	// overhead separately from payload.
	w.Key("download_overhead_bps");
	w.ValueInt(static_cast<int64_t>(s.download_overhead_bps));
	w.Key("upload_overhead_bps");
	w.ValueInt(static_cast<int64_t>(s.upload_overhead_bps));
	w.EndObject();

	// null rather than a number when the daemon has no figure. Emitting the
	// -1 sentinel as an unsigned value would read as 17 exabytes free, and
	// 0 would read as a full disk -- the desktop hides the label for the
	// same reason.
	w.Key("disk");
	w.BeginObject();
	WriteIntOrNull(
		w, "temp_free_bytes", s.temp_free_bytes >= 0, static_cast<std::int64_t>(s.temp_free_bytes));
	WriteIntOrNull(w,
		"incoming_free_bytes",
		s.incoming_free_bytes >= 0,
		static_cast<std::int64_t>(s.incoming_free_bytes));
	w.EndObject();

	w.Key("queue");
	w.BeginObject();
	w.Key("upload_clients_waiting");
	w.ValueInt(static_cast<int64_t>(s.ul_queue_len));
	w.Key("download_sources_total");
	w.ValueInt(static_cast<int64_t>(s.total_src_count));
	w.EndObject();
	// Nickname is a /preferences field, not a /status one.
	w.EndObject();

	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// Write a single download object. Used both inline (in the list
// endpoint, iterated) and as the body of the detail endpoint (bare,
// per Q3). The `include_envelope_keys` flag controls whether we
// emit the snapshot_at envelope around it — list mode wraps in its
// own envelope, detail mode is the bare object.
// The chunk size and the part-count arithmetic both live in State.h now
// (webapi::kPartSizeBytes / webapi::PartCountForSize). They used to be a
// literal here plus six separately open-coded ceiling divisions, which is
// the duplication that mattered; the constant still cannot come from
// `protocol/ed2k/Constants.h` directly, because that header is written
// against amule's legacy typedefs (see the note beside the definition).

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
	const std::uint64_t part_count = webapi::PartCountForSize(f.size);
	std::vector<bool> has_gap(part_count, false);
	const auto &gaps = f.download.decoded_gaps;
	const std::size_t gap_pair_count = gaps.size() / 2;
	for (std::size_t g = 0; g < gap_pair_count; ++g) {
		const std::uint64_t gap_start = gaps[2 * g];
		const std::uint64_t gap_end = gaps[2 * g + 1];
		const std::uint64_t start_idx = gap_start / webapi::kPartSizeBytes;
		const std::uint64_t end_idx = gap_end / webapi::kPartSizeBytes;
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

// Per-part source availability backing the shared "Obtained Parts" bar.
// A complete known file carries its own vector, decoded from the
// EC_TAG_KNOWNFILE tag; a shared partfile is emitted by amuled as
// EC_TAG_PARTFILE only, so its vector lands on the download side. Same
// server-side encoder either way, so the fallback is the same numbers,
// not an approximation.
const std::vector<std::uint16_t> &SharedPartSources(const webapi::FileSnapshot &f)
{
	return f.shared.decoded_part_sources.empty() ? f.download.decoded_part_sources
						     : f.shared.decoded_part_sources;
}

// `parts` for the shared detail endpoint: one `{sources}` per part, in
// file order, always exactly `part_count` long. Deliberately NOT the
// downloads shape -- `state` there encodes local completeness, which is
// meaningless for a share and would invite a progress-bar renderer. The
// caller omits the key entirely when nothing has been decoded yet, so
// "no data" and "no sources anywhere" stay distinguishable.
void WriteSharedAvailabilityParts(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	const std::vector<std::uint16_t> &part_sources = SharedPartSources(f);
	if (part_sources.empty())
		return;
	w.Key("parts");
	w.BeginArray();
	const std::uint64_t part_count = webapi::PartCountForSize(f.size);
	for (std::uint64_t i = 0; i < part_count; ++i) {
		const std::uint16_t sources = (static_cast<std::size_t>(i) < part_sources.size())
						      ? part_sources[static_cast<std::size_t>(i)]
						      : static_cast<std::uint16_t>(0);
		w.BeginObject();
		w.Key("sources");
		w.ValueInt(static_cast<int64_t>(sources));
		w.EndObject();
	}
	w.EndArray();
}

// Emit the `media` object (issue #418) when the file carries probed
// audio/video metadata; nothing at all otherwise. Shared by the download
// and shared detail writers.
void WriteMediaIfPresent(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	if (!f.has_media)
		return;
	w.Key("media");
	w.BeginObject();
	w.Key("length_s");
	w.ValueInt(static_cast<int64_t>(f.media.length_s));
	w.Key("bitrate");
	w.ValueInt(static_cast<int64_t>(f.media.bitrate));
	w.Key("codec");
	w.ValueString(wxString::FromUTF8(f.media.codec.c_str()));
	w.Key("artist");
	w.ValueString(wxString::FromUTF8(f.media.artist.c_str()));
	w.Key("album");
	w.ValueString(wxString::FromUTF8(f.media.album.c_str()));
	w.Key("title");
	w.ValueString(wxString::FromUTF8(f.media.title.c_str()));
	w.EndObject();
}

void WriteDownloadObject(
	CJsonWriter &w, const webapi::FileSnapshot &f, bool include_parts = false, bool detail = false)
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
	w.ValueString(wxString::FromUTF8(f.download.priority.c_str()));
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
	// True while an on-demand Kad notes lookup is in flight (issue #434). Kept in
	// the shared object so list, detail and the SSE download event stay identical;
	// clients can watch download_updated for the start -> finish transition.
	w.Key("kad_comment_search_running");
	w.ValueBool(f.download.kad_comment_searching);
	// On the list, not detail-only: a client rendering a hashing indicator
	// needs it wherever the file appears, and it only moves while a hash is
	// actually running. Parts hashed so far, not an index -- 0 when idle.
	w.Key("hashing_progress");
	w.ValueInt(static_cast<int64_t>(f.download.hashing_progress));
	if (detail) {
		// Detail-only fields (GET /downloads/{hash}); omitted from the
		// list. `part_count` and `remaining_time` are computed here from
		// the snapshot — no EC tag exists for them.
		const std::int64_t part_count = static_cast<std::int64_t>(webapi::PartCountForSize(f.size));
		// ETA seconds, or null when stalled/paused (speed ~0) and there is
		// nothing to compute from. It was -1, which a client had to know
		// meant "unknown" while the neighbouring unknowns on this surface
		// used 0, an omitted key, and null. One rule: nullable field,
		// unknown is null.
		bool has_remaining_time = false;
		std::int64_t remaining_time = 0;
		if (f.download.speed_bps > 0) {
			has_remaining_time = true;
			remaining_time = (f.size > f.download.size_done)
						 ? static_cast<std::int64_t>((f.size - f.download.size_done) /
									     f.download.speed_bps)
						 : 0;
		}
		w.Key("last_seen_complete");
		w.ValueInt(static_cast<int64_t>(f.download.last_seen_complete));
		w.Key("last_changed");
		w.ValueInt(static_cast<int64_t>(f.download.last_changed));
		w.Key("download_active_time");
		w.ValueInt(static_cast<int64_t>(f.download.download_active_time));
		w.Key("available_part_count");
		w.ValueInt(static_cast<int64_t>(f.download.available_part_count));
		w.Key("part_count");
		w.ValueInt(part_count);
		WriteIntOrNull(w, "remaining_time", has_remaining_time, remaining_time);
		w.Key("lost_to_corruption");
		w.ValueInt(static_cast<int64_t>(f.download.lost_to_corruption));
		w.Key("gained_by_compression");
		w.ValueInt(static_cast<int64_t>(f.download.gained_by_compression));
		w.Key("saved_by_ich");
		w.ValueInt(static_cast<int64_t>(f.download.saved_by_ich));
		w.Key("aich_hash");
		w.ValueString(wxString::FromUTF8(f.aich_hash.c_str()));
		w.Key("met_file");
		// The ".part" control-file basename. Empty once the download
		// completes: the daemon then reuses the _FILENAME tag to carry the
		// directory path, so only surface it while still a partfile (#417).
		w.ValueString(f.download.status == "completed"
				      ? wxString()
				      : wxString::FromUTF8(f.part_met_basename.c_str()));
		w.Key("path");
		// The on-disk directory (Temp while downloading, destination once
		// completed) — mirrors the `path` field on /shared/{hash} (#417).
		w.ValueString(wxString::FromUTF8(f.on_disk_dir.c_str()));
		w.Key("partmet_id");
		w.ValueInt(static_cast<int64_t>(f.download.partmet_id));
		w.Key("queued_count");
		w.ValueInt(static_cast<int64_t>(f.queued_count));
		w.Key("comment");
		w.ValueString(wxString::FromUTF8(f.comment.c_str()));
		w.Key("rating");
		w.ValueInt(static_cast<int64_t>(f.rating));
		w.Key("a4af_auto");
		w.ValueBool(f.download.a4af_auto);
		WriteMediaIfPresent(w, f);
	}
	w.EndObject();
}

// Base (list-level) client fields. Emits keys into an already-open
// object (no Begin/End) so both the list writer and the detail writer
// share one definition of the A-field set.
void WriteClientBaseFields(CJsonWriter &w, const webapi::ClientSnapshot &c)
{
	w.Key("ecid");
	w.ValueInt(static_cast<int64_t>(c.ecid));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(c.client_name.c_str()));
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(c.user_hash.c_str()));
	w.Key("ip");
	w.ValueString(wxString::FromUTF8(c.ip.c_str()));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(c.port));
	// ISO 3166-1 alpha-2 (lowercase); "" when GeoIP is off/unresolved (#439).
	w.Key("country_code");
	w.ValueString(wxString::FromUTF8(c.country_code.c_str()));
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
	w.Key("upload_file_name");
	w.ValueString(wxString::FromUTF8(c.upload_file_name.c_str()));
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
	// Promoted out of the detail object (issue #984): the desktop's per-file
	// peer panels render Origin and "Shares File List" as list columns, so a
	// caller should not have to fetch each peer individually to draw a table.
	// Anything added here also reaches the SSE payload -- see ToJson AND Equal
	// in EventDiff.cpp; a field in one but not the other never updates.
	w.Key("source_origin");
	w.ValueString(wxString::FromUTF8(c.source_origin.c_str()));
	w.Key("available_parts");
	w.ValueInt(static_cast<int64_t>(c.available_parts));
	w.Key("mod_version");
	w.ValueString(wxString::FromUTF8(c.mod_version.c_str()));
	w.Key("view_shared_disabled");
	w.ValueBool(c.view_shared_disabled);
	// Omitted rather than sent negative when not computable (no linked
	// download / unknown part count).
	if (c.part_progress_percent >= 0.0) {
		w.Key("part_progress_percent");
		w.ValueDouble(c.part_progress_percent);
	}
}

// List-level client object (GET /clients).
void WriteClientObject(CJsonWriter &w, const webapi::ClientSnapshot &c)
{
	w.BeginObject();
	WriteClientBaseFields(w, c);
	w.EndObject();
}

// One EC_TAG_CLIENT entry of an EC_OP_CLIENT_HISTORY reply.
//
// Tag-absent means "the daemon has no such record for this peer", not "empty":
// a record written before per-peer metadata existed carries only the hash, the
// totals and a last-seen, and the fields below simply stay unset so the writer
// can omit them. The numeric codes go through the same decoders the refresher
// uses for live peers (ClientTagNames.h), so a consumer switching on "kad" or
// "emule" gets the same token whichever endpoint produced it.
webapi::KnownClientSnapshot DecodeKnownClient(const CECTag &entry)
{
	webapi::KnownClientSnapshot c;
	c.user_hash = std::string(entry.GetMD4Data().Encode().Lower().utf8_str());

	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_UPLOAD_TOTAL))
		c.total_uploaded = t->GetInt();
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_DOWNLOAD_TOTAL))
		c.total_downloaded = t->GetInt();
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_LAST_SEEN))
		c.last_seen = static_cast<std::time_t>(t->GetInt());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_FIRST_SEEN))
		c.first_seen = static_cast<std::time_t>(t->GetInt());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_SESSIONS))
		c.sessions = static_cast<std::uint32_t>(t->GetInt());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_NAME))
		c.client_name = std::string(t->GetStringData().utf8_str());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_USER_IP)) {
		const std::uint32_t ip = static_cast<std::uint32_t>(t->GetInt());
		if (ip != 0)
			c.ip = std::string(Uint32toStringIP(ip).utf8_str());
	}
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_USER_PORT))
		c.port = static_cast<std::uint16_t>(t->GetInt());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_KAD_PORT))
		c.kad_port = static_cast<std::uint16_t>(t->GetInt());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_COUNTRY))
		c.country_code = std::string(t->GetStringData().Lower().utf8_str());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_SOFTWARE))
		c.software = webapi::ClientSoftwareName(static_cast<std::uint32_t>(t->GetInt()));
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_SOFT_VER_STR))
		c.version = std::string(t->GetStringData().utf8_str());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_FROM))
		c.source_origin = webapi::SourceOriginName(static_cast<std::uint32_t>(t->GetInt()));
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_OBFUSCATION_STATUS))
		c.obfuscation = webapi::ClientObfuscationName(static_cast<std::uint8_t>(t->GetInt()));
	return c;
}

// One credit-store record (GET /known_clients).
//
// Optional fields are omitted rather than emitted empty: a record written
// before the daemon kept per-peer metadata genuinely has no name, address or
// software, and a consumer should be able to tell "not recorded" from "recorded
// as empty". The hash, the totals and last_seen are always present.
void WriteKnownClientObject(CJsonWriter &w, const webapi::KnownClientSnapshot &c)
{
	w.BeginObject();
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(c.user_hash.c_str()));
	if (!c.client_name.empty()) {
		w.Key("name");
		w.ValueString(wxString::FromUTF8(c.client_name.c_str()));
	}
	if (!c.ip.empty()) {
		w.Key("ip");
		w.ValueString(wxString::FromUTF8(c.ip.c_str()));
		w.Key("port");
		w.ValueInt(static_cast<int64_t>(c.port));
		w.Key("kad_port");
		w.ValueInt(static_cast<int64_t>(c.kad_port));
	}
	if (!c.country_code.empty()) {
		w.Key("country_code");
		w.ValueString(wxString::FromUTF8(c.country_code.c_str()));
	}
	if (!c.software.empty()) {
		w.Key("software");
		w.ValueString(wxString::FromUTF8(c.software.c_str()));
		w.Key("version");
		w.ValueString(wxString::FromUTF8(c.version.c_str()));
	}
	if (!c.source_origin.empty()) {
		w.Key("source_origin");
		w.ValueString(wxString::FromUTF8(c.source_origin.c_str()));
	}
	if (!c.obfuscation.empty()) {
		w.Key("obfuscation");
		w.ValueString(wxString::FromUTF8(c.obfuscation.c_str()));
	}
	w.Key("total_uploaded");
	w.ValueUInt(static_cast<uint64_t>(c.total_uploaded));
	w.Key("total_downloaded");
	w.ValueUInt(static_cast<uint64_t>(c.total_downloaded));
	w.Key("last_seen");
	w.ValueUInt(static_cast<uint64_t>(c.last_seen));
	if (c.first_seen != 0) {
		w.Key("first_seen");
		w.ValueUInt(static_cast<uint64_t>(c.first_seen));
		w.Key("sessions");
		w.ValueUInt(static_cast<uint64_t>(c.sessions));
	}
	// Correlate with /clients by user_hash to reach the live peer.
	w.Key("online");
	w.ValueBool(c.online);
	w.EndObject();
}

// Single-client detail object (GET /clients/{ecid}, issue #422): the
// full A-field set plus the detail-only B fields. A superset of the
// list object, so the list schema is unaffected.
void WriteClientDetailObject(CJsonWriter &w, const webapi::ClientSnapshot &c)
{
	w.BeginObject();
	WriteClientBaseFields(w, c);
	w.Key("user_id_hybrid");
	w.ValueUInt(static_cast<uint64_t>(c.user_id_hybrid));
	w.Key("high_id");
	w.ValueBool(c.high_id);
	w.Key("server_ip");
	w.ValueString(wxString::FromUTF8(c.server_ip.c_str()));
	w.Key("server_port");
	w.ValueInt(static_cast<int64_t>(c.server_port));
	w.Key("server_name");
	w.ValueString(wxString::FromUTF8(c.server_name.c_str()));
	w.Key("kad_port");
	w.ValueInt(static_cast<int64_t>(c.kad_port));
	// Friend status + DL/UP modifier (issue #423). is_friend is
	// friends-list membership, distinct from the friend_slot reserved
	// upload slot above.
	w.Key("is_friend");
	w.ValueBool(c.is_friend);
	w.Key("dl_up_modifier");
	w.ValueDouble(c.dl_up_modifier);
	w.EndObject();
}

// Base shared-file fields, shared by the list writer and the detail
// writer. Emits keys into an already-open object (no Begin/End).
void WriteSharedBaseFields(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	w.Key("hash");
	w.ValueString(wxString::FromUTF8(f.hash.c_str()));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(f.name.c_str()));
	w.Key("ed2k_link");
	w.ValueString(wxString::FromUTF8(f.ed2k_link.c_str()));
	w.Key("size");
	w.ValueInt(static_cast<int64_t>(f.size));
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(f.shared.priority.c_str()));
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
	// Live upload activity (issue #466). `upload_speed_bps` + `uploading`
	// refresh every tick; `last_upload` / `shared_since` are unix seconds,
	// null when unknown -- never uploaded, or a known.met entry that predates
	// the field. They were 0, which reads as 1970 rather than "no idea".
	w.Key("upload_speed_bps");
	w.ValueInt(static_cast<int64_t>(f.shared.upload_speed_bps));
	w.Key("uploading");
	w.ValueInt(static_cast<int64_t>(f.shared.uploading_count));
	WriteIntOrNull(
		w, "last_upload", f.shared.last_upload != 0, static_cast<std::int64_t>(f.shared.last_upload));
	WriteIntOrNull(w,
		"shared_since",
		f.shared.shared_since != 0,
		static_cast<std::int64_t>(f.shared.shared_since));
	// Parts hashed so far by a Verify Local Data or an AICH hashset rebuild
	// over this share; 0 when idle. Goes through the accessor so a shared
	// download, which amuled reports as a partfile, still reads correctly.
	w.Key("hashing_progress");
	w.ValueInt(static_cast<int64_t>(webapi::SharedHashingProgress(f)));
}

void WriteSharedObject(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	w.BeginObject();
	WriteSharedBaseFields(w, f);
	// Media rides the list item, not just the detail endpoint, because the
	// shared_added / shared_updated payload is documented to match this object
	// byte-for-byte -- that is what lets a subscriber skip the re-GET. The
	// event has to carry media (a metadata re-extraction is otherwise
	// invisible: the refresh endpoints answer 202 with no result), so the list
	// carries it too or the guarantee stops being true. Six small scalars,
	// unlike the per-part arrays the list deliberately omits.
	WriteMediaIfPresent(w, f);
	w.EndObject();
}

// Locale-independent file-type token for the shared detail endpoint:
// the desktop's own category label (GetFiletypeByName, untranslated)
// lowercased — e.g. "audio", "video"(s), "cd-images", "any". Reuses the
// GUI categorization rather than duplicating the extension table.
std::string FileTypeToken(const std::string &name)
{
	const wxString desc =
		GetFiletypeByName(CPath(wxString::FromUTF8(name.c_str())), /*translated=*/false);
	std::string s(desc.utf8_str());
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

// GET /shared/{hash} detail: every list field plus the shared-table gaps
// + shared-applicable identity fields. See issue #417 Part B.
void WriteSharedDetailObject(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	w.BeginObject();
	WriteSharedBaseFields(w, f);
	w.Key("file_type");
	w.ValueString(wxString::FromUTF8(FileTypeToken(f.name).c_str()));
	w.Key("share_ratio");
	w.ValueDouble(
		f.size > 0 ? static_cast<double>(f.shared.xfer_total) / static_cast<double>(f.size) : 0.0);
	w.Key("path");
	// The on-disk directory (Temp while downloading, destination once
	// completed) -- the same value /downloads/{hash} reports for this file.
	// This was once masked with a placeholder while the file was incomplete,
	// which hid nothing -- the sibling endpoint served the real value for the
	// same file on the same tick -- and cost clients a usable field.
	// `incomplete` below carries that state explicitly instead.
	w.ValueString(wxString::FromUTF8(f.on_disk_dir.c_str()));
	w.Key("incomplete");
	// Always present, so clients can test it rather than probe for absence.
	// Detail-only, like `path`: the list object and the shared_updated diff
	// deliberately carry neither, so the SSE event rate is unaffected.
	w.ValueBool(f.IsIncompletePartfile());
	w.Key("complete_sources_range");
	w.BeginObject();
	w.Key("low");
	w.ValueInt(static_cast<int64_t>(f.shared.complete_sources_low));
	w.Key("high");
	w.ValueInt(static_cast<int64_t>(f.shared.complete_sources_high));
	w.EndObject();
	w.Key("aich_hash");
	w.ValueString(wxString::FromUTF8(f.aich_hash.c_str()));
	w.Key("part_count");
	w.ValueInt(static_cast<int64_t>(webapi::PartCountForSize(f.size)));
	WriteSharedAvailabilityParts(w, f);
	w.Key("queued_count");
	w.ValueInt(static_cast<int64_t>(f.queued_count));
	w.Key("comment");
	w.ValueString(wxString::FromUTF8(f.comment.c_str()));
	w.Key("rating");
	w.ValueInt(static_cast<int64_t>(f.rating));
	WriteMediaIfPresent(w, f);
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

// Sort keys for peer rows, shared by /clients and by the per-file client
// routes -- the latter derive theirs from this set, so a key added here shows
// up on every peer list rather than only the one it was added to.
// One row of GET /search. The listing is built from a live EC response rather
// than a snapshot vector, so it needs a materialised row before it can go
// through the same envelope every other collection uses. Absent values stay
// absent rather than becoming 0: `started_at` is unknowable for a search this
// process did not start, and `result_count` is unreported by an older daemon,
// which has to stay distinguishable from "found nothing".
struct SearchListRow
{
	std::uint32_t search_id = 0;
	wxString query;
	std::string kind;
	std::string state;
	bool has_client_ecid = false;
	std::uint32_t client_ecid = 0;
	std::time_t started_at = 0; // 0 = not started by this process
	bool has_result_count = false;
	std::uint32_t result_count = 0;
};

void WriteSearchListRow(CJsonWriter &w, const SearchListRow &row)
{
	w.BeginObject();
	w.Key("search_id");
	w.ValueInt(static_cast<int64_t>(row.search_id));
	w.Key("query");
	w.ValueString(row.query);
	w.Key("kind");
	w.ValueString(wxString::FromUTF8(row.kind.c_str()));
	w.Key("state");
	w.ValueString(wxString::FromUTF8(row.state.c_str()));
	if (row.has_client_ecid) {
		w.Key("client_ecid");
		w.ValueInt(static_cast<int64_t>(row.client_ecid));
	}
	if (row.started_at != 0) {
		w.Key("started_at");
		w.ValueInt(static_cast<int64_t>(row.started_at));
	}
	if (row.has_result_count) {
		w.Key("result_count");
		w.ValueInt(static_cast<int64_t>(row.result_count));
	}
	w.EndObject();
}

// Sort keys for /search. The daemon hands its searches back id-ascending and id
// order is not recency (Kad ids carry SEARCH_ID_KAD_MASK and always sort above
// ed2k ones), so a client had nothing to rank the list by.
const ListComparators<SearchListRow> &SearchListComparators()
{
	static const ListComparators<SearchListRow> kComps = {
		{ "search_id",
			[](const SearchListRow &a, const SearchListRow &b) {
				return a.search_id < b.search_id;
			} },
		{ "query", [](const SearchListRow &a, const SearchListRow &b) { return a.query < b.query; } },
		{ "started_at",
			[](const SearchListRow &a, const SearchListRow &b) {
				return a.started_at < b.started_at;
			} },
		{ "result_count",
			[](const SearchListRow &a, const SearchListRow &b) {
				return a.result_count < b.result_count;
			} },
	};
	return kComps;
}

// Sort keys for /categories. The tenth list endpoint was also the only one
// that never parsed ?limit/&offset/&sort/&order, so the same query string was a
// hard error on /downloads and a silent no-op here -- while the response still
// carried the page-meta trio a caller could not influence.
const ListComparators<webapi::CategorySnapshot> &CategoryComparators()
{
	static const ListComparators<webapi::CategorySnapshot> kComps = {
		{ "index",
			[](const webapi::CategorySnapshot &a, const webapi::CategorySnapshot &b) {
				return a.index < b.index;
			} },
		{ "name",
			[](const webapi::CategorySnapshot &a, const webapi::CategorySnapshot &b) {
				return a.name < b.name;
			} },
	};
	return kComps;
}

const ListComparators<webapi::ClientSnapshot> &ClientComparators()
{
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
	return kComps;
}

std::unique_ptr<CHttpServer::Response> BadRequestPtr(const char *message)
{
	return std::make_unique<CHttpServer::Response>(ErrorResponse(400, "bad_request", message));
}

// Optional unsigned query parameter, with an inclusive upper bound.
//
// One parser for every count on the surface. The seven written by hand
// disagreed about the two questions that decide what a client sees -- what an
// unparseable value does, and what an out-of-range one does -- so a typo was a
// hard error on `interval` and a silent behaviour change on `width`, on the
// same endpoint. Rejecting both, here, is the only version of "consistent" a
// tenth call site cannot quietly opt out of, and the status, the code and the
// sentence are contract rather than local wording.
//
// Absent leaves `out` untouched, so the caller's default stands. `min` and
// `max` are inclusive; the running value is bounded inside the loop, so a long
// digit string cannot wrap before the range check sees it.
std::unique_ptr<CHttpServer::Response> ParseUintParam(const std::map<std::string, std::string> &qmap,
	const char *name,
	std::uint64_t min,
	std::uint64_t max,
	std::uint64_t &out)
{
	const auto it = qmap.find(name);
	if (it == qmap.end())
		return nullptr;
	if (!web_api_path::ParseBoundedUint(it->second, min, max, out)) {
		const std::string msg = std::string("`") + name + "` must be an integer between " +
					std::to_string(min) + " and " + std::to_string(max);
		return BadRequestPtr(msg.c_str());
	}
	return nullptr;
}

// Optional boolean query parameter. Accepts 1/0, true/false and yes/no, and
// rejects anything else -- `include_completed` used to read every other value
// as false while its neighbour `include_parts` answered 400, so the same typo
// was silent on one endpoint and fatal on the next.
std::unique_ptr<CHttpServer::Response> ParseBoolParam(
	const std::map<std::string, std::string> &qmap, const char *name, bool &out)
{
	const auto it = qmap.find(name);
	if (it == qmap.end())
		return nullptr;
	if (!web_api_path::ParseBoolValue(it->second, out)) {
		const std::string msg = std::string("`") + name + "` must be 1/0, true/false or yes/no";
		return BadRequestPtr(msg.c_str());
	}
	return nullptr;
}

// Parse ?limit/&offset/&sort/&order from a raw query string. A non-numeric or
// out-of-range `limit`/`offset`, and a bad `order`, are 400s. `sort` is
// validated later against the endpoint's comparator table (BuildListWindow).
std::unique_ptr<CHttpServer::Response> ParseListParams(const std::string &query, ListParams &out)
{
	const auto qmap = web_api_path::ParseQuery(query);
	// 500 is the window cap and 1e9 is well past anything these lists hold;
	// both are now rejections rather than silent clamps, so a client that
	// asks for more learns it did.
	if (qmap.count("limit")) {
		std::uint64_t v = 0;
		if (auto r = ParseUintParam(qmap, "limit", 0, 500, v))
			return r;
		out.has_limit = true;
		out.limit = static_cast<std::size_t>(v);
	}
	{
		std::uint64_t v = 0;
		if (auto r = ParseUintParam(qmap, "offset", 0, 1000000000ull, v))
			return r;
		out.offset = static_cast<std::size_t>(v);
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
std::unique_ptr<CHttpServer::Response> BuildListWindowFromPtrs(std::vector<const T *> &ptrs,
	const ListParams &params,
	const ListComparators<T> &comparators,
	std::vector<const T *> &out_window,
	std::size_t &out_total)
{
	out_total = ptrs.size();

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

// Sort + window a list the caller owns as values. Thin wrapper over the
// pointer form above: an endpoint whose records are not all in one contiguous
// vector -- /known_clients, which serves most rows straight out of a shared
// cache and only materialises the few it has to patch -- addresses that one
// directly instead of copying the whole set to get a vector.
template <class T>
std::unique_ptr<CHttpServer::Response> BuildListWindow(const std::vector<T> &items,
	const ListParams &params,
	const ListComparators<T> &comparators,
	std::vector<const T *> &out_window,
	std::size_t &out_total)
{
	std::vector<const T *> ptrs;
	ptrs.reserve(items.size());
	for (const auto &it : items)
		ptrs.push_back(&it);
	return BuildListWindowFromPtrs(ptrs, params, comparators, out_window, out_total);
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
// Envelope over records the caller addresses as pointers, so an endpoint whose
// rows are not all in one vector does not have to build one. See
// BuildListWindowFromPtrs.
// State-free: takes no lock of its own, so a caller already holding CState's
// read lock can build a response inside it. m_mu is not recursive -- a second
// shared_lock taken while a writer is queued deadlocks -- so anything reached
// from under WithKnownClients() must not touch the state again.
template <class T, class WriterFn>
CHttpServer::Response ListResponseFromPtrsUnlocked(const char *plural_key,
	std::vector<const T *> &ptrs,
	WriterFn write_item,
	const ListParams &params,
	const ListComparators<T> &comparators)
{
	std::vector<const T *> window;
	std::size_t total = 0;
	if (auto err = BuildListWindowFromPtrs(ptrs, params, comparators, window, total))
		return *err;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
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

template <class T, class WriterFn>
CHttpServer::Response ListResponse(const webapi::CState &state,
	const char *plural_key,
	const std::vector<T> &items,
	WriterFn write_item,
	const ListParams &params = ListParams(),
	const ListComparators<T> &comparators = ListComparators<T>())
{
	if (auto r = RequireSnapshot(state))
		return *r;
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

// JSON body parser. Returns true on success; false + `err` on
// failure. Non-object roots are rejected.
//
// Pre-parse depth cap, so a deeply nested body cannot exhaust the
// handler thread's stack inside picojson's recursive descent. The
// scanner and the reasoning behind it live in JsonDepthScan.h, which
// keeps it reachable from JsonDepthScanTest. CJwt::Verify caps its
// own parses for the same reason, by a cruder count that suits the
// payloads it sees -- see the note there.
bool ParseJsonObjectBody(const std::string &body, picojson::value &out, std::string &err)
{
	if (!webapi::JsonNestingWithinLimit(body)) {
		err = "JSON nesting too deep";
		return false;
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
// inverse of DownloadPriorityName in Refresher.cpp). Downloads support
// only LOW/NORMAL/HIGH plus AUTO: CPartFile's .part.met loader clamps
// any download priority that isn't one of those back to PR_NORMAL on
// load (PR_AUTO=5 is the magic value stored as High + the auto flag),
// so `very_low` and `release` are shared/upload-side levels only. They
// are intentionally rejected here so the download PATCH enum matches
// what the daemon can actually hold; the shared side keeps the full
// set in SharedPriorityToCode.
// Returns false if the wire string isn't a known download priority.
// The one place the file-priority vocabulary is declared.
//
// /downloads, /shared and /categories all speak the PR_* code space and differ
// only in which names they accept, which is how three near-identical converters
// came to exist: adding or renaming a level was a multi-site edit with no
// compiler help, and each call site additionally spelled its accepted set out
// again in a rejection string.
//
// The differences in what each accepts are deliberate and stay. The .part.met
// loader clamps anything but PR_LOW/PR_NORMAL/PR_HIGH back to Normal on
// restart, so very_low and release are upload-side levels only and are refused
// on the download path on purpose. Categories apply their priority to member
// files as a download priority, so they inherit the download set.
//
// Servers are deliberately NOT in this table. SRV_PR_* is a different code
// space in which the same word means a different number -- `low` is 0 for a
// file and 2 for a server -- so folding them together would produce one table
// that lies about half its rows. ServerPriorityCode stays where it is.
enum PriorityDomain : unsigned
{
	kPrioDownload = 1u << 0,
	kPrioShared = 1u << 1,
	kPrioCategory = 1u << 2,
};

struct FilePriorityLevel
{
	const char *name;
	std::uint8_t code;
	unsigned domains;
};

const FilePriorityLevel kFilePriorities[] = {
	{ "very_low", PR_VERY_LOW, kPrioShared },
	{ "low", PR_LOW, kPrioDownload | kPrioShared | kPrioCategory },
	{ "normal", PR_NORMAL, kPrioDownload | kPrioShared | kPrioCategory },
	{ "high", PR_HIGH, kPrioDownload | kPrioShared | kPrioCategory },
	{ "release", PR_VERYHIGH, kPrioShared },
	{ "auto", PR_AUTO, kPrioDownload | kPrioShared | kPrioCategory },
};

bool FilePriorityToCode(const std::string &name, unsigned domain, std::uint8_t &out)
{
	for (const FilePriorityLevel &level : kFilePriorities) {
		if ((level.domains & domain) != 0 && name == level.name) {
			out = level.code;
			return true;
		}
	}
	return false;
}

// The rejection names exactly what this domain accepts, built from the table
// rather than restated at each call site -- five sites had their own copy, so a
// new level would have been announced in some of them and not others.
std::string FilePriorityAccepted(unsigned domain)
{
	std::string out;
	for (const FilePriorityLevel &level : kFilePriorities) {
		if ((level.domains & domain) == 0)
			continue;
		if (!out.empty())
			out += ", ";
		out += level.name;
	}
	return "`priority` must be one of " + out;
}

// The three "fetch a list from a URL" endpoints — /servers/update,
// /kad/update and /ipfilter/update — are the same operation over three
// different lists: take one http(s) URL, hand it to amuled in a single
// string tag, echo the effective URL back with a 202. amuled persists the
// URL into the matching preference itself in all three cases, so none of
// them also PATCHes it here.
struct UrlFetchSpec
{
	// JSON body field carrying the URL, and the key echoed in the reply.
	const char *field;
	ec_opcode_t op;
	// Tag the URL travels in. EC_OP_IPFILTER_UPDATE reads the packet's
	// first tag whatever it is named, so that one uses EC_TAG_STRING to
	// match what amulegui has always sent.
	ec_tagname_t tag;
	// false: an absent field falls back to the configured URL the caller
	// passes in, instead of being a 400.
	bool url_required;
	// Run an inline RefresherTick so the caches (and the SSE diff built
	// from them) pick up whatever already landed. Only worth it where the
	// result shows up in a cache this process holds.
	bool refresh_after;
};

// Pull the URL out of the request body per `spec`, falling back to
// `configured` when the body omits it and the spec allows that. Returns
// false with `rejection` filled on any rejection.
//
// `configured` is null when there is no fallback to offer — always for a
// url_required spec, and for the others while amuleapi has no preferences
// snapshot yet. A request that carries its own URL is unaffected either
// way; one that needs the fallback then gets a 503 rather than a 400
// blaming a URL that simply has not been read yet.
bool ResolveFetchUrl(const CHttpServer::Request &req,
	const UrlFetchSpec &spec,
	const std::string *configured,
	std::string &out_url,
	CHttpServer::Response &rejection)
{
	const std::string field = std::string("`") + spec.field + "`";
	bool present = false;
	// An optional-URL endpoint accepts no body at all; a required-URL one
	// needs the object, so let ParseJsonObjectBody produce the error.
	if (spec.url_required || !req.body.empty()) {
		picojson::value root;
		std::string parse_err;
		if (!ParseJsonObjectBody(req.body, root, parse_err)) {
			rejection = ErrorResponse(400, "bad_request", parse_err.c_str());
			return false;
		}
		const auto &obj = root.get<picojson::object>();
		const auto it = obj.find(spec.field);
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				rejection = ErrorResponse(
					400, "bad_request", (field + " must be a string").c_str());
				return false;
			}
			out_url = it->second.get<std::string>();
			present = true;
		}
	}
	if (!present) {
		if (spec.url_required) {
			rejection = ErrorResponse(400,
				"bad_request",
				("required string field " + field + " is missing").c_str());
			return false;
		}
		if (configured == nullptr) {
			rejection = ErrorResponse(
				503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet");
			return false;
		}
		out_url = *configured;
		if (out_url.empty()) {
			rejection = ErrorResponse(400,
				"bad_request",
				(field + " was omitted and no URL is configured").c_str());
			return false;
		}
		// A configured URL predates this request — it came in over
		// PATCH /preferences or amuled's own config file — so it is not
		// re-validated here. Sending it is what the desktop button does.
		return true;
	}
	if (out_url.empty()) {
		rejection = ErrorResponse(400, "bad_request", (field + " must not be empty").c_str());
		return false;
	}
	// Light hygiene check — amuled hands the string straight to the HTTP
	// downloader, so a bad scheme would otherwise fail asynchronously with
	// nowhere to report it. Rejecting here also gives a clearer error than
	// the EC "amuled rejected" wrapper.
	if (out_url.compare(0, 7, "http://") != 0 && out_url.compare(0, 8, "https://") != 0) {
		rejection = ErrorResponse(
			400, "bad_request", (field + " must be an http:// or https:// URL").c_str());
		return false;
	}
	return true;
}

// Send the EC op for one resolved URL and build the 202 echo.
CHttpServer::Response UrlFetchOp(
	CamuleapiApp &app, webapi::CState &state, const UrlFetchSpec &spec, const std::string &url)
{
	auto ec_req = std::make_unique<CECPacket>(spec.op);
	ec_req->AddTag(CECTag(spec.tag, wxString::FromUTF8(url.c_str())));
	const CECPacket *ec_resp = app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed");
	}
	std::string ec_err;
	if (IsEcFailedResponse(ec_resp, ec_err)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err.c_str());
	}
	delete ec_resp;

	if (spec.refresh_after) {
		(void)RefresherTick(app, state);
	}

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key(spec.field);
	w.ValueString(wxString::FromUTF8(url.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
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
	auto a = Authenticate(req);
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
		if (auto r = ParseBoolParam(qmap, "include_completed", include_completed))
			return *r;
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
	if (auto r = RequireSnapshot(m_state))
		return *r;
	// Pointers into the live map rather than copies; see HandleSharedList.
	CHttpServer::Response resp;
	// Named captures; see HandleSharedList.
	m_state.WithFiles([&resp, &params, include_completed](const webapi::FileMap &files) {
		std::vector<const webapi::FileSnapshot *> ptrs;
		ptrs.reserve(files.size());
		for (const auto &entry : files) {
			const webapi::FileSnapshot &d = entry.second;
			if (!d.is_downloading)
				continue;
			if (!include_completed && d.download.status == "completed")
				continue;
			ptrs.push_back(&d);
		}
		resp = ListResponseFromPtrsUnlocked(
			"downloads",
			ptrs,
			[](CJsonWriter &w, const webapi::FileSnapshot &d) {
				// List mode — omit `progress.parts` (Q2 + the per-list
				// shape: omitting parts keeps the list response compact,
				// detail endpoint is where parts ship).
				WriteDownloadObject(w, d, /*include_parts=*/false);
			},
			params,
			kComps);
	});
	return resp;
}

namespace
{
// One peer row of a per-file client list: the ordinary /clients object plus the
// three things that only make sense relative to a file.
struct FileClientRow
{
	webapi::ClientSnapshot client;
	std::string role; // "source" | "peer" | "both" | "none"
	bool a4af = false;
	std::vector<bool> parts;
	bool has_parts = false;
};

// Sort keys, derived from the /clients set rather than restated, so the two
// surfaces cannot drift apart.
const ListComparators<FileClientRow> &FileClientComparators()
{
	static const ListComparators<FileClientRow> kComps = [] {
		ListComparators<FileClientRow> out;
		for (const auto &kv : ClientComparators()) {
			auto fn = kv.second;
			out.emplace_back(kv.first, [fn](const FileClientRow &a, const FileClientRow &b) {
				return fn(a.client, b.client);
			});
		}
		return out;
	}();
	return kComps;
}

void WriteFileClientRow(CJsonWriter &w, const FileClientRow &row)
{
	w.BeginObject();
	WriteClientBaseFields(w, row.client);
	// The peer's relation to THIS file, which the global /clients row cannot
	// express: "source" serves it to us, "peer" pulls it from us, "both" does
	// each way, "none" is a row that exists only because it is parked here as
	// an A4AF source.
	w.Key("role");
	w.ValueString(wxString::FromUTF8(row.role.c_str()));
	// Orthogonal to role on purpose: a peer can be parked on another file and
	// still be pulling this one from us, which a fourth role value could not
	// represent.
	w.Key("a4af");
	w.ValueBool(row.a4af);
	if (row.has_parts) {
		w.Key("parts");
		w.BeginArray();
		for (const bool b : row.parts) {
			w.ValueBool(b);
		}
		w.EndArray();
	}
	w.EndObject();
}

// Resolve one peer's bitmap for `part_count` chunks of the file this row is
// about, following the two wire conventions: an "all" flag means the core sent
// an empty tag for a full source, and a decoded bitmap that cannot cover the
// file is dropped rather than padded (its tail beyond part_count is the
// buffer's byte padding, which is fine to trim).
bool ResolvePartBitmap(const std::vector<bool> &bits,
	bool all,
	bool present,
	std::uint64_t part_count,
	std::vector<bool> &out)
{
	if (!present || part_count == 0) {
		return false;
	}
	if (all) {
		out.assign(static_cast<std::size_t>(part_count), true);
		return true;
	}
	if (bits.size() < part_count) {
		return false;
	}
	out.assign(bits.begin(), bits.begin() + static_cast<std::ptrdiff_t>(part_count));
	return true;
}
} // namespace

// Serves both /downloads/{hash}/clients and /shared/{hash}/clients. The rows
// are the same object out of the same cache either way -- the relation to the
// file is a field, not a path -- so the two routes differ only in which role
// the hash must already have, which is what makes each a sub-resource of its
// own collection. A partfile with at least one completed chunk is in both
// collections at once and answers identically on both.
CHttpServer::Response CApiDispatcher::HandleFileClients(
	const CHttpServer::Request &req, const std::string &key, bool require_downloading)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto r = RequireSnapshot(m_state))
		return *r;

	const std::string needle = LowerHexKey(key);

	webapi::FileSnapshot file;
	const bool found =
		require_downloading ? m_state.FindDownload(needle, file) : m_state.FindShared(needle, file);
	if (!found) {
		return ErrorResponse(404,
			"not_found",
			require_downloading ? "no download with that hash" : "no shared file with that hash");
	}

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;

	bool include_parts = false;
	{
		const auto qmap = web_api_path::ParseQuery(QueryOf(req));
		if (auto r = ParseBoolParam(qmap, "include_parts", include_parts))
			return *r;
	}

	const std::uint64_t part_count = webapi::PartCountForSize(file.size);
	const auto &a4af_sources = file.download.a4af_sources;

	std::vector<FileClientRow> rows;
	for (auto client : m_state.Clients()) {
		const bool is_source = (client.download_file_hash == needle);
		const bool is_peer = (client.upload_file_hash == needle);
		const bool is_a4af = std::find(a4af_sources.begin(), a4af_sources.end(), client.ecid) !=
				     a4af_sources.end();
		if (!is_source && !is_peer && !is_a4af) {
			continue;
		}

		FileClientRow row;
		row.role =
			is_source && is_peer ? "both" : (is_source ? "source" : (is_peer ? "peer" : "none"));
		row.a4af = is_a4af;
		ComputePartProgressPercent(m_state, client);
		if (include_parts) {
			// Which bitmap belongs to this row follows its direction: the
			// download map describes the file we pull from the peer, the
			// upload map the file it pulls from us, and for a "both" row
			// those are this same file seen from each side. A pure A4AF row
			// has no bitmap for this file at all.
			if (is_source) {
				row.has_parts = ResolvePartBitmap(client.part_status,
					client.part_status_all,
					client.has_part_status,
					part_count,
					row.parts);
			} else if (is_peer) {
				row.has_parts = ResolvePartBitmap(client.upload_part_status,
					client.upload_part_status_all,
					client.has_upload_part_status,
					part_count,
					row.parts);
			}
		}
		row.client = std::move(client);
		rows.push_back(std::move(row));
	}

	return ListResponse(m_state, "clients", rows, WriteFileClientRow, params, FileClientComparators());
}

CHttpServer::Response CApiDispatcher::HandleClients(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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

	// Same derived field the per-file rows and the detail object carry. It
	// was omitted here, which left the SSE payload (which always carries it)
	// contradicting both EVENTS.md's "same field set as the /clients list
	// row" and REFERENCE.md's claim that it is detail-only.
	for (auto &client : clients) {
		ComputePartProgressPercent(m_state, client);
	}

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	return ListResponse(m_state, "clients", clients, WriteClientObject, params, ClientComparators());
}

CHttpServer::Response CApiDispatcher::HandleSharedList(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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
	if (auto r = RequireSnapshot(m_state))
		return *r;
	// Pointers into the live map rather than copies -- this was the single
	// biggest allocation the daemon made per request. Serialising inside the
	// read lock is safe because WriteSharedObject reads only the snapshot it is
	// handed, so ListResponseFromPtrsUnlocked's "must not re-enter CState"
	// contract holds.
	CHttpServer::Response resp;
	// Named captures rather than [&]: [&] would pull `this` in, putting
	// m_state within reach of a callback that must not touch it. kComps is
	// static, so it is in scope without being captured.
	m_state.WithFiles([&resp, &params](const webapi::FileMap &files) {
		std::vector<const webapi::FileSnapshot *> ptrs;
		ptrs.reserve(files.size());
		for (const auto &entry : files) {
			if (entry.second.is_shared)
				ptrs.push_back(&entry.second);
		}
		resp = ListResponseFromPtrsUnlocked("shared", ptrs, WriteSharedObject, params, kComps);
	});
	return resp;
}

CHttpServer::Response CApiDispatcher::HandleDownloadDetail(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	// {hash} is the 32-char lowercase-hex MD4. URL is case-tolerant;
	// State writes lowercase, so we down-case the capture before the
	// O(1) m_hash_to_ecid lookup.
	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
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
	WriteDownloadObject(w, d, /*include_parts=*/true, /*detail=*/true);
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSharedDetail(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	// {hash} is the 32-char MD4; URL is case-tolerant, State keys are
	// lowercase — down-case before the O(1) lookup (mirrors the download
	// detail handler).
	webapi::FileSnapshot s;
	if (!FindSharedByKey(m_state, key, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}

	// Bare object (the detail resource itself), same shape contract as
	// GET /downloads/{hash}: every GET /shared list field plus the
	// Part-B detail fields.
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteSharedDetailObject(w, s);
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleDownloadComments(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("count");
	w.ValueInt(static_cast<int64_t>(d.download.source_comments.size()));
	// True while an on-demand Kad notes lookup is in flight (issue #434); poll
	// this endpoint until it flips back to false to observe retrieved notes.
	w.Key("kad_comment_search_running");
	w.ValueBool(d.download.kad_comment_searching);
	w.Key("comments");
	w.BeginArray();
	for (const auto &c : d.download.source_comments) {
		w.BeginObject();
		w.Key("username");
		w.ValueString(wxString::FromUTF8(c.username.c_str()));
		w.Key("filename");
		w.ValueString(wxString::FromUTF8(c.filename.c_str()));
		w.Key("rating");
		w.ValueInt(static_cast<int64_t>(c.rating));
		w.Key("comment");
		w.ValueString(wxString::FromUTF8(c.comment.c_str()));
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// POST /downloads/{hash}/comments — trigger an on-demand Kad NOTES lookup for
// this download (issue #434). The lookup is asynchronous on amuled (up to ~45s);
// retrieved community ratings/comments subsequently appear via GET on the same
// path, alongside per-source comments. Returns 202 Accepted.
CHttpServer::Response CApiDispatcher::HandleDownloadCommentsKadSearch(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// Admin-only, like every other mutation. This drives an unbounded Kad
	// NOTES lookup on the daemon, so a guest session must not reach it.
	if (auto r = RequireAdmin(a))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	CMD4Hash file_hash;
	if (!HashFromHex(d.hash, file_hash)) {
		return ErrorResponse(500, "internal_error", "failed to decode file hash");
	}

	auto ec_req = std::make_unique<CECPacket>(EC_OP_SHARED_FILE_SEARCH_KAD_NOTES);
	ec_req->AddTag(CECTag(EC_TAG_KNOWNFILE, file_hash));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SEARCH_KAD_NOTES");
	}
	std::string ec_err;
	if (IsEcFailedResponse(ec_resp, ec_err)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err.c_str());
	}
	delete ec_resp;

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("status");
	w.ValueString("kad_search_started");
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleDownloadFilenames(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("filenames");
	w.BeginArray();
	for (const auto &kv : d.download.source_names) {
		w.BeginObject();
		w.Key("name");
		w.ValueString(wxString::FromUTF8(kv.second.name.c_str()));
		w.Key("count");
		w.ValueInt(static_cast<int64_t>(kv.second.count));
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// Serialize the A4AF view (auto flag + source client ECIDs) for a
// resolved download snapshot.
namespace
{

// Defined further down beside its FindServerByEcid / FindFriendByEcid
// siblings; declared here so the A4AF handlers below can validate a
// `client_ecid` without moving the helper away from its family or copying it.
bool FindClientByEcid(const webapi::CState &state, std::uint32_t ecid, webapi::ClientSnapshot &out);

void WriteA4afObject(CJsonWriter &w, const webapi::FileSnapshot &d)
{
	w.BeginObject();
	w.Key("a4af_auto");
	w.ValueBool(d.download.a4af_auto);
	w.Key("source_ecids");
	w.BeginArray();
	for (const std::uint32_t ecid : d.download.a4af_sources) {
		w.ValueInt(static_cast<int64_t>(ecid));
	}
	w.EndArray();
	w.EndObject();
}
} // namespace

CHttpServer::Response CApiDispatcher::HandleDownloadA4afAction(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();
	const auto ait = obj.find("action");
	if (ait == obj.end() || !ait->second.is<std::string>()) {
		return ErrorResponse(400, "bad_request", "request body must include a string `action`");
	}
	const std::string &action = ait->second.get<std::string>();
	ec_opcode_t op;
	if (action == "swap_this")
		op = EC_OP_PARTFILE_SWAP_A4AF_THIS;
	else if (action == "swap_this_auto")
		op = EC_OP_PARTFILE_SWAP_A4AF_THIS_AUTO;
	else if (action == "swap_others")
		op = EC_OP_PARTFILE_SWAP_A4AF_OTHERS;
	else {
		return ErrorResponse(
			400, "bad_request", "`action` must be one of swap_this, swap_this_auto, swap_others");
	}

	// `client_ecid` narrows swap_this from "every A4AF source of this file" to
	// one named source, which is what the desktop's per-peer "Swap to this
	// file" does. The core has no per-source form of the other two actions, so
	// pairing it with them is a request that cannot be honoured rather than one
	// that quietly does something else.
	bool per_source = false;
	std::uint32_t client_ecid = 0;
	if (const auto cit = obj.find("client_ecid"); cit != obj.end()) {
		if (!cit->second.is<double>() || cit->second.get<double>() < 0) {
			return ErrorResponse(
				400, "bad_request", "`client_ecid` must be a non-negative integer");
		}
		if (op != EC_OP_PARTFILE_SWAP_A4AF_THIS) {
			return ErrorResponse(
				400, "bad_request", "`client_ecid` is only valid with action `swap_this`");
		}
		per_source = true;
		client_ecid = static_cast<std::uint32_t>(cit->second.get<double>());

		webapi::ClientSnapshot peer;
		if (!FindClientByEcid(m_state, client_ecid, peer)) {
			return ErrorResponse(
				404, "not_found", "no client with that ECID in the current snapshot");
		}
		const auto &srcs = d.download.a4af_sources;
		if (std::find(srcs.begin(), srcs.end(), client_ecid) == srcs.end()) {
			return ErrorResponse(
				409, "conflict", "that client is not an A4AF source of this download");
		}
	}

	CMD4Hash file_hash;
	if (!HashFromHex(d.hash, file_hash)) {
		return ErrorResponse(500, "internal_error", "failed to decode partfile hash");
	}
	std::unique_ptr<CECPacket> ec_req(new CECPacket(per_source ? EC_OP_CLIENT_SWAP_TO_ANOTHER_FILE : op));
	if (per_source) {
		ec_req->AddTag(CECTag(EC_TAG_CLIENT, client_ecid));
	}
	ec_req->AddTag(CECTag(EC_TAG_PARTFILE, file_hash));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for A4AF swap");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void)RefresherTick(m_app, m_state);
	webapi::FileSnapshot d_after = d;
	(void)m_state.FindDownload(d.hash, d_after);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteA4afObject(w, d_after);
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

CHttpServer::Response CApiDispatcher::HandleVersionCheck(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// Before the first EC snapshot there are no preferences to read, and
	// version_check_available defaults to false -- so the capability check
	// below used to answer 409 "disabled or unavailable on the connected
	// daemon" during the window every client hits at startup, blaming the
	// daemon's configuration for amuleapi not having read it yet. Every other
	// handler that reads cached state answers 503 ec_unavailable there, which
	// is the condition a client can retry.
	if (auto r = RequireSnapshot(m_state))
		return *r;

	// The daemon owns the check; amuleapi only triggers it. Reject early
	// when the daemon can't check, so we never send an EC op that will fail
	// and never expose the daemon's localized reason (English-only contract).
	const auto prefs = m_state.Preferences();
	if (!(prefs.version_check_available && prefs.check_new_version)) {
		return ErrorResponse(409,
			"update_check_unavailable",
			"version check is disabled or unavailable on the connected daemon");
	}

	auto ec_req = std::make_unique<CECPacket>(EC_OP_VERSION_CHECK);
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for version check");
	}
	// The daemon replies EC_OP_NOOP (accepted) or EC_OP_FAILED (throttled).
	const bool failed = ec_resp->GetOpCode() == EC_OP_FAILED;
	delete ec_resp;
	if (failed) {
		// The only expected failure past the gate above is the daemon's
		// throttle. Report an English code; the daemon's message is not relayed.
		return ErrorResponse(429,
			"update_check_throttled",
			"version check was throttled by the daemon; try again shortly");
	}

	// Accepted. The check runs asynchronously on the daemon; the result
	// (latest_version / update_available / last_checked) appears on a
	// subsequent GET /api/v0/version once it completes.
	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("status");
	w.ValueString("started");
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleDownloadAdd(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

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

namespace
{
// Handle the optional `comment`+`rating` pair shared by PATCH
// /downloads/{hash} and PATCH /shared/{hash} (issue #419). Both must be
// present together or neither. Sends EC_OP_SHARED_FILE_SET_COMMENT, which
// amuled resolves against the shared-files registry — so the file must be
// shared. Returns false and fills `err` on any problem; sets `applied`
// when a valid pair was written. A body with neither field is a no-op
// (returns true, applied=false).
bool TrySetCommentRating(CamuleapiApp &app,
	const picojson::object &obj,
	const webapi::FileSnapshot &f,
	bool &applied,
	CHttpServer::Response &err)
{
	applied = false;
	const auto cit = obj.find("comment");
	const auto rit = obj.find("rating");
	const bool has_c = cit != obj.end();
	const bool has_r = rit != obj.end();
	if (!has_c && !has_r)
		return true;
	if (has_c != has_r) {
		err = ErrorResponse(400, "bad_request", "`comment` and `rating` must be set together");
		return false;
	}
	if (!cit->second.is<std::string>()) {
		err = ErrorResponse(400, "bad_request", "`comment` must be a string");
		return false;
	}
	const std::string comment = cit->second.get<std::string>();
	// MAXFILECOMMENTLEN (include/protocol/ed2k/Constants.h) = 50.
	if (comment.size() > 50) {
		err = ErrorResponse(400, "bad_request", "`comment` exceeds 50 characters");
		return false;
	}
	if (!rit->second.is<double>()) {
		err = ErrorResponse(400, "bad_request", "`rating` must be an integer in [0, 5]");
		return false;
	}
	const double rd = rit->second.get<double>();
	const int rating = static_cast<int>(rd);
	if (static_cast<double>(rating) != rd || rating < 0 || rating > 5) {
		err = ErrorResponse(400, "bad_request", "`rating` must be an integer in [0, 5]");
		return false;
	}
	if (!f.is_shared) {
		err = ErrorResponse(409, "not_shared", "comment and rating can only be set on a shared file");
		return false;
	}
	CMD4Hash file_hash;
	if (!HashFromHex(f.hash, file_hash)) {
		err = ErrorResponse(500, "internal_error", "failed to decode file hash");
		return false;
	}
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SHARED_FILE_SET_COMMENT));
	ec_req->AddTag(CECTag(EC_TAG_KNOWNFILE, file_hash));
	ec_req->AddTag(CECTag(EC_TAG_KNOWNFILE_COMMENT, comment));
	ec_req->AddTag(CECTag(EC_TAG_KNOWNFILE_RATING, static_cast<std::uint8_t>(rating)));
	const CECPacket *ec_resp = app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		err = ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SET_COMMENT");
		return false;
	}
	std::string ec_err;
	if (IsEcFailedResponse(ec_resp, ec_err)) {
		delete ec_resp;
		err = ErrorResponse(400, "amuled_rejected", ec_err.c_str());
		return false;
	}
	delete ec_resp;
	applied = true;
	return true;
}

// Handle the optional `name` (rename) field shared by PATCH
// /downloads/{hash} and PATCH /shared/{hash} (issue #420). Maps to
// EC_OP_RENAME_FILE. Rejects empty names and names containing path
// separators — amuled's RenameFile JoinPaths()es the value, so a
// separator would let the rename escape the file's directory. Returns
// false + fills `err` on a problem; sets `applied` when a rename was
// sent. Absent `name` is a no-op (returns true, applied=false).
bool TryRename(CamuleapiApp &app,
	const picojson::object &obj,
	const webapi::FileSnapshot &f,
	bool &applied,
	CHttpServer::Response &err)
{
	applied = false;
	const auto nit = obj.find("name");
	if (nit == obj.end())
		return true;
	if (!nit->second.is<std::string>()) {
		err = ErrorResponse(400, "bad_request", "`name` must be a string");
		return false;
	}
	const std::string name = nit->second.get<std::string>();
	if (name.empty()) {
		err = ErrorResponse(400, "bad_request", "`name` must not be empty");
		return false;
	}
	if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
		err = ErrorResponse(400, "bad_request", "`name` must not contain path separators");
		return false;
	}
	CMD4Hash file_hash;
	if (!HashFromHex(f.hash, file_hash)) {
		err = ErrorResponse(500, "internal_error", "failed to decode file hash");
		return false;
	}
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_RENAME_FILE));
	ec_req->AddTag(CECTag(EC_TAG_KNOWNFILE, file_hash));
	ec_req->AddTag(CECTag(EC_TAG_PARTFILE_NAME, name));
	const CECPacket *ec_resp = app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		err = ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for RENAME_FILE");
		return false;
	}
	std::string ec_err;
	if (IsEcFailedResponse(ec_resp, ec_err)) {
		delete ec_resp;
		err = ErrorResponse(400, "amuled_rejected", ec_err.c_str());
		return false;
	}
	delete ec_resp;
	applied = true;
	return true;
}
} // namespace

CHttpServer::Response CApiDispatcher::HandleDownloadPatch(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
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

	// status: "paused" | "resumed" | "stopped"
	{
		const auto it = obj.find("status");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400,
					"bad_request",
					"`status` must be one of \"paused\", \"resumed\" or \"stopped\"");
			}
			const std::string &v = it->second.get<std::string>();
			ec_opcode_t op;
			if (v == "paused")
				op = EC_OP_PARTFILE_PAUSE;
			else if (v == "resumed")
				op = EC_OP_PARTFILE_RESUME;
			else if (v == "stopped")
				op = EC_OP_PARTFILE_STOP;
			else {
				return ErrorResponse(400,
					"bad_request",
					"`status` must be one of \"paused\", \"resumed\" or \"stopped\"");
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
			if (!FilePriorityToCode(it->second.get<std::string>(), kPrioDownload, code)) {
				return ErrorResponse(
					400, "bad_request", FilePriorityAccepted(kPrioDownload).c_str());
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

	// comment + rating (both required together; issue #419). Only
	// settable on a file that is shared (a downloading partfile with
	// ≥1 complete chunk); otherwise TrySetCommentRating returns 409.
	{
		bool applied = false;
		CHttpServer::Response cr_err;
		if (!TrySetCommentRating(m_app, obj, d, applied, cr_err))
			return cr_err;
		if (applied)
			any_change = true;
	}

	// name (rename; issue #420).
	{
		bool applied = false;
		CHttpServer::Response rn_err;
		if (!TryRename(m_app, obj, d, applied, rn_err))
			return rn_err;
		if (applied)
			any_change = true;
	}

	if (!any_change) {
		return ErrorResponse(400,
			"bad_request",
			"request body must include at least one of "
			"`status`, `priority`, `category`, `comment`+`rating`, or `name`");
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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

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
		// Named captures; see HandleSharedList.
		m_state.WithFiles([&ecids, &hashes_cleared](const webapi::FileMap &files) {
			for (const auto &entry : files) {
				const webapi::FileSnapshot &d = entry.second;
				if (d.is_downloading && d.download.status == "completed") {
					ecids.push_back(d.ecid);
					hashes_cleared.push_back(d.hash);
				}
			}
		});
	}

	if (ecids.empty()) {
		// Nothing to do. An empty `results` array is the no-op, and it stays
		// distinguishable from "amuled rejected" -- which is a 4xx with an
		// error envelope -- without needing a shape of its own.
		return BulkResultsResponse({}, 200);
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

	// One entry per hash, in the envelope every other multi-item mutation
	// uses. `cleared` was a count and `cleared_hashes` a bare array, so a
	// per-entry failure had nowhere to appear; the daemon clears them in one
	// roundtrip today, but the shape no longer assumes that.
	std::vector<BulkItem> results;
	results.reserve(hashes_cleared.size());
	for (const auto &h : hashes_cleared) {
		results.push_back(BulkOk(h));
	}
	return BulkResultsResponse(results, 200);
}

// --- /servers, /kad, /categories, /preferences -------------------------

namespace
{

// Dotted-quad IPv4 -> host-order uint32, the encoding EC_TAG_*_IP uses.
// Three call sites parsed this inline with the same sscanf; the friends add
// path made it four.
bool ParseIpv4Dotted(const std::string &text, std::uint32_t &out_he)
{
	unsigned a = 0, b = 0, c = 0, d = 0;
	if (std::sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return false;
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return false;
	out_he = (a) | (b << 8) | (c << 16) | (d << 24);
	return true;
}

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
	// ISO 3166-1 alpha-2 (lowercase); "" when GeoIP is off/unresolved (#440).
	w.Key("country_code");
	w.ValueString(wxString::FromUTF8(s.country_code.c_str()));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(s.port));
	w.Key("users");
	w.ValueInt(static_cast<int64_t>(s.users));
	w.Key("max_users");
	w.ValueInt(static_cast<int64_t>(s.max_users));
	w.Key("files");
	w.ValueInt(static_cast<int64_t>(s.files));
	// 0 means the server has not reported a limit yet, not a limit of zero;
	// the sentinel is documented so a UI can render it blank the way the
	// desktop's Soft/Hard Files columns do.
	w.Key("soft_file_limit");
	w.ValueInt(static_cast<int64_t>(s.soft_file_limit));
	w.Key("hard_file_limit");
	w.ValueInt(static_cast<int64_t>(s.hard_file_limit));
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(s.priority.c_str()));
	w.Key("ping_ms");
	w.ValueInt(static_cast<int64_t>(s.ping_ms));
	w.Key("failed_count");
	w.ValueInt(static_cast<int64_t>(s.failed_count));
	w.Key("static");
	w.ValueBool(s.is_static);
	// Decoded capability bits. Written as a pre-built fragment from the shared
	// tables so this object and the SSE payload in EventDiff.cpp -- two
	// different writers, one documented shape -- cannot drift apart.
	w.Key("tcp_flags");
	w.ValueRaw(webapi::ServerTcpFlagsJson(s.tcp_flags));
	w.Key("udp_flags");
	w.ValueRaw(webapi::ServerUdpFlagsJson(s.udp_flags));
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

void WriteFriendObject(CJsonWriter &w, const webapi::FriendSnapshot &f)
{
	w.BeginObject();
	// The friend's own EC handle, so it is spelled `ecid` like every other
	// self-handle; `client_ecid` below points OUT of this object, which is
	// what the prefix is for. Like every ECID it does not survive an amuled
	// restart -- `user_hash` is the durable reference, when the friend has one.
	w.Key("ecid");
	w.ValueInt(static_cast<int64_t>(f.ecid));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(f.name.c_str()));
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(f.user_hash.c_str()));
	w.Key("ip");
	w.ValueString(wxString::FromUTF8(f.ip.c_str()));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(f.port));
	// The live peer this friend is linked to, joinable against /clients. 0
	// when the friend is not connected, which is what `online` reports.
	w.Key("client_ecid");
	w.ValueInt(static_cast<int64_t>(f.client_ecid));
	w.Key("online");
	w.ValueBool(f.client_ecid != 0);
	w.Key("friend_slot");
	w.ValueBool(f.friend_slot);
	w.EndObject();
}

CHttpServer::Response CApiDispatcher::HandleServers(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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

// Path-capture counterpart to RequireSnapshot above, used the same way:
// ` if (auto r = RequireEcidPath(caps["ecid"], ecid)) return *r;`.
//
// Nine handlers had spelled out the same parse-and-reject, and the status,
// the code and the sentence are part of the API contract rather than local
// wording -- a tenth route inheriting a different one is how a surface stops
// being predictable.
std::unique_ptr<CHttpServer::Response> RequireEcidPath(const std::string &s, std::uint32_t &out)
{
	if (!ParseEcidPath(s, out)) {
		return BadRequestPtr("path `{ecid}` must be a non-negative integer");
	}
	return nullptr;
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

// Look up a client in the State cache by ECID (issue #422). Mirrors
// FindServerByEcid; the handler 404s on false.
bool FindClientByEcid(const webapi::CState &state, std::uint32_t ecid, webapi::ClientSnapshot &out)
{
	const auto all = state.Clients();
	for (const auto &c : all) {
		if (c.ecid == ecid) {
			out = c;
			return true;
		}
	}
	return false;
}

// Same shape again for friends. The EC remove/slot ops are idempotent about
// unknown ids, but a REST caller who mistypes one should get a 404 rather than
// a cheerful 200, so every /friends/{ecid} handler looks the id up first.
// The category set as a client sees it, which is not quite what amuled holds.
//
// amuled's EC suppresses the whole `EC_TAG_PREFS_CATEGORIES` block when no
// custom categories exist, and starts including index 0 once the first custom
// one is added. Faithful at the wire layer, but a client iterating /categories
// expecting at least the default has to special-case the empty case, so a
// synthetic index-0 entry is injected when missing. The defaults mirror what
// amuled emits for category 0 itself: empty title/path/comment, color 0,
// priority_code PR_LOW (the amuled default for `defaultcat->prio` in
// CPreferences::LoadCats).
//
// Both read routes go through here. The collection did this inline and the
// member route added later read the raw snapshot instead, so /categories
// listed a category 0 that /categories/0 reported as absent. Mutations
// deliberately do NOT use this: the synthetic entry is a read-shape
// convenience rather than something the daemon holds, so there is nothing
// there to PATCH or DELETE.
std::vector<webapi::CategorySnapshot> CategoriesWithDefault(const webapi::CState &state)
{
	std::vector<webapi::CategorySnapshot> cats = state.Categories();
	for (const auto &c : cats) {
		if (c.index == 0) {
			return cats;
		}
	}
	webapi::CategorySnapshot d;
	d.index = 0;
	d.priority_code = 0; // PR_LOW (matches amuled default)
	d.priority = "low";
	cats.insert(cats.begin(), std::move(d));
	return cats;
}

// Category counterpart to the FindXByEcid family above. Three handlers walked
// m_state.Categories() with the same loop, and the read route added below would
// have been a fourth.
bool FindCategoryByIndex(const webapi::CState &state, std::uint8_t index, webapi::CategorySnapshot &out)
{
	for (const auto &c : state.Categories()) {
		if (static_cast<std::uint8_t>(c.index) == index) {
			out = c;
			return true;
		}
	}
	return false;
}

bool FindFriendByEcid(const webapi::CState &state, std::uint32_t ecid, webapi::FriendSnapshot &out)
{
	const auto all = state.Friends();
	for (const auto &f : all) {
		if (f.ecid == ecid) {
			out = f;
			return true;
		}
	}
	return false;
}

} // namespace

// GET /api/v0/clients/{ecid} (issue #422) — the full detail object for
// one peer: every list field plus the detail-only B fields. Bare
// object (no list envelope), mirroring HandleDownloadDetail. 404 when
// the ecid isn't in the current snapshot.
// --- Chat (issue #971) -------------------------------------------------
//
// Conversations are keyed on "<ip>:<port>", the readable form of the GUI_ID
// the wire already uses. Stable across peer reconnects (unlike an ECID),
// needs no invented identifier, and converts straight back to the GUI_ID the
// EC ops want.

namespace
{

// Parse "<ip>:<port>" back into a GUI_ID. Strict on purpose: anything that is
// not four dotted octets plus a port is a client bug, and accepting it would
// address some other conversation.
bool ParseChatPeerKey(const std::string &peer, std::uint64_t &out_gui_id)
{
	const std::size_t colon = peer.rfind(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 >= peer.size())
		return false;

	unsigned a = 0, b = 0, c = 0, d = 0;
	char extra = 0;
	if (std::sscanf(peer.substr(0, colon).c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
		return false;
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return false;

	const std::string port_str = peer.substr(colon + 1);
	if (port_str.find_first_not_of("0123456789") != std::string::npos)
		return false;
	const unsigned long port = std::strtoul(port_str.c_str(), nullptr, 10);
	if (port == 0 || port > 65535)
		return false;

	// LSB-first, matching IPv4ToDotted and EC_TAG_CLIENT_USER_IP.
	const std::uint32_t ip = static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) |
				 (static_cast<std::uint32_t>(c) << 16) |
				 (static_cast<std::uint32_t>(d) << 24);
	out_gui_id = (static_cast<std::uint64_t>(ip) << 16) | static_cast<std::uint64_t>(port);
	return true;
}

void WriteChatMessageObject(CJsonWriter &w, const webapi::ChatMessageSnapshot &m)
{
	w.BeginObject();
	w.Key("id");
	w.ValueInt(static_cast<int64_t>(m.id));
	w.Key("direction");
	// "in" = from the peer, "out" = sent by us from ANY client: this API,
	// amulegui, or the local GUI.
	w.ValueString(wxString::FromAscii(m.outgoing ? "out" : "in"));
	w.Key("text");
	w.ValueString(wxString::FromUTF8(m.text.c_str()));
	w.Key("timestamp");
	w.ValueInt(static_cast<int64_t>(m.timestamp));
	w.EndObject();
}

void WriteChatObject(CJsonWriter &w, const webapi::ChatSessionSnapshot &s)
{
	w.BeginObject();
	w.Key("peer");
	w.ValueString(wxString::FromUTF8(s.PeerKey().c_str()));
	w.Key("ip");
	w.ValueString(wxString::FromUTF8(s.ip.c_str()));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(s.port));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(s.DisplayName().c_str()));
	w.Key("client_ecid");
	w.ValueInt(static_cast<int64_t>(s.client_ecid));
	w.Key("friend_ecid");
	w.ValueInt(static_cast<int64_t>(s.friend_ecid));
	w.Key("online");
	w.ValueBool(s.client_ecid != 0);
	w.Key("message_count");
	w.ValueInt(static_cast<int64_t>(s.messages.size()));
	w.Key("last_msg_id");
	w.ValueInt(static_cast<int64_t>(s.LastMsgId()));
	w.Key("last_message_at");
	w.ValueInt(static_cast<int64_t>(s.messages.empty() ? 0 : s.messages.back().timestamp));
	// The transcript itself is deliberately NOT on the list: a 50-session
	// store at 200 messages each would be 10 000 objects per list read.
	if (!s.messages.empty()) {
		w.Key("last_message");
		WriteChatMessageObject(w, s.messages.back());
	}
	w.EndObject();
}

const webapi::ChatSessionSnapshot *FindChat(
	const std::vector<webapi::ChatSessionSnapshot> &chats, std::uint64_t gui_id)
{
	for (const webapi::ChatSessionSnapshot &s : chats) {
		if (s.gui_id == gui_id)
			return &s;
	}
	return nullptr;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleKnownClients(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// Never sent blind: a daemon predating EC_OP_GET_CLIENT_HISTORY reaches
	// the unknown-opcode branch of ProcessRequest2(), which asserts before it
	// reaches the EC_OP_FAILED it would otherwise answer with -- so simply
	// trying the request takes the core down.
	if (!m_app.IsServerClientHistoryActive()) {
		return ErrorResponse(
			503, "ec_unsupported", "the connected amuled does not serve the client history");
	}

	if (auto r = RequireSnapshot(m_state))
		return *r;

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;

	// One fetch per process. From here the refresher maintains it: every tick
	// folds the connected peers back in, which is the whole of what can
	// change -- a record whose peer is away cannot move, since credit totals
	// only grow during a transfer and last-seen is written at disconnect. The
	// remaining difference a re-read would show is the expiry prune the core
	// applies when *it* starts, and amuleapi does not outlive a core restart.
	//
	// Two concurrent first requests can both fetch; the second simply replaces
	// the first with an equivalent store. Not worth a lock held across an EC
	// roundtrip to avoid.
	if (!m_state.KnownClientsLoaded()) {
		std::vector<webapi::KnownClientSnapshot> rows;
		std::unique_ptr<CECPacket> req_ec(new CECPacket(EC_OP_GET_CLIENT_HISTORY));
		const CECPacket *resp = m_app.SendRecvSerialized(req_ec.get());
		if (!resp) {
			return ErrorResponse(503, "ec_unavailable", "the EC connection is unavailable");
		}
		const bool got_history = resp->GetOpCode() == EC_OP_CLIENT_HISTORY;
		if (got_history) {
			rows.reserve(resp->GetTagCount());
			for (const CECTag &entry : *resp) {
				if (entry.GetTagName() != EC_TAG_CLIENT)
					continue;
				rows.push_back(DecodeKnownClient(entry));
			}
		}
		delete resp;
		if (!got_history) {
			// An answer we cannot read latches nothing: the store is loaded
			// once and never re-read, so installing an empty one here would
			// serve an empty history for the life of the process. There is no
			// reachable path today -- the daemon always answers this opcode
			// and older ones are refused above -- but the cost of being wrong
			// is permanent, and retrying next request is free.
			return ErrorResponse(502,
				"bad_gateway",
				"the core answered the history request with an unknown reply");
		}
		m_state.SetKnownClients(std::move(rows));
	}

	static const ListComparators<webapi::KnownClientSnapshot> kComps = {
		{ "name",
			[](const webapi::KnownClientSnapshot &a, const webapi::KnownClientSnapshot &b) {
				return a.client_name < b.client_name;
			} },
		{ "software",
			[](const webapi::KnownClientSnapshot &a, const webapi::KnownClientSnapshot &b) {
				return a.software < b.software;
			} },
		{ "first_seen",
			[](const webapi::KnownClientSnapshot &a, const webapi::KnownClientSnapshot &b) {
				return a.first_seen < b.first_seen;
			} },
		{ "last_seen",
			[](const webapi::KnownClientSnapshot &a, const webapi::KnownClientSnapshot &b) {
				return a.last_seen < b.last_seen;
			} },
		{ "sessions",
			[](const webapi::KnownClientSnapshot &a, const webapi::KnownClientSnapshot &b) {
				return a.sessions < b.sessions;
			} },
		{ "total_uploaded",
			[](const webapi::KnownClientSnapshot &a, const webapi::KnownClientSnapshot &b) {
				return a.total_uploaded < b.total_uploaded;
			} },
		{ "total_downloaded",
			[](const webapi::KnownClientSnapshot &a, const webapi::KnownClientSnapshot &b) {
				return a.total_downloaded < b.total_downloaded;
			} },
	};

	// Built under the state's read lock: the store is never copied out, so the
	// response is written straight from it.
	CHttpServer::Response r;
	m_state.WithKnownClients([&](const std::vector<webapi::KnownClientSnapshot> &rows) {
		std::vector<const webapi::KnownClientSnapshot *> ptrs;
		ptrs.reserve(rows.size());
		for (const auto &rec : rows)
			ptrs.push_back(&rec);
		r = ListResponseFromPtrsUnlocked(
			"known_clients", ptrs, WriteKnownClientObject, params, kComps);
	});
	return r;
}

CHttpServer::Response CApiDispatcher::HandleClientDetail(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;
	if (auto r = RequireSnapshot(m_state))
		return *r;
	webapi::ClientSnapshot cli;
	if (!FindClientByEcid(m_state, ecid, cli)) {
		return ErrorResponse(404, "not_found", "no client with that ECID in the current snapshot");
	}

	ComputePartProgressPercent(m_state, cli);

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteClientDetailObject(w, cli);
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleChats(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (!m_app.IsServerChatActive()) {
		return ErrorResponse(
			503, "ec_unsupported", "the connected amuled does not serve chat sessions");
	}
	// RequireSnapshot is ListResponse's job; params first so a malformed
	// query is a 400 rather than a 503 while EC is still warming up.
	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;

	// Served straight from the refresher snapshot -- no EC roundtrip per
	// request. The daemon's own order is most-recently-active first, which is
	// the order a client wants by default.
	const std::vector<webapi::ChatSessionSnapshot> chats = m_state.Chats();

	static const ListComparators<webapi::ChatSessionSnapshot> kComps = {
		{ "last_message_at",
			[](const webapi::ChatSessionSnapshot &x, const webapi::ChatSessionSnapshot &y) {
				const std::uint32_t xa = x.messages.empty() ? 0 : x.messages.back().timestamp;
				const std::uint32_t ya = y.messages.empty() ? 0 : y.messages.back().timestamp;
				return xa < ya;
			} },
		{ "name",
			[](const webapi::ChatSessionSnapshot &x, const webapi::ChatSessionSnapshot &y) {
				return x.DisplayName() < y.DisplayName();
			} },
	};
	return ListResponse(m_state, "chats", chats, WriteChatObject, params, kComps);
}

CHttpServer::Response CApiDispatcher::HandleChatMessages(
	const CHttpServer::Request &req, const std::string &peer)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (!m_app.IsServerChatActive()) {
		return ErrorResponse(
			503, "ec_unsupported", "the connected amuled does not serve chat sessions");
	}
	if (auto r = RequireSnapshot(m_state))
		return *r;

	std::uint64_t gui_id = 0;
	if (!ParseChatPeerKey(peer, gui_id)) {
		return ErrorResponse(400, "bad_request", "path `{peer}` must be `<ip>:<port>`");
	}

	const std::vector<webapi::ChatSessionSnapshot> chats = m_state.Chats();
	const webapi::ChatSessionSnapshot *session = FindChat(chats, gui_id);
	if (!session) {
		return ErrorResponse(404, "not_found", "no chat session with that peer");
	}

	// `since_id` is a safe polling cursor: ids are monotonic per daemon
	// process, so a client never sees a duplicate and never skips one. They
	// reset when the daemon restarts, which also empties the store.
	std::uint32_t since_id = 0;
	std::size_t tail = 0;
	const auto qmap = web_api_path::ParseQuery(QueryOf(req));
	{
		std::uint64_t v = since_id;
		if (auto r = ParseUintParam(qmap, "since_id", 0, 0xFFFFFFFFull, v))
			return *r;
		since_id = static_cast<std::uint32_t>(v);
	}
	{
		// `tail`, not `limit`. This selects the last N of the window
		// rather than a page of it, which is what the log endpoints
		// already call `tail` -- naming it `limit` gave the surface two
		// meanings for one word, and the paginated one is the meaning a
		// client meets on nine other collections.
		std::uint64_t v = tail;
		if (auto r = ParseUintParam(qmap, "tail", 0, 100000, v))
			return *r;
		tail = static_cast<std::size_t>(v);
	}

	std::vector<const webapi::ChatMessageSnapshot *> selected;
	for (const webapi::ChatMessageSnapshot &m : session->messages) {
		if (m.id > since_id)
			selected.push_back(&m);
	}
	// `tail` means the LAST n, matching "show me the tail of this
	// conversation"; combined with since_id it trims the same window from the
	// front, so the newest are always the ones kept.
	if (tail && selected.size() > tail) {
		selected.erase(selected.begin(), selected.end() - static_cast<std::ptrdiff_t>(tail));
	}

	CJsonWriter w;
	w.BeginObject();
	w.Key("peer");
	w.ValueString(wxString::FromUTF8(session->PeerKey().c_str()));
	w.Key("messages");
	w.BeginArray();
	for (const webapi::ChatMessageSnapshot *m : selected)
		WriteChatMessageObject(w, *m);
	w.EndArray();
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(session->messages.size()));
	w.Key("last_msg_id");
	w.ValueInt(static_cast<int64_t>(session->LastMsgId()));
	w.EndObject();
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	FinalizeJsonBody(w, r);
	return r;
}

// Shared by all three send forms. `target` is the already-built EC tag naming
// the recipient -- a GUI_ID, a live peer's ECID, or a friend's ECID. The
// friend form is the one that reaches an OFFLINE friend, resolved by the
// daemon through the friend's stored ip:port.
CHttpServer::Response CApiDispatcher::SendChatMessageTo(const CHttpServer::Request &req, const CECTag &target)
{
	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();
	const auto it = obj.find("text");
	if (it == obj.end() || !it->second.is<std::string>()) {
		return ErrorResponse(400, "bad_request", "required string field `text` is missing");
	}
	const std::string text = it->second.get<std::string>();
	if (text.empty()) {
		return ErrorResponse(400, "bad_request", "`text` must be a non-empty string");
	}
	const std::size_t kMaxChatText = 1024;
	if (text.size() > kMaxChatText) {
		return ErrorResponse(400, "bad_request", "`text` exceeds 1024 bytes");
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_CHAT_SEND));
	ec_req->AddTag(CECTag(EC_TAG_CHAT, wxString::FromUTF8(text.c_str())));
	ec_req->AddTag(target);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for chat send");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(404, "not_found", ec_err_msg.c_str());
	}
	std::uint64_t gui_id = 0;
	std::uint32_t msg_id = 0;
	if (const CECTag *t = ec_resp->GetTagByName(EC_TAG_CHAT_CLIENT_ID))
		gui_id = t->GetInt();
	if (const CECTag *t = ec_resp->GetTagByName(EC_TAG_CHAT_MSG_ID))
		msg_id = static_cast<std::uint32_t>(t->GetInt());
	delete ec_resp;

	// 202, not 200: the core acknowledges that it queued the message on the
	// peer connection, not that the peer received it. An unreachable peer is
	// not an error -- the desktop behaves the same, optimistically printing
	// *** Connecting to Client ***.
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("peer");
	w.ValueString(wxString::FromUTF8(webapi::ChatPeerKeyFromGuiId(gui_id).c_str()));
	w.Key("message");
	w.BeginObject();
	w.Key("id");
	w.ValueInt(static_cast<int64_t>(msg_id));
	w.Key("direction");
	w.ValueString(wxString::FromAscii("out"));
	w.Key("text");
	w.ValueString(wxString::FromUTF8(text.c_str()));
	w.EndObject();
	w.EndObject();
	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleChatSend(const CHttpServer::Request &req, const std::string &peer)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_app.IsServerChatActive()) {
		return ErrorResponse(
			503, "ec_unsupported", "the connected amuled does not serve chat sessions");
	}
	std::uint64_t gui_id = 0;
	if (!ParseChatPeerKey(peer, gui_id)) {
		return ErrorResponse(400, "bad_request", "path `{peer}` must be `<ip>:<port>`");
	}
	// No 404 for an unknown peer here: the core creates the session if it does
	// not exist, so this doubles as "start a chat with this address".
	return SendChatMessageTo(req, CECTag(EC_TAG_CHAT_CLIENT_ID, gui_id));
}

CHttpServer::Response CApiDispatcher::HandleFriendMessageSend(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_app.IsServerChatActive()) {
		return ErrorResponse(
			503, "ec_unsupported", "the connected amuled does not serve chat sessions");
	}
	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;
	return SendChatMessageTo(req, CECTag(EC_TAG_FRIEND, ecid));
}

CHttpServer::Response CApiDispatcher::HandleClientMessageSend(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_app.IsServerChatActive()) {
		return ErrorResponse(
			503, "ec_unsupported", "the connected amuled does not serve chat sessions");
	}
	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;
	return SendChatMessageTo(req, CECTag(EC_TAG_CLIENT, ecid));
}

CHttpServer::Response CApiDispatcher::HandleChatClose(
	const CHttpServer::Request &req, const std::string &peer)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (!m_app.IsServerChatActive()) {
		return ErrorResponse(
			503, "ec_unsupported", "the connected amuled does not serve chat sessions");
	}
	std::uint64_t gui_id = 0;
	if (!ParseChatPeerKey(peer, gui_id)) {
		return ErrorResponse(400, "bad_request", "path `{peer}` must be `<ip>:<port>`");
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_CHAT_CLOSE_SESSION));
	ec_req->AddTag(CECTag(EC_TAG_CHAT_CLIENT_ID, gui_id));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for chat close");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(404, "not_found", ec_err_msg.c_str());
	}
	delete ec_resp;

	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("peer");
	w.ValueString(wxString::FromUTF8(peer.c_str()));
	w.EndObject();
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleFriends(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	static const ListComparators<webapi::FriendSnapshot> kComps = {
		{ "name",
			[](const webapi::FriendSnapshot &a, const webapi::FriendSnapshot &b) {
				return a.name < b.name;
			} },
		{ "online",
			[](const webapi::FriendSnapshot &a, const webapi::FriendSnapshot &b) {
				return (a.client_ecid != 0) < (b.client_ecid != 0);
			} },
	};
	// Served from the refresher snapshot: the friends list rides along with
	// every GET_UPDATE, so this costs no EC roundtrip of its own.
	return ListResponse(m_state, "friends", m_state.Friends(), WriteFriendObject, params, kComps);
}

CHttpServer::Response CApiDispatcher::HandleFriendAdd(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	const bool has_client = obj.find("client_ecid") != obj.end();
	const bool has_manual = obj.find("ip") != obj.end() || obj.find("port") != obj.end() ||
				obj.find("user_hash") != obj.end();
	if (has_client && has_manual) {
		return ErrorResponse(400,
			"bad_request",
			"`client_ecid` and the ip/port/user_hash form are mutually exclusive");
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_FRIEND));
	CECEmptyTag addtag(EC_TAG_FRIEND_ADD);

	if (has_client) {
		// Promote a connected peer, the desktop's "Add to Friends" item.
		const auto &v = obj.at("client_ecid");
		if (!v.is<double>() || v.get<double>() < 0) {
			return ErrorResponse(
				400, "bad_request", "`client_ecid` must be a non-negative integer");
		}
		const std::uint32_t client_ecid = static_cast<std::uint32_t>(v.get<double>());
		webapi::ClientSnapshot peer;
		if (!FindClientByEcid(m_state, client_ecid, peer)) {
			return ErrorResponse(404, "not_found", "no connected client with that `client_ecid`");
		}
		addtag.AddTag(CECTag(EC_TAG_CLIENT, client_ecid));
	} else {
		// Manual add, the desktop's "Add a Friend" dialog. The EC handler
		// wants all four tags present, so an omitted hash is sent empty and
		// an omitted name defaults to the address -- same as the dialog.
		std::string ip_str;
		{
			const auto it = obj.find("ip");
			if (it == obj.end() || !it->second.is<std::string>()) {
				return ErrorResponse(
					400, "bad_request", "required string field `ip` is missing");
			}
			ip_str = it->second.get<std::string>();
		}
		std::uint32_t ip_he = 0;
		if (!ParseIpv4Dotted(ip_str, ip_he) || ip_he == 0) {
			return ErrorResponse(
				400, "bad_request", "`ip` must be a non-zero dotted IPv4 address");
		}
		std::uint16_t port = 0;
		{
			const auto it = obj.find("port");
			if (it == obj.end() || !it->second.is<double>()) {
				return ErrorResponse(
					400, "bad_request", "required integer field `port` is missing");
			}
			const double d = it->second.get<double>();
			if (d <= 0 || d > 65535) {
				return ErrorResponse(400, "bad_request", "`port` must be in 1..65535");
			}
			port = static_cast<std::uint16_t>(d);
		}
		CMD4Hash hash;
		{
			const auto it = obj.find("user_hash");
			if (it != obj.end()) {
				if (!it->second.is<std::string>()) {
					return ErrorResponse(
						400, "bad_request", "`user_hash` must be a string");
				}
				const std::string h = it->second.get<std::string>();
				if (!h.empty() && !hash.Decode(wxString::FromUTF8(h.c_str()))) {
					return ErrorResponse(400,
						"bad_request",
						"`user_hash` must be 32 hexadecimal characters");
				}
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
		if (name.empty()) {
			name = ip_str;
		}
		addtag.AddTag(CECTag(EC_TAG_FRIEND_HASH, hash));
		addtag.AddTag(CECTag(EC_TAG_FRIEND_IP, ip_he));
		addtag.AddTag(CECTag(EC_TAG_FRIEND_PORT, port));
		addtag.AddTag(CECTag(EC_TAG_FRIEND_NAME, wxString::FromUTF8(name.c_str())));
	}
	ec_req->AddTag(addtag);

	std::set<std::uint32_t> before;
	for (const auto &f : m_state.Friends())
		before.insert(f.ecid);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for FRIEND add");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// The daemon applies the add synchronously but the record only reaches us
	// on a GET_UPDATE, so tick before answering and return the real object.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	r.status = 201;
	r.content_type = "application/json";
	CJsonWriter w;
	// The daemon does not echo the new friend's ECID, so identify it as the
	// one the pre-add snapshot did not have. Diffing beats picking the
	// highest id: nothing promises ECIDs are handed out in ascending order,
	// and a concurrent removal could free a lower one for reuse.
	webapi::FriendSnapshot added;
	bool found = false;
	for (const auto &f : m_state.Friends()) {
		if (before.find(f.ecid) == before.end()) {
			added = f;
			found = true;
			break;
		}
	}
	if (found) {
		WriteFriendObject(w, added);
	} else {
		w.BeginObject();
		w.Key("ok");
		w.ValueBool(true);
		w.EndObject();
	}
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleFriendRemove(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;
	webapi::FriendSnapshot existing;
	if (!FindFriendByEcid(m_state, ecid, existing)) {
		return ErrorResponse(404, "not_found", "no friend with that ecid");
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_FRIEND));
	CECEmptyTag removetag(EC_TAG_FRIEND_REMOVE);
	removetag.AddTag(CECTag(EC_TAG_FRIEND, ecid));
	ec_req->AddTag(removetag);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for FRIEND remove");
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

CHttpServer::Response CApiDispatcher::HandleFriendPatch(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;
	webapi::FriendSnapshot existing;
	if (!FindFriendByEcid(m_state, ecid, existing)) {
		return ErrorResponse(404, "not_found", "no friend with that ecid");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();
	const auto it = obj.find("friend_slot");
	if (it == obj.end() || !it->second.is<bool>()) {
		return ErrorResponse(400, "bad_request", "required boolean field `friend_slot` is missing");
	}
	const bool slot = it->second.get<bool>();

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_FRIEND));
	CECTag slottag(EC_TAG_FRIEND_FRIENDSLOT, slot);
	slottag.AddTag(CECTag(EC_TAG_FRIEND, ecid));
	ec_req->AddTag(slottag);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for FRIEND slot");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Only one friend can hold the slot, so granting it here clears whoever
	// held it before -- the tick picks up both changes and both emit an SSE
	// event, not just the friend named in the URL.
	(void)RefresherTick(m_app, m_state);

	webapi::FriendSnapshot updated;
	if (!FindFriendByEcid(m_state, ecid, updated)) {
		return ErrorResponse(404, "not_found", "friend disappeared while setting the slot");
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteFriendObject(w, updated);
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleServerAdd(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;
	if (auto r = RequireSnapshot(m_state))
		return *r;
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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;
	if (auto r = RequireSnapshot(m_state))
		return *r;
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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// amuled streams the new server list into its CServerList
	// asynchronously over the next few ticks (download + parse + merge
	// in CServerList::UpdateServerMetFromURL). The inline RefresherTick
	// grabs whatever's already there; the `server_added` SSE events keep
	// firing on subsequent natural ticks as more entries land.
	static const UrlFetchSpec kSpec = {
		"servers_url", EC_OP_SERVER_UPDATE_FROM_URL, EC_TAG_SERVERS_UPDATE_URL, true, true
	};
	std::string url;
	CHttpServer::Response rejection;
	if (!ResolveFetchUrl(req, kSpec, nullptr, url, rejection)) {
		return rejection;
	}
	return UrlFetchOp(m_app, m_state, kSpec, url);
}

// One "<ip>:<port>" selector, parsed. Split out from the lookup so a
// selector that cannot be parsed is distinguishable from one that parses
// but names no server we know -- those are a 400 and a 404, and the old
// single-uint32 return collapsed them, and every other failure, onto 0.
// Separate also so the parsing is testable without a populated cache.
struct IpPortSelector
{
	std::uint32_t ip_he; // host order, as ServerSnapshot::ip holds it
	std::uint16_t port;
};

// Accepts a dotted quad and a port in 1..65535, nothing else.
//
// Hostname forms are deliberately rejected: an earlier version fell back to
// matching ServerSnapshot::address, which amuled fills from the wire "name"
// tag -- that can be a hostname OR a synthetic display string ("Eserver
// No.1"), so a DELETE by address could match a colliding label and remove
// the wrong row. An exact IP + port has no such ambiguity.
// boost::optional, not std::optional: the tree builds as C++14 and this file
// already uses the boost form (PreflightEvents), so it is the house spelling
// as well as the available one.
boost::optional<IpPortSelector> ParseIpPortSelector(const std::string &ip_port)
{
	const auto colon = ip_port.rfind(':');
	if (colon == std::string::npos)
		return boost::none;

	const std::string ip_str = ip_port.substr(0, colon);
	const std::string port_str = ip_port.substr(colon + 1);
	if (ip_str.empty() || port_str.empty())
		return boost::none;

	char *end = nullptr;
	const unsigned long port = std::strtoul(port_str.c_str(), &end, 10);
	if (end == port_str.c_str() || *end != '\0' || port == 0 || port > 0xFFFF)
		return boost::none;

	// ParseIpv4Dotted rather than a second sscanf of our own: it landed on
	// master while this was in review and does exactly this job, with three
	// other callers already relying on it.
	//
	// Only genuine syntax failures are reported here. 0.0.0.0 parses fine
	// and is rejected by the caller instead, so the two get error messages
	// that describe what actually happened.
	IpPortSelector sel;
	sel.ip_he = 0;
	if (!ParseIpv4Dotted(ip_str, sel.ip_he))
		return boost::none;

	sel.port = static_cast<std::uint16_t>(port);
	return sel;
}

// Resolve a selector to a server ECID, in the same shape as RequireAdmin
// and RequireSnapshot: nullptr means @a ecid is set and the caller may
// proceed, otherwise it is the response to return. Mapping the outcomes
// here rather than at each call site is what keeps the three
// /servers/{ip:port} routes from drifting apart, and no caller compares an
// ECID against a magic value any more.
std::unique_ptr<CHttpServer::Response> ResolveServerEcid(
	const webapi::CState &state, const std::string &ip_port, std::uint32_t &ecid)
{
	const auto sel = ParseIpPortSelector(ip_port);
	if (!sel) {
		return std::make_unique<CHttpServer::Response>(ErrorResponse(400,
			"bad_request",
			"malformed ip:port selector: expected a dotted quad and a port in 1..65535"));
	}
	// 0.0.0.0 is well-formed but is not a server address, and it must not be
	// allowed to reach the lookup below: a ServerSnapshot whose
	// EC_TAG_SERVER_IP the daemon did not ship keeps `ip == 0`
	// (Refresher.cpp, which notes that `address` then reads "0.0.0.0:0"), so
	// a 0.0.0.0 selector would otherwise resolve to whichever such row
	// happened to share the port -- acting on a server the caller never
	// named. Rejected with its own message rather than folded into the
	// syntax error above, which would claim a malformed quad that the caller
	// can see is not malformed.
	if (sel->ip_he == 0) {
		return std::make_unique<CHttpServer::Response>(
			ErrorResponse(400, "bad_request", "0.0.0.0 is not a server address"));
	}

	for (const auto &s : state.Servers()) {
		if (s.port == sel->port && s.ip == sel->ip_he) {
			ecid = s.ecid;
			return nullptr;
		}
	}
	return std::make_unique<CHttpServer::Response>(
		ErrorResponse(404, "not_found", "no server matches that ip:port"));
}

CHttpServer::Response CApiDispatcher::HandleServerConnectByAddress(
	const CHttpServer::Request &req, const std::string &ip_port)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto r = RequireSnapshot(m_state))
		return *r;
	std::uint32_t ecid = 0;
	if (auto r = ResolveServerEcid(m_state, ip_port, ecid))
		return *r;
	// Delegate to the ECID-keyed handler; passing the resolved ECID as
	// a decimal string keeps the contract uniform.
	std::ostringstream os;
	os << ecid;
	return HandleServerConnect(req, os.str());
}

// PATCH /servers/{ecid} — priority and/or static flag (#692).
//
// EC_OP_SERVER_SET_STATIC_PRIO carries EC_TAG_SERVER as a plain ECID integer,
// unlike EC_OP_SERVER_REMOVE next door which carries an EC_IPv4_t. It also
// applies each of EC_TAG_SERVER_PRIO / EC_TAG_SERVER_STATIC only when present,
// so a partial update is native to the wire and this handler simply forwards
// whichever fields the body carried.
//
// amuled answers EC_OP_NOOP whether or not the ECID resolved, so an unknown
// server is indistinguishable from success at the EC layer — the 404 has to
// come from checking the snapshot here.
CHttpServer::Response CApiDispatcher::HandleServerPatch(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	std::uint32_t prio_code = 0;
	bool has_prio = false;
	if (const auto it = obj.find("priority"); it != obj.end()) {
		if (!it->second.is<std::string>()) {
			return ErrorResponse(400, "bad_request", "`priority` must be a string");
		}
		if (!webapi::ServerPriorityCode(it->second.get<std::string>(), prio_code)) {
			return ErrorResponse(
				400, "bad_request", "`priority` must be one of low, normal, high");
		}
		has_prio = true;
	}

	bool is_static = false;
	bool has_static = false;
	if (const auto it = obj.find("static"); it != obj.end()) {
		if (!it->second.is<bool>()) {
			return ErrorResponse(400, "bad_request", "`static` must be a bool");
		}
		is_static = it->second.get<bool>();
		has_static = true;
	}

	if (!has_prio && !has_static) {
		return ErrorResponse(
			400, "bad_request", "body must include at least one of `priority`, `static`");
	}

	if (auto r = RequireSnapshot(m_state))
		return *r;
	webapi::ServerSnapshot srv;
	if (!FindServerByEcid(m_state, ecid, srv)) {
		return ErrorResponse(404, "not_found", "no server with that ECID in the current snapshot");
	}

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SERVER_SET_STATIC_PRIO));
	ec_req->AddTag(CECTag(EC_TAG_SERVER, ecid));
	if (has_prio) {
		ec_req->AddTag(CECTag(EC_TAG_SERVER_PRIO, static_cast<std::uint8_t>(prio_code)));
	}
	if (has_static) {
		ec_req->AddTag(CECTag(EC_TAG_SERVER_STATIC, static_cast<std::uint8_t>(is_static ? 1 : 0)));
	}

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SERVER_SET_STATIC_PRIO");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// Inline refresh so the next GET /servers and the SSE stream both show
	// the new values, matching the sibling server mutations.
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

// Address-keyed alias for the above, mirroring the connect / delete pair.
CHttpServer::Response CApiDispatcher::HandleServerPatchByAddress(
	const CHttpServer::Request &req, const std::string &ip_port)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto r = RequireSnapshot(m_state))
		return *r;
	std::uint32_t ecid = 0;
	if (auto r = ResolveServerEcid(m_state, ip_port, ecid))
		return *r;
	std::ostringstream os;
	os << ecid;
	return HandleServerPatch(req, os.str());
}

CHttpServer::Response CApiDispatcher::HandleServerDeleteByAddress(
	const CHttpServer::Request &req, const std::string &ip_port)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto r = RequireSnapshot(m_state))
		return *r;
	std::uint32_t ecid = 0;
	if (auto r = ResolveServerEcid(m_state, ip_port, ecid))
		return *r;
	std::ostringstream os;
	os << ecid;
	return HandleServerDelete(req, os.str());
}

CHttpServer::Response CApiDispatcher::HandleCategories(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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
	std::vector<webapi::CategorySnapshot> cats = CategoriesWithDefault(m_state);
	ListParams params;
	if (auto r = ParseListParams(QueryOf(req), params))
		return *r;
	return ListResponse(m_state, "categories", cats, WriteCategoryObject, params, CategoryComparators());
}

CHttpServer::Response CApiDispatcher::HandleKad(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto r = RequireSnapshot(m_state))
		return *r;

	// Dashboard() rather than Kad(): `connected_since` below lives on
	// the status snapshot (the refresher already parses it there for
	// GET /status), and taking both halves in one shared_lock keeps
	// the timestamp describing the same tick as the rest of the
	// payload. Parsing the tag a second time into KadSnapshot would
	// duplicate state for no gain.
	const webapi::CState::DashboardSnapshot d = m_state.Dashboard();
	const webapi::KadSnapshot &k = d.kad;
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	// Bare object (Q3 — Kad is a single resource, not a list).
	w.Key("state");
	w.ValueString(wxString::FromUTF8(k.state.c_str()));
	// Our own Kademlia node id, "" while Kad is not running (i.e.
	// exactly when `state` is "disabled"). Persisted by the daemon,
	// so unlike every other identifier for the local node it is
	// stable across restarts.
	w.Key("node_id");
	w.ValueString(wxString::FromUTF8(k.node_id.c_str()));
	w.Key("firewalled");
	w.ValueBool(k.firewalled);
	w.Key("firewalled_udp");
	w.ValueBool(k.firewalled_udp);
	w.Key("in_lan_mode");
	w.ValueBool(k.in_lan_mode);
	// Same value GET /status reports as kad.connected_since; 0 when
	// not connected, so gate on `state` rather than on a nonzero.
	w.Key("connected_since");
	w.ValueInt(static_cast<int64_t>(d.status.kad_connected_since));
	// Ours, as opposed to `buddy.ip` below — which is why this one
	// is not called plain `ip`.
	w.Key("public_ip");
	w.ValueString(wxString::FromUTF8(k.public_ip.c_str()));
	WriteKadNetworkObject(w, k);
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
// 100k lines is the cap: a bogus `?tail=2147483647` would otherwise try to
// serialise the entire wxString through the JSON escaper. It used to clamp
// silently, which meant a client asking for more got fewer with nothing saying
// so; it is a 400 now, like every other out-of-range count.
std::unique_ptr<CHttpServer::Response> ParseTailParam(const std::string &query, std::size_t &out)
{
	const auto qmap = web_api_path::ParseQuery(query);
	std::uint64_t v = 0;
	if (auto r = ParseUintParam(qmap, "tail", 0, 100000, v))
		return r;
	out = static_cast<std::size_t>(v);
	return nullptr;
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
	// Additive, locale-independent token for well-known sentinel values
	// ("never"/"not_available"); the English "value" above is kept so old
	// clients keep working. Omitted when the value is not a sentinel.
	if (!v.enum_token.empty()) {
		// `token`, not `enum`: `enum` is a reserved word in C++, C#, Java,
		// Rust, PHP and Swift, so a generated client cannot name a field
		// after the key. Matches the internal enum_token.
		w.Key("token");
		w.ValueString(wxString::FromUTF8(v.enum_token.c_str()));
	}
	// Optional nested sub-value. Three different quantities depending on
	// the node -- percentage of parent, packet count, or all-time total --
	// so a client formats it from its `type`, not from its position.
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
	// Raw machine value (client version / OS string) for data-labelled
	// nodes; omitted when absent so clients read it without parsing `label`.
	if (!n.raw.empty()) {
		// `label_value`, not `raw`: for a row whose label is itself data
		// ("v0.70b: %s") this carries the datum. "raw" says unprocessed
		// without saying of what, and sits next to values[] where a reader
		// would look for a raw value.
		w.Key("label_value");
		w.ValueString(wxString::FromUTF8(n.raw.c_str()));
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
// `extra_a` / `extra_b` are optional series point-aligned with `samples`,
// emitted under `key_a` / `key_b` beside each `value`. Used by the
// connections graph for its active-uploads / active-downloads lines; null or
// short (an amuled predating the tag reports neither) leaves the keys off
// entirely, so a consumer can tell "not reported" from "zero".
void WritePointArray(CJsonWriter &w,
	const std::vector<std::uint32_t> &samples,
	std::time_t snapshot_at,
	std::uint32_t interval,
	std::size_t max_width,
	const std::vector<std::uint32_t> *extra_a = nullptr,
	const char *key_a = nullptr,
	const std::vector<std::uint32_t> *extra_b = nullptr,
	const char *key_b = nullptr)
{
	w.BeginArray();
	if (samples.empty()) {
		w.EndArray();
		return;
	}
	const bool has_a = (extra_a != nullptr && extra_a->size() == samples.size() && key_a != nullptr);
	const bool has_b = (extra_b != nullptr && extra_b->size() == samples.size() && key_b != nullptr);
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
		if (has_a) {
			w.Key(key_a);
			w.ValueInt(static_cast<int64_t>((*extra_a)[i]));
		}
		if (has_b) {
			w.Key(key_b);
			w.ValueInt(static_cast<int64_t>((*extra_b)[i]));
		}
		w.EndObject();
	}
	w.EndArray();
}

// The results-array element. Fields come from the shared writer so this
// endpoint and the `search_result_added` SSE payload cannot drift; only the
// braces are ours.
void WriteSearchObject(CJsonWriter &w, const webapi::SearchResult &r)
{
	w.BeginObject();
	webapi::WriteSearchResultFields(w, r);
	w.EndObject();
}

} // namespace

namespace
{
// Reverse of SearchTypeFromString (below, further down this file).
// EC_SEARCH_LOCAL/GLOBAL/KAD share their numeric values with CSearchList's
// own SearchType (LocalSearch/GlobalSearch/KadSearch), which is what
// EC_TAG_SEARCH_LIFECYCLE_KIND carries on the wire (see ExternalConn.cpp's
// Get_EC_Response_Search_List), so a plain uint8 in is enough -- no
// separate SearchType include needed here.
wxString SearchKindToString(std::uint8_t kind)
{
	switch (kind) {
	case EC_SEARCH_LOCAL:
		return wxString::FromAscii("local");
	case EC_SEARCH_KAD:
		return wxString::FromAscii("kad");
	case EC_SEARCH_BROWSE:
		// A "View Files" browse of one peer's share. Reported, never
		// accepted by SearchTypeFromString: browses are not started
		// through the search endpoint.
		return wxString::FromAscii("browse");
	case EC_SEARCH_GLOBAL:
	default:
		return wxString::FromAscii("global");
	}
}

// Shared by HandleSearchResults' `progress.state` and HandleSearchList's
// `state`, so the two endpoints cannot drift into reporting different
// strings for the same search's lifecycle. state_val is a raw
// CSearchList::SearchLifecycleState numeric value (IDLE=0/RUNNING=1/
// FINISHED=2); ExternalConn.cpp's static_assert next to
// Get_EC_Response_Search_List keeps that alignment honest at compile time.
wxString SearchLifecycleStateToString(std::uint8_t state_val)
{
	switch (state_val) {
	case 2:
		return wxString::FromAscii("finished");
	case 1:
		return wxString::FromAscii("running");
	default:
		return wxString::FromAscii("idle");
	}
}
} // namespace

CHttpServer::Response CApiDispatcher::HandleStatsTree(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// ?max_client_versions=N — caps how many per-software version rows the
	// daemon serializes (EC_TAG_STATTREE_CAPPING). 0 is unlimited, which
	// stays the default so a caller that never passes it sees today's tree.
	// Only the version lists are affected; the OS breakdown and the fixed
	// skeleton nodes are not.
	std::uint8_t max_client_versions = 0;
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos)
			query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		std::uint64_t v = max_client_versions;
		if (auto r = ParseUintParam(qmap, "max_client_versions", 0, 255, v))
			return *r;
		max_client_versions = static_cast<std::uint8_t>(v);
	}

	// lazy-fetch with 1 s TTL coalescing. The fetcher runs
	// the EC roundtrip under m_app's m_ec_mtx (SendRecvSerialized);
	// concurrent burst reads serialize on m_stats_tree_cache's mutex
	// and the second waiter reads the just-stored value. As with the graph
	// bundle, the cache is unkeyed, so an entry fetched at a different cap
	// counts as a miss.
	auto pair = m_stats_tree_cache.GetOrFetch(
		std::chrono::milliseconds(1000),
		[this, max_client_versions]() -> TtlPair_StatsTree {
			std::unique_ptr<CECPacket> req_ec(new CECPacket(EC_OP_GET_STATSTREE, EC_DETAIL_WEB));
			req_ec->AddTag(CECTag(EC_TAG_STATTREE_CAPPING, max_client_versions));
			const CECPacket *resp = m_app.SendRecvSerialized(req_ec.get());
			webapi::StatsTreeNode tree;
			std::time_t ts = 0;
			if (resp) {
				webapi::ParseStatsTreeFromPacket(resp, tree);
				tree.max_client_versions = max_client_versions;
				ts = std::time(nullptr);
				delete resp;
			}
			return TtlPair_StatsTree(std::move(tree), ts);
		},
		[max_client_versions](const TtlPair_StatsTree &c) {
			return c.first.max_client_versions == max_client_versions;
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
	// No snapshot_at: the ETag is the cache validator
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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// Validate the graph name BEFORE fetching — saves an EC roundtrip
	// on tab-complete typos hitting /stats/graphs/<bogus>.
	const char *unit = nullptr;
	if (graph == "download_speed") {
		unit = "bytes_per_second";
	} else if (graph == "upload_speed") {
		unit = "bytes_per_second";
	} else if (graph == "connections") {
		unit = "count";
	} else if (graph == "kad_nodes") {
		unit = "count";
	} else {
		return ErrorResponse(404,
			"not_found",
			"unknown graph; expected one of: download_speed, upload_speed, "
			"connections, kad_nodes");
	}

	std::string query;
	const std::size_t q = req.target.find('?');
	if (q != std::string::npos)
		query = req.target.substr(q + 1);
	const auto qmap = web_api_path::ParseQuery(query);

	// ?interval=N — seconds between samples, passed through as
	// EC_TAG_STATSGRAPH_SCALE. Rejected rather than clamped: 0 makes the
	// daemon answer EC_OP_FAILED, which would reach the caller as an
	// unexplained empty graph, and past an hour almost every point falls
	// off the start of the session (the daemon's ranges hold ~63 h in
	// total) — besides, SCALE is a uint16 on the wire.
	std::uint32_t interval = 1;
	{
		std::uint64_t v = interval;
		if (auto r = ParseUintParam(qmap, "interval", 1, 3600, v))
			return *r;
		interval = static_cast<std::uint32_t>(v);
	}

	// Lazy-fetch the full graph bundle (one EC call serves all four named
	// graphs, so the cache shares across concurrent requests for different
	// graph names). The cache is unkeyed, so an entry fetched at another
	// interval has to count as a miss — see CTtlCache's validated overload.
	auto pair = m_stats_graphs_cache.GetOrFetch(
		std::chrono::milliseconds(1000),
		[this, interval]() -> TtlPair_StatsGraphs {
			std::unique_ptr<CECPacket> req_ec(new CECPacket(EC_OP_GET_STATSGRAPHS));
			req_ec->AddTag(CECTag(EC_TAG_STATSGRAPH_SCALE, static_cast<std::uint16_t>(interval)));
			req_ec->AddTag(CECTag(EC_TAG_STATSGRAPH_WIDTH, static_cast<std::uint16_t>(1800)));
			const CECPacket *resp = m_app.SendRecvSerialized(req_ec.get());
			webapi::StatsGraphs g;
			std::time_t ts = 0;
			if (resp) {
				webapi::ParseGraphsFromPacket(resp, g);
				g.interval_seconds = interval;
				ts = std::time(nullptr);
				delete resp;
			}
			return TtlPair_StatsGraphs(std::move(g), ts);
		},
		[interval](const TtlPair_StatsGraphs &c) { return c.first.interval_seconds == interval; });

	if (pair.second == 0) {
		return ErrorResponse(503,
			"ec_unavailable",
			"EC fetch failed for stats graphs; amuled may be disconnected");
	}

	const webapi::StatsGraphs &g = pair.first;
	const std::vector<std::uint32_t> *series = nullptr;
	if (graph == "download_speed") {
		series = &g.download_bps;
	} else if (graph == "upload_speed") {
		series = &g.upload_bps;
	} else if (graph == "connections") {
		series = &g.connections;
	} else /* kad_nodes */ {
		series = &g.kad_nodes;
	}

	// ?width=N — tail the sample count returned. 0 / absent means
	// "everything we have". Applied after the fetch, deliberately: the EC
	// request always asks for the full window, so one cached bundle still
	// answers every (graph, width) combination.
	std::size_t width = 0;
	{
		std::uint64_t v = 0;
		if (auto r = ParseUintParam(qmap, "width", 0, 1800, v))
			return *r;
		width = static_cast<std::size_t>(v);
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
	// How many points this daemon can answer with before it starts
	// repeating records. `points` is never longer than this.
	w.Key("max_points");
	w.ValueInt(static_cast<int64_t>(g.max_points));
	// No snapshot_at in the response. WritePointArray
	// still consumes `ts` to compute per-point timestamps (anchoring
	// the time-series backwards from the fetch wall-clock).
	//
	// The two extra series exist only on the connections graph, and only
	// when the daemon sent the second data blob.
	w.Key("points");
	if (graph == "connections") {
		WritePointArray(w,
			*series,
			ts,
			g.interval_seconds,
			width,
			&g.active_downloads,
			"active_downloads",
			&g.active_uploads,
			"active_uploads");
	} else {
		WritePointArray(w, *series, ts, g.interval_seconds, width);
	}
	// Session totals tag along — clients showing "this session: X GB
	// down" don't need a separate roundtrip. Divide any of the three by
	// duration_seconds for the session average the desktop plots.
	w.Key("session");
	w.BeginObject();
	w.Key("download_bytes");
	w.ValueInt(static_cast<int64_t>(g.session_download_bytes));
	w.Key("upload_bytes");
	w.ValueInt(static_cast<int64_t>(g.session_upload_bytes));
	w.Key("kad_node_seconds");
	w.ValueInt(static_cast<int64_t>(g.session_kad_node_seconds));
	w.Key("duration_seconds");
	w.ValueInt(static_cast<int64_t>(g.session_duration_seconds));
	w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSearchResults(
	const CHttpServer::Request &req, std::uint32_t search_id)
{
	auto a = Authenticate(req);
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
	// The search is named in the path. An id that names no live slot (never
	// started, freed, or evicted from the daemon's ring) is a 404 — distinct
	// from a known-but-empty search, which returns an idle/empty envelope.
	if (auto rej = RequireSearch(search_id))
		return *rej;
	// A FINISHED search is not polled by the tick, so its cached results
	// would otherwise be frozen at the moment it completed. Refresh on read,
	// coalesced by a short TTL.
	RefreshSearchIfStale(search_id);
	const std::vector<webapi::SearchResult> results_vec = m_state.Search(search_id);
	const webapi::SearchProgressSnapshot progress = m_state.SearchProgress(search_id);

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
		// Browse listings are read folder by folder, which is how the
		// desktop sorts its Directories column too. Empty on server/Kad
		// hits, so sorting a non-browse search by it is a stable no-op
		// rather than an error.
		{ "directory",
			[](const webapi::SearchResult &a, const webapi::SearchResult &b) {
				return a.directory < b.directory;
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
	// The search these results belong to. Echoed even though the caller put
	// it in the path: clients key their tabs on it and it costs nothing.
	w.Key("search_id");
	w.ValueInt(static_cast<int64_t>(search_id));
	// What was searched for. Without it a client that adopted an id (from
	// GET /search, or from another client) would have to cross-reference the
	// list endpoint just to label the tab it is already reading. For a browse
	// this is the peer's name rather than a query string.
	w.Key("query");
	w.ValueString(wxString::FromUTF8(m_state.SearchQuery(search_id).c_str()));
	// Mirrors the `search_progress` SSE event field-for-field. `state`
	// is canonical and encodes the full lifecycle (running / finished /
	// idle), so we don't also emit redundant `active` / `complete`
	// booleans — consumers derive them from `state` and read the same
	// shape whether they poll here or subscribe to the stream.
	w.Key("progress");
	w.BeginObject();
	w.Key("state");
	w.ValueString(SearchLifecycleStateToString(progress.complete ? 2 : progress.active ? 1 : 0));
	w.Key("kind");
	w.ValueString(wxString::FromUTF8(progress.kind.c_str()));
	w.Key("percent");
	w.ValueInt(static_cast<int64_t>(progress.percent));
	w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

std::unique_ptr<CHttpServer::Response> CApiDispatcher::RequireSearch(std::uint32_t search_id)
{
	if (m_state.HasSearch(search_id))
		return nullptr;
	// Cache miss: before giving up, ask the core once whether it holds this
	// id anyway -- a search amulegui, the monolithic GUI or a previous
	// amuleapi run started. Seeding it here is what lets a UI adopt one.
	if (DiscoverSearchIfHeldByCore(search_id))
		return nullptr;
	return std::make_unique<CHttpServer::Response>(ErrorResponse(
		404, "not_found", "no search with that search_id (never started, freed or expired)"));
}

void CApiDispatcher::RefreshSearchIfStale(std::uint32_t search_id)
{
	// ClaimSearchRefresh does the gating: it returns true only for a slot
	// that exists, is not active (the tick already covers those every
	// second), and has not been fetched within the TTL -- and it stamps the
	// slot as it hands out the claim, so two readers racing on the same
	// finished search cost one roundtrip, not two.
	static constexpr std::chrono::milliseconds kSearchRefreshTtl{ 1000 };
	if (!m_state.ClaimSearchRefresh(search_id, kSearchRefreshTtl))
		return;
	// A failed roundtrip leaves the cached results in place: serving the
	// previous set is strictly better than failing a read that has a
	// perfectly good answer. An expiry is left to the tick's own retirement
	// path rather than duplicated here.
	(void)webapi::FetchSearchResults(m_app, m_state, search_id);
}

// See the declaration in Api.h for why this is shared rather than inlined
// at each call site.
bool CApiDispatcher::DiscoverSearchIfHeldByCore(std::uint32_t search_id)
{
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SEARCH_LIST));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return false;
	}
	bool found = false;
	for (const CECTag &entry : *ec_resp) {
		if (static_cast<std::uint32_t>(entry.GetInt()) != search_id)
			continue;
		const CECTag *kindTag = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_KIND);
		// The list entry carries the daemon's name for the search, which is
		// the query string (or, for a browse, the peer's nickname). Taking it
		// here is what lets an adopted search report its own `query` instead
		// of an empty one.
		const CECTag *nameTag = entry.GetTagByName(EC_TAG_SEARCH_NAME);
		// ...and its lifecycle state, which was being dropped and replaced
		// with an assumed "active". A finished search seeded as running is
		// not just cosmetic: POST /search/{id}/more rejects a finished
		// search, so it would have accepted one until the next tick
		// corrected the flag, and answered 202 for a request amuled turns
		// into a no-op. 1 = running, 2 = finished (SearchLifecycleStateToString).
		const CECTag *stateTag = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_STATE);
		const std::uint8_t state_val = stateTag ? static_cast<std::uint8_t>(stateTag->GetInt()) : 0;
		// ...and the percent, when the daemon reports one. -1 means it did
		// not, which is what an older daemon looks like; the seed then falls
		// back to deriving it from the lifecycle state.
		const CECTag *pctTag = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_PERCENT);
		const int reported_pct = pctTag ? static_cast<int>(pctTag->GetInt()) : -1;
		m_state.MarkSearchDiscovered(search_id,
			SearchKindToString(
				kindTag ? static_cast<std::uint8_t>(kindTag->GetInt()) : EC_SEARCH_GLOBAL)
				.ToStdString(),
			nameTag ? std::string(nameTag->GetStringData().utf8_str()) : std::string(),
			state_val == 1,
			state_val == 2,
			reported_pct);
		found = true;
		break;
	}
	delete ec_resp;
	return found;
}

// Reachability fix (amule-org/amule#641): enumerates every search the
// daemon currently holds via EC_OP_SEARCH_LIST, rather than reading the
// Refresher-cached m_state (which -- like m_curr_search on amulegui --
// only ever knows about searches THIS session started with POST /search).
// A direct one-off EC round trip, same pattern HandleSearchStart already
// uses for SEARCH_START; no Refresher/m_state changes needed to make a
// search started by another client (or, once persistence lands, restored
// from disk) discoverable here.
CHttpServer::Response CApiDispatcher::HandleSearchList(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	ListParams params;
	if (auto r = ParseListParams(QueryOf(req), params))
		return *r;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SEARCH_LIST));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SEARCH_LIST");
	}

	std::vector<SearchListRow> rows;
	for (const CECTag &entry : *ec_resp) {
		SearchListRow row;
		row.search_id = static_cast<std::uint32_t>(entry.GetInt());
		const CECTag *nameTag = entry.GetTagByName(EC_TAG_SEARCH_NAME);
		row.query = nameTag ? nameTag->GetStringData() : wxString();
		const CECTag *kindTag = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_KIND);
		row.kind = SearchKindToString(kindTag ? static_cast<std::uint8_t>(kindTag->GetInt()) : 0);
		const CECTag *stateTag = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_STATE);
		row.state = SearchLifecycleStateToString(
			stateTag ? static_cast<std::uint8_t>(stateTag->GetInt()) : 0);
		// Browse ("View Files") entries carry the browsed peer's ecid. Without
		// it a consumer sees `kind: "browse"` with no way to tell WHOSE share
		// it is listing. Absent on an ordinary search, which never carries it.
		if (const CECTag *clientTag = entry.GetTagByName(EC_TAG_CLIENT)) {
			row.has_client_ecid = true;
			row.client_ecid = static_cast<std::uint32_t>(clientTag->GetInt());
		}
		// When THIS amuleapi started the search. Absent for one this process
		// did not start -- another client's, or one the daemon restored from
		// disk -- because a 0 would read as 1970 rather than "no idea".
		row.started_at = m_state.SearchStartedAt(row.search_id);
		// The same number GET /search/{id}/results reports as `total`. Absent
		// when the daemon is older than the tag, so that "does not report"
		// stays distinguishable from "found nothing".
		if (const CECTag *countTag = entry.GetTagByName(EC_TAG_SEARCH_RESULT_COUNT)) {
			row.has_result_count = true;
			row.result_count = static_cast<std::uint32_t>(countTag->GetInt());
		}
		rows.push_back(std::move(row));
	}
	delete ec_resp;

	return ListResponse(m_state, "searches", rows, WriteSearchListRow, params, SearchListComparators());
}

CHttpServer::Response CApiDispatcher::HandleLogAmule(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// Extract the query string from the raw target (request.target is
	// the literal URI, e.g. "/api/v0/logs/amule?tail=200").
	std::string path, query;
	const size_t q = req.target.find('?');
	if (q != std::string::npos) {
		query = req.target.substr(q + 1);
	}
	std::size_t tail = 0;
	if (auto r = ParseTailParam(query, tail))
		return *r;
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
	auto a = Authenticate(req);
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
	// (AppendAmuleLog) can't shrink the cache, and EmitDiffsAndUpdate treats a
	// size decrease as a silent truncation (its `log_size <
	// prev.amule_log_count` branch), so no spurious log_appended event fires on
	// the next tick -- and, as noted there, no event at all for whatever was
	// appended before that tick.
	m_state.ClearAmuleLog();

	CHttpServer::Response r;
	r.status = 204;
	r.content_type.clear();
	return r;
}

CHttpServer::Response CApiDispatcher::HandleLogServerinfo(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	std::string query;
	const size_t q = req.target.find('?');
	if (q != std::string::npos) {
		query = req.target.substr(q + 1);
	}
	std::size_t tail = 0;
	if (auto r = ParseTailParam(query, tail))
		return *r;

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
	auto a = Authenticate(req);
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

namespace
{

// Emit the full /preferences JSON object from the declarative field table
// (PrefsSchema.cpp). Shared by the GET handler and the PATCH echo so the
// response shape is defined exactly once.
//
// Categories whose name contains a dot are nested one level under their
// prefix (remote_controls.webserver -> "remote_controls": {"webserver": {...}}),
// which is how the two remote-control subsystems avoid prefixing every field.
// Write-only rows (passwords, the ip2country trigger) and Rejected rows are
// never emitted.
void WritePrefFieldValue(CJsonWriter &w, const webapi::PrefField &f, const webapi::PreferencesSnapshot &p)
{
	// The accessor takes a non-const snapshot; emitting never mutates it.
	webapi::PreferencesSnapshot &m = const_cast<webapi::PreferencesSnapshot &>(p);
	switch (f.type) {
	case webapi::PrefType::Bool:
		w.ValueBool(*static_cast<bool *>(f.member(m)));
		break;
	case webapi::PrefType::Uint16:
		w.ValueInt(static_cast<int64_t>(*static_cast<std::uint16_t *>(f.member(m))));
		break;
	case webapi::PrefType::Uint32:
		w.ValueInt(static_cast<int64_t>(*static_cast<std::uint32_t *>(f.member(m))));
		break;
	case webapi::PrefType::String:
	case webapi::PrefType::Enum:
	case webapi::PrefType::Md4Hex:
		w.ValueString(wxString::FromUTF8(static_cast<std::string *>(f.member(m))->c_str()));
		break;
	case webapi::PrefType::StringArray: {
		w.BeginArray();
		for (const std::string &s : *static_cast<std::vector<std::string> *>(f.member(m)))
			w.ValueString(wxString::FromUTF8(s.c_str()));
		w.EndArray();
		break;
	}
	}
}

bool PrefFieldIsEmitted(const webapi::PrefField &f)
{
	return f.access != webapi::PrefAccess::WriteOnly && f.access != webapi::PrefAccess::Rejected;
}

void WritePrefCategoryFields(CJsonWriter &w, const char *category, const webapi::PreferencesSnapshot &p)
{
	for (std::size_t i = 0; i < webapi::PrefSchemaSize(); ++i) {
		const webapi::PrefField &f = webapi::PrefSchema()[i];
		if (!PrefFieldIsEmitted(f) || std::strcmp(f.category, category) != 0)
			continue;
		w.Key(f.key);
		WritePrefFieldValue(w, f, p);
	}
}

void WritePreferencesBody(CJsonWriter &w, const webapi::PreferencesSnapshot &p)
{
	w.BeginObject();

	std::string emitted; // top-level names already written, "|name|" separated
	for (std::size_t c = 0; c < webapi::PrefCategoryCount(); ++c) {
		const char *name = webapi::PrefCategories()[c].name;
		const char *dot = std::strchr(name, '.');
		const std::string top = dot ? std::string(name, dot) : std::string(name);
		if (emitted.find("|" + top + "|") != std::string::npos)
			continue;
		emitted += "|" + top + "|";

		w.Key(top.c_str());
		w.BeginObject();
		// Fields sitting directly on the top-level category.
		WritePrefCategoryFields(w, top.c_str(), p);
		// Then each nested sub-object, in table order.
		for (std::size_t s = 0; s < webapi::PrefCategoryCount(); ++s) {
			const char *sub = webapi::PrefCategories()[s].name;
			if (std::strncmp(sub, top.c_str(), top.size()) != 0 || sub[top.size()] != '.')
				continue;
			w.Key(sub + top.size() + 1);
			w.BeginObject();
			WritePrefCategoryFields(w, sub, p);
			w.EndObject();
		}
		w.EndObject();
	}

	w.EndObject();
}

} // namespace

CHttpServer::Response CApiDispatcher::HandlePreferences(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto r = RequireSnapshot(m_state))
		return *r;

	const webapi::PreferencesSnapshot p = m_state.Preferences();
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WritePreferencesBody(w, p);
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

// --- Generic optional-field extractors for the #437 categories -------
//
// Each pulls one optional key from a sub-object into the EC group tag,
// validating its JSON type. Returns false and sets `err` on a type/
// range error; returns true (and leaves `group` untouched) when the
// key is simply absent. `any` is set true when a field is written.
// Booleans always pack as a value tag (uint8 0/1): CEC_Prefs_Packet::
// Apply reads `GetInt()!=0` under EC_DETAIL_FULL, so an empty presence
// tag would be read as false.
bool PrefTakeUint(const picojson::object &o,
	CECTag &group,
	const char *key,
	ec_tagname_t name,
	std::uint32_t max,
	bool &any,
	std::string &err)
{
	const auto it = o.find(key);
	if (it == o.end())
		return true;
	if (!it->second.is<double>()) {
		err = std::string(key) + " must be a non-negative integer";
		return false;
	}
	const double v = it->second.get<double>();
	if (v < 0 || v > static_cast<double>(max)) {
		err = std::string(key) + " out of range";
		return false;
	}
	group.AddTag(CECTag(name, static_cast<std::uint32_t>(v)));
	any = true;
	return true;
}

// invert=true stores the opposite of the JSON value in the EC tag, for
// positive-sense API fields whose EC tag is negatively named (today only
// extended_udp_port_enabled -> EC_TAG_CONN_UDP_DISABLE). It is a schema
// column, not a special case in the caller.
bool PrefTakeBool(const picojson::object &o,
	CECTag &group,
	const char *key,
	ec_tagname_t name,
	bool &any,
	std::string &err,
	bool invert)
{
	const auto it = o.find(key);
	if (it == o.end())
		return true;
	if (!it->second.is<bool>()) {
		err = std::string(key) + " must be a bool";
		return false;
	}
	const bool v = it->second.get<bool>();
	group.AddTag(CECTag(name, static_cast<std::uint8_t>((invert ? !v : v) ? 1 : 0)));
	any = true;
	return true;
}

// Enum field (#655): the API spells the value out ("socks5", "friends", ...)
// while EC carries the bare ordinal. `names` lists the accepted strings in
// wire order, so a name's index is exactly the value the daemon's Apply()
// casts back to its enum. Shared by connection.proxy_type,
// security.shared_files_visibility and ip2country.source.
bool PrefTakeEnum(const picojson::object &o,
	CECTag &group,
	const char *key,
	ec_tagname_t name,
	const char *const *names,
	bool &any,
	std::string &err)
{
	const auto it = o.find(key);
	if (it == o.end())
		return true;
	if (!it->second.is<std::string>()) {
		err = std::string(key) + " must be a string";
		return false;
	}
	const std::string &v = it->second.get<std::string>();
	for (std::uint8_t idx = 0; names[idx] != nullptr; ++idx) {
		if (v == names[idx]) {
			group.AddTag(CECTag(name, idx));
			any = true;
			return true;
		}
	}
	std::string accepted;
	for (std::size_t i = 0; names[i] != nullptr; ++i) {
		if (!accepted.empty())
			accepted += ", ";
		accepted += names[i];
	}
	err = std::string(key) + " must be one of " + accepted;
	return false;
}

bool PrefTakeString(const picojson::object &o,
	CECTag &group,
	const char *key,
	ec_tagname_t name,
	bool &any,
	std::string &err)
{
	const auto it = o.find(key);
	if (it == o.end())
		return true;
	if (!it->second.is<std::string>()) {
		err = std::string(key) + " must be a string";
		return false;
	}
	group.AddTag(CECTag(name, wxString::FromUTF8(it->second.get<std::string>().c_str())));
	any = true;
	return true;
}

// String-array field (directories.shared): a JSON array of strings
// packed as EC_TAG_STRING children, mirroring the core serializer.
bool PrefTakeStringArray(const picojson::object &o,
	CECTag &group,
	const char *key,
	ec_tagname_t name,
	bool &any,
	std::string &err)
{
	const auto it = o.find(key);
	if (it == o.end())
		return true;
	if (!it->second.is<picojson::array>()) {
		err = std::string(key) + " must be an array of strings";
		return false;
	}
	const auto &arr = it->second.get<picojson::array>();
	CECTag list(name, static_cast<std::uint32_t>(arr.size()));
	for (const auto &el : arr) {
		if (!el.is<std::string>()) {
			err = std::string(key) + " must be an array of strings";
			return false;
		}
		list.AddTag(CECTag(EC_TAG_STRING, wxString::FromUTF8(el.get<std::string>().c_str())));
	}
	group.AddTag(list);
	any = true;
	return true;
}

// Write-only password: hash the plaintext with MD5 (matching how the
// daemon stores WS/amuleapi passwords) and pack the 16-byte digest as
// the given hash tag. Never round-trips on GET.
bool PrefTakePassword(const picojson::object &o,
	CECTag &group,
	const char *key,
	ec_tagname_t name,
	bool &any,
	std::string &err)
{
	const auto it = o.find(key);
	if (it == o.end())
		return true;
	if (!it->second.is<std::string>()) {
		err = std::string(key) + " must be a string";
		return false;
	}
	const wxString md5hex = MD5Sum(wxString::FromUTF8(it->second.get<std::string>().c_str())).GetHash();
	CMD4Hash hash;
	if (!HashFromHex(std::string(md5hex.utf8_str()), hash)) {
		err = std::string(key) + " could not be hashed";
		return false;
	}
	group.AddTag(CECTag(name, hash));
	any = true;
	return true;
}

// Resolve an optional sub-object by key. Returns false + err when the
// key is present but not an object; leaves `out` null when absent.
bool PrefFindSubObject(
	const picojson::object &obj, const char *key, const picojson::object *&out, std::string &err)
{
	const auto it = obj.find(key);
	if (it == obj.end())
		return true;
	if (!it->second.is<picojson::object>()) {
		err = std::string("`") + key + "` must be an object";
		return false;
	}
	out = &it->second.get<picojson::object>();
	return true;
}
} // namespace

CHttpServer::Response CApiDispatcher::HandlePreferencesPatch(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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

	// Body shape mirrors the GET: one optional sub-object per category, every
	// field within optional, fields not present left unchanged. Categories and
	// their fields both come from the schema table (#655), so the accepted
	// shape cannot drift from the emitted one.
	//
	// Build at EC_DETAIL_FULL: amuled's Apply() gates ApplyBoolean on
	// detail == FULL, so booleans are only honoured at that level. That is
	// also why every bool below is written as a value tag rather than the
	// presence tag the daemon uses when serializing in the other direction.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SET_PREFERENCES, EC_DETAIL_FULL));
	bool any_change = false;

	// One CECTag per EC group, created on first use. Two categories can share
	// a group (remote_controls.webserver / .amuleapi), so they must land in
	// the same tag rather than two conflicting ones.
	std::map<ec_tagname_t, CECTag> groups;
	std::vector<ec_tagname_t> group_order;
	auto group_for = [&](ec_tagname_t tag) -> CECTag & {
		auto it = groups.find(tag);
		if (it == groups.end()) {
			it = groups.emplace(tag, CECTag(tag, static_cast<std::uint32_t>(0))).first;
			group_order.push_back(tag);
		}
		return it->second;
	};

	// Resolve each category's sub-object up front so an unknown *type* is a
	// 400 even when the category carries no recognized field.
	std::vector<const picojson::object *> cat_obj(webapi::PrefCategoryCount(), nullptr);
	for (std::size_t c = 0; c < webapi::PrefCategoryCount(); ++c) {
		const char *name = webapi::PrefCategories()[c].name;
		const char *dot = std::strchr(name, '.');
		const picojson::object *parent = &obj;
		std::string leaf(name);
		if (dot) {
			const std::string top(name, dot);
			const picojson::object *sub = nullptr;
			if (!PrefFindSubObject(obj, top.c_str(), sub, parse_err))
				return ErrorResponse(400, "bad_request", parse_err.c_str());
			if (!sub)
				continue;
			parent = sub;
			leaf = dot + 1;
		}
		const picojson::object *found = nullptr;
		if (!PrefFindSubObject(*parent, leaf.c_str(), found, parse_err))
			return ErrorResponse(400, "bad_request", parse_err.c_str());
		cat_obj[c] = found;
	}

	const webapi::PreferencesSnapshot current = m_state.Preferences();

	for (std::size_t i = 0; i < webapi::PrefSchemaSize(); ++i) {
		const webapi::PrefField &f = webapi::PrefSchema()[i];

		// Locate this field's category object; skip the whole row when the
		// client did not send that category at all.
		const picojson::object *src = nullptr;
		for (std::size_t c = 0; c < webapi::PrefCategoryCount(); ++c) {
			if (std::strcmp(webapi::PrefCategories()[c].name, f.category) == 0) {
				src = cat_obj[c];
				break;
			}
		}
		if (!src || src->find(f.key) == src->end())
			continue;

		// Read-only fields (daemon capabilities, live status) are ignored
		// rather than rejected, matching how the endpoint has always treated
		// them. Bespoke fields are applied by dedicated code further down.
		if (f.access == webapi::PrefAccess::ReadOnly || f.access == webapi::PrefAccess::Bespoke)
			continue;

		if (f.access == webapi::PrefAccess::Rejected) {
			return ErrorResponse(400,
				"bad_request",
				"amuleapi passwords are managed through PATCH /auth/passwords, "
				"not through /preferences");
		}

		// Capability gate: refuse rather than silently drop a setting the
		// connected daemon cannot honour.
		if (f.gated_by) {
			bool ok = true;
			for (std::size_t g = 0; g < webapi::PrefSchemaSize(); ++g) {
				const webapi::PrefField &cap = webapi::PrefSchema()[g];
				if (std::strcmp(cap.category, f.category) != 0 ||
					std::strcmp(cap.key, f.gated_by) != 0)
					continue;
				webapi::PreferencesSnapshot &snap =
					const_cast<webapi::PreferencesSnapshot &>(current);
				ok = *static_cast<bool *>(cap.member(snap));
				break;
			}
			if (!ok) {
				return ErrorResponse(409,
					"conflict",
					"this daemon was built without support for that option");
			}
		}

		CECTag &g = group_for(webapi::PrefGroupTagFor(f.category));
		std::string err;
		bool ok = true;
		switch (f.type) {
		case webapi::PrefType::Bool:
			ok = PrefTakeBool(*src, g, f.key, f.tag, any_change, err, f.invert);
			break;
		case webapi::PrefType::Uint16:
		case webapi::PrefType::Uint32:
			ok = PrefTakeUint(*src, g, f.key, f.tag, f.max, any_change, err);
			break;
		case webapi::PrefType::String:
			ok = PrefTakeString(*src, g, f.key, f.tag, any_change, err);
			break;
		case webapi::PrefType::StringArray:
			ok = PrefTakeStringArray(*src, g, f.key, f.tag, any_change, err);
			break;
		case webapi::PrefType::Enum:
			ok = PrefTakeEnum(*src, g, f.key, f.tag, f.enum_names, any_change, err);
			break;
		case webapi::PrefType::Md4Hex:
			// Only reachable for a write-only password row: the plaintext is
			// hashed here and the hash is what crosses EC.
			ok = PrefTakePassword(*src, g, f.key, f.tag, any_change, err);
			break;
		}
		if (!ok)
			return ErrorResponse(400, "bad_request", err.c_str());
	}

	// --- The one field pair the table cannot describe. ------------------
	// remote_controls.webserver.guest_enabled and .guest_password share a
	// single EC tag: EC_TAG_WEBSERVER_GUEST carries the enable bool as its
	// value and the password hash as a child. There is no 1:1 field-to-tag
	// mapping to put in the schema, so the packing stays hand-written. When
	// only the password is given, the enable bit falls back to the current
	// snapshot value.
	{
		const picojson::object *ws = nullptr;
		for (std::size_t c = 0; c < webapi::PrefCategoryCount(); ++c) {
			if (std::strcmp(webapi::PrefCategories()[c].name, "remote_controls.webserver") == 0) {
				ws = cat_obj[c];
				break;
			}
		}
		if (ws) {
			const auto en_it = ws->find("guest_enabled");
			const auto pw_it = ws->find("guest_password");
			const bool has_en = en_it != ws->end();
			const bool has_pw = pw_it != ws->end();
			if (has_en || has_pw) {
				if (has_en && !en_it->second.is<bool>())
					return ErrorResponse(
						400, "bad_request", "guest_enabled must be a bool");
				if (has_pw && !pw_it->second.is<std::string>())
					return ErrorResponse(
						400, "bad_request", "guest_password must be a string");
				const bool enabled = has_en ? en_it->second.get<bool>()
							    : current.remote_controls.webserver.guest_enabled;
				CECTag guest(
					EC_TAG_WEBSERVER_GUEST, static_cast<std::uint8_t>(enabled ? 1 : 0));
				if (has_pw) {
					const wxString md5hex = MD5Sum(
						wxString::FromUTF8(pw_it->second.get<std::string>().c_str()))
									.GetHash();
					CMD4Hash h;
					if (HashFromHex(std::string(md5hex.utf8_str()), h))
						guest.AddTag(CECTag(EC_TAG_PASSWD_HASH, h));
				}
				group_for(EC_TAG_PREFS_REMOTECTRL).AddTag(guest);
				any_change = true;
			}
		}
	}

	for (ec_tagname_t tag : group_order)
		ec_req->AddTag(groups.find(tag)->second);

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
	WritePreferencesBody(w, p);
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
	auto a = Authenticate(req);
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
	auto a = Authenticate(req);
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

// POST /kad/update — refresh the Kad node list from a nodes.dat URL (#693).
//
// Shares ResolveFetchUrl / UrlFetchOp with /servers/update and
// /ipfilter/update: same validation, same 202. The EC handler
// (EC_OP_KAD_UPDATE_FROM_URL) persists the URL into preferences itself via
// SetKadNodesUrl(), so this deliberately does NOT also patch
// kademlia.update_url — doing both would diverge from the ed2k path and could
// race it.
//
// Side effect worth knowing: once the download completes amuled stops Kad,
// swaps nodes.dat in, and starts Kad again. The desktop GUI warns before
// firing this; API callers get the same behaviour without the prompt.
CHttpServer::Response CApiDispatcher::HandleKadUpdateFromUrl(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// No inline RefresherTick: nothing observable has changed yet. Once
	// the download completes amuled stops Kad, swaps nodes.dat in and
	// starts Kad again, and the natural tick reports that.
	static const UrlFetchSpec kSpec = {
		"nodes_url", EC_OP_KAD_UPDATE_FROM_URL, EC_TAG_KADEMLIA_UPDATE_URL, true, false
	};
	std::string url;
	CHttpServer::Response rejection;
	if (!ResolveFetchUrl(req, kSpec, nullptr, url, rejection)) {
		return rejection;
	}
	return UrlFetchOp(m_app, m_state, kSpec, url);
}

// POST /ipfilter/reload — re-read ipfilter.dat + ipfilter_static.dat from
// amuled's config directory into the live filter. The desktop client's
// "Reload List" button, and the same EC op amulegui sends for it.
//
// amuled queues a CIPFilterTask and keeps the current filter live until the
// new one has finished loading, so this is accepted, never completed; the
// outcome is only ever an amule log line ("Loading IP filters ...",
// "IP filter is ready"), read back through /logs/amule or the SSE log
// channel.
CHttpServer::Response CApiDispatcher::HandleIpfilterReload(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	return SimpleConnControlOp(m_app, m_state, EC_OP_IPFILTER_RELOAD, 202);
}

// POST /ipfilter/update — download ipfilter.dat from a URL, swap it in and
// reload. The desktop client's "Update now" button.
//
// Unlike its two siblings the URL is optional: with no body amuleapi
// resolves security.ipfilter_update_url from its own preferences snapshot
// and sends that, so the behaviour does not depend on which amuled build
// answers. With neither, this is a 400 rather than a request the core turns
// into a silent no-op (CIPFilter::Update() returns immediately on an empty
// URL). The snapshot trails amuled by up to one refresher tick, so a
// PATCH /preferences immediately followed by a bodyless update can still
// send the previous URL — pass it explicitly to be sure which one runs.
CHttpServer::Response CApiDispatcher::HandleIpfilterUpdate(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	// EC_OP_IPFILTER_UPDATE reads the packet's first tag by position, not
	// by name, so the tag is EC_TAG_STRING — what amulegui has always sent.
	// No inline RefresherTick: the download is asynchronous and lands in
	// amuled's filter, not in a cache this process holds.
	static const UrlFetchSpec kSpec = {
		"ipfilter_url", EC_OP_IPFILTER_UPDATE, EC_TAG_STRING, false, false
	};
	// Named local: Preferences() hands back a snapshot by value. Offer it as
	// the fallback only once there is a snapshot to read — before the first
	// one the defaults would look like "no URL configured".
	const bool have_prefs = m_state.HasFirstSnapshot();
	const std::string configured =
		have_prefs ? m_state.Preferences().security.ipfilter_update_url : std::string();
	std::string url;
	CHttpServer::Response rejection;
	if (!ResolveFetchUrl(req, kSpec, have_prefs ? &configured : nullptr, url, rejection)) {
		return rejection;
	}
	return UrlFetchOp(m_app, m_state, kSpec, url);
}

CHttpServer::Response CApiDispatcher::HandleKadBootstrap(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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
			if (!ParseIpv4Dotted(s, ip_he)) {
				return ErrorResponse(400,
					"bad_request",
					"`ip` must be a dotted-quad IPv4 address or a "
					"host-order uint32");
			}
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

} // namespace

CHttpServer::Response CApiDispatcher::HandleSharedPatch(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot s;
	if (!FindSharedByKey(m_state, key, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}

	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	bool any_change = false;

	// priority (optional now that comment/rating share this endpoint).
	const auto pit = obj.find("priority");
	if (pit != obj.end()) {
		if (!pit->second.is<std::string>()) {
			return ErrorResponse(400, "bad_request", "`priority` must be a wire-string enum");
		}
		std::uint8_t code = 0;
		if (!FilePriorityToCode(pit->second.get<std::string>(), kPrioShared, code)) {
			return ErrorResponse(400, "bad_request", FilePriorityAccepted(kPrioShared).c_str());
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
			return ErrorResponse(
				503, "ec_unavailable", "EC roundtrip failed for SHARED_SET_PRIO");
		}
		std::string ec_err_msg;
		if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
			delete ec_resp;
			return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
		}
		delete ec_resp;
		any_change = true;
	}

	// comment + rating (both required together; issue #419).
	{
		bool applied = false;
		CHttpServer::Response cr_err;
		if (!TrySetCommentRating(m_app, obj, s, applied, cr_err))
			return cr_err;
		if (applied)
			any_change = true;
	}

	// name (rename; issue #420).
	{
		bool applied = false;
		CHttpServer::Response rn_err;
		if (!TryRename(m_app, obj, s, applied, rn_err))
			return rn_err;
		if (applied)
			any_change = true;
	}

	if (!any_change) {
		return ErrorResponse(400,
			"bad_request",
			"request body must include `priority`, `comment`+`rating`, or `name`");
	}

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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto r = RequireSnapshot(m_state))
		return *r;

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
					"`status` must be one of \"paused\", \"resumed\" or \"stopped\"");
			const std::string &v = it->second.get<std::string>();
			if (v == "paused")
				ops.push_back(
					{ EC_OP_PARTFILE_PAUSE, false, static_cast<ec_tagname_t>(0), 0 });
			else if (v == "resumed")
				ops.push_back(
					{ EC_OP_PARTFILE_RESUME, false, static_cast<ec_tagname_t>(0), 0 });
			else if (v == "stopped")
				ops.push_back(
					{ EC_OP_PARTFILE_STOP, false, static_cast<ec_tagname_t>(0), 0 });
			else
				return ErrorResponse(400,
					"bad_request",
					"`status` must be one of \"paused\", \"resumed\" or \"stopped\"");
		}
	}
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>())
				return ErrorResponse(
					400, "bad_request", "`priority` must be a wire-string enum");
			std::uint8_t code = 0;
			if (!FilePriorityToCode(it->second.get<std::string>(), kPrioDownload, code))
				return ErrorResponse(
					400, "bad_request", FilePriorityAccepted(kPrioDownload).c_str());
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
		const std::string needle = LowerHexKey(raw);
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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto r = RequireSnapshot(m_state))
		return *r;

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
		const std::string needle = LowerHexKey(raw);
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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto r = RequireSnapshot(m_state))
		return *r;

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
	if (!FilePriorityToCode(pit->second.get<std::string>(), kPrioShared, code))
		return ErrorResponse(400, "bad_request", FilePriorityAccepted(kPrioShared).c_str());

	std::vector<BulkItem> results;
	results.reserve(hashes.size());
	for (const std::string &raw : hashes) {
		const std::string needle = LowerHexKey(raw);
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

CHttpServer::Response CApiDispatcher::HandleSharedVerify(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot s;
	if (!FindSharedByKey(m_state, key, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}

	// Partfiles have no verify implementation: the hashing task bails out on
	// IsPartFile(), and amuled's EC handler answers NOOP either way, so a
	// caller would be told the re-hash was accepted and then never see a
	// report. Reject up front instead -- a download that has completed but is
	// still listed is a knownfile by then, and so a legitimate verify target,
	// which is exactly what IsIncompletePartfile() excludes.
	if (s.IsIncompletePartfile()) {
		return ErrorResponse(
			409, "partfile_unsupported", "verify local data is not supported on a partfile");
	}

	CMD4Hash file_hash;
	if (!HashFromHex(s.hash, file_hash)) {
		return ErrorResponse(500, "internal_error", "failed to decode file hash");
	}

	auto ec_req = std::make_unique<CECPacket>(EC_OP_VERIFY_LOCAL_DATA);
	ec_req->AddTag(CECTag(EC_TAG_KNOWNFILE, file_hash));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for VERIFY_LOCAL_DATA");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	// 202, not 200: amuled queues a CVerifyLocalDataTask and answers NOOP
	// immediately, so the re-hash is still in flight here. The verdict is
	// only ever reported as an amule log line (CVerifyLocalDataTask::
	// PrintReport -> "Verify Local Data (...): Result OK" / "ERRORS
	// FOUND!"), which clients read back through /logs/amule or the SSE log
	// channel. No RefresherTick: nothing observable has changed yet.
	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

namespace
{
struct SharedDirEntry
{
	wxString path;
	bool recursive = false;
};

// The core's shared-directory op is a whole-list replace, so adding or removing
// a single root is a read-modify-write. Serialise those here: SendRecvSerialized
// locks per roundtrip, not across the pair, so two concurrent single-entry calls
// would otherwise read the same list and the second SET would drop the first's
// change. (Nothing can make this atomic against a simultaneous amuleGUI edit —
// the protocol has no compare-and-set — so that stays last-write-wins.)
std::mutex s_sharedDirsMutex;

// Read the current roots. Returns false and fills `err` when EC is unreachable
// or refuses; the caller turns that into an HTTP error.
bool FetchSharedDirs(CamuleapiApp &app, std::vector<SharedDirEntry> &out, std::string &err)
{
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_GET_SHARED_DIRS));
	const CECPacket *ec_resp = app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		err = "no reply from amuled";
		return false;
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		err = ec_err_msg;
		return false;
	}
	for (const CECTag &tag : *ec_resp) {
		if (tag.GetTagName() != EC_TAG_SHAREDDIR) {
			continue;
		}
		const CECTag *recursive_tag = tag.GetTagByName(EC_TAG_SHAREDDIR_RECURSIVE);
		SharedDirEntry entry;
		entry.path = tag.GetStringData();
		entry.recursive = recursive_tag != nullptr && recursive_tag->GetInt() != 0;
		out.push_back(entry);
	}
	delete ec_resp;
	return true;
}
// Replace the core's roots with `dirs` and render the outcome. The core applies
// every path that validates and reports the rest, so a single bad entry does not
// discard the edit.
CHttpServer::Response ApplySharedDirs(CamuleapiApp &app, const std::vector<SharedDirEntry> &dirs)
{
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SET_SHARED_DIRS));
	for (const SharedDirEntry &entry : dirs) {
		CECTag dir_tag(EC_TAG_SHAREDDIR, entry.path);
		if (entry.recursive) {
			dir_tag.AddTag(CECTag(EC_TAG_SHAREDDIR_RECURSIVE, (uint8)1));
		}
		ec_req->AddTag(dir_tag);
	}

	const CECPacket *ec_resp = app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "no reply from amuled");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(502, "amuled_rejected", ec_err_msg.c_str());
	}

	// One entry per submitted path, in the envelope every other multi-item
	// mutation uses. It reported only the rejects, in a shape of its own, so a
	// caller could not tell an applied path from one the response simply did
	// not mention. The reasons are still rendered here rather than shipped as
	// text from a core whose locale is not the caller's.
	std::map<std::string, std::string> rejected; // path -> reason code
	for (const CECTag &tag : *ec_resp) {
		if (tag.GetTagName() != EC_TAG_SHAREDDIR_REJECTED) {
			continue;
		}
		const CECTag *err_tag = tag.GetTagByName(EC_TAG_SHAREDDIR_ERROR);
		rejected[std::string(tag.GetStringData().utf8_str())] =
			(err_tag != nullptr && err_tag->GetInt() == 2) ? "not_readable" : "not_found";
	}
	delete ec_resp;

	std::vector<BulkItem> results;
	results.reserve(dirs.size());
	for (const SharedDirEntry &entry : dirs) {
		const std::string path(entry.path.utf8_str());
		const auto it = rejected.find(path);
		if (it == rejected.end()) {
			results.push_back(BulkOk(path));
		} else if (it->second == "not_readable") {
			results.push_back(BulkErr(path, 403, "not_readable", "amuled cannot read that path"));
		} else {
			results.push_back(BulkErr(path, 404, "not_found", "no such directory"));
		}
	}
	return BulkResultsResponse(results, 200);
}
} // namespace

// The core's configured share roots: the explicit ones and the recursive ones,
// each with the flag that says which. This is the *intent*, not the expansion —
// a recursive root yields one entry here however many subdirectories it covers,
// while /shared lists the files that expansion produced.
CHttpServer::Response CApiDispatcher::HandleSharedDirectories(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_GET_SHARED_DIRS));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "no reply from amuled");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(502, "amuled_rejected", ec_err_msg.c_str());
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("directories");
	w.BeginArray();
	for (const CECTag &tag : *ec_resp) {
		if (tag.GetTagName() != EC_TAG_SHAREDDIR) {
			continue;
		}
		const CECTag *recursive_tag = tag.GetTagByName(EC_TAG_SHAREDDIR_RECURSIVE);
		w.BeginObject();
		w.Key("path");
		w.ValueString(tag.GetStringData());
		w.Key("recursive");
		w.ValueBool(recursive_tag != nullptr && recursive_tag->GetInt() != 0);
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	delete ec_resp;
	FinalizeJsonBody(w, r);
	return r;
}

// Replace the whole set of roots. A full replace rather than add/remove verbs
// because that is exactly what the core's EC op does — expressing it as PUT
// keeps the API honest and avoids a read-modify-write race between two clients.
// The core validates each path (a REST client cannot stat the core's
// filesystem), applies the ones that pass and reports the rest, so a single bad
// entry does not discard the whole edit.
CHttpServer::Response CApiDispatcher::HandleSharedDirectoriesPut(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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
	const auto dirs_it = obj.find("directories");
	if (dirs_it == obj.end() || !dirs_it->second.is<picojson::array>()) {
		return ErrorResponse(400, "bad_request", "`directories` must be an array");
	}

	std::vector<SharedDirEntry> dirs;
	for (const auto &entry : dirs_it->second.get<picojson::array>()) {
		if (!entry.is<picojson::object>()) {
			return ErrorResponse(400, "bad_request", "each directory must be an object");
		}
		const auto &dir = entry.get<picojson::object>();
		const auto path_it = dir.find("path");
		if (path_it == dir.end() || !path_it->second.is<std::string>() ||
			path_it->second.get<std::string>().empty()) {
			return ErrorResponse(400, "bad_request", "each directory needs a non-empty `path`");
		}
		SharedDirEntry parsed;
		parsed.path = wxString::FromUTF8(path_it->second.get<std::string>().c_str());
		const auto rec_it = dir.find("recursive");
		if (rec_it != dir.end()) {
			if (!rec_it->second.is<bool>()) {
				return ErrorResponse(400, "bad_request", "`recursive` must be a boolean");
			}
			parsed.recursive = rec_it->second.get<bool>();
		}
		dirs.push_back(parsed);
	}

	// Whole-list replace: no read-modify-write, so no lock needed beyond the
	// per-roundtrip one SendRecvSerialized already holds.
	return ApplySharedDirs(m_app, dirs);
}

// Add one root, leaving the others alone. Idempotent: re-adding a configured
// path just updates its recursive flag, which is friendlier to scripts than a
// conflict. Read-modify-write, so it runs under s_sharedDirsMutex.
CHttpServer::Response CApiDispatcher::HandleSharedDirectoriesAdd(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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
	const auto path_it = obj.find("path");
	if (path_it == obj.end() || !path_it->second.is<std::string>() ||
		path_it->second.get<std::string>().empty()) {
		return ErrorResponse(400, "bad_request", "`path` must be a non-empty string");
	}
	bool recursive = false;
	const auto rec_it = obj.find("recursive");
	if (rec_it != obj.end()) {
		if (!rec_it->second.is<bool>()) {
			return ErrorResponse(400, "bad_request", "`recursive` must be a boolean");
		}
		recursive = rec_it->second.get<bool>();
	}
	const wxString wanted = wxString::FromUTF8(path_it->second.get<std::string>().c_str());

	std::lock_guard<std::mutex> guard(s_sharedDirsMutex);
	std::vector<SharedDirEntry> dirs;
	std::string ec_err;
	if (!FetchSharedDirs(m_app, dirs, ec_err)) {
		return ErrorResponse(503, "ec_unavailable", ec_err.c_str());
	}
	bool found = false;
	for (SharedDirEntry &entry : dirs) {
		if (entry.path == wanted) {
			entry.recursive = recursive;
			found = true;
			break;
		}
	}
	if (!found) {
		SharedDirEntry added;
		added.path = wanted;
		added.recursive = recursive;
		dirs.push_back(added);
	}
	return ApplySharedDirs(m_app, dirs);
}

// Remove one root. The path arrives as a query parameter rather than a path
// segment because it is an absolute filesystem path and would otherwise have to
// survive being spliced into the URL path. Unknown paths are a 404 so a typo is
// visible instead of silently succeeding.
CHttpServer::Response CApiDispatcher::HandleSharedDirectoriesDelete(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::string query;
	const std::size_t q = req.target.find('?');
	if (q != std::string::npos) {
		query = req.target.substr(q + 1);
	}
	const auto qmap = web_api_path::ParseQuery(query);
	const auto path_param = qmap.find("path");
	if (path_param == qmap.end() || path_param->second.empty()) {
		return ErrorResponse(400, "bad_request", "`path` query parameter is required");
	}
	const std::string wanted_utf8 = path_param->second;
	const wxString wanted = wxString::FromUTF8(wanted_utf8.c_str());

	std::lock_guard<std::mutex> guard(s_sharedDirsMutex);
	std::vector<SharedDirEntry> dirs;
	std::string ec_err;
	if (!FetchSharedDirs(m_app, dirs, ec_err)) {
		return ErrorResponse(503, "ec_unavailable", ec_err.c_str());
	}
	const size_t before = dirs.size();
	for (std::vector<SharedDirEntry>::iterator it = dirs.begin(); it != dirs.end(); ++it) {
		if (it->path == wanted) {
			dirs.erase(it);
			break;
		}
	}
	if (dirs.size() == before) {
		return ErrorResponse(404, "not_found", "no such shared directory");
	}
	return ApplySharedDirs(m_app, dirs);
}

namespace
{

// Send EC_OP_REFRESH_MEDIA_METADATA and turn the reply into a response.
// `hashTag` is null for the whole-share form.
//
// The op is deliberately not behind a capability tag, so a daemon that
// predates it answers EC_OP_FAILED rather than being detectable in advance.
// That is reported as 501, not 400: the request was well-formed and the
// server simply does not implement it, and a client that gets 501 knows to
// stop offering the action rather than to fix its input.
CHttpServer::Response SendMediaRefresh(CamuleapiApp &app, const CECTag *hashTag, const char *what)
{
	auto ec_req = std::make_unique<CECPacket>(EC_OP_REFRESH_MEDIA_METADATA);
	if (hashTag) {
		ec_req->AddTag(*hashTag);
	}
	const CECPacket *ec_resp = app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for media refresh");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		const bool unknown_op = ec_err_msg.find("Invalid opcode") != std::string::npos;
		delete ec_resp;
		if (unknown_op) {
			// 503, matching every other ec_unsupported site in this file and
			// the rule stated in App.cpp. 501 is arguably the better literal
			// answer for "server does not implement it", but one endpoint
			// disagreeing with seven is worse than either choice.
			return ErrorResponse(503,
				"ec_unsupported",
				"the connected amuled does not implement media metadata refresh");
		}
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	std::uint32_t queued = 0;
	if (const CECTag *t = ec_resp->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_QUEUED)) {
		queued = static_cast<std::uint32_t>(t->GetInt());
	}
	delete ec_resp;

	// 202, not 200: amuled queues the probes on its media-probe worker and
	// answers immediately, so nothing has been re-extracted yet. `queued` is
	// how many files were accepted for probing -- files the scheduler dropped
	// (not audio/video, an incomplete download, missing on disk) are not
	// counted. Progress is observable through the amule log and, as each
	// probe lands, the shared_updated SSE events.
	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("scope");
	w.ValueString(wxString::FromAscii(what));
	w.Key("queued");
	w.ValueInt(static_cast<int64_t>(queued));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleSharedMediaRefresh(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	return SendMediaRefresh(m_app, nullptr, "all");
}

CHttpServer::Response CApiDispatcher::HandleSharedMediaRefreshOne(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot s;
	if (!FindSharedByKey(m_state, key, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}
	// Same exclusion the daemon's Refresh mode applies: an in-progress
	// download has no complete file to read. Rejected here so the caller is
	// told why, rather than getting a 202 for a probe that was silently
	// dropped.
	if (s.IsIncompletePartfile()) {
		return ErrorResponse(409,
			"partfile_unsupported",
			"media metadata cannot be extracted from an incomplete download");
	}
	CMD4Hash file_hash;
	if (!HashFromHex(s.hash, file_hash)) {
		return ErrorResponse(500, "internal_error", "failed to decode file hash");
	}
	const CECTag hashTag(EC_TAG_KNOWNFILE, file_hash);
	return SendMediaRefresh(m_app, &hashTag, "file");
}

CHttpServer::Response CApiDispatcher::HandleSharedReload(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	// EC_OP_SHAREDFILES_RELOAD: amuled schedules a re-walk of every
	// configured share root and answers immediately, so 202 is literal --
	// accepted and scheduled, not completed. The walk starts on amuled's
	// next Process() tick (within about a second) and repeated calls while
	// one is pending or running coalesce into a single walk.
	//
	// This used to be synchronous on amuled's side, on the assumption that
	// the walk "completes in well under a second". That is exactly the
	// assumption that fails on the trees where it matters -- a large or
	// network-mounted share -- and because our EC lane is one serialised
	// worker, the blocked roundtrip held the in-flight slot, filled the
	// queue and turned unrelated endpoints into 503s.
	//
	// Completion is observable through the amule log (GET /logs/amule, or
	// the log_appended SSE event) and the shared_added / shared_removed
	// events, not through this response.
	//
	// SimpleConnControlOp still runs an inline RefresherTick. For this
	// endpoint it now snapshots state from before the walk, so it buys
	// nothing -- but it is harmless (a few EC roundtrips) and shared with
	// the connect/disconnect endpoints, so it stays as-is.
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
// A category priority is applied to member files as a DOWNLOAD priority
// (CDownloadQueue::SetCatPrio -> CPartFile::SetDownPriority), so it must use
// the same restricted set as DownloadPriorityToCode: low / normal / high /
// auto. very_low and release are not real download levels -- the .part.met
// loader clamps anything but PR_LOW/PR_NORMAL/PR_HIGH back to Normal on the
// next restart -- so accepting them here would be the same silent downgrade
// #396 fixed for the direct download PATCH path (issue #384).

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
			if (!FilePriorityToCode(it->second.get<std::string>(), kPrioCategory, out.prio)) {
				return ErrorResponse(
					400, "bad_request", FilePriorityAccepted(kPrioCategory).c_str());
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
	auto a = Authenticate(req);
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

// GET /categories/{index}. Every other resource with a member path has a member
// GET; this one had PATCH and DELETE only, so a client that had just created a
// category and wanted the stored result had to re-fetch the whole collection
// and search it by index. GUEST, matching the collection read.
CHttpServer::Response CApiDispatcher::HandleCategoryOne(
	const CHttpServer::Request &req, const std::string &index_str)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	std::uint8_t idx = 0;
	if (!ParseCategoryIndex(index_str, idx)) {
		return ErrorResponse(400, "bad_request", "path `{index}` must be a uint8 in [0, 255]");
	}
	if (auto r = RequireSnapshot(m_state))
		return *r;

	// The same set the collection lists, synthetic default included, so the
	// two routes cannot disagree about which categories exist.
	bool found = false;
	webapi::CategorySnapshot cat;
	for (const auto &c : CategoriesWithDefault(m_state)) {
		if (static_cast<std::uint8_t>(c.index) == idx) {
			cat = c;
			found = true;
			break;
		}
	}
	if (!found) {
		return ErrorResponse(404, "not_found", "no category with that index");
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteCategoryObject(w, cat);
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleCategoryUpdate(
	const CHttpServer::Request &req, const std::string &index_str)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint8_t idx = 0;
	if (!ParseCategoryIndex(index_str, idx)) {
		return ErrorResponse(400, "bad_request", "path `{index}` must be a uint8 in [0, 255]");
	}
	if (auto r = RequireSnapshot(m_state))
		return *r;

	// Find the existing category — we need its current values for any
	// field the PATCH body doesn't override (CEC_Category_Tag is
	// not delta-friendly; we always send the full tag).
	webapi::CategorySnapshot current;
	if (!FindCategoryByIndex(m_state, idx, current)) {
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
	(void)FindCategoryByIndex(m_state, idx, after);

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
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint8_t idx = 0;
	if (!ParseCategoryIndex(index_str, idx)) {
		return ErrorResponse(400, "bad_request", "path `{index}` must be a uint8 in [0, 255]");
	}
	if (auto r = RequireSnapshot(m_state))
		return *r;
	// Index 0 is the implicit "All" category — amuled treats deleting
	// it as illegal. Reject before the EC roundtrip.
	if (idx == 0) {
		return ErrorResponse(400, "bad_request", "cannot delete the default (index=0) category");
	}
	webapi::CategorySnapshot existing;
	if (!FindCategoryByIndex(m_state, idx, existing)) {
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

CHttpServer::Response CApiDispatcher::HandleClientBrowse(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	return HandleBrowse(req, ecid_str, /*by_friend=*/false);
}

CHttpServer::Response CApiDispatcher::HandleFriendBrowse(
	const CHttpServer::Request &req, const std::string &ecid_str)
{
	return HandleBrowse(req, ecid_str, /*by_friend=*/true);
}

// Shared by /clients/{ecid}/shared_files and /friends/{ecid}/shared_files.
// Same opcode and reply shape either way; only the sub-tag differs, which is
// what tells the daemon whether the id names a live peer or a friend record.
// The friend form is the more capable of the two: a friend carries a stored
// ip:port, so the daemon can build a client for it and browse a friend that is
// not currently connected.
CHttpServer::Response CApiDispatcher::HandleBrowse(
	const CHttpServer::Request &req, const std::string &ecid_str, bool by_friend)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	std::uint32_t ecid = 0;
	if (auto r = RequireEcidPath(ecid_str, ecid))
		return *r;

	// Ask amuled to browse this peer's shared file list. In multi-search mode
	// (amuleapi always is) the daemon allocates a browse search_id, echoes it in
	// the reply, and files the returned listing under it — so results, progress
	// and SSE all address the browse exactly like a search. MarkSearchStarted
	// then drives the refresher's per-id polling.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_FRIEND));
	CECEmptyTag sharedtag(EC_TAG_FRIEND_SHARED);
	sharedtag.AddTag(CECTag(by_friend ? EC_TAG_FRIEND : EC_TAG_CLIENT, ecid));
	ec_req->AddTag(sharedtag);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for browse");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		// The daemon replies FAILED "Client not found." for a stale/unknown ECID.
		return ErrorResponse(404, "not_found", ec_err_msg.c_str());
	}
	std::uint32_t search_id = 0;
	if (const CECTag *t = ec_resp->GetTagByName(EC_TAG_SEARCH_ID)) {
		search_id = static_cast<std::uint32_t>(t->GetInt());
	}
	delete ec_resp;
	if (search_id == 0) {
		return ErrorResponse(502, "amuled_rejected", "daemon did not return a search_id for browse");
	}

	// A browse's "query" is the peer whose share is being listed -- that is
	// what the daemon names the search, and what GET /search reports for it.
	// Take the nickname from the snapshot that actually owns this ECID so the
	// results envelope can label the tab; a peer we have no snapshot for
	// simply leaves it empty, same as any slot seeded before its name was
	// observed.
	//
	// Which collection depends on how the browse was addressed. CECID hands
	// out one global counter, so a CFriend's ECID never collides with a
	// client's -- but it never *matches* one either, and searching the client
	// list for it silently found nothing: every friend browse stored an empty
	// name while GET /search reported the real nick for the same id.
	std::string peer_name;
	if (by_friend) {
		for (const auto &f : m_state.Friends()) {
			if (f.ecid == ecid) {
				peer_name = f.name;
				break;
			}
		}
	} else {
		for (const auto &c : m_state.Clients()) {
			if (c.ecid == ecid) {
				peer_name = c.client_name;
				break;
			}
		}
	}
	m_state.MarkSearchStarted(search_id, "browse", peer_name);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("search_id");
	w.ValueInt(static_cast<int64_t>(search_id));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSearchStart(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
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
	// The daemon (in multi-search mode) allocates a globally-unique search_id
	// and echoes it in the START reply; every subsequent results/stop/more
	// call for this search is addressed by that id, so a reply without one
	// leaves the caller nothing to address and is reported as such.
	std::uint32_t search_id = 0;
	if (const CECTag *t = ec_resp->GetTagByName(EC_TAG_SEARCH_ID)) {
		search_id = static_cast<std::uint32_t>(t->GetInt());
	}
	delete ec_resp;
	if (search_id == 0) {
		return ErrorResponse(
			502, "amuled_rejected", "daemon did not return a search_id for SEARCH_START");
	}

	// Seed this search's slot: the refresher polls EC_OP_SEARCH_RESULTS +
	// _PROGRESS for it (addressed by search_id) each tick until the daemon
	// reports completion. This is the single fetcher, so SSE
	// search_result_added / search_progress fire on the same delta a polling
	// consumer would observe. The query rides along so the results envelope
	// can report what this search was for.
	m_state.MarkSearchStarted(search_id, search_kind, query);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.Key("search_id");
	w.ValueInt(static_cast<int64_t>(search_id));
	w.Key("query");
	w.ValueString(wxString::FromUTF8(query.c_str()));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// The three per-search actions share one EC exchange: address the search by
// EC_TAG_SEARCH_ID, send, and turn a failure reply into a 400. Only the
// opcode, the optional close flag and the success shape differ, so the
// exchange itself lives here rather than three times over.
CHttpServer::Response CApiDispatcher::SendSearchOp(
	ec_opcode_t opcode, std::uint32_t search_id, bool close, int success_status, int *out_more_reaskable)
{
	std::unique_ptr<CECPacket> ec_req(new CECPacket(opcode));
	// Always addressed. The old no-id form let the daemon decide what "stop"
	// meant when the caller had no current search; a concrete id is the only
	// way to stop the right one when several are running.
	ec_req->AddTag(CECTag(EC_TAG_SEARCH_ID, search_id));
	if (close) {
		ec_req->AddTag(CECEmptyTag(EC_TAG_SEARCH_CLOSE));
	}
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for the search operation");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	if (out_more_reaskable) {
		// Left at its caller-set -1 when the tag is absent: that is a daemon
		// older than it, whose answer is unknown rather than negative.
		if (const CECTag *t = ec_resp->GetTagByName(EC_TAG_SEARCH_MORE_REASKABLE)) {
			*out_more_reaskable = t->GetInt() != 0 ? 1 : 0;
		}
	}
	delete ec_resp;

	CHttpServer::Response r;
	r.status = success_status;
	if (success_status == 204) {
		// No body, per RFC 9110: a 204 must not carry one. Clearing the
		// content type explicitly is what the other 204 handlers do --
		// a default-constructed Response does not necessarily start empty.
		r.content_type.clear();
		return r;
	}
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("ok");
	w.ValueBool(true);
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSearchStop(
	const CHttpServer::Request &req, std::uint32_t search_id)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto rej = RequireSearch(search_id))
		return *rej;

	// Stop only: the results stay readable -- amuled keeps them until the
	// search is closed or evicted -- so a consumer viewing this search sees
	// the same set it was just looking at. Siblings are untouched.
	return SendSearchOp(EC_OP_SEARCH_STOP, search_id, /*close=*/false, 200);
}

CHttpServer::Response CApiDispatcher::HandleSearchClose(
	const CHttpServer::Request &req, std::uint32_t search_id)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto rej = RequireSearch(search_id))
		return *rej;

	CHttpServer::Response r = SendSearchOp(EC_OP_SEARCH_STOP, search_id, /*close=*/true, 204);
	if (r.status != 204) {
		return r;
	}
	// Drop the local slot too, so its polling stops and a later
	// GET /search/{id}/results is a 404. The vanished slot is also what
	// makes the next diff pass publish `search_closed` to SSE subscribers.
	m_state.CloseSearch(search_id);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSearchMore(
	const CHttpServer::Request &req, std::uint32_t search_id)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto rej = RequireSearch(search_id))
		return *rej;

	// The desktop "More" button re-asks already-queried Kad peers for a wider
	// result frontier. Both constraints below mirror what that button does
	// rather than what the core tolerates: CSearchManager::RequestMoreResults
	// returns false for a non-Kad id and the GUI greys the button out once the
	// search finishes, so forwarding either case would turn a user's request
	// into a silent no-op with a 202 on top of it.
	const webapi::SearchProgressSnapshot progress = m_state.SearchProgress(search_id);
	if (progress.kind != "kad") {
		return ErrorResponse(400, "bad_request", "`more` applies to Kad searches only");
	}
	if (progress.complete || !progress.active) {
		return ErrorResponse(
			400, "bad_request", "`more` applies to a running search; this one has finished");
	}

	// The daemon logs what actually happened and answers with the other half:
	// whether a LATER press could still widen this search. False is terminal --
	// the reask budget of 4 is spent, or the search is inside the stopping
	// window Kad enters 20 s before a keyword search ends, which is most of the
	// second half of its life. Reporting that as 202 is what let a client press
	// "More" five times while the daemon did nothing.
	//
	// A press that simply has no responded peer left to reask *yet* still gets
	// 202: it clears as soon as another peer answers, and retrying is genuinely
	// the right next action.
	int reaskable = -1;
	CHttpServer::Response r =
		SendSearchOp(EC_OP_SEARCH_REQUEST_MORE, search_id, /*close=*/false, 202, &reaskable);
	// Only reinterpret a success. An error from the exchange is its own answer,
	// and -1 means the daemon never reported -- keep today's 202 for it.
	if (r.status == 202 && reaskable == 0) {
		return ErrorResponse(409,
			"kad_more_exhausted",
			"this Kad search cannot be widened any further (reask budget spent, or the "
			"search is in its final seconds)");
	}
	return r;
}

CHttpServer::Response CApiDispatcher::HandleSearchDownload(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// Canonicalise the URL hash to lowercase.
	const std::string needle = LowerHexKey(hash);

	CMD4Hash file_hash;
	if (!HashFromHex(needle, file_hash)) {
		return ErrorResponse(400, "bad_request", "`{hash}` must be a 32-char hex MD4");
	}

	// Optional body: {"category": uint8, "ecid": uint32}. amulegui's
	// CDownQueueRem::AddSearchToDownload defaults to category 0
	// when none is supplied; we mirror that. The body itself is
	// optional — clients that don't care about category POST with
	// no body and get the default download path. `ecid` (issue #431)
	// selects one same-hash/different-name grouped child (from a result's
	// `children[].ecid`) so it downloads under that chosen filename;
	// omitted => the parent (first result matching the hash).
	std::uint8_t category = 0;
	bool has_ecid = false;
	std::uint32_t ecid = 0;
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
		const auto eit = obj.find("ecid");
		if (eit != obj.end()) {
			if (!eit->second.is<double>()) {
				return ErrorResponse(
					400, "bad_request", "`ecid` must be a non-negative integer");
			}
			const double v = eit->second.get<double>();
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request", "`ecid` out of range");
			}
			ecid = static_cast<std::uint32_t>(v);
			has_ecid = true;
		}
	}

	// amuled accepts the result hash as the partfile-tag's int
	// payload (matches amule-remote-gui.cpp:2230). amuled looks up
	// the hash in its searchlist; if not present, returns FAILED. When
	// an `ecid` selector is supplied, it rides as an EC_TAG_SEARCHFILE
	// child and amuled downloads that specific grouped result instead.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_DOWNLOAD_SEARCH_RESULT));
	CECTag hash_tag(EC_TAG_PARTFILE, file_hash);
	hash_tag.AddTag(CECTag(EC_TAG_PARTFILE_CAT, category));
	if (has_ecid) {
		hash_tag.AddTag(CECTag(EC_TAG_SEARCHFILE, ecid));
	}
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

// GET /search/results/{hash}/comments — community ratings/comments for one
// search result (issue #434): the Kad notes retrieved so far plus the running
// flag. Mirrors GET /downloads/{hash}/comments for single-hash polling.
CHttpServer::Response CApiDispatcher::HandleSearchComments(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	const std::string needle = LowerHexKey(hash);

	// Locate the result carrying this hash across ALL open searches — the
	// comments endpoints are search-agnostic (the same file may appear in
	// several searches). Grouped children share the parent's hash, so the
	// parent, which owns any fetched notes, matches first.
	webapi::SearchResult hit;
	std::uint32_t owner_search_id = 0;
	if (!m_state.FindSearchResultByHash(needle, hit, &owner_search_id)) {
		return ErrorResponse(404, "not_found", "no search result with that hash");
	}
	// This is THE polling path for a Kad notes lookup: the notes only reach
	// amuleapi through the owning search's result fetch, and a finished
	// search is never fetched by the tick. Without this refresh a lookup
	// started after the search completed -- which is when a user actually
	// reads a result list -- would leave `kad_comment_search_running` stuck
	// and `comments` empty forever. Re-read the hit afterwards so the
	// response reflects the refresh rather than the snapshot before it.
	RefreshSearchIfStale(owner_search_id);
	// A refresh can drop the hit -- the daemon frees a search's results when
	// it is closed, and the set is rebuilt wholesale rather than merged. The
	// bool was discarded here, so that case answered 200 from the pre-refresh
	// copy: a result the daemon no longer has, reported as if it did.
	if (!m_state.FindSearchResultByHash(needle, hit, nullptr)) {
		return ErrorResponse(404, "not_found", "no search result with that hash");
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("count");
	w.ValueInt(static_cast<int64_t>(hit.comments.size()));
	w.Key("kad_comment_search_running");
	w.ValueBool(hit.kad_comment_searching);
	w.Key("comments");
	w.BeginArray();
	for (const auto &c : hit.comments) {
		w.BeginObject();
		w.Key("username");
		w.ValueString(wxString::FromUTF8(c.username.c_str()));
		w.Key("filename");
		w.ValueString(wxString::FromUTF8(c.filename.c_str()));
		w.Key("rating");
		w.ValueInt(static_cast<int64_t>(c.rating));
		w.Key("comment");
		w.ValueString(wxString::FromUTF8(c.comment.c_str()));
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// POST /search/results/{hash}/comments — trigger an on-demand Kad NOTES lookup
// for a search result the user has not downloaded (issue #434). Asynchronous on
// amuled (up to ~45s); retrieved notes then appear via GET here and on the
// /search/results list. Returns 202 Accepted.
CHttpServer::Response CApiDispatcher::HandleSearchCommentsKadSearch(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// Admin-only, like every other mutation. This drives an unbounded Kad
	// NOTES lookup on the daemon, so a guest session must not reach it.
	if (auto r = RequireAdmin(a))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	const std::string needle = LowerHexKey(hash);

	CMD4Hash file_hash;
	if (!HashFromHex(needle, file_hash)) {
		return ErrorResponse(400, "bad_request", "`{hash}` must be a 32-char hex MD4");
	}

	// Must be a live search result in some open search (mirrors the download
	// endpoint's 404). The daemon runs one Kad NOTES lookup per hash and fans
	// the notes out to every same-hash result, so the specific search doesn't
	// matter here.
	webapi::SearchResult known_hit;
	std::uint32_t owner_search_id = 0;
	if (!m_state.FindSearchResultByHash(needle, known_hit, &owner_search_id)) {
		return ErrorResponse(404, "not_found", "no search result with that hash");
	}
	auto ec_req = std::make_unique<CECPacket>(EC_OP_SHARED_FILE_SEARCH_KAD_NOTES);
	ec_req->AddTag(CECTag(EC_TAG_KNOWNFILE, file_hash));
	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for SEARCH_KAD_NOTES");
	}
	std::string ec_err;
	if (IsEcFailedResponse(ec_resp, ec_err)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err.c_str());
	}
	delete ec_resp;

	// Refresh AFTER the lookup has been started, for the same reason the GET
	// refreshes at all: a finished search is otherwise frozen, and the flag
	// this POST turns on would never be observed turning off again.
	//
	// Order matters. Refreshing first cached a pre-lookup snapshot and spent
	// the one-second ClaimSearchRefresh token on it, so a GET issued straight
	// afterwards fell inside the TTL and reported
	// `kad_comment_search_running: false` for a lookup that had just begun --
	// breaking the poll-until-false loop the docs prescribe.
	RefreshSearchIfStale(owner_search_id);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("status");
	w.ValueString("kad_search_started");
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
// CORS for the replies the transport builds without a parsed request: the
// read-side limits and the request timeout. Takes the raw Origin header
// because at that point there is no Request to resolve one from.
void CApiDispatcher::StampCorsForTransport(
	std::map<std::string, std::string> &headers, const std::string &origin_header)
{
	CHttpServer::Request synthetic;
	if (!origin_header.empty()) {
		synthetic.headers.emplace("Origin", origin_header);
	}
	const std::string cors_org = ResolveCorsOrigin(synthetic, m_config);
	ApplyCorsHeaders(headers, cors_org, m_config.ServerCfg().allow_cors);
}

boost::optional<CHttpServer::Response> CApiDispatcher::PreflightEvents(const CHttpServer::Request &req)
{
	// Same bearer/cookie check the live handler used to do, but run
	// on the I/O thread BEFORE a worker thread is spawned and BEFORE
	// the 32-slot SSE budget is touched. Unauth/locked-out peers
	// get a normal request/response 401/429 and never reach the
	// streaming path; the slot stays free for legitimate
	// subscribers.
	auto a = Authenticate(req);
	if (!a.ok) {
		// CORS on the rejection too. The HEAD probe below was moved
		// into this function to pick the bundle up, and leaving the
		// 401/403/429 without it means a cross-origin SSE client sees
		// an opaque fetch failure exactly when it most needs to read
		// why it was turned away.
		{
			const std::string cors_org = ResolveCorsOrigin(req, m_config);
			ApplyCorsHeaders(a.rejection.headers, cors_org, m_config.ServerCfg().allow_cors);
		}
		return a.rejection;
	}
	// A HEAD here asks what a GET would answer with, not for the stream.
	// Answered in the dispatcher rather than in the transport so it picks
	// up the same CORS bundle the stream itself carries -- built in the
	// transport it had none, and a credentialed cross-origin HEAD was
	// blocked by the browser while the GET succeeded.
	if (req.method == "HEAD") {
		CHttpServer::Response probe;
		probe.status = 200;
		probe.content_type = "text/event-stream";
		probe.headers["Cache-Control"] = "no-cache";
		probe.headers["X-Accel-Buffering"] = "no";
		// Mirror the encoding the GET would negotiate. The probe body
		// is empty, so nothing downstream will compress it and the
		// header has to be stated: without it a HEAD says identity
		// while the stream it stands for arrives gzipped, which is the
		// same divergence that makes a HEAD on any other route unsafe
		// to cache against.
		if (AcceptsGzip(FindHeaderCaseInsensitive(req.headers, "Accept-Encoding"))) {
			probe.headers["Content-Encoding"] = "gzip";
		}
		// NOT keep-alive: this connection is answered and closed, and
		// advertising reuse makes a pooling client fail its next
		// request on a socket we already shut down.
		probe.headers["Connection"] = "close";
		{
			const std::string cors_org = ResolveCorsOrigin(req, m_config);
			ApplyCorsHeaders(probe.headers, cors_org, m_config.ServerCfg().allow_cors);
		}
		return probe;
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
	auto a = Authenticate(req);
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
		// identifies the channel. The only no-underscore name is
		// `resync`, which the caller bypasses by name: it reaches the
		// wire both synthesised per subscriber and published on the
		// bus, and a cache invalidation is not opt-out-able either
		// way. Future bare-token events need explicit channel mapping
		// or must always bypass like `resync`.
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
		if (prefix == "friend")
			return "friends";
		if (prefix == "status")
			return "status";
		if (prefix == "log")
			return "logs";
		if (prefix == "search")
			return "search";
		// Plural, matching the /chats collection: the bootstrap advice is to
		// GET the collections matching your subscribed channels, which only
		// works if the two names line up.
		if (prefix == "chat")
			return "chats";
		return prefix;
	};
	auto event_passes_filter = [&](const std::string &name) {
		if (!channels_set)
			return true;
		// A cache invalidation is not opt-out-able, and this one arrives over
		// the bus rather than synthesised per subscriber, so it has to bypass
		// here as well as in the reconnect path.
		if (name == "resync")
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
	// Registers this session for the life of the stream, so the refresher knows
	// to resume diffing.
	webapi::CEventBus::Subscription subscription(m_app.EventBus());

	std::uint64_t since_id;
	const std::string lei = FindHeaderCaseInsensitive(req.headers, "Last-Event-ID");
	const std::uint64_t newest = m_app.EventBus().NewestId();
	const std::uint64_t oldest = m_app.EventBus().OldestId();
	if (lei.empty()) {
		// No cursor to invalidate: the client GETs the collections itself.
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
