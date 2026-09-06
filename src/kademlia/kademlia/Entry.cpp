//
// This file is part of the aMule Project.
//
// Copyright (c) 2008-2011 Dévai Tamás ( gonosztopi@amule.org )
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

#include "Entry.h"
#include <common/Macros.h>
#include <tags/FileTags.h>
#include <protocol/kad/Constants.h>
#include "Indexed.h"
#include "../../SafeFile.h"
#include "../../GetTickCount.h"
#include "../../Logger.h"
#include "../../NetworkFunctions.h"

using namespace Kademlia;

CKeyEntry::GlobalPublishIPMap CKeyEntry::s_globalPublishIPs;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////// CEntry
CEntry::~CEntry()
{
	deleteTagPtrListEntries(&m_taglist);
}

CEntry *CEntry::Copy() const
{
	CEntry *entry = new CEntry();
	for (FileNameList::const_iterator it = m_filenames.begin(); it != m_filenames.end(); ++it) {
		entry->m_filenames.push_back(*it);
	}
	entry->m_uIP = m_uIP;
	entry->m_uKeyID = m_uKeyID;
	entry->m_tLifeTime = m_tLifeTime;
	entry->m_uSize = m_uSize;
	entry->m_bSource = m_bSource;
	entry->m_uSourceID = m_uSourceID;
	entry->m_uTCPport = m_uTCPport;
	entry->m_uUDPport = m_uUDPport;
	for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
		entry->m_taglist.push_back((*it)->CloneTag());
	}
	return entry;
}

void CEntry::AddTag(CTag *tag, uint32_t dbgSourceIP)
{
	// Filter tags which are for sending query results only and should never be stored (or even
	// worse sent within the taglist). TAG_PUBLISHINFO is one we build when answering a keyword
	// search, out of our own count of distinct publishers and how much we trust them; stored, a
	// peer-supplied one travels back out of CKeyEntry::WriteTagListWithPublishInfo() alongside
	// our genuine value.
	//
	// TAG_KADAICHHASHRESULT is the same shape of tag and belongs in the same
	// branch: we build it when answering a keyword search, out of the AICH
	// hashes publishers gave us and how many gave each one. A stored
	// peer-supplied one leaves by the same route and reads as our own
	// assessment. Deliberately ungated: the guard's value is that it is one
	// unconditional branch covering every caller, and filtering a tag this
	// build never emits costs nothing. It also covers the three storing paths
	// a handler-side filter does not -- source and notes publishing, and
	// CIndexed::ReadFile(), which rebuilds every entry from key_index.dat
	// with its raw taglist, so a node poisoned before this fix would reload
	// the tag on restart and keep serving it.
	//
	// TAG_KADAICHHASHPUB is not here: that one is consumed into a member
	// rather than filtered, which is the split 0.70b, 0.72a, eMuleAI and
	// emule-qt all use.
	if (!tag->GetName().Cmp(TAG_PUBLISHINFO) || !tag->GetName().Cmp(TAG_KADAICHHASHRESULT)) {
		AddDebugLogLineN(logKadEntryTracking,
			CFormat("Filtered result-only tag on storing, source %s") %
				(dbgSourceIP ? KadIPToString(dbgSourceIP) : wxString(wxT("local"))));
		delete tag;
		return;
	}
	m_taglist.push_back(tag);
}

bool CEntry::GetIntTagValue(const wxString &tagname, uint64_t &value, bool includeVirtualTags) const
{
	for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
		if ((*it)->IsInt() && ((*it)->GetName() == tagname)) {
			value = (*it)->GetInt();
			return true;
		}
	}

	if (includeVirtualTags) {
		// SizeTag is not stored anymore, but queried in some places
		if (tagname == TAG_FILESIZE) {
			value = m_uSize;
			return true;
		}
	}
	value = 0;
	return false;
}

wxString CEntry::GetStrTagValue(const wxString &tagname) const
{
	for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
		if (((*it)->GetName() == tagname) && (*it)->IsStr()) {
			return (*it)->GetStr();
		}
	}
	return "";
}

