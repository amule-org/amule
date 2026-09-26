// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <ClientMetaTrailer.h>
#include <MemFile.h>

#include <vector>

using namespace muleunit;
using ClientMetaTrailer::Entry;

// Stands in for the credit records, which the trailer follows.
static const uint8 kRecords[] = { 1, 0, 0, 0, 0 };

static Entry MakeEntry(uint8 seed, const wxString &name)
{
	Entry entry;
	uint8 key[16];
	for (int i = 0; i < 16; i++) {
		key[i] = static_cast<uint8>(seed + i);
	}
	entry.key = CMD4Hash(key);
	entry.meta.name = name;
	entry.meta.firstSeen = 0x11111111u * seed;
	entry.meta.sessions = seed + 1;
	entry.meta.lastIP = 0x0100007fu + seed;
	entry.meta.lastPort = 4662 + seed;
	entry.meta.kadPort = 4672 + seed;
	entry.meta.version = 0x00030100u + seed;
	entry.meta.clientSoft = seed;
	entry.meta.sourceFrom = seed + 2;
	entry.meta.obfuscation = seed + 3;
	return entry;
}

static void AssertSameEntry(const Entry &expected, const Entry &actual)
{
	ASSERT_EQUALS(expected.key.Encode(), actual.key.Encode());
	ASSERT_EQUALS(expected.meta.name, actual.meta.name);
	ASSERT_EQUALS(expected.meta.firstSeen, actual.meta.firstSeen);
	ASSERT_EQUALS(expected.meta.sessions, actual.meta.sessions);
	ASSERT_EQUALS(expected.meta.lastIP, actual.meta.lastIP);
	ASSERT_EQUALS(expected.meta.lastPort, actual.meta.lastPort);
	ASSERT_EQUALS(expected.meta.kadPort, actual.meta.kadPort);
	ASSERT_EQUALS(expected.meta.version, actual.meta.version);
	ASSERT_EQUALS(expected.meta.clientSoft, actual.meta.clientSoft);
	ASSERT_EQUALS(expected.meta.sourceFrom, actual.meta.sourceFrom);
	ASSERT_EQUALS(expected.meta.obfuscation, actual.meta.obfuscation);
}

// A file holding the records, then whatever `tail` holds.
static CMemFile *FileWithTail(const std::vector<uint8> &tail)
{
	CMemFile *file = new CMemFile();
	file->Write(kRecords, sizeof(kRecords));
	if (!tail.empty()) {
		file->Write(tail.data(), tail.size());
	}
	file->Seek(sizeof(kRecords));
	return file;
}

static std::vector<uint8> Written(const std::vector<Entry> &entries)
{
	CMemFile file;
	ClientMetaTrailer::Write(file, entries);
	const uint8 *raw = file.GetRawBuffer();
	return std::vector<uint8>(raw, raw + file.GetLength());
}

DECLARE_SIMPLE(ClientMetaTrailer)

TEST(ClientMetaTrailer, EntriesSurviveARoundTrip)
{
	std::vector<Entry> saved;
	saved.push_back(MakeEntry(1, "peer"));
	saved.push_back(MakeEntry(2, wxString::FromUTF8("p\xC3\xA9\xC3\xABr")));

	CMemFile *file = FileWithTail(Written(saved));
	std::vector<Entry> loaded;
	ASSERT_EQUALS(ClientMetaTrailer::READ_OK, ClientMetaTrailer::Read(*file, loaded));
	delete file;

	ASSERT_EQUALS(saved.size(), loaded.size());
	for (size_t i = 0; i < saved.size(); i++) {
		AssertSameEntry(saved[i], loaded[i]);
	}
}

