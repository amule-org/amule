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

#include "BrowseManager.h"

#include <algorithm> // Needed for std::sort (StoreSearches)
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
#include "GuiEvents.h"   // Needed for Notify_*

#ifndef AMULE_DAEMON
#include "amuleDlg.h"  // Needed for CamuleDlg
#include "SearchDlg.h" // Needed for CSearchDlg
#endif

#include "kademlia/kademlia/Kademlia.h"
#include "kademlia/kademlia/Search.h"
#include "kademlia/kademlia/Defines.h" // Needed for SEARCHKEYWORD_LIFETIME (Kad ramp)

#include "SearchExpr.h"

#include "Scanner.h"
void LexInit(const wxString &pszInput);
void LexFree();

#include "Parser.hpp"
int yyerror(wxString errstr);

static wxString s_strCurKadKeyword;

static CSearchExpr _SearchExpr;

wxArrayString _astrParserErrors;

// Helper function for lexer.
void ParsedSearchExpression(const CSearchExpr *pexpr)
{
	int iOpAnd = 0;
	int iOpOr = 0;
	int iOpNot = 0;

	for (unsigned int i = 0; i < pexpr->m_aExpr.GetCount(); i++) {
		const wxString &str = pexpr->m_aExpr[i];
		if (str == SEARCHOPTOK_AND) {
			iOpAnd++;
		} else if (str == SEARCHOPTOK_OR) {
			iOpOr++;
		} else if (str == SEARCHOPTOK_NOT) {
			iOpNot++;
		}
	}

	// this limit (+ the additional operators which will be added later) has to match the limit in
	// 'CreateSearchExpressionTree'
	//	+1 Type (Audio, Video)
	//	+1 MinSize
	//	+1 MaxSize
	//	+1 Avail
	//	+1 Extension
	//	+1 Complete sources
	//	+1 Codec
	//	+1 Bitrate
	//	+1 Length
	//	+1 Title
	//	+1 Album
	//	+1 Artist
	// ---------------
	//  12
	if (iOpAnd + iOpOr + iOpNot > 10) {
		yyerror("Search expression is too complex");
	}

	_SearchExpr.m_aExpr.Empty();

	// optimize search expression, if no OR nor NOT specified
	if (iOpAnd > 0 && iOpOr == 0 && iOpNot == 0) {
		// figure out if we can use a better keyword than the one the user selected
		// for example most user will search like this "The oxymoronaccelerator 2", which would ask
		// the node which indexes "the" This causes higher traffic for such nodes and makes them a
		// viable target to attackers, while the kad result should be the same or even better if we
		// ask the node which indexes the rare keyword "oxymoronaccelerator", so we try to rearrange
		// keywords and generally assume that the longer keywords are rarer
		if (/*thePrefs::GetRearrangeKadSearchKeywords() &&*/ !s_strCurKadKeyword.IsEmpty()) {
			for (unsigned int i = 0; i < pexpr->m_aExpr.GetCount(); i++) {
				if (pexpr->m_aExpr[i] != SEARCHOPTOK_AND) {
					if (pexpr->m_aExpr[i] != s_strCurKadKeyword &&
						pexpr->m_aExpr[i].find_first_of(
							Kademlia::CSearchManager::GetInvalidKeywordChars()) ==
							wxString::npos &&
						pexpr->m_aExpr[i].Find('"') !=
							0 // no quoted expressions as keyword
						&& pexpr->m_aExpr[i].length() >= 3 &&
						s_strCurKadKeyword.length() < pexpr->m_aExpr[i].length()) {
						s_strCurKadKeyword = pexpr->m_aExpr[i];
					}
				}
			}
		}
		wxString strAndTerms;
		for (unsigned int i = 0; i < pexpr->m_aExpr.GetCount(); i++) {
			if (pexpr->m_aExpr[i] != SEARCHOPTOK_AND) {
				// Minor optimization: Because we added the Kad keyword to the boolean search
				// expression, we remove it here (and only here) again because we know that
				// the entire search expression does only contain (implicit) ANDed strings.
				if (pexpr->m_aExpr[i] != s_strCurKadKeyword) {
					if (!strAndTerms.IsEmpty()) {
						strAndTerms += ' ';
					}
					strAndTerms += pexpr->m_aExpr[i];
				}
			}
		}
		wxASSERT(_SearchExpr.m_aExpr.GetCount() == 0);
		_SearchExpr.m_aExpr.Add(strAndTerms);
	} else {
		if (pexpr->m_aExpr.GetCount() != 1 || pexpr->m_aExpr[0] != s_strCurKadKeyword)
			_SearchExpr.Add(pexpr);
	}
}

//! Helper class for packet creation
class CSearchExprTarget
{
public:
	CSearchExprTarget(CMemFile *pData, EUtf8Str eStrEncode, bool supports64bit, bool &using64bit)
	: m_data(pData)
	, m_eStrEncode(eStrEncode)
	, m_supports64bit(supports64bit)
	, m_using64bit(using64bit)
	{
		m_using64bit = false;
	}

	void WriteBooleanAND()
	{
		m_data->WriteUInt8(0);    // boolean operator parameter type
		m_data->WriteUInt8(0x00); // "AND"
	}

	void WriteBooleanOR()
	{
		m_data->WriteUInt8(0);    // boolean operator parameter type
		m_data->WriteUInt8(0x01); // "OR"
	}

	void WriteBooleanNOT()
	{
		m_data->WriteUInt8(0);    // boolean operator parameter type
		m_data->WriteUInt8(0x02); // "NOT"
	}

	void WriteMetaDataSearchParam(const wxString &rstrValue)
	{
		m_data->WriteUInt8(1);                        // string parameter type
		m_data->WriteString(rstrValue, m_eStrEncode); // string value
	}

	void WriteMetaDataSearchParam(uint8 uMetaTagID, const wxString &rstrValue)
	{
		m_data->WriteUInt8(2);                        // string parameter type
		m_data->WriteString(rstrValue, m_eStrEncode); // string value
		m_data->WriteUInt16(sizeof(uint8));           // meta tag ID length
		m_data->WriteUInt8(uMetaTagID);               // meta tag ID name
	}

	void WriteMetaDataSearchParamASCII(uint8 uMetaTagID, const wxString &rstrValue)
	{
		m_data->WriteUInt8(2);                       // string parameter type
		m_data->WriteString(rstrValue, utf8strNone); // string value
		m_data->WriteUInt16(sizeof(uint8));          // meta tag ID length
		m_data->WriteUInt8(uMetaTagID);              // meta tag ID name
	}

	void WriteMetaDataSearchParam(const wxString &pszMetaTagID, const wxString &rstrValue)
	{
		m_data->WriteUInt8(2);                        // string parameter type
		m_data->WriteString(rstrValue, m_eStrEncode); // string value
		m_data->WriteString(pszMetaTagID);            // meta tag ID
	}