void CEntry::SetFileName(const wxString &name)
{
	// Empty filenames are protocol garbage -- a peer publishing a Kad note
	// or keyword with an empty FT_FILENAME tag should never end up in
	// m_filenames at all.  Without this guard the empty entry takes the
	// popularity slot, GetCommonFileName returns "" and trips its own
	// !empty-or-list-empty invariant on the next call (issue #674).
	if (name.IsEmpty()) {
		return;
	}
	if (!m_filenames.empty()) {
		wxFAIL;
		m_filenames.clear();
	}
	sFileNameEntry sFN = { name, 1 };
	m_filenames.push_front(sFN);
}

wxString CEntry::GetCommonFileName() const
{
	// Return the filename most publishers agreed on (by popularity
	// index).  The index isn't the actual count of publishers — just
	// a relative number for comparing entries — so the choice is
	// approximate.
	//
	// Seed the running max from the first entry (rather than 0) so
	// that an all-zero m_filenames still yields a non-empty result.
	// Our own code never produces a popularity-0 entry -- SetFileName
	// creates at 1 and the merge path only ever increments -- but two
	// paths we don't control can land one in m_filenames anyway:
	// (a) a remote publisher could send a popularity-0 entry over the
	// wire (no validation on ReadUInt32), and (b) on-disk data from
	// any node that ran an older build doing popularity decay can
	// load back at 0 here.  The previous "highest > 0"-style loop
	// would then leave the result iterator at end() and return an
	// empty string -- silently dropping TAG_FILENAME from search
	// responses (Entry.h GetTagCount), making SearchTermsMatch
	// return false for every term, and tripping the
	// GetCommonFileName().IsEmpty() reject path in
	// CIndexed::AddKeyword.  Picking the first entry as a
	// deterministic fallback preserves the protocol invariant
	// "non-empty m_filenames yields a non-empty common name" without
	// changing behaviour for any case where a real winner exists.
	if (m_filenames.empty()) {
		return wxString("");
	}
	FileNameList::const_iterator result = m_filenames.begin();
	uint32_t highestPopularityIndex = result->m_popularityIndex;
	for (FileNameList::const_iterator it = std::next(result); it != m_filenames.end(); ++it) {
		if (it->m_popularityIndex > highestPopularityIndex) {
			highestPopularityIndex = it->m_popularityIndex;
			result = it;
		}
	}
	wxASSERT(!result->m_filename.IsEmpty());
	return result->m_filename;
}

