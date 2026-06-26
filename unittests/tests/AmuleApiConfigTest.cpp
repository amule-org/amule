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

TEST(AmuleApiConfig, DefaultConfigDirIsNonEmpty)
{
	const wxString d = DefaultConfigDir();
	ASSERT_TRUE(!d.IsEmpty());
}

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
	// Default state: both roles disabled until --set-*-pass writes a line.
	ASSERT_TRUE(cfg.AdminPasswordMd5().empty());
	ASSERT_TRUE(cfg.GuestPasswordMd5().empty());
}

TEST(AmuleApiConfig, WritePasswordsFileReloadable)
{
	const wxString dir = MakeTmpDir("pw-rt");
	CAmuleApiConfig cfg;
	ASSERT_TRUE(cfg.Load(dir));

	// 32 lowercase hex chars; doesn't need to be a real MD5.
	const std::string admin_md5 = "0123456789abcdef0123456789abcdef";
	const std::string guest_md5 = "fedcba9876543210fedcba9876543210";
	ASSERT_TRUE(cfg.WritePasswordsFile(dir, admin_md5, guest_md5));

	CAmuleApiConfig cfg2;
	ASSERT_TRUE(cfg2.Load(dir));
	ASSERT_EQUALS(admin_md5, cfg2.AdminPasswordMd5());
	ASSERT_EQUALS(guest_md5, cfg2.GuestPasswordMd5());
}

TEST(AmuleApiConfig, MalformedPasswordLineRejected)
{
	const wxString dir = MakeTmpDir("pw-bad");
	// Hand-create the dir + a bad passwords file BEFORE Load() runs,
	// otherwise the auto-create path writes a fresh empty file and
	// we never exercise the parser failure path.
	::wxMkdir(dir, 0700);
	wxFile bad(dir + "/amuleapi-passwords", wxFile::write);
	const char *bad_line = "admin=not_a_valid_md5\n";
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