	void WriteMetaDataSearchParam(uint8_t uMetaTagID, uint8_t uOperator, uint64_t value)
	{
		bool largeValue = value > wxULL(0xFFFFFFFF);
		if (largeValue && m_supports64bit) {
			m_using64bit = true;
			m_data->WriteUInt8(8);      // numeric parameter type (int64)
			m_data->WriteUInt64(value); // numeric value
		} else {
			if (largeValue) {
				value = 0xFFFFFFFFu;
			}
			m_data->WriteUInt8(3);      // numeric parameter type (int32)
			m_data->WriteUInt32(value); // numeric value
		}
		m_data->WriteUInt8(uOperator);      // comparison operator
		m_data->WriteUInt16(sizeof(uint8)); // meta tag ID length
		m_data->WriteUInt8(uMetaTagID);     // meta tag ID name
	}

	void WriteMetaDataSearchParam(const wxString &pszMetaTagID, uint8_t uOperator, uint64_t value)
	{
		bool largeValue = value > wxULL(0xFFFFFFFF);
		if (largeValue && m_supports64bit) {
			m_using64bit = true;
			m_data->WriteUInt8(8);      // numeric parameter type (int64)
			m_data->WriteUInt64(value); // numeric value
		} else {
			if (largeValue) {
				value = 0xFFFFFFFFu;
			}
			m_data->WriteUInt8(3);      // numeric parameter type (int32)
			m_data->WriteUInt32(value); // numeric value
		}
		m_data->WriteUInt8(uOperator);     // comparison operator
		m_data->WriteString(pszMetaTagID); // meta tag ID
	}

protected:
	CMemFile *m_data;
	EUtf8Str m_eStrEncode;
	bool m_supports64bit;
	bool &m_using64bit;
};

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
, m_searchPacket(NULL)
, m_64bitSearchPacket(false)
, m_KadSearchFinished(true)
, m_ed2kSearchFinished(true)
, m_awaitingServerAnswer(false)
, m_searchStart(0)
, m_shuttingDown(false)
{
}

CSearchList::~CSearchList()
{
	StopSearch();

	// Teardown: the GUI is being dismantled around us, so suppress the
	// Search_Removed broadcast below -- there is no tab left worth closing
	// and the notify would reach a half-destroyed CamuleDlg.
	m_shuttingDown = true;
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
	// A non-existent search id will just be ignored
	Kademlia::CSearchManager::StopSearch(searchID, true);

	// Tell the GUI before the CSearchFile objects below are deleted: in a
	// monolithic build CSearchListCtrl's model holds them as raw pointers
	// (each row's wxDataViewItem ID is the CSearchFile*) and nothing else
	// removes those rows, so a tab left open on this search would fault on
	// the next repaint, sort, scroll or click. Also the local counterpart
	// of amuleGUI's
	// EC_TAG_SEARCH_EXPIRED-driven close, so "the search is gone" closes its
	// tab through one path in both builds (got3nks, PR #680 review).
	if (!m_shuttingDown) {
		Notify_Search_Removed(searchID);
	}

	// Drop any per-search tracking for this ID (bounded growth).
	m_finishedKadSearches.erase(static_cast<uint32_t>(searchID));
	m_searchStartTimes.erase(static_cast<uint32_t>(searchID));
	m_searchKinds.erase(static_cast<uint32_t>(searchID));
	m_searchStrings.erase(static_cast<uint32_t>(searchID));
	m_browsePeers.erase(static_cast<uint32_t>(searchID));
	// The browse record outlives its client but not its search.
	theApp->browsemanager->Remove(static_cast<uint32_t>(searchID));

	// This list owns its results, so free them before dropping the index
	// (which, being shared with the remote search list, never deletes).
	const CSearchResultList &list = GetSearchResults(searchID);
	for (CSearchFile *file : list) {
		delete file;
	}
	DropResultIndex(searchID);
}

const wxChar *const CSearchList::s_storedSearchesFilename = wxT("StoredSearches.met");

void CSearchList::StoreSearches() const
{
	// Same contract as the search-terms dropdown this setting already
	// governs (SearchDlg.cpp): off means "don't remember", and a search's
	// full result set is a stronger form of that than the query string
	// alone. LoadSearches() is the one that removes a file left over from
	// when this was still on.
	if (!thePrefs::RememberSearchHistory()) {
		return;
	}

	CFile file(thePrefs::GetConfigDir() + s_storedSearchesFilename, CFile::write_safe);
	if (!file.IsOpened()) {
		return;
	}

	// Oldest-first by start time, so if we have to drop any for the cap
	// below, we drop the oldest rather than an arbitrary map-ordered subset.
	std::vector<uint32_t> ids;
	ids.reserve(m_searchStrings.size());
	for (const auto &kv : m_searchStrings) {
		// Browses are deliberately not persisted. They are a snapshot of one
		// peer's share taken while that peer was connected, so restoring one
		// resurrects a listing for someone who is very likely gone -- and a
		// large share would spend the MAX_STORED_SEARCHES budget that exists
		// for the user's own searches. They are in m_searchStrings only so
		// the search list can report them by peer name while they are live.
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
		AddLogLineC(CFormat(_("Only saving the %u most recent of %u open searches to %s.")) %
			    MAX_STORED_SEARCHES % ids.size() % s_storedSearchesFilename);
		ids.erase(ids.begin(), ids.end() - MAX_STORED_SEARCHES);
	}

	file.WriteUInt8(STOREDSEARCHES_VERSION);
	file.WriteUInt32(static_cast<uint32>(ids.size()));

	for (uint32_t id : ids) {
		const CSearchResultList &results = GetSearchResults(id);
		std::size_t resultCount = results.size();
		if (resultCount > MAX_STORED_RESULTS_PER_SEARCH) {
			AddLogLineC(CFormat(_("Only saving the first %u of %u results for search \"%s\".")) %
				    MAX_STORED_RESULTS_PER_SEARCH % resultCount % m_searchStrings.at(id));
			resultCount = MAX_STORED_RESULTS_PER_SEARCH;
		}

		file.WriteUInt32(id);
		// m_searchStrings/m_searchKinds/m_searchStartTimes are written and
		// erased together (StartNewSearch/RemoveResults), so ids is drawn
		// from m_searchStrings, and matches this instance and the sort
		// comparator above, this can't miss -- .at() throughout says so.
		file.WriteString(m_searchStrings.at(id), utf8strRaw);
		file.WriteUInt8(static_cast<uint8>(m_searchKinds.at(id)));
		file.WriteUInt64(static_cast<uint64>(m_searchStartTimes.at(id)));
		file.WriteUInt32(static_cast<uint32>(resultCount));

		for (std::size_t i = 0; i < resultCount; ++i) {
			results[i]->WriteToFile(&file);
		}
	}

	// write_safe writes to a .new sibling; only Close() performs the rename
	// onto the real filename (CFile.h). Without this call the .new file is
	// left orphaned and StoredSearches.met itself is never updated.
	file.Close();
}

