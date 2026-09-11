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
#include "ServerFlagNames.h"

#include "config.h" // AMULEAPI_STATIC_DIR (compile-time install path)
#include "AmuleApiConfig.h"
#include "App.h"
#include "Auth.h"
#include "ConstantTime.h"
#include "Etag.h"
#include "JsonWriter.h"
#include "../PeerCapabilities.h" // Needed for CPeerCapabilities::GetApiTokens

#include <mutex> // serialises the shared-directory read-modify-write
#include "Jwt.h"
#include "PathPatterns.h"
#include "Refresher.h"     // ParseStatsTreeFromPacket / ParseGraphsFromPacket / ApplySearchFull
#include "StaticFs.h"      // IsDir, ResolveWithinRoot
#include "SharedContent.h" // /shared/{hash}/content: path resolution, Range, disposition
#include "PartIndex.h"     // UsablePartIndex / UsableLastDownloadingPart, unit-tested standalone
#include <cmath>
#include <cstring>
#include <map>

#include "PrefsSchema.h"
#include "SearchJson.h" // WriteSearchResultFields, shared with the SSE payload
#include "State.h"

#include "Constants.h"
#include "OtherFunctions.h" // GetFiletypeByName for the shared file_type token
#include <common/Path.h>    // CPath
#include <icon_data.h>      // amule_find_icon -- country flags for GET /flags/{code}.png

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

// strncasecmp is in <strings.h> on POSIX; glibc also exposes it via
// <string.h>, but musl and the BSDs do not.
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#include "PicoJson_Inc.h"

#include "config.h"      // VERSION
#include "MuleVersion.h" // Needed for GetShortMuleVersion()

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

// Emit `key` as a number, or null when the value is not known. The rule lives in
// REFERENCE.md under `Unknown values`; this is the one implementation, so a new
// nullable field cannot quietly pick -1 or 0 instead.
void WriteIntOrNull(CJsonWriter &w, const char *key, bool known, std::int64_t value)
{
	w.Key(key);
	if (known)
		w.ValueInt(value);
	else
		w.ValueNull();
}

void WriteUIntOrNull(CJsonWriter &w, const char *key, bool known, std::uint64_t value)
{
	w.Key(key);
	if (known)
		w.ValueUInt(value);
	else
		w.ValueNull();
}

// Callers pass the predicate rather than letting this guess: `false` and "not
// measured" are different answers for a firewall verdict.
void WriteBoolOrNull(CJsonWriter &w, const char *key, bool known, bool value)
{
	w.Key(key);
	if (known)
		w.ValueBool(value);
	else
		w.ValueNull();
}

void WriteStringOrNull(CJsonWriter &w, const char *key, bool known, const std::string &value)
{
	w.Key(key);
	if (known)
		w.ValueString(wxString::FromUTF8(value.c_str()));
	else
		w.ValueNull();
}

void FinalizeJsonBody(CJsonWriter &w, CHttpServer::Response &r)
{
	// The writer already holds UTF-8, so this is a move. Never route it through
	// wxString: amuleapi calls neither setlocale nor wxLocale, so it runs in the "C"
	// locale and wxString's std::string ctor decodes with the locale, emptying any
	// body with a non-ASCII byte.
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

// RFC 9110 15.5.6 requires an Allow header on a 405. `allow` is the
// machine-readable list, comma-separated in RFC order, HEAD wherever GET is served.
CHttpServer::Response MethodNotAllowed(const char *allow, const char *message)
{
	CHttpServer::Response r = ErrorResponse(405, "method_not_allowed", message);
	r.headers["Allow"] = allow;
	return r;
}

// Pulls the JWT from the Authorization header or the session cookie, verifies it
// and rejects revoked tokens. The header wins when both are present: an explicit
// bearer header signals intent over an implicit cookie.
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
		// Header names are case-insensitive but Beast preserves what the client
		// sent, so a lowercase `authorization:` slips past the literal find.
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
	// A password change ends the sessions the old password opened. The cutoff is the
	// credential file's own mtime, so it holds however the change was made -- REST,
	// CLI, preferences dialog, amulegui -- and survives a restart.
	if (credentials_changed_at > 0 && out.verified.iat < credentials_changed_at) {
		out.rejection =
			ErrorResponse(401, "unauthorized", "credentials changed; please sign in again");
		return out;
	}
	out.ok = true;
	return out;
}

// Admin gate: `if (auto r = RequireAdmin(a)) return *r;`
std::unique_ptr<CHttpServer::Response> RequireAdmin(const AuthOutcome &a)
{
	if (a.verified.role != Role::ADMIN) {
		return std::make_unique<CHttpServer::Response>(
			ErrorResponse(403, "forbidden", "admin role required for this endpoint"));
	}
	return nullptr;
}

// First-snapshot gate, used like RequireAdmin. Until the first EC snapshot lands
// there is nothing to answer from, and the status, code and sentence are API
// contract rather than local wording.
std::unique_ptr<CHttpServer::Response> RequireSnapshot(const webapi::CState &state)
{
	if (!state.HasFirstSnapshot()) {
		return std::make_unique<CHttpServer::Response>(ErrorResponse(
			503, "ec_unavailable", "amuleapi has not received its first EC snapshot yet"));
	}
	return nullptr;
}

// The URL carries a hash in whatever case the caller typed; the snapshot keys
// everything lowercase. The two lookups that always follow come with it.
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

// AuthenticateRequest behind a per-IP failure counter: every 401 counts, and a
// filled bucket gets 429 with Retry-After. Checked BEFORE Verify() so a locked-out
// IP cannot burn CPU on MAC compares. Login keeps its own limiter for the
// password-failure path.
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

// `<name>=<value>; HttpOnly; SameSite=Strict; Path=/api/v0; Max-Age=<lifetime>`
//
// No `Secure`: amuleapi serves HTTP by design, with TLS terminated in front.
// Shared with the clear-cookie path because RFC 6265 5.3 requires (name, path,
// domain) to match for a delete.
const char *const kSessionCookieAttrs = "; HttpOnly; SameSite=Strict; Path=/api/v0";

std::string MakeSetCookie(const std::string &name, const std::string &value, std::time_t expires_at)
{
	const std::time_t now = std::time(nullptr);
	// An already-expired `expires_at` yields Max-Age=0, which deletes the cookie on
	// receipt (RFC 6265 5.2.2): an expired token must not grant a working session.
	const std::time_t lifetime = expires_at > now ? expires_at - now : 0;
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

// Max-Age=0 invalidates whatever a prior login set. MUST use the same
// (name, path, domain) tuple as MakeSetCookie or the browser keeps the original.
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

// Namespaced apart from amuleweb's `amule_token` so the two daemons can
// coexist behind one host without a Set-Cookie tug-of-war.
const char *const kSessionCookieName = "amuleapi_token";

// Ceiling for one static-asset read: keeps a StaticRoot pointing at /dev/zero
// or a multi-GB log file from exhausting daemon RAM.
constexpr std::size_t kStaticMaxFileBytes = 16 * 1024 * 1024;

// Extension to Content-Type; unknown maps to application/octet-stream, so a
// wrong-type response on an attacker-named file cannot amplify XSS.
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

// Slurp into `out`; false on a non-regular file, oversize, or read error. `st` is
// filled on success so the caller can ETag from mtime + size without re-stat'ing.
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

// "mtime-size" hex ETag, the shape nginx defaults to, strong-form quoted per RFC
// 7232. Enough for a local frontend, where daemon and file system are colocated.
std::string BuildStaticEtag(const struct stat &st)
{
	std::ostringstream oss;
	oss << '"' << std::hex << static_cast<std::uint64_t>(st.st_mtime) << '-'
	    << static_cast<std::uint64_t>(st.st_size) << '"';
	return oss.str();
}

// Resolve the default static directory when amuleapi.conf's [Server]/StaticRoot is
// empty. Mirrors amuleweb's GetTemplateDir (src/webserver/src/WebInterface.cpp):
// the macOS .app bundle's Resources/ first (so an installed aMule.app surfaces the
// bundled frontend without a conf edit), then a copy beside the running binary
// (the relocatable Linux static tarball), then AMULEAPI_STATIC_DIR, then
// wxStandardPaths' platform-adjusted resource dir. First existing one wins.
std::string ResolveDefaultStaticDir()
{
	const std::string asset = "amuleapi-static";

#ifdef __WXMAC__
	// LaunchServices lookup for the installed aMule.app. Picks up the bundled
	// placeholder when the operator launched amuleapi from a path-registered .app.
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

	// Beside the running binary: the Linux static tarball's layout (binaries plus
	// amuleapi-static/, extracted anywhere, no install step). Every other candidate
	// resolves through a resources directory, which a relocatable bundle has none of.
	//
	// The executable's directory, not the working directory: a daemon is commonly
	// started from ~ or /, and resolving against cwd would let a directory an
	// unprivileged user can create decide what a root daemon serves.
	{
		const wxString exe = wxStandardPaths::Get().GetExecutablePath();
		if (!exe.empty()) {
			wxFileName exe_dir(exe);
			exe_dir.SetFullName(wxEmptyString);
			const wxString cand = wxFileName(exe_dir.GetPath(), asset).GetFullPath();
			const std::string s(cand.utf8_str());
			if (webapi::IsDir(s))
				return s;
		}
	}

#ifdef AMULEAPI_STATIC_DIR
	if (webapi::IsDir(AMULEAPI_STATIC_DIR)) {
		return std::string(AMULEAPI_STATIC_DIR);
	}
#endif

	// wxStandardPaths fallback, with the same platform adjustments amuleweb
	// applies for its `webserver/` lookup.
	wxString dir = wxStandardPaths::Get().GetResourcesDir();
#if defined(__WINDOWS__)
	// Installer layout: bin\amuleapi.exe + share\amule\amuleapi-static\.
	// wxStandardPaths returns the exe directory on Windows, so climb into share/amule/.
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
// Generic-401 limiter against credential stuffing across every authenticated
// endpoint, counting rejected TOKENS rather than bad passwords. Tunable via the
// `[Auth]/Token*` keys because this is the one a browser tab left open overnight
// trips.
m_authRateLimiter(webapi::CRateLimiter::Config{ config.AuthCfg().token_failure_window_seconds,
	config.AuthCfg().token_failure_threshold,
	config.AuthCfg().token_lockout_seconds })
{
}

namespace
{

// Beast preserves the wire-form casing the client sent, so a literal find
// misses lowercased headers. Walks the map once on a miss.
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

// Resolve the CORS Origin echo: the verbatim Origin for
// `Access-Control-Allow-Origin` plus whether the allowlist named it. Empty when
// CORS is off, the request carried no Origin, or the allowlist rejected it.
// `allow_cors=1` with an empty allowlist echoes verbatim, which is `*`-equivalent;
// credentials are NOT granted on that path (see ApplyCorsHeaders).
struct CorsDecision
{
	std::string origin;
	bool allowlisted = false;
};

CorsDecision ResolveCorsOrigin(const CHttpServer::Request &req, const CAmuleApiConfig &cfg)
{
	if (!cfg.ServerCfg().allow_cors)
		return CorsDecision{};
	const std::string origin = FindHeaderCaseInsensitive(req.headers, "Origin");
	if (origin.empty())
		return CorsDecision{};
	const auto &list = cfg.ServerCfg().cors_origin_allowlist;
	if (list.empty())
		return CorsDecision{ origin, false }; // echo any origin, no credentials
	for (const auto &allowed : list) {
		if (allowed == origin)
			return CorsDecision{ origin, true };
	}
	return CorsDecision{};
}

// `Vary: Origin` goes on whenever CORS is enabled, even for a rejected origin, so
// an intermediary cannot cache a cross-origin response against a same-origin key.
// The auth and content headers go on only if it was allowed.
void ApplyCorsHeaders(
	std::map<std::string, std::string> &headers, const CorsDecision &cors, bool cors_enabled)
{
	if (!cors_enabled)
		return;
	AppendHeaderToken(headers, "Vary", "Origin");
	if (cors.origin.empty())
		return;
	headers["Access-Control-Allow-Origin"] = cors.origin;
	// Credentials only for an origin the operator named. With an empty allowlist the
	// echo is `*`-equivalent, and granting credentials there would let any site the
	// user visits call this API with their session cookie and read the replies. An
	// anonymous cross-origin read still works.
	if (cors.allowlisted)
		headers["Access-Control-Allow-Credentials"] = "true";
	// What a client may read from `fetch().headers.get(...)`: the Fetch spec exposes
	// only the CORS-safelisted response headers, and clients need ETag to validate.
	headers["Access-Control-Expose-Headers"] = "ETag, Allow, Retry-After";
}

// The Kad `network` rollup, byte-identical under `kad` on GET /status and on
// GET /kad: both read the same KadSnapshot from one Dashboard() acquisition.
void WriteKadNetworkObject(CJsonWriter &w, const webapi::KadSnapshot &k)
{
	w.Key("network");
	w.BeginObject();
	// null unless Kad is connected. user_count/file_count are the last estimate and
	// survive into `connecting`; node_count is our own routing-table size, measured at
	// 2 with Kad fully stopped, so not even the terminal state reaches 0. A number
	// here would claim knowledge of a network we are not on.
	WriteIntOrNull(w, "user_count", k.has_network, static_cast<int64_t>(k.users));
	WriteIntOrNull(w, "file_count", k.has_network, static_cast<int64_t>(k.files));
	WriteIntOrNull(w, "node_count", k.has_network, static_cast<int64_t>(k.nodes));
	w.EndObject();
}

// The {id} segment of every search-scoped route: a non-zero decimal fitting a
// uint32. Zero is rejected rather than read as "no search": it used to mean
// "whichever search this session started last", and letting it through would
// resurrect the implicit-target behaviour these routes exist to remove.
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

bool ParseJsonObjectBody(const std::string &body, picojson::value &out, std::string &err);

} // namespace

// Did the caller present credentials at all? Mirrors what Authenticate() accepts,
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
	const CorsDecision cors_org = ResolveCorsOrigin(req, m_config);

	// Browser preflights carry no credentials, so they skip the auth gate and the
	// route handler: 204 plus the CORS bundle, or 204 with `Vary: Origin` alone when
	// the origin is rejected, which makes the browser block the real request.
	if (req.method == "OPTIONS" &&
		!FindHeaderCaseInsensitive(req.headers, "Access-Control-Request-Method").empty()) {
		CHttpServer::Response pre;
		pre.status = 204;
		pre.content_type.clear();
		ApplyCorsHeaders(pre.headers, cors_org, cors_enabled);
		if (!cors_org.origin.empty()) {
			pre.headers["Access-Control-Allow-Methods"] =
				"GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS";
			// Headers actual requests may send: Authorization for bearer,
			// If-None-Match for conditional GET, Last-Event-ID for SSE replay.
			pre.headers["Access-Control-Allow-Headers"] =
				"Authorization, Content-Type, If-None-Match, Last-Event-ID";
			pre.headers["Access-Control-Max-Age"] = "86400";
		}
		return pre;
	}

	// ETag + If-None-Match -> 304, on GET/HEAD 200 only: a mutation's response carries
	// post-mutation state the client always wants delivered.
	//
	// The revision is sampled before AND after the handler. The body was serialized
	// under a read lock already dropped, so reading the revision only afterwards can
	// pair etag(old body) with the NEW revision, and every later hit then serves a
	// validator describing neither. If anything moved, the response is not memoized.
	const std::uint64_t rev_before = m_state.SnapshotRevision();
	CHttpServer::Response resp = DispatchToHandler(req);
	const std::uint64_t rev_after = m_state.SnapshotRevision();

	const bool is_safe_method = (req.method == "GET" || req.method == "HEAD");
	// A handler that computed its own validator owns it. Stamping the body hash over
	// the top would yield two ETags for one resource depending on which branch
	// answered -- as the static path did, since it clears the body for HEAD so only
	// the GET reached the hashing branch.
	const bool handler_set_etag = (resp.headers.find("ETag") != resp.headers.end());
	if (webapi::ShouldStampEtag(is_safe_method, handler_set_etag, resp.status, resp.body.empty())) {
		// Skip the MD5 over a multi-MB body when nothing has changed. The key is
		// (target, snapshot revision): a revision advances on every eligible write,
		// so unlike a timestamp it cannot stand still through a mutation or collapse
		// two changes inside one second.
		//
		// Opt-in per target (MemoizableTarget), covering only /downloads and
		// /shared. MemoUsable is the other half: without it the key says which
		// revision was current when we looked, not which one this body came from.
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
					// Crude memory backstop: the real workload is a few dozen
					// targets, so a wholesale clear beats real LRU machinery.
					m_etagCache.clear();
				}
				EtagCacheEntry e;
				e.snapshot_rev = snap;
				e.etag = etag;
				m_etagCache[req.target] = std::move(e);
			}
		}
		// RFC 7232 2.3: the value MUST be quoted.
		//
		// Which representation does this validator name? The transport appends the
		// coding when it compresses, but a 304 carries no body to compress, so the
		// answer is worked out here while the body is still present -- and must be the
		// SAME answer, or a client that cached the gzip form and gets the identity ETag
		// can never match its stored response again.
		const bool coded = WillCompressBody(
			AcceptsGzip(FindHeaderCaseInsensitive(req.headers, "Accept-Encoding")),
			resp.body.size(),
			resp.content_type,
			resp.headers.find("Content-Encoding") != resp.headers.end());
		const std::string wire_etag = webcommon::WithCodingSuffix(etag, coded);
		resp.headers["ETag"] = "\"" + wire_etag + "\"";

		// Against wire_etag, which names the representation THIS request selected.
		// Matching either coding would defeat the suffix: a client holding gzip bytes
		// and asking for identity would be told its copy is current.
		const std::string inm = FindHeaderCaseInsensitive(req.headers, "If-None-Match");
		if (webcommon::IfNoneMatchHits(inm, wire_etag)) {
			// 304 carries no body and no Content-Type, but the ETag header IS
			// preserved (RFC 7232 4.1): clients re-stamp the cached copy with it.
			resp.status = 304;
			resp.body.clear();
			resp.content_type.clear();
		}
	}
	// HEAD carries no content on ANY status. The strip used to sit inside the 200-only
	// block, so a HEAD ending in 4xx shipped the JSON error envelope -- content RFC
	// 9110 9.3.2 forbids, and bytes a correct client leaves in the socket to corrupt
	// the next response on a keep-alive connection. Content-Length still reports what
	// the equivalent GET would return.

	// A response produced for a credentialed caller must not be stored where another
	// caller can be served it. Authenticate() takes a bearer token or a session
	// cookie, and the WebUI uses the cookie -- so these carry no Authorization header
	// and RFC 9111 3.5's shared-cache prohibition never engages. Without an explicit
	// policy a shared cache may keep the 200 under heuristic freshness, on a key that
	// does not include Cookie.
	//
	// Stamped centrally so a new authenticated route cannot forget it, and only when
	// credentials were actually presented: an unauthenticated probe like /health stays
	// cacheable. Handlers that set their own policy are untouched.
	//
	// private, NOT no-store: no-store forbids the client's own cache too, so no
	// If-None-Match would ever arrive and the validator and memo machinery here would
	// be dead weight; it also lands on 304s, telling a cache to drop the entry it was
	// just told is good. private alone stops a SHARED cache storing it.
	if (RequestCarriesCredentials(req, kSessionCookieName) &&
		resp.headers.find("Cache-Control") == resp.headers.end()) {
		resp.headers["Cache-Control"] = "private";
		// Vary is the half that matters to a cache which stores anyway.
		AppendHeaderToken(resp.headers, "Vary", "Cookie");
	}

	// On every response, success or error, so a browser can read a 4xx body too.
	ApplyCorsHeaders(resp.headers, cors_org, cors_enabled);
	return resp;
}