void CEntry::WriteTagListInc(CFileDataIO *data, uint32_t increaseTagNumber)
{
	// write taglist and add name + size tag
	wxCHECK_RET(data != NULL, "data must not be NULL");

	uint32_t count =
		GetTagCount() + increaseTagNumber; // will include name and size tag in the count if needed
	wxASSERT(count <= 0xFF);
	data->WriteUInt8((uint8_t)count);

	if (!GetCommonFileName().IsEmpty()) {
		wxASSERT(count > m_taglist.size());
		data->WriteTag(CTagString(TAG_FILENAME, GetCommonFileName()));
	}
	if (m_uSize != 0) {
		wxASSERT(count > m_taglist.size());
		data->WriteTag(CTagVarInt(TAG_FILESIZE, m_uSize));
	}

	for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
		data->WriteTag(**it);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////// CKeyEntry
CKeyEntry::CKeyEntry()
{
	m_publishingIPs = NULL;
	m_trustValue = 0;
	m_lastTrustValueCalc = 0;
}

CKeyEntry::~CKeyEntry()
{
	if (m_publishingIPs != NULL) {
		for (PublishingIPList::const_iterator it = m_publishingIPs->begin();
			it != m_publishingIPs->end();
			++it) {
			AdjustGlobalPublishTracking(it->m_ip, false, "instance delete");
		}
		delete m_publishingIPs;
		m_publishingIPs = NULL;
	}
}

bool CKeyEntry::SearchTermsMatch(const SSearchTerm *searchTerm) const
{
	// boolean operators
	if (searchTerm->type == SSearchTerm::AND) {
		return SearchTermsMatch(searchTerm->left) && SearchTermsMatch(searchTerm->right);
	}

	if (searchTerm->type == SSearchTerm::OR) {
		return SearchTermsMatch(searchTerm->left) || SearchTermsMatch(searchTerm->right);
	}

	if (searchTerm->type == SSearchTerm::NOT) {
		return SearchTermsMatch(searchTerm->left) && !SearchTermsMatch(searchTerm->right);
	}

	// word which is to be searched in the file name (and in additional meta data as done by some ed2k
	// servers???)
	if (searchTerm->type == SSearchTerm::String) {
		int strSearchTerms = searchTerm->astr->GetCount();
		if (strSearchTerms == 0) {
			return false;
		}
		// if there are more than one search strings specified (e.g. "aaa bbb ccc") the entire string
		// is handled like "aaa AND bbb AND ccc". search all strings from the string search term in
		// the tokenized list of the file name. all strings of string search term have to be found
		// (AND)
		wxString commonFileNameLower(GetCommonFileNameLowerCase());
		for (int i = 0; i < strSearchTerms; i++) {
			// this will not give the same results as when tokenizing the filename string, but it
			// is 20 times faster.
			if (commonFileNameLower.Find((*(searchTerm->astr))[i]) == -1) {
				return false;
			}
		}
		return true;
	}

	if (searchTerm->type == SSearchTerm::MetaTag) {
		if (searchTerm->tag->GetType() == 2) { // meta tags with string values
			if (searchTerm->tag->GetName() == TAG_FILEFORMAT) {
				// 21-Sep-2006 []: Special handling for TAG_FILEFORMAT which is already part
				// of the filename and thus does not need to get published nor stored
				// explicitly,
				wxString commonFileName(GetCommonFileName());
				int ext = commonFileName.Find('.', true);
				if (ext != wxNOT_FOUND) {
					return commonFileName.Mid(ext + 1).CmpNoCase(
						       searchTerm->tag->GetStr()) == 0;
				}
			} else {
				for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end();
					++it) {
					if ((*it)->IsStr() &&
						searchTerm->tag->GetName() == (*it)->GetName()) {
						return (*it)->GetStr().CmpNoCase(searchTerm->tag->GetStr()) ==
						       0;
					}
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpGreaterEqual) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value >= searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() >= searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpLessEqual) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value <= searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() <= searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpGreater) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value > searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() > searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpLess) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value < searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() < searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpEqual) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value == searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() == searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpNotEqual) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value != searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() != searchTerm->tag->GetFloat();
				}
			}
		}
	}

	return false;
}

void CKeyEntry::AdjustGlobalPublishTracking(uint32_t ip, bool increase, const wxString &DEBUG_ONLY(dbgReason))
{
	uint32_t count = 0;
	bool found = false;
	GlobalPublishIPMap::const_iterator it =
		s_globalPublishIPs.find(ip & 0xFFFFFF00 /* /24 netmask, take care of endian if needed */);
	if (it != s_globalPublishIPs.end()) {
		count = it->second;
		found = true;
	}

	if (increase) {
		count++;
	} else {
		count--;
	}

	if (count > 0) {
		s_globalPublishIPs[ip & 0xFFFFFF00] = count;
	} else if (found) {
		s_globalPublishIPs.erase(ip & 0xFFFFFF00);
	} else {
		wxFAIL;
	}
#ifdef __DEBUG__
	if (!dbgReason.IsEmpty()) {
		AddDebugLogLineN(logKadEntryTracking,
			CFormat("%s %s (%s) - (%s), new count %u") % (increase ? "Adding" : "Removing") %
				KadIPToString(ip & 0xFFFFFF00) % KadIPToString(ip) % dbgReason % count);
	}
#endif
}

