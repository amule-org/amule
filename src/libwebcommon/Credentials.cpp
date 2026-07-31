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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "Credentials.h"

#include "AtomicFile.h"
#include "ConstantTime.h"

// cryptopp headers pull in deprecated implicit copy ctors + throw()
// specs (P0806 + C++17). Same wrap as Jwt.cpp; see CryptoPP_Inc.h for
// the full rationale.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
#pragma clang diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#endif
#include <cryptopp/osrng.h>
#include <cryptopp/pwdbased.h>
#include <cryptopp/sha.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace webcommon
{

namespace
{

const char *const kPrefix = "pbkdf2-sha256$";
const std::size_t kSaltBytes = 16;
const std::size_t kHashBytes = 32;

// Iteration count for new records. Sized for a credential check that
// happens at login rather than per request.
const unsigned kPbkdf2Iterations = 210000;

std::string ToHex(const unsigned char *data, std::size_t len)
{
	static const char *digits = "0123456789abcdef";
	std::string out;
	out.reserve(len * 2);
	for (std::size_t i = 0; i < len; ++i) {
		out.push_back(digits[(data[i] >> 4) & 0x0F]);
		out.push_back(digits[data[i] & 0x0F]);
	}
	return out;
}

bool FromHex(const std::string &hex, std::vector<unsigned char> &out)
{
	if (hex.size() % 2 != 0) {
		return false;
	}
	out.clear();
	out.reserve(hex.size() / 2);
	for (std::size_t i = 0; i < hex.size(); i += 2) {
		int hi = -1, lo = -1;
		for (int k = 0; k < 2; ++k) {
			const char c = hex[i + k];
			int v;
			if (c >= '0' && c <= '9') {
				v = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				v = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'F') {
				v = c - 'A' + 10;
			} else {
				return false;
			}
			(k == 0 ? hi : lo) = v;
		}
		out.push_back(static_cast<unsigned char>((hi << 4) | lo));
	}
	return true;
}

bool IsMd5Hex(const std::string &s)
{
	if (s.size() != 32) {
		return false;
	}
	for (const char c : s) {
		if (!std::isxdigit(static_cast<unsigned char>(c))) {
			return false;
		}
	}
	return true;
}

// Lowercase a copy; stored digests are compared case-insensitively
// because aMule's preferences dialog and the EC path have historically
// disagreed about case.
std::string ToLower(const std::string &s)
{
	std::string out(s);
	for (char &c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

std::string Derive(const std::string &md5_hex, const std::vector<unsigned char> &salt, unsigned iterations)
{
	unsigned char derived[kHashBytes];
	CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA256> kdf;
	const std::string lowered = ToLower(md5_hex);
	kdf.DeriveKey(derived,
		sizeof(derived),
		0x00,
		reinterpret_cast<const CryptoPP::byte *>(lowered.data()),
		lowered.size(),
		salt.empty() ? nullptr : &salt[0],
		salt.size(),
		iterations);
	return ToHex(derived, sizeof(derived));
}

// Splits "pbkdf2-sha256$<iters>$<salt>$<hash>". Returns false on any
// malformed record; a malformed record must never verify.
bool ParsePhc(const std::string &stored,
	unsigned &iterations,
	std::vector<unsigned char> &salt,
	std::string &hash_hex)
{
	if (stored.compare(0, std::strlen(kPrefix), kPrefix) != 0) {
		return false;
	}
	const std::string rest = stored.substr(std::strlen(kPrefix));
	const std::size_t d1 = rest.find('$');
	if (d1 == std::string::npos) {
		return false;
	}
	const std::size_t d2 = rest.find('$', d1 + 1);
	if (d2 == std::string::npos) {
		return false;
	}

	const std::string iter_s = rest.substr(0, d1);
	if (iter_s.empty() || iter_s.find_first_not_of("0123456789") != std::string::npos) {
		return false;
	}
	unsigned long parsed = 0;
	std::istringstream(iter_s) >> parsed;
	if (parsed == 0) {
		return false;
	}
	iterations = static_cast<unsigned>(parsed);

	if (!FromHex(rest.substr(d1 + 1, d2 - d1 - 1), salt) || salt.empty()) {
		return false;
	}
	hash_hex = rest.substr(d2 + 1);
	return hash_hex.size() == kHashBytes * 2;
}

std::string TrimAscii(const std::string &s)
{
	std::size_t b = 0;
	std::size_t e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
		++b;
	}
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
		--e;
	}
	return s.substr(b, e - b);
}

// True when `record` is something Verify could ever accept: a PHC record
// in a form this build understands, or a legacy bare MD5. An empty string
// is NOT valid — that is the separate "role unset" state.
bool IsValidRecord(const std::string &record)
{
	if (record.empty()) {
		return false;
	}
	if (IsMd5Hex(record)) {
		return true; // legacy: the record is the unsalted digest itself
	}
	unsigned iterations = 0;
	std::vector<unsigned char> salt;
	std::string hash_hex;
	return ParsePhc(record, iterations, salt, hash_hex);
}

} // namespace

bool IsLegacyMd5Record(const std::string &stored)
{
	return IsMd5Hex(stored);
}

std::string HashMd5Hex(const std::string &md5_hex)
{
	if (!IsMd5Hex(md5_hex)) {
		return std::string();
	}

	std::vector<unsigned char> salt(kSaltBytes);
	CryptoPP::AutoSeededRandomPool rng;
	rng.GenerateBlock(&salt[0], salt.size());

	std::ostringstream out;
	out << kPrefix << kPbkdf2Iterations << '$' << ToHex(&salt[0], salt.size()) << '$'
	    << Derive(md5_hex, salt, kPbkdf2Iterations);
	return out.str();
}

bool VerifyMd5Hex(const std::string &md5_hex, const std::string &stored, bool *needs_rehash)
{
	if (needs_rehash != nullptr) {
		*needs_rehash = false;
	}
	if (stored.empty() || !IsMd5Hex(md5_hex)) {
		return false;
	}

	// Legacy: the record is the unsalted MD5 itself. Still accepted so a
	// developer config from before the KDF keeps working, and flagged so
	// the caller rewrites it after a successful login.
	if (IsLegacyMd5Record(stored)) {
		if (!ConstantTimeEquals(ToLower(md5_hex), ToLower(stored))) {
			return false;
		}
		if (needs_rehash != nullptr) {
			*needs_rehash = true;
		}
		return true;
	}

	unsigned iterations = 0;
	std::vector<unsigned char> salt;
	std::string hash_hex;
	if (!ParsePhc(stored, iterations, salt, hash_hex)) {
		return false;
	}
	if (!ConstantTimeEquals(Derive(md5_hex, salt, iterations), ToLower(hash_hex))) {
		return false;
	}
	// A record written with fewer rounds than we now use still verifies,
	// but wants rewriting at the current cost.
	if (needs_rehash != nullptr && iterations != kPbkdf2Iterations) {
		*needs_rehash = true;
	}
	return true;
}

bool MakeRecord(const std::string &md5_hex, std::string &record)
{
	if (md5_hex.empty()) {
		record.clear();
		return true;
	}
	const std::string hashed = HashMd5Hex(md5_hex);
	if (hashed.empty()) {
		return false;
	}
	record = hashed;
	return true;
}

namespace
{

// Join a config dir and a well-known leaf name. Shared by every file this
// module names so the separator handling cannot drift between them.
std::string JoinConfigDir(const std::string &config_dir, const char *leaf)
{
	if (config_dir.empty()) {
		return std::string(leaf);
	}
	const char sep = config_dir[config_dir.size() - 1];
#ifdef _WIN32
	const bool has_sep = (sep == '/' || sep == '\\');
#else
	const bool has_sep = (sep == '/');
#endif
	return config_dir + (has_sep ? "" : "/") + leaf;
}

} // namespace

std::string CredentialsFilePath(const std::string &config_dir)
{
	return JoinConfigDir(config_dir, "amuleapi-passwords");
}

std::string EcTokenFilePath(const std::string &config_dir)
{
	return JoinConfigDir(config_dir, "amuleapi-ec-token");
}

bool ReadAndConsumeEcToken(const std::string &path, std::string &out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if (!f.is_open()) {
		return false;
	}
	std::ostringstream buf;
	buf << f.rdbuf();
	f.close();

	// Unlink regardless of what we just read: a file that failed to parse
	// is still not something to leave lying around, and the caller has no
	// later moment at which removing it would be safer.
	::remove(path.c_str());

	const std::string token = TrimAscii(buf.str());
	if (!IsMd5Hex(token)) {
		// Same 32-hex-char shape as an MD5 digest -- see GenerateEcToken
		// for why that width. Reject anything else rather than attempt a
		// connection with a truncated or tampered value.
		return false;
	}
	out = ToLower(token);
	return true;
}

std::string GenerateEcToken()
{
	// 16 bytes -> 32 hex chars, the exact width EC's challenge-response
	// already consumes as an MD5 hex digest.
	unsigned char raw[16];
	CryptoPP::AutoSeededRandomPool rng;
	rng.GenerateBlock(raw, sizeof(raw));
	return ToHex(raw, sizeof(raw));
}

bool LoadCredentialsFile(const std::string &config_dir, Credentials &out, std::string &error)
{
	out = Credentials();
	error.clear();

	const std::string path = CredentialsFilePath(config_dir);
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.is_open()) {
		// Absent is the first-run state, not a failure: amuleapi binds to
		// loopback with no credentials until one is claimed.
		return true;
	}

	std::string line;
	while (std::getline(in, line)) {
		const std::string trimmed = TrimAscii(line);
		if (trimmed.empty() || trimmed[0] == '#') {
			continue;
		}
		const std::size_t eq = trimmed.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		const std::string key = TrimAscii(trimmed.substr(0, eq));
		const std::string value = TrimAscii(trimmed.substr(eq + 1));
		if (key != "admin" && key != "guest") {
			error = "unknown key '" + key + "'";
			out = Credentials();
			return false;
		}
		if (value.empty()) {
			continue; // role explicitly disabled
		}
		if (!IsValidRecord(value)) {
			error = "value for '" + key + "' is not a valid credential record";
			out = Credentials();
			return false;
		}
		if (key == "admin") {
			out.admin = value;
		} else {
			out.guest = value;
		}
	}
	return true;
}

