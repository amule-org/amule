//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "SearchList.h" // Interface declarations.
#include "SearchPacket.h"
#include "SearchStartBookkeeping.h"

#include "BrowseManager.h"

#include <algorithm> // Needed for std::sort (StoreSearches)
#include <limits>    // Needed for std::numeric_limits (StoredSearches parameter tags)
#include <utility>   // Needed for std::move (LoadSearches)

#include <protocol/Protocols.h>
#include <protocol/kad/Constants.h>
#include <tags/ClientTags.h>
#include <tags/FileTags.h>
#include <common/DataFileVersion.h> // Needed for STOREDSEARCHES_VERSION

#include "updownclient.h"    // Needed for CUpDownClient
#include "MemFile.h"         // Needed for CMemFile
#include "amule.h"           // Needed for theApp
#include "ServerConnect.h"   // Needed for theApp->serverconnect
#include "Server.h"          // Needed for CServer
#include "ServerList.h"      // Needed for theApp->serverlist
#include "Statistics.h"      // Needed for theStats
#include "ObservableQueue.h" // Needed for CQueueObserver
#include <common/Format.h>
#include "CFile.h"       // Needed for CFile (StoredSearches.met)
#include "Preferences.h" // Needed for thePrefs::GetConfigDir
#include "Logger.h"      // Needed for AddLogLineM/...
#include "Packet.h"      // Needed for CPacket
#include "Tag.h"         // Needed for CTag (StoredSearches.met parameter tags)
#include "GuiEvents.h"   // Needed for Notify_*

#ifndef AMULE_DAEMON
#include "amuleDlg.h"  // Needed for CamuleDlg
#include "SearchDlg.h" // Needed for CSearchDlg
#endif

#include "kademlia/kademlia/Kademlia.h"
#include "kademlia/kademlia/Search.h"
#include "kademlia/kademlia/Defines.h" // Needed for SEARCHKEYWORD_LIFETIME (Kad ramp)

///////////////////////////////////////////////////////////
// CSearchList

wxBEGIN_EVENT_TABLE(CSearchList, wxEvtHandler)
	EVT_MULE_TIMER(wxID_ANY, CSearchList::OnGlobalSearchTimer)
wxEND_EVENT_TABLE()

CSearchList::CSearchList()
: m_searchTimer(this, 0 /* Timer-id doesn't matter. */)
, m_searchType(LocalSearch)
, m_searchInProgress(false)
, m_currentSearch(-1)
, m_64bitSearchPacket(false)
, m_KadSearchFinished(true)
, m_ed2kSearchFinished(true)
, m_awaitingServerAnswer(false)
, m_searchStart(0)
, m_shuttingDown(false)
, m_storedSearchesDirty(false)
, m_lastStoredSearchesAttemptMs(0)
{
}

CSearchList::~CSearchList()
{
	// Teardown: the GUI is being dismantled around us, so suppress the Search_Removed broadcast
	// below -- there is no tab left worth closing and the notify would reach a half-destroyed
	// CamuleDlg.
	m_shuttingDown = true;
	StopSearch();
	m_searchTimer.Stop();
	m_searchPacket.reset();
	// Searches with zero hits still own Kad work and per-search bookkeeping.
	while (!m_searchStrings.empty()) {
		RemoveResults(m_searchStrings.begin()->first);
	}
	while (!AllResults().empty()) {
		RemoveResults(AllResults().begin()->first);
	}
}

uint32 CSearchList::AllocateEd2kId()
{
	do {
		m_nextEd2kId = (m_nextEd2kId + 1) & 0x3fffffff;
	} while (m_nextEd2kId == 0);
	return m_nextEd2kId;
}

void CSearchList::RemoveResults(wxUIntPtr searchID)
{
	if (!IsKnownSearchId(static_cast<uint32_t>(searchID)) && !HasSearchResults(searchID)) {
		return;
	}
	StopSearchById(searchID);
	MarkStoredSearchesDirty(static_cast<uint32_t>(searchID));
	// A closed search must not be recreated by an already queued server reply.
	if (m_currentSearch == searchID) {
		m_currentSearch = wxUIntPtr(-1);
	}
	struct RemovedResults
	{
		CSearchResultList files;
		~RemovedResults()
		{
			for (CSearchFile *file : files) {
				delete file;
			}
		}
	} removed{ TakeSearchResults(searchID) };
	const uint32_t kadID = KadComponentOf(static_cast<uint32_t>(searchID));
	m_finishedKadSearches.erase(kadID);
	m_kadToEd2kSearchId.erase(kadID);

	// Drop any per-search tracking for this ID (bounded growth).
	m_finishedKadSearches.erase(static_cast<uint32_t>(searchID));
	m_kadAICHKeys.erase(static_cast<uint32_t>(searchID));
	m_searchStartTimes.erase(static_cast<uint32_t>(searchID));
	m_searchKinds.erase(static_cast<uint32_t>(searchID));
	m_searchStrings.erase(static_cast<uint32_t>(searchID));
	m_searchParams.erase(static_cast<uint32_t>(searchID));
	m_browsePeers.erase(static_cast<uint32_t>(searchID));

	m_resultTypes.erase(static_cast<uint32_t>(searchID));
	// The browse record outlives its client but not its search.
	theApp->browsemanager->Remove(static_cast<uint32_t>(searchID));

	// Tell the GUI before the CSearchFile objects below are deleted: in a monolithic build
	// CSearchListCtrl's model holds them as raw pointers -- each row's wxDataViewItem ID is the
	// CSearchFile* -- and nothing else removes those rows, so a tab left open on this search
	// would fault on the next repaint, sort, scroll or click. Also the local counterpart of
	// amuleGUI's EC_TAG_SEARCH_EXPIRED-driven close.
	if (!m_shuttingDown) {
		Notify_Search_Removed(searchID);
	}

	// removed owns the detached results until all notifications have returned. The dirty
	// state is flushed by the core timer, avoiding a full rewrite for every close/eviction.
}

const wxChar *const CSearchList::s_storedSearchesFilename = wxT("StoredSearches.met");

namespace
{
//! Namespaced names keep these file tags distinct from the result tags a search already uses.
const wxChar *const kSearchTagFileType = wxT("search.file_type");
const wxChar *const kSearchTagExtension = wxT("search.extension");
const wxChar *const kSearchTagMinSize = wxT("search.min_size_bytes");
const wxChar *const kSearchTagMaxSize = wxT("search.max_size_bytes");
const wxChar *const kSearchTagMinSourceCount = wxT("search.min_source_count");

//! Trailing-block marker, written once after the 0x01 records when any search has recorded
//! parameters. Trailing data with any other value means a layout this build does not know,
//! and the file is rejected.
const uint32 kStoredSearchesParamsMarker = 0x534D5052; // "SMPR"

//! Add a sparse tag. Zero and empty values are the defaults on read, so they are omitted.
void AddStringTag(TagPtrList &tags, const wxChar *name, const wxString &value)
{
	if (!value.IsEmpty()) {
		tags.push_back(new CTagString(name, value));
	}
}

void AddIntTag(TagPtrList &tags, const wxChar *name, uint64 value)
{
	if (value != 0) {
		tags.push_back(new CTagVarInt(name, value));
	}
}

//! A known name with the wrong type is corruption, not a value to default to zero or empty.
wxString ReadStringTag(const CTag &tag)
{
	if (!tag.IsStr()) {
		throw CInvalidPacket(CFormat("Search parameter tag '%s' is not a string") % tag.GetName());
	}
	return tag.GetStr();
}

uint64 ReadIntTag(const CTag &tag)
{
	if (!tag.IsInt()) {
		throw CInvalidPacket(CFormat("Search parameter tag '%s' is not an integer") % tag.GetName());
	}
	return tag.GetInt();
}

uint32 ReadUInt32Tag(const CTag &tag)
{
	const uint64 value = ReadIntTag(tag);
	if (value > std::numeric_limits<uint32>::max()) {
		throw CInvalidPacket(CFormat("Search parameter tag '%s' is out of range") % tag.GetName());
	}
	return static_cast<uint32>(value);
}

void WriteSearchParams(CFileDataIO &file, const CSearchList::CSearchParams &params)
{
	TagPtrList tags;
	AddStringTag(tags, kSearchTagFileType, params.typeText);
	AddStringTag(tags, kSearchTagExtension, params.extension);
	AddIntTag(tags, kSearchTagMinSize, params.minSize);
	AddIntTag(tags, kSearchTagMaxSize, params.maxSize);
	AddIntTag(tags, kSearchTagMinSourceCount, params.availability);

	file.WriteTagPtrList(tags);
	deleteTagPtrListEntries(&tags);
}

void ReadSearchParams(CFileDataIO &file, CSearchList::CSearchParams &params)
{
	params = CSearchList::CSearchParams();

	TagPtrList tags;
	file.ReadTagPtrList(&tags, true);
	for (const CTag *tag : tags) {
		const wxString &name = tag->GetName();
		if (name == kSearchTagFileType) {
			params.typeText = ReadStringTag(*tag);
		} else if (name == kSearchTagExtension) {
			params.extension = ReadStringTag(*tag);
		} else if (name == kSearchTagMinSize) {
			params.minSize = ReadIntTag(*tag);
		} else if (name == kSearchTagMaxSize) {
			params.maxSize = ReadIntTag(*tag);
		} else if (name == kSearchTagMinSourceCount) {
			params.availability = ReadUInt32Tag(*tag);
		} else {
			// Unknown names are additive: a filter written by a newer build is simply not
			// used by this one.
		}
	}
	deleteTagPtrListEntries(&tags);
}
} // namespace

