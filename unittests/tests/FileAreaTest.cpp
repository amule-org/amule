// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include "config.h"
#include <muleunit/test.h>
#include "FileArea.h"
#include "FileAutoClose.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include "WriteBufferBudget.h"
#ifdef AMULE_TEST_WRAP_MSYNC
#include <sys/uio.h>
namespace
{
unsigned vectorCalls = 0;
bool shortVectorWrite = false;
} // namespace
extern "C" ssize_t __real_writev(int fd, const iovec *iov, int count);
extern "C" ssize_t __wrap_writev(int fd, const iovec *iov, int count)
{
	++vectorCalls;
	if (shortVectorWrite) {
		iovec first{ iov[0].iov_base, iov[0].iov_len / 2 };
		return __real_writev(fd, &first, 1);
	}
	return __real_writev(fd, iov, count);
}
#endif

std::atomic<uint32> TheTime{ 0 };

#if defined(AMULE_TEST_WRAP_MSYNC) && defined(MMAP_SUPPORTED)
#include <unistd.h>

namespace
{
bool failSync = false;
unsigned syncCalls = 0;
int truncateOnFailure = -1;
} // namespace
extern "C" int __real_msync(void *address, size_t length, int flags);
extern "C" int __wrap_msync(void *address, size_t length, int flags)
{
	++syncCalls;
	if (failSync) {
		// Model writeback that cleaned the pages before reporting an error. A
		// later msync alone must not be enough to complete the pending write.
		if (__real_msync(address, length, flags) != 0) {
			return -1;
		}
		// Model a device whose pages are gone: reading the mapping now raises SIGBUS.
		if (truncateOnFailure >= 0 && ftruncate(truncateOnFailure, 0) != 0) {
			return -1;
		}
		errno = EIO;
		return -1;
	}
	return __real_msync(address, length, flags);
}
#endif

using namespace muleunit;
namespace
{
const CPath &TestFile()
{
	static const CPath path("FileAreaTest.dat");
	return path;
}
} // namespace

DECLARE(FileArea);
void setUp()
{
	CFileArea::SetMMapEnabled(false);
	CFile file(TestFile(), CFile::write);
	std::array<char, 8192> data{};
	file.Write(data.data(), data.size());
}
void tearDown()
{
#ifdef AMULE_TEST_WRAP_MSYNC
	shortVectorWrite = false;
#endif
#if defined(AMULE_TEST_WRAP_MSYNC) && defined(MMAP_SUPPORTED)
	failSync = false;
	truncateOnFailure = -1;
#endif
	CFileArea::SetMMapEnabled(false);
	CPath::RemoveFile(TestFile());
}
END_DECLARE;

TEST(FileArea, BufferedWriteRoundTrip)
{
	CFileAutoClose file(TestFile(), CFile::read_write);
	CFileArea area;
	area.StartWriteAt(file, 17, 23);
	std::memset(area.GetBuffer(), 0x5A, 23);
	area.FlushAt(file, 17, 23);
	ASSERT_TRUE(area.GetBuffer() == nullptr);
	std::array<unsigned char, 40> data{};
	file.ReadAt(data.data(), 0, data.size());
	for (size_t i = 0; i < data.size(); ++i) {
		ASSERT_EQUALS(i < 17 ? 0 : 0x5A, static_cast<int>(data[i]));
	}
}

TEST(FileArea, BufferedWriteFailureRetainsData)
{
	CFileAutoClose file(TestFile(), CFile::read);
	CFileArea area;
	area.StartWriteAt(file, 0, 23);
	std::memset(area.GetBuffer(), 0x5A, 23);
	ASSERT_RAISES(CIOFailureException, area.FlushAt(file, 0, 23));
	ASSERT_TRUE(area.GetBuffer() != nullptr);
	ASSERT_EQUALS(0x5A, static_cast<int>(area.GetBuffer()[0]));
}