CHttpServer::Response CApiDispatcher::DispatchToHandler(const CHttpServer::Request &req)
{
	std::string path, query;
	SplitPathAndQuery(req.target, path, query);

	// Defence in depth: reject NUL, encoded NUL and `..` segments before routing.
	// Today's byte-exact routes 404 these organically, but a future endpoint with a
	// path capture would silently inherit a traversal surface without this.
	if (web_api_path::LooksMalicious(path)) {
		return ErrorResponse(400, "bad_request", "path contains a traversal/injection token");
	}

	// `/api/v0/status/` and `/api/v0/status` name one resource. Confined to the API
	// prefix: the static fallthrough maps a path onto a filesystem, where a trailing
	// slash is a directory rather than a spelling.
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

	// /events reaches the dispatcher only on a method the streaming resolver declined,
	// since it diverts GET and HEAD earlier. Without an arm here it fell to the
	// catch-all 404 and was the one route to escape the Allow sweep.
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
			return HandleDownloadAdd(req);
		}
		if (req.method == "PATCH") {
			return HandleDownloadsBulkPatch(req);
		}
		if (req.method == "DELETE") {
			return HandleDownloadsBulkDelete(req);
		}
		return MethodNotAllowed("GET, HEAD, POST, PATCH, DELETE",
			"only GET / HEAD / POST / PATCH / DELETE on /downloads");
	}

	if (path == "/api/v0/downloads_clear_completed") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /downloads_clear_completed");
		}
		return HandleDownloadsClearCompleted(req);
	}

	// The whole peer surface, every upload_state including queue waiters.
	// Consumers filter client-side rather than asking for a pre-filtered view.
	if (path == "/api/v0/clients") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET on /clients");
		}
		return HandleClients(req);
	}

	// The daemon's credit store: every peer it has ever exchanged data with. Its own
	// resource rather than a sub-path of /clients because these are keyed by user
	// hash, outlive the ECID-issuing process, and carry stored history. Matched before
	// /clients/{ecid}, which accepts any single segment.
	if (path == "/api/v0/known_clients") {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET on /known_clients");
		}
		return HandleKnownClients(req);
	}

	// Single-peer detail. {ecid} is unique per live EC connection.
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

	// Browse the peer's shared files; POST starts it and returns a search_id.
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
			return HandleSharedBulkPatch(req);
		}
		return MethodNotAllowed("GET, HEAD, PATCH", "only GET / HEAD / PATCH on /shared");
	}

	if (path == "/api/v0/shared_reload") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /shared_reload");
		}
		return HandleSharedReload(req);
	}

	// Literal path, so it must be matched before the /shared/{hash} patterns or
	// "media" would be captured as a hash.
	if (path == "/api/v0/shared/media/refresh") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /shared/media/refresh");
		}
		return HandleSharedMediaRefresh(req);
	}

	// The configured share roots, as opposed to /shared, which lists the files those
	// roots produced. Its own top-level path, so no ordering against /shared/{hash}.
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
			"only GET / HEAD / PUT / POST / DELETE on /share_directories");
	}

	if (path == "/api/v0/servers") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleServers(req);
		}
		if (req.method == "POST") {
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

	// One friend by ECID. The browse form is checked first, same ordering
	// rationale as the server routes.
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

	// One conversation keyed "<ip>:<port>". The messages sub-resource matches
	// first or the longer pattern would be shadowed.
	{
		static const auto chat_messages =
			web_api_path::ParsePattern("/api/v0/chats/{address}/messages");
		static const auto chat_one = web_api_path::ParsePattern("/api/v0/chats/{address}");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(chat_messages, path_segs, caps)) {
			if (req.method == "GET" || req.method == "HEAD") {
				return HandleChatMessages(req, caps["address"]);
			}
			if (req.method == "POST") {
				return HandleChatSend(req, caps["address"]);
			}
			return MethodNotAllowed(
				"GET, HEAD, POST", "only GET / HEAD / POST on /chats/{address}/messages");
		}
		if (web_api_path::Match(chat_one, path_segs, caps)) {
			if (req.method == "DELETE") {
				return HandleChatClose(req, caps["address"]);
			}
			return MethodNotAllowed("DELETE", "only DELETE on /chats/{address}");
		}
	}

	// Address a conversation by ECID, for a caller holding a peer or friend row.
	// The friend form is the one that reaches an OFFLINE friend.
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
			return MethodNotAllowed("POST", "only POST on /servers_update");
		}
		return HandleServerUpdateFromUrl(req);
	}

	// Server connect and remove, by ECID or by address; the forms share handlers and
	// differ only in the lookup. Address patterns are tried FIRST because they have
	// the same segment count: the address form with "connect" as the address would
	// otherwise match the ECID pattern with `ecid == "by-address"`.
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
		// The address form has its own path rather than sharing {ecid}: one capture with
		// two identity domains, sniffed apart by a colon, is a dispatch rule invisible
		// from outside, and forecloses ever accepting an IPv6 literal.
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

	// IP filter actions. The IP-filter *settings* are ordinary preferences; these two
	// are the operations behind the desktop Security page's "Reload List" and "Update
	// now" buttons.
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

	// GeoIP action. The GeoIP *settings* are ordinary preferences under [geoip]; this
	// is the standalone "update now" operation, a route rather than a write-only
	// boolean inside PATCH /preferences.
	if (path == "/api/v0/geoip/update") {
		if (req.method != "POST") {
			return MethodNotAllowed("POST", "only POST on /geoip/update");
		}
		return HandleGeoipUpdate(req);
	}

	// re-hash one shared file against its on-disk data. Matched before the
	// single-segment `/shared/{hash}` pattern below purely for locality: the two
	// cannot collide, this one carries an extra path segment.
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

	// the file's own bytes. Two-segment like its neighbours above and matched before
	// `/shared/{hash}` for the same reason; "content" cannot be read as a hash, so
	// the ordering is locality rather than necessity.
	{
		static const auto shared_content =
			web_api_path::ParsePattern("/api/v0/shared/{hash}/content");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(shared_content, path_segs, caps)) {
			if (req.method != "GET" && req.method != "HEAD") {
				return MethodNotAllowed(
					"GET, HEAD", "only GET / HEAD on /shared/{hash}/content");
			}
			return HandleSharedContent(req, caps["hash"]);
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

	if (path == "/api/v0/logs/server_info") {
		if (req.method == "GET" || req.method == "HEAD") {
			return HandleLogServerinfo(req);
		}
		if (req.method == "DELETE") {
			return HandleLogServerinfoReset(req);
		}
		return MethodNotAllowed("GET, HEAD, DELETE", "only GET / HEAD / DELETE on /logs/server_info");
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

	// Matched before anything capturing {id}. They cannot collide (different segment
	// counts, {id} numeric), but ordering keeps that independent of the matcher's
	// internals. Both are search-AGNOSTIC: the daemon resolves a hash against its
	// whole search list, so nesting them under {id} would advertise a scoping that
	// does not exist.
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

	// Community ratings for a search result. POST triggers an on-demand Kad NOTES
	// lookup; GET returns what has arrived plus the running flag. Matched before the
	// /download sibling (distinct trailing segment).
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

	// /search/{id} -- DELETE stops the search AND frees it (results included).
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

	// /search/{id}/{action} -- results / stop / more.
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

	// Path-pattern matches the four allowed graph names; HandleStatsGraph
	// rejects anything else.
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

	// Per-source comments. Downloads-only: needs a live source list. Matched
	// before /downloads/{hash} (more segments).
	{
		static const auto dl_comments =
			web_api_path::ParsePattern("/api/v0/downloads/{hash}/comments");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(dl_comments, path_segs, caps)) {
			if (req.method == "POST") {
				// POST triggers an on-demand Kad NOTES lookup.
				return HandleDownloadCommentsKadSearch(req, caps["hash"]);
			}
			if (req.method != "GET" && req.method != "HEAD") {
				return MethodNotAllowed("GET, HEAD, POST",
					"only GET / HEAD / POST on /downloads/{hash}/comments");
			}
			return HandleDownloadComments(req, caps["hash"]);
		}
	}

	// Source-reported filenames and counts. Downloads-only.
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

	// A4AF swap actions. Downloads-only.
	{
		static const auto dl_a4af = web_api_path::ParsePattern("/api/v0/downloads/{hash}/a4af");
		const auto path_segs = web_api_path::SplitPath(path);
		std::map<std::string, std::string> caps;
		if (web_api_path::Match(dl_a4af, path_segs, caps)) {
			if (req.method == "POST") {
				return HandleDownloadA4afAction(req, caps["hash"]);
			}
			// POST only: A4AF sources are rows of /downloads/{hash}/clients carrying
			// the whole peer object, and `a4af_auto` is on the download detail.
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

	// Single-resource detail plus the mutation surface. `{hash}` is the
	// lowercase 32-char hex MD4; the dispatcher lower-cases input on the way in.
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

	// Ahead of the static fallthrough so the route answers identically whether or not
	// StaticRoot is set: the bytes are compiled in. Outside /api/v0 because it is an
	// image an <img src> points at, carrying no per-installation data.
	if (path.compare(0, 7, "/flags/") == 0) {
		if (req.method != "GET" && req.method != "HEAD") {
			return MethodNotAllowed("GET, HEAD", "only GET / HEAD on /flags/{code}.png");
		}
		return ServeCountryFlag(req, path);
	}

	// Anything that matched no /api/v0 route and is a safe method for a non-API path.
	// ServeStaticFile 404s when StaticRoot is unset, so API-only deployments are
	// unaffected. Auth is deliberately NOT required: the shell is public, and the API
	// calls it makes still pass the per-handler role gates.
	if ((req.method == "GET" || req.method == "HEAD") && path.compare(0, 5, "/api/") != 0) {
		return ServeStaticFile(req, path);
	}

	return ErrorResponse(404, "not_found", "no such endpoint");
}

CHttpServer::Response CApiDispatcher::ServeStaticFile(
	const CHttpServer::Request &req, const std::string &url_path)
{
	// Resolved once per process, conf override first. std::call_once because handlers
	// run concurrently on the worker pool and a plain lazy bool would race the string
	// assignment on the first concurrent requests.
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

	// Map "/" to the SPA entry; strip the leading slash so the join is relative.
	// LooksMalicious has already rejected NUL and `..` segments.
	std::string rel =
		(url_path == "/" || url_path.empty()) ? std::string("index.html") : url_path.substr(1);

	std::string fs_path;
	struct stat st
	{
	};
	std::string body;
	bool found = webapi::ResolveWithinRoot(root, rel, fs_path) && ReadStaticFile(fs_path, body, st);

	// An extension-less path that did not resolve is a client-side route, so the entry
	// document is served and a deep-linked reload still boots. Paths that look like an
	// asset 404 honestly, so a missing JS/CSS is visible.
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

	// ETag is mtime+size, so a frontend rebuild invalidates without cache-busting.
	// Case-insensitive because Beast preserves the client's wire casing, and via the
	// shared matcher rather than a string compare: the header may be `*`, a
	// comma-separated list, or a weak `W/"..."` validator, which is what an nginx in
	// front emits once it gzips.
	const std::string inm_val = FindHeaderCaseInsensitive(req.headers, "If-None-Match");
	// A 304 has no body to compress, so which representation this validator names is
	// decided here while the body is still present, and the comparison uses it.
	const bool coded =
		WillCompressBody(AcceptsGzip(FindHeaderCaseInsensitive(req.headers, "Accept-Encoding")),
			body.size(),
			StaticContentType(rel),
			/*already_encoded=*/false);
	// The shared helper takes the quoted form this path carries as readily as the bare
	// form the API path does.
	const std::string wire_static_etag = webcommon::WithCodingSuffix(etag, coded);
	const std::string wire_static_bare =
		(wire_static_etag.size() >= 2 && wire_static_etag.front() == '"' &&
			wire_static_etag.back() == '"')
			? wire_static_etag.substr(1, wire_static_etag.size() - 2)
			: wire_static_etag;
	if (webcommon::IfNoneMatchHits(inm_val, wire_static_bare)) {
		CHttpServer::Response r;
		r.status = 304;
		// content_type defaults to application/json, which is wrong on a 304 for an HTML
		// or CSS asset. Cleared rather than corrected: a 304 carries no content and the
		// transport omits an empty header.
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
	// Kept for HEAD too: the transport writes headers only, so nothing reaches the
	// wire, and this lets Content-Length report the real size and stops HEAD and GET
	// disagreeing about the validator.
	r.body = std::move(body);
	r.headers["ETag"] = etag;
	// The shell is the same bytes for everyone, so it overrides the authenticated
	// default.
	//
	// no-cache rather than a max-age: index.html, app.js and app.css keep their names
	// across a rebuild, so inside a freshness lifetime the browser would not ask, an
	// upgraded daemon would serve the old shell, and per-asset expiry could pair a new
	// shell with an old bundle. no-cache still lets the copy be stored, so an
	// unchanged bundle costs one conditional GET answered 304.
	//
	// public is load-bearing: RFC 9111 3.5 bars a shared cache from reusing a response
	// to an Authorization-bearing request unless it is public, must-revalidate or
	// s-maxage. That is the bearer-token client, not the cookie-authenticated WebUI,
	// which is why the credential stamp above needs Vary: Cookie instead.
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

	// Two lowercase ASCII letters, the shape `country_code` arrives in, plus the one
	// literal name the set ships alongside them: "unknown", the "??" placeholder
	// CCountryFlags falls back to, offered so a frontend can match the desktop.
	//
	// The art id is built by concatenation, so this whitelist is what stops a crafted
	// code naming a non-flag entry in the shared icon table. LooksMalicious rejects
	// those upstream, but the lookup must not depend on it.
	const bool is_alpha2 =
		code.size() == 2 && code[0] >= 'a' && code[0] <= 'z' && code[1] >= 'a' && code[1] <= 'z';
	if (!is_alpha2 && code != "unknown") {
		return ErrorResponse(404, "not_found", "no such flag");
	}

	// The famfamfam set covers 248 of the ~300 assignable alpha-2 codes and GeoIP
	// can resolve one it has no artwork for, so a well-formed miss is a 404.
	const struct AMuleIconEntry *icon = amule_find_icon(("flag_" + code).c_str());
	if (!icon || icon->png_data == nullptr || icon->png_len == 0) {
		return ErrorResponse(404, "not_found", "no such flag");
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "image/png";
	// Dispatch() applies the ETag and 304 swap to every 200 GET/HEAD, and the
	// transport writes a HEAD as headers only, so this handler just produces bytes.
	r.body.assign(reinterpret_cast<const char *>(icon->png_data), icon->png_len);
	// The artwork is compiled in and can only change with a new build, while a peer
	// list is a page full of <img> tags pointing here. A day of freshness turns those
	// into cache hits, while bounding how long an upgraded daemon serves stale art.
	r.headers["Cache-Control"] = "public, max-age=86400";
	return r;
}

// GET /health.
//
// Liveness, not readiness: always 200 while the HTTP server answers, so a container
// or load-balancer probe never restarts a healthy process because amuled went away.
// Readiness is in the body, where a caller that wants it can key on the two flags
// without the status code moving under one that does not.
//
// No EC roundtrip: amuleapi serialises EC through one worker, so a probe that
// waited on the daemon could block behind an unrelated slow mutation and time out,
// reporting the service down when it is merely busy.
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
	// `snapshot_ready`, not `snapshot`: a bare noun reads as "here is a snapshot"
	// rather than the readiness state it reports, on the first response most clients
	// parse (R4).
	w.Key("snapshot_ready");
	w.ValueBool(m_state.HasFirstSnapshot());
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleVersion(const CHttpServer::Request &req)
{
	// Identity stays unauthenticated: version negotiation has to work before anyone
	// holds a token. Liveness is /health's job. The `update` block is authenticated,
	// because it reports whether THIS daemon is outdated, which an unauthenticated
	// caller on a reachable interface should not learn.
	//
	// Auth is OPTIONAL here, which is why Authenticate() is not called
	// unconditionally: that wrapper counts every 401 against the generic limiter, and a
	// request with no credential is this endpoint's documented use rather than a
	// failure. Counting it would let an anonymous poller -- or one poller behind a
	// reverse proxy, on the address every client shares -- spend the bucket in 30
	// requests and lock real sessions out of the authenticated surface. A credential
	// that IS presented and rejected still counts: the `update` block is an oracle a
	// token guesser could otherwise read for free.
	AuthOutcome auth;
	if (RequestCarriesCredentials(req, kSessionCookieName)) {
		auth = Authenticate(req);
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	// `service`, not `name`: the value is the constant "amuleapi".
	w.Key("service");
	w.ValueString(wxT("amuleapi"));
	w.Key("api_version");
	w.ValueString(wxT("v0"));
	// `amuleapi_version`, not `amule_version`: the version the amuleapi binary was
	// built from, which need not match the daemon it talks to.
	w.Key("amuleapi_version");
	// The short form the daemon reports for itself, so the two stay comparable:
	// bare VERSION is the literal "GIT" on a development build.
	w.ValueString(GetShortMuleVersion());
	// Version of the connected amuled from the EC handshake; empty when EC is not
	// connected or the daemon predates EC_TAG_SERVER_VERSION.
	w.Key("daemon_version");
	w.ValueString(m_app.GetDaemonVersion());

	// Relayed from the connected daemon, never checked by amuleapi itself, and
	// English/C-locale per the API contract. When the daemon cannot check (built
	// without ENABLE_VERSION_CHECK, the pref off, or a pre-3.1 daemon emitting none of
	// these tags) check_enabled is false and a client shows nothing.
	if (auth.ok) {
		const auto prefs = m_state.Preferences();
		const auto status = m_state.Status();
		const bool check_enabled = prefs.version_check_available && prefs.version_check_enabled;
		const bool checked = status.version_check_done;
		w.Key("update");
		w.BeginObject();
		w.Key("check_enabled");
		w.ValueBool(check_enabled);
		w.Key("checked");
		w.ValueBool(checked);
		// null rather than "" before a check completes (R10).
		WriteStringOrNull(w,
			"latest_version",
			checked && !status.version_check_latest.empty(),
			status.version_check_latest);
		// `available`, not `update_available`: it stutters inside its own object.
		w.Key("available");
		if (checked) {
			w.ValueBool(status.version_check_outdated);
		} else {
			w.ValueNull();
		}
		WriteIntOrNull(w,
			"last_checked_at",
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

	// Before the credential path: a locked-out IP burns no MD5 cycles and cannot
	// drive a side channel distinguishing lockout from wrong password.
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

	// Through ParseJsonObjectBody so this pre-auth path shares the depth cap: without
	// it a deeply nested body would blow the worker stack via picojson's recursive
	// descent, and login is reachable unauthenticated.
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

	// Admin first, then guest. The comparison is constant time inside webcommon; what
	// is visible from outside is the PBKDF2 cost, which is why the limiter runs first.
	// This is also where amuleapi picks up a password another process wrote, and where
	// a record predating the current KDF cost is upgraded. Empty roles skip the KDF.
	Role role = Role::GUEST;
	const CAmuleApiConfig::MatchedRole matched = m_config.VerifyPassword(md5_hex);
	if (matched == CAmuleApiConfig::MatchedRole::Admin) {
		role = Role::ADMIN;
	}

	if (matched == CAmuleApiConfig::MatchedRole::None) {
		// Distinguish "nothing configured" from "wrong password", or every login silently
		// fails and the operator suspects the JWT. Read after VerifyPassword, which
		// refreshes it from disk. A misconfiguration is not a failed guess, so it does
		// not count against the limiter.
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

	// Deliberately NO NoteSuccess(). One bucket, keyed by IP, guards both passwords,
	// since VerifyPassword tries admin then guest. Clearing it on any match would let
	// a guest-credential holder erase the admin failure streak at will -- four wrong
	// admin guesses, one good guest login, repeat -- so the one control protecting the
	// admin password never fires. Stamps age out on their own.
	//
	// The clear in HandleAuthPasswords is a different case: admin-gated and verifying
	// the current password, so its success is on the credential the bucket protects.

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

// Issues a session for `role`, attaches the cookie to `r`, and writes the standard
// session fields into the object `w` is building. Shared with the password change,
// which re-issues so that changing a password does not sign the caller out of the
// request they are making.
void CApiDispatcher::BeginSession(
	const CHttpServer::Request &req, Role role, CHttpServer::Response &r, CJsonWriter &w)
{
	const CJwt::IssuedToken issued = m_jwt.Issue(role);
	r.headers["Set-Cookie"] = MakeSetCookie(kSessionCookieName, issued.token, issued.expires_at);

	// Cookie-auth default: the HttpOnly+SameSite cookie carries the token, and echoing
	// it into the body would defeat HttpOnly -- any XSS that could call
	// fetch('/auth/login') could read and exfiltrate the bearer.
	//
	// Opt-in for SDK and curl clients with no cookie jar: `Accept: application/jwt` or
	// `?include_token=true` adds `token` to the body and nothing else.
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
			// `include_token=true`, not `type=bearer`: "type" named no axis.
			const auto it = qmap.find("include_token");
			if (it != qmap.end() && it->second == "true") {
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
	// One `expires_at`, unix seconds, like every other `_at` on the surface.
	w.Key("expires_at");
	w.ValueInt(static_cast<int64_t>(issued.expires_at));
	// `session_id`, not `jti` (a JWT internal), and unconditional: it used to appear
	// only in the token shape, so cookie-auth logins lacked the id /auth/session
	// returns for the same session.
	w.Key("session_id");
	w.ValueString(wxString::FromUTF8(issued.jti.c_str()));
}

CHttpServer::Response CApiDispatcher::HandleLogout(const CHttpServer::Request &req)
{
	// The generic-401 cap applies here too: repeat 401s are a credential-stuffing
	// signal even on an idempotent path, and locked-out IPs short-circuit.
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

	// Logout is idempotent: a revoked-but-unexpired token still gets a 204, because
	// what it asked for has already happened. Otherwise a tab that fires logout twice
	// sees a 401 and renders a spurious "session expired". Softer than
	// AuthenticateRequest: reject only bad-sig, expired or missing; revoked is a noop.
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
	// Already revoked → 204 noop (don't re-revoke, don't re-emit a
	// clear-cookie that might race with the browser's own delete).
	if (!m_revocations.IsRevoked(v.jti)) {
		// TTL is the JWT's own exp, so the GC drops the entry once the token
		// would have expired anyway.
		m_revocations.Revoke(v.jti, v.exp);
	}
	m_authRateLimiter.NoteSuccess(ip);

	CHttpServer::Response r;
	// 204 with no body: everything this used to echo came from the request URL,
	// and `ok` restated the status code.
	r.status = 204;
	r.content_type.clear();
	r.headers["Set-Cookie"] = MakeClearCookie(kSessionCookieName);

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
	w.Key("session_id");
	w.ValueString(wxString::FromUTF8(a.verified.jti.c_str()));
	w.Key("expires_at");
	w.ValueInt(static_cast<int64_t>(a.verified.exp));
	w.EndObject();
	// Per-principal document: a shared cache must not hand one caller another's
	// session. The ETag memo excludes this target for the same reason.
	r.headers["Cache-Control"] = "private, no-store";
	FinalizeJsonBody(w, r);
	return r;
}

// What is configured, never the credentials. Admin-only: whether a guest
// account exists is not something a guest session needs to know.
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
	w.Key("admin_password_set");
	w.ValueBool(!m_config.AdminCredential().empty());
	w.Key("guest_access_enabled");
	w.ValueBool(!m_config.GuestCredential().empty());
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// Change the admin password, and turn the guest role on/off or change its password.
//
// `current_password` is mandatory even though the caller holds an admin token: a
// stolen token should not be enough to lock the operator out of their own daemon.
// It shares /auth/login's rate limiter, so this is not a softer place to guess.
// Fields are omitted rather than nulled to mean "leave alone".
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

	bool guest_access_enabled = !m_config.GuestCredential().empty();
	bool has_guest_access_enabled = false;
	{
		auto it = obj.find("guest_access_enabled");
		has_guest_access_enabled = (it != obj.end());
		if (has_guest_access_enabled) {
			if (!it->second.is<bool>()) {
				return ErrorResponse(
					400, "bad_request", "`guest_access_enabled` must be a boolean");
			}
			guest_access_enabled = it->second.get<bool>();
		}
	}
	// Setting a guest password while switching the role off is contradictory, and
	// guessing which half was meant would silently do the wrong one.
	if (has_guest && !guest_new.empty() && has_guest_access_enabled && !guest_access_enabled) {
		return ErrorResponse(400,
			"bad_request",
			"`guest_password` cannot be set together with `guest_access_enabled: false`");
	}
	// A guest password on its own means "turn guest on with this".
	if (has_guest && !guest_new.empty() && !has_guest_access_enabled) {
		guest_access_enabled = true;
	}
	if (!has_admin && !has_guest && !has_guest_access_enabled) {
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
	// The member keeps the name libwebcommon's credential file uses.
	change.guest_enabled = guest_access_enabled;
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
	w.Key("admin_password_set");
	w.ValueBool(!m_config.AdminCredential().empty());
	w.Key("guest_access_enabled");
	w.ValueBool(!m_config.GuestCredential().empty());
	// Writing the file invalidated every token issued before it, this caller's
	// included. Re-issuing theirs keeps the operator who changed the password signed
	// in while everyone else is signed out, which is the point of changing it.
	BeginSession(req, Role::ADMIN, r, w);
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

CHttpServer::Response CApiDispatcher::HandleStatus(const CHttpServer::Request &req)
{
	// Any authenticated role; mutating endpoints gate on admin only.
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// 503 with a structured code until the refresher has completed a tick, so
	// clients retry rather than guess.
	if (auto r = RequireSnapshot(m_state))
		return *r;

	// One shared_lock for the whole composite read: Dashboard() returns status, kad,
	// snapshot_at and ec_connected in a single acquisition, so a refresher tick cannot
	// land between sub-snapshots and produce an inconsistent rollup (kad.network from
	// tick N+1 beside ed2k.* from tick N).
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
	// No snapshot timestamp in the envelope: a value that moves every tick changes the
	// body bytes and defeats the ETag, so list endpoints would never see a cache hit.
	// `ec_connected` is the staleness signal, and the HTTP Date header carries
	// wall-clock for anyone who needs it.
	w.Key("ec_connected");
	w.ValueBool(ec);
	(void)ts;

	w.Key("ed2k");
	w.BeginObject();
	w.Key("state");
	w.ValueString(wxString::FromUTF8(s.ed2k_state.c_str()));
	// Positive sense, matching the peer-side high_id on /clients/{ecid}. False for
	// a LowID and while disconnected alike, so read it with `state`.
	w.Key("high_id");
	w.ValueBool(s.ed2k_high_id);
	// Our server-assigned id; a HighID (>= 16777216) is our public address packed
	// LSB-first, which is where public_ip comes from. NOT the same encoding as the
	// peer-side ed2k_user_id on /clients/{ecid}, which byte-swaps a HighID, so the two
	// must not be fed through each other's decoder.
	w.Key("user_id");
	w.ValueInt(static_cast<int64_t>(s.ed2k_user_id));
	WriteStringOrNull(w, "public_ip", !s.ed2k_public_ip.empty(), s.ed2k_public_ip);
	// 0 when not connected -- gate on ed2k.state, not on this being nonzero.
	w.Key("connected_since_at");
	w.ValueInt(static_cast<int64_t>(s.ed2k_connected_since));
	// Null when not connected: ed2k.state already says whether there is a server, so
	// "" only ever meant "not connected", and a port on its own describes nothing.
	const bool has_server = !s.server_ip.empty();
	WriteStringOrNull(w, "server_name", has_server, s.server_name);
	WriteStringOrNull(w, "server_ip", has_server, s.server_ip);
	WriteIntOrNull(w, "server_port", has_server, static_cast<int64_t>(s.server_port));
	// Aggregate user and file counts across all connected ed2k servers, from the
	// same EC_OP_STAT_REQ response the kad counters ride on.
	w.Key("network");
	w.BeginObject();
	// null unless eD2k is connected: these sum the whole known server list, not
	// the server we are attached to, and nothing zeroes them on disconnect.
	WriteIntOrNull(w, "user_count", s.has_ed2k_network, static_cast<int64_t>(s.ed2k_users));
	WriteIntOrNull(w, "file_count", s.has_ed2k_network, static_cast<int64_t>(s.ed2k_files));
	w.EndObject();
	w.EndObject();

	w.Key("kad");
	w.BeginObject();
	w.Key("state");
	w.ValueString(wxString::FromUTF8(s.kad_state.c_str()));
	// Named for the transport because GET /kad reports it beside firewalled_udp,
	// a separate measurement rather than a refinement of this one.
	WriteBoolOrNull(w, "firewalled_tcp", s.has_kad_firewalled_tcp, s.kad_firewalled_tcp);
	// 0 when not connected -- gate on kad.state, not on this being nonzero.
	w.Key("connected_since_at");
	w.ValueInt(static_cast<int64_t>(s.kad_connected_since));
	// The same numbers GET /kad serves under `network`, through the same helper so the
	// two cannot drift. `k` was snapshotted in the same lock batch as `s`, so these
	// counters describe the same refresher tick as ed2k.* above.
	WriteKadNetworkObject(w, k);
	w.EndObject();

	w.Key("speeds");
	w.BeginObject();
	// `download_speed_bytes_per_second`, not `download_bytes_per_second`: the `speeds`
	// wrapper does not rename the quantity, and the short form reads close enough to
	// the cumulative `downloaded_bytes_total` to be misread as one.
	w.Key("download_speed_bytes_per_second");
	w.ValueInt(static_cast<int64_t>(s.download_bytes_per_second));
	w.Key("upload_speed_bytes_per_second");
	w.ValueInt(static_cast<int64_t>(s.upload_bytes_per_second));
	// Additive to the two above, not a subset: amuled counts protocol
	// overhead separately from payload.
	w.Key("download_overhead_bytes_per_second");
	w.ValueInt(static_cast<int64_t>(s.download_overhead_bytes_per_second));
	w.Key("upload_overhead_bytes_per_second");
	w.ValueInt(static_cast<int64_t>(s.upload_overhead_bytes_per_second));
	w.EndObject();

	// null rather than a number when the daemon has no figure: the -1 sentinel as
	// an unsigned value would read as 17 exabytes free, and 0 as a full disk.
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
	w.Key("waiting_upload_client_count");
	w.ValueInt(static_cast<int64_t>(s.ul_queue_len));
	w.Key("download_source_count");
	w.ValueInt(static_cast<int64_t>(s.total_src_count));
	w.EndObject();
	// Nickname is a /preferences field, not a /status one.
	w.EndObject();

	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// Write a single download object, used inline by the list endpoint and as the bare
// body of the detail endpoint; `include_envelope_keys` picks which.
//
// The chunk size and part-count arithmetic live in State.h (webapi::kPartSizeBytes
// / webapi::PartCountForSize). The constant still cannot come from
// `protocol/ed2k/Constants.h`, which is written against amule's legacy typedefs.

// Render the per-part state array from the decoded gap list and per-part source
// counts: count = ceil(size / PARTSIZE); a part "has gap" if any byte-range in
// `gaps` covers it; state is "complete" (no gap), "pending" (gap + sources > 0) or
// "unavailable" (gap + zero sources). `gaps` is flat (start, end) uint64 pairs,
// both inclusive on amule's side (CGapList::Encode semantics).
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
		// "pending": we lack it and a source has it. "unavailable": we lack it and
		// none does. The old spellings both read as "we lack it".
		const char *state = !has_gap[static_cast<std::size_t>(i)]
					    ? "complete"
					    : (sources > 0 ? "pending" : "unavailable");
		w.BeginObject();
		w.Key("state");
		w.ValueString(wxString::FromAscii(state));
		w.Key("sources");
		w.ValueInt(static_cast<int64_t>(sources));
		w.EndObject();
	}
	w.EndArray();
}

// Per-part source availability behind the shared "Obtained Parts" bar. A complete
// known file carries its own vector from EC_TAG_KNOWNFILE; a shared partfile is
// emitted as EC_TAG_PARTFILE only, so its vector lands on the download side. Same
// server-side encoder, so the fallback is the same numbers.
const std::vector<std::uint16_t> &SharedPartSources(const webapi::FileSnapshot &f)
{
	return f.shared.decoded_part_sources.empty() ? f.download.decoded_part_sources
						     : f.shared.decoded_part_sources;
}

// One `{sources}` per part, in file order, always exactly `part_count` long.
// Deliberately NOT the downloads shape: `state` there encodes local completeness,
// meaningless for a share and an invitation to render a progress bar. Always
// present, null when nothing has been decoded (R10), which keeps "no data"
// distinct from "no sources for any part".
void WriteSharedAvailabilityParts(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	const std::vector<std::uint16_t> &part_sources = SharedPartSources(f);
	if (part_sources.empty()) {
		w.Key("parts");
		w.ValueNull();
		return;
	}
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

// The `media` object, or null when the file carries no probed metadata. Shared by
// the download, shared and search-result writers. null rather than omitted so the
// key is always there: this is the one place the unknown-value rule reaches an
// object rather than a scalar, so a client tests `media === null` before reaching
// into it.
void WriteMediaIfPresent(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	if (!f.has_media) {
		w.Key("media");
		w.ValueNull();
		return;
	}
	w.Key("media");
	w.BeginObject();
	w.Key("duration_seconds");
	w.ValueInt(static_cast<int64_t>(f.media.duration_seconds));
	w.Key("bitrate_kilobits_per_second");
	w.ValueInt(static_cast<int64_t>(f.media.bitrate_kilobits_per_second));
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
	w.Key("size_bytes");
	w.ValueInt(static_cast<int64_t>(f.size));
	w.Key("completed_bytes");
	w.ValueInt(static_cast<int64_t>(f.download.completed_bytes));
	w.Key("transferred_bytes");
	w.ValueInt(static_cast<int64_t>(f.download.transferred_bytes));
	w.Key("speed_bytes_per_second");
	w.ValueInt(static_cast<int64_t>(f.download.speed_bytes_per_second));
	w.Key("status");
	w.ValueString(wxString::FromUTF8(f.download.status.c_str()));
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(f.download.priority.c_str()));
	w.Key("priority_auto");
	w.ValueBool(f.download.priority_auto);
	w.Key("category_index");
	w.ValueInt(static_cast<int64_t>(f.download.category));
	w.Key("sources");
	w.BeginObject();
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(f.download.sources_total));
	w.Key("unavailable");
	w.ValueInt(static_cast<int64_t>(f.download.sources_unavailable));
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
	// True while an on-demand Kad notes lookup is in flight. In the shared object
	// so list, detail and the SSE download event stay identical.
	w.Key("kad_comment_lookup_running");
	w.ValueBool(f.download.kad_comment_searching);
	// On the list, not detail-only: a hashing indicator needs it wherever the file
	// appears. Parts hashed so far, not an index; 0 when idle.
	w.Key("hashed_part_count");
	w.ValueInt(static_cast<int64_t>(f.download.hashed_part_count));
	// Its denominator, on the list for the same reason: a rising count with no
	// total cannot be rendered. Computed, not decoded -- there is no EC tag.
	const std::int64_t total_part_count = static_cast<std::int64_t>(webapi::PartCountForSize(f.size));
	w.Key("total_part_count");
	w.ValueInt(total_part_count);
	// On the list so the SSE download event carries it: A4AF is a client-to-file
	// relation, the one thing a per-file client list needs that the `clients` channel
	// cannot say.
	w.Key("source_ecids");
	w.BeginArray();
	for (const std::uint32_t ecid : f.download.a4af_sources) {
		w.ValueInt(static_cast<int64_t>(ecid));
	}
	w.EndArray();
	if (detail) {
		// Detail-only fields, omitted from the list. `remaining_seconds` is computed
		// here -- no EC tag exists -- and is null when stalled or paused, where there
		// is nothing to compute from. It was -1, which a client had to read as unknown.
		bool has_remaining_seconds = false;
		std::int64_t remaining_seconds = 0;
		if (f.download.speed_bytes_per_second > 0) {
			has_remaining_seconds = true;
			remaining_seconds =
				(f.size > f.download.completed_bytes)
					? static_cast<std::int64_t>((f.size - f.download.completed_bytes) /
								    f.download.speed_bytes_per_second)
					: 0;
		}
		// Null rather than 0 for "no complete copy has ever been seen": a unix
		// timestamp of 0 reads as 1970, not as never.
		WriteIntOrNull(w,
			"last_seen_complete_at",
			f.download.last_seen_complete_at != 0,
			static_cast<std::int64_t>(f.download.last_seen_complete_at));
		// Same rule: 0 is "no byte has arrived yet", not a 1970 date. It is only
		// set when amuled ships the tag.
		WriteIntOrNull(w,
			"last_received_at",
			f.download.last_received_at != 0,
			static_cast<std::int64_t>(f.download.last_received_at));
		w.Key("active_seconds");
		w.ValueInt(static_cast<int64_t>(f.download.active_seconds));
		w.Key("available_part_count");
		w.ValueInt(static_cast<int64_t>(f.download.available_part_count));
		WriteIntOrNull(w, "remaining_seconds", has_remaining_seconds, remaining_seconds);
		w.Key("lost_to_corruption_bytes");
		w.ValueInt(static_cast<int64_t>(f.download.lost_to_corruption_bytes));
		w.Key("gained_by_compression_bytes");
		w.ValueInt(static_cast<int64_t>(f.download.gained_by_compression_bytes));
		w.Key("ich_recovered_packet_count");
		w.ValueInt(static_cast<int64_t>(f.download.ich_recovered_packet_count));
		// null, not "": the AICH hashset does not exist until the file has been
		// hashed, and R10 spells an absent value null.
		WriteStringOrNull(w, "aich_hash", !f.aich_hash.empty(), f.aich_hash);
		// The ".part" control-file basename, omitted once the download completes: a
		// completed file structurally has no partfile, which is the absent-key case
		// rather than the null of "not reported". It used to be a manufactured "" a
		// client had to read as "completed". Nothing to surface either way: on a
		// completed file the daemon reuses the _FILENAME tag to carry the directory path.
		if (f.download.status != "completed") {
			w.Key("part_file_name");
			w.ValueString(wxString::FromUTF8(f.part_met_basename.c_str()));
		}
		w.Key("directory");
		// The on-disk directory (Temp while downloading, destination once
		// completed) -- mirrors the `path` field on /shared/{hash} (#417).
		w.ValueString(wxString::FromUTF8(f.on_disk_dir.c_str()));
		w.Key("upload_queue_count");
		w.ValueInt(static_cast<int64_t>(f.queued_count));
		w.Key("my_comment");
		w.ValueString(wxString::FromUTF8(f.comment.c_str()));
		w.Key("my_rating");
		w.ValueInt(static_cast<int64_t>(f.rating));
		w.Key("a4af_auto");
		w.ValueBool(f.download.a4af_auto);
		WriteMediaIfPresent(w, f);
	}
	w.EndObject();
}

// Base (list-level) client fields, emitted into an already-open object so the
// list and detail writers share one definition of the set.
void WriteClientBaseFields(CJsonWriter &w, const webapi::ClientSnapshot &c)
{
	w.Key("ecid");
	w.ValueInt(static_cast<int64_t>(c.ecid));
	// Null, not "", for every optional string below (R10), matching
	// WriteKnownClientObject. user_hash and the *_state enums stay unconditional.
	WriteStringOrNull(w, "name", !c.client_name.empty(), c.client_name);
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(c.user_hash.c_str()));
	// Nulled together, keyed on the address: a port without one describes nothing.
	// Matches the SSE row and WriteKnownClientObject.
	const bool has_addr = !c.ip.empty();
	WriteStringOrNull(w, "ip", has_addr, c.ip);
	WriteIntOrNull(w, "port", has_addr, static_cast<int64_t>(c.port));
	// ISO 3166-1 alpha-2 (lowercase); null when GeoIP is off/unresolved (#439).
	WriteStringOrNull(w, "country_code", !c.country_code.empty(), c.country_code);
	WriteStringOrNull(w, "software", !c.software.empty(), c.software);
	WriteStringOrNull(w, "software_version", !c.software_version.empty(), c.software_version);
	WriteStringOrNull(w, "reported_os", !c.reported_os.empty(), c.reported_os);
	// The three *_state values are enum labels, not free text: the daemon always
	// answers, and an answer it does not recognise is the enum's "unknown" member.
	// Empty is unreachable, so there is nothing to null.
	w.Key("upload_state");
	w.ValueString(wxString::FromUTF8(c.upload_state.c_str()));
	w.Key("download_state");
	w.ValueString(wxString::FromUTF8(c.download_state.c_str()));
	w.Key("ident_state");
	w.ValueString(wxString::FromUTF8(c.ident_state.c_str()));
	WriteStringOrNull(w, "download_file_name", !c.download_file_name.empty(), c.download_file_name);
	WriteStringOrNull(w, "upload_file_name", !c.upload_file_name.empty(), c.upload_file_name);
	WriteStringOrNull(w, "upload_file_hash", !c.upload_file_hash.empty(), c.upload_file_hash);
	WriteStringOrNull(w, "download_file_hash", !c.download_file_hash.empty(), c.download_file_hash);
	// R11: flattened out of the old `xfer` wrapper. A sub-object earns its place by
	// grouping DIFFERENT quantities; this grouped one quantity split by time window,
	// which belongs in the key.
	w.Key("uploaded_bytes_session");
	w.ValueInt(static_cast<int64_t>(c.uploaded_bytes_session));
	w.Key("downloaded_bytes_session");
	w.ValueInt(static_cast<int64_t>(c.downloaded_bytes_session));
	w.Key("uploaded_bytes_total");
	w.ValueInt(static_cast<int64_t>(c.uploaded_bytes_total));
	w.Key("downloaded_bytes_total");
	w.ValueInt(static_cast<int64_t>(c.downloaded_bytes_total));
	w.Key("upload_speed_bytes_per_second");
	w.ValueInt(static_cast<int64_t>(c.upload_speed_bytes_per_second));
	w.Key("download_speed_bytes_per_second");
	w.ValueInt(static_cast<int64_t>(c.download_speed_bytes_per_second));
	w.Key("upload_queue_position");
	w.ValueInt(static_cast<int64_t>(c.upload_queue_position));
	// 0xffff is amuled's "that peer's queue is full" sentinel
	// (ECSpecialCoreTags.cpp), not a position. Relayed verbatim it renders as
	// "position 65535", and sorting by it buries full queues at the far end as though
	// they were merely very distant.
	WriteIntOrNull(w,
		"remote_queue_position",
		c.remote_queue_position != webapi::kRemoteQueueFullSentinel,
		static_cast<int64_t>(c.remote_queue_position));
	w.Key("upload_queue_score");
	w.ValueInt(static_cast<int64_t>(c.score));
	WriteStringOrNull(w, "obfuscation_state", !c.obfuscation_state.empty(), c.obfuscation_state);
	// Being in this list means the daemon holds a client object, which starts
	// at the first contact attempt. This says whether a socket is actually up.
	WriteBoolOrNull(w, "connected", c.has_connected, c.connected);
	// The extensions the peer claimed, as tokens rather than the
	// EC_TAG_CLIENT_MOD_CAPABILITIES word: an integer would make every consumer carry
	// its own copy of the bit table, with nothing keeping those in step with
	// src/PeerCapabilities.h. A list because a peer claims any combination. An empty
	// array is what nearly every peer produces: a peer that sent no capability tag and
	// one that sent an all-zero word are the same state.
	w.Key("protocol_extensions");
	w.BeginArray();
	{
		CPeerCapabilities caps;
		caps.SetFromWire(c.protocol_extensions);
		for (const std::string &token : caps.GetApiTokens()) {
			w.ValueString(token.c_str());
		}
	}
	w.EndArray();
	w.Key("friend_slot");
	w.ValueBool(c.friend_slot);
	// On the list because the desktop's per-file peer panels render Origin and "Shares
	// File List" as columns. Anything added here must also reach the SSE payload --
	// both ToJson AND Equal in EventDiff.cpp; a field in one but not the other never
	// updates.
	WriteStringOrNull(w, "source_origin", !c.source_origin.empty(), c.source_origin);
	// Gated on the flag the refresher sets when the tag actually arrives: emitted
	// unconditionally, a peer that never reported its part map was indistinguishable
	// from one reporting zero, and zero is a real answer.
	WriteIntOrNull(w,
		"parts_offered_count",
		c.has_parts_offered_count,
		static_cast<int64_t>(c.parts_offered_count));
	WriteStringOrNull(w, "client_mod_name", !c.client_mod_name.empty(), c.client_mod_name);
	// Inverted from the old `view_shared_disabled`: a negated boolean forces
	// `=== false` at every call site, and R4 wants the positive form.
	w.Key("shared_files_browsable");
	w.ValueBool(!c.view_shared_disabled);
	// null, not omitted, when there is no linked download to be a fraction
	// of. -1 is the in-process sentinel and never reaches the wire.
	w.Key("part_progress_percent");
	if (c.part_progress_percent >= 0.0)
		w.ValueDouble(c.part_progress_percent);
	else
		w.ValueNull();
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
// Tag-absent means "the daemon has no such record", not "empty": a record written
// before per-peer metadata existed carries only the hash, the totals and a
// last-seen, and the fields below stay unset so the writer emits null. The numeric
// codes go through the decoders the refresher uses for live peers
// (ClientTagNames.h), so a consumer gets the same token from either endpoint.
webapi::KnownClientSnapshot DecodeKnownClient(const CECTag &entry)
{
	webapi::KnownClientSnapshot c;
	c.user_hash = std::string(entry.GetMD4Data().Encode().Lower().utf8_str());

	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_UPLOAD_TOTAL))
		c.uploaded_bytes_total = t->GetInt();
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_DOWNLOAD_TOTAL))
		c.downloaded_bytes_total = t->GetInt();
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_LAST_SEEN))
		c.last_seen_at = static_cast<std::time_t>(t->GetInt());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_FIRST_SEEN))
		c.first_seen_at = static_cast<std::time_t>(t->GetInt());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_SESSIONS))
		c.session_count = static_cast<std::uint32_t>(t->GetInt());
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
		c.software_version = std::string(t->GetStringData().utf8_str());
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_FROM))
		c.source_origin = webapi::SourceOriginName(static_cast<std::uint32_t>(t->GetInt()));
	if (const CECTag *t = entry.GetTagByName(EC_TAG_CLIENT_OBFUSCATION_STATUS))
		c.obfuscation_state = webapi::ClientObfuscationName(static_cast<std::uint8_t>(t->GetInt()));
	return c;
}

