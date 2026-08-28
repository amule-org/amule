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

#include <muleunit/test.h>

#include "AmuleApiConfig.h"

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace muleunit;

DECLARE(AmuleApiConfig)
// Fresh per-test config dir under the system temp tree. Tearing
// down inside the test bodies avoids muleunit's lack of a
// TearDown hook in the DECLARE_SIMPLE style — each test owns its
// own dir. wxStandardPaths::GetTempDir() returns `/tmp` on
// POSIX and `%TEMP%` on Windows (typically `C:\\Users\\<user>\\
	// AppData\\Local\\Temp`), so the test is portable across the CI
// matrix.
wxString MakeTmpDir(const char *tag)
{
	wxString d;
	d.Printf("%s/amuleapi-cfg-test-%s-%ld",
		wxStandardPaths::Get().GetTempDir(),
		tag,
		static_cast<long>(::wxGetProcessId()));
	// Best-effort cleanup of any stragglers from a prior crashed run.
	wxString secret = d + "/amuleapi-jwt-secret";
	wxString pwfile = d + "/amuleapi-passwords";
	wxString conf = d + "/amuleapi.conf";
	::wxRemoveFile(secret);
	::wxRemoveFile(pwfile);
	::wxRemoveFile(conf);
	::wxRmdir(d);
	return d;
}
END_DECLARE;

TEST(AmuleApiConfig, FreshLoadCreatesAllThreeFiles)
{
	const wxString dir = MakeTmpDir("fresh");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));

	ASSERT_TRUE(::wxFileExists(dir + "/amuleapi.conf"));
	ASSERT_TRUE(::wxFileExists(dir + "/amuleapi-jwt-secret"));
	ASSERT_TRUE(::wxFileExists(dir + "/amuleapi-passwords"));
}

TEST(AmuleApiConfig, FreshLoadProducesStreamingDefaults)
{
	const wxString dir = MakeTmpDir("stream-defaults");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));
	ASSERT_EQUALS(static_cast<unsigned>(16384), cfg.StreamingCfg().event_bus_ring_capacity);
	ASSERT_EQUALS(static_cast<unsigned>(6), cfg.StreamingCfg().max_concurrent_file_responses);
}

// Hand-writes an amuleapi.conf carrying one `[Streaming]` line, at the 0600
// Load() insists on, and reports what the parser made of the file-response cap.
// The dir is created first so the first-run path can't get in ahead and write
// the defaults we are trying to override.
unsigned LoadedFileResponseCap(const char *tag, const char *streaming_line)
{
	// Not MakeTmpDir: that one is a member of the DECLARE block and these
	// helpers live outside it. Same naming scheme, same straggler cleanup.
	wxString dir;
	dir.Printf("%s/amuleapi-cfg-test-%s-%ld",
		wxStandardPaths::Get().GetTempDir(),
		tag,
		static_cast<long>(::wxGetProcessId()));
	::wxRemoveFile(dir + "/amuleapi-jwt-secret");
	::wxRemoveFile(dir + "/amuleapi-passwords");
	::wxRemoveFile(dir + "/amuleapi.conf");
	::wxRmdir(dir);
	::wxMkdir(dir, 0700);

	const std::string conf = std::string("[Server]\nBindAddress=127.0.0.1\nPort=4713\n"
					     "\n[Streaming]\n") +
				 streaming_line + "\n";
	wxFile f(dir + "/amuleapi.conf", wxFile::write);
	f.Write(conf.c_str(), conf.size());
	f.Close();
#ifndef _WIN32
	::chmod(std::string((dir + "/amuleapi.conf").utf8_str()).c_str(), S_IRUSR | S_IWUSR);
#endif

	CAmuleApiConfig cfg;
	if (!cfg.Load(dir)) {
		return 0;
	}
	return cfg.StreamingCfg().max_concurrent_file_responses;
}

TEST(AmuleApiConfig, FileResponseCapIsConfigurable)
{
	// The point of the whole option: 6 is a default, not a ceiling. A NAS
	// with several devices behind one proxy address raises it; a Pi serving
	// off the disk it downloads to lowers it.
	ASSERT_EQUALS(
		static_cast<unsigned>(24), LoadedFileResponseCap("cap-24", "MaxConcurrentFileResponses=24"));
	ASSERT_EQUALS(
		static_cast<unsigned>(1), LoadedFileResponseCap("cap-1", "MaxConcurrentFileResponses=1"));
}

