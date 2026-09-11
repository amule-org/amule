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
		// Filesystem root of a bundled web frontend. Empty (default) = API-only
		// deployment, so non-/api/ paths return 404. Non-empty = the daemon serves
		// GET/HEAD for paths outside /api/ from here, with an index.html SPA
		// fallback for extension-less misses.
		std::string static_root;
	};

	struct Ec
	{
		std::string host = "127.0.0.1";
		unsigned port = 4712;
		std::string password; // matches amuled's [ExternalConnect]/Password
		// Offer EC transport encryption. On by default for every destination; only
		// the client knows what it dialed, so the choice lives here, not in amuled.
		bool encryption = true;
	};

	struct Auth
	{
		unsigned login_failure_window_seconds = 60;
		unsigned login_failure_threshold = 5;
		unsigned login_lockout_seconds = 300;
		// The generic 401 limiter, counting every rejected token on any
		// authenticated route rather than password failures on /auth/login. It was
		// hard-coded while the three above were documented knobs, so an operator
		// could not loosen the one a browser tab left open overnight actually trips.
		unsigned token_failure_window_seconds = 60;
		unsigned token_failure_threshold = 30;
		unsigned token_lockout_seconds = 300;
	};

	struct Streaming
	{
		// SSE ring capacity. Sized for a cold-start tick on a busy node (5K
		// downloads + 5K shared can publish ~10K `*_added` in one tick before any
		// subscriber drains). Values below CEventBus::kMinCapacity are clamped up
		// at the bus level. Memory is roughly capacity x ~1 KB of JSON payload.
		unsigned event_bus_ring_capacity = 16384;

		// Concurrent file-backed responses allowed on `GET /shared/{hash}/content`;
		// over it the transport answers 503 + Retry-After. Six suits one mechanical
		// disk shared with the hasher and the ed2k uploader, but it is a GLOBAL
		// budget: behind a reverse proxy every client arrives from one address, so
		// there is no per-user fairness in it. Each slot pins a file descriptor and
		// a 64 KiB buffer for as long as the peer takes to drain, so a four-digit
		// value would trade away the RSS ceiling the streaming path exists to hold.
		unsigned max_concurrent_file_responses = 6;
	};

	// Bring everything into memory from `config_dir`. Returns false on a missing
	// required field, a mode-bit failure, or malformed INI, leaving the
	// human-readable reason in LastError().
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
	// amuleapi-passwords holds them (a PHC string normally, a bare MD5 for a
	// config predating the KDF). Empty when the role is unset. These are NOT
	// digests to compare an input against -- use VerifyPassword for that, which
	// also brings the file-freshness check with it.
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
	// Not const, for two reasons. It re-reads amuleapi-passwords first, so a
	// password set by amuled or by aMule's preferences dialog takes effect
	// without restarting amuleapi -- which also means HasAnyCredential() is up
	// to date immediately after this call and stale before it. And a record that
	// verified but predates the current KDF parameters is rewritten at the
	// current cost, so the upgrade needs no operator step.
	MatchedRole VerifyPassword(const std::string &md5_hex);

	// Sets or clears one role and persists it, leaving the other as it stands on
	// disk. `md5_hex` empty clears the role -- that is how the guest role is
	// disabled. False with LastError() set on a malformed digest or write failure.
	bool SetPassword(webcommon::CredentialRole role, const std::string &md5_hex);

	// Modification time of amuleapi-passwords, or 0 when there is no file. Used
	// to reject sessions older than the last password change, whoever made it.
	std::time_t CredentialsChangedAt() const;

	const std::string &LastError() const { return m_lastError; }

	// Re-reads amuleapi-passwords, keeping the current values if the read fails
	// so a transient error cannot lock everyone out.
	//
	// Unconditional rather than gated on a stat: mtime is second-granularity on
	// every platform here, and two writes inside one second produce records of
	// identical length, so a timestamp-plus-size check would miss a rotation
	// *permanently*. The file is under 4 KB and this runs once per login
	// attempt, behind the rate limiter and in front of a PBKDF2.
	void ReloadCredentials();

private:
	// Writes amuleapi-jwt-secret with owner-only permissions. Internal to
	// Load()'s first-run path -- callers get the secret via JwtSecret().
	bool WriteJwtSecretFile(const wxString &config_dir, const std::vector<unsigned char> &secret_32);

	// Rewrites one role's record at the current KDF cost without moving the
	// file's modification time -- an upgrade is not a password change, and only
	// a password change should end sessions.
	void RehashInPlace(webcommon::CredentialRole role, const std::string &md5_hex);

	bool LoadAmuleapiConf(const wxString &path);
	bool LoadJwtSecret(const wxString &path);
	bool LoadPasswords(const wxString &path);

	// POSIX-only mode check. Returns true on Windows (no enforcement possible) or
	// when the file matches 0600. Sets m_lastError on failure.
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
