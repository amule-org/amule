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

#include "AmuleApiConfig.h"

#include <wx/file.h>
#include <wx/fileconf.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>
#include <wx/wfstream.h>

// See CryptoPP_Inc.h for pragma rationale.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
#pragma clang diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#endif
#include <cryptopp/osrng.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{

bool HexDecode(const std::string &in, std::vector<unsigned char> &out)
{
	if (in.size() % 2 != 0)
		return false;
	out.clear();
	out.reserve(in.size() / 2);
	auto nibble = [](char c, unsigned &v) -> bool {
		if (c >= '0' && c <= '9') {
			v = c - '0';
			return true;
		}
		if (c >= 'a' && c <= 'f') {
			v = c - 'a' + 10;
			return true;
		}
		if (c >= 'A' && c <= 'F') {
			v = c - 'A' + 10;
			return true;
		}
		return false;
	};
	for (size_t i = 0; i < in.size(); i += 2) {
		unsigned hi, lo;
		if (!nibble(in[i], hi) || !nibble(in[i + 1], lo))
			return false;
		out.push_back(static_cast<unsigned char>((hi << 4) | lo));
	}
	return true;
}

std::string HexEncode(const std::vector<unsigned char> &data)
{
	static const char hex[] = "0123456789abcdef";
	std::string out;
	out.resize(data.size() * 2);
	for (size_t i = 0; i < data.size(); ++i) {
		out[i * 2] = hex[(data[i] >> 4) & 0x0F];
		out[i * 2 + 1] = hex[data[i] & 0x0F];
	}
	return out;
}

// Trim ASCII whitespace from both ends. Used on each line of the
// passwords file before tokenising; tolerates trailing CR (Windows-
// edited file checked out on POSIX) and stray indentation.
std::string Trim(const std::string &s)
{
	size_t a = 0;
	while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
		++a;
	size_t b = s.size();
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		--b;
	return s.substr(a, b - a);
}

// 32 lowercase hex chars = the canonical MD5 digest shape. Reject
// anything else so we never store a half-typed/half-pasted line as
// a "password".
bool LooksLikeMd5Hex(const std::string &s)
{
	if (s.size() != 32)
		return false;
	for (char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

wxString JoinPath(const wxString &dir, const wxString &leaf)
{
	wxFileName fn(dir, leaf);
	return fn.GetFullPath();
}

// Crash-safe writer for the 0600 secret files (amuleapi-passwords,
// amuleapi-jwt-secret). Writes the body to a sibling `<name>.tmp`,
// fsyncs, then atomically rename(2)s onto the target. A partial write
// or a crash mid-write leaves the original file intact — important
// because amuleapi-passwords stores the only admin/guest credentials
// the daemon has. Falls back to a non-atomic best-effort path on
// Windows (POSIX rename(2) semantics aren't available there for
// existing-target replacement).
bool WriteFileAtomic0600(const wxString &target_path, const std::string &body)
{
#ifndef _WIN32
	const std::string final_p(target_path.utf8_str());
	const std::string tmp_p = final_p + ".tmp";

	const int fd = ::open(tmp_p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0)
		return false;
	::fchmod(fd, S_IRUSR | S_IWUSR); // belt+braces against odd umasks

	std::size_t written = 0;
	while (written < body.size()) {
		const ssize_t n = ::write(fd, body.data() + written, body.size() - written);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			::close(fd);
			::unlink(tmp_p.c_str());
			return false;
		}
		written += static_cast<std::size_t>(n);
	}
	if (::fsync(fd) != 0) {
		::close(fd);
		::unlink(tmp_p.c_str());
		return false;
	}
	if (::close(fd) != 0) {
		::unlink(tmp_p.c_str());
		return false;
	}
	if (::rename(tmp_p.c_str(), final_p.c_str()) != 0) {
		::unlink(tmp_p.c_str());
		return false;
	}
	return true;
#else
	// Windows: best-effort. wxFile::Write returns the bytes written;
	// short writes are caught and reported.
	wxFile f;
	if (!f.Create(target_path, true))
		return false;
	if (body.empty()) {
		f.Close();
		return true;
	}
	const ssize_t n = f.Write(body.data(), body.size());
	f.Close();
	return n == static_cast<ssize_t>(body.size());
#endif
}

} // namespace