TEST(AmuleApiConfig, InvalidFileResponseCapFallsBackToDefault)
{
	// Every one of these has to land on 6 rather than on itself. Zero and a
	// negative would close the route outright; the four-digit value would
	// trade away the bounded-RSS property the streaming path exists for;
	// the non-numeric one is what a comment or a stray unit looks like to
	// wxFileConfig.
	ASSERT_EQUALS(
		static_cast<unsigned>(6), LoadedFileResponseCap("cap-zero", "MaxConcurrentFileResponses=0"));
	ASSERT_EQUALS(
		static_cast<unsigned>(6), LoadedFileResponseCap("cap-neg", "MaxConcurrentFileResponses=-4"));
	ASSERT_EQUALS(static_cast<unsigned>(6),
		LoadedFileResponseCap("cap-huge", "MaxConcurrentFileResponses=100000"));
	ASSERT_EQUALS(static_cast<unsigned>(6),
		LoadedFileResponseCap("cap-junk", "MaxConcurrentFileResponses=lots"));
}

TEST(AmuleApiConfig, GeneratedJwtSecretIs32Bytes)
{
	const wxString dir = MakeTmpDir("jwt32");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));
	ASSERT_EQUALS(static_cast<size_t>(32), cfg.JwtSecret().size());
}

TEST(AmuleApiConfig, GeneratedJwtSecretIsRandom)
{
	const wxString dir_a = MakeTmpDir("jwt-a");
	CAmuleApiConfig cfg_a;
	ASSERT_TRUE(cfg_a.Load(dir_a));
	const std::vector<unsigned char> a = cfg_a.JwtSecret();

	const wxString dir_b = MakeTmpDir("jwt-b");
	CAmuleApiConfig cfg_b;
	ASSERT_TRUE(cfg_b.Load(dir_b));
	const std::vector<unsigned char> b = cfg_b.JwtSecret();

	// Two fresh dirs → two distinct secrets. ~2^256 collision odds.
	ASSERT_TRUE(a != b);
}

TEST(AmuleApiConfig, JwtSecretRoundTripStable)
{
	const wxString dir = MakeTmpDir("jwt-rt");

	CAmuleApiConfig cfg1;
	ASSERT_TRUE(cfg1.Load(dir));
	const std::vector<unsigned char> first = cfg1.JwtSecret();

	CAmuleApiConfig cfg2;
	ASSERT_TRUE(cfg2.Load(dir));
	const std::vector<unsigned char> second = cfg2.JwtSecret();

	// Second load reads what the first generated — same bytes.
	ASSERT_TRUE(first == second);
}

TEST(AmuleApiConfig, EmptyPasswordsFilePassesLoad)
{
	const wxString dir = MakeTmpDir("pw-empty");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));
	// Default state: both roles disabled until a password is set.
	ASSERT_TRUE(cfg.AdminCredential().empty());
	ASSERT_TRUE(cfg.GuestCredential().empty());
	ASSERT_FALSE(cfg.HasAnyCredential());
}

TEST(AmuleApiConfig, SetPasswordIsReloadableAndVerifies)
{
	const wxString dir = MakeTmpDir("pw-rt");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));

	// 32 hex chars; doesn't need to be a real MD5.
	const std::string admin_md5 = "0123456789abcdef0123456789abcdef";
	const std::string guest_md5 = "fedcba9876543210fedcba9876543210";
	ASSERT_TRUE(cfg.SetPassword(webcommon::kAdminCredential, admin_md5));
	ASSERT_TRUE(cfg.SetPassword(webcommon::kGuestCredential, guest_md5));

	CAmuleApiConfig cfg2;
	ASSERT_TRUE(cfg2.Load(dir));
	ASSERT_TRUE(cfg2.HasAnyCredential());
	// The stored form is a salted record, not the digest that produced it.
	ASSERT_TRUE(cfg2.AdminCredential() != admin_md5);
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Admin == cfg2.VerifyPassword(admin_md5));
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Guest == cfg2.VerifyPassword(guest_md5));
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::None ==
		    cfg2.VerifyPassword("00000000000000000000000000000000"));
}

