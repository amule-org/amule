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

#include <string>
#include <vector>

#include <wx/string.h>

// amuleapi's three on-disk config files (in the user's amule config
// dir; independent of remote.conf):
//
//   amuleapi.conf         INI — HTTP bind + port + EC connection
//                         params + auth tunables
//   amuleapi-jwt-secret   raw hex (64 chars + \n) — HMAC-SHA-256 key
//   amuleapi-passwords    two-line text — admin=<md5> / guest=<md5>
//
// `amuleapi-jwt-secret` is auto-generated with 32 random bytes on
// first run. `amuleapi-passwords` may be empty (daemon refuses
// /auth/login until at least one role is set via
// `amuleapi --set-admin-pass=...`). `amuleapi.conf` is created
// from defaults if missing.
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

	// MD5 hex digests for the two roles. Empty when the corresponding
	// line is absent from amuleapi-passwords — `/auth/login` returns
	// `login_disabled` for that role.
	const std::string &AdminPasswordMd5() const { return m_adminPasswordMd5; }
	const std::string &GuestPasswordMd5() const { return m_guestPasswordMd5; }

	// In-memory override of the admin password digest (lowercase MD5 hex),
	// used when amule pushes /AmuleApi/Password over --amule-config-file.
	// Does NOT touch the amuleapi-passwords file, so a standalone operator's
	// saved password is preserved. No-op unless `md5_hex` is 32 lowercase
	// hex chars.
	void SetAdminPasswordMd5(const std::string &md5_hex);

	const std::string &LastError() const { return m_lastError; }

	// Test/CLI helpers — used by `amuleapi --set-admin-pass=...` and
	// the unit test. Writes the file with mode 0600; the caller is
	// responsible for hashing the plaintext to MD5 hex first.
	bool WritePasswordsFile(
		const wxString &config_dir, const std::string &admin_md5, const std::string &guest_md5);

	bool WriteJwtSecretFile(const wxString &config_dir, const std::vector<unsigned char> &secret_32);

private:
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
	std::string m_adminPasswordMd5;
	std::string m_guestPasswordMd5;

	std::string m_lastError;
};

// Canonical config dir per platform. Mirrors amule's own
// GetUserDataDir() but without the dependency on `amule.h` — amuleapi
// runs standalone and can't pull in the GUI/daemon-side helpers.
//
// POSIX (XDG):    ${XDG_CONFIG_HOME:-$HOME/.config}/aMule
//  macOS:          $HOME/Library/Application Support/aMule
//  Windows:        %APPDATA%/aMule
wxString DefaultConfigDir();

#endif // WEBAPI_CONFIG_H
