// Tests the production disk parser and main-thread adoption without a running app.
#include <muleunit/test.h>
#include "TestScratchDir.h"
#include <kademlia/kademlia/Indexed.h>
#include <kademlia/kademlia/Kademlia.h>
#include <kademlia/net/KademliaUDPListener.h>
#include <CFile.h>
#include <Preferences.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <thread>
#include <chrono>
#include <string>
#include <tags/FileTags.h>
#include <protocol/kad/Constants.h>
#ifdef __linux__
#include <unistd.h>
#endif

using namespace muleunit;
using namespace Kademlia;
DECLARE_SIMPLE(IndexedLoad)

// The standalone index never sends a packet or calls the default app constructor.
CKademlia *CKademlia::instance = nullptr;
CPacketTracking::~CPacketTracking() = default;
CKademliaUDPListener::~CKademliaUDPListener() = default;
void CKademliaUDPListener::ProcessPacket(
	const uint8_t *, uint32_t, uint32_t, uint16_t, bool, const CKadUDPKey &)
{
	throw std::runtime_error("unexpected packet");
}
wxString CPreferences::s_configDir;
void CKademliaUDPListener::SendPacket(
	const CMemFile &, uint8_t, uint32_t, uint16_t, const CKadUDPKey &, const CUInt128 *)
{
	throw std::runtime_error("unexpected packet");
}