// One credit-store record.
//
// Optional fields are null rather than omitted: a record written before the daemon
// kept per-peer metadata genuinely has no name, address or software, and a consumer
// should tell "not recorded" from "recorded as empty" without testing for the key.
// Hash, totals and last_seen_at are always present.
void WriteKnownClientObject(CJsonWriter &w, const webapi::KnownClientSnapshot &c)
{
	w.BeginObject();
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(c.user_hash.c_str()));
	// An unknown value is null; a key is omitted only where absence itself is the
	// meaning, which is not the case for any of these. "The daemon did not report this
	// peer's IP" is a value.
	WriteStringOrNull(w, "name", !c.client_name.empty(), c.client_name);
	const bool has_addr = !c.ip.empty();
	WriteStringOrNull(w, "ip", has_addr, c.ip);
	WriteIntOrNull(w, "port", has_addr, static_cast<int64_t>(c.port));
	WriteIntOrNull(w, "kad_port", has_addr, static_cast<int64_t>(c.kad_port));
	WriteStringOrNull(w, "country_code", !c.country_code.empty(), c.country_code);
	const bool has_software = !c.software.empty();
	WriteStringOrNull(w, "software", has_software, c.software);
	WriteStringOrNull(w, "software_version", has_software, c.software_version);
	WriteStringOrNull(w, "source_origin", !c.source_origin.empty(), c.source_origin);
	WriteStringOrNull(w, "obfuscation_state", !c.obfuscation_state.empty(), c.obfuscation_state);
	// Same quantity as the live /clients row, so the same key (R6).
	w.Key("uploaded_bytes_total");
	w.ValueUInt(static_cast<uint64_t>(c.uploaded_bytes_total));
	w.Key("downloaded_bytes_total");
	w.ValueUInt(static_cast<uint64_t>(c.downloaded_bytes_total));
	// Bare, not nulled like the two below, because 0 cannot reach here. nLastSeen
	// predates the clients.met metadata trailer, so it lives in the fixed credit record
	// every accepted file version carries, is stamped by GetCredit on both branches,
	// and any record loading below the 150-day expiry is dropped. The metadata-derived
	// fields have no such guarantee, which is what first_seen_at gates on.
	w.Key("last_seen_at");
	w.ValueUInt(static_cast<uint64_t>(c.last_seen_at));
	const bool has_first_seen = c.first_seen_at != 0;
	WriteUIntOrNull(w, "first_seen_at", has_first_seen, static_cast<uint64_t>(c.first_seen_at));
	WriteUIntOrNull(w, "session_count", has_first_seen, static_cast<uint64_t>(c.session_count));
	// Correlate with /clients by user_hash to reach the live peer. This is
	// reachability, not presence in that list: the daemon holds a client object from
	// the first contact ATTEMPT. null when the core predates EC_TAG_CLIENT_CONNECTED
	// -- unknown, not offline.
	//
	// Same key as the live rows: the same bit reaching a persisted row by correlation
	// should not meet a client under a second name on the far side of the join (R6).
	WriteBoolOrNull(w, "connected", c.has_connected, c.connected);
	w.EndObject();
}

// Single-client detail: the full list field set plus the detail-only ones, so
// the list schema is unaffected.
void WriteClientDetailObject(CJsonWriter &w, const webapi::ClientSnapshot &c)
{
	w.BeginObject();
	WriteClientBaseFields(w, c);
	w.Key("ed2k_user_id");
	w.ValueUInt(static_cast<uint64_t>(c.ed2k_user_id));
	w.Key("high_id");
	w.ValueBool(c.high_id);
	// Paired and nulled together, like ip/port/kad_port. server_ip is "" when unknown,
	// and "" is not a value an address field can legitimately take, unlike the 0
	// parts_offered_count uses as a real answer.
	const bool has_server = !c.server_ip.empty();
	WriteStringOrNull(w, "server_ip", has_server, c.server_ip);
	WriteStringOrNull(w, "server_name", has_server, c.server_name);
	WriteIntOrNull(w, "server_port", has_server, static_cast<int64_t>(c.server_port));
	// Nulled on the same condition as ip/port: a client with no recorded address has no
	// recorded Kad port either, and a raw 0 would spell absence differently from the
	// fields it is paired with.
	WriteIntOrNull(w, "kad_port", !c.ip.empty(), static_cast<int64_t>(c.kad_port));
	// Friends-list membership, distinct from the `friend_slot` reserved upload slot
	// above. The key drops the `is_` prefix per R4; the C++ member cannot, because
	// `friend` is a keyword. The one place key and member deliberately differ.
	w.Key("friend");
	w.ValueBool(c.is_friend);
	w.Key("credit_ratio");
	w.ValueDouble(c.credit_ratio);
	w.EndObject();
}

// Base shared-file fields, emitted into an already-open object so the list and
// detail writers share one definition.
//
// `detail` widens the `sources` group with the estimated range. A parameter rather
// than a second block in the detail writer, because a JSON object has to be emitted
// in one piece.
void WriteSharedBaseFields(CJsonWriter &w, const webapi::FileSnapshot &f, bool detail = false)
{
	w.Key("hash");
	w.ValueString(wxString::FromUTF8(f.hash.c_str()));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(f.name.c_str()));
	w.Key("ed2k_link");
	w.ValueString(wxString::FromUTF8(f.ed2k_link.c_str()));
	w.Key("size_bytes");
	w.ValueInt(static_cast<int64_t>(f.size));
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(f.shared.priority.c_str()));
	w.Key("priority_auto");
	w.ValueBool(f.shared.priority_auto);
	// A stated exception to R11: this wraps a single quantity. The identical figure is
	// `sources.complete` on a search result, so one access path works on every endpoint
	// that has the concept.
	w.Key("sources");
	w.BeginObject();
	w.Key("complete");
	w.ValueInt(static_cast<int64_t>(f.shared.complete_sources));
	if (detail) {
		// The estimated range of that same count, so it belongs beside it rather
		// than in a separate object.
		w.Key("complete_min");
		w.ValueInt(static_cast<int64_t>(f.shared.complete_sources_low));
		w.Key("complete_max");
		w.ValueInt(static_cast<int64_t>(f.shared.complete_sources_high));
	}
	w.EndObject();
	// Flattened (R11): the window belongs in the key, not a wrapper.
	w.Key("uploaded_bytes_session");
	w.ValueInt(static_cast<int64_t>(f.shared.uploaded_bytes_session));
	w.Key("uploaded_bytes_total");
	w.ValueInt(static_cast<int64_t>(f.shared.uploaded_bytes_total));
	w.Key("request_count_session");
	w.ValueInt(static_cast<int64_t>(f.shared.request_count_session));
	w.Key("request_count_total");
	w.ValueInt(static_cast<int64_t>(f.shared.request_count_total));
	// `accepts` was a verb doing duty as a plural noun.
	w.Key("accepted_request_count_session");
	w.ValueInt(static_cast<int64_t>(f.shared.accepted_request_count_session));
	w.Key("accepted_request_count_total");
	w.ValueInt(static_cast<int64_t>(f.shared.accepted_request_count_total));
	// `upload_speed_bytes_per_second` and `uploading` refresh every tick; `last_upload`
	// / `shared_since` are unix seconds, null when unknown -- never uploaded, or a
	// known.met entry predating the field. They were 0, which reads as 1970 rather than
	// "no idea".
	w.Key("upload_speed_bytes_per_second");
	w.ValueInt(static_cast<int64_t>(f.shared.upload_speed_bytes_per_second));
	// Read as a boolean, held an integer.
	w.Key("uploading_client_count");
	w.ValueInt(static_cast<int64_t>(f.shared.uploading_client_count));
	WriteIntOrNull(w,
		"last_upload_at",
		f.shared.last_upload != 0,
		static_cast<std::int64_t>(f.shared.last_upload));
	WriteIntOrNull(w,
		"shared_since_at",
		f.shared.shared_since != 0,
		static_cast<std::int64_t>(f.shared.shared_since));
	// Parts hashed so far by a Verify Local Data or an AICH rebuild; 0 when idle.
	// Through the accessor so a shared download, which amuled reports as a partfile,
	// still reads correctly.
	w.Key("hashed_part_count");
	w.ValueInt(static_cast<int64_t>(webapi::SharedHashingProgress(f)));
}

void WriteSharedObject(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	w.BeginObject();
	WriteSharedBaseFields(w, f);
	// Media rides the list item because the shared_added / shared_updated payload is
	// documented to match this object byte-for-byte, which is what lets a subscriber
	// skip the re-GET. The event has to carry media -- a re-extraction is otherwise
	// invisible, since the refresh endpoints answer 202 with no result -- so the list
	// carries it too.
	WriteMediaIfPresent(w, f);
	w.EndObject();
}

// Every list field plus the shared-table gaps and shared-applicable identity.
void WriteSharedDetailObject(CJsonWriter &w, const webapi::FileSnapshot &f)
{
	w.BeginObject();
	WriteSharedBaseFields(w, f, /*detail=*/true);
	w.Key("file_type");
	w.ValueString(wxString::FromUTF8(webapi::FileTypeToken(f.name).c_str()));
	w.Key("upload_ratio");
	w.ValueDouble(
		f.size > 0 ? static_cast<double>(f.shared.uploaded_bytes_total) / static_cast<double>(f.size)
			   : 0.0);
	w.Key("directory");
	// The on-disk directory (Temp while downloading, destination once completed), the
	// same value /downloads/{hash} reports. It was once masked with a placeholder while
	// incomplete, which hid nothing and cost clients a usable field; `incomplete` below
	// carries that state explicitly.
	w.ValueString(wxString::FromUTF8(f.on_disk_dir.c_str()));
	w.Key("incomplete");
	// Always present, so clients test it rather than probe for absence.
	// Detail-only, like `path`, so the SSE event rate is unaffected.
	w.ValueBool(f.IsIncompletePartfile());
	WriteStringOrNull(w, "aich_hash", !f.aich_hash.empty(), f.aich_hash);
	w.Key("total_part_count");
	w.ValueInt(static_cast<int64_t>(webapi::PartCountForSize(f.size)));
	WriteSharedAvailabilityParts(w, f);
	w.Key("upload_queue_count");
	w.ValueInt(static_cast<int64_t>(f.queued_count));
	w.Key("my_comment");
	w.ValueString(wxString::FromUTF8(f.comment.c_str()));
	w.Key("my_rating");
	w.ValueInt(static_cast<int64_t>(f.rating));
	WriteMediaIfPresent(w, f);
	w.EndObject();
}

// Server-side window shared by every list endpoint: `limit` (default 100),
// `offset`, `sort`, `order` (asc|desc) and `after` (keyset anchor). `total`,
// `offset` and `limit` are always emitted so a paging consumer can size its
// requests.
//
// `limit` DEFAULTS rather than meaning "everything when omitted". The old rule was
// not monotonic -- 500 rows, an error at 501, the whole collection at zero -- and
// it capped the explicit caller while leaving the naive one unbounded.
struct ListParams
{
	static const std::size_t kDefaultLimit = 100;
	std::size_t limit = kDefaultLimit;
	std::size_t offset = 0;
	std::string sort;  // empty = unsorted (native snapshot order)
	std::string after; // empty = no keyset anchor; needs an identity `sort`
	bool desc = false;
};

// One sortable column of a list endpoint.
//
// `less` is the ascending comparator. `after_less` is what makes the column usable
// as a KEYSET anchor: given the raw `?after=` token and a row, is the token ordered
// before it. Only an IDENTITY column provides one -- anchoring on a mutable column
// is meaningless, because the anchor's own value moves between two page requests
// and the window silently skips or repeats rows.
//
// Keyset rather than `offset` because an offset is a position, and a position
// shifts whenever the set changes size BELOW the cursor: delete one row from an
// already-fetched page and the next request starts one row late, so that row is
// never fetched by anything, and no SSE event mentions it either.
template <class T> struct ListSortColumn
{
	const char *name;
	std::function<bool(const T &, const T &)> less;
	std::function<bool(const std::string &, const T &)> after_less; // null == not anchorable
};

// Endpoint-specific sortable fields, in definition order. A vector, not a map,
// so an unknown `sort` value is simply a lookup miss and a 400.
template <class T> using ListComparators = std::vector<ListSortColumn<T>>;

// One member (possibly nested: SORT_BY(download.percent)) ascending. The column
// tables are long enough that a five-line lambda per column hid what they sort on.
#define SORT_BY(field) [](const auto &a, const auto &b) { return a.field < b.field; }

// The anchor half, for an identity column. Same member the column sorts on --
// they have to agree, or `after` would seek into a differently-ordered set.
#define ANCHOR_ON(field) [](const std::string &tok, const auto &r) { return tok < r.field; }

// Numeric identity (an ECID). The token is parsed once per upper_bound probe, so
// the seek is O(log n) rather than per row. A non-numeric token sorts before
// everything, so a malformed `after` yields the first page rather than an error.
#define ANCHOR_ON_NUM(field) \
	[](const std::string &tok, const auto &r) { \
		char *end = nullptr; \
		const unsigned long long v = std::strtoull(tok.c_str(), &end, 10); \
		return (end == tok.c_str()) ? true : v < static_cast<unsigned long long>(r.field); \
	}

// One row of GET /search. The listing is built from a live EC response rather than
// a snapshot vector, so it needs a materialised row before it can go through the
// same envelope every other collection uses. Absent values stay absent rather than
// becoming 0: `started_at` is unknowable for a search this process did not start,
// and `result_count` is unreported by an older daemon, which has to stay
// distinguishable from "found nothing".
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
	// `search_id`, the same key the SSE payloads and the results envelope use, so
	// a client never reads a search's id under two names.
	w.Key("search_id");
	w.ValueInt(static_cast<int64_t>(row.search_id));
	w.Key("query");
	w.ValueString(row.query);
	w.Key("type");
	w.ValueString(wxString::FromUTF8(row.kind.c_str()));
	w.Key("state");
	w.ValueString(wxString::FromUTF8(row.state.c_str()));
	WriteIntOrNull(w, "client_ecid", row.has_client_ecid, static_cast<int64_t>(row.client_ecid));
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

// The daemon hands searches back id-ascending, and id order is not recency:
// Kad ids carry SEARCH_ID_KAD_MASK and always sort above ed2k ones.
const ListComparators<SearchListRow> &SearchListComparators()
{
	static const ListComparators<SearchListRow> kComps = {
		{ "search_id", SORT_BY(search_id), ANCHOR_ON_NUM(search_id) },
		{ "query", SORT_BY(query) },
		{ "started_at", SORT_BY(started_at) },
		{ "result_count", SORT_BY(result_count) },
	};
	return kComps;
}

// /categories was the only list endpoint that never parsed ?limit/&offset/&sort/
// &order, so the same query string was a hard error on /downloads and a silent
// no-op here.
const ListComparators<webapi::CategorySnapshot> &CategoryComparators()
{
	static const ListComparators<webapi::CategorySnapshot> kComps = {
		{ "index", SORT_BY(index), ANCHOR_ON_NUM(index) },
		{ "name", SORT_BY(name) },
	};
	return kComps;
}

// Sort keys for peer rows, shared by /clients and the per-file client routes,
// which derive theirs from this set: a key added here reaches every peer list.
const ListComparators<webapi::ClientSnapshot> &ClientComparators()
{
	static const ListComparators<webapi::ClientSnapshot> kComps = {
		// Identity column: immutable, so it is the one a keyset sweep can anchor
		// on. Listed first because that is the order a paging client cares about.
		{ "ecid", SORT_BY(ecid), ANCHOR_ON_NUM(ecid) },
		{ "name", SORT_BY(client_name) },
		{ "software", SORT_BY(software) },
		// R7: each value is spelled exactly like the response key it orders by, and only
		// keys this row emits. /known_clients can also sort by session_count and
		// last_seen_at, which are not on a live peer.
		{ "uploaded_bytes_total", SORT_BY(uploaded_bytes_total) },
		{ "downloaded_bytes_total", SORT_BY(downloaded_bytes_total) },
		{ "upload_speed_bytes_per_second", SORT_BY(upload_speed_bytes_per_second) },
		{ "download_speed_bytes_per_second", SORT_BY(download_speed_bytes_per_second) },
	};
	return kComps;
}

std::unique_ptr<CHttpServer::Response> BadRequestPtr(const char *message)
{
	return std::make_unique<CHttpServer::Response>(ErrorResponse(400, "bad_request", message));
}

// Optional unsigned query parameter, with an inclusive upper bound.
//
// One parser for every count on the surface: the seven written by hand disagreed
// about what an unparseable value does and what an out-of-range one does, so the
// same typo was a hard error on `interval` and a silent behaviour change on `width`.
//
// Absent leaves `out` untouched, so the caller's default stands. `min` and `max`
// are inclusive, and the running value is bounded inside the loop so a long digit
// string cannot wrap before the range check sees it.
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

