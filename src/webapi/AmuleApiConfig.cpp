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

#include <cryptopp/osrng.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif


namespace {

bool HexDecode(const std::string &in, std::vector<unsigned char> &out)
{
	if (in.size() % 2 != 0) return false;
	out.clear();
	out.reserve(in.size() / 2);
	auto nibble = [](char c, unsigned &v) -> bool {
		if (c >= '0' && c <= '9') { v = c - '0';        return true; }
		if (c >= 'a' && c <= 'f') { v = c - 'a' + 10;   return true; }
		if (c >= 'A' && c <= 'F') { v = c - 'A' + 10;   return true; }
		return false;
	};
	for (size_t i = 0; i < in.size(); i += 2) {
		unsigned hi, lo;
		if (!nibble(in[i], hi) || !nibble(in[i + 1], lo)) return false;
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
		out[i * 2]     = hex[(data[i] >> 4) & 0x0F];
		out[i * 2 + 1] = hex[ data[i]       & 0x0F];
	}
	return out;
}

// Trim ASCII whitespace from both ends. Used on each line of the
// passwords file before tokenising; tolerates trailing CR (Windows-
// edited file checked out on POSIX) and stray indentation.
std::string Trim(const std::string &s)
{
	size_t a = 0;
	while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
	size_t b = s.size();
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
	return s.substr(a, b - a);
}

// 32 lowercase hex chars = the canonical MD5 digest shape. Reject
// anything else so we never store a half-typed/half-pasted line as
// a "password".
bool LooksLikeMd5Hex(const std::string &s)
{
	if (s.size() != 32) return false;
	for (char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	return true;
}

wxString JoinPath(const wxString &dir, const wxString &leaf)
{
	wxFileName fn(dir, leaf);
	return fn.GetFullPath();
}

}  // namespace


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
			m_lastError = "config dir does not exist and could not be created: "
				+ std::string(m_configDir.utf8_str());
			return false;
		}
	}

	const wxString cfg_path     = JoinPath(m_configDir, "amuleapi.conf");
	const wxString secret_path  = JoinPath(m_configDir, "amuleapi-jwt-secret");
	const wxString pwfile_path  = JoinPath(m_configDir, "amuleapi-passwords");

	if (!LoadAmuleapiConf(cfg_path))   return false;
	if (!LoadJwtSecret(secret_path))   return false;
	if (!LoadPasswords(pwfile_path))   return false;

	return true;
}


bool CAmuleApiConfig::LoadAmuleapiConf(const wxString &path)
{
	if (!wxFileExists(path)) {
		// First-run: write a defaults file so the operator has something
		// to edit. The EC password stays empty; amuleapi will refuse to
		// connect until it's filled in (matching amuleweb's behaviour).
		wxFFileOutputStream out(path);
		if (!out.IsOk()) {
			m_lastError = "cannot create amuleapi.conf: "
				+ std::string(path.utf8_str());
			return false;
		}
		const char *defaults =
			"[Server]\n"
			"BindAddress=127.0.0.1\n"
			"Port=4713\n"
			"AllowCORS=0\n"
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
			"[Logging]\n"
			"Level=info\n"
			"File=\n";
		out.Write(defaults, std::strlen(defaults));
	}

	wxFileConfig cfg("", "", path, "",
		wxCONFIG_USE_LOCAL_FILE | wxCONFIG_USE_RELATIVE_PATH);

	wxString s;
	long     n = 0;

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

	if (cfg.Read("/Logging/Level", &s) && !s.IsEmpty()) {
		m_logging.level = std::string(s.utf8_str());
	}
	if (cfg.Read("/Logging/File", &s)) {
		m_logging.file = std::string(s.utf8_str());
	}

	return true;
}