void CKeyEntry::MergeIPsAndFilenames(CKeyEntry *fromEntry)
{
	// this is called when replacing a stored entry with a refreshed one.
	// we want to take over the tracked IPs and the different filenames from the old entry, the rest is
	// still "overwritten" with the refreshed values. This might be not perfect for the taglist in some
	// cases, but we can't afford to store hundreds of taglists to figure out the best one like we do for
	// the filenames now
	if (m_publishingIPs !=
		NULL) { // This instance needs to be a new entry, otherwise we don't want/need to merge
		wxASSERT(fromEntry == NULL);
		wxASSERT(!m_publishingIPs->empty());
		wxASSERT(!m_filenames.empty());
		return;
	}

	// Fetch the AICH root hash this publisher reported, if any, and clear our
	// own single-slot list: from here on m_aichHashes is the *merged* list
	// taken over from the stored entry, and the reported hash is folded into
	// it below so the popularity counts stay right.
	wxASSERT(m_aichHashes.GetSlotCount() <= 1);
	bool hasNewAICHHash = (m_aichHashes.GetSlotCount() > 0);
	CKadAICHHash newAICHHash = m_aichHashes.GetHashAt(0);
	m_aichHashes = CKadAICHHashList();

	bool refresh = false;
	if (fromEntry == NULL || fromEntry->m_publishingIPs == NULL) {
		wxASSERT(fromEntry == NULL);
		// if called with NULL, this is a complete new entry and we need to initialize our lists
		if (m_publishingIPs == NULL) {
			m_publishingIPs = new PublishingIPList();
		}
		// update the global track map below
	} else {
		// take over the AICH hashes the stored entry accumulated
		m_aichHashes = fromEntry->m_aichHashes;

		// merge the tracked IPs, add this one if not already on the list
		m_publishingIPs = fromEntry->m_publishingIPs;
		fromEntry->m_publishingIPs = NULL;
		bool fastRefresh = false;
		for (PublishingIPList::iterator it = m_publishingIPs->begin(); it != m_publishingIPs->end();
			++it) {
			if (it->m_ip == m_uIP) {
				refresh = true;
				if ((time(NULL) - it->m_lastPublish) < (KADEMLIAREPUBLISHTIMES - HR2S(1))) {
					AddDebugLogLineN(logKadEntryTracking,
						"FastRefresh publish, ip: " + KadIPToString(m_uIP));
					fastRefresh = true; // refreshed faster than expected, will not count
							    // into filenamepopularity index
				}
				it->m_lastPublish = time(NULL);

				// Has the AICH hash this publisher reports changed?
				// A publisher that stops reporting one (downgrade,
				// or a hash set it can no longer vouch for) must
				// lose its vote, or the count would keep a hash
				// alive that nobody publishes any more.
				if (hasNewAICHHash) {
					if (it->m_aichHashIdx == CKadAICHHashList::INVALID_INDEX) {
						AddDebugLogLineN(logKadEntryTracking,
							"New AICH hash during publishing (publisher "
							"reported none before), publisher ip: " +
								KadIPToString(m_uIP));
						it->m_aichHashIdx = m_aichHashes.AddReference(newAICHHash);
					} else if (!(m_aichHashes.GetHashAt(it->m_aichHashIdx) ==
							   newAICHHash)) {
						AddDebugLogLineN(logKadEntryTracking,
							"AICH hash changed, publisher ip: " +
								KadIPToString(m_uIP));
						m_aichHashes.DropReferenceAt(it->m_aichHashIdx);
						it->m_aichHashIdx = m_aichHashes.AddReference(newAICHHash);
					}
				} else if (it->m_aichHashIdx != CKadAICHHashList::INVALID_INDEX) {
					AddDebugLogLineN(logKadEntryTracking,
						"AICH hash removed, publisher ip: " + KadIPToString(m_uIP));
					m_aichHashes.DropReferenceAt(it->m_aichHashIdx);
					it->m_aichHashIdx = CKadAICHHashList::INVALID_INDEX;
				}

				m_publishingIPs->push_back(*it);
				m_publishingIPs->erase(it);
				break;
			}
		}

		// copy over trust value, in case we don't want to recalculate
		m_trustValue = fromEntry->m_trustValue;
		m_lastTrustValueCalc = fromEntry->m_lastTrustValueCalc;

		// copy over the different names, if they are different the one we have right now
		wxASSERT(m_filenames.size() ==
			 1); // we should have only one name here, since it's the entry from one single source
		sFileNameEntry currentName = { "", 0 };
		if (m_filenames.size() != 0) {
			currentName = m_filenames.front();
			m_filenames.pop_front();
		}

		// Cap m_filenames so a single CKeyEntry can't accumulate
		// unbounded filename variants. A popular file collects one
		// sFileNameEntry per distinct publisher-chosen name (renames,
		// language variants, mirror prefixes, trailing-paren copies,
		// case differences); without a cap the list grows monotonically
		// for the lifetime of the entry, which on a long-running
		// shareset shows up as a steady ~MB/hour RSS climb in amuled.
		// 100 matches the m_publishingIPs cap below and is comfortably
		// above the count of variants any honest publisher set produces
		// for a single hash — GetCommonFileName already picks the
		// highest-popularity entry, so the cap is shaped to keep
		// popularity-ordered survivors.
		const size_t MAX_FILENAMES = 100;

		// Compare-and-skip insertion (per irwir's review of #314): if
		// we're already at the cap, only accept the new entry when its
		// popularity beats the weakest survivor — otherwise drop it on
		// the floor instead of pushing then immediately re-evicting.
		// O(N) per insert via std::min_element; no global sort needed.
		auto pushBounded = [&](const sFileNameEntry &candidate) {
			if (m_filenames.size() < MAX_FILENAMES) {
				m_filenames.push_back(candidate);
				return;
			}
			FileNameList::iterator weakest = std::min_element(m_filenames.begin(),
				m_filenames.end(),
				[](const sFileNameEntry &a, const sFileNameEntry &b) {
					return a.m_popularityIndex < b.m_popularityIndex;
				});
			if (candidate.m_popularityIndex > weakest->m_popularityIndex) {
				*weakest = candidate;
			}
			// else: candidate's popularity is no better than the
			// weakest already-kept entry; drop the candidate.
		};

		bool duplicate = false;
		for (FileNameList::iterator it = fromEntry->m_filenames.begin();
			it != fromEntry->m_filenames.end();
			++it) {
			sFileNameEntry nameToCopy = *it;
			// Defence-in-depth: even though SetFileName now rejects empty
			// names, an older on-disk Kad index could still hold one from
			// before that guard landed.  Drop it here too rather than
			// propagating into our m_filenames.
			if (nameToCopy.m_filename.IsEmpty()) {
				continue;
			}
			if (currentName.m_filename.CmpNoCase(nameToCopy.m_filename) == 0) {
				// the filename of our new entry matches with our old, increase the popularity
				// index for the old one
				duplicate = true;
				if (!fastRefresh) {
					nameToCopy.m_popularityIndex++;
				}
			}
			pushBounded(nameToCopy);
		}
		if (!duplicate && !currentName.m_filename.IsEmpty()) {
			// Skip the synthetic currentName = { "", 0 } default that
			// happens when m_filenames was unexpectedly empty above
			// (wxASSERT fires in Debug, but Release keeps going).
			pushBounded(currentName);
		}
	}

	// if this was a refresh done, otherwise update the global track map
	if (!refresh) {
		wxASSERT(m_uIP != 0);
		uint16_t aichHashIdx = hasNewAICHHash ? m_aichHashes.AddReference(newAICHHash)
						      : CKadAICHHashList::INVALID_INDEX;
		sPublishingIP add = { m_uIP, time(nullptr), aichHashIdx };
		m_publishingIPs->push_back(add);

		// add the publisher to the tacking list
		AdjustGlobalPublishTracking(m_uIP, true, "new publisher");

		// we keep track of max 100 IPs, in order to avoid too much time for
		// calculation/storing/loading.
		if (m_publishingIPs->size() > 100) {
			sPublishingIP curEntry = m_publishingIPs->front();
			m_publishingIPs->pop_front();
			m_aichHashes.DropReferenceAt(curEntry.m_aichHashIdx);
			AdjustGlobalPublishTracking(curEntry.m_ip, false, "more than 100 publishers purge");
		}

		// since we added a new publisher, we want to (re)calculate the trust value for this entry
		ReCalculateTrustValue();
	}
	AddDebugLogLineN(logKadEntryTracking,
		CFormat("Indexed Keyword, Refresh: %s, Current Publisher: %s, Total Publishers: %u, Total "
			"different Names: %u, TrustValue: %.2f, file: %s") %
			(refresh ? "Yes" : "No") % KadIPToString(m_uIP) % m_publishingIPs->size() %
			m_filenames.size() % m_trustValue % m_uSourceID.ToHexString());
}