// Optional boolean query parameter: 1/0, true/false, yes/no, and 400 on anything
// else. One vocabulary for the whole surface -- `include_completed` used to read
// every other value as false while its neighbour `include_parts` answered 400, so
// the same typo was silent on one endpoint and fatal on the next.
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
// out-of-range `limit`/`offset`, and a bad `order`, are 400s. `sort` is validated
// later against the endpoint's comparator table (BuildListWindow).
std::unique_ptr<CHttpServer::Response> ParseListParams(const std::string &query, ListParams &out)
{
	const auto qmap = web_api_path::ParseQuery(query);
	// 1e9 on both, past any collection that can exist, so it reads as "no upper bound"
	// while still rejecting a fat-fingered limit.
	//
	// Not SIZE_MAX: `offset + limit` has to stay inside a 32-bit size_t for the 32-bit
	// builds, and 1e9 + 1e9 is the largest round pair that does. It is also inside JS's
	// exact-integer range, so a browser can send the ceiling and get back what it sent.
	if (qmap.count("limit")) {
		std::uint64_t v = 0;
		if (auto r = ParseUintParam(qmap, "limit", 0, 1000000000ull, v))
			return r;
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
	const auto after_it = qmap.find("after");
	if (after_it != qmap.end())
		out.after = after_it->second;
	return nullptr;
}

// Stable-sort the full set then slice to the window. `out_window` is filled with
// pointers into `items` (no copies), `out_total` with the pre-slice count. 400 when
// `params.sort` is set but absent from `comparators`.
template <class T>
std::unique_ptr<CHttpServer::Response> BuildListWindowFromPtrs(std::vector<const T *> &ptrs,
	const ListParams &params,
	const ListComparators<T> &comparators,
	std::vector<const T *> &out_window,
	std::size_t &out_total)
{
	out_total = ptrs.size();

	const ListSortColumn<T> *col = nullptr;
	if (!params.sort.empty()) {
		auto c = std::find_if(comparators.begin(), comparators.end(), [&](const auto &p) {
			return params.sort == p.name;
		});
		if (c == comparators.end())
			return BadRequestPtr("unknown `sort` field for this endpoint");
		col = &*c;
		const auto &cmp = col->less;
		std::stable_sort(ptrs.begin(), ptrs.end(), [&](const T *a, const T *b) {
			return params.desc ? cmp(*b, *a) : cmp(*a, *b);
		});
	}

	// Keyset seek, anchored on a value, so nothing that happened to the rows before it
	// can move the window.
	//
	// Rejected rather than ignored on a column that cannot anchor: falling back to the
	// whole set would hand a paging client the first page forever, which reads as "the
	// collection never grows" rather than as a fixable error.
	std::size_t seek = 0;
	if (!params.after.empty()) {
		if (!col || !col->after_less)
			return BadRequestPtr("`after` needs a `sort` field that identifies a row");
		if (params.desc)
			return BadRequestPtr("`after` is ascending-only; drop `order=desc`");
		const auto it = std::upper_bound(
			ptrs.begin(), ptrs.end(), params.after, [&](const std::string &tok, const T *r) {
				return col->after_less(tok, *r);
			});
		seek = static_cast<std::size_t>(it - ptrs.begin());
	}

	// `offset` counts from the seek, so `after` alone pages a collection and
	// the two compose for a caller that wants both.
	const std::size_t begin = std::min(seek + std::min(params.offset, out_total - seek), out_total);
	// Clamp the COUNT before adding it, never the sum. `begin + limit` first overflows
	// a 32-bit size_t on the ceilings this API accepts, and an inverted iterator range
	// is undefined behaviour rather than a big page.
	const std::size_t end = begin + std::min(params.limit, out_total - begin);
	out_window.assign(ptrs.begin() + begin, ptrs.begin() + end);
	return nullptr;
}

// Sort and window a list the caller owns as values. An endpoint whose records are
// not all in one contiguous vector -- /known_clients serves most rows out of a
// shared cache and materialises only the few it patches -- uses the pointer form
// above instead of copying the whole set.
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

// Emit the `total` / `offset` / `limit` pagination metadata.
//
// `limit` echoes the page size the caller asked for. It used to report the row count
// instead, which is not a page size and could not be used as one: a caller that
// stored it pinned its window to whatever the first response held, and re-sending
// it was a 400 once the list outgrew the cap.
void WritePageMeta(CJsonWriter &w, std::size_t total, const ListParams &params)
{
	w.Key("total");
	w.ValueUInt(total);
	w.Key("offset");
	w.ValueUInt(params.offset);
	// Never null now: every response has a real page size, so round-tripping
	// {total, offset, limit} straight back into the next request works.
	w.Key("limit");
	w.ValueUInt(params.limit);
}

// Extract the raw query string from a request target ("/x?a=1" -> "a=1").
std::string QueryOf(const CHttpServer::Request &req)
{
	std::string path, query;
	SplitPathAndQuery(req.target, path, query);
	return query;
}

// Envelope for every list endpoint: the list under its named key plus the
// pagination metadata, with ec_unavailable + 503 handled here so no handler repeats
// it. Records are addressed as pointers, so an endpoint whose rows are not all in
// one vector need not build one.
//
// State-free: takes no lock of its own, so a caller already holding CState's read
// lock can build a response inside it. m_mu is NOT recursive -- a second
// shared_lock taken while a writer is queued deadlocks.
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
	WritePageMeta(w, total, params);
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
	// No snapshot_at_* in the envelope: it churned the body bytes every refresher
	// tick and defeated the ETag cache. The ETag is the validator now.
	CJsonWriter w;
	w.BeginObject();
	w.Key(plural_key);
	w.BeginArray();
	for (const T *item : window)
		write_item(w, *item);
	w.EndArray();
	WritePageMeta(w, total, params);
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

} // namespace

// Mutation helpers. Every mutation handler follows:
//  1. AuthenticateRequest (bearer or cookie)
//  2. RequireAdmin
//  3. parse the JSON body
//  4. SendRecvSerialized the EC packet
//  5. EC_OP_NOOP is success; EC_OP_FAILED carries amuled's rejection
//  6. run RefresherTick inline so the response sees post-mutation state
//  7. return the updated resource, or 201 / 204 per HTTP convention

namespace
{

// JSON body parser: true on success, false + `err` otherwise. Non-object roots are
// rejected. The depth cap runs before the parse, so a deeply nested body cannot
// exhaust the handler thread's stack inside picojson's recursive descent. The
// scanner lives in JsonDepthScan.h, which keeps it reachable from its test.
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

// Relay the EC_OP_FAILED reply shape: amuled carries the rejection message in
// EC_TAG_STRING children, and the first is passed to the client. True means the
// caller short-circuits.
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

// The two category ops answer EC_OP_FAILED for a PARTIAL SUCCESS, not a failure:
// amuled created or updated the category, found it could not use the path, kept
// another one -- the incoming directory for a create, the previous path for an
// update -- and returns that in EC_TAG_CATEGORY_PATH beside the index. An update
// keeps name, comment, colour and priority; only the path is refused.
//
// Relaying it as a 400 would say the request failed while the category exists, and
// discard the one field that says what happened. A reply carrying no such tag is a
// genuine failure.
bool EcCategoryPathKept(const CECPacket *resp, std::string &kept_path)
{
	if (!resp || resp->GetOpCode() != EC_OP_FAILED)
		return false;
	const CECTag *t = resp->GetTagByName(EC_TAG_CATEGORY_PATH);
	if (!t)
		return false;
	kept_path = std::string(t->GetStringData().utf8_str());
	return true;
}

// Map our wire-string priorities back to amule's PR_* encoding, the inverse of
// PriorityName in Refresher.cpp. PR_AUTO=5 is the magic value stored as High plus
// the auto flag. The one place the file-priority vocabulary is declared.
//
// /downloads, /shared and /categories share the PR_* code space and differ only in
// which names they accept, deliberately. The .part.met loader clamps anything but
// PR_LOW/PR_NORMAL/PR_HIGH back to Normal on restart, so very_low and release are
// upload-side levels and are refused on the download path. Categories apply their
// priority to member files as a download priority (CDownloadQueue::SetCatPrio), so
// they inherit that set.
//
// Servers are deliberately NOT here: SRV_PR_* is a different code space in which
// the same word means a different number -- `low` is 0 for a file and 2 for a
// server -- so one table would lie about half its rows.
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
	// R9: a writable field accepts the values the same field returns. A category's
	// `priority` is rendered by PriorityName(), which can answer very_low and release,
	// so the write side must admit them. The download domain stays narrow because its
	// read side cannot produce them.
	{ "very_low", PR_VERY_LOW, kPrioShared | kPrioCategory },
	{ "low", PR_LOW, kPrioDownload | kPrioShared | kPrioCategory },
	{ "normal", PR_NORMAL, kPrioDownload | kPrioShared | kPrioCategory },
	{ "high", PR_HIGH, kPrioDownload | kPrioShared | kPrioCategory },
	{ "release", PR_VERYHIGH, kPrioShared | kPrioCategory },
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

// The rejection names exactly what this domain accepts, built from the table: five
// sites had their own copy, so a new level would have been announced in some and
// not others.
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

// /servers_update, /kad/update and /ipfilter/update are the same operation over
// three lists: take one http(s) URL, hand it to amuled in a single string tag, echo
// the effective URL back with a 202. amuled persists the URL into the matching
// preference itself, so none of them also PATCHes it here.
struct UrlFetchSpec
{
	// JSON body field carrying the URL, and the key echoed in the reply.
	const char *field;
	ec_opcode_t op;
	// Tag the URL travels in. EC_OP_IPFILTER_UPDATE reads the packet's first tag
	// whatever it is named, so that one uses EC_TAG_STRING to match what amulegui has
	// always sent.
	ec_tagname_t tag;
	// false: an absent field falls back to the configured URL the caller
	// passes in, instead of being a 400.
	bool url_required;
	// Run an inline RefresherTick so the caches, and the SSE diff built from them,
	// pick up whatever already landed.
	bool refresh_after;
};

// Pull the URL out of the body per `spec`, falling back to `configured` when the
// body omits it and the spec allows it.
//
// `configured` is null when there is no fallback: always for a url_required spec,
// and for the others while amuleapi has no preferences snapshot yet. A request
// needing the fallback then gets a 503 rather than a 400 blaming a URL that simply
// has not been read yet.
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
		// A configured URL predates this request, arriving over PATCH /preferences
		// or amuled's config, so it is not re-validated here.
		return true;
	}
	if (out_url.empty()) {
		rejection = ErrorResponse(400, "bad_request", (field + " must not be empty").c_str());
		return false;
	}
	// amuled hands the string straight to the HTTP downloader, so a bad scheme would
	// fail asynchronously with nowhere to report it. Rejecting here also beats the EC
	// "amuled rejected" wrapper for clarity.
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
	// 202 with no body: the URL echoed back came from the request, and the
	// download runs asynchronously -- its outcome arrives on the log channel.
	r.status = 202;
	r.content_type.clear();
	return r;
}

// JSON has one number type and picojson hands every one over as a double, so a
// field documented as an integer must say so: without this, `{"min_size_bytes":
// 2.9}` is accepted and silently truncated. One helper rather than a cast per site,
// because `v != (double)(int)v` cannot judge a byte count above INT_MAX, which is
// the range these fields live in.
inline bool IsIntegralJsonNumber(double v)
{
	return std::isfinite(v) && v == std::floor(v);
}

// MD4 hex to CMD4Hash; false unless 32 hex chars. Either case is tolerated --
// the route already lowercases what comes off the URL.
bool HashFromHex(const std::string &hex, CMD4Hash &out)
{
	if (hex.size() != 32)
		return false;
	return out.Decode(wxString::FromAscii(hex.c_str()));
}

// The {hash} twin of RequireEcidPath: a path hash that is not 32 hex characters is
// a malformed request, not a missing file. The find-based routes used to skip the
// check and fall through to their own 404, so a client could not tell "not a hash"
// from "valid hash, no such file" -- and the split ran through a single route,
// whose GET answered 404 where its own POST answered 400.
std::unique_ptr<CHttpServer::Response> RequireHashPath(const std::string &hash)
{
	CMD4Hash parsed;
	if (!HashFromHex(hash, parsed)) {
		return BadRequestPtr("path `{hash}` must be 32 hex characters");
	}
	return nullptr;
}

} // namespace

CHttpServer::Response CApiDispatcher::HandleDownloads(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// `?status=` selects which part of the queue to list: amuled holds finished
	// downloads in m_completedDownloads as a separate "awaiting clear" list, so "what
	// is transferring" and "what finished" are different row sets.
	//
	// It replaced `?include_completed=`, a boolean over a three-state axis: there was
	// no way to ask for completed-only, which is what a Finished view needs. `status`
	// is the key the download object already reports, so filter and field agree.
	//
	// GET /downloads/{hash} is unaffected: the caller named the file.
	enum class DownloadsFilter
	{
		Active,
		All,
		Completed,
	};
	DownloadsFilter filter = DownloadsFilter::Active;
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos)
			query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		if (qmap.count("include_completed")) {
			return ErrorResponse(400,
				"bad_request",
				"`include_completed` is not accepted; use "
				"`status=active|all|completed`");
		}
		const auto it = qmap.find("status");
		if (it != qmap.end()) {
			if (it->second == "active") {
				filter = DownloadsFilter::Active;
			} else if (it->second == "all") {
				filter = DownloadsFilter::All;
			} else if (it->second == "completed") {
				filter = DownloadsFilter::Completed;
			} else {
				return ErrorResponse(
					400, "bad_request", "`status` must be one of active, all, completed");
			}
		}
	}

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	static const ListComparators<webapi::FileSnapshot> kComps = {
		// Identity column: immutable, so it is what a keyset sweep anchors on.
		// Listed first because that is the order a paging client cares about.
		{ "hash", SORT_BY(hash), ANCHOR_ON(hash) },
		{ "name", SORT_BY(name) },
		// R7: each value is spelled exactly like the response key it orders by,
		// dotted for a nested one, so a field rename cannot orphan a sort value.
		{ "size_bytes", SORT_BY(size) },
		{ "progress.percent", SORT_BY(download.percent) },
		{ "speed_bytes_per_second", SORT_BY(download.speed_bytes_per_second) },
		{ "status", SORT_BY(download.status) },
	};
	if (auto r = RequireSnapshot(m_state))
		return *r;
	// Pointers into the live map rather than copies; see HandleSharedList.
	CHttpServer::Response resp;
	// Named captures; see HandleSharedList.
	m_state.WithFiles([&resp, &params, filter](const webapi::FileMap &files) {
		std::vector<const webapi::FileSnapshot *> ptrs;
		ptrs.reserve(files.size());
		for (const auto &entry : files) {
			const webapi::FileSnapshot &d = entry.second;
			if (!d.is_downloading)
				continue;
			const bool done = d.download.status == "completed";
			if (filter == DownloadsFilter::Active && done)
				continue;
			if (filter == DownloadsFilter::Completed && !done)
				continue;
			ptrs.push_back(&d);
		}
		resp = ListResponseFromPtrsUnlocked(
			"downloads",
			ptrs,
			[](CJsonWriter &w, const webapi::FileSnapshot &d) {
				// List mode omits `progress.parts`; the detail endpoint is where
				// parts ship.
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
	std::string role; // "downloading_from" | "uploading_to" | "both" | "none"
	bool a4af = false;
	std::vector<bool> parts;
	bool has_parts = false;
	// Whether each index actually addresses a chunk of THIS file that this row also
	// carries a bitmap for. Resolved in the handler, the only place that knows
	// part_count; false means the key goes out as null.
	bool next_requested_part_known = false;
	bool last_downloading_part_known = false;
};

// Sort keys, derived from the /clients set rather than restated, so the two
// surfaces cannot drift apart.
const ListComparators<FileClientRow> &FileClientComparators()
{
	static const ListComparators<FileClientRow> kComps = [] {
		// Both halves are lifted, not just the comparator: a column that can anchor a
		// keyset page on /clients must anchor one here too, or the two surfaces drift and
		// it shows up only as a paging client silently missing rows.
		ListComparators<FileClientRow> out;
		for (const auto &col : ClientComparators()) {
			auto less = col.less;
			auto after = col.after_less;
			out.push_back({ col.name,
				[less](const FileClientRow &a, const FileClientRow &b) {
					return less(a.client, b.client);
				},
				after ? std::function<bool(const std::string &, const FileClientRow &)>(
						[after](const std::string &tok, const FileClientRow &r) {
							return after(tok, r.client);
						})
				      : nullptr });
		}
		return out;
	}();
	return kComps;
}

void WriteFileClientRow(CJsonWriter &w, const FileClientRow &row, bool include_parts)
{
	w.BeginObject();
	WriteClientBaseFields(w, row.client);
	// This client's relation to THIS file, which the global /clients row cannot
	// express: "downloading_from" is we pull from it, "uploading_to" is it pulls from
	// us, "both" is each way, and "none" is a row that exists only because it is parked
	// here as an A4AF source.
	w.Key("role");
	w.ValueString(wxString::FromUTF8(row.role.c_str()));
	// Orthogonal to role on purpose: a peer can be parked on another file and
	// still be pulling this one from us, which a fourth role value could not say.
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
	// The two chunks the desktop's source bar paints on the bitmap: the one in flight
	// and the one queued behind it (GenericClientListCtrl.cpp, crPending and
	// crNextPending). They sit on the row rather than in WriteClientBaseFields because
	// they describe a peer's relation to ONE file, which also keeps them out of the
	// shared SSE client payload where a value moving every tick would be noise.
	//
	// Gated on include_parts: an index is meaningless without the bitmap it indexes.
	// Under the flag both keys are always present, null rather than omitted where the
	// index does not apply, so one query yields one row shape.
	if (include_parts) {
		WriteIntOrNull(w,
			"next_requested_part_index",
			row.next_requested_part_known,
			static_cast<int64_t>(row.client.next_requested_part));
		WriteIntOrNull(w,
			"downloading_part_index",
			row.last_downloading_part_known,
			static_cast<int64_t>(row.client.last_downloading_part));
	}
	w.EndObject();
}

// Resolve one peer's bitmap for `part_count` chunks, following the two wire
// conventions: an "all" flag means the core sent an empty tag for a full source,
// and a bitmap that cannot cover the file is dropped rather than padded.
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

// Serves both /downloads/{hash}/clients and /shared/{hash}/clients: the rows are the
// same object out of the same cache, since the relation to the file is a field
// rather than a path. The routes differ only in which role the hash must already
// have. A partfile with one completed chunk is in both at once.
CHttpServer::Response CApiDispatcher::HandleFileClients(
	const CHttpServer::Request &req, const std::string &key, bool require_downloading)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto r = RequireHashPath(key))
		return *r;

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
		// R12: named by direction. The old pair was `source` / `peer`, which is not
		// one axis -- `source` names a relation to the file, `peer` the entity.
		row.role = is_source && is_peer
				   ? "both"
				   : (is_source ? "downloading_from" : (is_peer ? "uploading_to" : "none"));
		row.a4af = is_a4af;
		ComputePartProgressPercent(m_state, client);
		if (include_parts) {
			// Which bitmap belongs to this row follows its direction: the download map
			// is the file we pull from the peer, the upload map the file it pulls from
			// us. A pure A4AF row has no bitmap for this file at all.
			if (is_source) {
				row.has_parts = ResolvePartBitmap(client.part_status,
					client.part_status_all,
					client.has_part_status,
					part_count,
					row.parts);
				// Both indices address the download map, so they describe THIS
				// file only on a source row. On a pure peer or A4AF row they
				// belong to whatever else the peer is pulling; left false, they
				// go out as null.
				//
				// has_parts is part of the test, not just the role: ResolvePartBitmap
				// declines a source that has sent no part status, or whose bitmap
				// cannot cover the file. Both reads must follow that call.
				row.next_requested_part_known =
					row.has_parts &&
					webapi::UsablePartIndex(client.has_next_requested_part,
						client.next_requested_part,
						part_count);
				row.last_downloading_part_known =
					row.has_parts &&
					webapi::UsableLastDownloadingPart(client.download_state,
						client.has_last_downloading_part,
						client.last_downloading_part,
						part_count);
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

	return ListResponse(
		m_state,
		"clients",
		rows,
		[include_parts](CJsonWriter &w, const FileClientRow &row) {
			WriteFileClientRow(w, row, include_parts);
		},
		params,
		FileClientComparators());
}

CHttpServer::Response CApiDispatcher::HandleClients(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	// Optional `?activity=uploading | downloading | active`.
	//
	// `activity`, not `filter`: "filter" named the mechanism rather than the axis, and
	// every list endpoint filters somehow. The values are client states, so they are
	// spelled the way `upload_state` / `download_state` spell them. `active` is the
	// union of the two; absent means every client the daemon knows.
	std::string activity;
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos)
			query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("activity");
		if (it != qmap.end())
			activity = it->second;
	}
	if (!activity.empty() && activity != "uploading" && activity != "downloading" &&
		activity != "active") {
		return ErrorResponse(400,
			"bad_request",
			"`activity` must be one of \"uploading\", \"downloading\", \"active\"");
	}

	auto clients = m_state.Clients();
	if (!activity.empty()) {
		auto matches = [&](const webapi::ClientSnapshot &c) {
			const bool up = (c.upload_state == "uploading");
			const bool down = (c.download_state == webapi::kDownloadStateDownloading);
			if (activity == "uploading")
				return up;
			if (activity == "downloading")
				return down;
			/* active */ return up || down;
		};
		clients.erase(std::remove_if(clients.begin(),
				      clients.end(),
				      [&](const webapi::ClientSnapshot &c) { return !matches(c); }),
			clients.end());
	}

	// Same derived field the per-file rows and the detail object carry. Omitted here,
	// it left the SSE payload contradicting EVENTS.md's "same field set as the /clients
	// list row".
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
		// Identity column: immutable, so it is what a keyset sweep anchors on.
		// Listed first because that is the order a paging client cares about.
		{ "hash", SORT_BY(hash), ANCHOR_ON(hash) },
		{ "name", SORT_BY(name) },
		// R7: spelled like the response key it orders by. The upload-side pair
		// rather than /downloads' progress.percent, which this row does not report.
		{ "size_bytes", SORT_BY(size) },
		{ "uploaded_bytes_total", SORT_BY(shared.uploaded_bytes_total) },
		{ "upload_speed_bytes_per_second", SORT_BY(shared.upload_speed_bytes_per_second) },
	};
	if (auto r = RequireSnapshot(m_state))
		return *r;
	// Pointers into the live map rather than copies -- the single biggest allocation
	// the daemon made per request. Safe inside the read lock because WriteSharedObject
	// reads only the snapshot it is handed.
	CHttpServer::Response resp;
	// Named captures rather than [&]: [&] would pull `this` in, putting m_state
	// within reach of a callback that must not touch it.
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

	if (auto r = RequireHashPath(key))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	// {hash} is the 32-char MD4. The URL is case-tolerant and State keys are
	// lowercase, so down-case before the O(1) lookup.
	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	// Bare object: list endpoints carry an envelope, a detail endpoint is the resource
	// itself. `include_parts=true` adds `progress.parts`, which the list omits: it can
	// be 100K+ entries for a multi-TiB download, uncapped.
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

	if (auto r = RequireHashPath(key))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	// {hash} is the 32-char MD4; down-case before the O(1) lookup, mirroring the
	// download detail handler.
	webapi::FileSnapshot s;
	if (!FindSharedByKey(m_state, key, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}

	// Bare object, same shape contract as GET /downloads/{hash}: every list field
	// plus the detail-only ones.
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

	if (auto r = RequireHashPath(key))
		return *r;

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
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(d.download.source_comments.size()));
	// True while an on-demand Kad notes lookup is in flight; poll until it flips.
	w.Key("kad_comment_lookup_running");
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

// Trigger an on-demand Kad NOTES lookup for this download. Asynchronous on amuled
// (up to ~45s); retrieved ratings appear via GET on the same path, alongside
// per-source comments.
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

	if (auto r = RequireHashPath(key))
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
	// 202 with no body. The field it used to carry could hold exactly one value, so it
	// said nothing the status code had not -- and `status` everywhere else on this
	// surface is a transfer state.
	r.status = 202;
	r.content_type.clear();
	return r;
}

CHttpServer::Response CApiDispatcher::HandleDownloadFilenames(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto r = RequireHashPath(key))
		return *r;

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
		w.Key("filename");
		w.ValueString(wxString::FromUTF8(kv.second.name.c_str()));
		w.Key("source_count");
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

