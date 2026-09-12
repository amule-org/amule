//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2003-2011 Barry Dunne (http://www.emule-project.net)
// Copyright (c) 2004-2011 Merkur ( strEmail.Format("%s@%s", "devteam", "emule-project.net") /
// http://www.emule-project.net )
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

#include "Search.h"

#include <protocol/Protocols.h>
#include <protocol/kad/Client2Client/UDP.h>
#include <protocol/kad/Constants.h>
#include <protocol/kad2/Client2Client/UDP.h>
#include <protocol/kad2/Constants.h> // Needed for KADEMLIA_VERSION9_50a
#include <tags/FileTags.h>

#include "Defines.h"
#include "AICHHashList.h"
#include "UDPFirewallTester.h"
#ifdef ENABLE_KAD_NODE_PROTECTION
#include "../net/FastKad.h"
#include "../net/SafeKad.h"
#endif
#include "../routing/RoutingZone.h"
#include "../routing/Contact.h"
#include "../net/KademliaUDPListener.h"
#include "../utils/KadClientSearcher.h"
#include "../../amule.h"
#include "../../SharedFileList.h"
#include "../../DownloadQueue.h"
#include "../../PartFile.h"
#include "../../SHAHashSet.h" // Needed for CAICHHash on Kad keyword storage
#include "../../SearchList.h"
#include "../../MemFile.h"
#include "../../ClientList.h"
#include "../../updownclient.h"
#include "../../PeerCapabilities.h" // Needed for DecodeIPv6HexTag
#include "../../Logger.h"
#include "../../Preferences.h"
#ifdef ENABLE_KAD_NODE_PROTECTION
#include "../../GetTickCount.h"     // Needed for GetTickCount64
#include "../../NetworkFunctions.h" // Needed for KadIPPortToString
#endif
#include "../../GuiEvents.h"

////////////////////////////////////////
using namespace Kademlia;
////////////////////////////////////////

// CKadAICHHashList is written against a plain 20-byte array so that the codec pinning
// TAG_KADAICHHASHPUB / TAG_KADAICHHASHRESULT stays testable without SHAHashSet.cpp behind it. This
// is where the two definitions of "AICH root hash size" meet, so this is where they are held
// together.
static_assert(
	Kademlia::KAD_AICH_HASH_SIZE == HASHSIZE, "Kad AICH hash size must match the AICH root hash size");

CSearch::CSearch()
{
	m_created = time(NULL);
	m_type = (uint32_t)-1;
	m_answers = 0;
	m_totalRequestAnswers = 0;
	m_searchID = (uint32_t)-1;
	m_searchIDAssigned = false;
	m_stopping = false;
	m_totalLoad = 0;
	m_totalLoadResponses = 0;
	m_lastResponse = m_created;
#ifdef ENABLE_KAD_NODE_PROTECTION
	m_lastResponseTick = ::GetTickCount64();
#endif
	m_searchTermsData = NULL;
	m_searchTermsDataSize = 0;
	m_nodeSpecialSearchRequester = NULL;
	m_closestDistantFound = 0;
}

CSearch::~CSearch()
{
	// remember the closest node we found and tried to contact (if any) during this search
	// for statistical caluclations, but only if its a certain type
	switch (m_type) {
	case NODECOMPLETE:
	case FILE:
	case KEYWORD:
	case NOTES:
	case STOREFILE:
	case STOREKEYWORD:
	case STORENOTES:
	case FINDSOURCE: // maybe also exclude
		if (m_closestDistantFound != 0) {
			CKademlia::StatsAddClosestDistance(m_closestDistantFound);
		}
		break;
	default: // NODE, NODESPECIAL, NODEFWCHECKUDP, FINDBUDDY
		break;
	}

	if (m_nodeSpecialSearchRequester != NULL) {
		m_nodeSpecialSearchRequester->KadSearchIPByNodeIDResult(KCSR_NOTFOUND, 0, 0);
	}

	CPartFile *temp = theApp->downloadqueue->GetFileByKadFileSearchID(GetSearchID());

	if (temp) {
		temp->SetKadFileSearchID(0);
	}

	// If this was an on-demand notes lookup, clear the running flag on the target
	// file so a fresh lookup can be requested later.
	if (m_type == NOTES) {
		uint8_t fileid[16];
		m_target.ToByteArray(fileid);
		const CMD4Hash fileHash(fileid);
		// Clear the running flag on EVERY local object that shares this hash. The lookup
		// may have been triggered from a search result while the same file is also
		// downloading or shared (two objects, one hash), and the flag was set on whichever
		// one the user used -- so two independent `if`s, not an `else if`.
		CKnownFile *knownFile = theApp->sharedfiles->GetFileByID(fileHash);
		if (!knownFile) {
			knownFile = theApp->downloadqueue->GetFileByID(fileHash);
		}
		if (knownFile) {
			knownFile->SetKadCommentSearchRunning(false);
			// Bump the file's EC generation so the next incremental update re-
			// serializes it: this is how amulegui and amuleapi learn the notes lookup
			// finished (they poll GET_UPDATE, which otherwise skips an unchanged
			// partfile).
			knownFile->MarkECChanged();
		}
		// Clear the flag on EVERY search result sharing this hash: the same file can be
		// shown in several open searches at once, and all were marked running. Search files
		// carry no EC change-generation, so the cleared flag rides the next periodic
		// search-results poll.
		std::vector<CSearchFile *> searchFiles;
		theApp->searchlist->GetAllSearchFilesByID(fileHash, searchFiles);
		for (CSearchFile *searchFile : searchFiles) {
			searchFile->SetKadCommentSearchRunning(false);
		}
	}

	for (ContactMap::iterator it = m_inUse.begin(); it != m_inUse.end(); ++it) {
		it->second->DecUse();
	}

	for (ContactList::const_iterator it = m_delete.begin(); it != m_delete.end(); ++it) {
		if (!(*it)->InUse()) {
			delete *it;
		}
	}

	// Check if this search was containing an overload node and adjust time of next time we use that node.
	if (CKademlia::IsRunning() && GetNodeLoad() > 20) {
		switch (GetSearchTypes()) {
		case CSearch::STOREKEYWORD:
			Kademlia::CKademlia::GetIndexed()->AddLoad(GetTarget(),
				((uint32_t)(DAY2S(7) * ((double)GetNodeLoad() / 100.0)) +
					(uint32_t)time(NULL)));
			break;
		}
	}

	delete[] m_searchTermsData;

	switch (m_type) {
	case KEYWORD:
		Notify_KadSearchEnd(m_searchID);
		break;
	}
}

void CSearch::Go()
{
	// Start with a lot of possible contacts, this is a fallback in case search stalls due to dead
	// contacts
	if (m_possible.empty()) {
		CUInt128 distance(CKademlia::GetPrefs()->GetKadID() ^ m_target);
		CKademlia::GetRoutingZone()->GetClosestTo(3, m_target, distance, 50, &m_possible, true, true);
	}

	if (!m_possible.empty()) {
		for (ContactMap::iterator it = m_possible.begin(); it != m_possible.end(); ++it) {
			m_inUse[it->first] = it->second;
		}

		wxASSERT(m_possible.size() == m_inUse.size());

		int count = m_type == NODE ? 1 : min(ALPHA_QUERY, (int)m_possible.size());

		ContactMap::iterator it = m_possible.begin();
		for (int i = 0; i < count; i++) {
			CContact *c = it->second;
			m_tried[it->first] = c;
			SendFindValue(c);
			++it;
		}
	}
}

// If we allow about a 15 sec delay before deleting, we won't miss a lot of delayed returning packets.
void CSearch::PrepareToStop() noexcept
{
	if (m_stopping) {
		return;
	}

	uint32_t baseTime = 0;
	switch (m_type) {
	case NODE:
	case NODECOMPLETE:
	case NODESPECIAL:
	case NODEFWCHECKUDP:
		baseTime = SEARCHNODE_LIFETIME;
		break;
	case FILE:
		baseTime = SEARCHFILE_LIFETIME;
		break;
	case KEYWORD:
		baseTime = SEARCHKEYWORD_LIFETIME;
		break;
	case NOTES:
		baseTime = SEARCHNOTES_LIFETIME;
		break;
	case STOREFILE:
		baseTime = SEARCHSTOREFILE_LIFETIME;
		break;
	case STOREKEYWORD:
		baseTime = SEARCHSTOREKEYWORD_LIFETIME;
		break;
	case STORENOTES:
		baseTime = SEARCHSTORENOTES_LIFETIME;
		break;
	case FINDBUDDY:
		baseTime = SEARCHFINDBUDDY_LIFETIME;
		break;
	case FINDSOURCE:
		baseTime = SEARCHFINDSOURCE_LIFETIME;
		break;
	default:
		baseTime = SEARCH_LIFETIME;
	}

	// Adjust created time so that search will delete within 15 seconds.
	// This gives late results time to be processed.
	m_created = time(NULL) - baseTime + SEC(15);
	m_stopping = true;
}