bool CSearchList::StoreSearches(bool reportTruncation) const
{
	// Same contract as the search-terms dropdown this setting already governs (SearchDlg.cpp):
	// off means "don't remember", and a search's full result set is a stronger form of that
	// than the query string alone. LoadSearches() is the one that removes a file left over from
	// when this was still on.
	if (!thePrefs::RememberSearchHistory()) {
		return true;
	}

	CFile file(thePrefs::GetConfigDir() + s_storedSearchesFilename, CFile::write_safe);
	if (!file.IsOpened()) {
		return false;
	}

	// Oldest-first by start time, so if we have to drop any for the cap
	// below, we drop the oldest rather than an arbitrary map-ordered subset.
	std::vector<uint32_t> ids;
	ids.reserve(m_searchStrings.size());
	for (const auto &kv : m_searchStrings) {
		// Browses are deliberately not persisted. They are a snapshot of one peer's share taken
		// while that peer was connected, so restoring one resurrects a listing for someone very
		// likely gone -- and a large share would spend the MAX_STORED_SEARCHES budget that exists
		// for the user's own searches. They are in m_searchStrings only so the search list can
		// report them by peer name while they are live.
		std::map<uint32_t, SearchType>::const_iterator kind = m_searchKinds.find(kv.first);
		if (kind != m_searchKinds.end() && kind->second == BrowseSearch) {
			continue;
		}
		ids.push_back(kv.first);
	}
	std::sort(ids.begin(), ids.end(), [this](uint32_t a, uint32_t b) {
		return m_searchStartTimes.at(a) < m_searchStartTimes.at(b);
	});

	if (ids.size() > MAX_STORED_SEARCHES) {
		if (reportTruncation) {
			AddLogLineC(CFormat(_("Only saving the %u most recent of %u open searches to %s.")) %
				    MAX_STORED_SEARCHES % ids.size() % s_storedSearchesFilename);
		}
		ids.erase(ids.begin(), ids.end() - MAX_STORED_SEARCHES);
	}

	file.WriteUInt8(STOREDSEARCHES_VERSION);
	file.WriteUInt32(static_cast<uint32>(ids.size()));

	for (uint32_t id : ids) {
		const CSearchResultList &results = GetSearchResults(id);
		std::size_t resultCount = results.size();
		if (reportTruncation && resultCount > MAX_STORED_RESULTS_PER_SEARCH) {
			AddLogLineC(CFormat(_("Only saving the first %u of %u results for search \"%s\".")) %
				    MAX_STORED_RESULTS_PER_SEARCH % resultCount % m_searchStrings.at(id));
		}
		if (resultCount > MAX_STORED_RESULTS_PER_SEARCH) {
			resultCount = MAX_STORED_RESULTS_PER_SEARCH;
		}

		file.WriteUInt32(id);
		// m_searchStrings / m_searchKinds / m_searchStartTimes are written and erased together,
		// so `ids` drawn from m_searchStrings cannot miss -- which is what .at() throughout says.
		file.WriteString(m_searchStrings.at(id), utf8strRaw);
		file.WriteUInt8(static_cast<uint8>(m_searchKinds.at(id)));
		file.WriteUInt64(static_cast<uint64>(m_searchStartTimes.at(id)));

		file.WriteUInt32(static_cast<uint32>(resultCount));

		for (std::size_t i = 0; i < resultCount; ++i) {
			results[i]->WriteToFile(&file);
		}
	}

	// The start parameters ride in a trailing block after the 0x01 records. A build that
	// predates the block reads exactly the records it knows and never checks for end of
	// file, so downgrading keeps every search and result and loses only the filters; its
	// next save drops the block. The block itself is additive: each search's filters are
	// a named tag list, so a future filter is another tag, never a format change.
	uint32 paramCount = 0;
	for (uint32 id : ids) {
		if (m_searchParams.count(id) != 0) {
			++paramCount;
		}
	}
	if (paramCount != 0) {
		file.WriteUInt32(kStoredSearchesParamsMarker);
		file.WriteUInt32(paramCount);
		for (uint32 id : ids) {
			const std::map<uint32_t, CSearchParams>::const_iterator pi = m_searchParams.find(id);
			if (pi == m_searchParams.end()) {
				continue;
			}
			file.WriteUInt32(id);
			WriteSearchParams(file, pi->second);
		}
	}

	// write_safe writes to a .new sibling; only Close() performs the rename onto the real
	// filename. Without this call the .new file is left orphaned and StoredSearches.met itself
	// is never updated.
	return file.Close();
}

void CSearchList::MarkStoredSearchesDirty(uint32_t searchID)
{
	const auto kind = m_searchKinds.find(searchID);
	if (kind == m_searchKinds.end() || kind->second != BrowseSearch) {
		m_storedSearchesDirty = true;
	}
}

void CSearchList::StoreSearchesIfDirty(uint64_t uptimeMs)
{
	static const uint64_t STORE_INTERVAL_MS = 30000;
	if (!m_storedSearchesDirty || uptimeMs - m_lastStoredSearchesAttemptMs < STORE_INTERVAL_MS) {
		return;
	}

	// Rate-limit retries too, so an unwritable config directory does not cause an open/write
	// attempt on every core-timer tick. Keep the dirty flag set if the safe write fails.
	m_lastStoredSearchesAttemptMs = uptimeMs;
	try {
		if (StoreSearches(false)) {
			m_storedSearchesDirty = false;
		}
	} catch (const CIOFailureException &e) {
		AddLogLineC(CFormat(_("IO error while saving %s: %s")) % s_storedSearchesFilename % e.what());
	}
}

namespace
{
//! One search record as read from disk, before it is committed into the live maps. Kept
//! separate so a malformed record partway through the file can discard everything read so
//! far instead of leaving CSearchList half-loaded.
struct LoadedSearch
{
	uint32_t id;
	wxString queryString;
	SearchType kind;
	time_t startTime;
	bool hasParams = false;            // true when the trailing block records this search's form
	CSearchList::CSearchParams params; // empty for a search the file records no form for
	std::vector<std::unique_ptr<CSearchFile>> results;
};
} // namespace

