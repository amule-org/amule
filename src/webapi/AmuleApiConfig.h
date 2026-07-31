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

#ifndef WEBAPI_CONFIG_H
#define WEBAPI_CONFIG_H

#include <ctime>
#include <string>
#include <vector>

#include <wx/string.h>

#include "Credentials.h"

// amuleapi's three on-disk config files (in the user's amule config
// dir; independent of remote.conf):
//
//   amuleapi.conf         INI — HTTP bind + port + EC connection
//                         params + auth tunables
//   amuleapi-jwt-secret   raw hex (64 chars + \n) — HMAC-SHA-256 key
//   amuleapi-passwords    two-line text — admin=<record> / guest=<record>
//
// The credential records are owned by webcommon/Credentials.h, not by
// this class: amuled and monolithic aMule write the same file when a
// password is changed from amulegui or the preferences dialog, so the
// format has exactly one implementation and the three cannot drift.
//
// `amuleapi-jwt-secret` is auto-generated with 32 random bytes on
// first run. `amuleapi-passwords` may be empty (daemon refuses
// /auth/login until at least one role is set via
// `amuleapi --set-admin-pass=...`, from amulegui, or over REST).
// `amuleapi.conf` is created from defaults if missing.
//
// POSIX: both secret files must be 0600; looser bits → daemon
// refuses to start with an actionable error. Windows has no
// equivalent enforcement (QUICKSTART covers ACL mitigation).

class CAmuleApiConfig
{
public:
	struct Server
	{
		std::string bind_address = "127.0.0.1";
		unsigned port = 4713;
		bool allow_cors = false;
		std::vector<std::string> cors_origin_allowlist;
		// Filesystem root of a bundled web frontend. Empty (default) =
		// API-only deployment: non-/api/ paths return 404. Non-empty =
		// the daemon serves GET/HEAD requests for paths outside /api/
		// from this directory, with an index.html SPA fallback for
		// extension-less misses. See ServeStaticFile in Api.cpp.
		std::string static_root;
	};

	struct Ec
	{
		std::string host = "127.0.0.1";
		unsigned port = 4712;
		std::string password; // matches amuled's [ExternalConnect]/Password
		// Offer EC transport encryption. On by default for every
		// destination; only the client knows what it dialed, so the
		// choice lives here rather than in amuled.
		bool encryption = true;
	};

	struct Auth
	{
		unsigned login_failure_window_seconds = 60;
		unsigned login_failure_threshold = 5;
		unsigned login_lockout_seconds = 300;
	};

	struct Streaming
	{
		// SSE ring capacity. Sized for a cold-start tick on a busy
		// node (5K downloads + 5K shared can publish ~10K `*_added`
		// in one tick before any subscriber drains). Values below
		// the CEventBus::kMinCapacity floor are clamped up at the
		// bus level so an operator can't accidentally disable
		// replay. Operators with very heavy nodes can raise this;
		// memory ≈ capacity × ~1 KB JSON payload.
		unsigned event_bus_ring_capacity = 16384;
	};

	// Bring everything into memory from `config_dir`. Returns true on
	// success; false on missing required field, mode-bit failure, or
	// malformed INI. On failure, the human-readable reason is left in
	// LastError() so the caller can surface it via Show(...).
	bool Load(const wxString &config_dir);

	const wxString &ConfigDir() const { return m_configDir; }
	const Server &ServerCfg() const { return m_server; }
	const Ec &EcCfg() const { return m_ec; }
	const Auth &AuthCfg() const { return m_auth; }
	const Streaming &StreamingCfg() const { return m_streaming; }

	// Raw HMAC secret (32 bytes when loaded from a valid 64-char
	// hex file). May be reloaded from disk via Load(...).
	const std::vector<unsigned char> &JwtSecret() const { return m_jwtSecret; }

	// Stored credential records for the two roles, in whatever form
	// amuleapi-passwords holds them (see webcommon/Credentials.h — a PHC
	// string normally, a bare MD5 for a config predating the KDF). Empty
	// when the role is unset: `/auth/login` returns `login_disabled`.
	// These are NOT digests to compare an input against — for that, and
	// for the file-freshness check that comes with it, use VerifyPassword.
	const std::string &AdminCredential() const { return m_credentials.admin; }
	const std::string &GuestCredential() const { return m_credentials.guest; }

	// True when at least one role can log in. Gates both the non-loopback
	// bind refusal and the `login_disabled` login response.
	bool HasAnyCredential() const;

	// Which role a presented password belongs to, if any.
	enum class MatchedRole
	{
		None,
		Admin,
		Guest
	};

	// Verifies an MD5 hex digest against both roles, admin first.
	//
	// Not const, for two reasons. It re-reads amuleapi-passwords first, so
	// a password set by amuled (pushed over EC from amulegui) or by
	// aMule's preferences dialog takes effect without restarting amuleapi
	// — which also means HasAnyCredential() is up to date immediately
	// after this call, and stale before it. And a record that verified but
	// predates the current KDF parameters is rewritten at the current
	// cost, so the upgrade needs no operator step.
	MatchedRole VerifyPassword(const std::string &md5_hex);

	// Sets or clears one role and persists it, leaving the other role as
	// it stands on disk. `md5_hex` empty clears the role — that is how the
	// guest role is disabled. Returns false with LastError() set on a
	// malformed digest or a write failure.
	bool SetPassword(webcommon::CredentialRole role, const std::string &md5_hex);

	// Modification time of amuleapi-passwords, or 0 when there is no file
	// (nothing configured, so nothing to invalidate). Used to reject
	// sessions older than the last password change, whoever made it.
	std::time_t CredentialsChangedAt() const;

	const std::string &LastError() const { return m_lastError; }

	// Re-reads amuleapi-passwords, keeping the current values if the read
	// fails so a transient error can't lock everyone out.
	//
	// Unconditional rather than gated on a stat: mtime is second-
	// granularity on every platform here, and two writes inside one
	// second produce records of identical length, so a
	// timestamp-plus-size check would miss a rotation *permanently*, not
	// just briefly. The file is under 4 KB and this runs once per login
	// attempt, behind the rate limiter and in front of a PBKDF2 — the
	// read does not show up next to either. VerifyPassword calls it first;
	// a caller that just wrote the file calls it directly so the state it
	// reports back is the state it stored.
	void ReloadCredentials();

private:
	// Writes amuleapi-jwt-secret with owner-only permissions. Internal to
	// Load()'s first-run path -- callers get the secret via JwtSecret().
	bool WriteJwtSecretFile(const wxString &config_dir, const std::vector<unsigned char> &secret_32);

	// Rewrites one role's record at the current KDF cost without moving
	// the file's modification time — an upgrade is not a password change,
	// and only a password change should end sessions.
	void RehashInPlace(webcommon::CredentialRole role, const std::string &md5_hex);

	bool LoadAmuleapiConf(const wxString &path);
	bool LoadJwtSecret(const wxString &path);
	bool LoadPasswords(const wxString &path);

	// POSIX-only mode check. Returns true on Windows (no enforcement
	// possible) or when the file matches 0600. Sets m_lastError on
	// failure.
	bool EnforceOwnerOnly(const wxString &path);

	wxString m_configDir;
	Server m_server;
	Ec m_ec;
	Auth m_auth;
	Streaming m_streaming;
	std::vector<unsigned char> m_jwtSecret;
	webcommon::Credentials m_credentials;

	std::string m_lastError;
};

#endif // WEBAPI_CONFIG_H
