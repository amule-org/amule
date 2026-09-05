//								-*- C++ -*-
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

#ifndef __SEARCH_H__
#define __SEARCH_H__

#include <set>

#include "SearchManager.h"

class CKnownFile;
class CTag;

////////////////////////////////////////
namespace Kademlia
{
////////////////////////////////////////

class CKadClientSearcher;

class CSearch
{
	friend class CSearchManager;

public:
	uint32_t GetSearchID() const noexcept { return m_searchID; }
	void SetSearchID(uint32_t id) noexcept
	{
		m_searchID = id;
		m_searchIDAssigned = true;
	}
	// Whether this search was ever given an id. Only PrepareFindKeywords and
	// PrepareLookup assign one; FindNode, FindNodeSpecial and
	// FindNodeFWCheckUDP leave the constructor's default, which is the same
	// value an EC client that predates multi-search uses for every search it
	// runs (0xFFFFFFFF). An id lookup must not resolve to a search that never
	// claimed one, or aMule's own node lookups answer for a legacy client's
	// search. The value alone cannot say: a legacy Kad keyword search really
	// does get 0xFFFFFFFF assigned, deliberately, in PrepareFindKeywords.
	bool HasSearchID() const noexcept { return m_searchIDAssigned; }
	uint32_t GetSearchTypes() const noexcept { return m_type; }
	void SetSearchTypes(uint32_t val) noexcept { m_type = val; }
	void SetTargetID(const CUInt128 &val) noexcept { m_target = val; }
	CUInt128 GetTarget() const noexcept { return m_target; }

	uint32_t GetAnswers() const noexcept
	{
		return m_fileIDs.size() ? m_answers / ((m_fileIDs.size() + 49) / 50) : m_answers;
	}
	uint32_t GetRequestAnswer() const noexcept { return m_totalRequestAnswers; }

	const wxString &GetFileName(void) const noexcept { return m_fileName; }
	void SetFileName(const wxString &fileName) noexcept { m_fileName = fileName; }

	void AddFileID(const CUInt128 &id) { m_fileIDs.push_back(id); }
	// `targetKadVersion` is the advertised Kad version of the node we are
	// about to publish to; tags introduced after its version are omitted.
	void PreparePacketForTags(CMemFile *packet, CKnownFile *file, uint8_t targetKadVersion);
	bool Stopping() const noexcept { return m_stopping; }

	uint32_t GetNodeLoad() const noexcept
	{
		return m_totalLoadResponses == 0 ? 0 : m_totalLoad / m_totalLoadResponses;
	}
	uint32_t GetNodeLoadResponse() const noexcept { return m_totalLoadResponses; }
	uint32_t GetNodeLoadTotal() const noexcept { return m_totalLoad; }
	void UpdateNodeLoad(uint8_t load) noexcept
	{
		m_totalLoad += load;
		m_totalLoadResponses++;
	}

	void SetSearchTermData(uint32_t searchTermsDataSize, const uint8_t *searchTermsData);

	CKadClientSearcher *GetNodeSpecialSearchRequester() const noexcept
	{
		return m_nodeSpecialSearchRequester;
	}
	void SetNodeSpecialSearchRequester(CKadClientSearcher *requester) noexcept
	{
		m_nodeSpecialSearchRequester = requester;
	}

	// User-triggered widening of the Kad result set.  Walks m_responded for
	// the closest contact we have not already reasked, and sends it
	// SendFindValue with reaskMore=true (i.e. KADEMLIA_FIND_VALUE_MORE on
	// the wire instead of KADEMLIA_FIND_VALUE — peers return up to 11
	// closer contacts instead of 2).  Subsequent FIND_VALUE queries against
	// those contacts surface additional file matches that the search's
	// initial alpha=ALPHA_QUERY frontier missed.  Bounded internally by
	// m_requestedMoreNodes.size() < KADEMLIA_FIND_VALUE_MORE_REASKS to
	// limit per-search network impact.  Returns true if a reask was
	// dispatched, false if no eligible candidate remains.
	bool RequestMoreResults();

