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
//

// What goes on the wire as a part status (OP_FILESTATUS, OP_REASKACK). All three clients checked
// (aMule, eMule, eMuleAI) reject a count other than their GetED2KPartCount(), and a bit-order
// slip would advertise the wrong parts.

#include <muleunit/test.h>

#include <MemFile.h>
#include <PartStatusWriter.h>

#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(PartStatusWriter)

namespace
{
// The bytes written to `file`, from the start.
std::vector<uint8> Bytes(const CMemFile &file)
{
	const uint8 *data = file.GetRawBuffer();
	return std::vector<uint8>(data, data + file.GetLength());
}

// CKnownFile::SetFileSize(): one part more than whole parts, so a file of n * PARTSIZE has n + 1.
uint16 ED2KPartCount(uint64 size)
{
	return (uint16)(size / PARTSIZE + 1);
}
} // namespace

// Count, then part 8k + i in bit i of byte k.
TEST(PartStatusWriter, BitmapIsLeastSignificantBitFirst)
{
	CMemFile file;
	WritePartBitmap(file, 10, [](uint16 part) { return part == 0 || part == 2 || part == 9; });
	ASSERT_TRUE((std::vector<uint8>{ 10, 0, 0x05, 0x02 }) == Bytes(file));
}

// A partial last byte is padded with zeros; a whole one gets no extra byte.
TEST(PartStatusWriter, BitmapHasOneBytePerEightParts)
{
	CMemFile nine;
	WritePartBitmap(nine, 9, [](uint16) { return true; });
	ASSERT_TRUE((std::vector<uint8>{ 9, 0, 0xFF, 0x01 }) == Bytes(nine));

	CMemFile eight;
	WritePartBitmap(eight, 8, [](uint16) { return true; });
	ASSERT_TRUE((std::vector<uint8>{ 8, 0, 0xFF }) == Bytes(eight));

	CMemFile none;
	WritePartBitmap(none, 0, [](uint16) { return true; });
	ASSERT_TRUE((std::vector<uint8>{ 0, 0 }) == Bytes(none));
}

TEST(PartStatusWriter, CleanCompleteFileSendsCountZero)
{
	CMemFile file;
	WriteCompleteFilePartStatus(file, CVerifyLocalDataResult(), ED2KPartCount(3 * PARTSIZE + 1));
	ASSERT_TRUE((std::vector<uint8>{ 0, 0 }) == Bytes(file));
}

// 3 full parts plus a 1-byte fourth: 4 bits, parts 1 and 3 withheld.
TEST(PartStatusWriter, CorruptCompleteFileWithholdsCorruptParts)
{
	const uint64 size = 3 * PARTSIZE + 1;
	CVerifyLocalDataResult result;
	result.DecodeCorrupted("1", "3:0", size);

	CMemFile file;
	WriteCompleteFilePartStatus(file, result, ED2KPartCount(size));
	ASSERT_TRUE((std::vector<uint8>{ 4, 0, 0x05 }) == Bytes(file));
}

// A file of exactly n * PARTSIZE has an extra, empty ed2k part; it must read as present, even
// next to a corrupt last part, or peers would never see the file as having all its parts.
TEST(PartStatusWriter, ExtraPartOfExactMultipleReadsAsPresent)
{
	const uint64 size = 2 * PARTSIZE;
	CVerifyLocalDataResult result;
	result.DecodeCorrupted("1", "", size);

	CMemFile file;
	WriteCompleteFilePartStatus(file, result, ED2KPartCount(size));
	ASSERT_TRUE((std::vector<uint8>{ 3, 0, 0x05 }) == Bytes(file));
}