void CSearch::JumpStart()
{
#ifdef ENABLE_KAD_NODE_PROTECTION
	// How long to wait on an outstanding request before treating the search as stalled. Derived
	// from the response times actually observed (CFastKad) rather than fixed at 3 seconds: on a
	// fast link the old constant wasted seconds on nodes that were never going to answer, and
	// on a congested one it abandoned nodes that answered just too late.
	//
	// Background store operations keep the fixed 3 seconds: nobody is waiting on a publish, and
	// a tight deadline would only add republish traffic.
	const uint32_t maxPending = (m_type == STOREFILE || m_type == STOREKEYWORD || m_type == STORENOTES)
					    ? SEC2MS(3)
					    : fastKad.GetEstMaxResponseTime();

	const uint64_t nowTick = ::GetTickCount64();

	// Stop waiting on requests that have passed the ceiling. The record is marked rather than
	// erased and kept for PENDING_SAMPLE_GRACE_MS, because an answer arriving after the
	// ceiling is exactly the sample the estimator needs: erasing here meant no round-trip
	// longer than the current estimate could ever be recorded, so on a link slower than the
	// starting value the estimate could only ratchet down and every request stalled.
	//
	// A timeout deliberately does not reach safeKad. It is evidence of a slow or absent node,
	// not of a misbehaving one, and TrackProblematicNode() is rung one of the ban ladder:
	// nothing reads problematic state for scheduling, so its only effect was that the next
	// rejected identity change went straight to a four-hour ban.
	for (PendingRequestMap::iterator it = m_pendingRequests.begin(); it != m_pendingRequests.end();) {
		const uint64_t waited = nowTick - it->second.m_sentTick;
		if (waited >= maxPending + PENDING_SAMPLE_GRACE_MS) {
			m_pendingRequests.erase(it++);
			continue;
		}
		if (waited >= maxPending) {
			it->second.m_timedOut = true;
		}
		++it;
	}

	// If we had a response within the derived ceiling, no need to jumpstart.
	if (m_lastResponseTick + maxPending > nowTick) {
		return;
	}
#else
	// Gate off: the fixed 3-second ceiling at its usual second granularity, so the moment a
	// jumpstart goes out is unchanged. m_lastResponse is cast to time_t before the addition,
	// which in uint32_t would wrap near the 2106 boundary and silently reorder the comparison.
	if ((time_t)m_lastResponse + SEC(3) > time(NULL)) {
		return;
	}
#endif

	if (m_possible.empty()) {
		PrepareToStop();
		return;
	}

	// Is this a find lookup, and are the best two (KADEMLIA_FIND_VALUE) nodes dead or
	// unreachable? Then discover more close nodes before using the other results: limiting
	// results to 2 contacts may have hidden the closest live node behind duplicates of the dead
	// ones.
	bool lookupCloserNodes = false;
	if (m_requestedMoreNodes.empty() && GetRequestContactCount() == KADEMLIA_FIND_VALUE &&
		m_tried.size() >= 3 * KADEMLIA_FIND_VALUE) {
		ContactMap::const_iterator it = m_tried.begin();
		lookupCloserNodes = true;
		for (unsigned i = 0; i < KADEMLIA_FIND_VALUE; i++) {
			if (m_responded.count(it->first) > 0) {
				lookupCloserNodes = false;
				break;
			}
			++it;
		}
		if (lookupCloserNodes) {
			while (it != m_tried.end()) {
				if (m_responded.count(it->first) > 0) {
					AddDebugLogLineN(logKadSearch,
						CFormat("Best %d nodes for lookup (id=%x) were unreachable "
							"or dead, reasking closest for more") %
							KADEMLIA_FIND_VALUE % GetSearchID());
					SendFindValue(it->second, true);
					return;
				}
				++it;
			}
		}
	}

	while (!m_possible.empty()) {
		CContact *c = m_possible.begin()->second;

		if (m_tried.count(m_possible.begin()->first) > 0) {
			if (m_responded.count(m_possible.begin()->first) > 0) {
				StorePacket();
			}
			m_possible.erase(m_possible.begin());
		} else {
			m_tried[m_possible.begin()->first] = c;
			SendFindValue(c);
			break;
		}
	}
}

void CSearch::ProcessResponse(uint32_t fromIP, uint16_t fromPort, ContactList *results)
{
	AddDebugLogLineN(
		logKadSearch, "Processing search response from " + KadIPPortToString(fromIP, fromPort));

	ContactList::iterator response;
	for (response = results->begin(); response != results->end(); ++response) {
		m_delete.push_back(*response);
	}

	m_lastResponse = time(NULL);
#ifdef ENABLE_KAD_NODE_PROTECTION
	m_lastResponseTick = ::GetTickCount64();
#endif

	CUInt128 fromDistance(0u);
	CContact *fromContact = NULL;
	for (ContactMap::const_iterator it = m_tried.begin(); it != m_tried.end(); ++it) {
		CContact *tmpContact = it->second;
		if ((tmpContact->GetIPAddress() == fromIP) && (tmpContact->GetUDPPort() == fromPort)) {
			fromDistance = it->first;
			fromContact = tmpContact;
			break;
		}
	}

#ifdef ENABLE_KAD_NODE_PROTECTION
	if (fromContact != nullptr) {
		// The answer closes out its pending record: leaving satisfied entries in the map
		// would only make the timeout sweep in JumpStart walk dead weight.
		PendingRequestMap::iterator pending = m_pendingRequests.find(fromContact->GetClientID());
		if (pending != m_pendingRequests.end()) {
			// A useful answer: feed its round-trip time to the shared estimator so the
			// next timeout reflects the network we are actually on.
			const uint64_t nowTick = ::GetTickCount64();
			fastKad.AddResponseTime(
				fromIP, (uint32_t)(nowTick - pending->second.m_sentTick), nowTick);
			m_pendingRequests.erase(pending);
		}

		// The contact may have gone bad since we sent the request. onlyOneNodePerIP is off
		// here because the contact is already in our routing table, so a second port on the
		// address is that table's problem.
		//
		// idVerified is false, and deliberately not the contact's own flag. An answer to a
		// search proves the address is live; it proves nothing about the identity, which we
		// took from wherever we learned the contact. CContact::IsIPVerified() is no better
		// here: Process2BootstrapResponse() sets it by assumption for every contact in the
		// answer when the routing table is empty, so trusting it would let one bootstrap
		// peer hand us twenty addresses that can be escalated to a ban.
		//
		// Nothing is lost by passing false. Per TrackNode(), idVerified gates the
		// escalation and not the refusal, so a bad node is still refused here; and the
		// three-way handshake in CRoutingZone already records the address as verified,
		// where the proof is real. That is the only caller that should ever pass true.
		if (safeKad.IsBadNode(fromIP,
			    fromPort,
			    fromContact->GetClientID(),
			    fromContact->GetVersion(),
			    false,
			    false,
			    time(nullptr))) {
			AddDebugLogLineN(logKadSearch,
				"Ignoring search response from a node judged bad by the Kad identity "
				"protections: " +
					KadIPPortToString(fromIP, fromPort));
			return;
		}
	}
#endif

	// Make sure the node is not sending more results than we requested, which is not only a protocol
	// violation but most likely a malicious answer.  When fromContact is in m_requestedMoreNodes we asked
	// it for KADEMLIA_FIND_VALUE_MORE contacts (the wider variant), so a larger response is legitimate.
	const bool wasReaskedForMore =
		fromContact && m_requestedMoreNodes.count(fromContact->GetClientID()) > 0;
	if (results->size() > GetRequestContactCount() &&
		!(wasReaskedForMore && results->size() <= KADEMLIA_FIND_VALUE_MORE)) {
		AddDebugLogLineN(logKadSearch,
			"Node " + KadIPToString(fromIP) +
				" sent more contacts than requested on a routing query, ignoring response");
		return;
	}

	if (m_type == NODEFWCHECKUDP) {
		m_answers++;
		return;
	}

	// Not interested in responses for FIND_NODE, will be added to contacts by udp listener
	if (m_type == NODE) {
		AddDebugLogLineN(logKadSearch, "Node type search result, discarding.");
		m_answers++;
		m_possible.clear();
		return;
	}

	if (fromContact != NULL) {
		bool providedCloserContacts = false;
		std::map<uint32_t, unsigned> receivedIPs;
		std::map<uint32_t, unsigned> receivedSubnets;
		// A node is not allowed to answer with contacts to itself
		receivedIPs[fromIP] = 1;
		receivedSubnets[fromIP & 0xFFFFFF00] = 1;
		for (ContactList::iterator it = results->begin(); it != results->end(); ++it) {
			CContact *c = *it;
			CUInt128 distance(c->GetClientID() ^ m_target);

			if (distance < fromDistance) {
				providedCloserContacts = true;
			}

			if (m_possible.count(distance) > 0) {
				AddDebugLogLineN(
					logKadSearch, "Search result from already known client: ignore");
				continue;
			}
			if (m_tried.count(distance) > 0) {
				AddDebugLogLineN(
					logKadSearch, "Search result from already tried client: ignore");
				continue;
			}

			// We only accept unique IPs in the answer, having multiple IDs pointing to one IP in
			// the routing tables is no longer allowed since eMule0.49a, aMule-2.2.1 anyway
			if (receivedIPs.count(c->GetIPAddress()) > 0) {
				AddDebugLogLineN(logKadSearch,
					"Multiple KadIDs pointing to same IP (" +
						KadIPToString(c->GetIPAddress()) +
						") in Kad2Res answer - ignored, sent by " +
						KadIPToString(fromContact->GetIPAddress()));
				continue;
			} else {
				receivedIPs[c->GetIPAddress()] = 1;
			}
			// and no more than 2 IPs from the same /24 subnet
			if (receivedSubnets.count(c->GetIPAddress() & 0xFFFFFF00) > 0 &&
				!::IsLanIP(wxUINT32_SWAP_ALWAYS(c->GetIPAddress()))) {
				wxASSERT(receivedSubnets.find(c->GetIPAddress() & 0xFFFFFF00) !=
					 receivedSubnets.end());
				int subnetCount =
					receivedSubnets.find(c->GetIPAddress() & 0xFFFFFF00)->second;
				if (subnetCount >= 2) {
					AddDebugLogLineN(logKadSearch,
						"More than 2 KadIDs pointing to same subnet (" +
							KadIPToString(c->GetIPAddress() & 0xFFFFFF00) +
							"/24) in Kad2Res answer - ignored, sent by " +
							KadIPToString(fromContact->GetIPAddress()));
					continue;
				} else {
					receivedSubnets[c->GetIPAddress() & 0xFFFFFF00] = subnetCount + 1;
				}
			} else {
				receivedSubnets[c->GetIPAddress() & 0xFFFFFF00] = 1;
			}

			m_possible[distance] = c;

			if (distance < fromDistance) {
				bool top = false;
				if (m_best.size() < ALPHA_QUERY) {
					top = true;
					m_best[distance] = c;
				} else {
					ContactMap::iterator worst = m_best.end();
					--worst;
					if (distance < worst->first) {
						m_best.erase(worst);
						m_best[distance] = c;
						top = true;
					}
				}

				if (top) {
					m_tried[distance] = c;
					SendFindValue(c);
				}
			}
		}

		m_responded[fromDistance] = providedCloserContacts;

		if (m_type == NODECOMPLETE || m_type == NODESPECIAL) {
			AddDebugLogLineN(logKadSearch,
				wxString("Search result type: Node") +
					(m_type == NODECOMPLETE ? "Complete" : "Special"));
			m_answers++;
		}
	}
}