// Each role is set independently. The write re-reads the file first, so
// changing one role never reverts a change another process made to the
// other one — that is the whole reason there is a single store.
TEST(AmuleApiConfig, SetPasswordLeavesTheOtherRoleAlone)
{
	const wxString dir = MakeTmpDir("pw-independent");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));

	const std::string admin_md5 = "0123456789abcdef0123456789abcdef";
	const std::string guest_md5 = "fedcba9876543210fedcba9876543210";
	ASSERT_TRUE(cfg.SetPassword(webcommon::kAdminCredential, admin_md5));

	// A second process (amuled applying an EC push, say) sets the guest
	// password without ever having seen the admin one.
	CAmuleApiConfig other;
	ASSERT_TRUE(other.Load(dir));
	ASSERT_TRUE(other.SetPassword(webcommon::kGuestCredential, guest_md5));

	CAmuleApiConfig cfg3;
	ASSERT_TRUE(cfg3.Load(dir));
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Admin == cfg3.VerifyPassword(admin_md5));
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Guest == cfg3.VerifyPassword(guest_md5));
}

// Clearing the guest credential is how guest access is turned off.
TEST(AmuleApiConfig, EmptyDigestClearsTheRole)
{
	const wxString dir = MakeTmpDir("pw-clear");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));

	const std::string admin_md5 = "0123456789abcdef0123456789abcdef";
	const std::string guest_md5 = "fedcba9876543210fedcba9876543210";
	ASSERT_TRUE(cfg.SetPassword(webcommon::kAdminCredential, admin_md5));
	ASSERT_TRUE(cfg.SetPassword(webcommon::kGuestCredential, guest_md5));

	ASSERT_TRUE(cfg.SetPassword(webcommon::kGuestCredential, ""));
	ASSERT_TRUE(cfg.GuestCredential().empty());
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::None == cfg.VerifyPassword(guest_md5));
	// Admin is untouched, so clearing guest doesn't lock the operator out.
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Admin == cfg.VerifyPassword(admin_md5));
	ASSERT_TRUE(cfg.HasAnyCredential());
}

TEST(AmuleApiConfig, SetPasswordRejectsMalformedDigest)
{
	const wxString dir = MakeTmpDir("pw-setreject");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));

	ASSERT_FALSE(cfg.SetPassword(webcommon::kAdminCredential, "too-short"));
	ASSERT_FALSE(cfg.SetPassword(webcommon::kAdminCredential, "zzzz56789abcdef0123456789abcdef0"));
	ASSERT_TRUE(cfg.AdminCredential().empty());

	// Uppercase is accepted: the preferences dialog and the EC path have
	// historically disagreed about digest case.
	ASSERT_TRUE(cfg.SetPassword(webcommon::kAdminCredential, "0123456789ABCDEF0123456789ABCDEF"));
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Admin ==
		    cfg.VerifyPassword("0123456789abcdef0123456789abcdef"));
}

// A password set by amuled or by aMule's preferences dialog while
// amuleapi is running has to take effect without restarting amuleapi.
TEST(AmuleApiConfig, PasswordChangedOnDiskIsPickedUpAtNextLogin)
{
	const wxString dir = MakeTmpDir("pw-reload");
	CAmuleApiConfig running;
	ASSERT_TRUE(running.Load(dir));

	const std::string first = "0123456789abcdef0123456789abcdef";
	ASSERT_TRUE(running.SetPassword(webcommon::kAdminCredential, first));

	// Another process rotates the admin password.
	const std::string second = "fedcba9876543210fedcba9876543210";
	{
		CAmuleApiConfig other;
		ASSERT_TRUE(other.Load(dir));
		ASSERT_TRUE(other.SetPassword(webcommon::kAdminCredential, second));
	}

	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Admin == running.VerifyPassword(second));
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::None == running.VerifyPassword(first));
}

