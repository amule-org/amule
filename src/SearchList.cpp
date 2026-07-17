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

#include <protocol/Protocols.h>
#include <protocol/kad/Constants.h>
#include <tags/ClientTags.h>
#include <tags/FileTags.h>

#include "updownclient.h"    // Needed for CUpDownClient
#include "MemFile.h"         // Needed for CMemFile
#include "amule.h"           // Needed for theApp
#include "ServerConnect.h"   // Needed for theApp->serverconnect
#include "Server.h"          // Needed for CServer
#include "ServerList.h"      // Needed for theApp->serverlist
#include "Statistics.h"      // Needed for theStats
#include "ObservableQueue.h" // Needed for CQueueObserver
#include <common/Format.h>
#include "Logger.h"    // Needed for AddLogLineM/...
#include "Packet.h"    // Needed for CPacket
#include "GuiEvents.h" // Needed for Notify_*

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
, m_searchStart(0)
{
}

CSearchList::~CSearchList()
{
	StopSearch();

	while (!m_results.empty()) {
		RemoveResults(m_results.begin()->first);
	}
}

void CSearchList::RemoveResults(wxUIntPtr searchID)
{
	// A non-existent search id will just be ignored
	Kademlia::CSearchManager::StopSearch(searchID, true);

	// Drop any per-search tracking for this ID (bounded growth).
	m_finishedKadSearches.erase(static_cast<uint32_t>(searchID));
	m_searchStartTimes.erase(static_cast<uint32_t>(searchID));
	m_browseBar.erase(searchID);

	ResultMap::iterator it = m_results.find(searchID);
	if (it != m_results.end()) {
		CSearchResultList &list = it->second;

		for (size_t i = 0; i < list.size(); ++i) {
			delete list.at(i);
		}

		m_results.erase(it);
	}
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

	return "";
}

void CSearchList::LocalSearchEnd()
{
	if (m_searchType == GlobalSearch) {
		wxCHECK_RET(m_searchPacket, "Global search, but no packet");

		// Ensure that every global search starts over.
		theApp->serverlist->RemoveObserver(&m_serverQueue);
		m_searchTimer.Start(750);
	} else {
		m_searchInProgress = false;
		m_ed2kSearchFinished = true;
		Notify_SearchLocalEnd();
	}
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
	ResultMap::const_iterator it = m_results.find(m_currentSearch);
	return (it == m_results.end()) ? 0 : it->second.size();
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

	// Route the browsed listing to a result bucket. For an EC-initiated browse
	// (remote GUI) the daemon has pinned a real, wire-safe search ID on the
	// client; use it so the union/per-ID poll and the LRU ring can address these
	// results. A monolithic local browse leaves it 0 and keeps the historical
	// per-client-pointer key.
	wxUIntPtr searchID = sender->GetBrowseSearchId() ? static_cast<wxUIntPtr>(sender->GetBrowseSearchId())
							 : reinterpret_cast<wxUIntPtr>(sender);

#ifndef AMULE_DAEMON
	// Find-or-create the peer's "View Files" tab, keyed by ECID (so two peers
	// sharing a nick don't collapse into one tab, and a re-browse refreshes the
	// same tab). Marks the tab as browsing; the terminal paths flip it to
	// finished/failed via Notify_Browse_Status.
	theApp->amuledlg->m_searchwnd->EnsureBrowseTab(sender->ECID(), sender->GetUserName(), searchID);
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

void CSearchList::ProcessSearchAnswer(
	const uint8_t *in_packet, uint32_t size, bool optUTF8, uint32_t serverIP, uint16_t serverPort)
{
	if (!IsActiveSearchTypeEd2k(m_searchType)) {
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
	if (!IsActiveSearchTypeEd2k(m_searchType)) {
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

	// Get, or implicitly create, the map of results for this search
	CSearchResultList &results = m_results[toadd->GetSearchID()];

	for (size_t i = 0; i < results.size(); ++i) {
		CSearchFile *item = results.at(i);

		if ((toadd->GetFileHash() == item->GetFileHash()) &&
			(toadd->GetFileSize() == item->GetFileSize())) {
			AddDebugLogLineN(logSearch,
				CFormat("Received duplicate results for '%s' : %s") % item->GetFileName() %
					item->GetFileHash().Encode());
			// Add the child, possibly updating the parents filename.
			item->AddChild(toadd);
			Notify_Search_Update_Sources(item);
			return true;
		}
	}

	AddDebugLogLineN(logSearch,
		CFormat("Added new result '%s' : %s") % toadd->GetFileName() % toadd->GetFileHash().Encode());

	// New unique result, simply add and display.
	results.push_back(toadd);
	Notify_Search_Add_Result(toadd);

	return true;
}

const CSearchResultList &CSearchList::GetSearchResults(wxUIntPtr searchID) const
{
	ResultMap::const_iterator it = m_results.find(searchID);
	if (it != m_results.end()) {
		return it->second;
	}

	// TODO: Should we assert in this case?
	static CSearchResultList list;
	return list;
}

void CSearchList::AddFileToDownloadByHash(const CMD4Hash &hash, uint8 cat)
{
	ResultMap::iterator it = m_results.begin();
	for (; it != m_results.end(); ++it) {
		CSearchResultList &list = it->second;

		for (unsigned int i = 0; i < list.size(); ++i) {
			if (list[i]->GetFileHash() == hash) {
				CoreNotify_Search_Add_Download(list[i], cat);

				return;
			}
		}
	}
}

CSearchFile *CSearchList::GetSearchFileByID(const CMD4Hash &hash) const
{
	for (const auto &entry : m_results) {
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
	for (const auto &entry : m_results) {
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
	for (auto &entry : m_results) {
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

bool CSearchList::RequestMoreResults(uint32_t searchID)
{
	return Kademlia::CSearchManager::RequestMoreResults(searchID);
}

void CSearchList::StopSearch(bool globalOnly)
{
	if (m_searchType == GlobalSearch) {
		FinalizeGlobalSearch();
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
	// Otherwise: results retained => finished; nothing => idle/unknown.
	return (m_results.find(searchID) != m_results.end()) ? SEARCH_LIFECYCLE_FINISHED
							     : SEARCH_LIFECYCLE_IDLE;
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
	// A "View Files" browse tab isn't a CSearchList search: its bar value is
	// pushed by the browsing client. Return it directly when present.
	if (!m_browseBar.empty()) {
		std::map<wxUIntPtr, uint16>::const_iterator it = m_browseBar.find(searchID);
		if (it != m_browseBar.end()) {
			return it->second;
		}
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
			AddLogLineNS(CFormat("Error %u: %s\n") % i % _astrParserErrors[i]);
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
	for (ResultMap::iterator it = m_results.begin(); it != m_results.end(); ++it) {
		CSearchResultList &results = it->second;
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
