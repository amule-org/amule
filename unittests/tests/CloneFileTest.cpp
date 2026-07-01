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
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
//

#include <muleunit/test.h>
#include <CFile.h>
#include <common/Path.h>
#include <wx/filename.h>
#include <vector>
#include <cstdlib> // getenv / atoi for the opt-in huge-file test
#include <cstring> // memcmp

using namespace muleunit;

namespace muleunit
{
//! Needed for ASSERT_* diagnostics involving CPath
template <> wxString StringFrom<CPath>(const CPath &path)
{
	return path.GetPrintable();
}
} // namespace muleunit

// Returns a path to a fresh, non-existent temp file. CreateTempFileName
// creates the file empty, so remove it to hand back a clean slate.
static CPath FreshTemp()
{
	CPath path(wxFileName::CreateTempFileName(wxT("amuleclone_")));
	CPath::RemoveFile(path);
	return path;
}

// Writes `length` bytes of deterministic, position-dependent data.
static void WriteData(const CPath &path, size_t length)
{
	CFile file;
	ASSERT_TRUE(file.Open(path, CFile::write));
	std::vector<char> buffer(length);
	for (size_t i = 0; i < length; ++i) {
		buffer[i] = static_cast<char>((i * 31 + 7) & 0xff);
	}
	if (length) {
		file.Write(buffer.data(), length);
	}
	file.Close();
}

static std::vector<char> ReadAll(const CPath &path)
{
	CFile file;
	if (!file.Open(path, CFile::read)) {
		return std::vector<char>();
	}
	const uint64 length = file.GetLength();
	std::vector<char> buffer(static_cast<size_t>(length));
	if (length) {
		file.Read(buffer.data(), static_cast<size_t>(length));
	}
	file.Close();
	return buffer;
}

static uint64 FileLength(const CPath &path)
{
	CFile file;
	if (!file.Open(path, CFile::read)) {
		return 0;
	}
	const uint64 length = file.GetLength();
	file.Close();
	return length;
}

// Writes `length` bytes of deterministic, position-dependent data in 1 MiB
// blocks. Chunked (rather than a single buffer) so the huge-file test never
// holds multiple GiB in memory. The pattern depends on absolute offset, so
// the tail differs from the head — a high-offset copy bug can't hide.
static void WriteDataChunked(const CPath &path, uint64 length)
{
	CFile file;
	ASSERT_TRUE(file.Open(path, CFile::write));
	const size_t block = 1u << 20;
	std::vector<char> buffer(block);
	uint64 written = 0;
	while (written < length) {
		const size_t chunk =
			(length - written < block) ? static_cast<size_t>(length - written) : block;
		for (size_t i = 0; i < chunk; ++i) {
			buffer[i] = static_cast<char>(((written + i) * 31 + 7) & 0xff);
		}
		file.Write(buffer.data(), chunk);
		written += chunk;
	}
	file.Close();
}

// Compares two files by streaming 1 MiB blocks, so it works on multi-GiB
// files without loading them into memory.
static bool SameContentStreaming(const CPath &a, const CPath &b)
{
	CFile fa, fb;
	if (!fa.Open(a, CFile::read) || !fb.Open(b, CFile::read)) {
		return false;
	}
	if (fa.GetLength() != fb.GetLength()) {
		return false;
	}
	uint64 remaining = fa.GetLength();
	const size_t block = 1u << 20;
	std::vector<char> ba(block), bb(block);
	while (remaining) {
		const size_t chunk = (remaining < block) ? static_cast<size_t>(remaining) : block;
		fa.Read(ba.data(), chunk);
		fb.Read(bb.data(), chunk);
		if (memcmp(ba.data(), bb.data(), chunk) != 0) {
			return false;
		}
		remaining -= chunk;
	}
	return true;
}

// A size larger than the internal copy buffer, deliberately not a round
// multiple of it, so the copy loop runs several full iterations plus a
// short trailing chunk.
static const size_t MULTI_CHUNK = 3 * 1024 * 1024 + 123;

DECLARE_SIMPLE(CFileCloneFile)

TEST(CFileCloneFile, ByteIdenticalMultiChunk)
{
	CPath src = FreshTemp();
	WriteData(src, MULTI_CHUNK);
	CPath dst = FreshTemp();

	ASSERT_TRUE(CFile::CloneFile(src, dst, false));
	ASSERT_TRUE(dst.FileExists());
	ASSERT_TRUE(SameContentStreaming(src, dst));

	CPath::RemoveFile(src);
	CPath::RemoveFile(dst);
}

