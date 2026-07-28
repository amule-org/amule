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

#ifndef WEBCOMMON_CREDENTIALS_H
#define WEBCOMMON_CREDENTIALS_H

#include <string>

namespace webcommon
{

// amuleapi's admin/guest credentials: how they are hashed and how the
// amuleapi-passwords file is read and written.
//
// This lives in webcommon rather than in amuleapi because three processes
// need to agree on the format: amuleapi itself, amuled (applying a change
// pushed from amulegui over EC), and monolithic amule (applying a change
// made in its own preferences dialog). Exactly one implementation of the
// format, so the three can never drift apart.
//
// What is stored
// --------------
// A PHC-style string, self-describing so the parameters can change later
// without another format break:
//
//     pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>
//
// The input to the KDF is the *MD5 hex* of the password, not the password
// itself. That is deliberate: aMule's preferences dialog hashes with MD5
// before the value goes over EC, and EC sessions are not encrypted, so
// asking for the plaintext would put a new secret on the wire to gain
// nothing. Pre-hashing client-side leaves the wire exactly as it is today
// while the stored form stops being an unsalted digest that a rainbow
// table resolves instantly.
//
// Legacy
// ------
// Early builds stored a bare 32-character MD5 hex digest. amuleapi has not
// shipped, so there is no installed base, but developer configs exist:
// those lines still verify, and Verify() reports that the record wants
// rewriting so the caller can transparently upgrade it.

// PBKDF2 iteration count for new records. Sized for a credential check
// that happens at login rather than per request.
extern const unsigned kPbkdf2Iterations;

// Hashes an MD5 hex digest into a new PHC-style record with a fresh
// random salt. Returns an empty string if `md5_hex` is not 32 hex chars.
std::string HashMd5Hex(const std::string &md5_hex);

// Constant-time verification of `md5_hex` against a stored record, in
// either the PHC or the legacy bare-MD5 form. `needs_rehash` (optional) is
// set when the record verified but is not in the current format, so the
// caller can rewrite it after a successful login.
bool VerifyMd5Hex(const std::string &md5_hex, const std::string &stored, bool *needs_rehash = nullptr);

// True when `stored` is a bare 32-char MD5 hex digest rather than a PHC
// record. Exposed for tests and for the upgrade path.
bool IsLegacyMd5Record(const std::string &stored);

// True when `record` is something Verify could ever accept: a PHC record
// in a form this build understands, or a legacy bare MD5. An empty string
// is NOT valid — that is the separate "role unset" state.
//
// Used to reject a corrupt or hand-mistyped file at load rather than at
// login: a record that silently never verifies is indistinguishable from
// a wrong password, and leaves the operator with nothing to debug.
bool IsValidRecord(const std::string &record);

// The two credentials as they live in amuleapi-passwords. An empty string
// means "not set": for `guest` that is what disables the guest role, and
// for `admin` it is the unclaimed first-run state.
struct Credentials
{
	std::string admin;
	std::string guest;
};

// Reads/writes `<config_dir>/amuleapi-passwords`. Write is crash-safe
// (temp file, fsync, rename) and leaves the file mode at 0600; a partial
// write can therefore never destroy the only credentials the daemon has.
// Both return false and set `error` on failure. Load() treats a missing
// file as an empty set rather than an error, which is the first-run
// state, but fails on an unknown key or a record that could never verify
// — see IsValidRecord for why that is loud rather than silent.
bool LoadCredentialsFile(const std::string &config_dir, Credentials &out, std::string &error);
bool SaveCredentialsFile(const std::string &config_dir, const Credentials &in, std::string &error);

// Which of the two credentials a call operates on.
enum CredentialRole
{
	kAdminCredential,
	kGuestCredential
};

// Sets one role and leaves the other alone. The file is re-read
// immediately before the write, so a change another process made to the
// *other* role since this one loaded is preserved rather than reverted —
// the amuleapi CLI, the REST endpoint and the EC apply path all change
// exactly one role at a time, and all three go through here. An empty
// `record` clears the role (which is how the guest role is disabled).
bool UpdateCredentialFile(
	const std::string &config_dir, CredentialRole role, const std::string &record, std::string &error);

// A requested change, in the form every entry point expresses one: aMule's
// preferences dialog, amulegui's the same dialog over EC, the amuleapi CLI
// and the amuleapi REST endpoint. They differ only in how the request
// arrives, so the rules for interpreting it live here once.
struct CredentialChange
{
	// MD5 hex of the new admin password, or empty to leave the stored one
	// alone. There is deliberately no way to clear it: a client that
	// simply forgot to include the field would otherwise lock a
	// non-loopback deployment out of its own API.
	std::string admin_md5;

	// False clears the guest credential — that IS how guest access is
	// turned off, since guest is enabled exactly when a record exists.
	bool guest_enabled = false;

	// MD5 hex of the new guest password. Empty with `guest_enabled` set
	// leaves the stored guest password alone, so a client changing an
	// unrelated setting need not resend a password it can never read.
	std::string guest_md5;
};

// Applies `change` to `<config_dir>/amuleapi-passwords`, reading the
// current records first so a role the change does not mention keeps
// whatever is stored.
//
// Writes nothing when the result matches what is already there. Callers
// invoke this on every preferences save, credential-related or not, and
// the file's modification time is load-bearing: it is what marks a
// rotation, and a rotation ends every session older than it.
bool ApplyCredentialChange(const std::string &config_dir, const CredentialChange &change, std::string &error);

// Turns a requested password into the record to store: hashes `md5_hex`,
// or yields an empty record for an empty input, which clears the role.
// Returns false (leaving `record` untouched) if the input is neither empty
// nor a 32-char MD5 hex digest, so a half-pasted line can never be stored
// as a password.
bool MakeRecord(const std::string &md5_hex, std::string &record);

// The full path Load/Save operate on, for callers that need to name the
// file in a diagnostic or check its permissions.
std::string CredentialsFilePath(const std::string &config_dir);

} // namespace webcommon

#endif // WEBCOMMON_CREDENTIALS_H