void CSearch::StorePacket()
{
	wxASSERT(!m_possible.empty());

	ContactMap::const_iterator possible = m_possible.begin();
	CUInt128 fromDistance(possible->first);
	CContact *from = possible->second;

	if (fromDistance < m_closestDistantFound || m_closestDistantFound == 0) {
		m_closestDistantFound = fromDistance;
	}

	if (fromDistance.Get32BitChunk(0) > SEARCHTOLERANCE &&
		!::IsLanIP(wxUINT32_SWAP_ALWAYS(from->GetIPAddress()))) {
		return;
	}

	switch (m_type) {
	case FILE: {
		AddDebugLogLineN(logKadSearch, "Search request type: File");
		CMemFile searchTerms;
		searchTerms.WriteUInt128(m_target);
		if (from->GetVersion() >= 3) {
			uint8_t fileid[16];
			m_target.ToByteArray(fileid);
			CKnownFile *file = theApp->downloadqueue->GetFileByID(CMD4Hash(fileid));
			if (file) {
				// Start position range (0x0 to 0x7FFF)
				searchTerms.WriteUInt16(0);
				searchTerms.WriteUInt64(file->GetFileSize());
				DebugSend(Kad2SearchSourceReq, from->GetIPAddress(), from->GetUDPPort());
				if (from->GetVersion() >= 6) {
					CUInt128 clientID = from->GetClientID();
					CKademlia::GetUDPListener()->SendPacket(searchTerms,
						KADEMLIA2_SEARCH_SOURCE_REQ,
						from->GetIPAddress(),
						from->GetUDPPort(),
						from->GetUDPKey(),
						&clientID);
				} else {
					CKademlia::GetUDPListener()->SendPacket(searchTerms,
						KADEMLIA2_SEARCH_SOURCE_REQ,
						from->GetIPAddress(),
						from->GetUDPPort(),
						0,
						NULL);
					wxASSERT(from->GetUDPKey() == CKadUDPKey(0));
				}
			} else {
				PrepareToStop();
				break;
			}
		} else {
			searchTerms.WriteUInt8(1);
			DebugSendF("KadSearchReq(File)", from->GetIPAddress(), from->GetUDPPort());
			CKademlia::GetUDPListener()->SendPacket(searchTerms,
				KADEMLIA_SEARCH_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				0,
				NULL);
		}
		m_totalRequestAnswers++;
		break;
	}
	case KEYWORD: {
		AddDebugLogLineN(logKadSearch, "Search request type: Keyword");
		CMemFile searchTerms;
		searchTerms.WriteUInt128(m_target);
		if (from->GetVersion() >= 3) {
			if (m_searchTermsDataSize == 0) {
				// Start position range (0x0 to 0x7FFF)
				searchTerms.WriteUInt16(0);
			} else {
				// Start position range (0x8000 to 0xFFFF)
				searchTerms.WriteUInt16(0x8000);
				searchTerms.Write(m_searchTermsData, m_searchTermsDataSize);
			}
			DebugSend(Kad2SearchKeyReq, from->GetIPAddress(), from->GetUDPPort());
		} else {
			if (m_searchTermsDataSize == 0) {
				searchTerms.WriteUInt8(0);
				// We send this extra byte to flag we handle large files.
				searchTerms.WriteUInt8(0);
			} else {
				// Set to 2 to flag we handle large files.
				searchTerms.WriteUInt8(2);
				searchTerms.Write(m_searchTermsData, m_searchTermsDataSize);
			}
			DebugSendF("KadSearchReq(Keyword)", from->GetIPAddress(), from->GetUDPPort());
		}
		if (from->GetVersion() >= 6) {
			CUInt128 clientID = from->GetClientID();
			CKademlia::GetUDPListener()->SendPacket(searchTerms,
				KADEMLIA2_SEARCH_KEY_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				from->GetUDPKey(),
				&clientID);
		} else if (from->GetVersion() >= 3) {
			CKademlia::GetUDPListener()->SendPacket(searchTerms,
				KADEMLIA2_SEARCH_KEY_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				0,
				NULL);
			wxASSERT(from->GetUDPKey() == CKadUDPKey(0));
		} else {
			CKademlia::GetUDPListener()->SendPacket(searchTerms,
				KADEMLIA_SEARCH_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				0,
				NULL);
		}
		m_totalRequestAnswers++;
		break;
	}
	case NOTES: {
		AddDebugLogLineN(logKadSearch, "Search request type: Notes");
		CMemFile searchTerms;
		searchTerms.WriteUInt128(m_target);
		if (from->GetVersion() >= 3) {
			// Find the file we are storing info about. The NOTES request carries the
			// file size, which we read from whichever local list holds the hash: shared
			// files, the download queue, or the search list for an on-demand lookup on
			// a result the user has not downloaded.
			uint8_t fileid[16];
			m_target.ToByteArray(fileid);
			const CMD4Hash fileHash(fileid);
			CAbstractFile *file = theApp->sharedfiles->GetFileByID(fileHash);
			if (!file) {
				file = theApp->downloadqueue->GetFileByID(fileHash);
			}
			if (!file) {
				file = theApp->searchlist->GetSearchFileByID(fileHash);
			}
			if (file) {
				// Start position range (0x0 to 0x7FFF)
				searchTerms.WriteUInt64(file->GetFileSize());
				DebugSend(Kad2SearchNotesReq, from->GetIPAddress(), from->GetUDPPort());
				if (from->GetVersion() >= 6) {
					CUInt128 clientID = from->GetClientID();
					CKademlia::GetUDPListener()->SendPacket(searchTerms,
						KADEMLIA2_SEARCH_NOTES_REQ,
						from->GetIPAddress(),
						from->GetUDPPort(),
						from->GetUDPKey(),
						&clientID);
				} else {
					CKademlia::GetUDPListener()->SendPacket(searchTerms,
						KADEMLIA2_SEARCH_NOTES_REQ,
						from->GetIPAddress(),
						from->GetUDPPort(),
						0,
						NULL);
					wxASSERT(from->GetUDPKey() == CKadUDPKey(0));
				}
			} else {
				PrepareToStop();
				break;
			}
		} else {
			searchTerms.WriteUInt128(CKademlia::GetPrefs()->GetKadID());
			DebugSend(KadSearchNotesReq, from->GetIPAddress(), from->GetUDPPort());
			CKademlia::GetUDPListener()->SendPacket(searchTerms,
				KADEMLIA_SEARCH_NOTES_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				0,
				NULL);
		}
		m_totalRequestAnswers++;
		break;
	}
	case STOREFILE: {
		AddDebugLogLineN(logKadSearch, "Search request type: StoreFile");
		// Try to store ourselves as a source to a Node.
		// As a safeguard, check to see if we already stored to the max nodes.
		if (m_answers > SEARCHSTOREFILE_TOTAL) {
			PrepareToStop();
			break;
		}

		uint8_t fileid[16];
		m_target.ToByteArray(fileid);
		CKnownFile *file = theApp->sharedfiles->GetFileByID(CMD4Hash(fileid));
		if (file) {
			// We store this mostly for GUI reasons.
			m_fileName = file->GetFileName().GetPrintable();

			CUInt128 id(CKademlia::GetPrefs()->GetClientHash());
			TagPtrList taglist;

			// Source types: 1 HighID, 3 firewalled Kad, 4 >4GB HighID, 5 >4GB
			// firewalled Kad, 6 firewalled with direct callback (supports >4GB).
			// 2 cannot be used, as older clients will not work with it.

			bool directCallback = false;
			if (theApp->IsFirewalled()) {
				directCallback = (Kademlia::CKademlia::IsRunning() &&
						  !Kademlia::CUDPFirewallTester::IsFirewalledUDP(true) &&
						  Kademlia::CUDPFirewallTester::IsVerified());
				if (directCallback) {
					// firewalled, but direct udp callback is possible so no need for
					// buddies We are not firewalled..
					taglist.push_back(new CTagVarInt(TAG_SOURCETYPE, 6));
					taglist.push_back(
						new CTagVarInt(TAG_SOURCEPORT, thePrefs::GetPort()));
					if (!CKademlia::GetPrefs()->GetUseExternKadPort()) {
						taglist.push_back(new CTagInt16(TAG_SOURCEUPORT,
							CKademlia::GetPrefs()->GetInternKadPort()));
					}
					if (from->GetVersion() >= 2) {
						taglist.push_back(
							new CTagVarInt(TAG_FILESIZE, file->GetFileSize()));
					}
				} else if (theApp->clientlist->GetBuddy()) { // We are firewalled, make sure
									     // we have a buddy.
					// We send the ID to our buddy so they can do a callback.
					CUInt128 buddyID(true);
					buddyID ^= CKademlia::GetPrefs()->GetKadID();
					taglist.push_back(
						new CTagInt8(TAG_SOURCETYPE, file->IsLargeFile() ? 5 : 3));
					taglist.push_back(new CTagVarInt(
						TAG_SERVERIP, theApp->clientlist->GetBuddy()->GetIP()));
					taglist.push_back(new CTagVarInt(TAG_SERVERPORT,
						theApp->clientlist->GetBuddy()->GetUDPPort()));
					uint8_t hashBytes[16];
					buddyID.ToByteArray(hashBytes);
					taglist.push_back(
						new CTagString(TAG_BUDDYHASH, CMD4Hash(hashBytes).Encode()));
					taglist.push_back(
						new CTagVarInt(TAG_SOURCEPORT, thePrefs::GetPort()));
					if (!CKademlia::GetPrefs()->GetUseExternKadPort()) {
						taglist.push_back(new CTagInt16(TAG_SOURCEUPORT,
							CKademlia::GetPrefs()->GetInternKadPort()));
					}
					if (from->GetVersion() >= 2) {
						taglist.push_back(
							new CTagVarInt(TAG_FILESIZE, file->GetFileSize()));
					}
				} else {
					// We are firewalled, but lost our buddy.. Stop everything.
					PrepareToStop();
					break;
				}
			} else {
				// We're not firewalled..
				taglist.push_back(new CTagInt8(TAG_SOURCETYPE, file->IsLargeFile() ? 4 : 1));
				taglist.push_back(new CTagVarInt(TAG_SOURCEPORT, thePrefs::GetPort()));
				if (!CKademlia::GetPrefs()->GetUseExternKadPort()) {
					taglist.push_back(new CTagInt16(
						TAG_SOURCEUPORT, CKademlia::GetPrefs()->GetInternKadPort()));
				}
				if (from->GetVersion() >= 2) {
					taglist.push_back(new CTagVarInt(TAG_FILESIZE, file->GetFileSize()));
				}
			}

			taglist.push_back(
				new CTagInt8(TAG_ENCRYPTION, CPrefs::GetMyConnectOptions(true, true)));

			CKademlia::GetUDPListener()->SendPublishSourcePacket(*from, m_target, id, taglist);
			m_totalRequestAnswers++;
			deleteTagPtrListEntries(&taglist);
		} else {
			PrepareToStop();
		}
		break;
	}
	case STOREKEYWORD: {
		AddDebugLogLineN(logKadSearch, "Search request type: StoreKeyword");
		// Try to store keywords to a Node.
		// As a safeguard, check to see if we already stored to the max nodes.
		if (m_answers > SEARCHSTOREKEYWORD_TOTAL) {
			PrepareToStop();
			break;
		}

		uint16_t count = m_fileIDs.size();
		if (count == 0) {
			PrepareToStop();
			break;
		} else if (count > 150) {
			count = 150;
		}

		UIntList::const_iterator itListFileID = m_fileIDs.begin();
		uint8_t fileid[16];

		while (count && (itListFileID != m_fileIDs.end())) {
			uint16_t packetCount = 0;
			CMemFile packetdata(1024 * 50); // Allocate a good amount of space.
			packetdata.WriteUInt128(m_target);
			packetdata.WriteUInt16(0); // Will be updated before sending.
			while ((packetCount < 50) && (itListFileID != m_fileIDs.end())) {
				CUInt128 id(*itListFileID);
				id.ToByteArray(fileid);
				CKnownFile *pFile = theApp->sharedfiles->GetFileByID(CMD4Hash(fileid));
				if (pFile) {
					count--;
					packetCount++;
					packetdata.WriteUInt128(id);
					PreparePacketForTags(&packetdata, pFile, from->GetVersion());
				}
				++itListFileID;
			}

			uint64_t current_pos = packetdata.GetPosition();
			packetdata.Seek(16);
			packetdata.WriteUInt16(packetCount);
			packetdata.Seek(current_pos);

			if (from->GetVersion() >= 6) {
				DebugSend(Kad2PublishKeyReq, from->GetIPAddress(), from->GetUDPPort());
				CUInt128 clientID = from->GetClientID();
				CKademlia::GetUDPListener()->SendPacket(packetdata,
					KADEMLIA2_PUBLISH_KEY_REQ,
					from->GetIPAddress(),
					from->GetUDPPort(),
					from->GetUDPKey(),
					&clientID);
			} else if (from->GetVersion() >= 2) {
				DebugSend(Kad2PublishKeyReq, from->GetIPAddress(), from->GetUDPPort());
				CKademlia::GetUDPListener()->SendPacket(packetdata,
					KADEMLIA2_PUBLISH_KEY_REQ,
					from->GetIPAddress(),
					from->GetUDPPort(),
					0,
					NULL);
				wxASSERT(from->GetUDPKey() == CKadUDPKey(0));
			} else {
				wxFAIL;
			}
		}
		m_totalRequestAnswers++;
		break;
	}
	case STORENOTES: {
		AddDebugLogLineN(logKadSearch, "Search request type: StoreNotes");
		uint8_t fileid[16];
		m_target.ToByteArray(fileid);
		CKnownFile *file = theApp->sharedfiles->GetFileByID(CMD4Hash(fileid));

		if (file) {
			CMemFile packetdata(1024 * 2);
			packetdata.WriteUInt128(m_target);
			packetdata.WriteUInt128(CKademlia::GetPrefs()->GetKadID());

			TagPtrList taglist;
			taglist.push_back(new CTagString(TAG_FILENAME, file->GetFileName().GetPrintable()));
			if (file->GetFileRating() != 0) {
				taglist.push_back(new CTagVarInt(TAG_FILERATING, file->GetFileRating()));
			}
			if (!file->GetFileComment().IsEmpty()) {
				taglist.push_back(new CTagString(TAG_DESCRIPTION, file->GetFileComment()));
			}
			if (from->GetVersion() >= 2) {
				taglist.push_back(new CTagVarInt(TAG_FILESIZE, file->GetFileSize()));
			}
			packetdata.WriteTagPtrList(taglist);

			if (from->GetVersion() >= 6) {
				DebugSend(Kad2PublishNotesReq, from->GetIPAddress(), from->GetUDPPort());
				CUInt128 clientID = from->GetClientID();
				CKademlia::GetUDPListener()->SendPacket(packetdata,
					KADEMLIA2_PUBLISH_NOTES_REQ,
					from->GetIPAddress(),
					from->GetUDPPort(),
					from->GetUDPKey(),
					&clientID);
			} else if (from->GetVersion() >= 2) {
				DebugSend(Kad2PublishNotesReq, from->GetIPAddress(), from->GetUDPPort());
				CKademlia::GetUDPListener()->SendPacket(packetdata,
					KADEMLIA2_PUBLISH_NOTES_REQ,
					from->GetIPAddress(),
					from->GetUDPPort(),
					0,
					NULL);
				wxASSERT(from->GetUDPKey() == CKadUDPKey(0));
			} else {
				wxFAIL;
			}
			m_totalRequestAnswers++;
			deleteTagPtrListEntries(&taglist);
		} else {
			PrepareToStop();
		}
		break;
	}
	case FINDBUDDY: {
		AddDebugLogLineN(logKadSearch, "Search request type: FindBuddy");
		// Send a buddy request as we are firewalled.
		// As a safeguard, check to see if we already requested the max nodes.
		if (m_answers > SEARCHFINDBUDDY_TOTAL) {
			PrepareToStop();
			break;
		}

		CMemFile packetdata;
		// Send the ID we used to find our buddy. Used for checks later and allows users to callback
		// someone if they change buddies.
		packetdata.WriteUInt128(m_target);
		packetdata.WriteUInt128(CKademlia::GetPrefs()->GetClientHash());
		packetdata.WriteUInt16(thePrefs::GetPort());

		DebugSend(KadFindBuddyReq, from->GetIPAddress(), from->GetUDPPort());
		if (from->GetVersion() >= 6) {
			CUInt128 clientID = from->GetClientID();
			CKademlia::GetUDPListener()->SendPacket(packetdata,
				KADEMLIA_FINDBUDDY_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				from->GetUDPKey(),
				&clientID);
		} else {
			CKademlia::GetUDPListener()->SendPacket(packetdata,
				KADEMLIA_FINDBUDDY_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				0,
				NULL);
			wxASSERT(from->GetUDPKey() == CKadUDPKey(0));
		}
		m_answers++;
		break;
	}
	case FINDSOURCE: {
		AddDebugLogLineN(logKadSearch, "Search request type: FindSource");
		// Try to find if this is a buddy to someone we want to contact.
		// As a safeguard, check to see if we already requested the max nodes.
		if (m_answers > SEARCHFINDSOURCE_TOTAL) {
			PrepareToStop();
			break;
		}

		CMemFile packetdata(34);
		packetdata.WriteUInt128(m_target);
		if (m_fileIDs.size() != 1) {
			throw wxString("Kademlia.CSearch.processResponse: m_fileIDs.size() != 1");
		}
		// Currently, we limit the type of callbacks for sources. We must know a file this person has
		// for it to work.
		packetdata.WriteUInt128(m_fileIDs.front());
		packetdata.WriteUInt16(thePrefs::GetPort());
		DebugSend(KadCallbackReq, from->GetIPAddress(), from->GetUDPPort());
		if (from->GetVersion() >= 6) {
			CUInt128 clientID = from->GetClientID();
			CKademlia::GetUDPListener()->SendPacket(packetdata,
				KADEMLIA_CALLBACK_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				from->GetUDPKey(),
				&clientID);
		} else {
			CKademlia::GetUDPListener()->SendPacket(packetdata,
				KADEMLIA_CALLBACK_REQ,
				from->GetIPAddress(),
				from->GetUDPPort(),
				0,
				NULL);
			wxASSERT(from->GetUDPKey() == CKadUDPKey(0));
		}
		m_answers++;
		break;
	}
	case NODESPECIAL: {
		// we are looking for the IP of a given NodeID, so we just check if we 0 distance and if so,
		// report the tip to the requester
		if (fromDistance == 0) {
			m_nodeSpecialSearchRequester->KadSearchIPByNodeIDResult(KCSR_SUCCEEDED,
				wxUINT32_SWAP_ALWAYS(from->GetIPAddress()),
				from->GetTCPPort());
			m_nodeSpecialSearchRequester = NULL;
			PrepareToStop();
		}
		break;
	}
	case NODECOMPLETE:
		AddDebugLogLineN(logKadSearch, "Search request type: NodeComplete");
		break;
	case NODE:
		AddDebugLogLineN(logKadSearch, "Search request type: Node");
		break;
	default:
		AddDebugLogLineN(logKadSearch, CFormat("Search result type: Unknown (%i)") % m_type);
		break;
	}
}

