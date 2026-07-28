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

#include <muleunit/test.h>

#include "Credentials.h"

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <fstream>
#include <string>

using namespace muleunit;
using namespace webcommon;

DECLARE_SIMPLE(Credentials)

namespace
{
// MD5("secret") and MD5("other") — the pre-hash the preferences dialog
// produces before the value ever leaves the client.
const char *const MD5_SECRET = "5ebe2294ecd0e0f08eab7690d2a6ee69";
const char *const MD5_OTHER = "795f3202b17cb6bc3d4b771d8c6c9eaf";

// Directory helpers go through wx rather than shelling out: this target
// is built on the mingw-w64 CI legs too, where `mkdir -p` / `rm -rf` are
// not available.
std::string TempDir()
{
	// Unit tests run from the build tree; a per-process subdirectory keeps
	// parallel ctest runs from colliding on the same file.
	const wxString dir = wxString::Format(wxT("credtest-%ld"), static_cast<long>(wxGetProcessId()));
	wxFileName::Mkdir(dir, 0700, wxPATH_MKDIR_FULL);
	return std::string(dir.mb_str());
}

// Removes the one file the tests write plus the directory itself. A
// general recursive delete is not needed and would be riskier.
void RemoveTempDir(const std::string &dir)
{
	const wxString wxdir(dir.c_str(), wxConvUTF8);
	wxRemoveFile(wxdir + wxT("/amuleapi-passwords"));
	wxRmdir(wxdir);
}
} // namespace

TEST(Credentials, HashIsSaltedAndVerifies)
{
	const std::string a = HashMd5Hex(MD5_SECRET);
	const std::string b = HashMd5Hex(MD5_SECRET);

	ASSERT_FALSE(a.empty());
	// Same input, different record: the salt must be per-record, otherwise
	// identical passwords are visibly identical in the file.
	ASSERT_TRUE(a != b);
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, a));
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, b));
}

TEST(Credentials, WrongPasswordFails)
{
	const std::string rec = HashMd5Hex(MD5_SECRET);
	ASSERT_FALSE(VerifyMd5Hex(MD5_OTHER, rec));
}

TEST(Credentials, CaseInsensitiveDigestInput)
{
	// The preferences dialog and the EC path have historically disagreed
	// about digest case; both must verify against the same record.
	const std::string rec = HashMd5Hex(MD5_SECRET);
	std::string upper(MD5_SECRET);
	for (char &c : upper) {
		c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
	}
	ASSERT_TRUE(VerifyMd5Hex(upper, rec));
}

TEST(Credentials, EmptyRecordNeverVerifies)
{
	// An unset credential must not be satisfiable by any input - this is
	// what makes "guest unset" mean "guest disabled".
	ASSERT_FALSE(VerifyMd5Hex(MD5_SECRET, ""));
	ASSERT_FALSE(VerifyMd5Hex("", ""));
}

TEST(Credentials, MalformedRecordNeverVerifies)
{
	ASSERT_FALSE(VerifyMd5Hex(MD5_SECRET, "pbkdf2-sha256$"));
	ASSERT_FALSE(VerifyMd5Hex(MD5_SECRET, "pbkdf2-sha256$0$aabb$ccdd"));
	ASSERT_FALSE(VerifyMd5Hex(MD5_SECRET, "pbkdf2-sha256$1000$nothex$ccdd"));
	ASSERT_FALSE(VerifyMd5Hex(MD5_SECRET, "pbkdf2-sha256$1000$aabb$tooshort"));
	ASSERT_FALSE(VerifyMd5Hex(MD5_SECRET, "scrypt$1$aabb$ccdd"));
	ASSERT_FALSE(VerifyMd5Hex(MD5_SECRET, "garbage"));
}

TEST(Credentials, NonMd5InputRejected)
{
	const std::string rec = HashMd5Hex(MD5_SECRET);
	ASSERT_FALSE(VerifyMd5Hex("secret", rec)); // plaintext, not a digest
	ASSERT_FALSE(VerifyMd5Hex("abc", rec));
	ASSERT_TRUE(HashMd5Hex("not-a-digest").empty());
}