std::vector<uint32_t> CSearchList::LoadSearches()
{
	std::vector<uint32_t> restored;

	CPath fullpath = CPath(thePrefs::GetConfigDir() + s_storedSearchesFilename);
	if (!thePrefs::RememberSearchHistory()) {
		// Remove whatever an earlier session with the setting on left behind, so turning it off
		// actually clears what is on disk rather than just stopping new writes.
		if (fullpath.FileExists()) {
			CPath::RemoveFile(fullpath);
		}
		return restored;
	}
	if (!fullpath.FileExists()) {
		return restored;
	}

	CFile file;
	if (!file.Open(fullpath)) {
		AddLogLineC(CFormat(_("WARNING: %s cannot be opened.")) % s_storedSearchesFilename);
		return restored;
	}

	std::vector<LoadedSearch> loaded;

	try {
		uint8 version = file.ReadUInt8();
		if (version != STOREDSEARCHES_VERSION) {
			AddLogLineC(_("WARNING: Stored search list corrupted, contains invalid header."));
			return restored;
		}

		uint32 searchCount = file.ReadUInt32();
		if (searchCount > MAX_STORED_SEARCHES * 2) {
			// StoreSearches() never writes more than MAX_STORED_SEARCHES; a count more than
			// double that is not a file we ever wrote, and trusting it would drive an equally
			// suspect per-search loop below. Fail closed rather than partially populate.
			AddLogLineC(_("WARNING: Stored search list corrupted, contains invalid header."));
			return restored;
		}
		loaded.reserve(searchCount);

		for (uint32 i = 0; i < searchCount; ++i) {
			LoadedSearch entry;
			entry.id = file.ReadUInt32();
			entry.queryString = file.ReadString(true);
			entry.kind = static_cast<SearchType>(file.ReadUInt8());
			entry.startTime = static_cast<time_t>(file.ReadUInt64());

			uint32 resultCount = file.ReadUInt32();
			if (resultCount > MAX_STORED_RESULTS_PER_SEARCH * 2) {
				AddLogLineC(
					_("WARNING: Stored search list corrupted, contains invalid header."));
				return std::vector<uint32_t>();
			}

			entry.results.reserve(resultCount);
			for (uint32 r = 0; r < resultCount; ++r) {
				auto result = CSearchFile::LoadFromFile(&file);
				if (!result) {
					AddLogLineC(_(
						"Invalid entry in stored search list, file may be corrupt"));
					return std::vector<uint32_t>();
				}
				entry.results.push_back(std::move(result));
			}

			loaded.push_back(std::move(entry));
		}

		// Trailing start-parameter block, keyed by search id. Absent for a file a build
		// that predates the block wrote: those searches restore without a recorded form.
		// Trailing data without the marker is rejected like an unknown header, so a
		// layout this build does not know is never half-read.
		if (!file.Eof()) {
			if (file.ReadUInt32() != kStoredSearchesParamsMarker) {
				AddLogLineC(
					_("WARNING: Stored search list corrupted, contains invalid header."));
				return restored;
			}
			uint32 paramCount = file.ReadUInt32();
			if (paramCount > MAX_STORED_SEARCHES * 2) {
				AddLogLineC(
					_("WARNING: Stored search list corrupted, contains invalid header."));
				return restored;
			}
			for (uint32 i = 0; i < paramCount; ++i) {
				const uint32 id = file.ReadUInt32();
				LoadedSearch *entry = nullptr;
				for (LoadedSearch &candidate : loaded) {
					if (candidate.id == id) {
						entry = &candidate;
						break;
					}
				}
				if (entry == nullptr) {
					throw CInvalidPacket(
						CFormat("Parameter block references unknown search id %u") %
						id);
				}
				ReadSearchParams(file, entry->params);
				entry->hasParams = true;
			}
		}
	} catch (const CInvalidPacket &e) {
		AddLogLineC(_("Invalid entry in stored search list, file may be corrupt") + ": " + e.what());
		return restored;
	} catch (const CSafeIOException &e) {
		AddLogLineC(CFormat(_("IO error while reading %s file: %s")) % s_storedSearchesFilename %
			    e.what());
		return restored;
	}

	// Everything parsed cleanly -- commit it all to the live maps/index.
	// theApp->downloadqueue/knownfiles/canceledfiles must already exist here (see amule.cpp's
	// call site) for SetDownloadStatus() to be meaningful.
	restored.reserve(loaded.size());
	for (LoadedSearch &entry : loaded) {
		m_searchStrings[entry.id] = entry.queryString;
		m_searchKinds[entry.id] = entry.kind;
		m_searchStartTimes[entry.id] = entry.startTime;
		// Only a search the file actually recorded parameters for carries a form: a
		// pre-block restore must leave GetSearchParams() at nullptr so the listing omits
		// the tags rather than reporting a form of all defaults as if the user typed them.
		if (entry.hasParams) {
			m_searchParams[entry.id] = entry.params;
		}

		// Reserve the id so the first new search of the same kind this session cannot be handed
		// it -- both counters restart every launch, so without this a restored search collides
		// with the next one started. Partitioned on the id's own high bit rather than the
		// persisted `kind` byte: bit 31 is intrinsic to which counter owns the value, while
		// `kind` is a separate field that could disagree with it if the file were corrupt.
		if (entry.id == 0xffffffff) {
			// The legacy single-search bucket comes from neither counter: every EC client
			// predating multi-search reuses this one id for all its searches, so there is no
			// allocation to advance past. It has to be excluded explicitly because the bit-31
			// test below would hand an ed2k search to the Kad counter -- and since this is the
			// largest uint32 there is, ReserveSearchId would pin m_nextID at its maximum and the
			// next Kad allocation would wrap to the FIRST Kad id.
		} else if (entry.id & 0x80000000) {
			Kademlia::CSearchManager::ReserveSearchId(entry.id);
		} else {
			ReserveEd2kId(entry.id);
		}

		if (entry.kind == KadSearch || entry.kind == AllSearch) {
			// The original in-flight Kad search can't survive a restart --
			// come back already finished, hits-only, "More results" disabled.
			m_finishedKadSearches.insert(entry.id);
		}

		for (auto &root : entry.results) {
			root->m_searchID = entry.id;
			root->SetDownloadStatus();
			for (CSearchFile *child : root->GetChildren()) {
				child->m_searchID = entry.id;
				child->SetDownloadStatus();
			}
			IndexResult(root.get());
			root.release(); // The live list owns it only after indexing succeeds.
		}

		restored.push_back(entry.id);
	}

	return restored;
}