// Defined below beside its FindServerByEcid / FindFriendByEcid siblings;
// declared here so the A4AF handlers can validate a `client_ecid`.
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

	if (auto r = RequireHashPath(key))
		return *r;

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
	else if (action == "swap_others")
		op = EC_OP_PARTFILE_SWAP_A4AF_OTHERS;
	else if (action == "swap_this_auto") {
		// A third action here was the odd one out: the other two move sources, while
		// this flipped a flag the download object reports. A flip cannot be retried
		// safely, so it is now PATCH /downloads/{hash} {"a4af_auto":...}.
		return ErrorResponse(400,
			"bad_request",
			"`swap_this_auto` is not accepted; set the flag with PATCH "
			"/downloads/{hash} `{\"a4af_auto\": true|false}`");
	} else {
		return ErrorResponse(400, "bad_request", "`action` must be one of swap_this, swap_others");
	}

	// `client_ecid` narrows swap_this from "every A4AF source of this file" to one
	// named source, which is what the desktop's per-peer swap does. The core has no
	// per-source form of the other two, so pairing it with them is a request that
	// cannot be honoured rather than one that quietly does something else.
	bool per_source = false;
	std::uint32_t client_ecid = 0;
	if (const auto cit = obj.find("client_ecid"); cit != obj.end()) {
		// Integrality checked, not just the sign: the message says integer,
		// and without it 2.9 was accepted and truncated to 2.
		if (!cit->second.is<double>() || cit->second.get<double>() < 0 ||
			cit->second.get<double>() !=
				static_cast<double>(static_cast<std::int64_t>(cit->second.get<double>()))) {
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
				409, "not_a4af_source", "that client is not an A4AF source of this download");
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

// Every bulk mutation reports one entry per input item under a unified `results`
// array, so a client submitting N items learns the fate of each without parallel
// arrays or a first-error-only summary.
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

// Emit `{"results":[{"id","ok"[,"error":{"code","message"}]}]}`. Aggregate status:
// every item ok -> `all_ok_status`; every item failed because the daemon was
// unreachable -> 503; any other mix -> 207 Multi-Status.
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

	// Before the first EC snapshot there are no preferences to read and
	// version_check_available defaults to false, so the capability check below used to
	// answer 409 "disabled on the connected daemon" during the window every client hits
	// at startup. 503 ec_unavailable is the retryable answer.
	if (auto r = RequireSnapshot(m_state))
		return *r;

	// The daemon owns the check; amuleapi only triggers it. Rejecting early avoids
	// an EC op that will fail and never exposes the daemon's localized reason.
	const auto prefs = m_state.Preferences();
	if (!(prefs.version_check_available && prefs.version_check_enabled)) {
		return ErrorResponse(409,
			"version_check_unavailable",
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
		// The only expected failure past the gate above is the daemon's throttle,
		// reported with an English code.
		//
		// Its own code, not the auth limiter's `rate_limited`: both answer 429, and a
		// client that cannot tell them apart treats a throttled update check as a lost
		// session. The Web UI did exactly that and logged the user out.
		return ErrorResponse(429,
			"version_check_throttled",
			"version check was throttled by the daemon; try again shortly");
	}

	// The check runs asynchronously; the result appears on a subsequent
	// GET /version.
	CHttpServer::Response r;
	// 202 with no body: "started" is what the status code says.
	r.status = 202;
	r.content_type.clear();
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

	// Body shape: {"links": ["ed2k://|file|...|/", ...], "category_index": 0}.
	picojson::value root;
	std::string parse_err;
	if (!ParseJsonObjectBody(req.body, root, parse_err)) {
		return ErrorResponse(400, "bad_request", parse_err.c_str());
	}
	const auto &obj = root.get<picojson::object>();

	std::vector<std::string> links;
	{
		// `links` only. A singular `ed2k_link` alias meant one input with two
		// spellings on the endpoint that already answers with the bulk `results`
		// envelope even for a single item, and every future field varying by arity
		// would have inherited the question.
		const auto it_array = obj.find("links");
		if (obj.find("ed2k_link") != obj.end()) {
			return ErrorResponse(400,
				"bad_request",
				"`ed2k_link` is not accepted; send `links` as an array, "
				"`{\"links\": [\"ed2k://...\"]}` for a single link");
		}
		{
			if (it_array == obj.end()) {
				return ErrorResponse(400,
					"bad_request",
					"required field missing: `links` (array of ed2k:// strings)");
			}
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
		const auto it = obj.find("category_index");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400,
					"bad_request",
					"`category_index` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			// The integrality test the message already promises: without it
			// 2.9 was accepted and truncated to 2.
			if (v < 0 || v > 255 || !IsIntegralJsonNumber(v)) {
				return ErrorResponse(
					400, "bad_request", "`category_index` must be in [0, 255]");
			}
			category = static_cast<std::uint8_t>(v);
		}
	}

	// One EC_OP_ADD_LINK packet per link: amuled's add-link op is single-link on the
	// wire, so the batching is ours and clients pay one round-trip. Accepted, failed
	// and disconnected-mid-batch are accumulated separately and reported together -- an
	// unconditional 503 on an EC blip would silently discard the links amuled had
	// already queued.
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

	// Inline-refresh so the response sees post-mutation state. amuled's ADD_LINK is
	// asynchronous -- the partfile is allocated and hashed before it appears in
	// m_filelist -- so the new entry may not surface until the next tick, and
	// GET /downloads is what surfaces it.
	(void)RefresherTick(m_app, m_state);

	// All accepted -> 202 (async); a mix -> 207; every link blocked by an EC
	// disconnect -> 503.
	return BulkResultsResponse(results, 202);
}

namespace
{
// The optional `my_comment` + `my_rating` pair shared by PATCH /downloads/{hash}
// and PATCH /shared/{hash}. Both must be present or neither. Sends
// EC_OP_SHARED_FILE_SET_COMMENT, which amuled resolves against the shared-files
// registry, so the file must be shared. A body with neither field is a no-op.
bool TrySetCommentRating(CamuleapiApp &app,
	const picojson::object &obj,
	const webapi::FileSnapshot &f,
	bool &applied,
	CHttpServer::Response &err)
{
	applied = false;
	const auto cit = obj.find("my_comment");
	const auto rit = obj.find("my_rating");
	const bool has_c = cit != obj.end();
	const bool has_r = rit != obj.end();
	if (!has_c && !has_r)
		return true;
	if (has_c != has_r) {
		err = ErrorResponse(400, "bad_request", "`my_comment` and `my_rating` must be set together");
		return false;
	}
	if (!cit->second.is<std::string>()) {
		err = ErrorResponse(400, "bad_request", "`my_comment` must be a string");
		return false;
	}
	const std::string comment = cit->second.get<std::string>();
	// MAXFILECOMMENTLEN (include/protocol/ed2k/Constants.h) = 50.
	if (comment.size() > 50) {
		err = ErrorResponse(400, "bad_request", "`my_comment` exceeds 50 characters");
		return false;
	}
	if (!rit->second.is<double>()) {
		err = ErrorResponse(400, "bad_request", "`my_rating` must be an integer in [0, 5]");
		return false;
	}
	const double rd = rit->second.get<double>();
	const int rating = static_cast<int>(rd);
	if (static_cast<double>(rating) != rd || rating < 0 || rating > 5) {
		err = ErrorResponse(400, "bad_request", "`my_rating` must be an integer in [0, 5]");
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

// The optional `name` (rename) field shared by PATCH /downloads/{hash} and PATCH
// /shared/{hash}, mapping to EC_OP_RENAME_FILE. Empty names and names containing
// path separators are rejected: amuled's RenameFile JoinPaths()es the value, so a
// separator would let the rename escape the file's directory.
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
	if (auto r = RequireHashPath(key))
		return *r;

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

	// Downstream EC ops still address by MD4 hash -- read it back off
	// the snapshot we just resolved.
	CMD4Hash file_hash;
	if (!HashFromHex(d.hash, file_hash)) {
		return ErrorResponse(500, "internal_error", "failed to decode partfile hash");
	}

	// Each field present in the body fires one EC mutation, in a fixed order (status,
	// priority, category) so the wire effect does not depend on JSON key order.
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

	// action: a command, not a state. The read side's `status` has eleven values,
	// of which this accepted three, in a different tense.
	{
		const auto it = obj.find("action");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400,
					"bad_request",
					"`action` must be one of \"pause\", \"resume\" or \"stop\"");
			}
			const std::string &v = it->second.get<std::string>();
			ec_opcode_t op;
			if (v == "pause")
				op = EC_OP_PARTFILE_PAUSE;
			else if (v == "resume")
				op = EC_OP_PARTFILE_RESUME;
			else if (v == "stop")
				op = EC_OP_PARTFILE_STOP;
			else {
				return ErrorResponse(400,
					"bad_request",
					"`action` must be one of \"pause\", \"resume\" or \"stop\"");
			}
			auto err = send_op(op, /*has_inner=*/false, static_cast<ec_tagname_t>(0), 0);
			if (err.status >= 400)
				return err;
			any_change = true;
		}
	}

	// priority: the kPrioDownload domain. "very_low" and "release" are upload-side
	// only and are refused here by design; see kFilePriorities.
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request", "`priority` must be a string");
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
		const auto it = obj.find("category_index");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400,
					"bad_request",
					"`category_index` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			// The integrality test the message already promises: without it
			// 2.9 was accepted and truncated to 2.
			if (v < 0 || v > 255 || !IsIntegralJsonNumber(v)) {
				return ErrorResponse(
					400, "bad_request", "`category_index` must be in [0, 255]");
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

	// a4af_auto: a SET, not a toggle.
	//
	// EC_OP_PARTFILE_SWAP_A4AF_THIS_AUTO flips the flag, which is what the desktop menu
	// wants and what an HTTP API must not expose: a client library can retry a request
	// without the caller knowing, and a retried flip lands on the opposite value.
	// EC_OP_PARTFILE_SET_A4AF_AUTO carries the value instead, so re-sending the same
	// body is a no-op rather than an undo.
	{
		const auto it = obj.find("a4af_auto");
		if (it != obj.end()) {
			if (!it->second.is<bool>()) {
				return ErrorResponse(400, "bad_request", "`a4af_auto` must be a boolean");
			}
			auto err = send_op(EC_OP_PARTFILE_SET_A4AF_AUTO,
				/*has_inner=*/true,
				EC_TAG_PARTFILE_A4AFAUTO,
				static_cast<std::uint8_t>(it->second.get<bool>() ? 1 : 0));
			if (err.status >= 400)
				return err;
			any_change = true;
		}
	}

	// comment + rating, both required together. Only settable on a file that is
	// shared; otherwise TrySetCommentRating returns 409.
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
			"`action`, `priority`, `category_index`, `my_comment`+`my_rating`, or `name`");
	}

	// Inline refresh so the response below sees post-mutation state.
	(void)RefresherTick(m_app, m_state);

	// Re-read the snapshot, falling back to the prior copy if the cache evicted it
	// between the mutation and this read.
	webapi::FileSnapshot d_after;
	if (!m_state.FindDownload(d.hash, d_after))
		d_after = d;

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	// The same shape GET /downloads/{hash} returns, not the narrower list row: a client
	// that PATCHed and stored the response used to hold a different object than one
	// that PATCHed and re-GETed, missing progress.parts and sixteen other keys.
	WriteDownloadObject(w, d_after, /*include_parts=*/true, /*detail=*/true);
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
	if (auto r = RequireHashPath(key))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot d;
	if (!FindDownloadByKey(m_state, key, d)) {
		return ErrorResponse(404, "not_found", "no download with that hash");
	}

	// DELETE handles ACTIVE downloads only. Completed entries live in amuled's
	// m_completedDownloads staging list, and the only EC op that touches it is
	// EC_OP_CLEAR_COMPLETED, which acks the notification rather than deleting the file
	// from Incoming. Conflating the two under one verb confused operators who expected
	// DELETE to remove a file from disk, so the completed case goes through
	// POST /downloads_clear_completed.
	if (d.download.status == "completed") {
		return ErrorResponse(409,
			"download_completed",
			"DELETE only removes active downloads (deletes .part/.met "
			"files from disk). Use POST /downloads_clear_completed "
			"with optional {\"hash\":\"...\"} body to clear a completed "
			"entry's post-completion notification; the file in the "
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

	// Inline refresh: the next GET /downloads must not show the deleted entry.
	// Eviction happens via FILE_REMOVED in the GET_UPDATE response.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	// 204 with no body: everything this used to echo came from the request URL,
	// and `ok` restated the status code.
	r.status = 204;
	r.content_type.clear();
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

	// Two shapes share this endpoint: no body clears every completed entry, and
	// {"hash": "<md4hex>"} clears that one, which must currently be a download with
	// status=="completed" (active or unknown hashes 404). The response envelope is
	// identical either way, so a client wrapping the call need not fork on its input.
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
		// Unknown keys are ignored rather than 400, so adding a flag later does
		// not break old clients.
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
		// Nothing to do. An empty `results` array is the no-op, and stays distinct
		// from "amuled rejected", which is a 4xx with an error envelope.
		return BulkResultsResponse({}, 200);
	}

	// One EC roundtrip with all ECIDs (per amulegui's pattern at
	// amule-remote-gui.cpp).
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

	// Inline refresh -- the response below + the next GET both must
	// show the post-clear state.
	(void)RefresherTick(m_app, m_state);

	// One entry per hash, in the envelope every other multi-item mutation uses.
	// `cleared` was a count and `cleared_hashes` a bare array, so a per-entry failure
	// had nowhere to appear.
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

// Dotted-quad IPv4 to host-order uint32, the encoding EC_TAG_*_IP uses.
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
	// `ecid` is the URL key for /servers/{ecid} and /servers/{ecid}/connect. The
	// same key is surfaced on /clients for the same reason; servers got it later.
	w.Key("ecid");
	w.ValueInt(static_cast<int64_t>(s.ecid));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(s.name.c_str()));
	w.Key("description");
	w.ValueString(wxString::FromUTF8(s.description.c_str()));
	// `software_version`, not `version`: every version on this surface is named for its
	// subject, and a bare `version` beside them reads as the API's own. null, not "",
	// for the reason the two peer writers null it: a server that never reported one is
	// unknown, and a client normalizing this key across /servers, /clients and
	// /known_clients should not learn the rule twice (R10).
	WriteStringOrNull(w, "software_version", !s.version.empty(), s.version);
	w.Key("address");
	w.ValueString(wxString::FromUTF8(s.address.c_str()));
	// The bare IP beside the "ip:port" form. Every client needed it and had
	// to re-parse `address` to get it.
	w.Key("ip");
	w.ValueString(wxString::FromUTF8(s.address.substr(0, s.address.rfind(':')).c_str()));
	// ISO 3166-1 alpha-2 (lowercase); null when GeoIP is off/unresolved (#440).
	WriteStringOrNull(w, "country_code", !s.country_code.empty(), s.country_code);
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(s.port));
	w.Key("user_count");
	w.ValueInt(static_cast<int64_t>(s.users));
	w.Key("max_user_count");
	w.ValueInt(static_cast<int64_t>(s.max_users));
	w.Key("file_count");
	w.ValueInt(static_cast<int64_t>(s.files));
	// 0 means the server has not reported a limit yet, not a limit of zero, so a
	// UI can render it blank the way the desktop's Soft/Hard Files columns do.
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
	// `permanent`, not `static`: `static` is a reserved word in C++, Java and C#,
	// so a generated client has to mangle it.
	w.Key("permanent");
	w.ValueBool(s.is_static);
	// Written as a pre-built fragment from the shared tables so this object and
	// the SSE payload in EventDiff.cpp cannot drift apart.
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
	// `save_path`, not `path`: it is where finished files in this category
	// land, which `path` did not distinguish from directories.incoming_path.
	w.Key("save_path");
	w.ValueString(wxString::FromUTF8(c.path.c_str()));
	w.Key("comment");
	w.ValueString(wxString::FromUTF8(c.comment.c_str()));
	// "#rrggbb", not the raw 24-bit integer. Mind the byte order: the core packs it as
	// 0x00BBGGRR -- red in the LOW byte (CMuleColour) -- so a naive hex print of the
	// integer yields #bbggrr, reversed.
	w.Key("color");
	{
		const unsigned r = c.color & 0xFF;
		const unsigned g = (c.color >> 8) & 0xFF;
		const unsigned b = (c.color >> 16) & 0xFF;
		w.ValueString(wxString::Format(wxT("#%02x%02x%02x"), r, g, b));
	}
	w.Key("priority");
	w.ValueString(wxString::FromUTF8(c.priority.c_str()));
	w.EndObject();
}

} // namespace

void WriteFriendObject(CJsonWriter &w, const webapi::FriendSnapshot &f)
{
	w.BeginObject();
	// The friend's own EC handle; `client_ecid` below points OUT of this object. Like
	// every ECID it does not survive an amuled restart -- `user_hash` is the durable
	// reference, when the friend has one.
	w.Key("ecid");
	w.ValueInt(static_cast<int64_t>(f.ecid));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(f.name.c_str()));
	w.Key("user_hash");
	w.ValueString(wxString::FromUTF8(f.user_hash.c_str()));
	// null rather than "" when the daemon has not reported an address (R10).
	WriteStringOrNull(w, "ip", !f.ip.empty(), f.ip);
	// Paired with ip, the way WriteKnownClientObject nulls ip/port/kad_port
	// together: a 0 port on an address-less friend is the R10 sentinel.
	WriteIntOrNull(w, "port", !f.ip.empty(), static_cast<int64_t>(f.port));
	// The live peer this friend is linked to, joinable against /clients. null rather
	// than 0 when not connected: a client joining naively on the raw value was building
	// GET /clients/0 and taking a 404.
	WriteIntOrNull(w, "client_ecid", f.client_ecid != 0, static_cast<int64_t>(f.client_ecid));
	// Whether a socket to the peer is actually up, not whether the daemon holds a
	// client object for it -- which it does from the first contact ATTEMPT. null when
	// the daemon predates EC_TAG_CLIENT_CONNECTED: unknown, not offline.
	WriteBoolOrNull(w, "connected", f.has_connected, f.connected);
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
		// Identity column: immutable, so a keyset sweep can anchor on it.
		{ "ecid", SORT_BY(ecid), ANCHOR_ON_NUM(ecid) },
		{ "name", SORT_BY(name) },
		// R7: spelled like the response keys they order by.
		{ "user_count", SORT_BY(users) },
		{ "ping_ms", SORT_BY(ping_ms) },
		{ "file_count", SORT_BY(files) },
	};
	return ListResponse(m_state, "servers", m_state.Servers(), WriteServerObject, params, kComps);
}

namespace
{

// Parse an integer ECID from a path capture. Returns false on negative,
// overflow, or non-digit content.
bool ParseEcidPath(const std::string &s, std::uint32_t &out)
{
	if (s.empty())
		return false;
	char *end = nullptr;
	// strtoull (not strtoul) because `unsigned long` is 32-bit on Windows -- there the
	// `v > 0xFFFFFFFFu` guard below would be a tautology and an out-of-range segment
	// like `99999999999` would saturate to 0xFFFFFFFF, silently matching an actual ECID
	// 0xFFFFFFFF.
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
// ` if (auto r = RequireEcidPath(caps["ecid"], ecid)) return *r;`. The status, the
// code and the sentence are part of the API contract, not local wording.
std::unique_ptr<CHttpServer::Response> RequireEcidPath(const std::string &s, std::uint32_t &out)
{
	if (!ParseEcidPath(s, out)) {
		return BadRequestPtr("path `{ecid}` must be a non-negative integer");
	}
	return nullptr;
}

// The FindXByEcid family. The EC remove/slot ops are idempotent about unknown
// ids, so handlers look the id up first and 404 rather than answer a cheerful 200.
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

// The category set as a client sees it, which is not quite what amuled holds.
//
// amuled's EC suppresses the whole `EC_TAG_PREFS_CATEGORIES` block when no custom
// categories exist, and starts including index 0 once the first custom one is
// added, so a synthetic index-0 entry is injected when missing. amuled's
// `defaultcat` is also constructed with an empty title and path, so the name and
// `directories.incoming_path` are filled in here -- unconditionally, or /categories/0
// would answer "Default" on a daemon with no custom categories and "" as soon as
// the operator added one.
//
// Both read routes go through here; mutations deliberately do not, since the
// synthetic entry is a read-shape convenience with nothing behind it to PATCH.
std::vector<webapi::CategorySnapshot> CategoriesWithDefault(const webapi::CState &state)
{
	std::vector<webapi::CategorySnapshot> cats = state.Categories();
	const auto fill_default = [&state](webapi::CategorySnapshot &c) {
		c.name = "Default";
		c.path = state.Preferences().directories.incoming_path;
	};
	for (auto &c : cats) {
		if (c.index == 0) {
			fill_default(c);
			return cats;
		}
	}
	webapi::CategorySnapshot d;
	d.index = 0;
	d.priority_code = 0; // PR_LOW (matches amuled default)
	d.priority = "low";
	fill_default(d);
	cats.insert(cats.begin(), std::move(d));
	return cats;
}

// Category counterpart to the FindXByEcid family above; three handlers walked
// m_state.Categories() with the same loop.
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

// --- Chat (issue #971) -------------------------------------------------
//
// Conversations are keyed on "<ip>:<port>", the readable form of the GUI_ID the
// wire already uses. Stable across peer reconnects (unlike an ECID), needs no
// invented identifier, and converts straight back to the GUI_ID the EC ops want.

namespace
{

// Parse "<ip>:<port>" back into a GUI_ID. Strict on purpose: anything that is
// not four dotted octets plus a port would address some other conversation.
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
	// null, not 0, when the core has not stamped it: the send response builds
	// its echo through here before any timestamp exists.
	WriteIntOrNull(w, "sent_at", m.timestamp != 0, static_cast<int64_t>(m.timestamp));
	w.EndObject();
}

void WriteChatObject(CJsonWriter &w, const webapi::ChatSessionSnapshot &s)
{
	w.BeginObject();
	w.Key("address");
	w.ValueString(wxString::FromUTF8(s.PeerKey().c_str()));
	w.Key("ip");
	w.ValueString(wxString::FromUTF8(s.ip.c_str()));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(s.port));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(s.DisplayName().c_str()));
	// null rather than 0 for "no live connection" / "not a friend": 0 is not how
	// this surface spells absence, and /clients/0 is a 404 waiting to happen.
	WriteIntOrNull(w, "client_ecid", s.client_ecid != 0, static_cast<int64_t>(s.client_ecid));
	WriteIntOrNull(w, "friend_ecid", s.friend_ecid != 0, static_cast<int64_t>(s.friend_ecid));
	// Same rule as the /friends row: reachability, not object existence.
	WriteBoolOrNull(w, "connected", s.has_connected, s.connected);
	w.Key("message_count");
	w.ValueInt(static_cast<int64_t>(s.messages.size()));
	w.Key("last_message_id");
	w.ValueInt(static_cast<int64_t>(s.LastMsgId()));
	// null, not 0: a session with no messages has no last-message time, and
	// 0 reads as 1970 (R10).
	WriteIntOrNull(w,
		"last_message_at",
		!s.messages.empty(),
		static_cast<int64_t>(s.messages.empty() ? 0 : s.messages.back().timestamp));
	// The transcript itself is deliberately NOT on the list: a 50-session store at 200
	// messages each would be 10 000 objects per list read. null, not omitted: a session
	// with no messages yet has no last message.
	w.Key("last_message");
	if (!s.messages.empty())
		WriteChatMessageObject(w, s.messages.back());
	else
		w.ValueNull();
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

	// Never sent blind: a daemon predating EC_OP_GET_CLIENT_HISTORY reaches the
	// unknown-opcode branch of ProcessRequest2(), which asserts before it reaches the
	// EC_OP_FAILED it would otherwise answer with -- so simply trying the request takes
	// the core down.
	if (!m_app.IsServerClientHistoryActive()) {
		return ErrorResponse(
			503, "ec_unsupported", "the connected amuled does not serve the client history");
	}

	if (auto r = RequireSnapshot(m_state))
		return *r;

	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;

	// One fetch per process. From here the refresher maintains it: every tick folds the
	// connected peers back in, which is the whole of what can change -- credit totals
	// only grow during a transfer and last-seen is written at disconnect. Two
	// concurrent first requests can both fetch; the second replaces the first with an
	// equivalent store, which is not worth a lock.
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
			// An answer we cannot read latches nothing: the store is loaded once
			// and never re-read, so installing an empty one here would serve an
			// empty history for the life of the process. No reachable path today,
			// but the cost of being wrong is permanent.
			return ErrorResponse(502,
				"amuled_response_invalid",
				"the core answered the history request with an unknown reply");
		}
		m_state.SetKnownClients(std::move(rows));
	}

	static const ListComparators<webapi::KnownClientSnapshot> kComps = {
		// Identity column: immutable, so a keyset sweep can anchor on it.
		{ "user_hash", SORT_BY(user_hash), ANCHOR_ON(user_hash) },
		{ "name", SORT_BY(client_name) },
		{ "software", SORT_BY(software) },
		// R7: each value is spelled exactly like the response key it orders by.
		{ "first_seen_at", SORT_BY(first_seen_at) },
		{ "last_seen_at", SORT_BY(last_seen_at) },
		{ "session_count", SORT_BY(session_count) },
		{ "uploaded_bytes_total", SORT_BY(uploaded_bytes_total) },
		{ "downloaded_bytes_total", SORT_BY(downloaded_bytes_total) },
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
	// request. The daemon's order is most-recently-active first.
	const std::vector<webapi::ChatSessionSnapshot> chats = m_state.Chats();

	static const ListComparators<webapi::ChatSessionSnapshot> kComps = {
		// Identity column: immutable, so a keyset sweep can anchor on it.
		{ "client_ecid", SORT_BY(client_ecid), ANCHOR_ON_NUM(client_ecid) },
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
		return ErrorResponse(400, "bad_request", "path `{address}` must be `<ip>:<port>`");
	}

	const std::vector<webapi::ChatSessionSnapshot> chats = m_state.Chats();
	const webapi::ChatSessionSnapshot *session = FindChat(chats, gui_id);
	if (!session) {
		return ErrorResponse(404, "not_found", "no chat session with that peer");
	}

	// `since_message_id` is a safe polling cursor: ids are monotonic per daemon
	// process, so a client never sees a duplicate and never skips one. They reset when
	// the daemon restarts, which also empties the store.
	std::uint32_t since_id = 0;
	std::size_t tail = 0;
	const auto qmap = web_api_path::ParseQuery(QueryOf(req));
	{
		std::uint64_t v = since_id;
		if (auto r = ParseUintParam(qmap, "since_message_id", 0, 0xFFFFFFFFull, v))
			return *r;
		since_id = static_cast<std::uint32_t>(v);
	}
	{
		// `tail`, not `limit`. This selects the last N of the window rather than a page
		// of it, which is what the log endpoints already call `tail`; `limit` is the
		// paginated meaning on nine other collections.
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
	// `tail` means the LAST n, matching "show me the tail of this conversation";
	// combined with since_id it trims the same window from the front, so the newest are
	// always the ones kept.
	if (tail && selected.size() > tail) {
		selected.erase(selected.begin(), selected.end() - static_cast<std::ptrdiff_t>(tail));
	}

	CJsonWriter w;
	w.BeginObject();
	w.Key("address");
	w.ValueString(wxString::FromUTF8(session->PeerKey().c_str()));
	w.Key("messages");
	w.BeginArray();
	for (const webapi::ChatMessageSnapshot *m : selected)
		WriteChatMessageObject(w, *m);
	w.EndArray();
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(session->messages.size()));
	w.Key("last_message_id");
	w.ValueInt(static_cast<int64_t>(session->LastMsgId()));
	w.EndObject();
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	FinalizeJsonBody(w, r);
	return r;
}