void CSearch::ProcessResult(const CUInt128 &answer, TagPtrList *info, uint32_t fromIP, uint16_t fromPort)
{
	wxString type = "Unknown";
	switch (m_type) {
	case FILE:
		type = "File";
		ProcessResultFile(answer, info);
		break;
	case KEYWORD:
		type = "Keyword";
		ProcessResultKeyword(answer, info, fromIP, fromPort);
		break;
	case NOTES:
		type = "Notes";
		ProcessResultNotes(answer, info);
		break;
	}
	AddDebugLogLineN(logKadSearch, "Got result (" + type + ")");
}

void CSearch::ProcessResultFile(const CUInt128 &answer, TagPtrList *info)
{
	// Process a possible source to a file.
	// Set of data we could receive from the result.
	uint8_t type = 0;
	uint32_t ip = 0;
	uint16_t tcp = 0;
	uint16_t udp = 0;
	uint32_t buddyip = 0;
	uint16_t buddyport = 0;
	uint8_t byCryptOptions = 0; // 0 = not supported.
	CUInt128 buddy;

	for (TagPtrList::const_iterator it = info->begin(); it != info->end(); ++it) {
		CTag *tag = *it;
		if (!tag->GetName().Cmp(TAG_SOURCETYPE)) {
			type = tag->GetInt();
		} else if (!tag->GetName().Cmp(TAG_SOURCEIP)) {
			ip = tag->GetInt();
		} else if (!tag->GetName().Cmp(TAG_SOURCEPORT)) {
			tcp = tag->GetInt();
		} else if (!tag->GetName().Cmp(TAG_SOURCEUPORT)) {
			udp = tag->GetInt();
		} else if (!tag->GetName().Cmp((TAG_SERVERIP))) {
			buddyip = tag->GetInt();
		} else if (!tag->GetName().Cmp(TAG_SERVERPORT)) {
			buddyport = tag->GetInt();
		} else if (!tag->GetName().Cmp(TAG_BUDDYHASH)) {
			CMD4Hash hash;
			// TODO: Error handling
			if (!hash.Decode(tag->GetStr())) {
#ifdef __DEBUG__
				printf("Invalid buddy-hash: '%s'\n", (const char *)tag->GetStr().fn_str());
#endif
			}
			buddy.SetValueBE(hash.GetHash());
		} else if (!tag->GetName().Cmp(TAG_ENCRYPTION)) {
			byCryptOptions = (uint8)tag->GetInt();
		} else if (!tag->GetName().Cmp(TAG_IPV6) || !tag->GetName().Cmp(TAG_SERVINGBUDDYIPV6)) {
			// eMuleAI publishes IPv6 sources alongside the IPv4 ones as 32 hex
			// characters. aMule has no IPv6 stack yet, so the address is validated and
			// dropped: a malformed tag is worth a log line, and a well-formed one must
			// not be mistaken for a reachable source. Routing to it is the dual-stack
			// change.
			uint8_t address[16];
			bool decoded = false;
			if (tag->IsStr()) {
				const wxScopedCharBuffer text = tag->GetStr().utf8_str();
				decoded = DecodeIPv6HexTag(text.data(), text.length(), address);
			}
			if (decoded) {
				AddDebugLogLineN(logKadSearch,
					CFormat("Ignoring IPv6 source tag '%s' in search result: no "
						"IPv6 transport in this build") %
						tag->GetName());
			} else {
				AddDebugLogLineN(logKadSearch,
					CFormat("Invalid IPv6 source tag '%s' in search result") %
						tag->GetName());
			}
		}
	}

	// Process source based on its type. Currently only one method is needed to process all types.
	switch (type) {
	case 1:
	case 3:
	case 4:
	case 5:
	case 6:
		AddDebugLogLineN(logKadSearch,
			CFormat("Trying to add a source type %i, ip %s") % type % KadIPPortToString(ip, udp));
		m_answers++;
		theApp->downloadqueue->KademliaSearchFile(
			m_searchID, &answer, &buddy, type, ip, tcp, udp, buddyip, buddyport, byCryptOptions);
		break;
	case 2:
		// Don't use this type, some clients will process it wrong.
	default:
		break;
	}
}