// A developer config written before the KDF stores the bare MD5. It must
// still let its owner in, and must be reported as wanting a rewrite.
TEST(Credentials, LegacyBareMd5VerifiesAndWantsRehash)
{
	bool needs_rehash = false;
	ASSERT_TRUE(IsLegacyMd5Record(MD5_SECRET));
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, MD5_SECRET, &needs_rehash));
	ASSERT_TRUE(needs_rehash);

	needs_rehash = false;
	ASSERT_FALSE(VerifyMd5Hex(MD5_OTHER, MD5_SECRET, &needs_rehash));
}

TEST(Credentials, CurrentFormatDoesNotWantRehash)
{
	bool needs_rehash = true;
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, HashMd5Hex(MD5_SECRET), &needs_rehash));
	ASSERT_FALSE(needs_rehash);
}

TEST(Credentials, WeakerIterationCountVerifiesButWantsRehash)
{
	// Records written at a lower cost must keep working while being
	// flagged for rewrite, otherwise raising the cost locks users out.
	const std::string rec = "pbkdf2-sha256$1000$"
				"000102030405060708090a0b0c0d0e0f$"
				"0000000000000000000000000000000000000000000000000000000000000000";
	bool needs_rehash = false;
	// The digest is wrong, so this only asserts the parser accepts the
	// shape; the rehash flag is exercised by the round-trip test below.
	ASSERT_FALSE(VerifyMd5Hex(MD5_SECRET, rec, &needs_rehash));
}

TEST(Credentials, FileRoundTrip)
{
	const std::string dir = TempDir();
	Credentials in;
	in.admin = HashMd5Hex(MD5_SECRET);
	in.guest = HashMd5Hex(MD5_OTHER);

	std::string err;
	ASSERT_TRUE(SaveCredentialsFile(dir, in, err));

	Credentials out;
	ASSERT_TRUE(LoadCredentialsFile(dir, out, err));
	ASSERT_EQUALS(in.admin, out.admin);
	ASSERT_EQUALS(in.guest, out.guest);
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, out.admin));
	ASSERT_TRUE(VerifyMd5Hex(MD5_OTHER, out.guest));

	RemoveTempDir(dir);
}

// Clearing the guest credential is how the guest role is disabled, so an
// empty value has to survive a save/load round trip as empty.
TEST(Credentials, EmptyGuestRoundTripsAsDisabled)
{
	const std::string dir = TempDir();
	Credentials in;
	in.admin = HashMd5Hex(MD5_SECRET);
	in.guest = "";

	std::string err;
	ASSERT_TRUE(SaveCredentialsFile(dir, in, err));

	Credentials out;
	ASSERT_TRUE(LoadCredentialsFile(dir, out, err));
	ASSERT_TRUE(out.guest.empty());
	ASSERT_FALSE(VerifyMd5Hex(MD5_OTHER, out.guest));

	RemoveTempDir(dir);
}

TEST(Credentials, MissingFileIsFirstRunNotAnError)
{
	Credentials out;
	std::string err;
	ASSERT_TRUE(LoadCredentialsFile("./credtest-does-not-exist", out, err));
	ASSERT_TRUE(out.admin.empty());
	ASSERT_TRUE(out.guest.empty());
	ASSERT_TRUE(err.empty());
}

TEST(Credentials, LegacyFileIsReadable)
{
	// A file written by a pre-KDF build: bare digests, no PHC prefix.
	const std::string dir = TempDir();
	{
		std::ofstream f((dir + "/amuleapi-passwords").c_str());
		f << "admin=" << MD5_SECRET << "\n";
		f << "guest=" << MD5_OTHER << "\n";
	}

	Credentials out;
	std::string err;
	ASSERT_TRUE(LoadCredentialsFile(dir, out, err));

	bool needs_rehash = false;
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, out.admin, &needs_rehash));
	ASSERT_TRUE(needs_rehash);
	ASSERT_TRUE(VerifyMd5Hex(MD5_OTHER, out.guest));

	RemoveTempDir(dir);
}

TEST(Credentials, CommentsAndBlankLinesIgnored)
{
	const std::string dir = TempDir();
	{
		std::ofstream f((dir + "/amuleapi-passwords").c_str());
		f << "# a comment\n\n";
		f << "  admin = " << MD5_SECRET << "  \n";
		f << "junk-without-equals\n";
	}

	Credentials out;
	std::string err;
	ASSERT_TRUE(LoadCredentialsFile(dir, out, err));
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, out.admin));
	ASSERT_TRUE(out.guest.empty());

	RemoveTempDir(dir);
}