namespace
{
//! One search record as read from disk, before it's committed into the live
//! maps. Kept separate so a malformed record partway through the file can
//! discard everything read so far instead of leaving CSearchList half-loaded.
struct LoadedSearch
{
	uint32_t id;
	wxString queryString;
	SearchType kind;
	time_t startTime;
	std::vector<CSearchFile *> results;
};

void FreeLoaded(std::vector<LoadedSearch> &loaded)
{
	for (LoadedSearch &entry : loaded) {
		for (CSearchFile *f : entry.results) {
			delete f;
		}
	}
	loaded.clear();
}
} // namespace

std::vector<uint32_t> CSearchList::LoadSearches()
{
	std::vector<uint32_t> restored;

	CPath fullpath = CPath(thePrefs::GetConfigDir() + s_storedSearchesFilename);
	if (!thePrefs::RememberSearchHistory()) {
		// Remove whatever an earlier session with the setting on left
		// behind, so turning it off actually clears what's on disk rather
		// than just stopping new writes.
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
			// StoreSearches() never writes more than MAX_STORED_SEARCHES; a
			// count more than double that is not a file we ever wrote, and
			// trusting it would drive an equally suspect per-search loop
			// below. Fail closed rather than partially populate.
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
				FreeLoaded(loaded);
				return std::vector<uint32_t>();
			}

			entry.results.reserve(resultCount);
			for (uint32 r = 0; r < resultCount; ++r) {
				CSearchFile *result = CSearchFile::LoadFromFile(&file);
				if (!result) {
					AddLogLineC(_(
						"Invalid entry in stored search list, file may be corrupt"));
					for (CSearchFile *f : entry.results) {
						delete f;
					}
					FreeLoaded(loaded);
					return std::vector<uint32_t>();
				}
				entry.results.push_back(result);
			}

			loaded.push_back(std::move(entry));
		}
	} catch (const CInvalidPacket &e) {
		AddLogLineC(_("Invalid entry in stored search list, file may be corrupt") + ": " + e.what());
		FreeLoaded(loaded);
		return restored;
	} catch (const CSafeIOException &e) {
		AddLogLineC(CFormat(_("IO error while reading %s file: %s")) % s_storedSearchesFilename %
			    e.what());
		FreeLoaded(loaded);
		return restored;
	}

	// Everything parsed cleanly -- commit it all to the live maps/index.
	// theApp->downloadqueue/knownfiles/canceledfiles must already exist here
	// (see amule.cpp's call site) for SetDownloadStatus() to be meaningful.
	restored.reserve(loaded.size());
	for (LoadedSearch &entry : loaded) {
		m_searchStrings[entry.id] = entry.queryString;
		m_searchKinds[entry.id] = entry.kind;
		m_searchStartTimes[entry.id] = entry.startTime;

		// Reserve the id so the first new search of the same kind this
		// session can't be handed it -- both counters restart every launch,
		// so without this a restored search collides with the next one
		// started. Partitioned on the id's own high bit rather than the
		// persisted `kind` byte: bit 31 is intrinsic to which counter owns
		// the value, while `kind` is a separate field from the same record
		// that could disagree with it if the file were corrupt. The one id
		// that bit is wrong about is the legacy sentinel, handled first.
		if (entry.id == 0xffffffff) {
			// The legacy single-search bucket comes from neither counter:
			// every EC client predating multi-search reuses this one id for
			// all of its searches, so there is no allocation to advance past.
			// It has to be excluded explicitly because the bit-31 test below
			// would hand an ed2k search to the Kad counter -- and since this
			// is the largest uint32 there is, ReserveSearchId would pin
			// m_nextID at its maximum and the next Kad allocation
			// (++m_nextID | SEARCH_ID_KAD_MASK) would wrap to the FIRST Kad
			// id, which is the collision this reservation exists to prevent.
		} else if (entry.id & 0x80000000) {
			Kademlia::CSearchManager::ReserveSearchId(entry.id);
		} else {
			ReserveEd2kId(entry.id);
		}

		if (entry.kind == KadSearch) {
			// The original in-flight Kad search can't survive a restart --
			// come back already finished, hits-only, "More results" disabled.
			m_finishedKadSearches.insert(entry.id);
		}

		for (CSearchFile *root : entry.results) {
			root->m_searchID = entry.id;
			root->SetDownloadStatus();
			for (CSearchFile *child : root->GetChildren()) {
				child->m_searchID = entry.id;
				child->SetDownloadStatus();
			}
			IndexResult(root);
		}

		restored.push_back(entry.id);
	}

	return restored;
}