void CSearch::ProcessResultNotes(const CUInt128 &answer, TagPtrList *info)
{
	// Process a received Note to a file.
	// Create a Note and set the IDs.
	CEntry *entry = new CEntry();
	entry->m_uKeyID = m_target;
	entry->m_uSourceID = answer;

	bool bFilterComment = false;

	// Loop through tags and pull wanted into. Currently we only keep Filename, Rating, Comment.
	for (TagPtrList::iterator it = info->begin(); it != info->end(); ++it) {
		CTag *tag = *it;
		if (!tag->GetName().Cmp(TAG_SOURCEIP)) {
			entry->m_uIP = tag->GetInt();
		} else if (!tag->GetName().Cmp(TAG_SOURCEPORT)) {
			entry->m_uTCPport = tag->GetInt();
		} else if (!tag->GetName().Cmp(TAG_FILENAME)) {
			entry->SetFileName(tag->GetStr());
		} else if (!tag->GetName().Cmp(TAG_DESCRIPTION)) {
			wxString strComment(tag->GetStr());
			bFilterComment = thePrefs::IsMessageFiltered(strComment);
			entry->AddTag(tag, entry->m_uIP);
			*it = NULL; // Prevent actual data being freed
		} else if (!tag->GetName().Cmp(TAG_FILERATING)) {
			entry->AddTag(tag, entry->m_uIP);
			*it = NULL; // Prevent actual data being freed
		}
	}

	if (bFilterComment) {
		delete entry;
		return;
	}

	uint8_t fileid[16];
	m_target.ToByteArray(fileid);
	const CMD4Hash fileHash(fileid);

	// The same file can exist locally as more than one object sharing this hash: a
	// downloading/shared CKnownFile and one CSearchFile per open search that returned it. The
	// user may have triggered the lookup from any of them, and each keeps its own note list, so
	// deliver the note to EVERY match. AddNote takes ownership and dedups per list, so each
	// extra target gets an independent Copy() and the single original is consumed exactly once.
	CKnownFile *knownFile = theApp->sharedfiles->GetFileByID(fileHash);
	if (!knownFile) {
		knownFile = theApp->downloadqueue->GetFileByID(fileHash);
	}
	std::vector<CSearchFile *> searchFiles;
	theApp->searchlist->GetAllSearchFilesByID(fileHash, searchFiles);

	if (!knownFile && searchFiles.empty()) {
		AddDebugLogLineN(logKadSearch, "Comment received for unknown file");
		delete entry;
		return;
	}

	m_answers++;
	// Every search result sharing this hash (one per open search tab) gets its own copy; the
	// note rides the next search-results poll, search files carrying no EC change-generation.
	for (CSearchFile *searchFile : searchFiles) {
		searchFile->AddNote(entry->Copy());
	}
	if (knownFile) {
		knownFile->AddNote(entry);
		// Re-emit the partfile so amulegui / amuleapi see notes stream in live
		// (matching the monolithic dialog), instead of all at once when the
		// search ends. AddNote dedups, so a repeat is cheap.
		knownFile->MarkECChanged();
	} else {
		// No known file to take ownership of the original; the search results
		// all received copies above.
		delete entry;
	}
}