wxString CSearchList::StartNewSearch(uint32 *searchID, SearchType type, CSearchParams &params)
{
	// Check that we can actually perform the specified desired search.
	if (type == AllSearch) {
		if (!theApp->IsConnectedED2K() && !Kademlia::CKademlia::IsRunning()) {
			return _("All search can't be done if neither eD2k nor Kad is available");
		}
	} else if ((type == KadSearch) && !Kademlia::CKademlia::IsRunning()) {
		return _("Kad search can't be done if Kad is not running");
	} else if ((type != KadSearch) && !theApp->IsConnectedED2K()) {
		return _("eD2k search can't be done if eD2k is not connected");
	}

	bool startKad = type == KadSearch || (type == AllSearch && Kademlia::CKademlia::IsRunning());
	if (startKad) {
		Kademlia::WordList words;
		Kademlia::CSearchManager::GetWords(params.searchString, &words);
		if (!words.empty()) {
			params.strKeyword = words.front();
		} else if (type == AllSearch && theApp->IsConnectedED2K()) {
			startKad = false;
			AddLogLineN(_("All search: no usable Kad keyword; continuing with eD2k."));
		} else {
			return _("No keyword for Kad search - aborting");
		}
	}

	bool supports64bit = type == KadSearch
				     ? true
				     : theApp->serverconnect->GetCurrentServer() != NULL &&
					       (theApp->serverconnect->GetCurrentServer()->GetTCPFlags() &
						       SRV_TCPFLG_LARGEFILES);
	bool packetUsing64bit;

	// This MemFile is automatically free'd
	// Server queries must keep the routing keyword; Kad queries omit it because
	// the DHT target already supplies it. Never share a serialized query between them.
	CMemFilePtr data = CreateSearchData(
		params, type == AllSearch ? GlobalSearch : type, supports64bit, packetUsing64bit);
	CMemFilePtr kadData;
	if (data.get() && type == AllSearch && startKad) {
		bool kadUsing64bit;
		kadData = CreateSearchData(params, KadSearch, true, kadUsing64bit);
		if (!kadData.get()) {
			data.reset();
		}
	}

	if (data.get() == NULL) {
		wxASSERT(_astrParserErrors.GetCount());
		wxString error;

		for (unsigned int i = 0; i < _astrParserErrors.GetCount(); ++i) {
			error += _astrParserErrors[i] + "\n";
		}

		return error;
	}

	const CKadAICHVotes::Key evidenceKey = startKad ? CKadAICHVotes::GenerateKey() : CKadAICHVotes::Key{};

	// The scalar m_searchType / m_currentSearch are the anchor for the single in-flight ed2k
	// (local/global) search: its results arrive asynchronously for several seconds and are
	// attributed via these scalars. A Kad search started ALONGSIDE an in-flight ed2k search has
	// its own per-ID machinery and needs neither scalar -- so it must not repoint them, or the
	// ed2k search's late hits get dropped (wrong type) or misfiled (wrong bucket). Every other
	// start updates the anchor as before.
	const bool preserveEd2kAnchor = (type == KadSearch) && m_searchInProgress;
	CSearchStartBookkeeping pendingStart(m_searchType, m_searchStart, type, preserveEd2kAnchor);

	// Legacy EC clients reuse the sentinel across networks. Ensure that no old
	// Kad search can still deliver results to the bucket before reusing it.
	if (*searchID == 0xffffffff) {
		Kademlia::CSearchManager::StopSearch(0xffffffff, false);
	}

	if (type == KadSearch) {
		try {
			// searchstring will get tokenized there
			// The tab must be created with the Kad search ID, so searchID is updated.
			Kademlia::CSearch *search = Kademlia::CSearchManager::PrepareFindKeywords(
				params.strKeyword, data->GetLength(), data->GetRawBuffer(), *searchID);

			*searchID = search->GetSearchID();
			m_kadAICHKeys[*searchID] = evidenceKey;
			// Do not repoint the ed2k result-attribution scalar when a Kad search runs alongside
			// an in-flight ed2k search (see preserveEd2kAnchor above); the Kad search is tracked
			// by its own ID regardless.
			if (!preserveEd2kAnchor) {
				m_currentSearch = *searchID;
			}
			m_KadSearchFinished = false;
		} catch (const wxString &what) {
			AddLogLineC(what);
			return _("Unexpected error while attempting Kad search: ") + what;
		}

	} else {
		// AllSearch owns an eD2k ID even when only Kad is available.
		if (type == AllSearch && kadData.get()) {
			try {
				// Always allocate a distinct Kad ID, including for legacy EC's sentinel.
				Kademlia::CSearch *search = Kademlia::CSearchManager::PrepareFindKeywords(
					params.strKeyword, kadData->GetLength(), kadData->GetRawBuffer(), 0);
				m_kadToEd2kSearchId[search->GetSearchID()] = *searchID;
				m_kadAICHKeys[*searchID] = evidenceKey;
			} catch (const wxString &what) {
				if (!theApp->IsConnectedED2K()) {
					return _("Unexpected error while attempting Kad search: ") + what;
				}
				AddLogLineN(_("All search: Kad component could not start: ") + what);
			}
		}
		// Replace the single eD2k slot only after query validation and Kad startup
		// have succeeded. All entry points share this ordering; rejected requests
		// and independent Kad searches must not terminate the previous search.
		StopInFlightEd2kSearch();
		m_currentSearch = *(searchID);
		m_askedServers.clear();
		m_searchInProgress = theApp->IsConnectedED2K();
		m_ed2kSearchFinished = !m_searchInProgress;

		if (m_searchInProgress) {
			auto searchPacket =
				std::make_unique<CPacket>(*data, OP_EDONKEYPROT, OP_SEARCHREQUEST);

			theStats::AddUpOverheadServer(searchPacket->GetPacketSize());
			theApp->serverconnect->SendPacket(searchPacket.get(), false);

			// Bound the wait for the server's answer, for either kind: a local search has no
			// other terminal path, and a global one's sweep is armed by the very answer we are
			// waiting for.
			m_awaitingServerAnswer = true;
			m_searchTimer.Start(SERVER_ANSWER_TIMEOUT_MS, true /* one shot */);

			if (type == GlobalSearch || type == AllSearch) {
				m_searchPacket = std::move(searchPacket);
				m_64bitSearchPacket = packetUsing64bit;
				m_searchPacket->SetOpCode(OP_GLOBSEARCHREQ); // will be changed later when
									     // actually sending the packet!!
			}
		}
	}

	// Commit the anchor only after a successful start. Rejected Kad keywords,
	// as well as key-generation failures, must not relabel the previous search.
	const time_t searchStart = time(nullptr);
	pendingStart.Commit(searchStart);

	// Record this search's own start time so its (cosmetic Kad) progress ramp is computed from
	// *its* age even after it is no longer the most-recently-started search -- otherwise a Kad
	// search running in parallel with a later ed2k search would report a fixed near-full
	// percent.
	m_searchStartTimes[static_cast<uint32_t>(*searchID)] = searchStart;
	// Record this search's kind by id (same reason as the start time above): a later search of
	// a different type must not make an older tab report the wrong kind. `type` is this
	// search's real type regardless of the scalar anchor bookkeeping.
	m_searchKinds[static_cast<uint32_t>(*searchID)] = type;
	m_searchStrings[static_cast<uint32_t>(*searchID)] = params.searchString;
	m_searchParams[static_cast<uint32_t>(*searchID)] = params;
	MarkStoredSearchesDirty(static_cast<uint32_t>(*searchID));
	// Concurrent Kad searches can have different filters. Apply each query's own
	// filter rather than whichever type was most recently submitted.
	if (!params.typeText.IsEmpty() && params.typeText != ED2KFTSTR_PROGRAM &&
		params.typeText.CmpNoCase("Any")) {
		m_resultTypes[*searchID] = params.typeText;
	} else {
		m_resultTypes.erase(*searchID);
	}

	// Tell the GUI a search now exists. Every producer funnels through here -- the monolithic
	// dialog and the EC_OP_SEARCH_START handler alike -- so this one call covers a search
	// started by any client. The monolithic GUI's own searches already have a tab by this point
	// and are filtered out on the handler side; what this adds is the tab for a search some
	// OTHER client started.
	Notify_Search_Added(
		static_cast<wxUIntPtr>(*searchID), params.searchString, static_cast<uint32>(type));

	return "";
}

void CSearchList::LocalSearchEnd()
{
	if (!m_searchInProgress) {
		// Nothing left for this reply to end: the wait for it ran out, or the search was stopped.
		// Without this, a late answer to a terminalized global search reaches the wxCHECK_RET below
		// with the packet already released -- silent under NDEBUG, an assertion in a Debug build.
		return;
	}

	// The answer is in; from here the global branch's sweep bounds itself and the local branch
	// is already terminal, so the one-shot has nothing left to guard. Cleared before either
	// branch, since the global one restarts the same timer as the sweep ticker.
	m_awaitingServerAnswer = false;

	if (m_searchType == GlobalSearch || m_searchType == AllSearch) {
		wxCHECK_RET(m_searchPacket, "Global/All search, but no packet");

		// Ensure that every global search starts over.
		theApp->serverlist->RemoveObserver(&m_serverQueue);
		m_searchTimer.Start(750);
	} else {
		FinalizeLocalSearch();
	}
}

void CSearchList::FinalizeLocalSearch()
{
	m_searchTimer.Stop();
	m_awaitingServerAnswer = false;
	m_searchInProgress = false;
	m_ed2kSearchFinished = true;

	if (!m_shuttingDown) {
		Notify_SearchLocalEnd();
	}
}

uint32 CSearchList::GetSearchProgress() const
{
	if (m_searchType == AllSearch) {
		const bool kadActive = HasKadComponent(static_cast<uint32_t>(m_currentSearch));
		if (!m_searchInProgress && !kadActive) {
			return 0xfffe;
		}
		uint32 sweep = 100;
		if (m_searchInProgress) {
			const uint32 servers = theApp->serverlist->GetServerCount();
			sweep = m_serverQueue.IsActive() && servers
					? 100 - (m_serverQueue.GetRemaining() * 100) / servers
					: 0;
		}
		const auto it = m_searchStartTimes.find(static_cast<uint32_t>(m_currentSearch));
		const time_t start = it != m_searchStartTimes.end() ? it->second : m_searchStart;
		const time_t elapsed = std::max<time_t>(0, time(nullptr) - start);
		const uint32 kad = kadActive ? static_cast<uint32>(std::min<time_t>(
						       99, elapsed * 100 / SEARCHKEYWORD_LIFETIME))
					     : 100;
		return std::min(sweep, kad);
	}
	if (m_searchType == KadSearch) {
		// We cannot measure the progress of Kad searches.
		// But we can tell when they are over.
		return m_KadSearchFinished ? 0xfffe : 0;
	}
	if (m_searchInProgress == false) { // true only for ED2K search
		// No search, no progress ;)
		return 0;
	}

	switch (m_searchType) {
	case LocalSearch:
		return 0xffff;

	case GlobalSearch:
	case AllSearch:
		// The sweep is not armed until the connected server answers the local part and the first
		// timer tick attaches the observer. Until then m_serverQueue is detached and empty, so
		// GetRemaining() is a stale 0 that would read as 100% ("done") the instant a search
		// starts. IsActive() is true only during the actual sweep, so report 0 before it begins.
		if (!m_serverQueue.IsActive()) {
			return 0;
		}
		return 100 - (m_serverQueue.GetRemaining() * 100) / theApp->serverlist->GetServerCount();

	default:
		wxFAIL;
	}
	return 0;
}

CSearchList::SearchLifecycleState CSearchList::GetSearchLifecycleState() const
{
	// m_currentSearch defaults to wxUIntPtr(-1) at construction and after an explicit
	// StopSearch. A natural global-search completion (via FinalizeGlobalSearch from
	// OnGlobalSearchTimer) preserves it, so completed-then-idle-view still reports FINISHED
	// here.
	if (m_currentSearch == wxUIntPtr(-1)) {
		return SEARCH_LIFECYCLE_IDLE;
	}
	if (m_searchType == KadSearch) {
		return m_KadSearchFinished ? SEARCH_LIFECYCLE_FINISHED : SEARCH_LIFECYCLE_RUNNING;
	}
	if (m_searchType == AllSearch && HasKadComponent(static_cast<uint32_t>(m_currentSearch))) {
		return SEARCH_LIFECYCLE_RUNNING;
	}
	// The eD2k component is tracked separately from all concurrent Kad searches.
	return m_ed2kSearchFinished ? SEARCH_LIFECYCLE_FINISHED : SEARCH_LIFECYCLE_RUNNING;
}

std::size_t CSearchList::GetCurrentSearchResultCount() const
{
	if (m_currentSearch == wxUIntPtr(-1)) {
		return 0;
	}
	return GetSearchResults(m_currentSearch).size();
}

