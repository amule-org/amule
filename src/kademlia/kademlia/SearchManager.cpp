//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2003-2011 Barry Dunne (http://www.emule-project.net)
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

// Note To Mods //
/*
Please do not change anything here and release it..
There is going to be a new forum created just for the Kademlia side of the client..
If you feel there is an error or a way to improve something, please
post it in the forum first and let us look at it.. If it is a real improvement,
it will be added to the official client.. Changing something without knowing
what all it does can cause great harm to the network if released in mass form..
Any mod that changes anything within the Kademlia side will not be allowed to advertise
there client on the eMule forum..
*/

#include <wx/wx.h>

#include "Search.h"
#include "../utils/LookupDiagnostics.h"
#include <algorithm>
#include "../../GetTickCount.h"
#include <common/Macros.h>

#include "Indexed.h"
#include "Defines.h"
#include "../routing/Contact.h"
#include "../../MemFile.h"
#include "../../Logger.h"
#include "../../RandomFunctions.h"  // Needed for GetRandomUInt128()
#include "../../OtherFunctions.h"   // Needed for DeleteContents()
#include "../../CompilerSpecific.h" // Needed for __FUNCTION__

#include <wx/tokenzr.h>

////////////////////////////////////////
using namespace Kademlia;
////////////////////////////////////////

// Top bit reserved for Kad-allocated IDs; ed2k Local/Global IDs (CSearchDlg::StartNewSearch) live
// in the bottom half, so the two ID spaces cannot collide however long the session runs.
#define SEARCH_ID_KAD_MASK 0x80000000
uint32_t CSearchManager::m_nextID = SEARCH_ID_KAD_MASK;
SearchMap CSearchManager::m_searches;

bool CSearchManager::IsSearching(uint32_t searchID) noexcept
{
	for (SearchMap::const_iterator it = m_searches.begin(); it != m_searches.end(); ++it) {
		if (it->second->HasSearchID() && it->second->GetSearchID() == searchID) {
			return true;
		}
	}
	return false;
}

bool CSearchManager::RequestMoreResults(uint32_t searchID, bool *out_fired)
{
	if (out_fired) {
		*out_fired = false;
	}
	// Linear scan because m_searches is keyed by target hash, not searchID. CSearch counts are
	// tiny at any one time (one per active user search plus internal lookups), so the scan cost
	// is negligible.
	for (SearchMap::iterator it = m_searches.begin(); it != m_searches.end(); ++it) {
		if (it->second->HasSearchID() && it->second->GetSearchID() == searchID) {
			const bool fired = it->second->RequestMoreResults();
			if (out_fired) {
				*out_fired = fired;
			}
			// Evaluated AFTER the attempt, and or-ed with it: the reask that consumes
			// the last of the budget really happened, but leaves CanReaskMore() false.
			// Reporting that as a refusal would have every client disable its control
			// on a success.
			return fired || it->second->CanReaskMore();
		}
	}
	// No live search carries that id -- it finished and was deleted, taking
	// m_responded / m_tried / m_requestedMoreNodes with it. Terminal.
	return false;
}

bool CSearchManager::IsKadSearch(uint32_t searchID)
{
	for (SearchMap::const_iterator it = m_searches.begin(); it != m_searches.end(); ++it) {
		// Skip searches that were never given an id. Their m_searchID is the constructor's
		// 0xFFFFFFFF, which is also the single bucket every pre-multi-search EC client
		// reuses for all of its searches -- so without this an ordinary node lookup, buddy
		// lookup or UDP firewall check answers "yes, that is a running Kad search" for a
		// legacy client's ed2k search, and the whole per-id lifecycle follows. Only
		// PrepareFindKeywords and PrepareLookup assign an id.
		if (!it->second->HasSearchID()) {
			continue;
		}
		if (it->second->GetSearchID() == searchID) {
			return true;
		}
	}
	return false;
}

void CSearchManager::StopSearch(uint32_t searchID, bool delayDelete)
{
	for (SearchMap::iterator it = m_searches.begin(); it != m_searches.end(); ++it) {
		if (it->second->HasSearchID() && it->second->GetSearchID() == searchID) {
			// Do not delete as we want to get a chance for late packets to be processed.
			if (delayDelete) {
				it->second->PrepareToStop();
			} else {
				DeleteSearch(it);
			}
			return;
		}
	}
}

void CSearchManager::DeleteSearch(SearchMap::iterator it)
{
	// Destructors notify the GUI and core synchronously. Those callbacks may
	// query the registry, so no entry may point at an object being destroyed.
	auto search = std::move(it->second);
	m_searches.erase(it);
	// Diagnostics must never prevent search cleanup or retain an object in the registry.
	try {
		RememberLookup(*search);
	} catch (...) {
	}
}