void CSearch::ProcessResultKeyword(
	const CUInt128 &answer, TagPtrList *info, uint32_t fromIP, uint16_t fromPort)
{
#ifdef ENABLE_KAD_PROTOCOL_10
	// Find the contact that answered, so version-gated result tags can be checked against the
	// version it advertised. A tag a peer cannot possibly have generated is a tag it is
	// relaying on someone else's behalf, and the whole point of the publisher-side filtering is
	// that we do not take those at face value.
	uint8_t fromKadVersion = 0;
	for (ContactMap::const_iterator it = m_tried.begin(); it != m_tried.end(); ++it) {
		const CContact *tmpContact = it->second;
		if ((tmpContact->GetIPAddress() == fromIP) && (tmpContact->GetUDPPort() == fromPort)) {
			fromKadVersion = tmpContact->GetVersion();
			break;
		}
	}
	if (fromKadVersion == 0) {
		AddDebugLogLineN(logKadSearch,
			"Unable to find the answering contact in ProcessResultKeyword - " +
				KadIPPortToString(fromIP, fromPort));
	}
#else
	(void)fromIP;
	(void)fromPort;
#endif

	// Process a keyword that we received.
	// Set of data we can use for a keyword result.
	wxString name;
	uint64_t size = 0;
	wxString type;
	wxString format;
	wxString artist;
	wxString album;
	wxString title;
	uint32_t length = 0;
	wxString codec;
	uint32_t bitrate = 0;
	uint32_t availability = 0;
	uint32_t publishInfo = 0;
#ifdef ENABLE_KAD_PROTOCOL_10
	std::vector<CKadAICHHashList::SResultHash> aichHashes;
#endif
	// Flag that is set if we want this keyword
	bool bFileName = false;
	bool bFileSize = false;

	for (TagPtrList::const_iterator it = info->begin(); it != info->end(); ++it) {
		CTag *tag = *it;
		if (tag->GetName() == TAG_FILENAME) {
			name = tag->GetStr();
			bFileName = !name.IsEmpty();
		} else if (tag->GetName() == TAG_FILESIZE) {
			if (tag->IsBsob() && (tag->GetBsobSize() == 8)) {
				// Kad1.0 uint64 type using a BSOB.
				size = PeekUInt64(tag->GetBsob());
			} else {
				wxASSERT(tag->IsInt());
				size = tag->GetInt();
			}
			bFileSize = true;
		} else if (tag->GetName() == TAG_FILETYPE) {
			type = tag->GetStr();
		} else if (tag->GetName() == TAG_FILEFORMAT) {
			format = tag->GetStr();
		} else if (tag->GetName() == TAG_MEDIA_ARTIST) {
			artist = tag->GetStr();
		} else if (tag->GetName() == TAG_MEDIA_ALBUM) {
			album = tag->GetStr();
		} else if (tag->GetName() == TAG_MEDIA_TITLE) {
			title = tag->GetStr();
		} else if (tag->GetName() == TAG_MEDIA_LENGTH) {
			length = tag->GetInt();
		} else if (tag->GetName() == TAG_MEDIA_BITRATE) {
			bitrate = tag->GetInt();
		} else if (tag->GetName() == TAG_MEDIA_CODEC) {
			codec = tag->GetStr();
		} else if (tag->GetName() == TAG_SOURCES) {
			availability = tag->GetInt();
			// Some rogue client was setting a invalid availability, just set it to 0.
			if (availability > 65500) {
				availability = 0;
			}
		} else if (tag->GetName() == TAG_PUBLISHINFO) {
			// we don't keep this as tag, but as a member property of the searchfile, as we only
			// need its information in the search list and don't want to carry the tag over when
			// downloading the file (and maybe even wrongly publishing it)
			publishInfo = (uint32_t)tag->GetInt();
#ifdef __DEBUG__
			uint32_t differentNames = (publishInfo & 0xFF000000) >> 24;
			uint32_t publishersKnown = (publishInfo & 0x00FF0000) >> 16;
			uint32_t trustValue = publishInfo & 0x0000FFFF;
			AddDebugLogLineN(logKadSearch,
				CFormat("Received PublishInfo Tag: %u different names, %u publishers, %.2f "
					"trustvalue") %
					differentNames % publishersKnown % ((double)trustValue / 100.0));
#endif
#ifdef ENABLE_KAD_PROTOCOL_10
		} else if (tag->GetName() == TAG_KADAICHHASHRESULT) {
			// AICH hashes on keyword storage arrived with Kad protocol version 0x09, so
			// a sender below that cannot have produced this tag itself and it is
			// filtered rather than trusted. Gated with the rest: with the switch off we
			// never publish an AICH hash, so acting on one a peer reports would be
			// behaviour upstream does not have.
			if (CKadAICHHashList::PeerSupportsAICHKeywordStorage(fromKadVersion) &&
				tag->IsBsob()) {
				if (!CKadAICHHashList::DecodeResultTag(
					    tag->GetBsob(), tag->GetBsobSize(), aichHashes)) {
					AddDebugLogLineN(logKadSearch,
						"ProcessResultKeyword: corrupt or invalid "
						"TAG_KADAICHHASHRESULT received from " +
							KadIPPortToString(fromIP, fromPort));
				}
			} else {
				AddDebugLogLineN(logKadSearch,
					CFormat("ProcessResultKeyword: received special publish tag "
						"(TAG_KADAICHHASHRESULT) from a node (version %u, %s) "
						"which is not aware of it, filtering") %
						fromKadVersion % KadIPPortToString(fromIP, fromPort));
			}
#endif
		}
	}

	// If we don't have a valid filename and filesize, drop this keyword.
	if (!bFileName || !bFileSize) {
		AddDebugLogLineN(logKadSearch,
			wxString("No ") + (!bFileName ? "filename" : "filesize") +
				" on search result, ignoring");
		return;
	}

	TagPtrList taglist;

	if (!format.IsEmpty()) {
		taglist.push_back(new CTagString(TAG_FILEFORMAT, format));
	}
	if (!artist.IsEmpty()) {
		taglist.push_back(new CTagString(TAG_MEDIA_ARTIST, artist));
	}
	if (!album.IsEmpty()) {
		taglist.push_back(new CTagString(TAG_MEDIA_ALBUM, album));
	}
	if (!title.IsEmpty()) {
		taglist.push_back(new CTagString(TAG_MEDIA_TITLE, title));
	}
	// The codec was read off the wire and then never mentioned again, so every Kad result
	// showed an empty Codec column and the value never reached a download started from it.
	// Being a wxString, the dead store was not something -Wunused-but-set-variable could flag.
	if (!codec.IsEmpty()) {
		taglist.push_back(new CTagString(TAG_MEDIA_CODEC, codec));
	}
	if (length) {
		taglist.push_back(new CTagVarInt(TAG_MEDIA_LENGTH, length));
	}
	if (bitrate) {
		taglist.push_back(new CTagVarInt(TAG_MEDIA_BITRATE, bitrate));
	}
	if (availability) {
		taglist.push_back(new CTagVarInt(TAG_SOURCES, availability));
	}
#ifdef ENABLE_KAD_PROTOCOL_10
	// Carry a trusted AICH root hash into the search result, under the same tag name
	// (FT_AICH_HASH) that an ed2k result and the part-file metadata use, so CPartFile takes it
	// as its master hash when a download starts. SelectTrusted() answers nullptr far more often
	// than not: see its declaration for why refusing is the right default here.
	const uint32_t publishersKnown = (publishInfo & 0x00FF0000) >> 16;
	const CKadAICHHashList::SResultHash *bestAICHHash =
		CKadAICHHashList::SelectTrusted(aichHashes, publishersKnown);
	if (bestAICHHash != nullptr) {
		CAICHHash hash;
		memcpy(hash.GetRawHash(), bestAICHHash->m_hash.data(), CAICHHash::GetHashSize());
		taglist.push_back(new CTagString(TAG_AICHHASH, hash.GetString()));
	}
#endif

	m_answers++;
	theApp->searchlist->KademliaSearchKeyword(
		m_searchID, &answer, name, size, type, publishInfo, taglist);

	deleteTagPtrListEntries(&taglist);
}