void CKeyEntry::ReCalculateTrustValue()
{
#define PUBLISHPOINTSSPERSUBNET 10.0
	// The trustvalue is supposed to be an indicator how trustworthy/important (or spammy) this entry is
	// and lies between 0 and ~10000, but mostly we say everything below 1 is bad, everything above 1 is
	// good. It is calculated by looking at how many different IPs/24 have published this entry and how
	// many entries each of those IPs have. Each IP/24 has x (say 3) points. This means if one IP
	// publishes 3 different entries without any other IP publishing those entries, each of those entries
	// will have 3 / 3 = 1 Trustvalue. That's fine. If it publishes 6 alone, each entry has 3 / 6 = 0.5
	// trustvalue - not so good However if there is another publisher for entry 5, which only publishes
	// this entry then we have 3/6 + 3/1 = 3.5 trustvalue for this entry
	//
	// What's the point? With this rating we try to avoid getting spammed with entries for a given keyword
	// by a small IP range, which blends out all other entries for this keyword do to its amount as well
	// as giving an indicator for the searcher. So if we are the node to index "Knoppix", and someone from
	// 1 IP publishes 500 times "knoppix casino 500% bonus.txt", all those entries will have a trustvalue
	// of 0.006 and we make sure that on search requests for knoppix, those entries are only returned
	// after all entries with a trustvalue > 1 were sent (if there is still space).
	//
	// Its important to note that entry with < 1 do NOT get ignored or singled out, this only comes into
	// play if we have 300 more results for a search request rating > 1
	wxCHECK_RET(m_publishingIPs != NULL, "No publishing IPs?");

	m_lastTrustValueCalc = ::GetTickCount64();
	m_trustValue = 0;
	wxASSERT(!m_publishingIPs->empty());
	for (PublishingIPList::iterator it = m_publishingIPs->begin(); it != m_publishingIPs->end(); ++it) {
		sPublishingIP curEntry = *it;
		uint32_t count = 0;
		GlobalPublishIPMap::const_iterator itMap = s_globalPublishIPs.find(
			curEntry.m_ip & 0xFFFFFF00 /* /24 netmask, take care of endian if needed*/);
		if (itMap != s_globalPublishIPs.end()) {
			count = itMap->second;
		}
		if (count > 0) {
			m_trustValue += PUBLISHPOINTSSPERSUBNET / count;
		} else {
			AddDebugLogLineN(logKadEntryTracking, "Inconsistency in RecalcualteTrustValue()");
			wxFAIL;
		}
	}
}