uint8 CSearchList::GetSearchLifecyclePercent() const
{
	switch (GetSearchLifecycleState()) {
	case SEARCH_LIFECYCLE_IDLE:
		return 0;
	case SEARCH_LIFECYCLE_FINISHED:
		// Authoritative completion edge for every search kind.
		return 100;
	case SEARCH_LIFECYCLE_RUNNING:
		break;
	}

	// --- RUNNING ---
	if (m_searchType == KadSearch) {
		// Kad has no measurable progress, so synthesise a cosmetic ramp from the fixed
		// keyword-search lifetime. The FINISHED state above is what snaps it to 100; capped at 99
		// so the ramp never claims completion before the daemon actually does.
		time_t elapsed = time(NULL) - m_searchStart;
		if (elapsed <= 0) {
			return 0;
		}
		uint32 pct = (uint32)((elapsed * 100) / SEARCHKEYWORD_LIFETIME);
		return (pct > 99) ? 99 : (uint8)pct;
	}

	if (m_searchType == GlobalSearch || m_searchType == AllSearch) {
		// Real server-queue-driven percent (0..100).
		uint32 pct = GetSearchProgress();
		return (pct > 100) ? 100 : (uint8)pct;
	}

	// LocalSearch is instantaneous and never observed RUNNING here.
	return 0;
}

void CSearchList::OnGlobalSearchTimer(CTimerEvent &WXUNUSED(evt))
{
	if (m_awaitingServerAnswer && m_searchInProgress) {
		// The one-shot armed at StartNewSearch: the connected server never sent OP_SEARCHRESULT,
		// so neither kind of ed2k search has anything left that would end it. Terminalize through
		// the finalizer the kind already has, rather than leave it reporting RUNNING for good.
		// Tested before the packet check below, which a local search would otherwise fall into.
		AddLogLineN(_("Search timed out: the server did not answer."));
		if (m_searchType == GlobalSearch || m_searchType == AllSearch) {
			FinalizeGlobalSearch();
		} else {
			FinalizeLocalSearch();
		}
		return;
	}

	// Ensure that the server-queue contains the current servers.
	if (!m_searchPacket) {
		// This was a pending event, handled after 'Stop' was pressed.
		return;
	} else if (!m_serverQueue.IsActive()) {
		theApp->serverlist->AddObserver(&m_serverQueue);
	}

	// UDP requests must not be sent to this server.
	const CServer *localServer = theApp->serverconnect->GetCurrentServer();
	if (localServer) {
		uint32 localIP = localServer->GetIP();
		uint16 localPort = localServer->GetPort();
		while (m_serverQueue.GetRemaining()) {
			CServer *server = m_serverQueue.GetNext();

			// Compare against the currently connected server.
			if ((server->GetPort() == localPort) && (server->GetIP() == localIP)) {
				// We've already requested from the local server.
				continue;
			} else {
				if (server->SupportsLargeFilesUDP() &&
					(server->GetUDPFlags() & SRV_UDPFLG_EXT_GETFILES)) {
					CMemFile data(50);
					uint32_t tagCount = 1;
					data.WriteUInt32(tagCount);
					CTagVarInt flags(
						CT_SERVER_UDPSEARCH_FLAGS, SRVCAP_UDP_NEWTAGS_LARGEFILES);
					flags.WriteNewEd2kTag(&data);
					CPacket *extSearchPacket = new CPacket(OP_GLOBSEARCHREQ3,
						m_searchPacket->GetPacketSize() + (uint32_t)data.GetLength(),
						OP_EDONKEYPROT);
					extSearchPacket->CopyToDataBuffer(
						0, data.GetRawBuffer(), data.GetLength());
					extSearchPacket->CopyToDataBuffer(data.GetLength(),
						m_searchPacket->GetDataBuffer(),
						m_searchPacket->GetPacketSize());
					theStats::AddUpOverheadServer(extSearchPacket->GetPacketSize());
					theApp->serverconnect->SendUDPPacket(extSearchPacket, server, true);
					m_askedServers.insert(server->ECID());
					AddDebugLogLineN(logServerUDP,
						"Sending OP_GLOBSEARCHREQ3 to server " +
							Uint32_16toStringIP_Port(
								server->GetIP(), server->GetPort()));
				} else if (server->GetUDPFlags() & SRV_UDPFLG_EXT_GETFILES) {
					if (!m_64bitSearchPacket || server->SupportsLargeFilesUDP()) {
						m_searchPacket->SetOpCode(OP_GLOBSEARCHREQ2);
						AddDebugLogLineN(logServerUDP,
							"Sending OP_GLOBSEARCHREQ2 to server " +
								Uint32_16toStringIP_Port(
									server->GetIP(), server->GetPort()));
						theStats::AddUpOverheadServer(
							m_searchPacket->GetPacketSize());
						theApp->serverconnect->SendUDPPacket(
							m_searchPacket.get(), server, false);
						m_askedServers.insert(server->ECID());
					} else {
						AddDebugLogLineN(logServerUDP,
							"Skipped UDP search on server " +
								Uint32_16toStringIP_Port(
									server->GetIP(), server->GetPort()) +
								": No large file support");
					}
				} else {
					if (!m_64bitSearchPacket || server->SupportsLargeFilesUDP()) {
						m_searchPacket->SetOpCode(OP_GLOBSEARCHREQ);
						AddDebugLogLineN(logServerUDP,
							"Sending OP_GLOBSEARCHREQ to server " +
								Uint32_16toStringIP_Port(
									server->GetIP(), server->GetPort()));
						theStats::AddUpOverheadServer(
							m_searchPacket->GetPacketSize());
						theApp->serverconnect->SendUDPPacket(
							m_searchPacket.get(), server, false);
						m_askedServers.insert(server->ECID());
					} else {
						AddDebugLogLineN(logServerUDP,
							"Skipped UDP search on server " +
								Uint32_16toStringIP_Port(
									server->GetIP(), server->GetPort()) +
								": No large file support");
					}
				}
				CoreNotify_Search_Update_Progress(GetSearchProgress());
				return;
			}
		}
	}
	// No more servers left to ask. Natural completion -- preserve m_currentSearch so
	// GetSearchLifecycleState reports FINISHED, not IDLE.
	FinalizeGlobalSearch();
}

void CSearchList::ProcessSharedFileList(const uint8_t *in_packet,
	uint32 size,
	CUpDownClient *sender,
	bool *moreResultsAvailable,
	const wxString &directory)
{
	wxCHECK_RET(sender, "No sender in search-results from client.");

	// Route the browsed listing to a result bucket. Every browse has a real, wire-safe search
	// ID by now -- pinned by the EC handler for a remote one, allocated by RequestSharedFileList
	// for a local one -- so the union and per-ID polls and the LRU ring can all address these
	// results.
	//
	// A local browse used to key its results on the client pointer, which no remote client can
	// address, so the browse was invisible to amulegui, amuleweb and amuleapi while an
	// EC-initiated one showed up everywhere. A pointer is also reused once the client is freed,
	// so two browses of different peers could collide on one key.
	//
	// Allocated once and pinned on the client, not per packet -- a browse arrives in several --
	// and registered so it is listed like any other search, with its kind and peer, which is
	// what lets a remote GUI rebuild it as a browse tab.
	//
	// Whether this browse was asked for HERE is read before the id is assigned below, because
	// that assignment is what makes a local browse look like an EC one afterwards. An
	// EC-initiated browse belongs to another client, so revealing it would pull this user's
	// panel and selection away.
#ifndef AMULE_DAEMON
	const bool ecInitiated = sender->IsBrowseEcInitiated();
#endif

	wxUIntPtr searchID = static_cast<wxUIntPtr>(sender->GetBrowseSearchId());

#ifndef AMULE_DAEMON
	// Find-or-create the peer's "View Files" tab, keyed by ECID (so two peers sharing a nick do
	// not collapse into one tab, and a re-browse refreshes the same tab). Marks the tab as
	// browsing; the terminal paths flip it to finished/failed via Notify_Browse_Status.
	theApp->amuledlg->m_searchwnd->EnsureBrowseTab(
		sender->ECID(), sender->GetUserName(), searchID, !ecInitiated);
#endif

	const CMemFile packet(in_packet, size);
	uint32 results = packet.ReadUInt32();
	bool unicoded = (sender->GetUnicodeSupport() != utf8strNone);
	for (unsigned int i = 0; i != results; ++i) {
		auto toadd = std::make_unique<CSearchFile>(packet, unicoded, searchID, 0, 0, directory);
		toadd->SetClientID(sender->GetUserIDHybrid());
		toadd->SetClientPort(sender->GetUserPort());
		AddToList(std::move(toadd), true);
	}

	if (moreResultsAvailable)
		*moreResultsAvailable = false;

	int iAddData = (int)(packet.GetLength() - packet.GetPosition());
	if (iAddData == 1) {
		uint8 ucMore = packet.ReadUInt8();
		if (ucMore == 0x00 || ucMore == 0x01) {
			if (moreResultsAvailable) {
				*moreResultsAvailable = (ucMore == 1);
			}
		}
	}
}