void CSearchManager::StopAllSearches()
{
	SearchMap stopped;
	stopped.swap(m_searches);
	for (const auto &item : stopped) {
		try {
			RememberLookup(*item.second);
		} catch (...) {
		}
	}
	// Every search is invisible before the first destructor invokes a callback.
}

bool CSearchManager::StartSearch(CSearch *search)
{
	std::unique_ptr<CSearch> owned(search);
	const auto target = search->GetTarget();
	if (AlreadySearchingFor(target)) {
		return false;
	}
	// Keep local ownership if registration rejects a duplicate target.
	const bool inserted = m_searches.try_emplace(target, std::move(owned)).second;
	if (!inserted) {
		return false;
	}
	try {
		search->Go();
	} catch (...) {
		// Go can throw after registration. Detach before freeing the search,
		// otherwise the next response/stop would dereference a dangling entry.
		// Go may invoke callbacks: do not reuse an iterator across that boundary.
		const auto registered = m_searches.find(target);
		if (registered != m_searches.end() && registered->second.get() == search) {
			DeleteSearch(registered);
		}
		throw;
	}
	return true;
}

CSearch *CSearchManager::PrepareFindKeywords(const wxString &keyword,
	uint32_t searchTermsDataSize,
	const uint8_t *searchTermsData,
	uint32_t searchid)
{
	// Create a keyword search object.
	auto s = std::make_unique<CSearch>();
	try {
		// Set search to a keyword type.
		s->SetSearchTypes(CSearch::KEYWORD);
		s->m_lookupKeyword = keyword;

		// Make sure we have a keyword list
		GetWords(keyword, &s->m_words, true);
		if (s->m_words.size() == 0) {
			throw wxString(_("Kademlia: search keyword too short"));
		}

		wxString wstrKeyword = s->m_words.front();

		AddDebugLogLineN(logSearch, CFormat("Keyword for search: %s") % wstrKeyword);

		// Kry - I just decided to assume everyone is unicoded
		// GonoszTopi - seconded
		KadGetKeywordHash(wstrKeyword, &s->m_target);

		// Kad routes replies by target. Never preempt another client's search.
		s->SetSearchTermData(searchTermsDataSize, searchTermsData);
		if (m_searches.find(s->m_target) != m_searches.end()) {
			throw _("Kademlia: Search keyword is already on search list: ") + wstrKeyword;
		}

		// Inc our searchID
		// If called from external client use predefined search id
		s->SetSearchID(
			(searchid & 0xffffff00) == 0xffffff00 ? searchid : (++m_nextID | SEARCH_ID_KAD_MASK));
		CSearch *started = s.get();
		if (!StartSearch(s.release())) {
			throw wxString(_("Kademlia: Search keyword is already on search list: ")) + keyword;
		}
		return started;
	} catch (const CEOFException &err) {
		wxString strError =
			"CEOFException in " + wxString::FromAscii(__FUNCTION__) + ": " + err.what();
		throw strError;
	} catch (const CInvalidPacket &err) {
		wxString strError = "CInvalidPacket exception in " + wxString::FromAscii(__FUNCTION__) +
				    ": " + err.what();
		throw strError;
	}
}

CSearch *CSearchManager::PrepareLookup(uint32_t type, bool start, const CUInt128 &id)
{
	// Prepare a kad lookup.
	// Make sure this target is not already in progress.
	if (AlreadySearchingFor(id)) {
		return NULL;
	}

	// Create a new search.
	auto s = std::make_unique<CSearch>();

	// Set type and target.
	s->SetSearchTypes(type);
	s->SetTargetID(id);

	try {
		switch (type) {
		case CSearch::STOREKEYWORD:
			if (!Kademlia::CKademlia::GetIndexed()->SendStoreRequest(id)) {
				return NULL;
			}
			break;
		}

		s->SetSearchID((++m_nextID | SEARCH_ID_KAD_MASK));
		if (start) {
			CSearch *started = s.get();
			return StartSearch(s.release()) ? started : nullptr;
		}
	} catch (const CEOFException &DEBUG_ONLY(err)) {
		AddDebugLogLineN(
			logKadSearch, "CEOFException in CSearchManager::PrepareLookup: " + err.what());
		return NULL;
	} catch (...) {
		AddDebugLogLineN(logKadSearch, "Exception in CSearchManager::PrepareLookup");
		throw;
	}

	return s.release();
}