wxString CSearchList::StartNewSearch(uint32 *searchID, SearchType type, CSearchParams &params)
{
	// Check that we can actually perform the specified desired search.
	if ((type == KadSearch) && !Kademlia::CKademlia::IsRunning()) {
		return _("Kad search can't be done if Kad is not running");
	} else if ((type != KadSearch) && !theApp->IsConnectedED2K()) {
		return _("eD2k search can't be done if eD2k is not connected");
	}

	if (params.typeText != ED2KFTSTR_PROGRAM) {
		if (params.typeText.CmpNoCase("Any")) {
			m_resultType = params.typeText;
		} else {
			m_resultType.Clear();
		}
	} else {
		// No check is to be made on returned results if the
		// type is 'Programs', since this returns multiple types.
		m_resultType.Clear();
	}

	if (type == KadSearch) {
		Kademlia::WordList words;
		Kademlia::CSearchManager::GetWords(params.searchString, &words);
		if (!words.empty()) {
			params.strKeyword = words.front();
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
	CMemFilePtr data = CreateSearchData(params, type, supports64bit, packetUsing64bit);

	if (data.get() == NULL) {
		wxASSERT(_astrParserErrors.GetCount());
		wxString error;

		for (unsigned int i = 0; i < _astrParserErrors.GetCount(); ++i) {
			error += _astrParserErrors[i] + "\n";
		}

		return error;
	}

	// The scalar m_searchType / m_currentSearch are the anchor for the single
	// in-flight ed2k (local/global) search: its results arrive asynchronously
	// for several seconds and are attributed via these scalars (see
	// ProcessSearchAnswer / LocalSearchEnd). A Kad search started ALONGSIDE an
	// in-flight ed2k search has its own per-ID machinery (results carry the Kad
	// search ID explicitly; lifecycle is IsKadSearch/m_finishedKadSearches) and
	// needs neither scalar — so it must not repoint them, or the ed2k search's
	// late hits get dropped (wrong type) or misfiled (wrong bucket). Preserve
	// the ed2k anchor in exactly that case; every other start updates it as
	// before (a new ed2k search first finalizes the old one via
	// StopInFlightEd2kSearch, and a lone Kad search has no ed2k in flight).
	const bool preserveEd2kAnchor = (type == KadSearch) && m_searchInProgress;
	if (!preserveEd2kAnchor) {
		m_searchType = type;
	}
	m_searchStart = time(NULL);

	// EC clients reuse the sentinel `0xffffffff` for every search regardless
	// of network type. `Get_EC_Response_Search` -> `RemoveResults(0xffffffff)`
	// already soft-stops the previous Kad search via `PrepareToStop()` so it
	// can drain in-flight packets, but those late `KademliaSearchKeyword(
	// 0xffffffff, ...)` callbacks would then land in the *new* search's
	// `m_results[0xffffffff]` bucket -- the Kad results contaminate an ed2k
	// (or vice-versa) result list whenever an EC client switches search type
	// without restarting the daemon. Hard-delete the previous Kad search
	// before either a Kad `PrepareFindKeywords` or an ed2k server packet
	// starts feeding the shared bucket. Native-GUI searches allocate
	// distinct top/bottom-half IDs (`3008ada0f`) so `*searchID != 0xffffffff`
	// for them and they are unaffected.
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
			// Don't repoint the ed2k result-attribution scalar when a Kad
			// search runs alongside an in-flight ed2k search (see the
			// preserveEd2kAnchor note above); the Kad search is tracked by its
			// own ID regardless.
			if (!preserveEd2kAnchor) {
				m_currentSearch = *searchID;
			}
			m_KadSearchFinished = false;
		} catch (const wxString &what) {
			AddLogLineC(what);
			return _("Unexpected error while attempting Kad search: ") + what;
		}
	} else {
		// This is an ed2k search, local or global
		m_currentSearch = *(searchID);
		m_searchInProgress = true;
		m_ed2kSearchFinished = false;

		CPacket *searchPacket = new CPacket(*data.get(), OP_EDONKEYPROT, OP_SEARCHREQUEST);

		theStats::AddUpOverheadServer(searchPacket->GetPacketSize());
		theApp->serverconnect->SendPacket(searchPacket, (type == LocalSearch));

		// Bound the wait for the server's answer, for either kind: a local
		// search has no other terminal path, and a global one's sweep is
		// armed by the very answer we are waiting for.
		m_awaitingServerAnswer = true;
		m_searchTimer.Start(SERVER_ANSWER_TIMEOUT_MS, true /* one shot */);

		if (type == GlobalSearch) {
			delete m_searchPacket;
			m_searchPacket = searchPacket;
			m_64bitSearchPacket = packetUsing64bit;
			m_searchPacket->SetOpCode(
				OP_GLOBSEARCHREQ); // will be changed later when actually sending the packet!!
		}
	}

	// Record this search's own start time so its (cosmetic Kad) progress ramp
	// is computed from *its* age even after it is no longer the most-recently-
	// started search — otherwise a Kad search running in parallel with a later
	// ed2k search would report a fixed near-full percent.
	m_searchStartTimes[static_cast<uint32_t>(*searchID)] = m_searchStart;
	// Record this search's kind by id (same reason as the start time above): a
	// later search of a different type must not make an older tab report the
	// wrong kind. `type` is this search's real type regardless of the scalar
	// anchor bookkeeping.
	m_searchKinds[static_cast<uint32_t>(*searchID)] = type;
	m_searchStrings[static_cast<uint32_t>(*searchID)] = params.searchString;

	// Tell the GUI a search now exists. Every producer funnels through here --
	// the monolithic dialog and the EC_OP_SEARCH_START handler alike -- so
	// this one call covers a search started by any client. The monolithic
	// GUI's own searches already have a tab by this point and are filtered
	// out on the handler side; what this adds is the tab for a search some
	// *other* client started, the last direction of the reachability work
	// amulegui and amuleapi already had over EC_OP_SEARCH_LIST
	// (amule-org/amule#703).
	Notify_Search_Added(
		static_cast<wxUIntPtr>(*searchID), params.searchString, static_cast<uint32>(type));

	return "";
}

void CSearchList::LocalSearchEnd()
{
	if (!m_searchInProgress) {
		// Nothing left for this reply to end: the wait for it ran out, or the
		// search was stopped. Without this, a late answer to a terminalized
		// global search reaches the wxCHECK_RET below with the packet already
		// released -- silent under NDEBUG, an assertion in a Debug build.
		return;
	}

	// The answer is in; from here the global branch's sweep bounds itself and
	// the local branch is already terminal, so the one-shot has nothing left
	// to guard. Cleared before either branch, since the global one restarts
	// the same timer as the sweep ticker.
	m_awaitingServerAnswer = false;

	if (m_searchType == GlobalSearch) {
		wxCHECK_RET(m_searchPacket, "Global search, but no packet");

		// Ensure that every global search starts over.
		theApp->serverlist->RemoveObserver(&m_serverQueue);
		m_searchTimer.Start(750);
	} else {
		FinalizeLocalSearch();
	}
}

void CSearchList::FinalizeLocalSearch()
{
	// Harmless when the server answered in time and the timer never fired;
	// required when it did not, so the one-shot cannot outlive its search.
	m_searchTimer.Stop();
	m_awaitingServerAnswer = false;
	m_searchInProgress = false;
	m_ed2kSearchFinished = true;
	Notify_SearchLocalEnd();
}

uint32 CSearchList::GetSearchProgress() const
{
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
		// The sweep is not armed until the connected server answers the local
		// part (OP_SEARCHRESULT -> LocalSearchEnd) and the first timer tick
		// attaches the observer. Until then m_serverQueue is detached and empty,
		// so GetRemaining() is a stale 0 that would read as 100% ("done") the
		// instant a search starts. IsActive() is true only during the actual
		// sweep, so report 0 (just-started) before it begins.
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
	// m_currentSearch defaults to wxUIntPtr(-1) at construction and after
	// an explicit StopSearch. A natural global-search completion (via
	// FinalizeGlobalSearch from OnGlobalSearchTimer) preserves it, so
	// completed-then-idle-view still reports FINISHED here.
	if (m_currentSearch == wxUIntPtr(-1)) {
		return SEARCH_LIFECYCLE_IDLE;
	}
	if (m_searchType == KadSearch) {
		return m_KadSearchFinished ? SEARCH_LIFECYCLE_FINISHED : SEARCH_LIFECYCLE_RUNNING;
	}
	// ED2K (Local / Global): m_ed2kSearchFinished mirrors m_KadSearchFinished.
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
		// Kad has no measurable progress, so synthesise a cosmetic ramp
		// from the fixed keyword-search lifetime. The FINISHED state above
		// is what snaps it to 100; capped at 99 so the ramp never claims
		// completion before the daemon actually does.
		time_t elapsed = time(NULL) - m_searchStart;
		if (elapsed <= 0) {
			return 0;
		}
		uint32 pct = (uint32)((elapsed * 100) / SEARCHKEYWORD_LIFETIME);
		return (pct > 99) ? 99 : (uint8)pct;
	}

	if (m_searchType == GlobalSearch) {
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
		// The one-shot armed at StartNewSearch: the connected server never
		// sent OP_SEARCHRESULT, so neither kind of ed2k search has anything
		// left that would end it. Terminalize through the finalizer the kind
		// already has rather than leave it reporting RUNNING for good.
		//
		// Tested before the packet check below, which a local search would
		// otherwise fall into: it has no search packet.
		AddLogLineN(_("Search timed out: the server did not answer."));
		if (m_searchType == GlobalSearch) {
			FinalizeGlobalSearch();
		} else {
			FinalizeLocalSearch();
		}
		return;
	}

	// Ensure that the server-queue contains the current servers.
	if (m_searchPacket == NULL) {
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
							m_searchPacket, server, false);
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
							m_searchPacket, server, false);
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
	// No more servers left to ask. Natural completion — preserve
	// m_currentSearch so GetSearchLifecycleState reports FINISHED,
	// not IDLE.
	FinalizeGlobalSearch();
}