// Shared by all three send forms. `target` is the already-built EC tag naming the
// recipient -- a GUI_ID, a live peer's ECID, or a friend's ECID. The friend form is
// the one that reaches an OFFLINE friend, via the stored ip:port.
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

	// 202, not 200: the core acknowledges that it queued the message on the peer
	// connection, not that the peer received it. An unreachable peer is not an error --
	// the desktop optimistically prints *** Connecting to Client ***.
	CJsonWriter w;
	w.BeginObject();
	// The nested `message` object is the created resource, built through the same writer
	// GET /chats/{address}/messages uses. `sent_at` is null here and only here:
	// EC_OP_CHAT_SEND answers with ids and no timestamp.
	w.Key("address");
	w.ValueString(wxString::FromUTF8(webapi::ChatPeerKeyFromGuiId(gui_id).c_str()));
	w.Key("message");
	webapi::ChatMessageSnapshot sent;
	sent.id = msg_id;
	sent.outgoing = true;
	sent.text = text;
	WriteChatMessageObject(w, sent);
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
		return ErrorResponse(400, "bad_request", "path `{address}` must be `<ip>:<port>`");
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
		return ErrorResponse(400, "bad_request", "path `{address}` must be `<ip>:<port>`");
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

	CHttpServer::Response r;
	r.status = 204;
	r.content_type.clear();
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
		// Identity column: immutable, so a keyset sweep can anchor on it.
		{ "ecid", SORT_BY(ecid), ANCHOR_ON_NUM(ecid) },
		{ "name", SORT_BY(name) },
		{ "connected",
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
		if (!v.is<double>() || v.get<double>() < 0 ||
			v.get<double>() != static_cast<double>(static_cast<std::int64_t>(v.get<double>()))) {
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
		// Manual add. The EC handler wants all four tags present, so an omitted
		// hash is sent empty and an omitted name defaults to the address.
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
			if (!IsIntegralJsonNumber(d) || d < 1 || d > 65535) {
				return ErrorResponse(
					400, "bad_request", "`port` must be an integer in 1..65535");
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

	// Tick so the new record is in the snapshot the caller's follow-up GET
	// reads, even though this response does not carry it.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	// 202 with no body: EC's FRIEND op answers success or failure and never returns the
	// created object, so naming it here would mean diffing the snapshot against a
	// pre-add copy and hoping the inline refresh won.
	r.status = 202;
	r.content_type.clear();
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
	// 204 with no body -- see the mutation-response rule in REFERENCE.md.
	r.status = 204;
	r.content_type.clear();
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

	// Only one friend can hold the slot, so granting it here clears whoever held it
	// before -- the tick picks up both changes and both emit an SSE event, not just the
	// friend named in the URL.
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

	// Inline refresh so the new server is in the next /servers response.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	// 202 with no body: amuled's EC op answers success or failure and does not
	// return the created object, so anything reported here would be a guess.
	r.status = 202;
	r.content_type.clear();
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

	// EC_OP_SERVER_CONNECT routes through Get_EC_Response_Server, which looks the
	// server up by IPv4 (ExternalConn.cpp), so build EC_TAG_SERVER with the IPv4 and
	// port from our cache.
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

	// Inline refresh so /status reflects "connecting" immediately.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	// 202: the connect is asynchronous -- the outcome shows up on /status and SSE.
	r.status = 202;
	r.content_type.clear();
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
	// 204 with no body -- see the mutation-response rule in REFERENCE.md.
	r.status = 204;
	r.content_type.clear();
	return r;
}

CHttpServer::Response CApiDispatcher::HandleServerUpdateFromUrl(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// amuled streams the new server list into its CServerList asynchronously over the
	// next few ticks (CServerList::UpdateServerMetFromURL), so the `server_added` SSE
	// events keep firing on subsequent natural ticks.
	static const UrlFetchSpec kSpec = {
		"url", EC_OP_SERVER_UPDATE_FROM_URL, EC_TAG_SERVERS_UPDATE_URL, true, true
	};
	std::string url;
	CHttpServer::Response rejection;
	if (!ResolveFetchUrl(req, kSpec, nullptr, url, rejection)) {
		return rejection;
	}
	return UrlFetchOp(m_app, m_state, kSpec, url);
}

// One "<ip>:<port>" selector, parsed. Split out from the lookup so a selector that
// cannot be parsed is distinguishable from one that parses but names no server we
// know -- a 400 and a 404 respectively.
struct IpPortSelector
{
	std::uint32_t ip_he; // host order, as ServerSnapshot::ip holds it
	std::uint16_t port;
};

// Accepts a dotted quad and a port in 1..65535, nothing else. Hostname forms are
// deliberately rejected: matching ServerSnapshot::address would match the wire
// "name" tag, which can be a synthetic display string ("Eserver No.1"), so a DELETE
// by address could remove the wrong row. boost::optional, not std::optional: the
// house spelling in this file.
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

	// Only genuine syntax failures are reported here. 0.0.0.0 parses fine and is
	// rejected by the caller instead, so the two get error messages that describe what
	// actually happened.
	IpPortSelector sel;
	sel.ip_he = 0;
	if (!ParseIpv4Dotted(ip_str, sel.ip_he))
		return boost::none;

	sel.port = static_cast<std::uint16_t>(port);
	return sel;
}

// Resolve a selector to a server ECID, in the same shape as RequireAdmin and
// RequireSnapshot: nullptr means @a ecid is set and the caller may proceed,
// otherwise it is the response to return.
std::unique_ptr<CHttpServer::Response> ResolveServerEcid(
	const webapi::CState &state, const std::string &ip_port, std::uint32_t &ecid)
{
	const auto sel = ParseIpPortSelector(ip_port);
	if (!sel) {
		return std::make_unique<CHttpServer::Response>(ErrorResponse(400,
			"bad_request",
			"malformed ip:port selector: expected a dotted quad and a port in 1..65535"));
	}
	// 0.0.0.0 is well-formed but is not a server address, and must not reach the lookup
	// below: a ServerSnapshot whose EC_TAG_SERVER_IP the daemon did not ship keeps
	// `ip == 0`, so a 0.0.0.0 selector would resolve to whichever such row happened to
	// share the port -- acting on a server the caller never named.
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
	// Delegate to the ECID-keyed handler, passing the resolved ECID as a decimal string.
	std::ostringstream os;
	os << ecid;
	return HandleServerConnect(req, os.str());
}

// PATCH /servers/{ecid} -- priority and/or static flag (#692).
//
// EC_OP_SERVER_SET_STATIC_PRIO carries EC_TAG_SERVER as a plain ECID integer,
// unlike EC_OP_SERVER_REMOVE next door which carries an EC_IPv4_t. It applies
// EC_TAG_SERVER_PRIO / EC_TAG_SERVER_STATIC only when present, so a partial update
// is native to the wire. amuled answers EC_OP_NOOP whether or not the ECID
// resolved, so the 404 has to come from checking the snapshot here.
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
	if (const auto it = obj.find("permanent"); it != obj.end()) {
		if (!it->second.is<bool>()) {
			return ErrorResponse(400, "bad_request", "`permanent` must be a bool");
		}
		is_static = it->second.get<bool>();
		has_static = true;
	}

	if (!has_prio && !has_static) {
		return ErrorResponse(
			400, "bad_request", "body must include at least one of `priority`, `permanent`");
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

	// Inline refresh so the next GET /servers and the SSE stream show the new values.
	(void)RefresherTick(m_app, m_state);

	// A mutation answers with the resource -- see REFERENCE.md. The inline
	// refresh above is what makes the re-read see the values just written.
	webapi::ServerSnapshot s_after;
	if (!FindServerByEcid(m_state, ecid, s_after)) {
		// The server went away between the patch and the re-read. Nothing
		// to describe, and inventing a body would be worse than saying so.
		CHttpServer::Response gone;
		gone.status = 204;
		gone.content_type.clear();
		return gone;
	}
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	WriteServerObject(w, s_after);
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
	// Injects the synthetic index-0 category -- see CategoriesWithDefault.
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

	// Dashboard() rather than Kad(): `connected_since` below lives on the status
	// snapshot, and taking both halves in one shared_lock keeps the timestamp
	// describing the same tick as the rest of the payload.
	const webapi::CState::DashboardSnapshot d = m_state.Dashboard();
	const webapi::KadSnapshot &k = d.kad;
	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	// Bare object (Q3 -- Kad is a single resource, not a list).
	w.Key("state");
	w.ValueString(wxString::FromUTF8(k.state.c_str()));
	// Our own Kademlia node id, null while Kad is not running. Persisted by the daemon,
	// so unlike every other identifier for the local node it survives a restart.
	WriteStringOrNull(w, "node_id", !k.node_id.empty(), k.node_id);
	// Two independent measurements, not a verdict and a refinement. firewalled_tcp is a
	// vote needing two peers to confirm reachability; firewalled_udp is a directed
	// test. LAN mode forces both to false. All three are null unless Kad is connected:
	// firewalled_tcp comes from a connstate bit that outlives the disconnect, the
	// others from defaults.
	WriteBoolOrNull(w, "firewalled_tcp", k.has_firewalled_tcp, k.firewalled_tcp);
	WriteBoolOrNull(w, "firewalled_udp", k.has_firewalled_udp, k.firewalled_udp);
	WriteBoolOrNull(w, "lan_mode", k.has_lan_mode, k.lan_mode);
	// Same value GET /status reports as kad.connected_since; 0 when
	// not connected, so gate on `state` rather than on a nonzero.
	w.Key("connected_since_at");
	w.ValueInt(static_cast<int64_t>(d.status.kad_connected_since));
	// Ours, as opposed to `buddy.ip` below -- which is why it is not plain `ip`.
	WriteStringOrNull(w, "public_ip", !k.public_ip.empty(), k.public_ip);
	WriteKadNetworkObject(w, k);
	// Both objects are null-valued unless Kad is connected: amuled only ships these
	// tags inside its own connected gate, so the numbers below were the struct defaults
	// rather than a measurement.
	w.Key("indexed");
	w.BeginObject();
	WriteIntOrNull(w, "sources", k.has_indexed, static_cast<int64_t>(k.indexed_sources));
	WriteIntOrNull(w, "keywords", k.has_indexed, static_cast<int64_t>(k.indexed_keywords));
	WriteIntOrNull(w, "notes", k.has_indexed, static_cast<int64_t>(k.indexed_notes));
	WriteIntOrNull(w, "load_percent", k.has_indexed, static_cast<int64_t>(k.indexed_load));
	w.EndObject();
	w.Key("buddy");
	w.BeginObject();
	WriteStringOrNull(w, "state", k.has_buddy, k.buddy_status);
	WriteStringOrNull(w, "ip", k.has_buddy, k.buddy_ip);
	WriteIntOrNull(w, "port", k.has_buddy, static_cast<int64_t>(k.buddy_port));
	w.EndObject();
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

namespace
{

// `?tail=N` parser. An absent parameter yields 0, which is every caller's contract
// for "return everything". 100k lines is the cap: a bogus `?tail=2147483647` would
// otherwise try to serialise the entire wxString through the JSON escaper.
// Out-of-range values are a 400, never a silent clamp.
std::unique_ptr<CHttpServer::Response> ParseTailParam(const std::string &query, std::size_t &out)
{
	const auto qmap = web_api_path::ParseQuery(query);
	std::uint64_t v = 0;
	if (auto r = ParseUintParam(qmap, "tail", 0, 100000, v))
		return r;
	out = static_cast<std::size_t>(v);
	return nullptr;
}

// Return a copy of `all` with at most `tail` trailing lines; 0 means all.
std::vector<std::string> SliceTail(const std::vector<std::string> &all, std::size_t tail)
{
	if (tail == 0 || all.size() <= tail)
		return all;
	return std::vector<std::string>(all.begin() + (all.size() - tail), all.end());
}

// For a single-string log, `?tail=N` slices at line boundaries from the END so
// the first line of the response is always whole. tail=0 returns it verbatim.
std::string TailString(const std::string &text, std::size_t tail_lines)
{
	if (tail_lines == 0 || text.empty())
		return text;
	std::size_t pos = text.size();
	std::size_t seen = 0;
	while (pos > 0 && seen < tail_lines) {
		--pos;
		if (text[pos] == '\n')
			++seen;
	}
	// Advance past the leading '\n' so the response does not start blank.
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
	// Additive, locale-independent token for well-known sentinel values ("never" /
	// "not_available"); the English "value" above is kept so old clients keep working.
	// `token`, not `enum`: `enum` is reserved in C++, C#, Java, Rust, PHP and Swift, so
	// a generated client could not name a field it.
	WriteStringOrNull(w, "token", !v.enum_token.empty(), v.enum_token);
	// Optional nested sub-value: percentage of parent, packet count, or
	// all-time total depending on the node, so a client formats it from `type`.
	w.Key("extra");
	if (!v.extra.empty())
		WriteStatsValue(w, v.extra.front());
	else
		w.ValueNull();
	w.EndObject();
}

void WriteStatsNode(CJsonWriter &w, const webapi::StatsTreeNode &n)
{
	w.BeginObject();
	// Stable machine key, when the daemon provides one. OMITTED rather than null when
	// absent: absence means a daemon too old to send it, which REFERENCE.md's
	// unknown-value rule keeps distinct from "there is no key".
	if (!n.key.empty()) {
		w.Key("key");
		w.ValueString(wxString::FromUTF8(n.key.c_str()));
	}
	// Raw machine value (client version / OS string) for data-labelled nodes.
	// `label_value`, not `raw`: for a row whose label is itself data ("v0.70b: %s") this
	// carries the datum. null on a node whose label is not data -- there is no datum,
	// as against the daemon not having sent one.
	WriteStringOrNull(w, "label_value", !n.raw.empty(), n.raw);
	w.Key("label");
	w.ValueString(wxString::FromUTF8(n.label.c_str()));
	w.Key("values");
	w.BeginArray();
	for (const auto &v : n.values)
		WriteStatsValue(w, v);
	w.EndArray();
	// Raw numeric UL:DL ratio (download-per-upload), for the ratio node only. R11 puts
	// the window in the key, the way uploaded_bytes_session / _total do. Each is
	// emitted only when computable, so a legacy daemon yields neither.
	if (n.has_ratio_session || n.has_ratio_total) {
		if (n.has_ratio_session) {
			w.Key("ratio_session");
			w.ValueDouble(n.ratio_session);
		}
		if (n.has_ratio_total) {
			w.Key("ratio_total");
			w.ValueDouble(n.ratio_total);
		}
	}
	w.Key("children");
	w.BeginArray();
	for (const auto &c : n.children)
		WriteStatsNode(w, c);
	w.EndArray();
	w.EndObject();
}

// Render an array of (t, value) points walking backwards from snapshot_at: the
// earliest sample corresponds to `snapshot_at - (samples.size()-1)*interval`.
// `extra_a` / `extra_b` are optional series point-aligned with `samples`, emitted
// under `key_a` / `key_b` beside each `value`. null or short (an amuled predating
// the tag reports neither) leaves the keys off entirely, so a consumer can tell
// "not reported" from "zero".
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
		// One representation, unix seconds, named `_at` like every other time
		// on the surface (R3). Formatting is a client concern.
		w.Key("at");
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

// The results-array element. Fields come from the shared writer so this endpoint
// and the `search_result_added` SSE payload cannot drift; only the braces are ours.
void WriteSearchObject(CJsonWriter &w, const webapi::SearchResult &r)
{
	w.BeginObject();
	webapi::WriteSearchResultFields(w, r);
	w.EndObject();
}

} // namespace

namespace
{
// Reverse of SearchTypeFromString below. EC_SEARCH_LOCAL/GLOBAL/KAD share their
// numeric values with CSearchList's own SearchType, which is what
// EC_TAG_SEARCH_LIFECYCLE_KIND carries on the wire, so a plain uint8 in is enough:
// no separate SearchType include needed here.
wxString SearchKindToString(std::uint8_t kind)
{
	switch (kind) {
	case EC_SEARCH_LOCAL:
		return wxString::FromAscii("local");
	case EC_SEARCH_KAD:
		return wxString::FromAscii("kad");
	case EC_SEARCH_BROWSE:
		// A "View Files" browse of one peer's share. Reported, never accepted
		// by SearchTypeFromString: browses are not started through /search.
		return wxString::FromAscii("browse");
	case EC_SEARCH_GLOBAL:
	default:
		return wxString::FromAscii("global");
	}
}

// Shared by HandleSearchResults' `progress.state` and HandleSearchList's `state`, so
// the two cannot drift. state_val is a raw CSearchList::SearchLifecycleState numeric
// (IDLE=0/RUNNING=1/FINISHED=2); a static_assert in ExternalConn.cpp keeps that
// alignment honest.
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

	// ?max_client_versions=N caps how many per-software version rows the daemon
	// serializes (EC_TAG_STATTREE_CAPPING). 0 is unlimited. Only the version lists are
	// affected, not the OS breakdown or skeleton nodes.
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

	// Lazy-fetch with 1 s TTL coalescing. The fetcher runs the EC roundtrip under
	// m_app's m_ec_mtx (SendRecvSerialized); concurrent burst reads serialize on
	// m_stats_tree_cache's mutex. The cache is unkeyed, so an entry fetched at a
	// different cap counts as a miss.
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
	// No snapshot_at: the ETag is the cache validator. TtlPair_StatsTree still
	// tracks the fetched-at time internally to drive the 1 s TTL coalescer.
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

	// Validate the graph name BEFORE fetching -- saves an EC roundtrip
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

	// ?interval_seconds=N is the seconds between samples, passed through as
	// EC_TAG_STATSGRAPH_SCALE. Rejected rather than clamped: 0 makes the daemon answer
	// EC_OP_FAILED, which would reach the caller as an unexplained empty graph, and
	// SCALE is a uint16 on the wire.
	std::uint32_t interval = 1;
	{
		std::uint64_t v = interval;
		// Named for its unit, and the same on the way in as on the way out:
		// the response has always echoed `interval_seconds`.
		if (auto r = ParseUintParam(qmap, "interval_seconds", 1, 3600, v))
			return *r;
		interval = static_cast<std::uint32_t>(v);
	}

	// Lazy-fetch the full graph bundle (one EC call serves all four named graphs). The
	// cache is unkeyed, so an entry fetched at another interval has to count as a miss
	// -- see CTtlCache's validated overload.
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
		series = &g.download_bytes_per_second;
	} else if (graph == "upload_speed") {
		series = &g.upload_bytes_per_second;
	} else if (graph == "connections") {
		series = &g.connections;
	} else /* kad_nodes */ {
		series = &g.kad_nodes;
	}

	// ?width=N tails the sample count returned. Applied after the fetch, deliberately:
	// the EC request always asks for the full window, so one cached bundle still answers
	// every (graph, width) combination.
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
	// No snapshot_at in the response; WritePointArray still consumes `ts` to anchor
	// per-point timestamps backwards from the fetch wall-clock. The two extra series
	// exist only on the connections graph.
	w.Key("points");
	if (graph == "connections") {
		WritePointArray(w,
			*series,
			ts,
			g.interval_seconds,
			width,
			&g.active_downloads,
			"active_download_count",
			&g.active_uploads,
			"active_upload_count");
	} else {
		WritePointArray(w, *series, ts, g.interval_seconds, width);
	}
	// Session totals tag along -- no separate roundtrip. Divide any of the
	// three by duration_seconds for the session average the desktop plots.
	w.Key("session");
	w.BeginObject();
	// Past tense, like `downloaded_bytes_session` elsewhere: bytes already
	// moved, not a rate. The `session` wrapper scopes them.
	w.Key("downloaded_bytes");
	w.ValueInt(static_cast<int64_t>(g.session_download_bytes));
	w.Key("uploaded_bytes");
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

	// Read straight from the refresher-maintained state: POST /search flips the active
	// flag, RefresherTick polls amuled while active and stores the normalized (kind,
	// percent, complete, active). An id that names no live slot (never started, freed,
	// or evicted from the daemon's ring) is a 404, distinct from a known-but-empty
	// search, which returns an idle envelope.
	if (auto rej = RequireSearch(search_id))
		return *rej;
	// A FINISHED search is not polled by the tick, so its cached results would otherwise
	// be frozen at the moment it completed. Refresh on read, coalesced by a short TTL.
	RefreshSearchIfStale(search_id);
	const std::vector<webapi::SearchResult> results_vec = m_state.Search(search_id);
	const webapi::SearchProgressSnapshot progress = m_state.SearchProgress(search_id);

	// This endpoint keeps its own envelope (`progress` rides alongside
	// `results`), so it cannot call ListResponse, but it shares the helpers.
	ListParams params;
	if (auto err = ParseListParams(QueryOf(req), params))
		return *err;
	static const ListComparators<webapi::SearchResult> kComps = {
		// Identity column: immutable, so a keyset sweep can anchor on it.
		{ "hash", SORT_BY(hash), ANCHOR_ON(hash) },
		{ "name", SORT_BY(name) },
		// R7: spelled like the response keys they order by.
		{ "size_bytes", SORT_BY(size) },
		{ "sources.total", SORT_BY(source_count) },
		{ "rating", SORT_BY(rating) },
		// Browse listings are read folder by folder. Empty on server/Kad hits,
		// so sorting a non-browse search by it is a stable no-op.
		{ "directory", SORT_BY(directory) },
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
	WritePageMeta(w, total, params);
	// Echoed even though the caller put it in the path: clients key tabs on it.
	w.Key("search_id");
	w.ValueInt(static_cast<int64_t>(search_id));
	// What was searched for, so a client that adopted an id need not
	// cross-reference /search to label its tab. For a browse, the peer's name.
	w.Key("query");
	w.ValueString(wxString::FromUTF8(m_state.SearchQuery(search_id).c_str()));
	// Mirrors the `search_progress` SSE event field-for-field. `state` encodes the full
	// lifecycle, so no redundant `active` / `complete` booleans -- consumers derive them
	// and read the same shape polling or streaming.
	w.Key("progress");
	w.BeginObject();
	w.Key("state");
	w.ValueString(SearchLifecycleStateToString(progress.complete ? 2 : progress.active ? 1 : 0));
	// `type`, matching POST /search's body field and the searches[] row (R6).
	w.Key("type");
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
	// Cache miss: before giving up, ask the core once whether it holds this id anyway --
	// a search amulegui, the monolithic GUI or a previous amuleapi run started. Seeding
	// it here is what lets a UI adopt one.
	if (DiscoverSearchIfHeldByCore(search_id))
		return nullptr;
	return std::make_unique<CHttpServer::Response>(ErrorResponse(
		404, "not_found", "no search with that search_id (never started, freed or expired)"));
}

void CApiDispatcher::RefreshSearchIfStale(std::uint32_t search_id)
{
	// ClaimSearchRefresh does the gating: it returns true only for a slot that exists,
	// is not active, and has not been fetched within the TTL -- and it stamps the slot
	// as it hands out the claim, so racing readers cost one roundtrip.
	static constexpr std::chrono::milliseconds kSearchRefreshTtl{ 1000 };
	if (!m_state.ClaimSearchRefresh(search_id, kSearchRefreshTtl))
		return;
	// A failed roundtrip leaves the cached results in place: serving the previous set
	// beats failing a read that has a good answer.
	//
	// Deliberately the per-search FULL fetch and not the union. The union is a
	// differential stream keyed on what the daemon has already sent this connection, so
	// it tolerates exactly one issuer: with the refresher thread issuing it every tick,
	// a second issuer here could have its reply applied out of order and leave a row no
	// later poll can correct. A FULL reply carries the whole search and races nothing.
	(void)webapi::FetchOneSearchFull(m_app, m_state, search_id);
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
		// The list entry carries the daemon's name for the search -- the query
		// string, or for a browse the peer's nickname.
		const CECTag *nameTag = entry.GetTagByName(EC_TAG_SEARCH_NAME);
		// ...and its lifecycle state. A finished search seeded as running is not
		// cosmetic: POST /search/{id}/more rejects a finished search, so it would
		// answer 202 for a request amuled turns into a no-op. 1 = running,
		// 2 = finished (SearchLifecycleStateToString).
		const CECTag *stateTag = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_STATE);
		const std::uint8_t state_val = stateTag ? static_cast<std::uint8_t>(stateTag->GetInt()) : 0;
		// ...and the percent, when the daemon reports one. -1 means it did not
		// (an older daemon); the seed then derives it from the lifecycle state.
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
	if (found) {
		// Seed the slot's results immediately, at EC_DETAIL_FULL. Waiting for the
		// next union poll would leave it permanently empty: the union responder has
		// very likely already offered these results, found no slot, dropped them,
		// and will elide them from here on.
		(void)webapi::FetchOneSearchFull(m_app, m_state, search_id);
	}
	return found;
}

// Enumerates every search the daemon currently holds via EC_OP_SEARCH_LIST, rather
// than reading the Refresher-cached m_state, which only ever knows about searches
// THIS session started with POST /search. That is what makes a search started by
// another client discoverable here (amule-org/amule#641).
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
		// Browse ("View Files") entries carry the browsed peer's ecid -- without
		// it a consumer cannot tell WHOSE share it is listing. Absent otherwise.
		if (const CECTag *clientTag = entry.GetTagByName(EC_TAG_CLIENT)) {
			row.has_client_ecid = true;
			row.client_ecid = static_cast<std::uint32_t>(clientTag->GetInt());
		}
		// When THIS amuleapi started the search. Absent for one this process did
		// not start, because a 0 would read as 1970 rather than "no idea".
		row.started_at = m_state.SearchStartedAt(row.search_id);
		// The same number GET /search/{id}/results reports as `total`. Absent when the
		// daemon is older than the tag, so "does not report" stays distinguishable
		// from "found nothing".
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

	// request.target is the literal URI, e.g. "/api/v0/logs/amule?tail=200".
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
	// Lets a client paging through history know what it missed.
	w.Key("total_lines");
	w.ValueInt(static_cast<int64_t>(all.size()));
	w.Key("returned_lines");
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

	// Drop the in-process mirror. This also bumps the log's clear-generation, which the
	// next refresher tick reads to publish a `resync` for every subscriber. Publishing
	// it here is not open to us: the bus has a single-publisher invariant and this is
	// the HTTP thread.
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

	// Lazy-fetch via TtlCache. EC_OP_GET_SERVERINFO ships one EC_TAG_STRING
	// with the whole accumulated text; amuled rotates it server-side.
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
	// Lets a client decide whether to re-poll with a smaller `?tail=`.
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

	// Lazy cache for /logs/server_info would otherwise return stale
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
// (PrefsSchema.cpp). Shared by the GET handler and the PATCH echo so the response
// shape is defined exactly once.
//
// Categories whose name contains a dot are nested one level under their prefix
// (remote_controls.webserver -> "remote_controls": {"webserver": {...}}). Write-only
// rows (passwords) and Rejected rows are never emitted.
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
		WritePrefCategoryFields(w, top.c_str(), p);
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

// Helpers that pull (& validate) optional fields from a JSON object. Each returns
// true and writes `out` when present and the right shape; on wrong shape it writes
// `err_label` for the caller to relay and returns false.
struct PrefsParseError
{
	bool is_error = false;
	std::string message;
};

// EC_OP_SET_PREFERENCES requires EC_DETAIL_FULL so the daemon honours boolean tags
// (CEC_Prefs_Packet::Apply gates ApplyBoolean on it). FULL is also what amulegui
// sends.

// --- Generic optional-field extractors for the #437 categories -------
//
// Each pulls one optional key from a sub-object into the EC group tag, validating
// its JSON type. Returns true (leaving `group` untouched) when the key is simply
// absent. Booleans always pack as a value tag (uint8 0/1): Apply reads `GetInt()!=0`
// under EC_DETAIL_FULL, so an empty presence tag would read as false. `scale`
// converts the API value to the unit EC carries; `max` is checked before scaling, so
// the error names the number the caller actually wrote.
bool PrefTakeUint(const picojson::object &o,
	CECTag &group,
	const char *key,
	ec_tagname_t name,
	std::uint32_t min,
	std::uint32_t max,
	std::uint32_t step,
	bool &any,
	std::string &err,
	std::uint32_t scale)
{
	const auto it = o.find(key);
	if (it == o.end())
		return true;
	if (!it->second.is<double>()) {
		err = std::string(key) + " must be a non-negative integer";
		return false;
	}
	const double v = it->second.get<double>();
	// Reject a fractional value rather than truncating it at the cast below. The step
	// modulo further down is not a substitute: it runs on the already-truncated value,
	// so it tests the floor's alignment and never integrality, and a field with no step
	// skips it entirely.
	if (!IsIntegralJsonNumber(v)) {
		err = std::string(key) + " must be a non-negative integer";
		return false;
	}
	if (v < 0 || v > static_cast<double>(max) || v < static_cast<double>(min)) {
		// Name the bounds. These domains are narrower than the field's type for
		// reasons a caller cannot infer, so "out of range" alone leaves them guessing.
		err = std::string(key) + " out of range (" + std::to_string(min) + "-" + std::to_string(max) +
		      ")";
		return false;
	}
	// Quantised rows: the core's setter divides, so a value between two steps
	// is truncated on the way in. Rejected rather than silently rounded.
	if (step && (static_cast<std::uint64_t>(v) % step) != 0) {
		err = std::string(key) + " must be a multiple of " + std::to_string(step);
		return false;
	}
	const std::uint64_t scaled = static_cast<std::uint64_t>(v) * (scale ? scale : 1u);
	group.AddTag(CECTag(name, static_cast<std::uint32_t>(scaled)));
	any = true;
	return true;
}

// invert=true stores the opposite of the JSON value in the EC tag, for
// positive-sense API fields whose EC tag is negatively named (today only
// extended_udp_port_enabled -> EC_TAG_CONN_UDP_DISABLE). A schema column, not a
// special case in the caller.
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

// Enum field: the API spells the value out ("socks5", "friends", ...) while EC
// carries the bare ordinal. `names` lists the accepted strings in wire order, so a
// name's index is exactly the value the daemon's Apply() casts back to its enum.
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

// String-array field (directories.shared_paths): a JSON array of strings
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

// Write-only password: hash the plaintext with MD5 (matching how the daemon
// stores WS/amuleapi passwords) and pack the 16-byte digest. Never on GET.
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

	// Body shape mirrors the GET: one optional sub-object per category, every field
	// within optional. Both come from the schema table, so the accepted shape cannot
	// drift from the emitted one. Built at EC_DETAIL_FULL because amuled's Apply()
	// gates ApplyBoolean on it -- which is also why every bool below is a value tag.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_SET_PREFERENCES, EC_DETAIL_FULL));
	bool any_change = false;
	// Names of read-only fields the body carried, so a request naming only
	// those can say which ones rather than claiming none were known.
	std::string skipped_read_only;

	// One CECTag per EC group, created on first use. Two categories can share a group
	// (remote_controls.webserver / .amuleapi), so they must land in the same tag.
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

		// Skip the whole row when the client did not send that category at all.
		const picojson::object *src = nullptr;
		for (std::size_t c = 0; c < webapi::PrefCategoryCount(); ++c) {
			if (std::strcmp(webapi::PrefCategories()[c].name, f.category) == 0) {
				src = cat_obj[c];
				break;
			}
		}
		if (!src || src->find(f.key) == src->end())
			continue;

		// Read-only fields (daemon capabilities, live status) are ignored rather than
		// rejected: the read-modify-write round trip necessarily sends them back.
		// Bespoke fields are applied further down.
		//
		// Remember the names, though: a body naming ONLY non-writable fields would
		// otherwise fall through to "did not include any known pref fields".
		if (f.access == webapi::PrefAccess::ReadOnly || f.access == webapi::PrefAccess::Bespoke) {
			if (f.access == webapi::PrefAccess::ReadOnly) {
				if (!skipped_read_only.empty())
					skipped_read_only += ", ";
				skipped_read_only += std::string(f.category) + "." + f.key;
			}
			continue;
		}

		if (f.access == webapi::PrefAccess::Rejected) {
			// Each Rejected row names the endpoint that owns the field, so
			// the answer is a redirection rather than a flat refusal.
			if (std::strcmp(f.category, "geoip") == 0) {
				return ErrorResponse(400,
					"bad_request",
					"`geoip.update_now` is an action, not a setting: "
					"POST /geoip/update instead");
			}
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
					"option_not_supported",
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
			ok = PrefTakeUint(
				*src, g, f.key, f.tag, f.min, f.max, f.step, any_change, err, f.ec_scale);
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
	// remote_controls.webserver.guest_enabled and .guest_password share a single EC
	// tag: EC_TAG_WEBSERVER_GUEST carries the enable bool as its value and the password
	// hash as a child, so there is no 1:1 field-to-tag mapping for the schema. When
	// only the password is given, the enable bit falls back to the snapshot value.
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
		if (!skipped_read_only.empty()) {
			const std::string msg =
				"no writable fields in the request; " + skipped_read_only +
				(skipped_read_only.find(',') == std::string::npos ? " is read-only"
										  : " are read-only");
			return ErrorResponse(400, "bad_request", msg.c_str());
		}
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

	// Inline refresh so the GET below reflects the post-mutation state.
	(void)RefresherTick(m_app, m_state);

	// Return the updated shape so consumers need no follow-up GET.
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

// Issue a single-shot mutation EC packet (no body), check the response, run
// RefresherTick inline, return `{message?: "..."}` -- the daemon's own account of
// what it did. Used by every connection-control endpoint whose EC op is
// parameterless. Callers pass 202, not 200: the request returns before the effect
// is observable, which is as true of a disconnect as of a connect.
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
	// amuled's CONNECT/DISCONNECT return EC_OP_STRINGS with a status message,
	// surfaced verbatim so consumers see what amuled would have shown.
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
	// `message` is the daemon's own explanation of what it did, not recoverable from
	// any subsequent read. With nothing to say, no body at all rather than `{}`: the
	// URL-fetch triggers beside these answer the same 202 with no body.
	if (message.empty()) {
		r.content_type.clear();
		return r;
	}
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("message");
	w.ValueString(wxString::FromUTF8(message.c_str()));
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

	// Optional `{"network": "ed2k" | "kad" | "both"}` selector -- same shape as
	// /networks/disconnect. Default "both" preserves the parameterless contract.
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

	// Optional `{"network": "ed2k" | "kad" | "both"}` selector; default "both",
	// and an empty body is fine -- that is the v0 contract callers built against.
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
		return SimpleConnControlOp(m_app, m_state, EC_OP_SERVER_DISCONNECT, 202);
	}
	if (network == "kad") {
		return SimpleConnControlOp(m_app, m_state, EC_OP_KAD_STOP, 202);
	}
	// "both": amuled's EC_OP_DISCONNECT short-circuits to both
	// SERVER_DISCONNECT and KAD_STOP in one EC roundtrip.
	return SimpleConnControlOp(m_app, m_state, EC_OP_DISCONNECT, 202);
}