double CKeyEntry::GetTrustValue()
{
	// update if last calculation is too old, will assert if this entry is not supposed to have a
	// trustvalue
	if (::GetTickCount64() - m_lastTrustValueCalc > MIN2MS(10)) {
		ReCalculateTrustValue();
	}
	return m_trustValue;
}

void CKeyEntry::CleanUpTrackedPublishers()
{
	if (m_publishingIPs == NULL) {
		return;
	}

	time_t now = time(NULL);
	while (!m_publishingIPs->empty()) {
		// entries are ordered, older ones first
		sPublishingIP curEntry = m_publishingIPs->front();
		if (now - curEntry.m_lastPublish > KADEMLIAREPUBLISHTIMEK) {
			AdjustGlobalPublishTracking(curEntry.m_ip, false, "cleanup");
			// An expired publisher loses its AICH vote with everything
			// else; without this the hash would outlive every publisher
			// that ever reported it and keep being handed to searchers.
			m_aichHashes.DropReferenceAt(curEntry.m_aichHashIdx);
			m_publishingIPs->pop_front();
		} else {
			break;
		}
	}
}

void CKeyEntry::SetPublishedAICHHash(const CKadAICHHash &hash)
{
	m_aichHashes.AddReference(hash);
}

void CKeyEntry::WritePublishTrackingDataToFile(CFileDataIO *data)
{
	// format: <AICH_HashCount 2><{<AICH Hash 20>} AICH_HashCount>
	//         <Names_Count 4><{<Name string><PopularityIndex 4>} Names_Count>
	//         <PublisherCount 4><{<IP 4><Time 4><AICH Idx 2>} PublisherCount>
	//
	// Only referenced hashes are written, so the stored indexes are the
	// compacted ones -- otherwise a hash whose last publisher expired would
	// be reloaded with a popularity of zero for ever.
	//
	// Gated together with the keyword-index version in CIndexed: with the
	// gate off we write a version-3 file with neither the AICH block nor the
	// per-publisher index, which is byte-for-byte what upstream writes and
	// what an upstream binary can read back.
#ifdef ENABLE_KAD_PROTOCOL_10
	const std::vector<uint16_t> newIndexes = m_aichHashes.BuildCompactionMap();
	data->WriteUInt16(m_aichHashes.GetReferencedCount());
	for (uint16_t i = 0; i < m_aichHashes.GetSlotCount(); i++) {
		if (newIndexes[i] != CKadAICHHashList::INVALID_INDEX) {
			const CKadAICHHash &hash = m_aichHashes.GetHashAt(i);
			data->Write(hash.data(), hash.size());
		}
	}
#endif

	data->WriteUInt32((uint32_t)m_filenames.size());
	for (FileNameList::const_iterator it = m_filenames.begin(); it != m_filenames.end(); ++it) {
		data->WriteString(it->m_filename, utf8strRaw, 2);
		data->WriteUInt32(it->m_popularityIndex);
	}

	if (m_publishingIPs != NULL) {
		data->WriteUInt32((uint32_t)m_publishingIPs->size());
		for (PublishingIPList::const_iterator it = m_publishingIPs->begin();
			it != m_publishingIPs->end();
			++it) {
			wxASSERT(it->m_ip != 0);
			data->WriteUInt32(it->m_ip);
			data->WriteUInt32((uint32_t)it->m_lastPublish);
#ifdef ENABLE_KAD_PROTOCOL_10
			uint16_t idx = CKadAICHHashList::INVALID_INDEX;
			if (it->m_aichHashIdx != CKadAICHHashList::INVALID_INDEX) {
				idx = newIndexes[it->m_aichHashIdx];
			}
			data->WriteUInt16(idx);
#endif
		}
	} else {
		wxFAIL;
		data->WriteUInt32(0);
	}
}