wxString DefaultConfigDir()
{
	// Prefer the wx-standard "user data dir" — it already encapsulates
	// the per-platform conventions amuled / amulegui use, so amuleapi
	// drops its files alongside theirs by default. Operators with a
	// custom amule home override the location at the CLI.
	const wxString d = wxStandardPaths::Get().GetUserDataDir();
	// GetUserDataDir() returns e.g. "/Users/foo/Library/Application Support/aMule"
	// on macOS or "/home/foo/.aMule" on Linux. Both already align with
	// where amule.conf lives.
	return d;
}

bool CAmuleApiConfig::Load(const wxString &config_dir)
{
	m_configDir = config_dir;
	m_lastError.clear();

	if (!wxDirExists(m_configDir)) {
		if (!wxMkdir(m_configDir, 0700)) {
			m_lastError = "config dir does not exist and could not be created: " +
				      std::string(m_configDir.utf8_str());
			return false;
		}
	}

	const wxString cfg_path = JoinPath(m_configDir, "amuleapi.conf");
	const wxString secret_path = JoinPath(m_configDir, "amuleapi-jwt-secret");
	const wxString pwfile_path = JoinPath(m_configDir, "amuleapi-passwords");

	if (!LoadAmuleapiConf(cfg_path))
		return false;
	if (!LoadJwtSecret(secret_path))
		return false;
	if (!LoadPasswords(pwfile_path))
		return false;

	return true;
}

bool CAmuleApiConfig::LoadAmuleapiConf(const wxString &path)
{
	const char *defaults = "[Server]\n"
			       "BindAddress=127.0.0.1\n"
			       "Port=4713\n"
			       "AllowCORS=0\n"
			       "StaticRoot=\n"
			       "\n"
			       "[EC]\n"
			       "Host=127.0.0.1\n"
			       "Port=4712\n"
			       "Password=\n"
			       "\n"
			       "[Auth]\n"
			       "LoginFailureWindowSeconds=60\n"
			       "LoginFailureThreshold=5\n"
			       "LoginLockoutSeconds=300\n"
			       "\n"
			       "[Streaming]\n"
			       "EventBusRingCapacity=16384\n";

	if (!wxFileExists(path)) {
		// First-run: write mode-0600 defaults file. EC password stays
		// empty; amuleapi refuses to connect until it's filled in.
		// amuleapi.conf carries `[EC]/Password=` in cleartext (base
		// class wants hashable plaintext), so owner-only mode matches
		// jwt-secret and passwords files.
		//
		// WriteFileAtomic0600 (write-temp, fsync, rename) so a crash
		// mid-write can't leave a truncated config that the next
		// start would happily load as partial → silent default flip.
		if (!WriteFileAtomic0600(path, std::string(defaults))) {
			m_lastError = "cannot create amuleapi.conf: " + std::string(path.utf8_str());
			return false;
		}
	}

	// Enforce 0600 on every load so a hand-edit (or a `cp` from a
	// loose-permission source) doesn't silently widen the EC
	// password's exposure.
	if (!EnforceOwnerOnly(path))
		return false;

	wxFileConfig cfg("", "", path, "", wxCONFIG_USE_LOCAL_FILE | wxCONFIG_USE_RELATIVE_PATH);

	wxString s;
	long n = 0;

	if (cfg.Read("/Server/BindAddress", &s) && !s.IsEmpty()) {
		m_server.bind_address = std::string(s.utf8_str());
	}
	if (cfg.Read("/Server/Port", &n) && n > 0 && n < 65536) {
		m_server.port = static_cast<unsigned>(n);
	}
	{
		long allow = 0;
		if (cfg.Read("/Server/AllowCORS", &allow)) {
			m_server.allow_cors = (allow != 0);
		}
	}
	if (cfg.Read("/Server/CorsOriginAllowlist", &s) && !s.IsEmpty()) {
		// Comma-separated. Trimmed; empty entries dropped.
		wxStringTokenizer tk(s, ",");
		while (tk.HasMoreTokens()) {
			const wxString item = tk.GetNextToken().Trim(true).Trim(false);
			if (!item.IsEmpty()) {
				m_server.cors_origin_allowlist.emplace_back(item.utf8_str());
			}
		}
	}
	if (cfg.Read("/Server/StaticRoot", &s)) {
		const wxString trimmed = s.Trim(true).Trim(false);
		if (!trimmed.IsEmpty()) {
			m_server.static_root = std::string(trimmed.utf8_str());
		}
	}

	if (cfg.Read("/EC/Host", &s) && !s.IsEmpty()) {
		m_ec.host = std::string(s.utf8_str());
	}
	if (cfg.Read("/EC/Port", &n) && n > 0 && n < 65536) {
		m_ec.port = static_cast<unsigned>(n);
	}
	if (cfg.Read("/EC/Password", &s)) {
		m_ec.password = std::string(s.utf8_str());
	}

	if (cfg.Read("/Auth/LoginFailureWindowSeconds", &n) && n > 0) {
		m_auth.login_failure_window_seconds = static_cast<unsigned>(n);
	}
	if (cfg.Read("/Auth/LoginFailureThreshold", &n) && n > 0) {
		m_auth.login_failure_threshold = static_cast<unsigned>(n);
	}
	if (cfg.Read("/Auth/LoginLockoutSeconds", &n) && n > 0) {
		m_auth.login_lockout_seconds = static_cast<unsigned>(n);
	}

	// `[Streaming]/EventBusRingCapacity`. Below the CEventBus floor
	// is silently clamped up by the bus itself; we just accept any
	// positive value here.
	if (cfg.Read("/Streaming/EventBusRingCapacity", &n) && n > 0) {
		m_streaming.event_bus_ring_capacity = static_cast<unsigned>(n);
	}

	return true;
}