// POST /kad/update -- refresh the Kad node list from a nodes.dat URL (#693).
//
// The EC handler (EC_OP_KAD_UPDATE_FROM_URL) persists the URL into preferences
// itself via SetKadNodesUrl(), so this deliberately does NOT also patch
// kad.update_url -- doing both would diverge from the ed2k path and could race it.
// Side effect worth knowing: once the download completes amuled stops Kad, swaps
// nodes.dat in, and starts Kad again, with no prompt.
CHttpServer::Response CApiDispatcher::HandleKadUpdateFromUrl(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// No inline RefresherTick: nothing observable has changed yet.
	static const UrlFetchSpec kSpec = {
		"url", EC_OP_KAD_UPDATE_FROM_URL, EC_TAG_KADEMLIA_UPDATE_URL, true, false
	};
	std::string url;
	CHttpServer::Response rejection;
	if (!ResolveFetchUrl(req, kSpec, nullptr, url, rejection)) {
		return rejection;
	}
	return UrlFetchOp(m_app, m_state, kSpec, url);
}

// POST /ipfilter/reload -- re-read ipfilter.dat + ipfilter_static.dat from amuled's
// config directory into the live filter. amuled queues a CIPFilterTask and keeps
// the current filter live until the new one has loaded, so this is accepted, never
// completed; the outcome is only ever an amule log line, read back through
// /logs/amule or the SSE log channel.
CHttpServer::Response CApiDispatcher::HandleIpfilterReload(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	return SimpleConnControlOp(m_app, m_state, EC_OP_IPFILTER_RELOAD, 202);
}

// POST /ipfilter/update -- download ipfilter.dat from a URL, swap it in and reload.
//
// Unlike its two siblings the URL is optional: with no body amuleapi resolves
// security.ipfilter_update_url from its own preferences snapshot. With neither,
// this is a 400 rather than a request the core turns into a silent no-op
// (CIPFilter::Update() returns immediately on an empty URL). The snapshot trails
// amuled by up to one tick, so a PATCH immediately followed by a bodyless update
// can still send the previous URL.
CHttpServer::Response CApiDispatcher::HandleIpfilterUpdate(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;
	// EC_OP_IPFILTER_UPDATE reads the packet's first tag by position, not by name, so
	// the tag is EC_TAG_STRING -- what amulegui has always sent. No inline
	// RefresherTick: the download is asynchronous and lands in amuled's filter, not in
	// a cache this process holds.
	static const UrlFetchSpec kSpec = { "url", EC_OP_IPFILTER_UPDATE, EC_TAG_STRING, false, false };
	// Offer the snapshot as the fallback only once there is one -- before the
	// first, the defaults would look like "no URL configured".
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

// POST /geoip/update -- fetch a fresh GeoIP database now.
//
// Unlike the three sibling fetch routes it takes no URL: which database to fetch
// comes from geoip.source and geoip.custom_update_url, which stay ordinary
// preferences, so the body is empty and the whole request is the verb. The core has
// no EC opcode for it -- the trigger is a preferences tag -- so the packet is still
// EC_OP_SET_PREFERENCES carrying that one tag.
CHttpServer::Response CApiDispatcher::HandleGeoipUpdate(const CHttpServer::Request &req)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;
	if (auto rej = RequireAdmin(a))
		return *rej;

	// EC_DETAIL_FULL and a value tag, for the same reason PATCH /preferences uses both:
	// CEC_Prefs_Packet::Apply() only calls ApplyBoolean when the detail level is FULL,
	// and reads the flag from the tag's value.
	auto ec_req = std::make_unique<CECPacket>(EC_OP_SET_PREFERENCES, EC_DETAIL_FULL);
	CECTag group(EC_TAG_PREFS_IP2COUNTRY, static_cast<std::uint32_t>(0));
	group.AddTag(CECTag(EC_TAG_IP2COUNTRY_UPDATE_NOW, static_cast<std::uint8_t>(1)));
	ec_req->AddTag(group);

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		delete ec_resp;
		return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
	}
	delete ec_resp;

	(void)RefresherTick(m_app, m_state);

	// 202 with no body, like the three sibling fetch routes (UrlFetchOp): the download
	// runs in the daemon after the reply. Progress is observable on GET /preferences as
	// geoip.download_in_progress, the outcome as last_update_status.
	CHttpServer::Response r;
	r.status = 202;
	r.content_type.clear();
	return r;
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

	// Body: {"ip": "1.2.3.4", "port": <uint16>}. A dotted quad, and only that. The
	// integer form is gone: ParseIpv4Dotted() packs a.b.c.d least-significant byte
	// first, while the integer branch took the JSON value verbatim, so 2130706433
	// (0x7F000001, the conventional big-endian spelling of 127.0.0.1) bootstrapped
	// 1.0.0.127.
	std::uint32_t ip_he = 0;
	{
		const auto it = obj.find("ip");
		if (it == obj.end()) {
			return ErrorResponse(400, "bad_request", "required field `ip` is missing");
		}
		if (!it->second.is<std::string>()) {
			return ErrorResponse(400,
				"bad_request",
				"`ip` must be a dotted-quad IPv4 address string, e.g. "
				"\"127.0.0.1\"");
		}
		if (!ParseIpv4Dotted(it->second.get<std::string>(), ip_he)) {
			return ErrorResponse(400, "bad_request", "`ip` must be a dotted-quad IPv4 address");
		}
	}
	std::uint16_t port = 0;
	{
		const auto it = obj.find("port");
		if (it == obj.end() || !it->second.is<double>()) {
			return ErrorResponse(400, "bad_request", "required integer field `port` is missing");
		}
		const double v = it->second.get<double>();
		// Same contract as the `port` on POST /friends. 0 used to be accepted
		// here, which no Kad contact can be reached on.
		if (!IsIntegralJsonNumber(v) || v < 1 || v > 65535) {
			return ErrorResponse(400, "bad_request", "`port` must be an integer in 1..65535");
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
	// `ip`/`port` stay as the documented exception to the no-body rule for actions: the
	// echo reports which address the daemon actually parsed, which the caller cannot
	// recover anywhere else.
	w.Key("ip");
	w.ValueString(Uint32toStringIP(ip_he));
	w.Key("port");
	w.ValueInt(static_cast<int64_t>(port));
	w.EndObject();
	FinalizeJsonBody(w, r);
	return r;
}

// `priority` here mirrors the /shared[].priority enum: bare upload levels plus
// "auto". Setting "auto" hands level selection to amuled, which reports it back as
// `priority` plus a true `priority_auto`. The combined "*_auto" strings are
// deliberately NOT accepted as input: a caller cannot pin a computed level.
CHttpServer::Response CApiDispatcher::HandleSharedPatch(
	const CHttpServer::Request &req, const std::string &key)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto rej = RequireAdmin(a))
		return *rej;
	if (auto r = RequireHashPath(key))
		return *r;

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
			return ErrorResponse(400, "bad_request", "`priority` must be a string");
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
	// Same writer GET /shared/{hash} uses: the list row is a narrower object,
	// and answering a mutation with it made PATCH-and-store differ from re-GET.
	WriteSharedDetailObject(w, s_after);
	FinalizeJsonBody(w, r);
	return r;
}

// --- Bulk mutations (issue #358) -------------------------------------
// PATCH/DELETE /downloads and PATCH /shared take a `hashes` array and apply the
// same op to each, reporting per-item outcomes under `results`. Best-effort per
// item -- each hash is an independent EC roundtrip, so a mid-batch failure does not
// abort the rest. One RefresherTick runs after the whole batch. All-ok is 200, a
// mix is 207 Multi-Status, an all-unreachable batch collapses to 503.

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

	// Validate the patch ONCE -- the same op list applies to every hash, so a malformed
	// patch is a 400 for the whole request; per-hash problems surface per item. Fixed
	// order (status, priority, category) keeps the effect deterministic.
	struct PatchOp
	{
		ec_opcode_t op;
		bool has_inner;
		ec_tagname_t inner_name;
		std::uint8_t inner_value;
	};
	std::vector<PatchOp> ops;
	{
		const auto it = obj.find("action");
		if (it != obj.end()) {
			if (!it->second.is<std::string>())
				return ErrorResponse(400,
					"bad_request",
					"`action` must be one of \"pause\", \"resume\" or \"stop\"");
			const std::string &v = it->second.get<std::string>();
			if (v == "pause")
				ops.push_back(
					{ EC_OP_PARTFILE_PAUSE, false, static_cast<ec_tagname_t>(0), 0 });
			else if (v == "resume")
				ops.push_back(
					{ EC_OP_PARTFILE_RESUME, false, static_cast<ec_tagname_t>(0), 0 });
			else if (v == "stop")
				ops.push_back(
					{ EC_OP_PARTFILE_STOP, false, static_cast<ec_tagname_t>(0), 0 });
			else
				return ErrorResponse(400,
					"bad_request",
					"`action` must be one of \"pause\", \"resume\" or \"stop\"");
		}
	}
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>())
				return ErrorResponse(400, "bad_request", "`priority` must be a string");
			std::uint8_t code = 0;
			if (!FilePriorityToCode(it->second.get<std::string>(), kPrioDownload, code))
				return ErrorResponse(
					400, "bad_request", FilePriorityAccepted(kPrioDownload).c_str());
			ops.push_back({ EC_OP_PARTFILE_PRIO_SET, true, EC_TAG_PARTFILE_PRIO, code });
		}
	}
	{
		const auto it = obj.find("category_index");
		if (it != obj.end()) {
			if (!it->second.is<double>())
				return ErrorResponse(400,
					"bad_request",
					"`category_index` must be a non-negative integer");
			const double v = it->second.get<double>();
			// The integrality test the message already promises: without it
			// 2.9 was accepted and truncated to 2.
			if (v < 0 || v > 255 || !IsIntegralJsonNumber(v))
				return ErrorResponse(
					400, "bad_request", "`category_index` must be in [0, 255]");
			ops.push_back({ EC_OP_PARTFILE_SET_CAT,
				true,
				EC_TAG_PARTFILE_CAT,
				static_cast<std::uint8_t>(v) });
		}
	}
	if (ops.empty())
		return ErrorResponse(400,
			"bad_request",
			"request body must include at least one of `action`, `priority`, or "
			"`category_index`");

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
		// removable here (use POST /downloads_clear_completed).
		if (d.download.status == "completed") {
			results.push_back(BulkErr(raw,
				409,
				"download_completed",
				"DELETE only removes active downloads; use POST "
				"/downloads_clear_completed to clear a completed entry"));
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
		return ErrorResponse(400, "bad_request", "`priority` must be a string");
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

	if (auto r = RequireHashPath(key))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot s;
	if (!FindSharedByKey(m_state, key, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}

	// Partfiles have no verify implementation: the hashing task bails out on
	// IsPartFile(), and amuled's EC handler answers NOOP either way, so a caller would
	// be told the re-hash was accepted and never see a report. A completed but
	// still-listed download is a knownfile, and so a legitimate target.
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

	// 202, not 200: amuled queues a CVerifyLocalDataTask and answers NOOP immediately,
	// so the re-hash is still in flight. The verdict is only ever an amule log line
	// (CVerifyLocalDataTask::PrintReport), read back through /logs/amule or the SSE log
	// channel. No RefresherTick, and no body.
	CHttpServer::Response r;
	r.status = 202;
	r.content_type.clear();
	return r;
}
namespace
{

// The set of directories this endpoint is willing to serve bytes out of.
//
// Read off the preferences snapshot rather than fetched with an
// EC_OP_GET_SHARED_DIRS roundtrip. GET_SHARED_DIRS serialises only the two *intent*
// lists (ExternalConn.cpp), which on a default install are both empty -- Incoming
// is shared implicitly and appears in neither, so containment against that list
// alone would 404 every file in the one directory aMule always shares.
// `directories.shared` is the runtime union the core keeps (explicit + expanded
// recursive) and `incoming` is the implicit root it omits.
//
// The category paths are the third root and are just as implicit as Incoming:
// CSharedFileList::Reload seeds its scan list with GetIncomingDir() and GetCatPath(i)
// for every category BEFORE the explicit shares, so a download that completed into a
// category's own directory is shared by the core and listed by /shared while
// appearing in neither `shared_paths` nor `incoming_path`.
//
// The one thing the core auto-shares that is deliberately NOT a root here is the
// part-file set: those live in the temp directory and are only reachable through
// this handler as a partfile, which it has already refused with 409.
std::vector<std::string> ShareRootsFromPrefs(
	const webapi::PreferencesSnapshot &p, const std::vector<webapi::CategorySnapshot> &cats)
{
	std::vector<std::string> roots;
	roots.reserve(p.directories.shared_paths.size() + cats.size() + 1);
	for (const std::string &d : p.directories.shared_paths) {
		if (!d.empty())
			roots.push_back(d);
	}
	if (!p.directories.incoming_path.empty()) {
		roots.push_back(p.directories.incoming_path);
	}
	// Empty paths are skipped rather than defaulted: amuled holds an empty path for a
	// category that saves to Incoming, which is already a root above. An empty string
	// here would be a root that contains everything.
	for (const webapi::CategorySnapshot &c : cats) {
		if (!c.path.empty())
			roots.push_back(c.path);
	}
	return roots;
}

} // namespace

// The bytes of one completed shared file, streamed off disk. GET / HEAD only, and
// the only route in this file whose response body is not materialised in memory: it
// hands the transport a path plus a byte window (CHttpServer::Response::file) and
// the 64 KiB streaming body does the rest.
CHttpServer::Response CApiDispatcher::HandleSharedContent(
	const CHttpServer::Request &req, const std::string &key)
{
	// There is no auth middleware in this codebase: every route gates itself, so a
	// content route that forgot this line would publish the whole share. Authenticate
	// but NOT RequireAdmin -- the guest role can already list it.
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto r = RequireHashPath(key))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot s;
	if (!FindSharedByKey(m_state, key, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}

	// Completed files only, and IsIncompletePartfile() is exactly that test. A
	// partfile's bytes on disk are a gapped .part file whose offsets do not correspond
	// to the file's own, so a range out of it would be silently wrong.
	if (s.IsIncompletePartfile()) {
		return ErrorResponse(
			409, "partfile_unsupported", "content download is not supported on a partfile");
	}

	// The directory rides EC_TAG_KNOWNFILE_PATH, which amuled emits only outside
	// EC_DETAIL_UPDATE and, being on the valuemap path, only on the frames where it
	// changed. A snapshot taken before the first such frame therefore has the file but
	// not its location. That resolves on the next full frame, which is what makes this
	// a 503-with-Retry-After rather than a 404.
	if (s.on_disk_dir.empty()) {
		CHttpServer::Response r =
			ErrorResponse(503, "path_unavailable", "the file's on-disk path is not known yet");
		r.headers["Retry-After"] = "5";
		return r;
	}

	// Resolution and the containment check are the HANDLER's job -- the transport opens
	// whatever path it is given. Every rejection below collapses into one 404 with the
	// same message the unknown-hash branch used, so the reply cannot be used to probe
	// the share layout or where the boundary sits.
	const std::vector<std::string> roots =
		ShareRootsFromPrefs(m_state.Preferences(), m_state.Categories());

	std::string fs_path;
	if (!webapi::ResolveSharedContentPath(roots, s.on_disk_dir, s.name, fs_path)) {
		// One case inside that failure is not the client's fault and must not be
		// reported as a missing hash: amuleapi is not guaranteed to share a filesystem
		// with amuled. The EC endpoint is configurable and amuleapi has no equivalent
		// of the remote GUI's path-mapping layer, so a remote deployment resolves the
		// daemon's paths against the wrong filesystem. Distinguishing it costs one stat.
		std::string joined;
		struct stat probe
		{
		};
		if (webapi::JoinSharedPath(s.on_disk_dir, s.name, joined) &&
			::stat(joined.c_str(), &probe) != 0) {
			return ErrorResponse(503,
				"ec_content_unreachable",
				"the file is not present on the filesystem running amuleapi");
		}
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}

	// Re-stat the RESOLVED path. ResolveSharedContentPath does not hand back its stat,
	// and the window, the Content-Length and the validator all have to come from one
	// observation of one path.
	struct stat st
	{
	};
	if (::stat(fs_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
		return ErrorResponse(503,
			"ec_content_unreachable",
			"the file is not present on the filesystem running amuleapi");
	}

	// The other half of the same remote-EC hazard, and the more dangerous half: the
	// path resolved and something regular is sitting there, but it is not the file the
	// hash names. On a split deployment two unrelated files can agree on a name, and
	// serving the local one under the remote one's hash would hand the caller bytes it
	// did not ask for. A knownfile's size is fixed at hash time, so a disagreement is
	// never benign. Same 503.
	if (static_cast<std::uint64_t>(st.st_size) != s.size) {
		return ErrorResponse(
			503, "ec_content_mismatch", "the file on disk does not match the shared file's size");
	}

	const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);

	CHttpServer::Response r;
	// Hard-coded, never derived from the extension -- StaticContentType is deliberately
	// NOT reused here. Completed downloads land in Incoming, so both the bytes and the
	// filename came from strangers on the ed2k network, and amuleapi serves the Web UI
	// from this same origin. octet-stream + attachment + nosniff + a sandbox CSP is
	// four reasons a browser will not run it.
	r.content_type = "application/octet-stream";
	r.headers["Content-Disposition"] = webapi::BuildContentDisposition(s.name);
	r.headers["X-Content-Type-Options"] = "nosniff";
	// Scoped to this response only: a global CSP would change every route
	// including the Web UI, which is a separate decision.
	r.headers["Content-Security-Policy"] = "default-src 'none'; sandbox";
	// Set by the handler so Dispatch stands aside instead of MD5-ing the body to derive
	// a validator -- there is no body here to hash, and hashing a multi-GB file per
	// request is not a slow path but an unusable one.
	const std::string content_etag = webapi::BuildContentEtag(
		static_cast<std::uint64_t>(st.st_mtime), static_cast<std::uint64_t>(st.st_size));
	r.headers["ETag"] = content_etag;

	// Conditional GET, answered HERE and not by Dispatch. Taking the handler-set-ETag
	// escape above also takes on this obligation: the whole If-None-Match block in
	// Dispatch sits inside ShouldStampEtag, which returns false the moment a handler
	// owns the validator, so a route that sets its own ETag and does not do this hands
	// out a validator no client can revalidate.
	//
	// Through the shared matcher rather than a string compare, because the header may
	// be `*`, a comma-separated list, or a weak `W/"..."` form -- which is what an nginx
	// in front of us emits. IfNoneMatchHits wants the BARE validator, so the quotes
	// BuildContentEtag adds come off for the comparison.
	//
	// Evaluated BEFORE the Range header, per RFC 9110 13.2.2: a matching precondition
	// wins outright, so a conditional request carrying a Range answers 304, never 206.
	const std::string inm_val = FindHeaderCaseInsensitive(req.headers, "If-None-Match");
	const std::string content_etag_bare =
		(content_etag.size() >= 2 && content_etag.front() == '"' && content_etag.back() == '"')
			? content_etag.substr(1, content_etag.size() - 2)
			: content_etag;
	if (webcommon::IfNoneMatchHits(inm_val, content_etag_bare)) {
		CHttpServer::Response nm;
		nm.status = 304;
		// A 304 carries no content, so no content_type (the default is application/json,
		// which would be a lie here), no body, no Response::file, and only the validator
		// RFC 7232 4.1 requires.
		nm.content_type.clear();
		nm.headers["ETag"] = content_etag;
		return nm;
	}

	std::uint64_t first = 0;
	std::uint64_t last = 0;
	std::string range_hdr = FindHeaderCaseInsensitive(req.headers, "Range");

	// If-Range, RFC 9110 13.1.5. Only meaningful next to a Range, so the lookup is
	// skipped entirely when there is none.
	//
	// A failed precondition DROPS the Range rather than rejecting the request: the
	// caller asked for a window of a representation it no longer holds, and the whole
	// current one leaves it with a correct file. Dropping it here also keeps a stale
	// validator from turning into a 416 about the OLD file's length.
	//
	// The comparison is NOT IfNoneMatchHits: 13.1.5 requires the strong form and that
	// function deliberately matches the weak one -- see SharedContent.h.
	if (!range_hdr.empty()) {
		const std::string ifr_val = FindHeaderCaseInsensitive(req.headers, "If-Range");
		if (!webapi::IfRangeAllowsRange(ifr_val, content_etag)) {
			range_hdr.clear();
		}
	}

	const webapi::RangeResult rr = webapi::ParseSingleByteRange(range_hdr, file_size, first, last);

	if (rr == webapi::RangeResult::kUnsatisfiable) {
		// 416 carries the error envelope rather than a file window, so it goes down the
		// ordinary buffered path. Content-Range in the unsatisfied form is what RFC 9110
		// 14.4 requires so the client can re-ask.
		CHttpServer::Response err = ErrorResponse(
			416, "range_not_satisfiable", "the requested range lies outside the file");
		err.headers["Content-Range"] = "bytes */" + std::to_string(file_size);
		err.headers["Accept-Ranges"] = "bytes";
		return err;
	}

	// A zero-length file has no valid byte window, so there is nothing for
	// Response::file to describe -- the transport rejects [0, 0] on an empty file
	// rather than clamping it. An empty buffered body is the honest 200 for it.
	if (file_size == 0) {
		r.status = 200;
		r.headers["Accept-Ranges"] = "bytes";
		return r;
	}

	if (rr == webapi::RangeResult::kOk) {
		r.status = 206;
		r.headers["Content-Range"] = "bytes " + std::to_string(first) + "-" + std::to_string(last) +
					     "/" + std::to_string(file_size);
	} else {
		// kAbsent (no header) and kIgnore (unsupported, malformed, or a multi-range
		// set) both answer 200 with the whole file. kIgnore is RFC 7233 3.1's explicit
		// permission being used as the CVE-2011-3192 mitigation -- see SharedContent.h.
		r.status = 200;
		first = 0;
		last = file_size - 1;
	}
	r.headers["Accept-Ranges"] = "bytes";

	CHttpServer::Response::FileSource fs;
	fs.fs_path = fs_path;
	fs.first = first;
	fs.last = last;
	r.file = fs;
	// HEAD needs the window too: the transport runs the serializer in split mode, so it
	// never reads a byte, but Content-Length still comes from RangeFileBody::size and
	// reports what the equivalent GET would send.
	return r;
}

namespace
{
struct SharedDirEntry
{
	wxString path;
	bool recursive = false;
};

// The core's shared-directory op is a whole-list replace, so adding or removing a
// single root is a read-modify-write. Serialise those here: SendRecvSerialized locks
// per roundtrip, not across the pair, so two concurrent single-entry calls would
// otherwise read the same list and the second SET would drop the first's change.
// Nothing can make this atomic against a simultaneous amuleGUI edit -- the protocol
// has no compare-and-set -- so that stays last-write-wins.
std::mutex s_sharedDirsMutex;

// Returns false and fills `err` when EC is unreachable or refuses.
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
// Replace the core's roots with `dirs`. The core applies every path that
// validates and reports the rest, so one bad entry does not discard the edit.
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

	// One entry per submitted path, in the envelope every other multi-item mutation
	// uses, so a caller can tell an applied path from one the response simply did not
	// mention. Reasons are rendered here rather than shipped as text from a core whose
	// locale is not the caller's.
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

// The core's configured share roots: the explicit ones and the recursive ones, each
// with the flag that says which. This is the *intent*, not the expansion -- a
// recursive root is one entry here however many subdirectories it covers.
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
// because that is exactly what the core's EC op does. The core validates each path
// (a REST client cannot stat the core's filesystem), applies the ones that pass and
// reports the rest.
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

// Add one root, leaving the others alone. Idempotent: re-adding a configured path
// just updates its recursive flag, which is friendlier to scripts than a conflict.
// Read-modify-write, so it runs under s_sharedDirsMutex.
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

// Remove one root. The path arrives as a query parameter rather than a path segment
// because it is an absolute filesystem path. Unknown paths are a 404 so a typo is
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

// Send EC_OP_REFRESH_MEDIA_METADATA and turn the reply into a response. `hashTag`
// is null for the whole-share form. The op is deliberately not behind a capability
// tag, so a daemon that predates it answers EC_OP_FAILED rather than being
// detectable in advance.
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
			// 503, matching every other ec_unsupported site in this file and the rule
			// stated in App.cpp. 501 is arguably the better literal answer for "server
			// does not implement it", but one endpoint disagreeing with seven is worse.
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

	// 202, not 200: amuled queues the probes on its media-probe worker and answers
	// immediately, so nothing has been re-extracted yet. `queued` is how many files were
	// accepted for probing; files the scheduler dropped (not audio/video, incomplete,
	// missing on disk) are not counted.
	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("scope");
	w.ValueString(wxString::FromAscii(what));
	w.Key("queued_file_count");
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

	if (auto r = RequireHashPath(key))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	webapi::FileSnapshot s;
	if (!FindSharedByKey(m_state, key, s)) {
		return ErrorResponse(404, "not_found", "no shared file with that hash");
	}
	// Same exclusion the daemon's Refresh mode applies: an in-progress download
	// has no complete file to read. Rejected here so the caller is told why.
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
	// EC_OP_SHAREDFILES_RELOAD: amuled schedules a re-walk of every configured share
	// root and answers immediately, so 202 is literal. The walk starts on amuled's next
	// Process() tick and repeated calls while one is pending coalesce into a single
	// walk.
	//
	// This used to be synchronous on amuled's side, on the assumption that the walk
	// "completes in well under a second" -- exactly the assumption that fails on a large
	// or network-mounted share, and because our EC lane is one serialised worker, the
	// blocked roundtrip held the in-flight slot, filled the queue and turned unrelated
	// endpoints into 503s.
	//
	// Completion is observable through the amule log and the shared_added /
	// shared_removed events, not through this response.
	return SimpleConnControlOp(m_app, m_state, EC_OP_SHAREDFILES_RELOAD, 202);
}

namespace
{

// Parse a uint8 index from a URL capture. Categories are 0..255 (the EC tag
// stores them as uint8). Returns false on overflow, negative or non-digits.
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

// Build the CEC_Category_Tag-shaped tag amuled expects: parent EC_TAG_CATEGORY with
// the index as the int payload, and children EC_TAG_CATEGORY_TITLE ("name" in our
// API), _PATH ("path"), _COMMENT ("comment"), _COLOR (uint32), _PRIO (uint8). For
// CREATE the index is 0xFFFFFFFF (amuled assigns the next free slot); for UPDATE the
// actual index; for DELETE just (EC_TAG_CATEGORY, index).
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

// Extract optional name/path/comment/color/priority from a JSON object. The
// `is_create` flag enables required-field enforcement: CREATE needs a name,
// UPDATE/PATCH treat all as optional.
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
	// `save_path` on the write side too (R9/R6 with the read key).
	auto r2 = get_string("save_path", out.path, out.has_path);
	if (r2.status >= 400)
		return r2;
	auto r3 = get_string("comment", out.comment, out.has_comment);
	if (r3.status >= 400)
		return r3;
	{
		const auto it = obj.find("color");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(
					400, "bad_request", "`color` must be a \"#rrggbb\" string");
			}
			const std::string v = it->second.get<std::string>();
			const bool well_formed = v.size() == 7 && v[0] == '#' &&
						 std::all_of(v.begin() + 1, v.end(), [](unsigned char ch) {
							 return std::isxdigit(ch) != 0;
						 });
			if (!well_formed) {
				return ErrorResponse(
					400, "bad_request", "`color` must be a \"#rrggbb\" string");
			}
			const auto hex = [&v](std::size_t i) {
				return static_cast<std::uint32_t>(std::stoul(v.substr(i, 2), nullptr, 16));
			};
			// Repack into the core's 0x00BBGGRR layout, red in the low byte.
			out.color = hex(1) | (hex(3) << 8) | (hex(5) << 16);
			out.has_color = true;
		}
	}
	{
		const auto it = obj.find("priority");
		if (it != obj.end()) {
			if (!it->second.is<std::string>()) {
				return ErrorResponse(400, "bad_request", "`priority` must be a string");
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

	// CREATE: index sentinel is 0xFFFFFFFF -- amuled assigns the next
	// free slot and returns NOOP on success.
	std::unique_ptr<CECPacket> ec_req(new CECPacket(EC_OP_CREATE_CATEGORY));
	ec_req->AddTag(BuildCategoryTag(0xFFFFFFFFu, f.name, f.path, f.comment, f.color, f.prio));

	const CECPacket *ec_resp = m_app.SendRecvSerialized(ec_req.get());
	if (!ec_resp) {
		return ErrorResponse(503, "ec_unavailable", "EC roundtrip failed for CREATE_CATEGORY");
	}
	std::string ec_err_msg;
	if (IsEcFailedResponse(ec_resp, ec_err_msg)) {
		// A kept-path reply means the category exists; only the path was refused, and
		// `save_path` on the follow-up read is the truthful answer. Anything else is a
		// real failure.
		std::string kept_path;
		const bool kept = EcCategoryPathKept(ec_resp, kept_path);
		delete ec_resp;
		if (!kept) {
			return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
		}
	} else {
		delete ec_resp;
	}

	// Tick so the new category is in the snapshot the caller's follow-up GET
	// reads, even though this response does not carry it.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	// 202 with no body: amuled's EC op does not return the index it assigned, so naming
	// the new category here meant scanning the snapshot for a matching name -- a guess
	// that can silently answer the wrong shape.
	r.status = 202;
	r.content_type.clear();
	return r;
}

// GET /categories/{index}. GUEST, matching the collection read.
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

	// Find the existing category -- CEC_Category_Tag is not delta-friendly, so
	// we always send the full tag and need current values for unset fields.
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
		// As on create: a kept-path reply is a partial success. Everything else
		// in this request landed, and the echoed object reports the kept path.
		std::string kept_path;
		const bool kept = EcCategoryPathKept(ec_resp, kept_path);
		delete ec_resp;
		if (!kept) {
			return ErrorResponse(400, "amuled_rejected", ec_err_msg.c_str());
		}
	} else {
		delete ec_resp;
	}

	(void)RefresherTick(m_app, m_state);

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
	// Index 0 is the implicit "All" category -- amuled treats deleting
	// it as illegal. Reject before the EC roundtrip.
	if (idx == 0) {
		return ErrorResponse(400, "bad_request", "cannot delete the default (index=0) category");
	}
	webapi::CategorySnapshot existing;
	if (!FindCategoryByIndex(m_state, idx, existing)) {
		return ErrorResponse(404, "not_found", "no category with that index");
	}

	// CEC_Category_Tag CMD-detail shape: just `(EC_TAG_CATEGORY, idx)`, no
	// children, replicating amule-remote-gui.cpp's CEC_Category_Tag(cat, EC_DETAIL_CMD).
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

	// amuled renumbers every download's category on delete (files at the deleted index
	// reset to 0, files above it shift down by one), but it mutates m_category directly
	// in CPartFile::RemoveCategory without flagging the partfile dirty, so the change is
	// never echoed back over the incremental EC feed (EC_DETAIL_INC_UPDATE) that
	// RefresherTick consumes. Mirror the renumber into our cached snapshot ourselves --
	// exactly as amulegui does in CDownQueueRem::ResetCatParts -- otherwise downloads
	// keep the stale index and the next-created category silently re-adopts them.
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
	// 204 with no body -- see the mutation-response rule in REFERENCE.md.
	r.status = 204;
	r.content_type.clear();
	return r;
}