void CKeyEntry::ReadPublishTrackingDataFromFile(CFileDataIO *data, bool includesAICH)
{
	// format: <AICH_HashCount 2><{<AICH Hash 20>} AICH_HashCount>
	//         <Names_Count 4><{<Name string><PopularityIndex 4>} Names_Count>
	//         <PublisherCount 4><{<IP 4><Time 4><AICH Idx 2>} PublisherCount>
	//
	// The AICH block and the per-publisher index only exist from keyword-index
	// version 4 onwards; an older file loads with no hashes at all, which is
	// the same state as an entry only pre-0x09 peers ever published.
	wxASSERT(m_aichHashes.GetSlotCount() == 0);
	std::vector<CKadAICHHash> loadedHashes;
	if (includesAICH) {
		uint16_t hashCount = data->ReadUInt16();
		for (uint16_t i = 0; i < hashCount; i++) {
			CKadAICHHash hash;
			data->Read(hash.data(), hash.size());
			loadedHashes.push_back(hash);
		}
	}

	wxASSERT(m_filenames.empty());
	uint32_t nameCount = data->ReadUInt32();
	for (uint32_t i = 0; i < nameCount; i++) {
		sFileNameEntry toAdd;
		toAdd.m_filename = data->ReadString(true, 2);
		toAdd.m_popularityIndex = data->ReadUInt32();
		m_filenames.push_back(toAdd);
	}

	wxASSERT(m_publishingIPs == NULL);
	m_publishingIPs = new PublishingIPList();
	uint32_t ipCount = data->ReadUInt32();
#ifdef __WXDEBUG__
	uint32_t dbgLastTime = 0;
#endif
	for (uint32_t i = 0; i < ipCount; i++) {
		sPublishingIP toAdd;
		toAdd.m_ip = data->ReadUInt32();
		wxASSERT(toAdd.m_ip != 0);
		toAdd.m_lastPublish = data->ReadUInt32();
#ifdef __WXDEBUG__
		wxASSERT(
			dbgLastTime <= (uint32_t)toAdd.m_lastPublish); // should always be sorted oldest first
		dbgLastTime = toAdd.m_lastPublish;
#endif

		// Re-attach this publisher to its AICH hash, rebuilding the
		// popularity counts as we go.  An index pointing past the hashes
		// we just read means a corrupt or truncated file, so drop the
		// hash rather than the whole entry.
		toAdd.m_aichHashIdx = CKadAICHHashList::INVALID_INDEX;
		if (includesAICH) {
			uint16_t storedIdx = data->ReadUInt16();
			if (storedIdx != CKadAICHHashList::INVALID_INDEX) {
				if (storedIdx >= loadedHashes.size()) {
					AddDebugLogLineC(logKadEntryTracking,
						"CKeyEntry::ReadPublishTrackingDataFromFile - out of range "
						"AICH hash index while loading keywords");
				} else {
					toAdd.m_aichHashIdx =
						m_aichHashes.AddReference(loadedHashes[storedIdx]);
				}
			}
		}

		AdjustGlobalPublishTracking(toAdd.m_ip, true, "");

		m_publishingIPs->push_back(toAdd);
	}
	ReCalculateTrustValue();
	// #ifdef __DEBUG__
	//	if (GetTrustValue() < 1.0) {
	//		AddDebugLogLineN(logKadEntryTracking,CFormat("Loaded %u different names, %u different
	// publishIPs (trustvalue = %.2f) for file %s") 			% nameCount % ipCount %
	// GetTrustValue() % m_uSourceID.ToHexString());
	//	}
	// #endif
}