bool CAmuleApiConfig::LoadJwtSecret(const wxString &path)
{
	// Rotation is operator-manual today: delete amuleapi-jwt-secret
	// and restart amuleapi, which auto-generates a fresh secret and
	// invalidates every previously-issued token. A `--rotate-jwt-
	// secret` CLI subcommand that does the file replacement + a
	// SIGHUP reload without a full restart is roadmapped for 3.1
	// (would let the daemon keep accepting old-keyed tokens for a
	// grace window). Until then, the manual flow is documented in
	// the amuleapi(1) FILES section.
	if (!wxFileExists(path)) {
		// Auto-generate 32 random bytes. The new file is 0600 from
		// the moment it lands on disk (open + chmod before any data).
		std::vector<unsigned char> secret(32, 0);
		CryptoPP::AutoSeededRandomPool rng;
		rng.GenerateBlock(secret.data(), secret.size());
		if (!WriteJwtSecretFile(m_configDir, secret)) {
			return false;
		}
		m_jwtSecret = std::move(secret);
		return true;
	}

	if (!EnforceOwnerOnly(path))
		return false;

	wxFile f(path, wxFile::read);
	if (!f.IsOpened()) {
		m_lastError = "cannot open amuleapi-jwt-secret";
		return false;
	}
	const wxFileOffset sz = f.Length();
	// 64 hex chars + optional trailing newline. Cap generously to
	// catch "someone pasted a 2 KB blob" without truncating valid
	// edits.
	if (sz < 64 || sz > 4096) {
		m_lastError = "amuleapi-jwt-secret has unexpected size; "
			      "expected 64 hex chars (256-bit secret)";
		return false;
	}
	std::string buf(static_cast<size_t>(sz), '\0');
	if (f.Read(&buf[0], buf.size()) != static_cast<ssize_t>(buf.size())) {
		m_lastError = "amuleapi-jwt-secret read failed";
		return false;
	}
	const std::string trimmed = Trim(buf);
	if (trimmed.size() != 64) {
		m_lastError = "amuleapi-jwt-secret is not 64 hex chars after trim";
		return false;
	}
	std::vector<unsigned char> decoded;
	if (!HexDecode(trimmed, decoded) || decoded.size() != 32) {
		m_lastError = "amuleapi-jwt-secret is not valid hex";
		return false;
	}
	m_jwtSecret = std::move(decoded);
	return true;
}