void CSearchManager::FindNode(const CUInt128 &id, bool complete)
{
	// Do a node lookup.
	CSearch *s = new CSearch;
	if (complete) {
		s->SetSearchTypes(CSearch::NODECOMPLETE);
	} else {
		s->SetSearchTypes(CSearch::NODE);
	}
	s->SetTargetID(id);
	StartSearch(s);
}

bool CSearchManager::IsFWCheckUDPSearch(const CUInt128 &target)
{
	// Check if this target is in the search map.
	SearchMap::const_iterator it = m_searches.find(target);
	if (it != m_searches.end()) {
		return (it->second->GetSearchTypes() == CSearch::NODEFWCHECKUDP);
	}
	return false;
}

void CSearchManager::GetWords(const wxString &str, WordList *words, bool allowDuplicates)
{
	wxString current_word;
	wxStringTokenizer tkz(str, GetInvalidKeywordChars());
	while (tkz.HasMoreTokens()) {
		current_word = tkz.GetNextToken();
		// TODO: we need a safe way to tell whether a 3-character sequence is a real word.
		// For now we go by the UTF-8 byte count, which works for Western locales AS LONG AS
		// the minimum byte count is 3(!). At 2 it breaks, many Western characters needing 2
		// bytes in UTF-8. Evaluating the Unicode character values, and whether they sit in
		// ranges where single characters are words, may be the answer.
		if (strlen((const char *)(current_word.utf8_str())) >= 3) {
			current_word.MakeLower();
			if (!allowDuplicates) {
				words->remove(current_word);
			}
			words->push_back(current_word);
		}
	}
}