void CSearchList::ProcessSharedFileList(const uint8_t *in_packet,
	uint32 size,
	CUpDownClient *sender,
	bool *moreResultsAvailable,
	const wxString &directory)
{
	wxCHECK_RET(sender, "No sender in search-results from client.");

	// Route the browsed listing to a result bucket. Every browse has a real,
	// wire-safe search ID by now -- pinned by the EC handler for a remote one,
	// allocated by RequestSharedFileList for a local one -- so the union and
	// per-ID polls and the LRU ring can all address these results.
	// A local browse used to key its results on the client pointer, which no
	// remote client can address -- it is this process's memory, so the browse
	// was invisible to amulegui, amuleweb and amuleapi while an EC-initiated
	// one (which is handed a real id) showed up everywhere, including in this
	// GUI. Give it the same kind of id so the two directions match. It is also
	// safer: a pointer is reused once the client is freed, so two browses of
	// different peers could collide on one key.
	//
	// Allocated once and pinned on the client, not per packet -- a browse
	// arrives in several -- and registered so it is listed like any other
	// search, with its kind and peer, which is what lets a remote GUI rebuild
	// it as a browse tab rather than a nameless search.
	// Whether this browse was asked for here. Read before the id is assigned
	// below, because that assignment is what makes a local browse look like an
	// EC one afterwards. An EC-initiated browse belongs to another client, so
	// revealing it would pull this user's panel and selection away for
	// something they never asked for -- the same rule the discovered-search
	// path follows. Gated with its use: there is no tab to reveal in a
	// daemon build, and an unread value there is a dead store.
#ifndef AMULE_DAEMON
	const bool ecInitiated = sender->IsBrowseEcInitiated();
#endif

	wxUIntPtr searchID = static_cast<wxUIntPtr>(sender->GetBrowseSearchId());

#ifndef AMULE_DAEMON
	// Find-or-create the peer's "View Files" tab, keyed by ECID (so two peers
	// sharing a nick don't collapse into one tab, and a re-browse refreshes the
	// same tab). Marks the tab as browsing; the terminal paths flip it to
	// finished/failed via Notify_Browse_Status.
	theApp->amuledlg->m_searchwnd->EnsureBrowseTab(
		sender->ECID(), sender->GetUserName(), searchID, !ecInitiated);
#endif

	const CMemFile packet(in_packet, size);
	uint32 results = packet.ReadUInt32();
	bool unicoded = (sender->GetUnicodeSupport() != utf8strNone);
	for (unsigned int i = 0; i != results; ++i) {
		CSearchFile *toadd = new CSearchFile(packet, unicoded, searchID, 0, 0, directory);
		toadd->SetClientID(sender->GetUserIDHybrid());
		toadd->SetClientPort(sender->GetUserPort());
		AddToList(toadd, true);
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

// Symmetric counterpart to PR #36's Kad hard-stop on EC search-start.
// Late ed2k server replies (TCP Local responses here, UDP Global responses
// in ProcessUDPSearchAnswer below) keep arriving for seconds after the
// search request is sent. When an EC client switches search type (ed2k ->
// Kad) those late results would land in `m_results[m_currentSearch]` --
// with the EC sentinel `0xffffffff` pinned across all EC searches, that
// bucket is now the new Kad search's bucket, producing ed2k contamination
// of a Kad result list. Drop late ed2k replies when the active search
// type is no longer ed2k. Native-GUI parallel searches keep
// `m_currentSearch` at bottom-half IDs and a Kad tab updates m_searchType
// to KadSearch -- this gate makes late ed2k packets stop misrouting to
// the Kad tab's bucket (a pre-existing GUI side bug that nobody noticed
// because cross-protocol hits in a Kad tab look like noise).
static inline bool IsActiveSearchTypeEd2k(SearchType t)
{
	return t == LocalSearch || t == GlobalSearch;
}

bool CSearchList::CanFileServerAnswer() const
{
	// Late server replies keep arriving after a search is over, and where they
	// may be filed is not the same question as whether the search is still
	// running. A terminalized search still owns its id -- both the timeout and
	// the sweep's natural drain leave m_currentSearch alone -- so its results
	// have a bucket of their own and are worth keeping.
	//
	// An explicit stop is what does not: it hands m_currentSearch back to the
	// sentinel the EC handler pins across all its searches, and filing server
	// hits there contaminates the bucket StartNewSearch takes care to keep
	// clean. Test that, rather than m_searchInProgress, so nothing is dropped
	// that had somewhere to go.
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
		AddToList(new CSearchFile(packet, optUTF8, m_currentSearch, serverIP, serverPort), false);
	}
}

void CSearchList::ProcessUDPSearchAnswer(
	const CMemFile &packet, bool optUTF8, uint32_t serverIP, uint16_t serverPort)
{
	if (!CanFileServerAnswer()) {
		return;
	}
	AddToList(new CSearchFile(packet, optUTF8, m_currentSearch, serverIP, serverPort), false);
}

bool CSearchList::AddToList(CSearchFile *toadd, bool clientResponse)
{
	const uint64 fileSize = toadd->GetFileSize();
	// If filesize is 0, or file is too large for the network, drop it
	if ((fileSize == 0) || (fileSize > MAX_FILE_SIZE)) {
		AddDebugLogLineN(logSearch,
			CFormat("Dropped result with filesize %u: %s") % fileSize % toadd->GetFileName());

		delete toadd;
		return false;
	}

	// If the result was not the type the user wanted, drop it.
	if ((clientResponse == false) && !m_resultType.IsEmpty()) {
		if (GetFileTypeByName(toadd->GetFileName()) != m_resultType) {
			AddDebugLogLineN(logSearch,
				CFormat("Dropped result type %s != %s, file %s") %
					GetFileTypeByName(toadd->GetFileName()) % m_resultType %
					toadd->GetFileName());

			delete toadd;
			return false;
		}
	}

	// Scan the results already indexed for this search (empty if it is the
	// first one) for a duplicate; the new result is indexed further down,
	// through IndexResult().
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
			item->AddChild(toadd);
			// AddChild() MERGES a duplicate filename into an existing child
			// and deletes the file it was handed, on two of its four paths
			// -- notifying with `toadd` afterwards would then read freed
			// memory (got3nks, PR #796 review). Infer survival from whether
			// the child count actually grew.
			const bool survived = item->GetChildren().size() > childrenBefore;
			// Structural change (leaf-or-nothing -> container, or a new row
			// under an existing container) needs its own notification --
			// Search_Update_Sources only signals that the parent's values
			// changed, via wxDataViewModel::ItemChanged(), which some
			// wxDataViewCtrl backends (GTK, MSW) don't treat as reason to
			// re-check IsContainer()/re-fetch children, so the child never
			// becomes reachable there. Native NSOutlineView survives the
			// omission by re-querying IsContainer() on every draw, which
			// masked this on macOS.
			if (survived) {
				Notify_Search_Add_Result(toadd);
			}
			Notify_Search_Update_Sources(item);
			return true;
		}
	}

	AddDebugLogLineN(logSearch,
		CFormat("Added new result '%s' : %s") % toadd->GetFileName() % toadd->GetFileHash().Encode());

	// New unique result, simply add and display.
	IndexResult(toadd);
	Notify_Search_Add_Result(toadd);

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
	// Unlike GetSearchFileByID (first match only), collect EVERY result object
	// that shares this hash. The same file can appear in more than one open
	// search (an EC client such as amulegui runs several at once), and each
	// search keeps its own CSearchFile with its own note list. On-demand Kad
	// notes and the running flag must reach all of them, or only the first tab
	// would show the comments.
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
	// Match against parents and their same-hash/different-name children
	// (issue #431 grouping); downloading the specific CSearchFile lands
	// the partfile under that result's own filename.
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