namespace
{

// Map wire-string search types to amule's EC_SEARCH_TYPE enum. "local" /
// "global" / "kad" matches amulegui's UI labels.
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

// Shared by /clients/{ecid}/shared_files and /friends/{ecid}/shared_files. Same
// opcode and reply shape either way; only the sub-tag differs, which is what tells
// the daemon whether the id names a live peer or a friend record. The friend form is
// the more capable: a friend carries a stored ip:port, so the daemon can browse one
// that is not currently connected.
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

	// Ask amuled to browse this peer's shared file list. In multi-search mode (amuleapi
	// always is) the daemon allocates a browse search_id, echoes it in the reply, and
	// files the listing under it -- so results, progress and SSE all address the browse
	// exactly like a search.
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

	// A browse's "query" is the peer whose share is being listed -- that is what the
	// daemon names the search, and what GET /search reports for it.
	//
	// Which collection to take the nickname from depends on how the browse was
	// addressed. CECID hands out one global counter, so a CFriend's ECID never collides
	// with a client's -- but it never *matches* one either, and searching the client
	// list for it silently found nothing.
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

	// A creation answers with the created resource and a Location, because here the
	// daemon really does hand one back: SEARCH_START returns EC_TAG_SEARCH_ID. The row
	// is the same shape GET /search lists, written through the same writer, so a client
	// can drop it straight into the collection it keeps.
	SearchListRow row;
	row.search_id = search_id;
	row.query = wxString::FromUTF8(peer_name.c_str());
	row.kind = "browse";
	row.state = "running";
	row.started_at = m_state.SearchStartedAt(search_id);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	r.headers["Location"] = "/api/v0/search/" + std::to_string(search_id);
	CJsonWriter w;
	WriteSearchListRow(w, row);
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

	// Body: { "query": required string, "type": "local"|"global"|"kad" (default
	// "global"), "file_type": optional label, "extension": optional (e.g. "mkv"),
	// "min_size_bytes"/"max_size_bytes": optional uint64 (0 = no cap),
	// "min_source_count": optional uint32 }
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
		const auto it = obj.find("min_size_bytes");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400,
					"bad_request",
					"`min_size_bytes` must be a non-negative integer (bytes)");
			}
			const double v = it->second.get<double>();
			if (!IsIntegralJsonNumber(v) || v < 0)
				return ErrorResponse(400,
					"bad_request",
					"`min_size_bytes` must be a non-negative integer");
			min_size = static_cast<std::uint64_t>(v);
		}
	}
	{
		const auto it = obj.find("max_size_bytes");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400,
					"bad_request",
					"`max_size_bytes` must be a non-negative integer (bytes; 0 = no "
					"cap)");
			}
			const double v = it->second.get<double>();
			if (!IsIntegralJsonNumber(v) || v < 0)
				return ErrorResponse(400,
					"bad_request",
					"`max_size_bytes` must be a non-negative integer");
			max_size = static_cast<std::uint64_t>(v);
		}
	}
	{
		const auto it = obj.find("min_source_count");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400,
					"bad_request",
					"`min_source_count` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			if (!IsIntegralJsonNumber(v)) {
				return ErrorResponse(400,
					"bad_request",
					"`min_source_count` must be a non-negative integer");
			}
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request", "`min_source_count` out of range");
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
	// The daemon (in multi-search mode) allocates a globally-unique search_id and echoes
	// it in the START reply; every subsequent results/stop/more call is addressed by it,
	// so a reply without one leaves the caller nothing to address.
	std::uint32_t search_id = 0;
	if (const CECTag *t = ec_resp->GetTagByName(EC_TAG_SEARCH_ID)) {
		search_id = static_cast<std::uint32_t>(t->GetInt());
	}
	delete ec_resp;
	if (search_id == 0) {
		return ErrorResponse(
			502, "amuled_rejected", "daemon did not return a search_id for SEARCH_START");
	}

	// Seed this search's slot: the refresher polls EC_OP_SEARCH_RESULTS + _PROGRESS for
	// it each tick until the daemon reports completion. This is the single fetcher, so
	// SSE search_result_added / search_progress fire on the same delta a polling
	// consumer would observe.
	m_state.MarkSearchStarted(search_id, search_kind, query);

	// Same creation shape as the browse handler above: the daemon hands back
	// EC_TAG_SEARCH_ID, so the response carries the resource and a Location.
	SearchListRow row;
	row.search_id = search_id;
	row.query = wxString::FromUTF8(query.c_str());
	row.kind = search_kind;
	row.state = "running";
	row.started_at = m_state.SearchStartedAt(search_id);

	CHttpServer::Response r;
	r.status = 202;
	r.content_type = "application/json";
	r.headers["Location"] = "/api/v0/search/" + std::to_string(search_id);
	CJsonWriter w;
	WriteSearchListRow(w, row);
	FinalizeJsonBody(w, r);
	return r;
}

// The three per-search actions share one EC exchange: address the search by
// EC_TAG_SEARCH_ID, send, and turn a failure reply into a 400. Only the opcode, the
// optional close flag and the success shape differ.
CHttpServer::Response CApiDispatcher::SendSearchOp(
	ec_opcode_t opcode, std::uint32_t search_id, bool close, int success_status, int *out_more_reaskable)
{
	std::unique_ptr<CECPacket> ec_req(new CECPacket(opcode));
	// Always addressed. A concrete id is the only way to stop the right search
	// when several are running.
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
	// No body on any of the three: a 204 must not carry one per RFC 9110, and a
	// default-constructed Response does not necessarily start empty.
	r.content_type.clear();
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

	// Stop only: the results stay readable -- amuled keeps them until the search is
	// closed or evicted -- so a consumer sees the same set it was looking at. 204,
	// matching DELETE /search/{id}.
	return SendSearchOp(EC_OP_SEARCH_STOP, search_id, /*close=*/false, 204);
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
	// Drop the local slot too, so its polling stops and a later GET /search/{id}/results
	// is a 404. The vanished slot is also what makes the next diff pass publish
	// `search_closed` to SSE subscribers.
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

	// The desktop "More" button re-asks already-queried Kad peers for a wider result
	// frontier. Both constraints below mirror what that button does rather than what the
	// core tolerates: CSearchManager::RequestMoreResults returns false for a non-Kad id,
	// and the GUI greys the button out once the search ends.
	const webapi::SearchProgressSnapshot progress = m_state.SearchProgress(search_id);
	if (progress.kind != "kad") {
		return ErrorResponse(400, "bad_request", "`more` applies to Kad searches only");
	}
	if (progress.complete || !progress.active) {
		return ErrorResponse(
			400, "bad_request", "`more` applies to a running search; this one has finished");
	}

	// The daemon logs what actually happened and answers with the other half: whether a
	// LATER press could still widen this search. False is terminal -- the reask budget
	// of 4 is spent, or the search is inside the stopping window Kad enters 20 s before
	// a keyword search ends. A press with no responded peer left to reask *yet* still
	// gets 202: it clears as soon as another peer answers.
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

	const std::string needle = LowerHexKey(hash);

	CMD4Hash file_hash;
	if (!HashFromHex(needle, file_hash)) {
		// RequireHashPath owns the message; it is called only here, where it is
		// certain to return a response, so the hash is not parsed twice.
		return *RequireHashPath(needle);
	}

	// Optional body: {"category_index": uint8, "ecid": uint32}. Defaults to category 0
	// when none is supplied, matching amulegui's CDownQueueRem::AddSearchToDownload.
	// `ecid` selects one same-hash/different-name grouped child (from a result's
	// `children[].ecid`) so it downloads under that filename; omitted means the parent.
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
		const auto it = obj.find("category_index");
		if (it != obj.end()) {
			if (!it->second.is<double>()) {
				return ErrorResponse(400,
					"bad_request",
					"`category_index` must be a non-negative integer");
			}
			const double v = it->second.get<double>();
			// The integrality test the message already promises: without it
			// 2.9 was accepted and truncated to 2.
			if (v < 0 || v > 255 || !IsIntegralJsonNumber(v)) {
				return ErrorResponse(
					400, "bad_request", "`category_index` must be in [0, 255]");
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
			if (!IsIntegralJsonNumber(v)) {
				return ErrorResponse(
					400, "bad_request", "`ecid` must be a non-negative integer");
			}
			if (v < 0 || v > 4294967295.0) {
				return ErrorResponse(400, "bad_request", "`ecid` out of range");
			}
			ecid = static_cast<std::uint32_t>(v);
			has_ecid = true;
		}
	}

	// amuled accepts the result hash as the partfile-tag's int payload and looks it up
	// in its searchlist, returning FAILED when absent. An `ecid` selector rides as an
	// EC_TAG_SEARCHFILE child to pick a specific grouped result.
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

	// Inline refresh so /downloads sees the new partfile, subject to amuled's
	// async allocate-and-hash -- it surfaces within 1-2 ticks.
	(void)RefresherTick(m_app, m_state);

	CHttpServer::Response r;
	// 202 with no body: `hash` came from the request and `category_index` is the
	// value applied, recoverable from the download itself.
	r.status = 202;
	r.content_type.clear();
	return r;
}

// GET /search/results/{hash}/comments -- community ratings/comments for one
// search result: the Kad notes retrieved so far plus the running flag.
CHttpServer::Response CApiDispatcher::HandleSearchComments(
	const CHttpServer::Request &req, const std::string &hash)
{
	auto a = Authenticate(req);
	if (!a.ok)
		return a.rejection;

	if (auto r = RequireHashPath(hash))
		return *r;

	if (auto r = RequireSnapshot(m_state))
		return *r;

	const std::string needle = LowerHexKey(hash);

	// Locate the result carrying this hash across ALL open searches -- the comments
	// endpoints are search-agnostic. Grouped children share the parent's hash, so the
	// parent, which owns any fetched notes, matches first.
	webapi::SearchResult hit;
	std::uint32_t owner_search_id = 0;
	if (!m_state.FindSearchResultByHash(needle, hit, &owner_search_id)) {
		return ErrorResponse(404, "not_found", "no search result with that hash");
	}
	// This is THE polling path for a Kad notes lookup: the notes only reach amuleapi
	// through the owning search's result fetch, and a finished search is never fetched
	// by the tick. Without this refresh a lookup started after the search completed
	// would leave `kad_comment_lookup_running` stuck.
	RefreshSearchIfStale(owner_search_id);
	// A refresh can drop the hit -- the daemon frees a search's results when it is
	// closed, and the set is rebuilt wholesale rather than merged. The bool was
	// discarded here, so that case answered 200 from the pre-refresh copy.
	if (!m_state.FindSearchResultByHash(needle, hit, nullptr)) {
		return ErrorResponse(404, "not_found", "no search result with that hash");
	}

	CHttpServer::Response r;
	r.status = 200;
	r.content_type = "application/json";
	CJsonWriter w;
	w.BeginObject();
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(hit.comments.size()));
	w.Key("kad_comment_lookup_running");
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

// POST /search/results/{hash}/comments -- trigger an on-demand Kad NOTES lookup for
// a search result the user has not downloaded. Asynchronous on amuled (up to ~45s);
// retrieved notes then appear via GET here and on /search/results.
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
		// RequireHashPath owns the message; it is called only here, where it is
		// certain to return a response, so the hash is not parsed twice.
		return *RequireHashPath(needle);
	}

	// Must be a live search result in some open search. The daemon runs one Kad NOTES
	// lookup per hash and fans the notes out to every same-hash result, so the specific
	// search does not matter here.
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

	// Refresh AFTER the lookup has been started, for the same reason the GET refreshes
	// at all: a finished search is otherwise frozen, and the flag this POST turns on
	// would never be observed turning off again. Order matters -- refreshing first
	// cached a pre-lookup snapshot and spent the one-second ClaimSearchRefresh token on
	// it, so a GET issued straight after reported `kad_comment_lookup_running: false`
	// for a lookup just begun.
	RefreshSearchIfStale(owner_search_id);

	CHttpServer::Response r;
	// 202 with no body: the field it used to carry could hold exactly one value,
	// so it said nothing the status code had not already said.
	r.status = 202;
	r.content_type.clear();
	return r;
}

// CORS for the replies the transport builds without a parsed request: the read-side
// limits and the request timeout. Takes the raw Origin header because at that point
// there is no Request to resolve one from.
void CApiDispatcher::StampCorsForTransport(
	std::map<std::string, std::string> &headers, const std::string &origin_header)
{
	CHttpServer::Request synthetic;
	if (!origin_header.empty()) {
		synthetic.headers.emplace("Origin", origin_header);
	}
	const CorsDecision cors_org = ResolveCorsOrigin(synthetic, m_config);
	ApplyCorsHeaders(headers, cors_org, m_config.ServerCfg().allow_cors);
}

boost::optional<CHttpServer::Response> CApiDispatcher::PreflightEvents(const CHttpServer::Request &req)
{
	// Same bearer/cookie check the live handler used to do, but run on the I/O thread
	// BEFORE a worker is spawned and BEFORE the 32-slot SSE budget is touched, so the
	// slot stays free for legitimate subscribers.
	auto a = Authenticate(req);
	if (!a.ok) {
		// CORS on the rejection too: leaving the 401/403/429 without it means a
		// cross-origin SSE client sees an opaque fetch failure exactly when it most
		// needs to read why it was turned away.
		{
			const CorsDecision cors_org = ResolveCorsOrigin(req, m_config);
			ApplyCorsHeaders(a.rejection.headers, cors_org, m_config.ServerCfg().allow_cors);
		}
		return a.rejection;
	}
	// A HEAD here asks what a GET would answer with, not for the stream. Answered in the
	// dispatcher rather than the transport so it picks up the same CORS bundle the
	// stream carries.
	if (req.method == "HEAD") {
		CHttpServer::Response probe;
		probe.status = 200;
		probe.content_type = "text/event-stream";
		probe.headers["Cache-Control"] = "no-cache";
		probe.headers["X-Accel-Buffering"] = "no";
		// Mirror the encoding the GET would negotiate. The probe body is empty, so
		// nothing downstream will compress it and the header has to be stated: without
		// it a HEAD says identity while the stream is gzipped.
		if (AcceptsGzip(FindHeaderCaseInsensitive(req.headers, "Accept-Encoding"))) {
			probe.headers["Content-Encoding"] = "gzip";
		}
		// NOT keep-alive: this connection is answered and closed, and advertising reuse
		// makes a pooling client fail its next request on a socket we already shut down.
		probe.headers["Connection"] = "close";
		{
			const CorsDecision cors_org = ResolveCorsOrigin(req, m_config);
			ApplyCorsHeaders(probe.headers, cors_org, m_config.ServerCfg().allow_cors);
		}
		return probe;
	}

	// `?channels=` is a 400, not "no filter". The surface's own query rule already says
	// an empty value is an error rather than an omission, and this is the parameter that
	// would otherwise be its exception: a client joining an empty selection list
	// produces exactly this URL, so a UI with every category unchecked was handed the
	// full firehose. Omitting the parameter remains the spelling for "every channel".
	//
	// Checked here rather than in the streaming handler because that one returns void --
	// by the time it parses the query the response is already committed.
	{
		std::string query;
		const std::size_t q = req.target.find('?');
		if (q != std::string::npos)
			query = req.target.substr(q + 1);
		const auto qmap = web_api_path::ParseQuery(query);
		const auto it = qmap.find("channels");
		if (it != qmap.end() && it->second.empty()) {
			return ErrorResponse(400,
				"bad_request",
				"`channels` must not be empty; omit it to receive every channel");
		}
	}
	return boost::none;
}

// SSE runs on a worker thread the HTTP server spawns per connection. Auth is
// enforced in PreflightEvents, synchronously, before the head write and the worker
// spawn. The 15 s heartbeat is a `: keepalive\n\n` SSE comment (RFC 6202) --
// proxies and many browsers drop idle TCP after ~30 s. DispatchStreaming reads head
// out-params ONCE before writing, so this one function sets the head AND runs the
// drain loop.
void CApiDispatcher::DispatchEvents(const CHttpServer::Request &req,
	CHttpServer::Writer &writer,
	unsigned &http_status,
	std::string &content_type,
	std::map<std::string, std::string> &response_headers)
{
	// Auth ran inside PreflightEvents on the I/O thread before this worker
	// spawned, so an authenticated principal can be assumed here.
	auto a = Authenticate(req);
	if (!a.ok) {
		// Defence in depth: if PreflightEvents was bypassed (test harness, future
		// routing change) we still reject here, just not as cheaply.
		http_status = a.rejection.status;
		content_type = "application/json";
		writer.Write(a.rejection.body);
		return;
	}
	// SSE does not need admin: reads are guest-friendly and SSE is a read-only
	// push -- admin-gated mutations do not ship over it.

	http_status = 200;
	content_type = "text/event-stream";
	response_headers["Cache-Control"] = "no-cache";
	response_headers["X-Accel-Buffering"] = "no"; // disable nginx buffering

	// CORS on the SSE response too: EventSource sends `Origin` and reads the
	// standard bundle for credentialed cross-origin streams. No Expose-Headers.
	{
		const CorsDecision cors_org = ResolveCorsOrigin(req, m_config);
		ApplyCorsHeaders(response_headers, cors_org, m_config.ServerCfg().allow_cors);
	}
	// No Connection: keep-alive override -- chunked + streaming requires the
	// default, and HttpServer adds chunked transfer-encoding automatically.

	// Initial reassurance chunk: some browser EventSource impls do not fire
	// `onopen` until at least one chunk lands.
	if (!writer.Write(": connected\n\n"))
		return;

	// Optional `?channels=<csv>`: limit the event types delivered. The mapping from
	// EventBus event name to channel is prefix-based -- download_* -> "downloads",
	// shared_* -> "shared", server_* -> "servers", client_* -> "clients", status_* ->
	// "status", log_* -> "logs".
	//
	// The synthetic per-subscriber `resync` event is ALWAYS delivered regardless of
	// filter: a cache invalidation the client cannot opt out of. Unknown channel names
	// are silently ignored, for forward-compatibility.
	std::set<std::string> channel_filter;
	bool channels_set = false;
	{
		// Cap unique channel tokens at 32 so a 1 MB `channels=` query cannot
		// build a 1M-entry set in the SSE worker.
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
		// Event naming convention: every bus event MUST contain at least one underscore
		// -- the prefix before the first `_` identifies the channel. The only
		// no-underscore name is `resync`, which the caller bypasses by name. Future
		// bare-token events need explicit channel mapping or must always bypass.
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
		// Plural, matching the /chats collection: the bootstrap advice is to GET the
		// collections matching your subscribed channels, which only works if the two
		// names line up.
		if (prefix == "chat")
			return "chats";
		return prefix;
	};
	auto event_passes_filter = [&](const std::string &name) {
		if (!channels_set)
			return true;
		// A cache invalidation is not opt-out-able, and this one arrives over the bus
		// rather than synthesised per subscriber, so it has to bypass here as well as
		// in the reconnect path.
		if (name == "resync")
			return true;
		return channel_filter.count(event_channel(name)) > 0;
	};

	// Registers this session for the life of the stream, so the refresher knows to
	// resume diffing. Drain blocks up to the heartbeat interval (15 s); on timeout we
	// emit `: keepalive`.
	//
	// `since_id` resolution per RFC 6202 4 reconnect:
	//  - absent / unparseable -> start from NewestId (post-connect events only)
	//  - in-range (parsed+1 >= OldestId) -> resume from `parsed`; the first Drain
	//    returns the missed range immediately
	//  - gap (parsed+1 < OldestId) -> events evicted before this client read them;
	//    emit `resync` (reason=gap), then start from NewestId
	//  - parsed > NewestId -> stale id from a prior daemon process (ids reset to 1 on
	//    restart); emit `resync` (reason=restart), start from NewestId.
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
			// Per-subscriber synthetic event -- not on the bus. id is the current
			// newest so the client's EventSource resumes there (no resync loop).
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
	// Heartbeat is wall-clock driven, not Drain-timeout driven: a busy bus plus a
	// `?channels=` that filters every drained event would otherwise leave the wire
	// silent (Drain returns immediately, the loop swallows and re-enters, keepalive
	// never fires). NAT/proxies drop idle TCP after ~30-60 s.
	const auto heartbeat_interval = std::chrono::seconds(15);
	auto last_write_at = std::chrono::steady_clock::now();
	std::vector<webapi::Event> drained;
	while (writer.Alive()) {
		// Shutdown poll. The flag is set by the App on OnExit and Drain() returns
		// immediately once observed, so this client is dropped cleanly before the
		// dispatcher reset() races a worker still holding `m_app` references.
		if (m_app.EventBus().IsShutdown())
			break;
		drained.clear();
		const std::uint64_t new_high = m_app.EventBus().Drain(since_id, heartbeat_interval, drained);
		if (!writer.Alive())
			break;
		if (m_app.EventBus().IsShutdown())
			break;

		// Live-path gap detection. The reconnect handler above only catches gaps at
		// session start; once running, a burst that fills and evicts the ring between
		// Drains would silently drop the missed range. On cursor fall-off emit a typed
		// resync and restart at newest.
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
			// Drop the events the Drain returned -- the client is about to
			// re-fetch the REST collections, which is the `resync` contract.
			continue;
		}

		// Apply ?channels= filter before emission. since_id still advances over EVERY
		// drained event, filtered or not, so the client does not re-see them on
		// reconnect: replay is id-based, not channel-based.
		std::ostringstream frame;
		bool wrote_any = false;
		for (const auto &ev : drained) {
			if (!event_passes_filter(ev.name))
				continue;
			// SSE frame:  event: <name>\nid: <id>\ndata: <data>\n\n
			// Per RFC 6202 4 `data:` lines are single-line; our JSON payloads never
			// contain literal newlines (EventDiff escapes them).
			//
			// `ev.name` is NOT escaped -- every event name on the bus is a
			// server-controlled compile-time literal. A future publisher taking a name
			// from external input MUST sanitize CR/LF/`\0` at its call site.
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
				// Advance the cursor silently so the next Drain does not re-read them.
				since_id = new_high;
			}
			// Drain timed out with nothing new, or everything was filtered out.
			// Either way, heartbeat IFF nothing was written in the window.
			const auto now = std::chrono::steady_clock::now();
			if (now - last_write_at >= heartbeat_interval) {
				if (!writer.Write(": keepalive\n\n"))
					break;
				last_write_at = now;
			}
		}
	}
}