TEST(FileArea, UninitializedAreaCannotReportACompletedWrite)
{
	CFileAutoClose file(TestFile(), CFile::read_write);
	CFileArea area;
	ASSERT_RAISES(CIOFailureException, area.FlushAt(file, 0, 23));
}

#if defined(AMULE_TEST_WRAP_MSYNC) && defined(MMAP_SUPPORTED)
TEST(FileArea, MappedWriteRoundTrip)
{
	CFileArea::SetMMapEnabled(true);
	CFileAutoClose file(TestFile(), CFile::read_write);
	CFileArea area;
	area.StartWriteAt(file, 17, 5000);
	std::memset(area.GetBuffer(), 0x5A, 5000);
	const auto previousCalls = syncCalls;
	area.FlushAt(file, 17, 5000);
	ASSERT_EQUALS(previousCalls + 1, syncCalls);
	ASSERT_TRUE(area.GetBuffer() == nullptr);
	ASSERT_TRUE(file.Release(true));
	std::array<unsigned char, 8192> data{};
	file.ReadAt(data.data(), 0, data.size());
	for (size_t i = 0; i < data.size(); ++i) {
		ASSERT_EQUALS(i >= 17 && i < 5017 ? 0x5A : 0, static_cast<int>(data[i]));
	}
}

TEST(FileArea, MappedSyncFailureRewritesData)
{
	CFileArea::SetMMapEnabled(true);
	CFileAutoClose file(TestFile(), CFile::read_write);
	CFileArea area;
	area.StartWriteAt(file, 17, 5000);
	std::memset(area.GetBuffer(), 0x5A, 5000);
	const auto previousCalls = syncCalls;
	failSync = true;
	ASSERT_RAISES(CIOFailureException, area.FlushAt(file, 17, 5000));
	ASSERT_EQUALS(previousCalls + 1, syncCalls);
	ASSERT_TRUE(area.GetBuffer() != nullptr);
	ASSERT_TRUE(std::all_of(area.GetBuffer(), area.GetBuffer() + 5000, [](unsigned char value) {
		return value == 0x5A;
	}));
	ASSERT_TRUE(file.Release(true));

	// Replace the file contents after the failure. A retained mapping or a
	// successful no-op sync would now lose the original pending bytes.
	std::array<unsigned char, 8192> data{};
	file.WriteAt(data.data(), 0, data.size());
	area.FlushAt(file, 17, 5000);
	ASSERT_EQUALS(previousCalls + 1, syncCalls);
	ASSERT_TRUE(area.GetBuffer() == nullptr);
	file.ReadAt(data.data(), 0, data.size());
	for (size_t i = 0; i < data.size(); ++i) {
		ASSERT_EQUALS(i >= 17 && i < 5017 ? 0x5A : 0, static_cast<int>(data[i]));
	}
}

TEST(FileArea, MappedSyncFailureRetainsDataAcrossFailedBufferedRetry)
{
	CFileArea::SetMMapEnabled(true);
	CFileAutoClose file(TestFile(), CFile::read_write);
	CFileArea area;
	area.StartWriteAt(file, 17, 5000);
	std::memset(area.GetBuffer(), 0x5A, 5000);
	const auto previousCalls = syncCalls;
	failSync = true;
	ASSERT_RAISES(CIOFailureException, area.FlushAt(file, 17, 5000));
	ASSERT_EQUALS(previousCalls + 1, syncCalls);
	CFileAutoClose readOnlyFile(TestFile(), CFile::read);
	ASSERT_RAISES(CIOFailureException, area.FlushAt(readOnlyFile, 17, 5000));
	ASSERT_EQUALS(previousCalls + 1, syncCalls);
	ASSERT_TRUE(area.GetBuffer() != nullptr);
	ASSERT_TRUE(std::all_of(area.GetBuffer(), area.GetBuffer() + 5000, [](unsigned char value) {
		return value == 0x5A;
	}));
	area.FlushAt(file, 17, 5000);
	ASSERT_EQUALS(previousCalls + 1, syncCalls);
	ASSERT_TRUE(area.GetBuffer() == nullptr);
	std::array<unsigned char, 5000> data{};
	file.ReadAt(data.data(), 17, data.size());
	ASSERT_TRUE(std::all_of(data.begin(), data.end(), [](unsigned char value) { return value == 0x5A; }));
}