bool CSearchList::IsOrWasKadSearch(uint32_t searchID) const
{
	// True if this ID is a Kad keyword search — still active in the manager, or
	// already finished (recorded on completion). Lets the per-search progress
	// sentinel pick 0xfffe (Kad done, clears the "!") vs 0xffff (ed2k done) for
	// an arbitrary search, not just the current one.
	return IsKadSearch(searchID) || m_finishedKadSearches.count(searchID) != 0;
}

wxString CSearchList::GetSearchStringById(uint32_t searchID) const
{
	std::map<uint32_t, wxString>::const_iterator it = m_searchStrings.find(searchID);
	return it != m_searchStrings.end() ? it->second : wxString();
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
	// All three, not just the kind: Save() iterates m_searchStrings and then
	// reads the other two with .at(), so an id present in one and missing
	// from another throws when the searches are persisted.
	m_searchStrings[searchID] = peerName;
	m_searchKinds[searchID] = BrowseSearch;
	m_searchStartTimes[searchID] = time(nullptr);
	m_browsePeers[searchID] = peerEcid;
}

SearchType CSearchList::GetSearchLifecycleKindById(wxUIntPtr searchID) const
{
	const uint32_t sid = static_cast<uint32_t>(searchID);
	// Kad is authoritative from the manager / finished-set even if the recorded
	// kind was pruned with the results.
	if (IsOrWasKadSearch(sid)) {
		return KadSearch;
	}
	std::map<uint32_t, SearchType>::const_iterator it = m_searchKinds.find(sid);
	if (it != m_searchKinds.end()) {
		return it->second;
	}
	// Unknown id (never started here, or evicted): fall back to the scalar.
	return m_searchType;
}

bool CSearchList::RequestMoreResults(uint32_t searchID)
{
	// Widen this Kad search (KADEMLIA_FIND_VALUE_MORE). The outcome is logged
	// here so the message has a single home: the monolithic GUI and the daemon
	// (for a remote-GUI request over EC) both reach this one path, and the
	// daemon forwards the line to amuleGUI — so the remote GUI shows the real
	// result rather than an optimistic guess.
	//
	// Two different answers come back, and they are not interchangeable. The
	// log line reports what actually happened on THIS press; the return value
	// reports whether a LATER press could still do something, which is what a
	// caller greys its control on. A reask that spends the last of the budget
	// fires (so it is logged as a success) and leaves the search un-widenable
	// (so it returns false).
	bool fired = false;
	const bool reaskable = Kademlia::CSearchManager::RequestMoreResults(searchID, &fired);
	AddLogLineN(fired ? _("Kad search: requested wider results from one more peer.")
			  : _("Kad search: no peer left to reask for more results (cap reached or no "
			      "responses yet)."));
	return reaskable;
}