void CSearch::SendFindValue(CContact *contact, bool reaskMore)
{
	try {
		if (m_stopping) {
			return;
		}

		CMemFile packetdata(33);
		uint8_t contactCount = GetRequestContactCount();

		if (reaskMore) {
			// Either the JumpStart dead-nodes fallback or RequestMoreResults() asked us
			// to send the wider KADEMLIA_FIND_VALUE_MORE variant to this contact.
			// Tracking its ClientID makes ProcessResponse's "more results than
			// requested" check accept the larger response, and stops
			// RequestMoreResults() reasking the same peer twice.
			wxASSERT(contactCount == KADEMLIA_FIND_VALUE);
			m_requestedMoreNodes.insert(contact->GetClientID());
			contactCount = KADEMLIA_FIND_VALUE_MORE;
		}

		if (contactCount > 0) {
			packetdata.WriteUInt8(contactCount);
		} else {
			return;
		}

		packetdata.WriteUInt128(m_target);
		packetdata.WriteUInt128(contact->GetClientID());
		if (contact->GetVersion() >= 2) {
			if (contact->GetVersion() >= 6) {
				const CUInt128 &clientID = contact->GetClientID();
				CKademlia::GetUDPListener()->SendPacket(packetdata,
					KADEMLIA2_REQ,
					contact->GetIPAddress(),
					contact->GetUDPPort(),
					contact->GetUDPKey(),
					&clientID);
			} else {
				CKademlia::GetUDPListener()->SendPacket(packetdata,
					KADEMLIA2_REQ,
					contact->GetIPAddress(),
					contact->GetUDPPort(),
					0,
					NULL);
				wxASSERT(contact->GetUDPKey() == CKadUDPKey(0));
			}
#ifdef __DEBUG__
			switch (m_type) {
			case NODE:
				DebugSendF("Kad2Req(Node)", contact->GetIPAddress(), contact->GetUDPPort());
				break;
			case NODECOMPLETE:
				DebugSendF("Kad2Req(NodeComplete)",
					contact->GetIPAddress(),
					contact->GetUDPPort());
				break;
			case NODESPECIAL:
				DebugSendF("Kad2Req(NodeSpecial)",
					contact->GetIPAddress(),
					contact->GetUDPPort());
				break;
			case NODEFWCHECKUDP:
				DebugSendF("Kad2Req(NodeFWCheckUDP)",
					contact->GetIPAddress(),
					contact->GetUDPPort());
				break;
			case FILE:
				DebugSendF("Kad2Req(File)", contact->GetIPAddress(), contact->GetUDPPort());
				break;
			case KEYWORD:
				DebugSendF(
					"Kad2Req(Keyword)", contact->GetIPAddress(), contact->GetUDPPort());
				break;
			case STOREFILE:
				DebugSendF(
					"Kad2Req(StoreFile)", contact->GetIPAddress(), contact->GetUDPPort());
				break;
			case STOREKEYWORD:
				DebugSendF("Kad2Req(StoreKeyword)",
					contact->GetIPAddress(),
					contact->GetUDPPort());
				break;
			case STORENOTES:
				DebugSendF("Kad2Req(StoreNotes)",
					contact->GetIPAddress(),
					contact->GetUDPPort());
				break;
			case NOTES:
				DebugSendF("Kad2Req(Notes)", contact->GetIPAddress(), contact->GetUDPPort());
				break;
			default:
				DebugSend(Kad2Req, contact->GetIPAddress(), contact->GetUDPPort());
				break;
			}
#endif
#ifdef ENABLE_KAD_NODE_PROTECTION
			// Start the clock on this request. The answer's round-trip time feeds the
			// shared response-time estimator, and JumpStart uses the same record to
			// notice a request that has gone past the estimated ceiling.
			sPendingRequest pending = {
				::GetTickCount64(), contact->GetIPAddress(), contact->GetUDPPort(), false
			};
			m_pendingRequests[contact->GetClientID()] = pending;
#endif
		} else {
			wxFAIL;
		}
	} catch (const CEOFException &err) {
		AddDebugLogLineC(logKadSearch, "CEOFException in CSearch::SendFindValue: " + err.what());
	} catch (const CInvalidPacket &err) {
		AddDebugLogLineC(
			logKadSearch, "CInvalidPacket Exception in CSearch::SendFindValue: " + err.what());
	} catch (const wxString &e) {
		AddDebugLogLineC(logKadSearch, "Exception in CSearch::SendFindValue: " + e);
	}
}