	// Whether this search could still be widened by a future reask, ignoring
	// whether one is dispatchable right now.
	//
	// The distinction is the point. RequestMoreResults() returns false both
	// when the search is finished with reasking for good (it is stopping, or
	// the reask budget is spent) and when it simply has no un-reasked peer to
	// send to *yet* -- and those want opposite answers from a UI. The first
	// is terminal, so the "More" control should go away; the second clears as
	// soon as another peer responds, so the control must stay.
	//
	// Callers pair the two as `fired || CanReaskMore()`. The `fired ||` is
	// not optional: the reask that consumes the last of the budget really
	// happens, and this predicate is already false by the time it returns.
	bool CanReaskMore() const;

	enum
	{
		NODE,
		NODECOMPLETE,
		FILE,
		KEYWORD,
		NOTES,
		STOREFILE,
		STOREKEYWORD,
		STORENOTES,
		FINDBUDDY,
		FINDSOURCE,
		// nodesearch request from requester "outside" of kad to find
		// the IP of a given NodeID
		NODESPECIAL,
		// find new unknown IPs for a UDP firewallcheck
		NODEFWCHECKUDP
	};

	CSearch();
	~CSearch();

private:
	void Go();
	void ProcessResponse(uint32 fromIP, uint16 fromPort, ContactList *results);
	void ProcessResult(const CUInt128 &answer, TagPtrList *info, uint32_t fromIP, uint16_t fromPort);
	void ProcessResultFile(const CUInt128 &answer, TagPtrList *info);
	void ProcessResultKeyword(
		const CUInt128 &answer, TagPtrList *info, uint32_t fromIP, uint16_t fromPort);
	void ProcessResultNotes(const CUInt128 &answer, TagPtrList *info);
	void JumpStart();
	void SendFindValue(CContact *contact, bool reaskMore = false);
	void PrepareToStop() noexcept;
	void StorePacket();

	uint8_t GetRequestContactCount() const;

	bool m_stopping;
	time_t m_created;
	uint32_t m_type;
	uint32_t m_answers;
	uint32_t m_totalRequestAnswers;
	uint32_t m_totalLoad;
	uint32_t m_totalLoadResponses;
	uint32_t m_lastResponse;

	uint32_t m_searchID;
	bool m_searchIDAssigned;
	CUInt128 m_target;
	uint32_t m_searchTermsDataSize;
	uint8_t *m_searchTermsData;
	WordList m_words; // list of words in the search string (populated in
			  // CSearchManager::PrepareFindKeywords)
	wxString m_fileName;
	UIntList m_fileIDs;
	CKadClientSearcher *m_nodeSpecialSearchRequester; // used to callback result for NODESPECIAL searches

	typedef std::map<CUInt128, bool> RespondedMap;

	ContactMap m_possible;
	ContactMap m_tried;
	RespondedMap m_responded;
	ContactMap m_best;
	ContactList m_delete;
	ContactMap m_inUse;
	CUInt128 m_closestDistantFound; // not used for the search itself, but for statistical data collecting

	// Set of contact ClientIDs we have asked KADEMLIA_FIND_VALUE_MORE
	// from (the wider 11-contact response variant).  Tracked as a set
	// rather than a single pointer so that:
	//   - the existing dead-nodes-fallback at JumpStart can still fire
	//     once on the closest responded node (semantically: empty set
	//     before, one entry after, like the old NULL/non-NULL check);
	//   - RequestMoreResults() can be called multiple times to widen
	//     further on subsequent responded nodes, capped by
	//     KADEMLIA_FIND_VALUE_MORE_REASKS.
	std::set<CUInt128> m_requestedMoreNodes;
};

} // namespace Kademlia

#endif //__SEARCH_H__
// File_checked_for_headers