bool CAmuleApiConfig::LoadPasswords(const wxString &path)
{
	if (!wxFileExists(path)) {
		// Auto-create empty so the operator sees the file exists, with
		// the right mode bits. CLI flow:
		//  amuleapi --set-admin-pass=<plain>
		// hashes + writes the admin line; the daemon then accepts logins.
		return WritePasswordsFile(m_configDir, "", "");
	}

	if (!EnforceOwnerOnly(path))
		return false;

	wxFile f(path, wxFile::read);
	if (!f.IsOpened()) {
		m_lastError = "cannot open amuleapi-passwords";
		return false;
	}
	const wxFileOffset sz = f.Length();
	if (sz < 0 || sz > 4096) {
		m_lastError = "amuleapi-passwords has unexpected size";
		return false;
	}
	std::string buf(static_cast<size_t>(sz), '\0');
	if (sz > 0 && f.Read(&buf[0], buf.size()) != static_cast<ssize_t>(buf.size())) {
		m_lastError = "amuleapi-passwords read failed";
		return false;
	}

	std::string remainder = buf;
	while (!remainder.empty()) {
		const size_t nl = remainder.find('\n');
		const std::string raw = (nl == std::string::npos) ? remainder : remainder.substr(0, nl);
		remainder = (nl == std::string::npos) ? std::string() : remainder.substr(nl + 1);

		const std::string line = Trim(raw);
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			m_lastError = "amuleapi-passwords: malformed line (no '=')";
			return false;
		}
		const std::string key = Trim(line.substr(0, eq));
		const std::string val = Trim(line.substr(eq + 1));
		if (val.empty())
			continue; // role explicitly disabled
		if (!LooksLikeMd5Hex(val)) {
			m_lastError =
				"amuleapi-passwords: value for '" + key + "' is not 32 lowercase hex chars";
			return false;
		}
		if (key == "admin")
			m_adminPasswordMd5 = val;
		else if (key == "guest")
			m_guestPasswordMd5 = val;
		else {
			m_lastError = "amuleapi-passwords: unknown key '" + key + "'";
			return false;
		}
	}
	return true;
}

bool CAmuleApiConfig::EnforceOwnerOnly(const wxString &path)
{
#ifdef _WIN32
	(void)path;
	return true;
#else
	struct stat st;
	const std::string p(path.utf8_str());
	if (stat(p.c_str(), &st) != 0) {
		m_lastError = "stat failed for " + p;
		return false;
	}
	if ((st.st_mode & 0077) != 0) {
		char buf[256];
		std::snprintf(buf,
			sizeof(buf),
			"%s has mode 0%o; expected 0600 (owner read/write only). "
			"Fix with: chmod 600 \"%s\"",
			p.c_str(),
			st.st_mode & 0777,
			p.c_str());
		m_lastError = buf;
		return false;
	}
	return true;
#endif
}

bool CAmuleApiConfig::WritePasswordsFile(
	const wxString &config_dir, const std::string &admin_md5, const std::string &guest_md5)
{
	const wxString path = JoinPath(config_dir, "amuleapi-passwords");
	std::string body;
	if (!admin_md5.empty())
		body += "admin=" + admin_md5 + "\n";
	if (!guest_md5.empty())
		body += "guest=" + guest_md5 + "\n";
	if (!WriteFileAtomic0600(path, body))
		return false;
	if (!admin_md5.empty())
		m_adminPasswordMd5 = admin_md5;
	if (!guest_md5.empty())
		m_guestPasswordMd5 = guest_md5;
	return true;
}

bool CAmuleApiConfig::WriteJwtSecretFile(
	const wxString &config_dir, const std::vector<unsigned char> &secret_32)
{
	if (secret_32.size() != 32) {
		m_lastError = "WriteJwtSecretFile: expected 32 bytes";
		return false;
	}
	const wxString path = JoinPath(config_dir, "amuleapi-jwt-secret");
	return WriteFileAtomic0600(path, HexEncode(secret_32) + "\n");
}