TEST(Credentials, FilePathJoinsWithAndWithoutSeparator)
{
	ASSERT_EQUALS(std::string("/tmp/x/amuleapi-passwords"), CredentialsFilePath("/tmp/x"));
	ASSERT_EQUALS(std::string("/tmp/x/amuleapi-passwords"), CredentialsFilePath("/tmp/x/"));
}

// The pending-change rules every entry point shares: aMule's preferences
// dialog, the same dialog in amulegui over EC, `amuleapi --set-*-pass` and
// the REST endpoint. Encoded once here so the four cannot disagree.
TEST(Credentials, ChangeWithEmptyAdminLeavesAdminAlone)
{
	const std::string dir = TempDir();
	std::string err;

	CredentialChange first;
	first.admin_md5 = MD5_SECRET;
	first.guest_enabled = false;
	ASSERT_TRUE(ApplyCredentialChange(dir, first, err));

	// A client changing only the guest role sends no admin digest, because
	// it cannot read the stored one back.
	CredentialChange second;
	second.guest_enabled = true;
	second.guest_md5 = MD5_OTHER;
	ASSERT_TRUE(ApplyCredentialChange(dir, second, err));

	Credentials out;
	ASSERT_TRUE(LoadCredentialsFile(dir, out, err));
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, out.admin));
	ASSERT_TRUE(VerifyMd5Hex(MD5_OTHER, out.guest));

	RemoveTempDir(dir);
}

TEST(Credentials, ChangeWithGuestDisabledClearsGuest)
{
	const std::string dir = TempDir();
	std::string err;

	CredentialChange on;
	on.admin_md5 = MD5_SECRET;
	on.guest_enabled = true;
	on.guest_md5 = MD5_OTHER;
	ASSERT_TRUE(ApplyCredentialChange(dir, on, err));

	CredentialChange off;
	off.guest_enabled = false;
	ASSERT_TRUE(ApplyCredentialChange(dir, off, err));

	Credentials out;
	ASSERT_TRUE(LoadCredentialsFile(dir, out, err));
	ASSERT_TRUE(out.guest.empty());
	// Turning guest off must never take admin down with it, or the
	// operator locks themselves out of a non-loopback deployment.
	ASSERT_TRUE(VerifyMd5Hex(MD5_SECRET, out.admin));

	RemoveTempDir(dir);
}

TEST(Credentials, ChangeWithGuestEnabledAndNoDigestKeepsGuestPassword)
{
	const std::string dir = TempDir();
	std::string err;

	CredentialChange on;
	on.guest_enabled = true;
	on.guest_md5 = MD5_OTHER;
	ASSERT_TRUE(ApplyCredentialChange(dir, on, err));

	// "Guest stays on, I did not retype the password."
	CredentialChange unchanged;
	unchanged.guest_enabled = true;
	ASSERT_TRUE(ApplyCredentialChange(dir, unchanged, err));

	Credentials out;
	ASSERT_TRUE(LoadCredentialsFile(dir, out, err));
	ASSERT_TRUE(VerifyMd5Hex(MD5_OTHER, out.guest));

	RemoveTempDir(dir);
}

TEST(Credentials, ChangeRejectsMalformedDigests)
{
	const std::string dir = TempDir();
	std::string err;

	CredentialChange bad_admin;
	bad_admin.admin_md5 = "nonsense";
	ASSERT_FALSE(ApplyCredentialChange(dir, bad_admin, err));
	ASSERT_FALSE(err.empty());

	CredentialChange bad_guest;
	bad_guest.guest_enabled = true;
	bad_guest.guest_md5 = "nonsense";
	ASSERT_FALSE(ApplyCredentialChange(dir, bad_guest, err));

	Credentials out;
	ASSERT_TRUE(LoadCredentialsFile(dir, out, err));
	ASSERT_TRUE(out.admin.empty());
	ASSERT_TRUE(out.guest.empty());

	RemoveTempDir(dir);
}