void CKeyEntry::DirtyDeletePublishData()
{
	// instead of deleting our publishers properly in the destructor with decreasing the count in the
	// global map we just remove them, and trust that the caller in the end also resets the global map, so
	// the kad shutdown is speed up a bit
	delete m_publishingIPs;
	m_publishingIPs = NULL;
}

void CKeyEntry::WriteTagListWithPublishInfo(CFileDataIO *data)
{
	if (m_publishingIPs == NULL || m_publishingIPs->empty()) {
		wxFAIL;
		WriteTagList(data);
		return;
	}

	// here we add a tag including how many publishers this entry has, the trustvalue and how many
	// different names are known this is supposed to get used in later versions as an indicator for the
	// user how valid this result is (of course this tag alone cannot be trusted 100%, because we could be
	// a bad node, but it's a part of the puzzle)

	// One tag for TAG_PUBLISHINFO, plus TAG_KADAICHHASHRESULT if we have any
	// AICH hash to report.  The AICH tag is written unconditionally rather
	// than per requester because a Kad search request carries no version
	// byte: pre-0x09 peers skip the unknown tag, and it is the *receiver*
	// that gates on the sender's advertised version (see
	// CSearch::ProcessResultKeyword) so a fake tag from an old node cannot
	// be laundered through us.
#ifdef ENABLE_KAD_PROTOCOL_10
	std::vector<uint8_t> aichTagValue = m_aichHashes.EncodeResultTag();
#else
	// Gate off: no AICH tag is ever put in a search answer, so the tag count
	// and the packet are exactly upstream's.
	const std::vector<uint8_t> aichTagValue;
#endif
	WriteTagListInc(data, aichTagValue.empty() ? 1 : 2);

	uint32_t trust = (uint16_t)(GetTrustValue() * 100);
	uint32_t publishers = m_publishingIPs->size() & 0xFF /*% 256*/;
	uint32_t names = m_filenames.size() & 0xFF /*% 256*/;
	// 32 bit tag: <namecount uint8><publishers uint8><trustvalue*100 uint16>
	uint32_t tagValue = (names << 24) | (publishers << 16) | trust;
	data->WriteTag(CTagVarInt(TAG_PUBLISHINFO, tagValue));

	// Last, the AICH hashes reported for this file with the number of
	// publishers behind each one -- normally exactly one hash. A BSOB tag in
	// Kad carries a uint8 length, and CKadAICHHashList keeps the payload
	// inside that budget.
	if (!aichTagValue.empty()) {
		wxASSERT(aichTagValue.size() <= 0xFF);
		data->WriteTag(
			CTagBsob(TAG_KADAICHHASHRESULT, &aichTagValue[0], (uint8_t)aichTagValue.size()));
	}
}