// The terminal half of RequestMoreResults()'s guards -- everything that will still be true on the
// next press. Deliberately does NOT walk m_responded: "no un-reasked peer right now" is transient,
// clears when another peer answers, and must not read as "never again".
bool CSearch::CanReaskMore() const
{
	return !m_stopping && GetRequestContactCount() == KADEMLIA_FIND_VALUE &&
	       m_requestedMoreNodes.size() < KADEMLIA_FIND_VALUE_MORE_REASKS;
}

bool CSearch::RequestMoreResults()
{
	// Walk m_responded (sorted by distance to target) for the closest peer not yet asked for
	// KADEMLIA_FIND_VALUE_MORE, and dispatch the wider variant to it. Each reask returns up to
	// 11 closer contacts instead of 2, which the ProcessResponse cascade then queries --
	// surfacing more file matches from one extra ring of the routing-table neighbourhood.
	//
	// Bounded by KADEMLIA_FIND_VALUE_MORE_REASKS: past 4 reasks the local neighbourhood for a
	// keyword is typically exhausted.

	if (m_stopping) {
		return false;
	}
	if (GetRequestContactCount() != KADEMLIA_FIND_VALUE) {
		// reaskMore is only meaningful for FIND_VALUE searches
		// (KEYWORD / FILE / FINDSOURCE / NOTES).
		return false;
	}
	if (m_requestedMoreNodes.size() >= KADEMLIA_FIND_VALUE_MORE_REASKS) {
		return false;
	}

	// m_responded is keyed by distance, so iteration is closest-first. m_tried is a parallel
	// ContactMap on the same key, so the CContact* is looked up there.
	for (RespondedMap::const_iterator it = m_responded.begin(); it != m_responded.end(); ++it) {
		const CUInt128 &distance = it->first;
		ContactMap::const_iterator triedIt = m_tried.find(distance);
		if (triedIt == m_tried.end()) {
			continue; // shouldn't happen; defensive
		}
		CContact *contact = triedIt->second;
		if (m_requestedMoreNodes.count(contact->GetClientID()) > 0) {
			continue; // already reasked this one
		}
		AddDebugLogLineN(logKadSearch,
			CFormat("User-triggered RequestMoreResults: reasking %s for more contacts (search "
				"id=%x, reask %u/%u)") %
				KadIPToString(contact->GetIPAddress()) % GetSearchID() %
				(unsigned)(m_requestedMoreNodes.size() + 1) %
				(unsigned)KADEMLIA_FIND_VALUE_MORE_REASKS);
		SendFindValue(contact, true);
		return true;
	}
	return false;
}

// TODO: Redundant metadata checks
void CSearch::PreparePacketForTags(CMemFile *bio, CKnownFile *file, uint8_t targetKadVersion)
{
	TagPtrList taglist;

	try {
		if (file && bio) {
			taglist.push_back(new CTagString(TAG_FILENAME, file->GetFileName().GetPrintable()));
			taglist.push_back(new CTagVarInt(TAG_FILESIZE, file->GetFileSize()));
			taglist.push_back(new CTagVarInt(TAG_SOURCES, file->m_nCompleteSourcesCount));

#ifdef ENABLE_KAD_PROTOCOL_10
			// AICH root hash, added to keyword storage by Kad protocol version 0x09. A
			// node at 0x08 has no handling for this tag, so it is omitted for it: the
			// entry it stores simply carries no AICH hash and stays usable for search
			// and routing.
			if (CKadAICHHashList::PeerSupportsAICHKeywordStorage(targetKadVersion) &&
				file->HasProperAICHHashSet()) {
				const CAICHHash &aichHash = file->GetAICHHashset()->GetMasterHash();
				taglist.push_back(new CTagBsob(TAG_KADAICHHASHPUB,
					aichHash.GetRawHash(),
					(uint8_t)CAICHHash::GetHashSize()));
			}
#else
			(void)targetKadVersion;
#endif

			// eD2K file type (Audio, Video, ...)
			// NOTE: Archives and CD-Images are published with file type "Pro"
			wxString strED2KFileType(
				GetED2KFileTypeSearchTerm(GetED2KFileTypeID(file->GetFileName())));
			if (!strED2KFileType.IsEmpty()) {
				taglist.push_back(new CTagString(TAG_FILETYPE, strED2KFileType));
			}

			// Additional meta data (Artist, Album, Codec, Length, ...).
			//
			// NOT only verified metadata, despite what this used to claim: a download
			// inherits its source's tags as a during-download preview, and
			// GetMetaDataVer() answers "has any FT_MEDIA_* tag", not "was locally
			// probed". So a partfile republishes what its search result advertised
			// until its own completion probe corrects it. Telling the two apart needs a
			// locally-probed flag on the file.
			if (file->GetMetaDataVer() > 0) {
				// Looked up by id ALONE, not by (id, type). The old exact
				// GetTag(id, TAGTYPE_UINT32) was safe only while everything
				// reaching a CKnownFile locally happened to be a CTagInt32; now
				// that a download can inherit a narrower integer from a Kad hit, it
				// would silently stop publishing the moment such a tag was stored.
				static const uint8_t _aMetaTags[] = { FT_MEDIA_ARTIST,
					FT_MEDIA_ALBUM,
					FT_MEDIA_TITLE,
					FT_MEDIA_LENGTH,
					FT_MEDIA_BITRATE,
					FT_MEDIA_CODEC };
				for (unsigned int i = 0; i < itemsof(_aMetaTags); i++) {
					const ::CTag *pTag = file->GetTag(_aMetaTags[i]);
					if (pTag) {
						// skip string tags with empty string values
						if (pTag->IsStr() && pTag->GetStr().IsEmpty()) {
							continue;
						}
						// skip integer tags with '0' values
						if (pTag->IsInt() && pTag->GetInt() == 0) {
							continue;
						}
						wxString szKadTagName = CFormat("%c") % pTag->GetNameID();
						if (pTag->IsStr()) {
							taglist.push_back(
								new CTagString(szKadTagName, pTag->GetStr()));
						} else {
							taglist.push_back(
								new CTagVarInt(szKadTagName, pTag->GetInt()));
						}
					}
				}
			}
			bio->WriteTagPtrList(taglist);
		} else {
			// If we get here.. Bad things happen.. Will fix this later if it is a real issue.
			wxFAIL;
		}
	} catch (const CEOFException &err) {
		AddDebugLogLineC(
			logKadSearch, "CEOFException in CSearch::PreparePacketForTags: " + err.what());
	} catch (const CInvalidPacket &err) {
		AddDebugLogLineC(logKadSearch,
			"CInvalidPacket Exception in CSearch::PreparePacketForTags: " + err.what());
	} catch (const wxString &e) {
		AddDebugLogLineC(logKadSearch, "Exception in CSearch::PreparePacketForTags: " + e);
	}

	deleteTagPtrListEntries(&taglist);
}

void CSearch::SetSearchTermData(uint32_t searchTermsDataSize, const uint8_t *searchTermsData)
{
	m_searchTermsDataSize = searchTermsDataSize;
	m_searchTermsData = new uint8_t[searchTermsDataSize];
	memcpy(m_searchTermsData, searchTermsData, searchTermsDataSize);
}

uint8_t CSearch::GetRequestContactCount() const
{
	// Returns the amount of contacts we request on routing queries based on the search type
	switch (m_type) {
	case NODE:
	case NODECOMPLETE:
	case NODESPECIAL:
	case NODEFWCHECKUDP:
		return KADEMLIA_FIND_NODE;
	case FILE:
	case KEYWORD:
	case FINDSOURCE:
	case NOTES:
		return KADEMLIA_FIND_VALUE;
	case FINDBUDDY:
	case STOREFILE:
	case STOREKEYWORD:
	case STORENOTES:
		return KADEMLIA_STORE;
	default:
		AddDebugLogLineN(logKadSearch, "Invalid search type. (CSearch::GetRequestContactCount())");
		wxFAIL;
		return 0;
	}
}