bool SaveCredentialsFile(const std::string &config_dir, const Credentials &in, std::string &error)
{
	error.clear();
	const std::string path = CredentialsFilePath(config_dir);

	std::ostringstream body;
	body << "# amuleapi credentials. Managed by amuleapi, amuled and aMule;\n"
	     << "# hand edits are honoured but will be rewritten on the next change.\n"
	     << "admin=" << in.admin << "\n"
	     << "guest=" << in.guest << "\n";
	const std::string text = body.str();

	// Crash-safe and owner-only; see WriteFileAtomic0600. This file holds
	// the only credentials the daemon has, so a partial write or a crash
	// mid-write must leave the previous contents intact.
	if (!WriteFileAtomic0600(path, text)) {
		error = "could not write " + path;
		return false;
	}
	return true;
}

bool UpdateCredentialFile(
	const std::string &config_dir, CredentialRole role, const std::string &record, std::string &error)
{
	Credentials creds;
	if (!LoadCredentialsFile(config_dir, creds, error)) {
		return false;
	}
	if (role == kAdminCredential) {
		creds.admin = record;
	} else {
		creds.guest = record;
	}
	return SaveCredentialsFile(config_dir, creds, error);
}

bool ApplyCredentialChange(const std::string &config_dir, const CredentialChange &change, std::string &error)
{
	Credentials current;
	if (!LoadCredentialsFile(config_dir, current, error)) {
		return false;
	}
	Credentials next = current;

	if (!change.admin_md5.empty() && !MakeRecord(change.admin_md5, next.admin)) {
		error = "admin password is not a valid MD5 digest";
		return false;
	}

	if (!change.guest_enabled) {
		next.guest.clear();
	} else if (!change.guest_md5.empty() && !MakeRecord(change.guest_md5, next.guest)) {
		error = "guest password is not a valid MD5 digest";
		return false;
	}

	// Write only when something actually changed. This is not just an
	// optimisation: the file's modification time is what tells amuleapi a
	// password was rotated, and rotation ends every session opened before
	// it. A no-op rewrite would therefore sign everyone out — and aMule
	// calls this after *every* preferences change, most of which have
	// nothing to do with credentials.
	if (next.admin == current.admin && next.guest == current.guest) {
		return true;
	}
	return SaveCredentialsFile(config_dir, next, error);
}

} // namespace webcommon