// A config written before the KDF holds a bare MD5. It must still let its
// owner in, and the record must be rewritten at the current cost so the
// upgrade needs no operator step.
TEST(AmuleApiConfig, LegacyBareMd5IsUpgradedOnSuccessfulLogin)
{
	const wxString dir = MakeTmpDir("pw-legacy");
	const std::string legacy = "0123456789abcdef0123456789abcdef";
	::wxMkdir(dir, 0700);
	{
		wxFile f(dir + "/amuleapi-passwords", wxFile::write);
		const std::string line = "admin=" + legacy + "\n";
		f.Write(line.data(), line.size());
	}
#ifndef _WIN32
	::chmod(std::string((dir + "/amuleapi-passwords").utf8_str()).c_str(), S_IRUSR | S_IWUSR);
#endif

	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));
	ASSERT_EQUALS(legacy, cfg.AdminCredential());

	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Admin == cfg.VerifyPassword(legacy));
	// Upgraded in place: no longer the bare digest, and still verifies.
	ASSERT_TRUE(cfg.AdminCredential() != legacy);
	ASSERT_FALSE(webcommon::IsLegacyMd5Record(cfg.AdminCredential()));

	CAmuleApiConfig cfg2;
	ASSERT_TRUE(cfg2.Load(dir));
	ASSERT_TRUE(CAmuleApiConfig::MatchedRole::Admin == cfg2.VerifyPassword(legacy));
}

TEST(AmuleApiConfig, MalformedPasswordLineRejected)
{
	const wxString dir = MakeTmpDir("pw-bad");
	// Hand-create the dir + a bad passwords file BEFORE Load() runs,
	// otherwise the auto-create path writes a fresh empty file and
	// we never exercise the parser failure path.
	::wxMkdir(dir, 0700);
	wxFile bad(dir + "/amuleapi-passwords", wxFile::write);
	const char *bad_line = "admin=not_a_valid_record\n";
	bad.Write(bad_line, std::strlen(bad_line));
	bad.Close();
#ifndef _WIN32
	::chmod(std::string((dir + "/amuleapi-passwords").utf8_str()).c_str(), S_IRUSR | S_IWUSR);
#endif

	CAmuleApiConfig cfg;
	ASSERT_FALSE(cfg.Load(dir));
	ASSERT_TRUE(!cfg.LastError().empty());
}

#ifndef _WIN32
// POSIX-only: the production hardening (mode-bit check in
// AmuleApiConfig::EnforceOwnerOnly) is itself POSIX-only. Windows
// uses ACLs rather than POSIX mode bits, and the typical Windows
// daemon footprint (single-operator workstation, %USERPROFILE%-
// scoped config dir) makes the threat model very different. If
// amuleapi ever ships a Windows hardening pass (via GetSecurityInfo
/// GetEffectiveRightsFromAcl on the secret file's DACL), the
// matching test should land under `#ifdef _WIN32` here. Until then,
// the #ifndef intentionally skips the assertion on Windows so the
// test suite stays green there without misrepresenting the
// platform's posture.
TEST(AmuleApiConfig, LooserSecretFilePermissionsRejected)
{
	const wxString dir = MakeTmpDir("perm");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir)); // first load auto-creates with 0600

	// Loosen the secret file to 0644 and verify the next Load fails
	// with an actionable error. This guards the "operator
	// accidentally chmodded the secret world-readable" scenario.
	const std::string path = std::string((dir + "/amuleapi-jwt-secret").utf8_str());
	ASSERT_EQUALS(0, ::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));

	CAmuleApiConfig cfg2;
	ASSERT_FALSE(cfg2.Load(dir));
	ASSERT_TRUE(cfg2.LastError().find("0600") != std::string::npos);
}
#endif

TEST(AmuleApiConfig, ConfDefaultsArePopulated)
{
	const wxString dir = MakeTmpDir("conf-defaults");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));

	ASSERT_EQUALS(std::string("127.0.0.1"), cfg.ServerCfg().bind_address);
	ASSERT_EQUALS(static_cast<unsigned>(4713), cfg.ServerCfg().port);
	ASSERT_EQUALS(std::string("127.0.0.1"), cfg.EcCfg().host);
	ASSERT_EQUALS(static_cast<unsigned>(4712), cfg.EcCfg().port);
	ASSERT_TRUE(!cfg.ServerCfg().allow_cors);
}