// Symmetric counterpart to the Kad hard-stop on EC search-start. Late ed2k server replies --
// TCP Local responses here, UDP Global responses in ProcessUDPSearchAnswer below -- keep
// arriving for seconds after the search request is sent. When an EC client switches search
// type (ed2k -> Kad) those late results would land in `m_results[m_currentSearch]`, and with
// the EC sentinel pinned across all EC searches that bucket is now the new Kad search's,
// producing ed2k contamination of a Kad result list.
//
// Native-GUI parallel searches keep m_currentSearch at bottom-half IDs and a Kad tab updates
// m_searchType, so this gate also stops late ed2k packets misrouting to the Kad tab's bucket
// -- a pre-existing GUI-side bug that went unnoticed because cross-protocol hits in a Kad
// tab look like noise.
static inline bool IsActiveSearchTypeEd2k(SearchType t)
{
	return t == LocalSearch || t == GlobalSearch || t == AllSearch;
}

bool CSearchList::CanFileServerAnswer() const
{
	// Late server replies keep arriving after a search is over, and where they may be filed is
	// not the same question as whether the search is still running. A terminalized search still
	// owns its id -- both the timeout and the sweep's natural drain leave m_currentSearch alone
	// -- so its results have a bucket of their own and are worth keeping.
	//
	// An explicit stop is what does not: it hands m_currentSearch back to the sentinel the EC
	// handler pins across all its searches, and filing server hits there contaminates the bucket
	// StartNewSearch keeps clean.
	return IsActiveSearchTypeEd2k(m_searchType) && m_currentSearch != wxUIntPtr(-1);
}

void CSearchList::ProcessSearchAnswer(
	const uint8_t *in_packet, uint32_t size, bool optUTF8, uint32_t serverIP, uint16_t serverPort)
{
	if (!CanFileServerAnswer()) {
		return;
	}
	CMemFile packet(in_packet, size);

	uint32_t results = packet.ReadUInt32();
	for (; results > 0; --results) {
		auto file =
			std::make_unique<CSearchFile>(packet, optUTF8, m_currentSearch, serverIP, serverPort);
		AddToList(std::move(file), false);
	}
}

void CSearchList::ProcessUDPSearchAnswer(
	const CMemFile &packet, bool optUTF8, uint32_t serverIP, uint16_t serverPort)
{
	if (!CanFileServerAnswer()) {
		return;
	}
	auto file = std::make_unique<CSearchFile>(packet, optUTF8, m_currentSearch, serverIP, serverPort);
	AddToList(std::move(file), false);
}

bool CSearchList::WasServerAsked(uint32 serverIP) const
{
	for (uint32 ecid : m_askedServers) {
		const CServer *server = theApp->serverlist->GetServerByECID(ecid);
		if (server && server->GetIP() == serverIP) {
			return true;
		}
	}
	return false;
}

bool CSearchList::AddToList(std::unique_ptr<CSearchFile> owned, bool clientResponse)
{
	CSearchFile *toadd = owned.get();
	const uint32_t searchID = static_cast<uint32_t>(toadd->GetSearchID());
	const uint64 fileSize = toadd->GetFileSize();
	// If filesize is 0, or file is too large for the network, drop it
	if ((fileSize == 0) || (fileSize > MAX_FILE_SIZE)) {
		AddDebugLogLineN(logSearch,
			CFormat("Dropped result with filesize %u: %s") % fileSize % toadd->GetFileName());

		return false;
	}

	// If the result was not the type the user wanted, drop it.
	const auto filter = m_resultTypes.find(static_cast<uint32_t>(toadd->GetSearchID()));
	if (!clientResponse && filter != m_resultTypes.end() && !filter->second.IsEmpty()) {
		if (GetFileTypeByName(toadd->GetFileName()) != filter->second) {
			AddDebugLogLineN(logSearch,
				CFormat("Dropped result type %s != %s, file %s") %
					GetFileTypeByName(toadd->GetFileName()) % filter->second %
					toadd->GetFileName());

			return false;
		}
	}

	// Scan the results already indexed for this search (empty if it is the first one) for a
	// duplicate; the new result is indexed further down, through IndexResult().
	const CSearchResultList &results = GetSearchResults(toadd->GetSearchID());

	for (size_t i = 0; i < results.size(); ++i) {
		CSearchFile *item = results.at(i);

		if ((toadd->GetFileHash() == item->GetFileHash()) &&
			(toadd->GetFileSize() == item->GetFileSize())) {
			AddDebugLogLineN(logSearch,
				CFormat("Received duplicate results for '%s' : %s") % item->GetFileName() %
					item->GetFileHash().Encode());
			// Add the child, possibly updating the parents filename.
			const size_t childrenBefore = item->GetChildren().size();
			item->AddChild(owned.release());
			// AddChild() MERGES a duplicate filename into an existing child and deletes the file
			// it was handed, on two of its four paths -- notifying with `toadd` afterwards would
			// then read freed memory (got3nks, PR #796 review). Infer survival from whether the
			// child count actually grew.
			const bool survived = item->GetChildren().size() > childrenBefore;
			// Structural change -- leaf-or-nothing to container, or a new row under an existing
			// container -- needs its own notification. Search_Update_Sources only signals that
			// the parent's values changed, via wxDataViewModel::ItemChanged(), which some
			// wxDataViewCtrl backends (GTK, MSW) do not treat as reason to re-check IsContainer()
			// or re-fetch children, so the child never becomes reachable there. Native
			// NSOutlineView re-queries IsContainer() on every draw, which masked it.
			if (survived) {
				Notify_Search_Add_Result(toadd);
			}
			Notify_Search_Update_Sources(item);
			MarkStoredSearchesDirty(searchID);
			return true;
		}
	}

	AddDebugLogLineN(logSearch,
		CFormat("Added new result '%s' : %s") % toadd->GetFileName() % toadd->GetFileHash().Encode());

	// New unique result, simply add and display.
	IndexResult(toadd);
	owned.release();
	Notify_Search_Add_Result(toadd);
	MarkStoredSearchesDirty(searchID);

	return true;
}

void CSearchList::AddFileToDownloadByHash(const CMD4Hash &hash, uint8 cat)
{
	for (const auto &entry : AllResults()) {
		for (CSearchFile *sf : entry.second) {
			if (sf->GetFileHash() == hash) {
				CoreNotify_Search_Add_Download(sf, cat);

				return;
			}
		}
	}
}

CSearchFile *CSearchList::GetSearchFileByID(const CMD4Hash &hash) const
{
	for (const auto &entry : AllResults()) {
		for (CSearchFile *sf : entry.second) {
			if (sf->GetFileHash() == hash) {
				return sf;
			}
			for (CSearchFile *child : sf->GetChildren()) {
				if (child->GetFileHash() == hash) {
					return child;
				}
			}
		}
	}
	return nullptr;
}

void CSearchList::GetAllSearchFilesByID(const CMD4Hash &hash, std::vector<CSearchFile *> &out) const
{
	// Unlike GetSearchFileByID (first match only), collect EVERY result object that shares this
	// hash. The same file can appear in more than one open search, and each keeps its own
	// CSearchFile with its own note list. On-demand Kad notes and the running flag must reach
	// all of them.
	for (const auto &entry : AllResults()) {
		for (CSearchFile *sf : entry.second) {
			if (sf->GetFileHash() == hash) {
				out.push_back(sf);
			}
			for (CSearchFile *child : sf->GetChildren()) {
				if (child->GetFileHash() == hash) {
					out.push_back(child);
				}
			}
		}
	}
}

void CSearchList::AddFileToDownloadByEcid(uint32 ecid, uint8 cat)
{
	// Match against parents and their same-hash/different-name children (issue #431 grouping);
	// downloading the specific CSearchFile lands the partfile under that result's own filename.
	for (const auto &entry : AllResults()) {
		for (CSearchFile *sf : entry.second) {
			if (sf->ECID() == ecid) {
				CoreNotify_Search_Add_Download(sf, cat);
				return;
			}
			if (sf->HasChildren()) {
				for (CSearchFile *child : sf->GetChildren()) {
					if (child->ECID() == ecid) {
						CoreNotify_Search_Add_Download(child, cat);
						return;
					}
				}
			}
		}
	}
}

bool CSearchList::IsKadSearch(uint32_t searchID) const
{
	return Kademlia::CSearchManager::IsKadSearch(searchID);
}

