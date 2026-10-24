//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include "FileAutoClose.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

// GetTickCount.cpp brings the timer thread; the auto-close clock is all this needs.
std::atomic<uint32> TheTime{ 0 };

using namespace muleunit;

namespace
{
const CPath &TestFile()
{
	static const CPath path("FileAutoCloseTest.dat");
	return path;
}

const CPath &RenamedFile()
{
	static const CPath path("FileAutoCloseTest.renamed");
	return path;
}
} // namespace

DECLARE(CFileAutoClose);
void setUp()
{
	CFile file(TestFile(), CFile::write);
	std::vector<char> data(1000, 'a');
	file.Write(data.data(), data.size());
}

void tearDown()
{
	if (TestFile().FileExists()) {
		CPath::RemoveFile(TestFile());
	}
	if (RenamedFile().FileExists()) {
		CPath::RemoveFile(RenamedFile());
	}
}
END_DECLARE;

TEST(CFileAutoClose, KnownSizeFollowsWritesAndResizes)
{
	CFileAutoClose file(TestFile(), CFile::read_write);
	ASSERT_EQUALS(1000u, file.GetKnownSize());

	char data[100] = {};
	file.WriteAt(data, 5000, sizeof(data));
	ASSERT_EQUALS(5100u, file.GetKnownSize());
	// A write inside the file does not move it.
	file.WriteAt(data, 0, sizeof(data));
	ASSERT_EQUALS(5100u, file.GetKnownSize());

	ASSERT_TRUE(file.SetLength(2000));
	ASSERT_EQUALS(2000u, file.GetKnownSize());

	// Auto-closed and reopened by the next write: the size carries over.
	ASSERT_TRUE(file.Release(true));
	ASSERT_EQUALS(2000u, file.GetKnownSize());
	file.WriteAt(data, 3000, 10);
	ASSERT_EQUALS(3010u, file.GetKnownSize());

	ASSERT_EQUALS(file.GetLength(), file.GetKnownSize());
	ASSERT_TRUE(file.Close());
}

TEST(CFileAutoClose, AWriteIsVisibleToAnotherHandleOnceItReturns)
{
	CFileAutoClose file(TestFile(), CFile::read_write);
	const char tail[] = "download-tail";
	file.WriteAt(tail, 1000, sizeof(tail));

	// The completion hash reads the part file through its own handle.
	CFile reader(TestFile(), CFile::read);
	ASSERT_EQUALS(1000u + sizeof(tail), reader.GetLength());
	char got[sizeof(tail)] = {};
	reader.Seek(1000);
	reader.Read(got, sizeof(got));
	ASSERT_TRUE(std::memcmp(tail, got, sizeof(tail)) == 0);

	// Still unbuffered once auto-closed and reopened by the next write.
	ASSERT_TRUE(file.Release(true));
	file.WriteAt(tail, 2000, sizeof(tail));
	CFile reopened(TestFile(), CFile::read);
	ASSERT_EQUALS(2000u + sizeof(tail), reopened.GetLength());
	ASSERT_TRUE(file.Close());
}

TEST(CFileAutoClose, KnownSizeOnlyGrowsWhileAWriterExtends)
{
	CFileAutoClose file(TestFile(), CFile::read_write);
	const uint64 blocks = 2000;
	const size_t blockSize = 4096;

	std::atomic<bool> done{ false };
	std::thread writer([&file, &done, blocks, blockSize] {
		std::vector<char> data(blockSize, 'b');
		for (uint64 i = 0; i < blocks; ++i) {
			file.WriteAt(data.data(), i * blockSize, blockSize);
		}
		done = true;
	});
	uint64 last = 0;
	bool monotonic = true;
	while (!done) {
		const uint64 now = file.GetKnownSize();
		monotonic = monotonic && now >= last;
		last = now;
	}
	writer.join();

	ASSERT_TRUE(monotonic);
	ASSERT_EQUALS(blocks * blockSize, file.GetKnownSize());
	ASSERT_EQUALS(file.GetLength(), file.GetKnownSize());
	ASSERT_TRUE(file.Close());
}

TEST(CFileAutoClose, ASharedDeleteReadHandleLetsTheFileMove)
{
	// The upload thread keeps such a handle open between blocks.
	CFileAutoClose file;
	ASSERT_TRUE(file.Open(TestFile(), CFile::read, true));

	ASSERT_TRUE(CPath::RenameFile(TestFile(), RenamedFile()));
	// wxRenameFile falls back to a copy when the rename fails, so check the original is gone.
	ASSERT_FALSE(TestFile().FileExists());
	char got[10] = {};
	file.ReadAt(got, 0, sizeof(got));
	ASSERT_EQUALS('a', got[0]);

	ASSERT_TRUE(CPath::RemoveFile(RenamedFile()));
	ASSERT_TRUE(file.Close());
}

#ifndef __WINDOWS__
TEST(CFileAutoClose, DescriptorIdentitySurvivesRenameAndAutoCloseWithoutProbingThePath)
{
	CFileAutoClose file;
	ASSERT_TRUE(file.Open(TestFile(), CFile::read, true));
	const auto identity = file.GetKnownStorageIdentity();
	ASSERT_FALSE(identity.empty());
	ASSERT_TRUE(CPath::RenameFile(TestFile(), RenamedFile()));
	ASSERT_EQUALS(identity, file.GetKnownStorageIdentity());
	ASSERT_TRUE(file.Release(true));
	// The old path is gone and the descriptor is closed. The cached getter
	// must neither probe it nor force a reopen to return this last identity.
	ASSERT_EQUALS(identity, file.GetKnownStorageIdentity());
}
#endif