TEST(FileArea, MappedSyncFailureSurvivesLostPages)
{
	CFileArea::SetMMapEnabled(true);
	CFileAutoClose file(TestFile(), CFile::read_write);
	{
		CFileArea area;
		area.StartWriteAt(file, 17, 5000);
		std::memset(area.GetBuffer(), 0x5A, 5000);
		failSync = true;
		truncateOnFailure = file.fd();
		ASSERT_RAISES(CIOFailureException, area.FlushAt(file, 17, 5000));
		truncateOnFailure = -1;
		ASSERT_TRUE(area.GetBuffer() != nullptr);
		ASSERT_EQUALS(0, static_cast<int>(area.GetBuffer()[0]));
	} // ~CFileArea must not rethrow the SIGBUS from the copy
}
#endif

TEST(FileArea, AdjacentFragmentsUseOneWriteAndRetainOwnershipOnPartialFailure)
{
	CFileAutoClose file(TestFile(), CFile::read_write);
	std::array<CFileArea, 16> areas;
	std::array<CFileArea::WriteRange, 16> ranges{};
	WriteBufferBudget budget;
	std::array<WriteBufferBudget::Credit, 16> credits;
	for (size_t i = 0; i < areas.size(); ++i) {
		areas[i].StartWriteAt(file, 17 + i * 23, 23);
		std::memset(areas[i].GetBuffer(), static_cast<int>(i + 1), 23);
		ranges[i] = { &areas[i], 17 + i * 23, 23 };
		credits[i] = budget.Reserve("test", 23);
	}
#ifdef AMULE_TEST_WRAP_MSYNC
	const auto before = vectorCalls;
	shortVectorWrite = true;
	ASSERT_RAISES(CIOFailureException, CFileArea::FlushAdjacentAt(file, ranges.data(), ranges.size()));
	ASSERT_EQUALS(before + 1, vectorCalls);
	ASSERT_EQUALS(uint64_t{ 368 }, budget.Used());
	for (size_t i = 0; i < areas.size(); ++i) {
		ASSERT_TRUE(areas[i].GetBuffer() != nullptr);
		ASSERT_EQUALS(static_cast<int>(i + 1), static_cast<int>(areas[i].GetBuffer()[22]));
	}
	shortVectorWrite = false;
#endif
	CFileArea::FlushAdjacentAt(file, ranges.data(), ranges.size());
#ifdef AMULE_TEST_WRAP_MSYNC
	ASSERT_EQUALS(before + 2, vectorCalls);
#endif
	std::array<unsigned char, 385> data{};
	file.ReadAt(data.data(), 0, data.size());
	for (size_t i = 0; i < data.size(); ++i) {
		ASSERT_EQUALS(i < 17 ? 0 : static_cast<int>((i - 17) / 23 + 1), static_cast<int>(data[i]));
	}
	for (auto &area : areas) {
		ASSERT_TRUE(area.GetBuffer() == nullptr);
	}
	// Completion harvesting, rather than the I/O helper, owns credit release.
	ASSERT_EQUALS(uint64_t{ 368 }, budget.Used());
	credits.fill(nullptr);
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
}

TEST(FileArea, NonAdjacentGroupRetainsBothBuffers)
{
	CFileAutoClose file(TestFile(), CFile::read_write);
	CFileArea first, second;
	first.StartWriteAt(file, 0, 23);
	second.StartWriteAt(file, 24, 23);
	const CFileArea::WriteRange ranges[] = { { &first, 0, 23 }, { &second, 24, 23 } };
	ASSERT_RAISES(CIOFailureException, CFileArea::FlushAdjacentAt(file, ranges, 2));
	ASSERT_TRUE(first.GetBuffer() != nullptr && second.GetBuffer() != nullptr);
}