TEST(CFileCloneFile, EmptyFile)
{
	CPath src = FreshTemp();
	WriteData(src, 0);
	CPath dst = FreshTemp();

	ASSERT_TRUE(CFile::CloneFile(src, dst, false));
	ASSERT_TRUE(dst.FileExists());
	ASSERT_TRUE(ReadAll(dst).empty());

	CPath::RemoveFile(src);
	CPath::RemoveFile(dst);
}

TEST(CFileCloneFile, NoOverwriteFailsAndPreservesDst)
{
	CPath src = FreshTemp();
	WriteData(src, 4096);
	CPath dst = FreshTemp();
	WriteData(dst, 10); // distinct existing content
	const std::vector<char> before = ReadAll(dst);

	ASSERT_FALSE(CFile::CloneFile(src, dst, false));
	ASSERT_TRUE(ReadAll(dst) == before); // untouched

	CPath::RemoveFile(src);
	CPath::RemoveFile(dst);
}

TEST(CFileCloneFile, OverwriteReplaces)
{
	CPath src = FreshTemp();
	WriteData(src, 4096);
	CPath dst = FreshTemp();
	WriteData(dst, 10);

	ASSERT_TRUE(CFile::CloneFile(src, dst, true));
	ASSERT_TRUE(SameContentStreaming(src, dst));

	CPath::RemoveFile(src);
	CPath::RemoveFile(dst);
}

TEST(CFileCloneFile, MissingSourceFailsAndCreatesNoDst)
{
	CPath src = FreshTemp(); // never written -> does not exist
	CPath dst = FreshTemp();

	ASSERT_FALSE(CFile::CloneFile(src, dst, true));
	ASSERT_FALSE(dst.FileExists()); // no partial destination left behind

	CPath::RemoveFile(dst);
}

TEST(CFileCloneFile, UnwritableDestFails)
{
	CPath src = FreshTemp();
	WriteData(src, 4096);
	// Parent directory does not exist, so opening the destination fails.
	CPath dst(wxT("amuleclone_missing_dir_xyz/dst.tmp"));

	ASSERT_FALSE(CFile::CloneFile(src, dst, true));

	CPath::RemoveFile(src);
}

// CPath::BackupFile is the small same-filesystem copy path that
// ClientCreditsList's clients.met backup now uses. Verify it produces a
// byte-identical copy at src + appendix and overwrites an existing backup.
TEST(CFileCloneFile, BackupFileCreatesIdenticalCopy)
{
	const wxString base = wxFileName::CreateTempFileName(wxT("amuleclone_"));
	CPath src(base);
	CPath::RemoveFile(src);
	WriteData(src, MULTI_CHUNK);

	ASSERT_TRUE(CPath::BackupFile(src, wxT(".bak")));
	CPath bak(base + wxT(".bak"));
	ASSERT_TRUE(bak.FileExists());
	ASSERT_TRUE(SameContentStreaming(src, bak));

	// A second call overwrites the existing backup and stays identical.
	ASSERT_TRUE(CPath::BackupFile(src, wxT(".bak")));
	ASSERT_TRUE(SameContentStreaming(src, bak));

	CPath::RemoveFile(src);
	CPath::RemoveFile(bak);
}

// Opt-in: exercises a copy past the 4 GiB / 32-bit-offset boundary, which
// would catch any length or offset truncation. Skipped unless
// AMULE_CLONEFILE_HUGE_GB is set (writing multiple GiB is far too heavy for
// the normal / CI test run). Example: AMULE_CLONEFILE_HUGE_GB=5 ./CloneFileTest
TEST(CFileCloneFile, HugeFileOptIn)
{
	const char *env = getenv("AMULE_CLONEFILE_HUGE_GB");
	if (!env) {
		return; // not requested -> no-op pass
	}
	const uint64 gb = static_cast<uint64>(atoi(env));
	if (!gb) {
		return;
	}
	// gb GiB, plus a non-block-aligned tail so the final short chunk and a
	// high absolute offset are both covered.
	const uint64 length = gb * 1024ull * 1024ull * 1024ull + 4096ull;

	CPath src = FreshTemp();
	WriteDataChunked(src, length);
	CPath dst = FreshTemp();

	ASSERT_TRUE(CFile::CloneFile(src, dst, false));
	ASSERT_TRUE(dst.FileExists());
	// Length equality alone catches a 32-bit truncation (a truncated copy
	// would land at length mod 4 GiB); streaming compare catches offset bugs.
	ASSERT_TRUE(FileLength(dst) == length);
	ASSERT_TRUE(SameContentStreaming(src, dst));

	CPath::RemoveFile(src);
	CPath::RemoveFile(dst);
}