void CSearchList::StopSearch(bool globalOnly)
{
	if (m_searchType == GlobalSearch) {
		FinalizeGlobalSearch();
		m_currentSearch = -1;
	} else if (m_searchType == LocalSearch) {
		// A local search has no sweep to halt, but it does have an armed
		// answer timeout and an m_searchInProgress that nothing else clears.
		// Leaving both is what made a stopped local search hang; leaving just
		// the timer would be worse still, announcing a timeout for a search
		// the user stopped, against a server that may well have answered.
		//
		// Not gated on globalOnly: that spares Kad, and a local search is
		// ed2k, exactly like the global branch above.
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
	// If this is the in-flight ed2k global sweep, finalize it. ed2k is
	// single-in-flight, so only the current search can be running.
	if (searchID == m_currentSearch && m_searchInProgress) {
		FinalizeGlobalSearch();
	}
	// Results are intentionally retained — the multi-search EC "stop" halts
	// activity but keeps the bucket; RemoveResults() is the free path.
}

void CSearchList::SetKadSearchFinished(uint32_t searchID)
{
	m_finishedKadSearches.insert(searchID);
	// Legacy scalar for the single-search (parameterless GetSearchProgress /
	// GetSearchLifecycleState) path.
	m_KadSearchFinished = true;
}

CSearchList::SearchLifecycleState CSearchList::GetSearchLifecycleStateById(wxUIntPtr searchID) const
{
	uint32_t sid = static_cast<uint32_t>(searchID);
	// A browse ("View Files") is not a CSearchList search: CBrowseManager owns
	// its lifecycle, keyed by the same id this function is asked about.
	// Without this the generic tail below decides it from retained results,
	// which is wrong in both directions -- a browse still streaming reports
	// finished as soon as its first directory lands, and one the peer denied
	// reports idle forever, since a failed browse has no results to flip the
	// ternary. Same mapping the EC progress reply applies (ExternalConn.cpp's
	// AppendSearchProgress), here instead of only there so the SEARCH_LIST
	// listing cannot disagree with the per-id progress reply about the same id.
	if (theApp->browsemanager->Has(sid)) {
		return theApp->browsemanager->StateOf(sid) == browse::State::InProgress
			       ? SEARCH_LIFECYCLE_RUNNING
			       : SEARCH_LIFECYCLE_FINISHED;
	}
	// Kad searches are tracked per-ID: still registered in the manager =>
	// running; recorded as finished (its CSearch was destroyed on the result
	// cap or the 45s lifetime) => finished. Independent of any other search, so
	// one search ending never reports a different running search as finished.
	if (IsKadSearch(sid)) {
		return SEARCH_LIFECYCLE_RUNNING;
	}
	if (m_finishedKadSearches.count(sid)) {
		return SEARCH_LIFECYCLE_FINISHED;
	}
	// ed2k / non-Kad: the most-recently-started search owns the scalar state.
	if (searchID == m_currentSearch) {
		return GetSearchLifecycleState();
	}
	// Otherwise this id is done, whatever any sweep is still doing: results are
	// filed under the scalar m_currentSearch (ProcessSearchAnswer /
	// ProcessUDPSearchAnswer), so once a newer ed2k search takes that scalar
	// nothing further can land in this id's bucket. Unreachable, not idle.
	//
	// Deliberately NOT "no other ed2k search is in flight", which is false on
	// one of the two paths: only the EC start handler finalizes the previous
	// sweep (StopInFlightEd2kSearch, whose sole caller it is), while the
	// monolithic dialog calls StartNewSearch directly and merely abandons it,
	// so an older search's sweep may well still be running. Reachability of the
	// bucket is the narrower property, and the one that holds on both paths.
	//
	// Deciding this from the retained results instead reported every search
	// that indexed *nothing* as IDLE forever -- a query no server matched, one
	// whose every hit AddToList dropped on the file-type filter, or a global
	// sweep stopped before its first result. IDLE is not terminal, so
	// GetSearchBarStatusById fell through to the running percent (0) and every
	// consumer read a finished search as running at 0%: the Stop button stayed
	// live, the bar never cleared, the EC lifecycle tag said "idle", and the
	// "an eD2k search is still running" prompt fired on a tab that had long
	// since finished (issue #1103).
	//
	// IsKnownSearchId is the discriminator the result bucket could not be:
	// m_searchStrings is written when the search starts, independent of what
	// it kept, so a search started here reads FINISHED whether or not it
	// retained anything, while an id that is genuinely unknown -- never
	// started here, or already evicted -- still reads IDLE.
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
	// RUNNING. A Kad search gets a cosmetic time-ramp from *its own* start time
	// (looked up per-search), so a Kad search still running while a later ed2k
	// search is the current one ramps correctly instead of sticking near full.
	// An in-flight ed2k global uses its real server-queue percent — that state
	// is single-slot, so it only applies to the current search.
	const uint32_t sid = static_cast<uint32_t>(searchID);
	// A browse reports the share of the peer's directory list received so far,
	// rather than the 0/100 the state switch above would derive.
	//
	// Straight from the manager, NOT through GetSearchBarStatusById: that
	// function falls through to this one for anything it does not consider
	// finished, so routing this branch through it would recurse between the
	// two.
	if (theApp->browsemanager->Has(sid)) {
		const uint16 pct = theApp->browsemanager->BarValue(sid);
		// 0xffff is the bar's terminal sentinel, not a percent.
		return (pct == 0xffff) ? 100 : static_cast<uint8>(pct);
	}
	if (IsOrWasKadSearch(sid)) {
		std::map<uint32_t, time_t>::const_iterator it = m_searchStartTimes.find(sid);
		time_t start = (it != m_searchStartTimes.end()) ? it->second : m_searchStart;
		time_t elapsed = time(nullptr) - start;
		if (elapsed <= 0) {
			return 0;
		}
		uint32 pct = (uint32)((elapsed * 100) / SEARCHKEYWORD_LIFETIME);
		return (pct > 99) ? 99 : (uint8)pct;
	}
	if (searchID == m_currentSearch && m_searchType == GlobalSearch) {
		uint32 pct = GetSearchProgress();
		return (pct > 100) ? 100 : (uint8)pct;
	}
	return 0;
}

uint32 CSearchList::GetSearchBarStatusById(wxUIntPtr searchID) const
{
	// A "View Files" browse tab isn't a CSearchList search: CBrowseManager
	// owns its lifecycle, so ask there rather than keeping a second copy here
	// that would have to be kept in step by hand.
	if (theApp->browsemanager->Has(static_cast<uint32_t>(searchID))) {
		return theApp->browsemanager->BarValue(static_cast<uint32_t>(searchID));
	}
	if (GetSearchLifecycleStateById(searchID) == SEARCH_LIFECYCLE_FINISHED) {
		return IsOrWasKadSearch(static_cast<uint32_t>(searchID)) ? 0xfffe : 0xffff;
	}
	// RUNNING or IDLE both map to the running percent (0 when idle).
	return GetSearchLifecyclePercentById(searchID);
}

void CSearchList::StopInFlightEd2kSearch()
{
	// m_searchInProgress is true only while an ed2k (local/global) search is
	// in flight. A local search finishes synchronously (LocalSearchEnd on the
	// server reply), so in practice this finalizes an in-progress global sweep
	// — halting the timer and dropping the packet so no further UDP results
	// are filed under the outgoing m_currentSearch. The results already
	// collected are retained; the caller starts a fresh search next.
	if (m_searchInProgress) {
		FinalizeGlobalSearch();
	}
}

void CSearchList::FinalizeGlobalSearch()
{
	m_ed2kSearchFinished = true;
	// Order is crucial here: on wx_MSW an additional event can be
	// generated during the stop. So the packet has to be deleted
	// first, so that OnGlobalSearchTimer() returns immediately
	// (packet-null early return) without re-entering this path.
	delete m_searchPacket;
	m_searchPacket = NULL;
	m_searchInProgress = false;
	m_searchTimer.Stop();
	m_awaitingServerAnswer = false;

	CoreNotify_Search_Update_Progress(0xffff);
}

CSearchList::CMemFilePtr CSearchList::CreateSearchData(
	CSearchParams &params, SearchType type, bool supports64bit, bool &packetUsing64bit)
{
	// Count the number of used parameters
	unsigned int parametercount = 0;
	if (!params.typeText.IsEmpty())
		++parametercount;
	if (params.minSize > 0)
		++parametercount;
	if (params.maxSize > 0)
		++parametercount;
	if (params.availability > 0)
		++parametercount;
	if (!params.extension.IsEmpty())
		++parametercount;

	wxString typeText = params.typeText;
	if (typeText == ED2KFTSTR_ARCHIVE) {
		// eDonkeyHybrid 0.48 uses type "Pro" for archives files
		// www.filedonkey.com uses type "Pro" for archives files
		typeText = ED2KFTSTR_PROGRAM;
	} else if (typeText == ED2KFTSTR_CDIMAGE) {
		// eDonkeyHybrid 0.48 uses *no* type for iso/nrg/cue/img files
		// www.filedonkey.com uses type "Pro" for CD-image files
		typeText = ED2KFTSTR_PROGRAM;
	}

	// Must write parametercount - 1 parameter headers
	CMemFilePtr data(new CMemFile(100));

	_astrParserErrors.Empty();
	_SearchExpr.m_aExpr.Empty();

	s_strCurKadKeyword.Clear();
	if (type == KadSearch) {
		wxASSERT(!params.strKeyword.IsEmpty());
		s_strCurKadKeyword = params.strKeyword;
	}

	LexInit(params.searchString);
	int iParseResult = yyparse();
	LexFree();

	if (_astrParserErrors.GetCount() > 0) {
		for (unsigned int i = 0; i < _astrParserErrors.GetCount(); ++i) {
			AddLogLineNS(CFormat(_("Error %u: %s\n")) % i % _astrParserErrors[i]);
		}

		return CMemFilePtr(nullptr);
	}

	if (iParseResult != 0) {
		_astrParserErrors.Add(CFormat("Undefined error %i on search expression") % iParseResult);

		return CMemFilePtr(nullptr);
	}

	if (type == KadSearch && s_strCurKadKeyword != params.strKeyword) {
		AddDebugLogLineN(logSearch,
			CFormat("Keyword was rearranged, using '%s' instead of '%s'") % s_strCurKadKeyword %
				params.strKeyword);
		params.strKeyword = s_strCurKadKeyword;
	}

	parametercount += _SearchExpr.m_aExpr.GetCount();

	/* Leave the unicode comment there, please... */
	CSearchExprTarget target(data.get(),
		true /*I assume everyone is unicoded */ ? utf8strRaw : utf8strNone,
		supports64bit,
		packetUsing64bit);

	unsigned int iParameterCount = 0;
	if (_SearchExpr.m_aExpr.GetCount() <= 1) {
		// lugdunummaster requested that searches without OR or NOT operators,
		// and hence with no more expressions than the string itself, be sent
		// using a series of ANDed terms, intersecting the ANDs on the terms
		// (but prepending them) instead of putting the boolean tree at the start
		// like other searches. This type of search is supposed to take less load
		// on servers. Go figure.
		//
		// input:      "a" AND min=1 AND max=2
		// instead of: AND AND "a" min=1 max=2
		// we use:     AND "a" AND min=1 max=2

		if (_SearchExpr.m_aExpr.GetCount() > 0) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(_SearchExpr.m_aExpr[0]);
		}

		if (!typeText.IsEmpty()) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			// Type is always ascii string
			target.WriteMetaDataSearchParamASCII(FT_FILETYPE, typeText);
		}

		if (params.minSize > 0) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(FT_FILESIZE, ED2K_SEARCH_OP_GREATER, params.minSize);
		}

		if (params.maxSize > 0) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(FT_FILESIZE, ED2K_SEARCH_OP_LESS, params.maxSize);
		}

		if (params.availability > 0) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(
				FT_SOURCES, ED2K_SEARCH_OP_GREATER, params.availability);
		}

		if (!params.extension.IsEmpty()) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(FT_FILEFORMAT, params.extension);
		}