bool CSearchList::HasKadComponent(uint32_t searchID) const
{
	if (IsKadSearch(searchID) && !m_finishedKadSearches.count(searchID)) {
		return true;
	}
	const uint32_t kadID = KadComponentOf(searchID);
	return IsKadSearch(kadID) && !m_finishedKadSearches.count(kadID);
}

uint32_t CSearchList::KadComponentOf(uint32_t searchID) const
{
	for (const auto &entry : m_kadToEd2kSearchId) {
		if (entry.second == searchID) {
			return entry.first;
		}
	}
	return searchID;
}

uint32_t CSearchList::GetEffectiveSearchId(uint32_t searchID) const
{
	const auto it = m_kadToEd2kSearchId.find(searchID);
	return it != m_kadToEd2kSearchId.end() ? it->second : searchID;
}

bool CSearchList::IsOrWasKadSearch(uint32_t searchID) const
{
	// True if this ID is a Kad keyword search -- still active in the manager, or already
	// finished (recorded on completion). Lets the per-search progress sentinel pick 0xfffe (Kad
	// done, clears the "!") vs 0xffff (ed2k done) for an arbitrary search, not just the current
	// one.
	return IsKadSearch(searchID) || m_finishedKadSearches.count(searchID) != 0;
}

wxString CSearchList::GetSearchStringById(uint32_t searchID) const
{
	std::map<uint32_t, wxString>::const_iterator it = m_searchStrings.find(searchID);
	return it != m_searchStrings.end() ? it->second : wxString();
}

const CSearchList::CSearchParams *CSearchList::GetSearchParams(uint32_t searchID) const
{
	std::map<uint32_t, CSearchParams>::const_iterator it = m_searchParams.find(searchID);
	return it != m_searchParams.end() ? &it->second : nullptr;
}

bool CSearchList::IsKnownSearchId(uint32_t searchID) const
{
	return m_searchStrings.count(searchID) != 0 || theApp->browsemanager->Has(searchID);
}

uint32 CSearchList::GetBrowsePeerEcid(uint32 searchID) const
{
	std::map<uint32_t, uint32>::const_iterator it = m_browsePeers.find(searchID);
	return it != m_browsePeers.end() ? it->second : 0;
}

void CSearchList::RegisterBrowseSearch(uint32 searchID, const wxString &peerName, uint32 peerEcid)
{
	// All three, not just the kind: Save() iterates m_searchStrings and then reads the other two
	// with .at(), so an id present in one and missing from another throws when the searches are
	// persisted.
	m_searchStrings[searchID] = peerName;
	m_searchKinds[searchID] = BrowseSearch;
	m_searchStartTimes[searchID] = time(nullptr);
	m_browsePeers[searchID] = peerEcid;
}

SearchType CSearchList::GetSearchLifecycleKindById(wxUIntPtr searchID) const
{
	const uint32_t sid = static_cast<uint32_t>(searchID);
	// Recorded kinds preserve AllSearch after a restart, even though its finished
	// marker also appears in the Kad completion set.
	const auto it = m_searchKinds.find(sid);
	if (it != m_searchKinds.end()) {
		return it->second;
	}
	if (IsOrWasKadSearch(sid)) {
		return KadSearch;
	}
	// Unknown id (never started here, or evicted): fall back to the scalar.
	return m_searchType;
}

bool CSearchList::RequestMoreResults(uint32_t searchID)
{
	// AllSearch: the tab's primary ID is the ed2k one, but "More" applies to the Kad
	// component. Find the mapped Kad ID and widen that.
	const uint32_t kadID = KadComponentOf(searchID);

	// Widen this Kad search (KADEMLIA_FIND_VALUE_MORE). The outcome is logged here so the
	// message has a single home: the monolithic GUI and the daemon (for a remote-GUI request
	// over EC) both reach this one path, and the daemon forwards the line to amuleGUI -- so the
	// remote GUI shows the real result rather than an optimistic guess.
	//
	// Two different answers come back, and they are not interchangeable. The log line reports
	// what actually happened on THIS press; the return value reports whether a LATER press could
	// still do something, which is what a caller greys its control on. A reask that spends the
	// last of the budget fires (so it is logged as a success) and leaves the search un-widenable
	// (so it returns false).
	bool fired = false;
	const bool reaskable = Kademlia::CSearchManager::RequestMoreResults(kadID, &fired);
	AddLogLineN(fired ? _("Kad search: requested wider results from one more peer.")
			  : _("Kad search: no peer left to reask for more results (cap reached or no "
			      "responses yet)."));
	return reaskable;
}

void CSearchList::StopSearch(bool globalOnly)
{
	if (m_searchType == GlobalSearch || m_searchType == AllSearch) {
		if (m_searchType == AllSearch && !globalOnly) {
			StopSearchById(m_currentSearch);
		} else {
			FinalizeGlobalSearch();
		}
		m_currentSearch = -1;
	} else if (m_searchType == LocalSearch) {
		// A local search has no sweep to halt, but it does have an armed answer timeout and an
		// m_searchInProgress that nothing else clears. Leaving both is what made a stopped local
		// search hang; leaving just the timer would be worse still, announcing a timeout for a
		// search the user stopped, against a server that may well have answered.
		//
		// Not gated on globalOnly: that spares Kad, and a local search is ed2k, exactly like the
		// global branch above.
		FinalizeLocalSearch();
		m_currentSearch = -1;
	} else if (m_searchType == KadSearch && !globalOnly) {
		Kademlia::CSearchManager::StopSearch(m_currentSearch, false);
		m_currentSearch = -1;
	}
}

void CSearchList::StopSearchById(wxUIntPtr searchID)
{
	// Stop the Kad keyword search for this ID, if it is one. Harmless no-op
	// when the ID is not an active Kad search (ed2k, or already finished).
	Kademlia::CSearchManager::StopSearch(searchID, false);
	const uint32_t kadID = KadComponentOf(static_cast<uint32_t>(searchID));
	if (kadID != searchID) {
		Kademlia::CSearchManager::StopSearch(kadID, false);
	}
	// If this is the in-flight ed2k global sweep, finalize it. ed2k is
	// single-in-flight, so only the current search can be running.
	if (searchID == m_currentSearch && m_searchInProgress) {
		FinalizeGlobalSearch();
	}
	// Results are intentionally retained -- the multi-search EC "stop" halts
	// activity but keeps the bucket; RemoveResults() is the free path.
}

void CSearchList::SetKadSearchFinished(uint32_t searchID)
{
	if (m_shuttingDown || (!m_searchStrings.count(searchID) && !m_kadToEd2kSearchId.count(searchID))) {
		return;
	}
	m_finishedKadSearches.insert(searchID);
	if (searchID == m_currentSearch && m_searchType == KadSearch) {
		m_KadSearchFinished = true;
	}
}

CSearchList::SearchLifecycleState CSearchList::GetSearchLifecycleStateById(wxUIntPtr searchID) const
{
	uint32_t sid = static_cast<uint32_t>(searchID);
	// A browse ("View Files") is not a CSearchList search: CBrowseManager owns its lifecycle,
	// keyed by the same id this function is asked about. Without this the generic tail below
	// decides it from retained results, which is wrong in both directions -- a browse still
	// streaming reports finished as soon as its first directory lands, and one the peer denied
	// reports idle forever, since a failed browse has no results to flip the ternary. Same
	// mapping the EC progress reply applies (ExternalConn.cpp's AppendSearchProgress), here
	// instead of only there so the SEARCH_LIST listing cannot disagree with the per-id progress
	// reply about the same id.
	if (theApp->browsemanager->Has(sid)) {
		return theApp->browsemanager->StateOf(sid) == browse::State::InProgress
			       ? SEARCH_LIFECYCLE_RUNNING
			       : SEARCH_LIFECYCLE_FINISHED;
	}
	// Kad searches are tracked per-ID: still registered in the manager means running; recorded
	// as finished (its CSearch was destroyed on the result cap or the 45s lifetime) means
	// finished. Independent of any other search, so one search ending never reports a different
	// running search as finished.
	if (HasKadComponent(sid)) {
		return SEARCH_LIFECYCLE_RUNNING;
	}
	if (m_finishedKadSearches.count(sid)) {
		return SEARCH_LIFECYCLE_FINISHED;
	}
	// ed2k / non-Kad: the most-recently-started search owns the scalar state.
	if (searchID == m_currentSearch) {
		return GetSearchLifecycleState();
	}
	// Otherwise this id is done, whatever any sweep is still doing: results are filed under the
	// scalar m_currentSearch (ProcessSearchAnswer / ProcessUDPSearchAnswer), so once a newer
	// ed2k search takes that scalar nothing further can land in this id's bucket. Unreachable,
	// not idle.
	//
	// StartNewSearch finalizes the previous eD2k slot before publishing a new one.
	// Any surviving Kad component was handled above, independently of that slot.
	//
	// Deciding this from the retained results instead reported every search that indexed
	// *nothing* as IDLE forever -- a query no server matched, one whose every hit AddToList
	// dropped on the file-type filter, or a global sweep stopped before its first result. IDLE
	// is not terminal, so GetSearchBarStatusById fell through to the running percent (0) and
	// every consumer read a finished search as running at 0%: the Stop button stayed live, the
	// bar never cleared, the EC lifecycle tag said "idle", and the "an eD2k search is still
	// running" prompt fired on a tab that had long since finished (issue #1103).
	//
	// IsKnownSearchId is the discriminator the result bucket could not be: m_searchStrings is
	// written when the search starts, independent of what it kept, so a search started here
	// reads FINISHED whether or not it retained anything, while an id that is genuinely unknown
	// -- never started here, or already evicted -- still reads IDLE.
	return IsKnownSearchId(sid) ? SEARCH_LIFECYCLE_FINISHED : SEARCH_LIFECYCLE_IDLE;
}

