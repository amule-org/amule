#include <muleunit/test.h>

#include <MemFile.h>
#include <ServerMet.h>
#include <common/MuleDebug.h>
#include <tags/ServerTags.h>

#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(ServerMet)

// 145.239.2.134 as server.met stores it: its first byte, 0x91, reads as a short STR1 tag with the
// unknown ID 0xEF, which is how a misread record header looked in the file that crashed amuled.
static const uint32 IP_A = 0x8602EF91;
static const uint32 IP_B = 0x0A00A8C0;

// Writes a server as aMule's writer does. legacyCount declares one tag too many for a server
// without a version, as older aMule versions did.
static void WriteServer(CMemFile &file, uint32 ip, const wxString &version, bool legacyCount)
{
	CMemFile tags;
	uint32 count = 0;
	auto write = [&tags, &count](const CTag &tag) {
		tag.WriteTagToFile(&tags);
		++count;
	};
	write(CTagString(ST_SERVERNAME, "server"));
	write(CTagInt32(ST_FAIL, 0));
	write(CTagInt32("users", 10));
	if (!version.IsEmpty()) {
		write(CTagString(ST_VERSION, version));
		write(CTagString(ST_VERSION, version));
	}
	write(CTagInt32(ST_LOWIDUSERS, 3));

	file.WriteUInt32(ip);
	file.WriteUInt16(4661);
	file.WriteUInt32(legacyCount && version.IsEmpty() ? count + 1 : count);
	file.Write(tags.GetRawBuffer(), static_cast<size_t>(tags.GetLength()));
}

// The server count, then the servers, with the position where ReadServerMetRecords expects it.
static void Finish(CMemFile &file, CMemFile &servers, uint32 count)
{
	file.WriteUInt32(count);
	file.Write(servers.GetRawBuffer(), static_cast<size_t>(servers.GetLength()));
	file.Seek(0);
}

TEST(ServerMet, CorrectCountsReadAsDeclared)
{
	CMemFile servers;
	WriteServer(servers, IP_A, "", false);
	WriteServer(servers, IP_B, "17.15", false);
	CMemFile file;
	Finish(file, servers, 2);

	std::vector<ServerMetRecord> records;
	ASSERT_FALSE(ReadServerMetRecords(file, records));
	ASSERT_EQUALS(size_t(2), records.size());
	ASSERT_EQUALS(size_t(4), records[0].tags.size());
	ASSERT_EQUALS(size_t(6), records[1].tags.size());
	ASSERT_EQUALS(IP_B, records[1].ip);
}

TEST(ServerMet, LegacyCountBeforeAnotherServer)
{
	CMemFile servers;
	WriteServer(servers, IP_B, "", true);
	WriteServer(servers, IP_A, "17.15", true);
	WriteServer(servers, IP_B, "17.15", true);
	CMemFile file;
	Finish(file, servers, 3);

	std::vector<ServerMetRecord> records;
	ASSERT_TRUE(ReadServerMetRecords(file, records));
	ASSERT_EQUALS(size_t(3), records.size());
	ASSERT_EQUALS(size_t(4), records[0].tags.size());
	ASSERT_EQUALS(IP_A, records[1].ip);
	ASSERT_EQUALS(uint16(4661), records[1].port);
	ASSERT_EQUALS(size_t(6), records[1].tags.size());
	ASSERT_EQUALS(file.GetLength(), file.GetPosition());
}

TEST(ServerMet, LegacyCountOnLastServer)
{
	CMemFile servers;
	WriteServer(servers, IP_A, "17.15", true);
	WriteServer(servers, IP_B, "", true);
	CMemFile file;
	Finish(file, servers, 2);

	std::vector<ServerMetRecord> records;
	ASSERT_TRUE(ReadServerMetRecords(file, records));
	ASSERT_EQUALS(size_t(2), records.size());
	ASSERT_EQUALS(size_t(4), records[1].tags.size());
}

TEST(ServerMet, LegacyCountOnEveryServer)
{
	CMemFile servers;
	WriteServer(servers, IP_B, "", true);
	WriteServer(servers, IP_A, "", true);
	WriteServer(servers, IP_B, "", true);
	CMemFile file;
	Finish(file, servers, 3);

	std::vector<ServerMetRecord> records;
	ASSERT_TRUE(ReadServerMetRecords(file, records));
	ASSERT_EQUALS(size_t(3), records.size());
	ASSERT_EQUALS(IP_A, records[1].ip);
}

TEST(ServerMet, TruncatedFileKeepsTheServersBeforeIt)
{
	CMemFile servers;
	WriteServer(servers, IP_A, "17.15", false);
	WriteServer(servers, IP_B, "17.15", false);
	CMemFile full;
	Finish(full, servers, 2);
	CMemFile file;
	file.Write(full.GetRawBuffer(), static_cast<size_t>(full.GetLength()) - 5);
	file.Seek(0);

	std::vector<ServerMetRecord> records;
	ASSERT_RAISES(CSafeIOException, ReadServerMetRecords(file, records));
	ASSERT_EQUALS(size_t(1), records.size());
	ASSERT_EQUALS(IP_A, records[0].ip);
}