bool CAmuleApiConfig::LoadJwtSecret(const wxString &path)
{
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

	if (!EnforceOwnerOnly(path)) return false;

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
		//   amuleapi --set-admin-pass=<plain>
		// hashes + writes the admin line; the daemon then accepts logins.
		return WritePasswordsFile(m_configDir, "", "");
	}

	if (!EnforceOwnerOnly(path)) return false;

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
		const std::string raw = (nl == std::string::npos)
			? remainder
			: remainder.substr(0, nl);
		remainder = (nl == std::string::npos) ? std::string()
		                                       : remainder.substr(nl + 1);

		const std::string line = Trim(raw);
		if (line.empty() || line[0] == '#') continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			m_lastError = "amuleapi-passwords: malformed line (no '=')";
			return false;
		}
		const std::string key = Trim(line.substr(0, eq));
		const std::string val = Trim(line.substr(eq + 1));
		if (val.empty()) continue;   // role explicitly disabled
		if (!LooksLikeMd5Hex(val)) {
			m_lastError = "amuleapi-passwords: value for '" + key +
				"' is not 32 lowercase hex chars";
			return false;
		}
		if      (key == "admin") m_adminPasswordMd5 = val;
		else if (key == "guest") m_guestPasswordMd5 = val;
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
		std::snprintf(buf, sizeof(buf),
			"%s has mode 0%o; expected 0600 (owner read/write only). "
			"Fix with: chmod 600 \"%s\"",
			p.c_str(), st.st_mode & 0777, p.c_str());
		m_lastError = buf;
		return false;
	}
	return true;
#endif
}


bool CAmuleApiConfig::WritePasswordsFile(const wxString &config_dir,
                                         const std::string &admin_md5,
                                         const std::string &guest_md5)
{
	const wxString path = JoinPath(config_dir, "amuleapi-passwords");
	// Open/create with 0600 from the start. wxFile uses umask, so use
	// the POSIX path under #ifndef _WIN32 to guarantee the mode bits.
#ifndef _WIN32
	const std::string p(path.utf8_str());
	const int fd = ::open(p.c_str(),
		O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		m_lastError = "cannot open " + p + " for writing";
		return false;
	}
	::fchmod(fd, S_IRUSR | S_IWUSR);   // belt+braces against odd umasks
	std::string body;
	if (!admin_md5.empty()) body += "admin=" + admin_md5 + "\n";
	if (!guest_md5.empty()) body += "guest=" + guest_md5 + "\n";
	if (!body.empty()) {
		(void)::write(fd, body.data(), body.size());
	}
	::close(fd);
#else
	wxFile f;
	if (!f.Create(path, true)) {
		m_lastError = "cannot create amuleapi-passwords";
		return false;
	}
	std::string body;
	if (!admin_md5.empty()) body += "admin=" + admin_md5 + "\n";
	if (!guest_md5.empty()) body += "guest=" + guest_md5 + "\n";
	if (!body.empty()) f.Write(body.data(), body.size());
	f.Close();
#endif
	if (!admin_md5.empty()) m_adminPasswordMd5 = admin_md5;
	if (!guest_md5.empty()) m_guestPasswordMd5 = guest_md5;
	return true;
}


bool CAmuleApiConfig::WriteJwtSecretFile(const wxString &config_dir,
                                         const std::vector<unsigned char> &secret_32)
{
	if (secret_32.size() != 32) {
		m_lastError = "WriteJwtSecretFile: expected 32 bytes";
		return false;
	}
	const wxString path = JoinPath(config_dir, "amuleapi-jwt-secret");
	const std::string hex_line = HexEncode(secret_32) + "\n";

#ifndef _WIN32
	const std::string p(path.utf8_str());
	const int fd = ::open(p.c_str(),
		O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		m_lastError = "cannot open amuleapi-jwt-secret for writing";
		return false;
	}
	::fchmod(fd, S_IRUSR | S_IWUSR);
	(void)::write(fd, hex_line.data(), hex_line.size());
	::close(fd);
#else
	wxFile f;
	if (!f.Create(path, true)) {
		m_lastError = "cannot create amuleapi-jwt-secret";
		return false;
	}
	f.Write(hex_line.data(), hex_line.size());
	f.Close();
#endif
	return true;
}