uint8 CSearchList::GetSearchLifecyclePercentById(wxUIntPtr searchID) const
{
	switch (GetSearchLifecycleStateById(searchID)) {
	case SEARCH_LIFECYCLE_FINISHED:
		return 100;
	case SEARCH_LIFECYCLE_IDLE:
		return 0;
	case SEARCH_LIFECYCLE_RUNNING:
		break;
	}
	// RUNNING. A Kad search gets a cosmetic time-ramp from *its own* start time (looked up
	// per-search), so a Kad search still running while a later ed2k search is the current one
	// ramps correctly instead of sticking near full. An in-flight ed2k global uses its real
	// server-queue percent -- that state is single-slot, so it only applies to the current
	// search.
	const uint32_t sid = static_cast<uint32_t>(searchID);
	// A browse reports the share of the peer's directory list received so far, rather than the
	// 0/100 the state switch above would derive.
	//
	// Straight from the manager, NOT through GetSearchBarStatusById: that function falls through
	// to this one for anything it does not consider finished, so routing this branch through it
	// would recurse between the two.
	if (theApp->browsemanager->Has(sid)) {
		const uint16 pct = theApp->browsemanager->BarValue(sid);
		// 0xffff is the bar's terminal sentinel, not a percent.
		return (pct == 0xffff) ? 100 : static_cast<uint8>(pct);
	}
	if (searchID == m_currentSearch && m_searchType == AllSearch) {
		return static_cast<uint8>(std::min<uint32>(100, GetSearchProgress()));
	}
	if (HasKadComponent(sid)) {
		std::map<uint32_t, time_t>::const_iterator it = m_searchStartTimes.find(sid);
		time_t start = (it != m_searchStartTimes.end()) ? it->second : m_searchStart;
		time_t elapsed = time(nullptr) - start;
		if (elapsed <= 0) {
			return 0;
		}
		uint32 pct = (uint32)((elapsed * 100) / SEARCHKEYWORD_LIFETIME);
		return (pct > 99) ? 99 : (uint8)pct;
	}
	if (searchID == m_currentSearch && (m_searchType == GlobalSearch || m_searchType == AllSearch)) {
		uint32 pct = GetSearchProgress();
		return (pct > 100) ? 100 : (uint8)pct;
	}
	return 0;
}

uint32 CSearchList::GetSearchBarStatusById(wxUIntPtr searchID) const
{
	// A "View Files" browse tab is not a CSearchList search: CBrowseManager owns its lifecycle,
	// so ask there rather than keeping a second copy here that would have to be kept in step by
	// hand.
	if (theApp->browsemanager->Has(static_cast<uint32_t>(searchID))) {
		return theApp->browsemanager->BarValue(static_cast<uint32_t>(searchID));
	}
	if (GetSearchLifecycleStateById(searchID) == SEARCH_LIFECYCLE_FINISHED) {
		const uint32_t sid = static_cast<uint32_t>(searchID);
		// AllSearch contains a Kad component, so use the Kad sentinel for it too.
		if (IsOrWasKadSearch(sid)) {
			return 0xfffe;
		}
		std::map<uint32_t, SearchType>::const_iterator kindIt = m_searchKinds.find(sid);
		if (kindIt != m_searchKinds.end() && kindIt->second == AllSearch) {
			return 0xfffe;
		}
		return 0xffff;
	}
	// RUNNING or IDLE both map to the running percent (0 when idle).
	return GetSearchLifecyclePercentById(searchID);
}

void CSearchList::StopInFlightEd2kSearch()
{
	// Cancel the outgoing eD2k timer and packet before a validated replacement
	// takes the slot. This covers both the TCP-answer wait and the UDP sweep.
	// Results and any independent Kad component are retained.
	if (m_searchInProgress) {
		FinalizeGlobalSearch();
	}
}

void CSearchList::FinalizeGlobalSearch()
{
	m_ed2kSearchFinished = true;
	// Order is crucial here: on wxMSW an additional event can be generated during the stop. So
	// the packet has to be deleted first, so that OnGlobalSearchTimer() returns immediately
	// (packet-null early return) without re-entering this path.
	m_searchPacket.reset();
	m_searchInProgress = false;
	m_searchTimer.Stop();
	m_awaitingServerAnswer = false;
	if (!m_shuttingDown) {
		// Discard the completed sweep before another request starts waiting for TCP.
		// At teardown the server list has already destroyed and detached its observers.
		theApp->serverlist->RemoveObserver(&m_serverQueue);
		CoreNotify_Search_Update_Progress(0xffff);
	}
}

void CSearchList::KademliaSearchKeyword(uint32_t searchID,
	const Kademlia::CUInt128 *fileID,
	const wxString &name,
	uint64_t size,
	const wxString &type,
	uint32_t kadPublishInfo,
	const TagPtrList &taglist,
	uint32_t kadAICHResponderIP)
{
	EUtf8Str eStrEncode = utf8strRaw;

	// AllSearch maps the Kad search ID back to the ed2k tab's primary ID so results
	// from both networks land in the same bucket.
	const uint32_t effectiveSearchID = GetEffectiveSearchId(searchID);
	// A late reply for a closed search must not recreate its bucket.
	if (!m_searchStrings.count(effectiveSearchID)) {
		return;
	}
	// StartNewSearch registers a key for every Kad keyword search. Should one ever be
	// missing, still show the result, only without AICH evidence.
	const auto key = m_kadAICHKeys.find(effectiveSearchID);
	const bool sampled = key != m_kadAICHKeys.end();
	if (!sampled) {
		AddDebugLogLineC(logKadSearch,
			CFormat("Kad search %u has no AICH sampling key; dropping its AICH evidence") %
				effectiveSearchID);
	}

	CMemFile temp(250);
	uint8_t fileid[16];
	fileID->ToByteArray(fileid);
	temp.WriteHash(CMD4Hash(fileid));

	temp.WriteUInt32(0); // client IP
	temp.WriteUInt16(0); // client port

	// write tag list
	unsigned int uFilePosTagCount = temp.GetPosition();
	uint32 tagcount = 0;
	temp.WriteUInt32(tagcount); // dummy tag count, will be filled later

	// standard tags
	CTagString tagName(FT_FILENAME, name);
	tagName.WriteTagToFile(&temp, eStrEncode);
	tagcount++;

	CTagInt64 tagSize(FT_FILESIZE, size);
	tagSize.WriteTagToFile(&temp, eStrEncode);
	tagcount++;

	if (!type.IsEmpty()) {
		CTagString tagType(FT_FILETYPE, type);
		tagType.WriteTagToFile(&temp, eStrEncode);
		tagcount++;
	}

	// Misc tags (bitrate, etc)
	for (TagPtrList::const_iterator it = taglist.begin(); it != taglist.end(); ++it) {
		(*it)->WriteTagToFile(&temp, eStrEncode);
		tagcount++;
	}

	temp.Seek(uFilePosTagCount, wxFromStart);
	temp.WriteUInt32(tagcount);

	temp.Seek(0, wxFromStart);

	auto tempFile = std::make_unique<CSearchFile>(temp,
		(eStrEncode == utf8strRaw),
		effectiveSearchID,
		0,
		0,
		"",
		true,
		sampled ? kadAICHResponderIP : 0,
		sampled ? &key->second : nullptr);
	tempFile->SetKadPublishInfo(kadPublishInfo);

	AddToList(std::move(tempFile));
}

void CSearchList::UpdateSearchFileByHash(const CMD4Hash &hash)
{
	for (const auto &entry : AllResults()) {
		const CSearchResultList &results = entry.second;
		for (size_t i = 0; i < results.size(); ++i) {
			CSearchFile *item = results.at(i);

			if (hash == item->GetFileHash()) {
				// This covers only parent items,
				// child items have to be updated separately.
				Notify_Search_Update_Sources(item);
			}
		}
	}
}

// File_checked_for_headers