class TempIndex
{
	const TestScratchDir m_dir{ "amule-kad-index-" };

public:
	const wxString path = m_dir.WxPath() + wxFileName::GetPathSeparator();
};
static bool WaitForLoad(CIndexed &index)
{
	while (index.GetLoadState() == CIndexed::LoadState::Loading) {
		index.ProcessIndexLoad();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return index.IsReady();
}
class DiskEntry : public CKeyEntry
{
public:
	explicit DiskEntry(const wxString &name = "fixture.bin")
	: CKeyEntry(std::make_shared<PublishTracking>())
	{
		m_uSize = 123;
		SetFileName(name);
		AddTag(new CTagString(TAG_FILETYPE, "Program"), 0);
		m_publishingIPs = new PublishingIPList;
		m_publishingIPs->push_back({ 0x01020304, time(nullptr), CKadAICHHashList::INVALID_INDEX });
		AdjustGlobalPublishTracking(0x01020304, true, "fixture");
	}
};
TEST(IndexedLoad, KeywordVersionsThreeAndFourRoundTrip)
{
	const bool old = CPreferences::GetKadProtocol10();
	for (uint32_t version : { 3u, 4u }) {
		TempIndex temp;
		CPreferences::SetKadProtocol10(version == 4);
		{
			CFile file(temp.path + "key_index.dat", CFile::write);
			file.WriteUInt32(version);
			file.WriteUInt32(time(nullptr) + 3600);
			file.WriteUInt128(CUInt128(1u));
			file.WriteUInt32(1);
			file.WriteUInt128(CUInt128(2u));
			file.WriteUInt32(1);
			file.WriteUInt128(CUInt128(3u));
			file.WriteUInt32(1);
			file.WriteUInt32(time(nullptr) + 3600);
			DiskEntry entry(wxString::FromUTF8("Клим café 東京.bin"));
			entry.AddTag(new CTagString("artist", wxString::FromUTF8("Ñandú 서울 中国")), 0);
			entry.WritePublishTrackingDataToFile(&file, version == 4);
			entry.WriteTagList(&file);
		}
		{
			CIndexed index(temp.path, CUInt128(1u));
			ASSERT_FALSE(index.IsReady());
			ASSERT_EQUALS(0u, index.m_totalIndexKeyword);
			ASSERT_TRUE(WaitForLoad(index));
			ASSERT_EQUALS(1u, index.m_totalIndexKeyword);
			ASSERT_EQUALS(size_t(1), index.GetFileKeyCount());
		}
		for (unsigned cycle = 0; cycle < 3; ++cycle) {
			// Check the saved bytes as well as the entry count: a Latin-1 read
			// preserves the entry but encodes its strings again on every save.
			CFile saved(temp.path + "key_index.dat");
			std::string bytes(saved.GetLength(), '\0');
			saved.Read(bytes.data(), bytes.size());
			ASSERT_TRUE(bytes.find("Клим café 東京.bin") != std::string::npos);
			ASSERT_TRUE(bytes.find("Ñandú 서울 中国") != std::string::npos);
			saved.Close();
			CIndexed reloaded(temp.path, CUInt128(1u));
			ASSERT_TRUE(WaitForLoad(reloaded));
			ASSERT_EQUALS(1u, reloaded.m_totalIndexKeyword);
		}
	}
	CPreferences::SetKadProtocol10(old);
}
TEST(IndexedLoad, SourcesAndLoadEntriesAreAdopted)
{
	TempIndex temp;
	{
		CFile file(temp.path + "src_index.dat", CFile::write);
		file.WriteUInt32(2);
		file.WriteUInt32(time(nullptr) + 3600);
		file.WriteUInt32(1);
		file.WriteUInt128(CUInt128(2u));
		file.WriteUInt32(1);
		file.WriteUInt128(CUInt128(3u));
		file.WriteUInt32(1);
		file.WriteUInt32(time(nullptr) + 3600);
		file.WriteUInt8(3);
		file.WriteTag(CTagVarInt(TAG_SOURCEIP, 0x01020304));
		file.WriteTag(CTagVarInt(TAG_SOURCEPORT, 4662));
		file.WriteTag(CTagVarInt(TAG_SOURCEUPORT, 4672));
	}
	{
		CFile file(temp.path + "load_index.dat", CFile::write);
		file.WriteUInt32(1);
		file.WriteUInt32(time(nullptr));
		file.WriteUInt32(1);
		file.WriteUInt128(CUInt128(2u));
		file.WriteUInt32(time(nullptr) + 3600);
	}
	CIndexed index(temp.path, CUInt128(1u));
	ASSERT_TRUE(WaitForLoad(index));
	ASSERT_EQUALS(1u, index.m_totalIndexSource);
	ASSERT_EQUALS(1u, index.m_totalIndexLoad);
}
TEST(IndexedLoad, TruncatedIndexIsQuarantinedAndReplaced)
{
	TempIndex temp;
	{
		CFile file(temp.path + "key_index.dat", CFile::write);
		file.WriteUInt32(4);
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_TRUE(index.IsReady());
		ASSERT_TRUE(index.AddLoad(CUInt128(2u), time(nullptr) + 3600));
		ASSERT_EQUALS(0u, index.m_totalIndexKeyword);
	}
	CFile file(temp.path + "key_index.dat.bad");
	ASSERT_EQUALS(uint64_t(4), file.GetLength());
	ASSERT_EQUALS(4u, file.ReadUInt32());
	CIndexed reloaded(temp.path, CUInt128(1u));
	ASSERT_TRUE(WaitForLoad(reloaded));
	ASSERT_EQUALS(1u, reloaded.m_totalIndexLoad);
	ASSERT_EQUALS(0u, reloaded.m_totalIndexKeyword);
}
TEST(IndexedLoad, StopBeforeAdoptionDoesNotRewriteFiles)
{
	TempIndex temp;
	{
		CFile file(temp.path + "load_index.dat", CFile::write);
		file.WriteUInt32(1);
		file.WriteUInt32(123);
		file.WriteUInt32(0);
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
	}
	CFile file(temp.path + "load_index.dat");
	ASSERT_EQUALS(1u, file.ReadUInt32());
	ASSERT_EQUALS(123u, file.ReadUInt32());
	ASSERT_FALSE(wxFileExists(temp.path + "key_index.dat"));
}
TEST(IndexedLoad, WrongIdentitySkipsKeywordIndex)
{
	TempIndex temp;
	{
		CFile file(temp.path + "key_index.dat", CFile::write);
		file.WriteUInt32(3);
		file.WriteUInt32(time(nullptr) + 3600);
		file.WriteUInt128(CUInt128(99u));
		file.WriteUInt32(0);
	}
	CIndexed index(temp.path, CUInt128(1u));
	ASSERT_TRUE(WaitForLoad(index));
	ASSERT_EQUALS(0u, index.m_totalIndexKeyword);
}

TEST(IndexedLoad, UnsupportedVersionIsQuarantined)
{
	TempIndex temp;
	{
		CFile file(temp.path + "src_index.dat", CFile::write);
		file.WriteUInt32(99);
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_TRUE(index.IsReady());
	}
	CFile file(temp.path + "src_index.dat.bad");
	ASSERT_EQUALS(uint64_t(4), file.GetLength());
	ASSERT_EQUALS(99u, file.ReadUInt32());
}

TEST(IndexedLoad, PartiallyDecodedKeywordIsDiscarded)
{
	TempIndex temp;
	uint64_t truncatedSize;
	{
		CFile file(temp.path + "key_index.dat", CFile::write);
		file.WriteUInt32(4);
		file.WriteUInt32(time(nullptr) + 3600);
		file.WriteUInt128(CUInt128(1u));
		file.WriteUInt32(1);
		file.WriteUInt128(CUInt128(2u));
		file.WriteUInt32(1);
		file.WriteUInt128(CUInt128(3u));
		file.WriteUInt32(1);
		file.WriteUInt32(time(nullptr) + 3600);
		DiskEntry entry;
		entry.WritePublishTrackingDataToFile(&file, true);
		file.WriteUInt8(1); // incomplete string tag, after publisher data was loaded
		truncatedSize = file.GetLength();
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_EQUALS(0u, index.m_totalIndexKeyword);
	}
	CFile file(temp.path + "key_index.dat.bad");
	ASSERT_EQUALS(truncatedSize, file.GetLength());
}

TEST(IndexedLoad, WrongTypeSourceTagIsDiscarded)
{
	TempIndex temp;
	uint64_t originalSize;
	{
		CFile file(temp.path + "src_index.dat", CFile::write);
		file.WriteUInt32(2);
		file.WriteUInt32(time(nullptr) + 3600);
		file.WriteUInt32(1);
		file.WriteUInt128(CUInt128(2u));
		file.WriteUInt32(1);
		file.WriteUInt128(CUInt128(3u));
		file.WriteUInt32(1);
		file.WriteUInt32(time(nullptr) + 3600);
		file.WriteUInt8(1);
		file.WriteTag(CTagString(TAG_SOURCEIP, "invalid integer"));
		originalSize = file.GetLength();
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_TRUE(index.IsReady());
		ASSERT_EQUALS(0u, index.m_totalIndexSource);
	}
	CFile file(temp.path + "src_index.dat.bad");
	ASSERT_EQUALS(originalSize, file.GetLength());
}

TEST(IndexedLoad, LargeLoadIndexAdoptsAndRoundTripsAsOneResult)
{
	TempIndex temp;
	constexpr uint32_t count = 50000;
	{
		CFile file(temp.path + "load_index.dat", CFile::write);
		file.WriteUInt32(1);
		file.WriteUInt32(time(nullptr));
		file.WriteUInt32(count);
		for (uint32_t i = 1; i <= count; ++i) {
			file.WriteUInt128(CUInt128(i));
			file.WriteUInt32(time(nullptr) + 3600);
		}
	}
	// Repeated stops before adoption cancel/join the production worker. A new
	// worker must still load the full original index, without quarantine or saves.
	for (int cycle = 0; cycle < 10; ++cycle) {
		CIndexed stopped(temp.path, CUInt128(1u));
	}
	ASSERT_FALSE(wxFileExists(temp.path + "load_index.dat.bad"));
	ASSERT_FALSE(wxFileExists(temp.path + "key_index.dat"));
	{
		CFile original(temp.path + "load_index.dat");
		ASSERT_EQUALS(1u, original.ReadUInt32());
		original.ReadUInt32();
		ASSERT_EQUALS(count, original.ReadUInt32());
		ASSERT_EQUALS(uint64_t(12 + count * 20), original.GetLength());
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		ASSERT_EQUALS(0u, index.m_totalIndexLoad);
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_EQUALS(count, index.m_totalIndexLoad);
	}
	CIndexed reloaded(temp.path, CUInt128(1u));
	ASSERT_TRUE(WaitForLoad(reloaded));
	ASSERT_EQUALS(count, reloaded.m_totalIndexLoad);
}

TEST(IndexedLoad, PublisherDecoderHonorsCancellation)
{
	TempIndex temp;
	const wxString path = temp.path + "publisher.dat";
	{
		CFile file(path, CFile::write);
		DiskEntry entry;
		entry.WritePublishTrackingDataToFile(&file, true);
	}
	CFile file(path);
	const auto position = file.GetPosition();
	std::atomic<bool> cancel{ true };
	CKeyEntry entry(std::make_shared<CKeyEntry::PublishTracking>());
	bool cancelled = false;
	try {
		entry.ReadPublishTrackingDataFromFile(&file, true, &cancel);
	} catch (const std::runtime_error &) {
		cancelled = true;
	}
	ASSERT_TRUE(cancelled);
	ASSERT_EQUALS(position, file.GetPosition());
}

class CancellingPublisherFile : public CFile
{
	std::atomic<bool> &m_cancel;

public:
	CancellingPublisherFile(const wxString &path, std::atomic<bool> &cancel)
	: CFile(path)
	, m_cancel(cancel)
	{
	}
	void Read(void *buffer, size_t count) const override
	{
		CFile::Read(buffer, count);
		if (GetPosition() >= 20) {
			m_cancel.store(true);
		}
	}
};
TEST(IndexedLoad, CancellationStopsInsidePublisherListAndReleasesTracking)
{
	TempIndex temp;
	const wxString path = temp.path + "publishers.dat";
	{
		CFile file(path, CFile::write);
		file.WriteUInt16(0); // no AICH hashes
		file.WriteUInt32(0); // no names
		file.WriteUInt32(10000);
		for (uint32_t i = 0; i < 10000; ++i) {
			file.WriteUInt32(0x01020304 + i);
			file.WriteUInt32(time(nullptr));
			file.WriteUInt16(CKadAICHHashList::INVALID_INDEX);
		}
	}
	std::atomic<bool> cancel{ false };
	CancellingPublisherFile file(path, cancel);
	auto tracking = std::make_shared<CKeyEntry::PublishTracking>();
	{
		CKeyEntry entry(tracking);
		bool stopped = false;
		try {
			entry.ReadPublishTrackingDataFromFile(&file, true, &cancel);
		} catch (const std::runtime_error &) {
			stopped = true;
		}
		ASSERT_TRUE(stopped);
		ASSERT_EQUALS(uint64_t(20), file.GetPosition());
		ASSERT_EQUALS(size_t(1), tracking->size());
	}
	ASSERT_TRUE(tracking->empty());
}

TEST(IndexedLoad, FailedSavePreservesPreviousLoadIndex)
{
	TempIndex temp;
	{
		CFile file(temp.path + "load_index.dat", CFile::write);
		file.WriteUInt32(1);
		file.WriteUInt32(123);
		file.WriteUInt32(0);
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_TRUE(index.AddLoad(CUInt128(2u), time(nullptr) + 3600));
		// Prevent creation of the staging file without touching the original.
		ASSERT_TRUE(wxMkdir(temp.path + "load_index.dat.new"));
	}
	ASSERT_TRUE(wxFileExists(temp.path + "key_index.dat"));
	ASSERT_TRUE(wxFileExists(temp.path + "src_index.dat"));
	CFile original(temp.path + "load_index.dat");
	ASSERT_EQUALS(uint64_t(12), original.GetLength());
	ASSERT_EQUALS(1u, original.ReadUInt32());
	ASSERT_EQUALS(123u, original.ReadUInt32());
	ASSERT_EQUALS(0u, original.ReadUInt32());
}

#ifdef __linux__
TEST(IndexedLoad, DiskFullDuringSavePreservesPreviousLoadIndex)
{
	TempIndex temp;
	{
		CFile file(temp.path + "load_index.dat", CFile::write);
		file.WriteUInt32(1);
		file.WriteUInt32(123);
		file.WriteUInt32(0);
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_TRUE(index.AddLoad(CUInt128(2u), time(nullptr) + 3600));
		ASSERT_EQUALS(0, symlink("/dev/full", (temp.path + "load_index.dat.new").fn_str()));
	}
	ASSERT_FALSE(wxFileExists(temp.path + "load_index.dat.new"));
	ASSERT_TRUE(wxFileExists(temp.path + "key_index.dat"));
	ASSERT_TRUE(wxFileExists(temp.path + "src_index.dat"));
	CFile original(temp.path + "load_index.dat");
	ASSERT_EQUALS(uint64_t(12), original.GetLength());
	ASSERT_EQUALS(1u, original.ReadUInt32());
	ASSERT_EQUALS(123u, original.ReadUInt32());
	ASSERT_EQUALS(0u, original.ReadUInt32());
}
#endif

TEST(IndexedLoad, LargeKeywordAndSourceIndexesRoundTrip)
{
	TempIndex temp;
	constexpr uint32_t count = 10000;
	{
		CFile keys(temp.path + "key_index.dat", CFile::write);
		keys.WriteUInt32(4);
		keys.WriteUInt32(time(nullptr) + 3600);
		keys.WriteUInt128(CUInt128(1u));
		keys.WriteUInt32(count);
		CFile sources(temp.path + "src_index.dat", CFile::write);
		sources.WriteUInt32(2);
		sources.WriteUInt32(time(nullptr) + 3600);
		sources.WriteUInt32(count);
		DiskEntry entry;
		for (uint32_t i = 1; i <= count; ++i) {
			keys.WriteUInt128(CUInt128(i));
			keys.WriteUInt32(1);
			keys.WriteUInt128(CUInt128(i + count));
			keys.WriteUInt32(1);
			keys.WriteUInt32(time(nullptr) + 3600);
			entry.WritePublishTrackingDataToFile(&keys, true);
			entry.WriteTagList(&keys);
			sources.WriteUInt128(CUInt128(i));
			sources.WriteUInt32(1);
			sources.WriteUInt128(CUInt128(i + count));
			sources.WriteUInt32(1);
			sources.WriteUInt32(time(nullptr) + 3600);
			sources.WriteUInt8(3);
			sources.WriteTag(CTagVarInt(TAG_SOURCEIP, 0x01020304));
			sources.WriteTag(CTagVarInt(TAG_SOURCEPORT, 4662));
			sources.WriteTag(CTagVarInt(TAG_SOURCEUPORT, 4672));
		}
	}
	for (int cycle = 0; cycle < 2; ++cycle) {
		CIndexed index(temp.path, CUInt128(1u));
		// Outgoing keyword publishing continues and records its delays; incoming
		// publications wait for adoption.
		ASSERT_TRUE(index.SendStoreRequest(CUInt128(1u)));
		ASSERT_TRUE(index.AddLoad(CUInt128(count + 1), time(nullptr) + 3600));
		ASSERT_FALSE(index.SendStoreRequest(CUInt128(count + 1)));
		uint8_t load = 0;
		auto entry = std::unique_ptr<CKeyEntry>(index.CreateKeyEntry());
		ASSERT_FALSE(index.AddKeyword(CUInt128(count + 1), CUInt128(2u), entry.get(), load));
		ASSERT_EQUALS(0u, index.m_totalIndexKeyword);
		ASSERT_EQUALS(0u, index.m_totalIndexSource);
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_EQUALS(count, index.m_totalIndexKeyword);
		ASSERT_EQUALS(count, index.m_totalIndexSource);
		ASSERT_EQUALS(size_t(count), index.GetFileKeyCount());
		// The second cycle reads the same delay back from disk and merges it.
		ASSERT_EQUALS(1u, index.m_totalIndexLoad);
		ASSERT_FALSE(index.SendStoreRequest(CUInt128(count + 1)));
	}
}

TEST(IndexedLoad, CorruptPartDoesNotDiscardHealthyParts)
{
	for (bool truncated : { false, true }) {
		for (const wxString bad : { wxString("load_index.dat"),
			     wxString("key_index.dat"),
			     wxString("src_index.dat") }) {
			TempIndex temp;
			{
				CIndexed seed(temp.path, CUInt128(1u));
				ASSERT_TRUE(WaitForLoad(seed));
				ASSERT_TRUE(seed.AddLoad(CUInt128(2u), time(nullptr) + 3600));
				uint8_t load = 0;
				auto key = std::unique_ptr<CKeyEntry>(seed.CreateKeyEntry());
				key->m_tLifeTime = time(nullptr) + 3600;
				key->m_uSize = 123;
				key->m_uIP = 0x01020304;
				key->SetFileName("healthy.bin");
				key->AddTag(new CTagString(TAG_FILETYPE, "Program"), 0);
				ASSERT_TRUE(seed.AddKeyword(CUInt128(2u), CUInt128(3u), key.get(), load));
				key.release();
				auto source = std::make_unique<CEntry>();
				source->m_tLifeTime = time(nullptr) + 3600;
				source->m_bSource = true;
				source->m_uIP = 0x01020304;
				source->m_uTCPport = 4662;
				source->m_uUDPport = 4672;
				source->AddTag(new CTagVarInt(TAG_SOURCEIP, source->m_uIP), 0);
				source->AddTag(new CTagVarInt(TAG_SOURCEPORT, source->m_uTCPport), 0);
				source->AddTag(new CTagVarInt(TAG_SOURCEUPORT, source->m_uUDPport), 0);
				ASSERT_TRUE(seed.AddSources(CUInt128(2u), CUInt128(3u), source.get(), load));
				source.release();
			}
			{
				CFile file(temp.path + bad, truncated ? CFile::read_write : CFile::write);
				if (truncated) {
					// Announce a second key after the first complete entry: the loader
					// must clear the inserted entry and its publisher counts on EOF.
					file.Seek(bad == "key_index.dat" ? 24 : 8);
					file.WriteUInt32(2);
				} else {
					file.WriteUInt32(99);
				}
			}
			{
				CIndexed index(temp.path, CUInt128(1u));
				ASSERT_TRUE(WaitForLoad(index));
				ASSERT_EQUALS(bad == "key_index.dat" ? 0u : 1u, index.m_totalIndexKeyword);
				ASSERT_EQUALS(bad == "src_index.dat" ? 0u : 1u, index.m_totalIndexSource);
				ASSERT_EQUALS(bad == "load_index.dat" ? 0u : 1u, index.m_totalIndexLoad);
				ASSERT_TRUE(wxFileExists(temp.path + bad + ".bad"));
				ASSERT_TRUE(index.AddLoad(CUInt128(4u), time(nullptr) + 3600));
				ASSERT_TRUE(index.SendStoreRequest(CUInt128(5u)));
			}
			CIndexed reloaded(temp.path, CUInt128(1u));
			ASSERT_TRUE(WaitForLoad(reloaded));
			ASSERT_EQUALS(bad == "load_index.dat" ? 1u : 2u, reloaded.m_totalIndexLoad);
		}
	}
}

TEST(IndexedLoad, PublishDelaysRecordedWhileLoadingAreKept)
{
	TempIndex temp;
	const uint32_t now = time(nullptr);
	{
		CFile file(temp.path + "load_index.dat", CFile::write);
		file.WriteUInt32(1);
		file.WriteUInt32(now);
		file.WriteUInt32(2);
		file.WriteUInt128(CUInt128(2u));
		file.WriteUInt32(now + 3600);
		file.WriteUInt128(CUInt128(4u));
		file.WriteUInt32(now + 3600);
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		// Our own publishes end while the worker reads: one new delay, one that extends a
		// saved delay, and one shorter than the saved delay, which must not cut it.
		ASSERT_TRUE(index.AddLoad(CUInt128(2u), now + 7200));
		ASSERT_TRUE(index.AddLoad(CUInt128(3u), now + 3600));
		ASSERT_TRUE(index.AddLoad(CUInt128(4u), now + 600));
		ASSERT_FALSE(index.SendStoreRequest(CUInt128(3u)));
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_EQUALS(3u, index.m_totalIndexLoad);
		for (uint32_t key : { 2u, 3u, 4u }) {
			ASSERT_FALSE(index.SendStoreRequest(CUInt128(key)));
		}
	}
	CFile saved(temp.path + "load_index.dat");
	ASSERT_EQUALS(1u, saved.ReadUInt32());
	saved.ReadUInt32();
	ASSERT_EQUALS(3u, saved.ReadUInt32());
	for (const auto &expected : { std::make_pair(2u, now + 7200),
		     std::make_pair(3u, now + 3600),
		     std::make_pair(4u, now + 3600) }) {
		ASSERT_TRUE(saved.ReadUInt128() == CUInt128(expected.first));
		ASSERT_EQUALS(expected.second, saved.ReadUInt32());
	}
}

TEST(IndexedLoad, PartialLoadMapIsClearedOnError)
{
	TempIndex temp;
	{
		CFile file(temp.path + "load_index.dat", CFile::write);
		file.WriteUInt32(1);
		file.WriteUInt32(time(nullptr));
		file.WriteUInt32(2);
		file.WriteUInt128(CUInt128(2u));
		file.WriteUInt32(time(nullptr) + 3600);
		// Truncated after a complete entry was inserted into the worker map.
	}
	CIndexed index(temp.path, CUInt128(1u));
	ASSERT_TRUE(WaitForLoad(index));
	ASSERT_EQUALS(0u, index.m_totalIndexLoad);
	ASSERT_TRUE(index.SendStoreRequest(CUInt128(2u)));
}

TEST(IndexedLoad, FailedQuarantinePreservesOriginalButAllowsNewEntries)
{
	TempIndex temp;
	{
		CFile file(temp.path + "key_index.dat", CFile::write);
		file.WriteUInt32(99);
	}
	// An existing directory prevents renaming to .bad.
	ASSERT_TRUE(wxMkdir(temp.path + "key_index.dat.bad"));
	{
		CIndexed index(temp.path, CUInt128(1u));
		ASSERT_TRUE(WaitForLoad(index));
		ASSERT_TRUE(index.AddLoad(CUInt128(2u), time(nullptr) + 3600));
	}
	CFile original(temp.path + "key_index.dat");
	ASSERT_EQUALS(uint64_t(4), original.GetLength());
	ASSERT_EQUALS(99u, original.ReadUInt32());
	ASSERT_TRUE(wxFileExists(temp.path + "load_index.dat"));
}

TEST(IndexedLoad, StopBeforeAdoptionDoesNotQuarantineCorruption)
{
	TempIndex temp;
	{
		CFile file(temp.path + "key_index.dat", CFile::write);
		file.WriteUInt32(99);
	}
	{
		CIndexed index(temp.path, CUInt128(1u));
		// Even if the worker finishes first, only adoption can touch disk.
	}
	ASSERT_FALSE(wxFileExists(temp.path + "key_index.dat.bad"));
	CFile original(temp.path + "key_index.dat");
	ASSERT_EQUALS(uint64_t(4), original.GetLength());
	ASSERT_EQUALS(99u, original.ReadUInt32());
}

static CEntry *NoteEntry(uint32_t ip, time_t lifetime)
{
	CEntry *entry = new CEntry();
	entry->m_uIP = ip;
	entry->m_uUDPport = 4672;
	entry->m_tLifeTime = lifetime;
	entry->AddTag(new CTagString(TAG_DESCRIPTION, "note"), ip);
	return entry;
}

TEST(IndexedLoad, NotesExpire)
{
	TempIndex temp;
	CIndexed index(temp.path, CUInt128(1u));
	ASSERT_TRUE(WaitForLoad(index));
	uint8_t load = 0;
	const time_t now = time(nullptr);
	// AddNotes refuses an entry that has already expired, so this one ages out after insertion.
	CEntry *stale = NoteEntry(0x01020304, now + 1);
	ASSERT_TRUE(index.AddNotes(CUInt128(2u), CUInt128(3u), stale, load));
	stale->m_tLifeTime = now - 1;
	ASSERT_TRUE(index.AddNotes(CUInt128(4u), CUInt128(5u), NoteEntry(0x01020305, now + 3600), load));
	ASSERT_EQUALS(2u, index.m_totalIndexNotes);
	index.Expire(now);
	ASSERT_EQUALS(1u, index.m_totalIndexNotes);
	CEntry *expired = NoteEntry(0x01020306, now - 1);
	ASSERT_FALSE(index.AddNotes(CUInt128(6u), CUInt128(7u), expired, load));
	delete expired;
}

TEST(IndexedLoad, NotesStopAtGlobalCap)
{
	TempIndex temp;
	CIndexed index(temp.path, CUInt128(1u));
	ASSERT_TRUE(WaitForLoad(index));
	uint8_t load = 0;
	const time_t lifetime = time(nullptr) + 3600;
	for (uint32_t i = 0; i < KADEMLIAMAXNOTES; ++i) {
		ASSERT_TRUE(index.AddNotes(CUInt128(i + 10), CUInt128(1u), NoteEntry(i + 1, lifetime), load));
	}
	ASSERT_EQUALS(uint32_t(KADEMLIAMAXNOTES), index.m_totalIndexNotes);
	CEntry *refused = NoteEntry(0x7f000001, lifetime);
	ASSERT_FALSE(index.AddNotes(CUInt128(9u), CUInt128(1u), refused, load));
	ASSERT_EQUALS(100u, unsigned(load));
	delete refused;
	// A publisher refreshing its own note still gets through.
	ASSERT_TRUE(index.AddNotes(CUInt128(10u), CUInt128(1u), NoteEntry(1, lifetime), load));
	ASSERT_EQUALS(uint32_t(KADEMLIAMAXNOTES), index.m_totalIndexNotes);
}

static CEntry *SourceEntry(uint32_t ip, time_t lifetime, uint32_t extraTagBytes)
{
	CEntry *entry = new CEntry();
	entry->m_uIP = ip;
	entry->m_uTCPport = 4662;
	entry->m_uUDPport = 4672;
	entry->m_tLifeTime = lifetime;
	entry->m_extraTagBytes = extraTagBytes;
	entry->AddTag(new CTagVarInt(TAG_SOURCETYPE, 1), ip);
	return entry;
}

TEST(IndexedLoad, ExtraTagBytesAreCountedAndRecounted)
{
	TempIndex temp;
	CIndexed index(temp.path, CUInt128(1u));
	ASSERT_TRUE(WaitForLoad(index));
	uint8_t load = 0;
	const time_t now = time(nullptr);
	CEntry *stale = SourceEntry(0x01020304, now + 1, 1000);
	ASSERT_TRUE(index.AddSources(CUInt128(2u), CUInt128(3u), stale, load));
	ASSERT_TRUE(
		index.AddSources(CUInt128(4u), CUInt128(5u), SourceEntry(0x01020305, now + 3600, 500), load));
	ASSERT_EQUALS(1500u, index.GetExtraTagBytes());
	ASSERT_TRUE(index.HasRoomForExtraTags(KADEMLIAMAXEXTRATAGBYTES - 1500));
	ASSERT_FALSE(index.HasRoomForExtraTags(KADEMLIAMAXEXTRATAGBYTES - 1499));
	stale->m_tLifeTime = now - 1;
	index.Expire(now);
	ASSERT_EQUALS(500u, index.GetExtraTagBytes());
}