// Pins the on-disk layout: a change here breaks files written by released builds.
TEST(ClientMetaTrailer, LayoutMatchesTheReleasedFormat)
{
	std::vector<Entry> saved;
	saved.push_back(MakeEntry(1, "ab"));

	const wxString expected = "414d554c454d4431"                 // magic
				  "01"                               // version
				  "01000000"                         // entry count
				  "0102030405060708090a0b0c0d0e0f10" // key
				  "11111111"                         // firstSeen
				  "02000000"                         // sessions
				  "80000001"                         // lastIP
				  "3712"                             // lastPort 4663
				  "4112"                             // kadPort 4673
				  "01010300"                         // version
				  "01"                               // clientSoft
				  "03"                               // sourceFrom
				  "04"                               // obfuscation
				  "02006162";                        // name "ab", uint16 length

	wxString written;
	for (uint8 byte : Written(saved)) {
		written += wxString::Format("%02x", byte);
	}
	ASSERT_EQUALS(expected, written);
}

TEST(ClientMetaTrailer, NoEntriesWriteNothing)
{
	ASSERT_EQUALS(0u, Written(std::vector<Entry>()).size());
}

TEST(ClientMetaTrailer, LongNamesAreCappedInCharacters)
{
	wxString name;
	for (int i = 0; i < 100; i++) {
		name += wxString::FromUTF8("\xC3\xA9");
	}
	std::vector<Entry> saved;
	saved.push_back(MakeEntry(1, name));

	CMemFile *file = FileWithTail(Written(saved));
	std::vector<Entry> loaded;
	ASSERT_EQUALS(ClientMetaTrailer::READ_OK, ClientMetaTrailer::Read(*file, loaded));
	delete file;

	ASSERT_EQUALS(1u, loaded.size());
	ASSERT_EQUALS(name.Left(64), loaded[0].meta.name);
}

TEST(ClientMetaTrailer, AFileFromAnOlderBuildHasNoTrailer)
{
	std::vector<Entry> loaded;
	CMemFile *file = FileWithTail(std::vector<uint8>());
	ASSERT_EQUALS(ClientMetaTrailer::READ_ABSENT, ClientMetaTrailer::Read(*file, loaded));
	delete file;

	// Shorter than magic, version and count together.
	file = FileWithTail(std::vector<uint8>(12, 'A'));
	ASSERT_EQUALS(ClientMetaTrailer::READ_ABSENT, ClientMetaTrailer::Read(*file, loaded));
	delete file;
	ASSERT_EQUALS(0u, loaded.size());
}

TEST(ClientMetaTrailer, OtherTrailingDataIsNotABlock)
{
	std::vector<uint8> tail = Written(std::vector<Entry>(1, MakeEntry(1, "peer")));
	tail[0] = 'X';

	CMemFile *file = FileWithTail(tail);
	std::vector<Entry> loaded;
	ASSERT_EQUALS(ClientMetaTrailer::READ_FOREIGN, ClientMetaTrailer::Read(*file, loaded));
	delete file;
	ASSERT_EQUALS(0u, loaded.size());
}

TEST(ClientMetaTrailer, ABlockFromANewerVersionIsLeftAlone)
{
	std::vector<uint8> tail = Written(std::vector<Entry>(1, MakeEntry(1, "peer")));
	tail[8] = 2;

	CMemFile *file = FileWithTail(tail);
	std::vector<Entry> loaded;
	ASSERT_EQUALS(ClientMetaTrailer::READ_OTHER_VERSION, ClientMetaTrailer::Read(*file, loaded));
	delete file;
	ASSERT_EQUALS(0u, loaded.size());
}

TEST(ClientMetaTrailer, ATruncatedBlockKeepsTheCompleteEntries)
{
	std::vector<Entry> saved;
	saved.push_back(MakeEntry(1, "first"));
	saved.push_back(MakeEntry(2, "second"));
	std::vector<uint8> tail = Written(saved);
	tail.resize(tail.size() - 3);

	CMemFile *file = FileWithTail(tail);
	std::vector<Entry> loaded;
	ASSERT_EQUALS(ClientMetaTrailer::READ_DAMAGED, ClientMetaTrailer::Read(*file, loaded));
	delete file;

	ASSERT_EQUALS(1u, loaded.size());
	AssertSameEntry(saved[0], loaded[0]);
}