void CSearchManager::JumpStart()
{
	// Find any searches that has stalled and jumpstart them.
	// This will also prune all searches.
	time_t now = time(NULL);
	SearchMap::iterator next_it = m_searches.begin();
	while (next_it != m_searches.end()) {
		SearchMap::iterator current_it = next_it++; /* don't change this to a ++next_it! */
		switch (current_it->second->GetSearchTypes()) {
		case CSearch::FILE: {
			if (current_it->second->m_created + SEARCHFILE_LIFETIME < now) {
				DeleteSearch(current_it);
			} else if (current_it->second->GetAnswers() > SEARCHFILE_TOTAL ||
				   current_it->second->m_created + SEARCHFILE_LIFETIME - SEC(20) < now) {
				current_it->second->PrepareToStop();
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::KEYWORD: {
			if (current_it->second->m_created + SEARCHKEYWORD_LIFETIME < now) {
				DeleteSearch(current_it);
			} else if (current_it->second->GetAnswers() > SEARCHKEYWORD_TOTAL ||
				   current_it->second->m_created + SEARCHKEYWORD_LIFETIME - SEC(20) < now) {
				current_it->second->PrepareToStop();
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::NOTES: {
			if (current_it->second->m_created + SEARCHNOTES_LIFETIME < now) {
				DeleteSearch(current_it);
			} else if (current_it->second->GetAnswers() > SEARCHNOTES_TOTAL ||
				   current_it->second->m_created + SEARCHNOTES_LIFETIME - SEC(20) < now) {
				current_it->second->PrepareToStop();
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::FINDBUDDY: {
			if (current_it->second->m_created + SEARCHFINDBUDDY_LIFETIME < now) {
				DeleteSearch(current_it);
			} else if (current_it->second->GetAnswers() > SEARCHFINDBUDDY_TOTAL ||
				   current_it->second->m_created + SEARCHFINDBUDDY_LIFETIME - SEC(20) < now) {
				current_it->second->PrepareToStop();
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::FINDSOURCE: {
			if (current_it->second->m_created + SEARCHFINDSOURCE_LIFETIME < now) {
				DeleteSearch(current_it);
			} else if (current_it->second->GetAnswers() > SEARCHFINDSOURCE_TOTAL ||
				   current_it->second->m_created + SEARCHFINDSOURCE_LIFETIME - SEC(20) <
					   now) {
				current_it->second->PrepareToStop();
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::NODE:
		case CSearch::NODESPECIAL:
		case CSearch::NODEFWCHECKUDP: {
			if (current_it->second->m_created + SEARCHNODE_LIFETIME < now) {
				DeleteSearch(current_it);
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::NODECOMPLETE: {
			if (current_it->second->m_created + SEARCHNODE_LIFETIME < now) {
				// Tell Kad it can start publishing.
				CKademlia::GetPrefs()->SetPublish(true);
				DeleteSearch(current_it);
			} else if ((current_it->second->m_created + SEARCHNODECOMP_LIFETIME < now) &&
				   (current_it->second->GetAnswers() > SEARCHNODECOMP_TOTAL)) {
				// Tell Kad it can start publishing.
				CKademlia::GetPrefs()->SetPublish(true);
				DeleteSearch(current_it);
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::STOREFILE: {
			if (current_it->second->m_created + SEARCHSTOREFILE_LIFETIME < now) {
				DeleteSearch(current_it);
			} else if (current_it->second->GetAnswers() > SEARCHSTOREFILE_TOTAL ||
				   current_it->second->m_created + SEARCHSTOREFILE_LIFETIME - SEC(20) < now) {
				current_it->second->PrepareToStop();
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::STOREKEYWORD: {
			if (current_it->second->m_created + SEARCHSTOREKEYWORD_LIFETIME < now) {
				DeleteSearch(current_it);
			} else if (current_it->second->GetAnswers() > SEARCHSTOREKEYWORD_TOTAL ||
				   current_it->second->m_created + SEARCHSTOREKEYWORD_LIFETIME - SEC(20) <
					   now) {
				current_it->second->PrepareToStop();
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		case CSearch::STORENOTES: {
			if (current_it->second->m_created + SEARCHSTORENOTES_LIFETIME < now) {
				DeleteSearch(current_it);
			} else if (current_it->second->GetAnswers() > SEARCHSTORENOTES_TOTAL ||
				   current_it->second->m_created + SEARCHSTORENOTES_LIFETIME - SEC(20) <
					   now) {
				current_it->second->PrepareToStop();
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		default: {
			if (current_it->second->m_created + SEARCH_LIFETIME < now) {
				DeleteSearch(current_it);
			} else {
				current_it->second->JumpStart();
			}
			break;
		}
		}
	}
}

void CSearchManager::UpdateStats() noexcept
{
	uint8_t m_totalFile = 0;
	uint8_t m_totalStoreSrc = 0;
	uint8_t m_totalStoreKey = 0;
	uint8_t m_totalSource = 0;
	uint8_t m_totalNotes = 0;
	uint8_t m_totalStoreNotes = 0;

	for (SearchMap::const_iterator it = m_searches.begin(); it != m_searches.end(); ++it) {
		switch (it->second->GetSearchTypes()) {
		case CSearch::FILE: {
			m_totalFile++;
			break;
		}
		case CSearch::STOREFILE: {
			m_totalStoreSrc++;
			break;
		}
		case CSearch::STOREKEYWORD: {
			m_totalStoreKey++;
			break;
		}
		case CSearch::FINDSOURCE: {
			m_totalSource++;
			break;
		}
		case CSearch::STORENOTES: {
			m_totalStoreNotes++;
			break;
		}
		case CSearch::NOTES: {
			m_totalNotes++;
			break;
		}
		default:
			break;
		}
	}

	CPrefs *prefs = CKademlia::GetPrefs();
	prefs->SetTotalFile(m_totalFile);
	prefs->SetTotalStoreSrc(m_totalStoreSrc);
	prefs->SetTotalStoreKey(m_totalStoreKey);
	prefs->SetTotalSource(m_totalSource);
	prefs->SetTotalNotes(m_totalNotes);
	prefs->SetTotalStoreNotes(m_totalStoreNotes);
}

void CSearchManager::ProcessPublishResult(const CUInt128 &target, const uint8_t load, const bool loadResponse)
{
	// We tried to publish some info and got a result.
	CSearch *s = NULL;
	SearchMap::const_iterator it = m_searches.find(target);
	if (it != m_searches.end()) {
		s = it->second.get();
	}

	// Result could be very late and store deleted, abort.
	if (s == NULL) {
		return;
	}

	switch (s->GetSearchTypes()) {
	case CSearch::STOREKEYWORD: {
		if (loadResponse) {
			s->UpdateNodeLoad(load);
		}
		break;
	}
	case CSearch::STOREFILE:
	case CSearch::STORENOTES:
		break;
	}

	s->m_answers++;
}

void CSearchManager::ProcessResponse(
	const CUInt128 &target, uint32_t fromIP, uint16_t fromPort, ContactList *results)
{
	// We got a response to a kad lookup.
	CSearch *s = NULL;
	SearchMap::const_iterator it = m_searches.find(target);
	if (it != m_searches.end()) {
		s = it->second.get();
	}

	// If this search was deleted before this response, delete contacts and abort, otherwise process them.
	if (s == NULL) {
		AddDebugLogLineN(logKadSearch,
			"Search either never existed or receiving late results "
			"(CSearchManager::ProcessResponse)");
		DeleteContents(*results);
	} else {
		s->ProcessResponse(fromIP, fromPort, results);
	}
	delete results;
}

void CSearchManager::ProcessResult(
	const CUInt128 &target, const CUInt128 &answer, TagPtrList *info, uint32_t fromIP, uint16_t fromPort)
{
	// We have results for a request for info.
	CSearch *s = NULL;
	SearchMap::const_iterator it = m_searches.find(target);
	if (it != m_searches.end()) {
		s = it->second.get();
	}

	// If this search was deleted before these results, delete contacts and abort, otherwise process them.
	if (s == NULL) {
		AddDebugLogLineN(logKadSearch,
			"Search either never existed or receiving late results "
			"(CSearchManager::ProcessResult)");
	} else {
		s->ProcessResult(answer, info, fromIP, fromPort);
	}
}

bool CSearchManager::FindNodeSpecial(const CUInt128 &id, CKadClientSearcher *requester)
{
	// Do a node lookup.
	AddDebugLogLineN(logKadSearch, "Starting NODESPECIAL Kad Search for " + id.ToHexString());
	CSearch *search = new CSearch;
	search->SetSearchTypes(CSearch::NODESPECIAL);
	search->SetTargetID(id);
	search->SetNodeSpecialSearchRequester(requester);
	return StartSearch(search);
}

bool CSearchManager::FindNodeFWCheckUDP()
{
	CancelNodeFWCheckUDPSearch();
	CUInt128 id(GetRandomUint128());
	AddDebugLogLineN(logKadSearch, "Starting NODEFWCHECKUDP Kad Search");
	CSearch *search = new CSearch;
	search->SetSearchTypes(CSearch::NODEFWCHECKUDP);
	search->SetTargetID(id);
	return StartSearch(search);
}

void CSearchManager::CancelNodeSpecial(CKadClientSearcher *requester)
{
	// Stop a specific nodespecialsearch
	for (SearchMap::iterator it = m_searches.begin(); it != m_searches.end(); ++it) {
		if (it->second->GetNodeSpecialSearchRequester() == requester) {
			it->second->SetNodeSpecialSearchRequester(NULL);
			it->second->PrepareToStop();
			return;
		}
	}
}

void CSearchManager::CancelNodeFWCheckUDPSearch()
{
	// Stop node searches done for udp firewallcheck
	for (SearchMap::iterator it = m_searches.begin(); it != m_searches.end(); ++it) {
		if (it->second->GetSearchTypes() == CSearch::NODEFWCHECKUDP) {
			it->second->PrepareToStop();
		}
	}
}
// File_checked_for_headers

namespace
{
std::deque<LookupSnapshot> s_recentLookups;
}

void CSearchManager::RememberLookup(const CSearch &search)
{
	if (search.GetLookupTrace().Peers().empty()) {
		return;
	}
	if (s_recentLookups.size() == MaxLookupHistory) {
		s_recentLookups.pop_front();
	}
	s_recentLookups.push_back({ search.GetSearchTypes(),
		search.GetTarget().ToHexString(),
		search.m_lookupKeyword,
		search.GetLookupStarted(),
		::GetTickCount64(),
		search.GetLookupTrace() });
}

void CSearchManager::GetLookupSnapshots(
	std::vector<LookupSnapshot> &active, std::deque<LookupSnapshot> &recent)
{
	for (const auto &item : m_searches) {
		const auto &search = *item.second;
		active.push_back({ search.GetSearchTypes(),
			search.GetTarget().ToHexString(),
			search.m_lookupKeyword,
			search.GetLookupStarted(),
			0,
			search.GetLookupTrace() });
	}
	std::sort(active.begin(), active.end(), [](const LookupSnapshot &a, const LookupSnapshot &b) {
		return a.started > b.started;
	});
	if (active.size() > MaxLookupHistory) {
		active.resize(MaxLookupHistory);
	}
	recent = s_recentLookups;
}

wxString CSearchManager::GetLookupDiagnostics()
{
	std::vector<LookupSnapshot> active;
	std::deque<LookupSnapshot> recent;
	GetLookupSnapshots(active, recent);
	return FormatLookupDiagnostics(active, recent, ::GetTickCount64());
}

void CSearchManager::ProcessResultReply(const CUInt128 &target, uint32_t fromIP, uint16_t fromPort)
{
	const auto found = m_searches.find(target);
	if (found != m_searches.end()) {
		found->second->m_lookupTrace.ItemReply({ fromIP, fromPort }, ::GetTickCount64());
	}
}