// #warning TODO - I keep this here, ready if we ever allow such searches...
#if 0
		if (complete > 0){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(FT_COMPLETE_SOURCES, ED2K_SEARCH_OP_GREATER, complete);
		}

		if (minBitrate > 0){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_BITRATE : FT_ED2K_MEDIA_BITRATE, ED2K_SEARCH_OP_GREATER, minBitrate);
		}

		if (minLength > 0){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_LENGTH : FT_ED2K_MEDIA_LENGTH, ED2K_SEARCH_OP_GREATER, minLength);
		}

		if (!codec.IsEmpty()){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_CODEC : FT_ED2K_MEDIA_CODEC, codec);
		}

		if (!title.IsEmpty()){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_TITLE : FT_ED2K_MEDIA_TITLE, title);
		}

		if (!album.IsEmpty()){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_ALBUM : FT_ED2K_MEDIA_ALBUM, album);
		}

		if (!artist.IsEmpty()){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_ARTIST : FT_ED2K_MEDIA_ARTIST, artist);
		}
#endif // 0

		// If this assert fails... we're seriously fucked up

		wxASSERT(iParameterCount == parametercount);

	} else {
		if (!params.extension.IsEmpty()) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (params.availability > 0) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (params.maxSize > 0) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (params.minSize > 0) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (!typeText.IsEmpty()) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

// #warning TODO - same as above...
#if 0
		if (complete > 0){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (minBitrate > 0){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (minLength > 0) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (!codec.IsEmpty()){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (!title.IsEmpty()){
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (!album.IsEmpty()) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}

		if (!artist.IsEmpty()) {
			if (++iParameterCount < parametercount) {
				target.WriteBooleanAND();
			}
		}
#endif // 0

		// As above, if this fails, we're seriously fucked up.
		wxASSERT(iParameterCount + _SearchExpr.m_aExpr.GetCount() == parametercount);

		for (unsigned int j = 0; j < _SearchExpr.m_aExpr.GetCount(); ++j) {
			if (_SearchExpr.m_aExpr[j] == SEARCHOPTOK_AND) {
				target.WriteBooleanAND();
			} else if (_SearchExpr.m_aExpr[j] == SEARCHOPTOK_OR) {
				target.WriteBooleanOR();
			} else if (_SearchExpr.m_aExpr[j] == SEARCHOPTOK_NOT) {
				target.WriteBooleanNOT();
			} else {
				target.WriteMetaDataSearchParam(_SearchExpr.m_aExpr[j]);
			}
		}

		if (!params.typeText.IsEmpty()) {
			// Type is always ASCII string
			target.WriteMetaDataSearchParamASCII(FT_FILETYPE, params.typeText);
		}

		if (params.minSize > 0) {
			target.WriteMetaDataSearchParam(FT_FILESIZE, ED2K_SEARCH_OP_GREATER, params.minSize);
		}

		if (params.maxSize > 0) {
			target.WriteMetaDataSearchParam(FT_FILESIZE, ED2K_SEARCH_OP_LESS, params.maxSize);
		}

		if (params.availability > 0) {
			target.WriteMetaDataSearchParam(
				FT_SOURCES, ED2K_SEARCH_OP_GREATER, params.availability);
		}

		if (!params.extension.IsEmpty()) {
			target.WriteMetaDataSearchParam(FT_FILEFORMAT, params.extension);
		}

// #warning TODO - third and last warning of the same series.
#if 0
		if (complete > 0) {
			target.WriteMetaDataSearchParam(FT_COMPLETE_SOURCES, ED2K_SEARCH_OP_GREATER, pParams->uComplete);
		}

		if (minBitrate > 0) {
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_BITRATE : FT_ED2K_MEDIA_BITRATE, ED2K_SEARCH_OP_GREATER, minBitrate);
		}

		if (minLength > 0) {
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_LENGTH : FT_ED2K_MEDIA_LENGTH, ED2K_SEARCH_OP_GREATER, minLength);
		}

		if (!codec.IsEmpty()) {
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_CODEC : FT_ED2K_MEDIA_CODEC, codec);
		}

		if (!title.IsEmpty()) {
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_TITLE : FT_ED2K_MEDIA_TITLE, title);
		}

		if (!album.IsEmpty()) {
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_ALBUM : FT_ED2K_MEDIA_ALBUM, album);
		}

		if (!artist.IsEmpty()) {
			target.WriteMetaDataSearchParam(type == KadSearch ? TAG_MEDIA_ARTIST : FT_ED2K_MEDIA_ARTIST, artist);
		}

#endif // 0
	}

	// Packet ready to go.
	return data;
}

void CSearchList::KademliaSearchKeyword(uint32_t searchID,
	const Kademlia::CUInt128 *fileID,
	const wxString &name,
	uint64_t size,
	const wxString &type,
	uint32_t kadPublishInfo,
	const TagPtrList &taglist)
{
	EUtf8Str eStrEncode = utf8strRaw;

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

	CSearchFile *tempFile = new CSearchFile(temp, (eStrEncode == utf8strRaw), searchID, 0, 0, "", true);
	tempFile->SetKadPublishInfo(kadPublishInfo);

	AddToList(tempFile);
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
