//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2011 Kry ( elkry@sourceforge.net / http://www.amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2008-2011 Froenchenko Leonid (lfroen@gmail.com)
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

#include "config.h" // Needed for VERSION

#include <set>       // Needed for std::set (m_lastSentSharedFileIds)
#include <list>      // Needed for std::list (multi-search LRU ring)
#include <algorithm> // Needed for std::find (multi-search LRU ring)

#include <ec/cpp/ECMuleSocket.h> // Needed for CECSocket

#include <common/Format.h> // Needed for CFormat

#include <common/ClientVersion.h>
#include <common/MD5Sum.h>
#include "libs/ec/cpp/ECCrypt.h"

#include "ECIdDiff.h"            // Needed for ComputeRemovedIds
#include "ExternalConn.h"        // Interface declarations
#include "ECFullResponseCache.h" // Needed for s_ec*FullCache
#include "updownclient.h"        // Needed for CUpDownClient
#include "Server.h"              // Needed for CServer
#include "ServerList.h"          // Needed for CServerList
#include "PartFile.h"            // Needed for CPartFile
#include "ServerConnect.h"       // Needed for CServerConnect
#include "UploadQueue.h"         // Needed for CUploadQueue
#include "AmuleApiCredentials.h"
#include "BrowseManager.h"
#include "amule.h"      // Needed for theApp
#include "SearchList.h" // Needed for GetSearchResults
#include "ClientList.h"
#include "ChatSessionStore.h"
#include "ClientCreditsList.h" // Needed for CClientCreditsList
#include "ClientCredits.h"     // Needed for CClientCredits, ClientMetaStruct
#ifdef ENABLE_IP2COUNTRY
#include "IP2Country.h" // Needed for CIP2Country (history country codes)
#endif
#include "Preferences.h" // Needed for CPreferences
#include "Logger.h"
#include "GuiEvents.h"     // Needed for Notify_* macros
#include "Statistics.h"    // Needed for theStats
#include "KnownFileList.h" // Needed for CKnownFileList
#include "Friend.h"
#include "FriendList.h"
#include "RandomFunctions.h"
#include "kademlia/kademlia/Kademlia.h"
#include "kademlia/kademlia/UDPFirewallTester.h"
#include "Statistics.h"

//-------------------- File_Encoder --------------------

/*
 * Encode 'obtained parts' info to be sent to remote gui
 */
class CKnownFile_Encoder
{
	// number of sources for each part for progress bar colouring
	RLE_Data m_enc_data;

	// Reconcile epoch, stamped by CFileEncoderMap::UpdateEncoders on every
	// encoder whose file is still listed. Encoders left carrying an older
	// stamp have lost their file and are swept. Replaces the per-call set of
	// live ECIDs that used to serve the same purpose -- see UpdateEncoders.
	uint64 m_seenEpoch;

	// Whether `Get_EC_Response_GetUpdate` has already sent this file to the
	// client carrying its identifying fields. False on a freshly-built
	// encoder, which is what makes a re-created encoder re-send full detail
	// without the caller having to erase anything.
	//
	// Deliberately specific to that one response path rather than a general
	// "the client has this file": a path that has never sent the identity
	// must not take the unchanged-file shortcut, or the client is left with a
	// child-less tag it turns into a ghost entry (#808). The two amuleweb
	// handlers answer the same question from their own sets instead, because
	// they iterate a CopyFileList snapshot rather than the encoder map and
	// reaching the encoder would cost the lookup this exists to avoid. That
	// is why this is one flag and not a per-path bitmask -- a second bit
	// would have no writer.
	bool m_sentOnUpdatePath;

protected:
	const CKnownFile *m_file;

public:
	CKnownFile_Encoder(const CKnownFile *file = nullptr)
	: m_seenEpoch(0)
	, m_sentOnUpdatePath(false)
	{
		m_file = file;
	}

	virtual ~CKnownFile_Encoder() {}

	virtual void Encode(CECTag *parent_tag);

	virtual void ResetEncoder() { m_enc_data.ResetEncoder(); }

	virtual void SetShared() {}
	virtual bool IsShared() { return true; }
	virtual bool IsPartFile_Encoder() { return false; }
	const CKnownFile *GetFile() { return m_file; }

	uint64 GetSeenEpoch() const { return m_seenEpoch; }
	void SetSeenEpoch(uint64 epoch) { m_seenEpoch = epoch; }

	bool WasSentOnUpdatePath() const { return m_sentOnUpdatePath; }
	void MarkSentOnUpdatePath() { m_sentOnUpdatePath = true; }
};

/*!
 * PartStatus strings and gap lists are quite long - RLE encoding will help.
 *
 * Instead of sending each time full part-status string, send
 * RLE encoded difference from previous one.
 *
 * PartFileEncoderData class is used for decode only,
 * while CPartFile_Encoder is used for encode only.
 */
class CPartFile_Encoder : public CKnownFile_Encoder
{
	// blocks requested for download
	RLE_Data m_req_status;
	// gap list
	RLE_Data m_gap_status;
	// source names
	SourcenameItemMap m_sourcenameItemMap;
	// counter for unique source name ids
	int m_sourcenameID;
	// not all part files are shared (only when at least one part is complete)
	bool m_shared;

	// cast inherited member to CPartFile
	const CPartFile *m_PartFile()
	{
		wxCHECK(m_file->IsCPartFile(), NULL);
		return static_cast<const CPartFile *>(m_file);
	}

public:
	// encoder side
	CPartFile_Encoder(const CPartFile *file = 0)
	: CKnownFile_Encoder(file)
	{
		m_sourcenameID = 0;
		m_shared = false;
	}

	virtual ~CPartFile_Encoder() {}

	// encode - take data from m_file
	virtual void Encode(CECTag *parent_tag);

	// Encoder may reset history if full info requested
	virtual void ResetEncoder();

	virtual void SetShared() { m_shared = true; }
	virtual bool IsShared() { return m_shared; }
	virtual bool IsPartFile_Encoder() { return true; }
};

// The encoders for the files this connection has told its client about, held
// in ECID order.
//
// A sorted vector rather than a std::map. The response walk touches every
// entry on every poll and does very little per entry, so its cost was
// dominated by chasing red-black-tree pointers around the heap rather than by
// the work itself -- a linear scan of a contiguous array is something the
// prefetcher can follow, a tree traversal is not. Lookup becomes a binary
// search, which is now the rarer operation: #775 removed the per-file lookups,
// leaving only the reconcile below and two amuleweb call sites. Structural
// change is rarer still, so paying O(n) to merge new entries or compact away
// dead ones is the right side of the trade.
class CFileEncoderMap
{
	typedef std::set<uint32> IDSet;

public:
	typedef std::pair<uint32, CKnownFile_Encoder *> value_type;
	typedef std::vector<value_type> Storage;
	typedef Storage::iterator iterator;
	typedef Storage::const_iterator const_iterator;

	~CFileEncoderMap();

	iterator begin() { return m_entries.begin(); }
	iterator end() { return m_entries.end(); }
	size_t size() const { return m_entries.size(); }

	// Binary search by ECID; end() when absent.
	iterator find(uint32 id);

	// The encoder for `id`, or NULL. Unlike std::map::operator[] this does
	// not insert a default-constructed entry on a miss -- that entry would
	// have held a NULL encoder, which the next reconcile would then have
	// dereferenced.
	CKnownFile_Encoder *operator[](uint32 id);

	// If freshEcids is non-null, receives the ECIDs whose encoder was
	// (re-)created this call. Freshly-created encoders signal that the
	// caller's per-ECID EC caches (CObjTagMap valuemap, and the encoder's
	// own sent-with-detail flag) are stale w.r.t. the client's local view
	// and must be dropped so INC_UPDATE emissions re-send identifying fields.
	void UpdateEncoders(IDSet *freshEcids = nullptr);

private:
	// Order entries, and order an entry against a bare ECID, so the same
	// predicate serves both the sort and the binary search.
	struct KeyLess
	{
		bool operator()(const value_type &a, const value_type &b) const { return a.first < b.first; }
		bool operator()(const value_type &a, uint32 id) const { return a.first < id; }
		bool operator()(uint32 id, const value_type &b) const { return id < b.first; }
	};

	// Entries created during a reconcile, merged in once the pass that made
	// them is done. Appending here rather than inserting into the middle of
	// m_entries keeps a first poll -- where every file is new -- from paying
	// a memmove of the whole array per file.
	Storage m_pending;
	void FlushPending();

	// Sorted ascending by ECID. The GET_UPDATE walk depends on that order:
	// it appends each ECID it passes to the list the removal merge consumes.
	Storage m_entries;

	// The list generations this map was last reconciled against. When both
	// still match, the lists have not gained or lost a file and the reconcile
	// is a no-op -- see the early return in UpdateEncoders. `m_haveListGen`
	// distinguishes "never reconciled" from "reconciled when both counters
	// happened to be zero", which is the state at startup and would otherwise
	// skip the very first pass and leave the map permanently empty.
	uint64 m_lastSharedGen = 0;
	uint64 m_lastDownloadGen = 0;
	bool m_haveListGen = false;

	// Monotonic reconcile counter; see UpdateEncoders. Two things make a
	// stamp comparison safe. The counter is a member of this map, and the map
	// belongs to one CECServerSocket, so it is per-connection and encoders
	// are never shared across clients -- there is no cross-client collision
	// to reason about. And every UpdateEncoders call takes a fresh value
	// before stamping anything, so no encoder can be carrying the value the
	// current call is about to use. 64-bit on top of that, so it also cannot
	// wrap back onto a live encoder's stamp within any plausible uptime.
	uint64 m_epoch = 0;
};

CFileEncoderMap::~CFileEncoderMap()
{
	// DeleteContents() causes infinite recursion here!
	for (const value_type &e : m_entries) {
		delete e.second;
	}
	// Normally empty: every reconcile merges what it created before it
	// returns. Covers a throw part-way through one.
	for (const value_type &e : m_pending) {
		delete e.second;
	}
}

CFileEncoderMap::iterator CFileEncoderMap::find(uint32 id)
{
	const iterator it = std::lower_bound(m_entries.begin(), m_entries.end(), id, KeyLess());
	return (it != m_entries.end() && it->first == id) ? it : m_entries.end();
}

CKnownFile_Encoder *CFileEncoderMap::operator[](uint32 id)
{
	const iterator it = find(id);
	return it == m_entries.end() ? nullptr : it->second;
}

void CFileEncoderMap::FlushPending()
{
	if (m_pending.empty()) {
		return;
	}
	// Sort what arrived, then merge the two runs. Both are already ordered,
	// so this is linear in the total rather than a re-sort of everything.
	std::sort(m_pending.begin(), m_pending.end(), KeyLess());
	const Storage::difference_type split = static_cast<Storage::difference_type>(m_entries.size());
	m_entries.insert(m_entries.end(), m_pending.begin(), m_pending.end());
	std::inplace_merge(m_entries.begin(), m_entries.begin() + split, m_entries.end(), KeyLess());
	m_pending.clear();
}

// Check if encoder contains files that are no longer used
// or if we have new files without encoder yet.
void CFileEncoderMap::UpdateEncoders(IDSet *freshEcids)
{
	// Nothing has entered or left either list since the last reconcile, so
	// the encoder map already mirrors them and the whole pass below -- two
	// O(n) CopyFileList snapshots, a lookup and a stamp per file, and a sweep
	// -- would end exactly where it started. On a library that is not
	// churning, which is the normal case, that is every poll for every
	// connected client.
	//
	// Read the counters BEFORE the snapshots, never after. Read first and the
	// values can only be older than what CopyFileList goes on to see: a file
	// arriving in between leaves us recording a stale generation, so the next
	// poll reconciles again and picks it up one cycle late. Read after, and a
	// change that landed between the copy and the read would be recorded as
	// already seen and never reconciled at all -- a file that silently never
	// appears, or never disappears, for the life of the connection.
	const uint64 sharedGen = theApp->sharedfiles->GetListGeneration();
	const uint64 downloadGen = theApp->downloadqueue->GetListGeneration();
	if (m_haveListGen && sharedGen == m_lastSharedGen && downloadGen == m_lastDownloadGen) {
		return;
	}

	// Stamp every encoder whose file is still listed with this epoch; the
	// sweep below then takes anything left behind. This used to build a set
	// of the live ECIDs instead, which cost a red-black-tree insertion per
	// file plus a lookup per encoder in the sweep -- on every poll, for every
	// connected client, to find the handful of files that actually came or
	// went. The stamp is a store through a pointer each loop already holds.
	const uint64 epoch = ++m_epoch;
	// Downloads
	std::vector<CPartFile *> downloads;
	theApp->downloadqueue->CopyFileList(downloads, true);
	for (uint32 i = downloads.size(); i--;) {
		uint32 id = downloads[i]->ECID();
		iterator it = find(id);
		if (it == end()) {
			CKnownFile_Encoder *enc = new CPartFile_Encoder(downloads[i]);
			enc->SetSeenEpoch(epoch);
			m_pending.emplace_back(id, enc);
			if (freshEcids) {
				freshEcids->insert(id);
			}
		} else {
			it->second->SetSeenEpoch(epoch);
		}
	}
	// Merge before the shares pass, not after both. A partfile appears in
	// both lists, and the check below asks whether this reconcile has already
	// reached it -- which it answers with find(). Leaving the new entries
	// unmerged would hide them from that lookup and build a second encoder
	// for the same ECID.
	FlushPending();
	// Shares
	std::vector<CKnownFile *> shares;
	theApp->sharedfiles->CopyFileList(shares);
	for (uint32 i = shares.size(); i--;) {
		uint32 id = shares[i]->ECID();
		iterator it = find(id);
		// Check if it is already there. Carrying this epoch means the
		// downloads loop above already reached it, which is what the old
		// `curr_files.count(id)` tested; the IsCPartFile() is just a speedup.
		if (shares[i]->IsCPartFile() && it != end() && it->second->GetSeenEpoch() == epoch) {
			it->second->SetShared();
			continue;
		}
		if (it == end()) {
			CKnownFile_Encoder *enc = new CKnownFile_Encoder(shares[i]);
			enc->SetSeenEpoch(epoch);
			m_pending.emplace_back(id, enc);
			if (freshEcids) {
				freshEcids->insert(id);
			}
		} else {
			it->second->SetSeenEpoch(epoch);
		}
	}
	FlushPending();

	// Anything still carrying an older stamp has lost its file. Delete those
	// encoders and close the gaps in one pass, preserving order. This used to
	// collect the dead into a set and then erase them one at a time, each
	// erase being a fresh lookup; compacting in place is a single sweep and
	// keeps the array contiguous for the walk that reads it next.
	iterator out = m_entries.begin();
	for (iterator it = m_entries.begin(); it != m_entries.end(); ++it) {
		if (it->second->GetSeenEpoch() != epoch) {
			delete it->second;
			continue;
		}
		if (out != it) {
			*out = *it;
		}
		++out;
	}
	m_entries.erase(out, m_entries.end());

	// Record what this pass reconciled against, so the next one can tell
	// whether anything moved. Set here rather than next to the read above so
	// an exception part-way through leaves the map looking un-reconciled and
	// the next poll redoes it.
	m_lastSharedGen = sharedGen;
	m_lastDownloadGen = downloadGen;
	m_haveListGen = true;

	// The GET_UPDATE walk relies on this order to feed the removal merge, and
	// getting it wrong loses or invents removals silently. Debug-only, and
	// rejects duplicate ECIDs as well as misordering -- a duplicate would mean
	// two encoders for one file.
	//
	// adjacent_find rather than is_sorted: is_sorted requires its comparator to
	// be a strict weak ordering, and the `<=` needed to reject equal neighbours
	// is not irreflexive. Hardened standard libraries check exactly that and
	// abort -- which would land on the debug build this change is verified
	// against. adjacent_find takes a plain binary predicate with no such
	// contract.
	assert(std::adjacent_find(
		       m_entries.begin(), m_entries.end(), [](const value_type &a, const value_type &b) {
			       return a.first >= b.first;
		       }) == m_entries.end());
}

//-------------------- CECServerSocket --------------------

class CECServerSocket : public CECMuleSocket
{
public:
	CECServerSocket(ECNotifier *notifier);
	virtual ~CECServerSocket();

	virtual const CECPacket *OnPacketReceived(const CECPacket *packet, uint32 trueSize);
	virtual void OnLost();

	virtual void WriteDoneAndQueueEmpty();

	void ResetLog() { m_LoggerAccess.Reset(); }

	// True once this connection advertised EC_TAG_CAN_CHAT — it speaks the
	// chat session ops and may be sent EC_OP_CHAT_SESSIONS replies.
	bool ChatActive() const { return m_chatActive; }

private:
	ECNotifier *m_ec_notifier;

	const CECPacket *Authenticate(const CECPacket *);

	enum
	{
		CONN_INIT,
		CONN_SALT_SENT,
		CONN_ESTABLISHED,
		CONN_FAILED
	} m_conn_state;

	uint64_t m_passwd_salt;

	// The credential this connection authenticated with -- the configured
	// EC password, or the ephemeral token the core issued to the amuleapi
	// it spawned. Per-socket precisely because two clients may be using
	// different credentials at the same time; ActivateAEAD() keys the
	// session from this rather than re-reading preferences.
	wxString m_authSecret;

	// Transport encryption, chosen during EC_OP_AUTH_REQ and only turned on
	// once the password verifies. Held here until then because a client that
	// fails authentication must never get a working key.
	uint8_t m_aeadCipher;
	// Set when the client offered encryption (CAN_AEAD + a nonce) but sent no
	// usable public key. Such a peer is provably new enough to send one, so the
	// offer is malformed and the login is refused rather than degraded to clear
	// even on a permissive daemon.
	bool m_aeadOfferMalformed;
	std::vector<uint8_t> m_aeadServerNonce;
	std::vector<uint8_t> m_aeadClientNonce;
	std::vector<uint8_t> m_aeadTranscript;

	// The X25519 shared secret, and the only thing the channel key is derived
	// from. Our ephemeral private key is wiped as soon as this exists, and this
	// is wiped once the keys are derived, so an unauthenticated peer that never
	// gets past the password check leaves nothing behind either.
	std::vector<uint8_t> m_aeadShared;

	/// Run the key exchange, pick a cipher, and add our half of the salt.
	void NegotiateAEAD(const CECPacket *request, CECPacket *response);
	/// Derive the keys and start sealing from the next packet on.
	void ActivateAEAD();
	/**
	 * Check the client's key-confirmation tag from EC_OP_AUTH_PASSWD.
	 *
	 * Separate from the password check above and not a substitute for it: that
	 * proves the client knows the credential, this proves the same client also
	 * ran the key exchange we think it did. A relay passes the first and fails
	 * this one.
	 */
	bool VerifyClientConfirm(const CECPacket *request, const wxString &secret) const;
	/// Our proof to the client, for EC_OP_AUTH_OK.
	std::vector<uint8_t> ServerConfirm(const wxString &secret) const;
	CLoggerAccess m_LoggerAccess;
	CFileEncoderMap m_FileEncoder;
	CObjTagMap m_obj_tagmap;
	CECPacket *ProcessRequest2(const CECPacket *request);

	virtual bool IsAuthorized() { return m_conn_state == CONN_ESTABLISHED; }

	// Bound on the WriteDoneAndQueueEmpty -> SendPacket -> OnOutput ->
	// WriteDoneAndQueueEmpty recursion chain that drains the notifier
	// queue. See WriteDoneAndQueueEmpty for the full reasoning.
	int m_notification_dispatch_depth;

	// EC INC_UPDATE skip-unchanged state. `m_lastEcGenSeen*` values are
	// the highest `CKnownFile::s_globalEcGen` already reflected in the
	// client's view *for that particular request path* — files with a
	// smaller `m_ecGen` did not change since the last response of that
	// path and can be skipped this cycle. Three independent counters
	// because the request paths interleave on different schedules:
	//   * `m_lastEcGenSeen`        — `EC_OP_GET_UPDATE`        (amulegui)
	//   * `m_lastEcGenSeenShared`  — `EC_OP_GET_SHARED_FILES`  (amuleweb)
	//   * `m_lastEcGenSeenPart`    — `EC_OP_GET_DLOAD_QUEUE`   (amuleweb)
	uint64 m_lastEcGenSeen;
	uint64 m_lastEcGenSeenShared;
	uint64 m_lastEcGenSeenPart;

	// Client opted in to partial-update protocol at auth time (advertised
	// `EC_TAG_CAN_PARTIAL_UPDATE`). When set, `Get_EC_Response_GetUpdate`
	// skips unchanged files entirely and emits explicit `EC_TAG_FILE_REMOVED`
	// markers for files that disappeared since the previous cycle; the
	// client mirrors this by skipping its bulk deletion loop. When *not*
	// set, the server falls back to emitting empty "alive marker" tags for
	// unchanged files so old clients (which infer deletion from absence)
	// keep working unchanged — see `Get_EC_Response_GetUpdate`.
	bool m_partialUpdateActive;

	// Client negotiated `EC_TAG_CAN_PARTIAL_SEARCH`: the multi-search results
	// union may skip results whose exported fields are unchanged and report
	// removals with explicit `EC_TAG_FILE_REMOVED` tombstones. Separate from
	// `m_partialUpdateActive` on purpose -- see EC_TAG_CAN_PARTIAL_SEARCH in
	// RemoteConnect.cpp for why reusing that flag would break an older
	// amuleGUI, which advertises it but still deletes any result absent from
	// the reply.
	bool m_partialSearchActive;
	// Client opted in to the multi-search protocol at auth time (advertised
	// `EC_TAG_CAN_MULTI_SEARCH`). When set, the EC search handlers allocate a
	// distinct daemon-side search ID per `EC_OP_SEARCH_START` (returned via
	// `EC_TAG_SEARCH_ID`) and address results/progress/stop by that ID, so the
	// client can run several concurrent searches. When *not* set, the legacy
	// single-search path runs verbatim (the `0xffffffff` sentinel bucket,
	// wipe-on-start, parameterless stop) so old clients keep working.
	bool m_multiSearchActive;
	// Set when the client advertised `EC_TAG_CAN_SEARCH_PROGRESS_UNION`: an
	// `EC_OP_SEARCH_PROGRESS` carrying no `EC_TAG_SEARCH_ID` reports every
	// search this connection could hold a tab for, one child per search,
	// instead of a single search's progress. Only consulted together with
	// `m_multiSearchActive` — a single-search client's id-less request keeps
	// meaning "the current search" (amulecmd's `search progress`).
	bool m_searchProgressUnionActive;
	// Set when the client advertised EC_TAG_CAN_CHAT: it speaks the chat
	// session ops (EC_OP_GET_CHAT_SESSIONS and friends). A client that omits
	// the tag never sees the capability echoed and must never send those
	// opcodes — an unknown opcode asserts before the EC_OP_FAILED path.
	bool m_chatActive;
	// File ECIDs sent in the previous response for each EC request path.
	// Diffed against the current snapshot to compute the removal list emitted
	// to partial-update-capable clients. Tracked per-path because amulegui
	// uses `EC_OP_GET_UPDATE` (mixed shared + partfile, served by
	// `Get_EC_Response_GetUpdate`) while amuleweb drives two separate
	// INC_UPDATE streams via `EC_OP_GET_SHARED_FILES` and
	// `EC_OP_GET_DLOAD_QUEUE` (each served by its own handler).
	//
	// `m_lastSentFileIds` is held sorted ascending in a vector rather than a
	// std::set: `Get_EC_Response_GetUpdate` walks the encoder map, which is
	// keyed by ECID, so the current IDs come out already in order and the
	// diff is a linear merge over two contiguous arrays -- where the set
	// cost a tree insertion per file to build and a tree lookup per file to
	// diff, on every poll, whether or not anything had changed.
	//
	// The other two keep the set. Their handlers iterate a CopyFileList
	// snapshot, which is neither ECID-ordered nor necessarily the whole
	// list (`queryitems` filters it), so neither the ordering the merge
	// needs nor the encoder-in-hand the mask below needs is available
	// without paying for a lookup that would cancel the saving out.
	std::vector<uint32> m_lastSentFileIds;
	std::set<uint32> m_lastSentSharedFileIds;
	std::set<uint32> m_lastSentPartFileIds;
	// `EC_OP_SEARCH_RESULTS` at `EC_DETAIL_UPDATE` (amuleweb's polling
	// path), which addresses one search at a time.
	std::set<uint32> m_lastSentSearchIds;

	// Result ECIDs last sent on the `EC_DETAIL_INC_UPDATE` union poll
	// (amulegui), so `Get_EC_Response_Search_Results_Union` can emit
	// `EC_TAG_FILE_REMOVED` for results that are gone instead of relying on
	// absence. Separate from `m_lastSentSearchIds` above: that one belongs
	// to amuleweb's per-search `EC_DETAIL_UPDATE` path and tracks a
	// different set on a different schedule.
	//
	// Only populated for clients that negotiated `EC_TAG_CAN_PARTIAL_UPDATE`.
	// A legacy client keeps the bulk "anything missing == deleted" rule, so
	// the union must keep re-sending every result to it and there is nothing
	// to track.
	std::set<uint32> m_lastSentSearchResultIds;

	// Which file ECIDs have already been sent to the client with full detail
	// (EC_DETAIL_INC_UPDATE / EC_DETAIL_UPDATE payload, not the legacy
	// childless alive-marker or the partial-update skip-silently path) now
	// lives as a flag on the encoder itself -- see
	// CKnownFile_Encoder::WasSentOnUpdatePath. It used to be three per-path
	// sets here, each costing a tree lookup per file on every poll to answer
	// a question the encoder was already in a position to answer.
	//
	// Keeping it on the encoder also bounds it. The sets were only ever
	// inserted into: an ECID whose file went away stayed in them for the life
	// of the connection, so a long-lived client on a churning library grew
	// them without limit. An encoder is destroyed with its file, and a
	// re-created one starts at zero, which is exactly the "re-send full
	// detail" state the freshEcids handling used to arrange by erasing.
	//
	// The two amuleweb paths keep their sets, for the reason given on
	// `m_lastSentSharedFileIds` above: they iterate a snapshot rather than
	// the encoder map, so reaching the encoder to read a mask would cost the
	// lookup the mask exists to avoid.
	std::set<uint32> m_sentWithDetailIdsShared;
	std::set<uint32> m_sentWithDetailIdsPart;
};

namespace
{
//! Identifies this daemon process to EC clients (EC_TAG_SESSION_ID). Random
//! rather than a counter or a pid so it cannot repeat across a restart.
uint64 GetEcSessionId()
{
	static const uint64 s_sessionId = GetRandomUint64();
	return s_sessionId;
}
} // namespace

CECServerSocket::CECServerSocket(ECNotifier *notifier)
: CECMuleSocket(true)
, m_conn_state(CONN_INIT)
, m_passwd_salt(GetRandomUint64())
, m_aeadCipher(ECCrypt::Cipher_None)
, m_aeadOfferMalformed(false)
, m_notification_dispatch_depth(0)
, m_lastEcGenSeen(0)
, m_lastEcGenSeenShared(0)
, m_lastEcGenSeenPart(0)
, m_partialUpdateActive(false)
, m_partialSearchActive(false)
, m_multiSearchActive(false)
, m_searchProgressUnionActive(false)
, m_chatActive(false)
{
	wxASSERT(theApp->ECServerHandler);
	theApp->ECServerHandler->AddSocket(this);
	m_ec_notifier = notifier;
}

CECServerSocket::~CECServerSocket()
{
	wxASSERT(theApp->ECServerHandler);
	theApp->ECServerHandler->RemoveSocket(this);
}

void CECServerSocket::NegotiateAEAD(const CECPacket *request, CECPacket *response)
{
	m_aeadCipher = ECCrypt::Cipher_None;
	m_aeadOfferMalformed = false;
	m_aeadServerNonce.clear();
	m_aeadClientNonce.clear();
	m_aeadTranscript.clear();
	m_aeadShared.clear();

	const CECTag *offer = request->GetTagByName(EC_TAG_CAN_AEAD);
	const CECTag *clientNonceTag = request->GetTagByName(EC_TAG_AEAD_CLIENT_NONCE);
	const CECTag *clientPubTag = request->GetTagByName(EC_TAG_AEAD_CLIENT_PUBKEY);
	if (offer == nullptr || clientNonceTag == nullptr) {
		// A client that predates this, or one told not to offer it.
		return;
	}
	if (clientNonceTag->GetTagDataLen() != ECCrypt::NONCE_TAG_LEN) {
		AddDebugLogLineN(logEC, "AEAD: client nonce has the wrong size, ignoring the offer");
		return;
	}
	if (clientPubTag == nullptr || clientPubTag->GetTagDataLen() != ECCrypt::X25519_KEY_LEN) {
		// Not an older client -- encryption has never shipped, so anything that
		// offers it at all is new enough to send a key. A keyless offer is
		// therefore malformed (or a middlebox that stripped the key), and the
		// login is refused in the auth decision rather than degraded to clear,
		// even on a permissive daemon.
		m_aeadOfferMalformed = true;
		AddDebugLogLineN(logEC, "AEAD: offer without a usable public key, refusing the login");
		return;
	}

	const uint8_t *offered = (const uint8_t *)offer->GetTagData();
	const uint16_t offeredLen = offer->GetTagDataLen();

	// The client prefers Cipher_ChaCha20_Poly1305, probably because it doesn't support
	// hardware AES, let's make our client happy.
	// If a third cipher is ever added, please update all the logic below and SupportedCiphers()
	// to make sure you send and select the intended ciphers
	if (offeredLen && offered[0] == ECCrypt::Cipher_ChaCha20_Poly1305 &&
		ECCrypt::IsCipherSupported(ECCrypt::Cipher_ChaCha20_Poly1305))
		m_aeadCipher = ECCrypt::Cipher_ChaCha20_Poly1305;
	else {
		// if the client didn't ask for ChaCha20, we will check our own list
		// in preference order, so take our first mutual entry.
		const std::vector<uint8_t> ours = ECCrypt::SupportedCiphers();
		for (size_t i = 0; i < ours.size() && m_aeadCipher == ECCrypt::Cipher_None; ++i) {
			for (uint16_t j = 0; j < offeredLen; ++j) {
				if (offered[j] == ours[i]) {
					m_aeadCipher = ours[i];
					break;
				}
			}
		}
		if (m_aeadCipher == ECCrypt::Cipher_None) {
			AddDebugLogLineN(logEC, "AEAD: no cipher in common with this client");
			return;
		}
	}

	m_aeadServerNonce = ECCrypt::RandomBytes(ECCrypt::NONCE_TAG_LEN);
	if (m_aeadServerNonce.empty()) {
		// Without usable randomness, stay in clear rather than derive a key
		// from a predictable salt.
		m_aeadCipher = ECCrypt::Cipher_None;
		AddDebugLogLineN(logEC, "AEAD: no randomness available, staying in clear");
		return;
	}

	// Our ephemeral half. Generated per connection and never stored: that is
	// what makes a recording of this session unreadable later, even to someone
	// who by then holds the EC password.
	std::vector<uint8_t> ephPriv;
	std::vector<uint8_t> ephPub;
	if (!ECCrypt::GenerateX25519KeyPair(ephPriv, ephPub)) {
		m_aeadCipher = ECCrypt::Cipher_None;
		m_aeadServerNonce.clear();
		AddDebugLogLineN(logEC, "AEAD: cannot generate a key pair, staying in clear");
		return;
	}

	const uint8_t *clientPubData = (const uint8_t *)clientPubTag->GetTagData();
	const std::vector<uint8_t> clientPub(clientPubData, clientPubData + clientPubTag->GetTagDataLen());

	const bool agreed = ECCrypt::X25519Agree(ephPriv, clientPub, m_aeadShared);
	// Done with the private half; wipe it here rather than letting it live in
	// this socket for the rest of the handshake.
	ECCrypt::SecureWipe(ephPriv);
	if (!agreed) {
		m_aeadCipher = ECCrypt::Cipher_None;
		m_aeadServerNonce.clear();
		m_aeadShared.clear();
		AddDebugLogLineN(logEC, "AEAD: bad client public key, staying in clear");
		return;
	}

	const uint8_t *clientNonceData = (const uint8_t *)clientNonceTag->GetTagData();
	m_aeadClientNonce.assign(clientNonceData, clientNonceData + clientNonceTag->GetTagDataLen());

	// Transcript: everything both sides exchanged, taken exactly as it reached
	// us. If any of it was edited in flight our derivation differs from the
	// client's and the first sealed packet fails, instead of the session
	// dropping to whatever the attacker preferred.
	const std::vector<uint8_t> offeredVec(offered, offered + offeredLen);
	m_aeadTranscript = ECCrypt::BuildTranscript(
		offeredVec, m_aeadCipher, m_aeadClientNonce, m_aeadServerNonce, clientPub, ephPub);

	response->AddTag(CECTag(EC_TAG_AEAD_CIPHER, m_aeadCipher));
	response->AddTag(
		CECTag(EC_TAG_AEAD_SERVER_NONCE, m_aeadServerNonce.size(), m_aeadServerNonce.data()));
	response->AddTag(CECTag(EC_TAG_AEAD_SERVER_PUBKEY, ephPub.size(), ephPub.data()));
}

namespace
{
/// The credential in the byte form both ends feed to ConfirmTag. Lower-cased
/// because the client derives from `pass.Lower()` and a hex hash that differs
/// only in case would otherwise produce a different tag on each side.
std::vector<uint8_t> CredentialBytes(const wxString &secret)
{
	const wxString lowered = secret.Lower();
	const wxCharBuffer buf = lowered.utf8_str();
	return std::vector<uint8_t>(
		(const uint8_t *)buf.data(), (const uint8_t *)buf.data() + strlen(buf.data()));
}
} // namespace

bool CECServerSocket::VerifyClientConfirm(const CECPacket *request, const wxString &secret) const
{
	const CECTag *tag = request->GetTagByName(EC_TAG_AEAD_CLIENT_CONFIRM);
	if (tag == nullptr) {
		AddDebugLogLineN(logEC, "AEAD: client sent no key confirmation");
		return false;
	}
	if (!tag->IsCustom()) {
		// GetTagData() asserts on a non-custom tag; a peer must not be able to
		// trip that in a debug build by mistyping the confirm tag.
		AddDebugLogLineN(logEC, "AEAD: client key confirmation has the wrong tag type");
		return false;
	}
	const uint8_t *data = (const uint8_t *)tag->GetTagData();
	const std::vector<uint8_t> got(data, data + tag->GetTagDataLen());
	const std::vector<uint8_t> want =
		ECCrypt::ConfirmTag(CredentialBytes(secret), m_aeadTranscript, "ec-confirm-client");
	if (!ECCrypt::ConstantTimeEquals(got, want)) {
		AddDebugLogLineN(logEC, "AEAD: client key confirmation does not match");
		return false;
	}
	return true;
}

std::vector<uint8_t> CECServerSocket::ServerConfirm(const wxString &secret) const
{
	return ECCrypt::ConfirmTag(CredentialBytes(secret), m_aeadTranscript, "ec-confirm-server");
}

void CECServerSocket::ActivateAEAD()
{
	if (m_aeadCipher == ECCrypt::Cipher_None || m_aeadShared.empty()) {
		return;
	}
	// Keyed from the ephemeral exchange and nothing else. It used to be keyed
	// from whichever credential this connection authenticated with, which is
	// what made a session recorded today readable by anyone who learns the
	// password tomorrow. The credential still has to be proved -- by the
	// password check and the confirmation tags -- but it no longer opens the
	// channel.
	if (!SetupAEAD(m_aeadCipher,
		    m_aeadShared,
		    m_aeadServerNonce,
		    m_aeadClientNonce,
		    m_aeadTranscript,
		    true)) {
		ECCrypt::SecureWipe(m_aeadShared);
		AddDebugLogLineN(logEC, "AEAD: key derivation failed, staying in clear");
		return;
	}
	// The shared secret has done its job; nothing later needs it, and keeping
	// it would put back exactly the long-lived value this change removes.
	ECCrypt::SecureWipe(m_aeadShared);
	// From the next packet on, which is EC_OP_AUTH_OK itself.
	EnableAEADNow();
	AddDebugLogLineN(logEC,
		CFormat("AEAD: %s with %s and X25519") % GetPeer() % ECCrypt::CipherName(m_aeadCipher));
}

const CECPacket *CECServerSocket::OnPacketReceived(const CECPacket *packet, uint32 trueSize)
{
	packet->DebugPrint(true, trueSize);

	const CECPacket *reply = NULL;

	if (m_conn_state == CONN_FAILED) {
		// Client didn't close the socket when authentication failed.
		AddLogLineN(_("Client sent packet after authentication failed."));
		CloseSocket();
	}

	if (m_conn_state != CONN_ESTABLISHED) {
		// This is called twice:
		// 1) send salt
		// 2) verify password
		reply = Authenticate(packet);
	} else {
		if (IsCryptReady() && !WasLastPacketEncrypted()) {
			// The session negotiated encryption, so every packet past the
			// handshake must arrive sealed. A cleartext packet here is an
			// injection attempt: our sealed replies stay confidential, but
			// executing an unauthenticated cleartext command with this
			// session's authority is not something to allow. Drop the
			// connection rather than process it. The one legitimate clear
			// packet, a terminal AUTH_FAIL, only ever arrives before
			// CONN_ESTABLISHED.
			AddDebugLogLineN(logEC, "EC: cleartext packet on an encrypted session, dropping");
			CloseSocket();
			return nullptr;
		}
		reply = ProcessRequest2(packet);
	}
	return reply;
}

void CECServerSocket::OnLost()
{
	AddLogLineN(_("External connection closed."));
	theApp->ECServerHandler->m_ec_notifier->Remove_EC_Client(this);
	DestroySocket();
}

void CECServerSocket::WriteDoneAndQueueEmpty()
{
	if (!HaveNotificationSupport() || m_conn_state != CONN_ESTABLISHED) {
		// printf("[EC] %p: WriteDoneAndQueueEmpty but notification disabled\n", this);
		return;
	}

	// CECSocket::OnOutput drains the per-socket output queue, then calls
	// WriteDoneAndQueueEmpty to pull the next notification packet. The
	// chain runs synchronously on the main thread:
	//
	//   WriteDoneAndQueueEmpty -> SendPacket -> WritePacket + OnOutput
	//     -> OnOutput drains queue -> WriteDoneAndQueueEmpty -> ...
	//
	// On a busy amuled (many peers / files generating notifications) the
	// ECNotifier always has the next packet ready, so the recursion never
	// bottoms out. The main thread stays inside this chain processing the
	// notifier feed and never yields back to the wx event loop. That
	// starves every other event -- including LibSocketLost from a
	// half-closed EC peer, which is what CECServerSocket::OnLost needs
	// to fire so it can tear the dead socket down.
	//
	// In the wedged-amuleweb scenario reported in #666, the peer is in
	// kernel CLOSE-WAIT, writes silently succeed against the dead
	// kernel buffer, and amuled spins indefinitely flushing the
	// notifier to nowhere -- amulegui can't connect because the main
	// thread is permanently occupied.
	//
	// Cap the dispatch depth so the chain returns to the event loop
	// every MAX_DEPTH packets. The pending asio LibSocketSend
	// completions (or LibSocketLost, if the peer has gone away) get
	// processed in between; on a healthy peer the loop simply resumes
	// when OnSend re-enters via the next completion.
	static const int MAX_DEPTH = 8;
	if (m_notification_dispatch_depth >= MAX_DEPTH) {
		return;
	}

	// ECNotifier::GetNextPacket returns a fresh new CECPacket(...) and
	// the caller owns it; SendPacket(const CECPacket*) only serialises
	// it into the per-socket output queue and never deletes.  Hand the
	// raw pointer to a smart pointer so the CECPacket (and its CECTag
	// tree) get freed at scope exit instead of leaking on every
	// notification dispatch.  Pre-fix, the EC notification path was
	// the dominant retained-bytes leak on long-running amuled with
	// connected amulegui / amuleweb peers (#765).
	CSmartPtr<CECPacket> packet(m_ec_notifier->GetNextPacket(this));
	if (!packet) {
		return;
	}

	m_notification_dispatch_depth++;
	try {
		SendPacket(packet.get());
	} catch (...) {
		m_notification_dispatch_depth--;
		throw;
	}
	m_notification_dispatch_depth--;
}

//-------------------- ExternalConn --------------------

ExternalConn::ExternalConn(amuleIPV4Address addr, wxString *msg)
// Defaults are ten failures a minute, then five minutes out -- deliberately
// looser than amuleapi's five, because an EC client retries on its own:
// amulegui reconnects on a dropped link, so a saved password that has gone
// stale burns attempts with no human in the loop, and a tight threshold would
// lock out someone who never typed anything. Ten still caps a guesser at nine
// attempts a minute against the thousands per second possible before this.
//
// Read once at construction, so changing them takes a restart -- the same as
// every other setting on this listener, which is rebuilt at startup anyway.
: m_authRateLimiter(CRateLimiter::Config{ thePrefs::ECAuthFailureWindowSeconds(),
	  thePrefs::ECAuthFailureThreshold(),
	  thePrefs::ECAuthLockoutSeconds() })
{
	wxString msgLocal;
	m_ECServer = NULL;
	// Are we allowed to accept External Connections?
	if (thePrefs::AcceptExternalConnections()) {
		// We must have a valid password, otherwise we will not allow EC connections
		if (thePrefs::ECPassword().IsEmpty()) {
			*msg += "External connections disabled due to empty password!\n";
			AddLogLineC(_("External connections disabled due to empty password!"));
			return;
		}

		// Create the socket
		m_ECServer = new CExternalConnListener(addr, MULE_SOCKET_REUSEADDR, this);
		m_ECServer->Notify(true);

		int port = addr.Service();
		wxString ip = addr.IPAddress();
		if (m_ECServer->IsOk()) {
			msgLocal = CFormat("*** TCP socket (ECServer) listening on %s:%d") % ip % port;
			*msg += msgLocal + "\n";
			AddLogLineN(msgLocal);
		} else {
			msgLocal = CFormat("Could not listen for external connections at %s:%d!") % ip % port;
			*msg += msgLocal + "\n";
			AddLogLineN(msgLocal);
		}
	} else {
		*msg += "External connections disabled in config file\n";
		AddLogLineN(_("External connections disabled in config file"));
	}
	m_ec_notifier = new ECNotifier();
}

ExternalConn::~ExternalConn()
{
	KillAllSockets();
	delete m_ECServer;
	delete m_ec_notifier;
}

void ExternalConn::AddSocket(CECServerSocket *s)
{
	wxASSERT(s);
	socket_list.insert(s);
}

void ExternalConn::RemoveSocket(CECServerSocket *s)
{
	wxASSERT(s);
	socket_list.erase(s);
}

void ExternalConn::KillAllSockets()
{
	AddDebugLogLineN(logGeneral,
		CFormat("ExternalConn::KillAllSockets(): %d sockets to destroy.") % socket_list.size());
	SocketSet::iterator it = socket_list.begin();
	while (it != socket_list.end()) {
		CECServerSocket *s = *(it++);
		s->Close();
		s->Destroy();
	}
	socket_list.clear();
}

void ExternalConn::ResetAllLogs()
{
	SocketSet::iterator it = socket_list.begin();
	while (it != socket_list.end()) {
		CECServerSocket *s = *(it++);
		s->ResetLog();
	}
}

CExternalConnListener::CExternalConnListener(const amuleIPV4Address &adr, int flags, ExternalConn *conn)
: CLibSocketServer(adr, flags, thePrefs::GetECNetworkInterface())
, m_conn(conn)
{
}

void CExternalConnListener::OnAccept()
{
	CECServerSocket *sock = new CECServerSocket(m_conn->m_ec_notifier);
	// Accept new connection if there is one in the pending
	// connections queue, else exit. We use Accept(FALSE) for
	// non-blocking accept (although if we got here, there
	// should ALWAYS be a pending connection).
	if (AcceptWith(*sock, false)) {
		// Apply EC keepalive on the freshly-accepted server-side
		// socket so amuled detects a half-open EC client (gui
		// process killed, network blip, FIN lost) symmetrically
		// with what the client just enabled on its end. Without
		// this, the kernel sits on the dead connection for the
		// default ~2h TCP retransmit timeout, holding the
		// CECServerSocket and its m_ec_notifier reference.
		sock->ApplyEcKeepalive();
		AddLogLineN(_("New external connection accepted"));
	} else {
		delete sock;
		AddLogLineN(_("ERROR: couldn't accept a new external connection"));
	}
}

//
// Authentication
//
const CECPacket *CECServerSocket::Authenticate(const CECPacket *request)
{
	CECPacket *response;

	if (request == NULL) {
		return new CECPacket(EC_OP_AUTH_FAIL);
	}

	// Password must be specified if we are to allow remote connections
	if (thePrefs::ECPassword().IsEmpty()) {
		AddLogLineC(_("External connection refused due to empty password in preferences!"));

		return new CECPacket(EC_OP_AUTH_FAIL);
	}

	if ((m_conn_state == CONN_INIT) && (request->GetOpCode() == EC_OP_AUTH_REQ)) {
		// cppcheck-suppress unreadVariable
		const CECTag *clientName = request->GetTagByName(EC_TAG_CLIENT_NAME);
		// cppcheck-suppress unreadVariable
		const CECTag *clientVersion = request->GetTagByName(EC_TAG_CLIENT_VERSION);

		AddLogLineN(
			CFormat(_("Connecting client: %s %s")) %
			(clientName ? clientName->GetStringData() : wxString(_("Unknown"))) %
			(clientVersion ? clientVersion->GetStringData() : wxString(_("Unknown version"))));
		const CECTag *protocol = request->GetTagByName(EC_TAG_PROTOCOL_VERSION);
#ifdef EC_VERSION_ID
		// For snapshot builds, both client and server must use GITDATE, and they must be the same
		CMD4Hash vhash;
		if (!vhash.Decode(EC_VERSION_ID)) {
			response = new CECPacket(EC_OP_AUTH_FAIL);
			response->AddTag(
				CECTag(EC_TAG_STRING, "Fatal error, version hash is not a valid MD4-hash."));
		} else if (!request->GetTagByName(EC_TAG_VERSION_ID) ||
			   request->GetTagByNameSafe(EC_TAG_VERSION_ID)->GetMD4Data() != vhash) {
			response = new CECPacket(EC_OP_AUTH_FAIL);
			response->AddTag(CECTag(EC_TAG_STRING,
				wxTRANSLATE("Incorrect EC version ID, there might be binary incompatibility. "
					    "Use core and remote from same snapshot.")));
#else
		// For release versions, we don't want to allow connections from any arbitrary snapshot
		// client.
		if (request->GetTagByName(EC_TAG_VERSION_ID)) {
			response = new CECPacket(EC_OP_AUTH_FAIL);
			response->AddTag(CECTag(EC_TAG_STRING,
				wxTRANSLATE("You cannot connect to a release version from an arbitrary "
					    "development snapshot! *sigh* possible crash prevented")));
#endif
		} else if (protocol != NULL) {
			uint16 proto_version = protocol->GetInt();
			if (proto_version == EC_CURRENT_PROTOCOL_VERSION) {
				response = new CECPacket(EC_OP_AUTH_SALT);
				response->AddTag(CECTag(EC_TAG_PASSWD_SALT, m_passwd_salt));
				m_conn_state = CONN_SALT_SENT;
				// Transport encryption. Pick the first cipher we can do from
				// the client's list -- its order is its preference -- and
				// answer with our half of the derivation salt. Keys are only
				// derived once the password checks out, below.
				NegotiateAEAD(request, response);
				//
				// So far ok, check capabilities of client
				//
				if (request->GetTagByName(EC_TAG_CAN_ZLIB)) {
					m_my_flags |= EC_FLAG_ZLIB;
				}
				// Honour the client's prefer-no-ZLIB hint: when set, the
				// client believes transit between us is fast (loopback /
				// LAN) and per-packet deflate/inflate is wasted CPU. The
				// decision lives on the client because only the client
				// knows the IP it dialed; the server's peer-IP view
				// would misclassify e.g. WireGuard tunnel endpoints as
				// "local" when the underlying transit is anything but.
				// CECSocket::WritePacket honours the resulting
				// `m_isLocalPeer` flag per packet, falling back to ZLIB
				// for payloads above `kLocalPeerZlibBypassMax` so very
				// large responses still stay inside the receiver's
				// 256 MB ReadHeader gate.
				if (request->GetTagByName(EC_TAG_PREFER_NO_ZLIB)) {
					SetLocalPeer(true);
					AddDebugLogLineN(logEC,
						CFormat("EC peer %s asked to skip ZLIB (loopback/LAN hint) - "
							"bypassing for small/medium packets") %
							GetPeer());
				}
				if (request->GetTagByName(EC_TAG_CAN_UTF8_NUMBERS)) {
					m_my_flags |= EC_FLAG_UTF8_NUMBERS;
				}
				if (request->GetTagByName(EC_TAG_CAN_LARGE_TAG_COUNT)) {
					// Client can decode the sentinel-extended children-
					// count format in CECTag::WriteChildren (#199). Only
					// new clients send this tag; old clients omit it
					// and we keep the wire format capped at uint16.
					m_my_flags |= EC_FLAG_LARGE_TAG_COUNT;
				}
				if (request->GetTagByName(EC_TAG_CAN_PARTIAL_UPDATE)) {
					// Client understands the partial-update protocol:
					// `Get_EC_Response_GetUpdate` may omit unchanged
					// files and emit explicit `EC_TAG_FILE_REMOVED`
					// markers instead of relying on absence-implies-
					// deletion. Old clients omit this tag and we keep
					// the backward-compat alive-marker path active for
					// them — see `Get_EC_Response_GetUpdate`.
					m_partialUpdateActive = true;
				}
				if (request->GetTagByName(EC_TAG_CAN_PARTIAL_SEARCH)) {
					// Client applies the same rule to search results. Old
					// clients omit this tag and the union keeps re-sending
					// every result of every open search on every poll.
					m_partialSearchActive = true;
				}
				if (request->GetTagByName(EC_TAG_CAN_MULTI_SEARCH)) {
					// Client understands the multi-search protocol: EC
					// searches are addressed by a daemon-allocated
					// `EC_TAG_SEARCH_ID` (returned on start) instead of the
					// single `0xffffffff` sentinel, so several can run at
					// once. Old clients omit this tag and keep the legacy
					// single-search sentinel path — see the search handlers.
					m_multiSearchActive = true;
				}
				if (request->GetTagByName(EC_TAG_CAN_SEARCH_PROGRESS_UNION)) {
					// Client reads an id-less EC_OP_SEARCH_PROGRESS as "every
					// open search", each reported as a child tag, so it polls
					// once instead of once per search. Only honoured together
					// with multi-search: for a single-search client an id-less
					// request still means "the current search", which is what
					// amulecmd's `search progress` with no argument expects.
					m_searchProgressUnionActive = true;
				}
				if (request->GetTagByName(EC_TAG_CAN_CHAT_SESSIONS)) {
					// Client speaks the chat session ops. Its own tag, NOT
					// EC_TAG_CAN_CHAT: that one is echoed by daemons which
					// predate these opcodes, so a client gating on it would
					// send EC_OP_GET_CHAT_SESSIONS to a core that asserts on
					// it. A client that omits this tag never sees the echo
					// and must never send those opcodes.
					m_chatActive = true;
				}
				m_haveNotificationSupport = request->GetTagByName(EC_TAG_CAN_NOTIFY) != NULL;
				AddDebugLogLineN(logEC,
					CFormat("Client capabilities: ZLIB: %s  UTF8 numbers: %s  Push "
						"notification: %s  Large tag count: %s  Partial update: %s") %
						((m_my_flags & EC_FLAG_ZLIB) ? "yes" : "no") %
						((m_my_flags & EC_FLAG_UTF8_NUMBERS) ? "yes" : "no") %
						(m_haveNotificationSupport ? "yes" : "no") %
						((m_my_flags & EC_FLAG_LARGE_TAG_COUNT) ? "yes" : "no") %
						(m_partialUpdateActive ? "yes" : "no"));
			} else {
				response = new CECPacket(EC_OP_AUTH_FAIL);
				response->AddTag(CECTag(EC_TAG_STRING,
					wxString(wxTRANSLATE("Invalid protocol version.")) +
						wxString(CFormat("( %#.4x != %#.4x )") % proto_version %
							 (uint16_t)EC_CURRENT_PROTOCOL_VERSION)));
			}
		} else {
			response = new CECPacket(EC_OP_AUTH_FAIL);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Missing protocol version tag.")));
		}
	} else if ((m_conn_state == CONN_SALT_SENT) && (request->GetOpCode() == EC_OP_AUTH_PASSWD)) {
		const CECTag *passwd = request->GetTagByName(EC_TAG_PASSWD_HASH);
		CMD4Hash passh;

		// Brute-force guard, ahead of every credential path. Keyed on the
		// address alone rather than address+port, or each reconnect would
		// look like a new client and reset the count -- which is precisely
		// what a guesser does between attempts.
		const std::string peerIp(wxString(GetIP()).ToStdString());
		const CRateLimiter::Decision throttle =
			theApp->ECServerHandler->AuthRateLimiter().Check(peerIp);
		if (throttle.locked_out) {
			// Say so explicitly instead of reusing "wrong password": the
			// client is being refused for a different reason, and a user
			// who has just fixed their password deserves to know why it
			// still fails. It tells an attacker nothing they cannot infer
			// from being refused anyway.
			const wxString err = CFormat(wxGetTranslation(wxTRANSLATE(
						     "Too many failed connection attempts; try again "
						     "in %d seconds."))) %
					     (int)throttle.retry_after_seconds;
			AddLogLineN(err + " " + GetPeer());
			response = new CECPacket(EC_OP_AUTH_FAIL);
			response->AddTag(CECTag(EC_TAG_STRING, err));
		} else if (!passh.Decode(thePrefs::ECPassword())) {
			wxString err =
				wxTRANSLATE("Authentication failed: invalid hash specified as EC password.");
			AddLogLineN(wxString(wxGetTranslation(err)) + " " + thePrefs::ECPassword());
			response = new CECPacket(EC_OP_AUTH_FAIL);
			response->AddTag(CECTag(EC_TAG_STRING, err));
		} else {
			wxString saltHash = MD5Sum(CFormat("%lX") % m_passwd_salt).GetHash();
			wxString saltStr = CFormat("%lX") % m_passwd_salt;

			passh.Decode(MD5Sum(thePrefs::ECPassword().Lower() + saltHash).GetHash());

			// Second accepted credential: the ephemeral token the core
			// issued to the amuleapi it spawned, so that daemon never
			// needs the password-equivalent value out of amule.conf.
			//
			// Empty when no token was issued, and an empty token must
			// never authenticate anyone -- so the emptiness is tested
			// here rather than relying on a digest of "" failing to
			// collide.
			const wxString &ecToken = theApp->GetEcToken();
			CMD4Hash tokenh;
			const bool tokenUsable = !ecToken.IsEmpty() &&
						 tokenh.Decode(MD5Sum(ecToken.Lower() + saltHash).GetHash());

			// Compare against both without short-circuiting. The naive
			// `if (pw) ... else if (token) ...` leaks which credential
			// matched through timing, and -- more usefully to an attacker
			// -- whether a token is live at all. Both branches are
			// evaluated and the results OR'd, so acceptance is one
			// decision.
			//
			// The per-comparison timing is not itself a concern: the
			// digests are salted per connection, so anything learned about
			// one is stale on the next, and the rate limiter above bounds
			// attempts regardless.
			const bool matchedPassword = passwd && passwd->GetMD4Data() == passh;
			const bool matchedToken = passwd && tokenUsable && passwd->GetMD4Data() == tokenh;
			const bool credentialOk = matchedPassword || matchedToken;
			// Which of the two it was. The client keys its confirmation from
			// the credential it presented, so a token-authenticated peer has
			// to be checked against the token and not the configured password.
			const wxString authSecret = matchedPassword ? thePrefs::ECPassword() : ecToken;

			// Whether that same client also ran the key exchange we completed.
			// The password check cannot tell us: a relay forwards the challenge
			// response untouched and passes it, but it has to run its own
			// exchange with each of us, so its two transcripts differ and this
			// tag cannot be right on both legs. Vacuously true on a session
			// with no encryption, where there is no exchange to confirm.
			const bool confirmOk = !credentialOk || m_aeadCipher == ECCrypt::Cipher_None ||
					       VerifyClientConfirm(request, authSecret);

			// Operator policy: refuse anything that did not negotiate
			// encryption. Checked ahead of the password so the client gets
			// the real reason rather than a misleading "wrong password",
			// and it costs an unauthenticated peer nothing it could not
			// discover by simply trying. Deliberately flat rather than
			// keyed on the peer address: only the client knows what it
			// dialed, and this side's view misclassifies tunnels.
			if (m_aeadOfferMalformed) {
				// The client offered encryption but sent no usable public key.
				// Encryption has never shipped, so a peer that offers it at all
				// is new enough to send a key: a keyless offer is malformed, or
				// an on-path attacker stripping the key, not an older client.
				// Refuse rather than let it degrade to clear even on a
				// permissive daemon.
				const wxString err = wxTRANSLATE(
					"Authentication failed: the client offered an encrypted External "
					"Connection but sent no usable key.");
				AddLogLineN(wxString(wxGetTranslation(err)) + " " + GetPeer());
				response = new CECPacket(EC_OP_AUTH_FAIL);
				response->AddTag(CECTag(EC_TAG_STRING, err));
			} else if (thePrefs::ECRequireEncryption() && m_aeadCipher == ECCrypt::Cipher_None) {
				const wxString err = wxTRANSLATE(
					"Authentication failed: this aMule requires an encrypted External "
					"Connection, and the client did not negotiate one.");
				AddLogLineN(wxString(wxGetTranslation(err)) + " " + GetPeer());
				response = new CECPacket(EC_OP_AUTH_FAIL);
				response->AddTag(CECTag(EC_TAG_STRING, err));
			} else if (credentialOk && !confirmOk) {
				// Right credential, wrong handshake. Refused rather than
				// continued in clear: a client that negotiated encryption and
				// cannot confirm it is either being relayed or is not the
				// client it claims to be, and neither deserves a session.
				ECCrypt::SecureWipe(m_aeadShared);
				const wxString err = wxTRANSLATE(
					"Authentication failed: the client could not confirm the encrypted "
					"session.");
				AddLogLineN(wxString(wxGetTranslation(err)) + " " + GetPeer());
				response = new CECPacket(EC_OP_AUTH_FAIL);
				response->AddTag(CECTag(EC_TAG_STRING, err));
			} else if (passwd && credentialOk) {
				// One of the two accepted credentials matched, so both ends
				// hold the same secret and the keys will match. Switch on now
				// rather than after the reply: EC_OP_AUTH_OK is then itself
				// sealed, which proves to the client that its peer really does
				// know the secret -- something the plain challenge never
				// established.
				//
				// Remember WHICH one for ActivateAEAD: the client keys its
				// half from the credential it presented, so keying ours from
				// the configured password regardless would break every
				// token-authenticated session the moment it was sealed.
				m_authSecret = authSecret;
				// Clear the bucket: a legitimate user who mistyped a few
				// times must not carry that streak into their next
				// connection.
				theApp->ECServerHandler->AuthRateLimiter().NoteSuccess(peerIp);
				// Computed before ActivateAEAD wipes what it derives from --
				// the tag depends only on the transcript, but keeping the
				// order explicit avoids that becoming a trap later.
				const std::vector<uint8_t> serverConfirm = ServerConfirm(authSecret);
				ActivateAEAD();
				response = new CECPacket(EC_OP_AUTH_OK);
				response->AddTag(CECTag(EC_TAG_SERVER_VERSION, VERSION));
				// Our half of the proof. Travels inside the now-sealed
				// AUTH_OK, so the client learns both that we hold the
				// credential and that we hold the key, from one packet.
				response->AddTag(CECTag(EC_TAG_AEAD_SERVER_CONFIRM,
					serverConfirm.size(),
					serverConfirm.data()));
				// Echo the negotiated large-tag-count capability so
				// the client mirrors EC_FLAG_LARGE_TAG_COUNT into its
				// own m_my_flags. Without this echo, client wouldn't
				// know the server supports the extended wire format
				// and would never set the flag in its outgoing
				// per-packet headers (#199).
				if (m_my_flags & EC_FLAG_LARGE_TAG_COUNT) {
					response->AddTag(CECEmptyTag(EC_TAG_CAN_LARGE_TAG_COUNT));
				}
				// Identifies this daemon *process*. ECIDs come from a
				// counter that restarts with the process (CECID), so
				// after a daemon restart the same numbers are handed
				// out again, in whatever order files load this time --
				// and a client that kept its objects across the
				// reconnect would pair them up by number and quietly
				// describe one file with another's data. A client that
				// sees a different value here knows its ECIDs mean
				// nothing any more and starts over. Old clients ignore
				// the tag; new clients that don't see it (old daemon)
				// fall back to starting over on every reconnect, which
				// is correct if wasteful.
				response->AddTag(CECTag(EC_TAG_SESSION_ID, GetEcSessionId()));
				if (m_partialUpdateActive) {
					// Confirm partial-update mode so the client switches
					// off its bulk "missing == deleted" fallback and
					// expects explicit `EC_TAG_FILE_REMOVED` markers.
					response->AddTag(CECEmptyTag(EC_TAG_CAN_PARTIAL_UPDATE));
				}
				if (m_partialSearchActive) {
					// Confirm the search half too. The client must not stop
					// deleting on absence until it knows the daemon actually
					// skips unchanged results -- against an older daemon that
					// never skips, absence still means the result is gone.
					response->AddTag(CECEmptyTag(EC_TAG_CAN_PARTIAL_SEARCH));
				}
				// Unconditional: this daemon answers
				// EC_OP_GET_CLIENT_HISTORY, and a client that does
				// not see the echo must not send the request --
				// against a daemon that predates it the unknown
				// opcode asserts before the EC_OP_FAILED path.
				response->AddTag(CECEmptyTag(EC_TAG_CAN_CLIENT_HISTORY));
				if (m_chatActive) {
					// Confirm the chat session ops so the client starts
					// polling EC_OP_GET_CHAT_SESSIONS.
					response->AddTag(CECEmptyTag(EC_TAG_CAN_CHAT_SESSIONS));
				}
				if (m_multiSearchActive) {
					// Confirm multi-search mode so the client addresses
					// searches by `EC_TAG_SEARCH_ID` rather than the
					// legacy single-search sentinel.
					response->AddTag(CECEmptyTag(EC_TAG_CAN_MULTI_SEARCH));
					if (m_searchProgressUnionActive) {
						// Confirm the union form of EC_OP_SEARCH_PROGRESS:
						// the client then sends one id-less request per poll
						// instead of one per open search tab. Nested inside
						// the multi-search echo on purpose -- the union
						// addresses its children by search ID, which only
						// exists in that mode.
						response->AddTag(
							CECEmptyTag(EC_TAG_CAN_SEARCH_PROGRESS_UNION));
					}
				}
				// Confirm we serve EC_OP_GET/SET_SHARED_DIRS, so a remote
				// GUI can present an editable shared-folders panel instead
				// of one whose edits it could never deliver. Unconditional:
				// unlike the flags above this needs no per-connection state,
				// the ops are always available once authenticated.
				response->AddTag(CECEmptyTag(EC_TAG_CAN_SHAREDDIRS_CONFIG));
				// Confirm we serve EC_OP_SEARCH_LIST, so a remote GUI can
				// enumerate searches it did not start itself. Unconditional,
				// for the same reason as the tag above: the op needs no
				// per-connection state. A client that gets no echo (this
				// daemon predates #680) must not send the opcode at all --
				// there is no case for it, so it would land in
				// ProcessRequest2's unknown-opcode branch and assert.
				response->AddTag(CECEmptyTag(EC_TAG_CAN_SEARCH_LIST));
			} else {
				wxString err;
				if (passwd) {
					err = wxTRANSLATE("Authentication failed: wrong password.");
				} else {
					err = wxTRANSLATE("Authentication failed: missing password.");
				}

				// Both branches are a failed credential attempt: a client
				// that omits the hash entirely is guessing just as much as
				// one that sends a wrong hash, and letting the omission go
				// uncounted would hand an attacker an unmetered path.
				theApp->ECServerHandler->AuthRateLimiter().NoteFailure(peerIp);

				response = new CECPacket(EC_OP_AUTH_FAIL);
				response->AddTag(CECTag(EC_TAG_STRING, err));
				AddLogLineN(wxGetTranslation(err));
			}
		}
	} else {
		response = new CECPacket(EC_OP_AUTH_FAIL);
		response->AddTag(
			CECTag(EC_TAG_STRING, wxTRANSLATE("Invalid request, please authenticate first.")));
	}

	if (response->GetOpCode() == EC_OP_AUTH_OK) {
		m_conn_state = CONN_ESTABLISHED;
		AddLogLineN(_("Access granted."));
		// Establish notification handler if client supports it
		if (HaveNotificationSupport()) {
			theApp->ECServerHandler->m_ec_notifier->Add_EC_Client(this);
		}
	} else if (response->GetOpCode() == EC_OP_AUTH_FAIL) {
		// Log message sent to client
		if (response->GetFirstTagSafe()->IsString()) {
			AddLogLineN(CFormat(_("Sent error message \"%s\" to client.")) %
				    wxGetTranslation(response->GetFirstTagSafe()->GetStringData()));
		}
		// Access denied!
		AddLogLineN(
			CFormat(_("Unauthorized access attempt from %s. Connection closed.")) % GetPeer());
		m_conn_state = CONN_FAILED;
	}

	return response;
}

// Make a Logger tag (if there are any logging messages) and add it to the response
// Max log lines packed into a single EC stats response. Kept bounded so a
// large first-sync backlog (a remote GUI attaching to a daemon with a big
// accumulated logfile — issue #445) drains in a handful of polls without any
// one poll hauling multiple MB: the log rides on the same response as the
// live stats, and the client renders each poll's batch in one go. The wire
// format itself imposes no such limit (ec_taglen_t is uint32 and the tag
// count is uint32-extensible) — this is purely a per-poll responsiveness cap.
static const int EC_LOG_LINES_PER_MESSAGE = 5000;

static void AddLoggerTag(CECPacket *response, CLoggerAccess &LoggerAccess)
{
	if (LoggerAccess.HasString()) {
		CECEmptyTag tag(EC_TAG_STATS_LOGGER_MESSAGE);
		// Tag structure is fix: tag carries nothing, inside are the strings.
		int entries = 0;
		wxString line;
		while (entries < EC_LOG_LINES_PER_MESSAGE && LoggerAccess.GetString(line)) {
			tag.AddTag(CECTag(EC_TAG_STRING, line));
			entries++;
		}
		response->AddTag(tag);
		// printf("send Log tag %d %d\n", FirstEntry, entries);
	}
}

static CECPacket *Get_EC_Response_StatRequest(const CECPacket *request, CLoggerAccess &LoggerAccess)
{
	CECPacket *response = new CECPacket(EC_OP_STATS);

	switch (request->GetDetailLevel()) {
	case EC_DETAIL_FULL:
	// This is not an actual INC_UPDATE.
	// amulegui only sets the detail level of the stats package to EC_DETAIL_INC_UPDATE
	// so that the included conn state tag is created the way it is needed here.
	case EC_DETAIL_INC_UPDATE:
		response->AddTag(CECTag(EC_TAG_STATS_UP_OVERHEAD, (uint32)theStats::GetUpOverheadRate()));
		response->AddTag(CECTag(EC_TAG_STATS_DOWN_OVERHEAD, (uint32)theStats::GetDownOverheadRate()));
		response->AddTag(CECTag(EC_TAG_STATS_BANNED_COUNT, /*(uint32)*/ theStats::GetBannedCount()));
		AddLoggerTag(response, LoggerAccess);
		// Needed only for the remote tray icon context menu
		response->AddTag(CECTag(EC_TAG_STATS_TOTAL_SENT_BYTES, theStats::GetTotalSentBytes()));
		response->AddTag(
			CECTag(EC_TAG_STATS_TOTAL_RECEIVED_BYTES, theStats::GetTotalReceivedBytes()));
		response->AddTag(CECTag(EC_TAG_STATS_SHARED_FILE_COUNT, theStats::GetSharedFileCount()));
		// Disk space for the Downloads and Shared Files panels. Only the
		// core can answer: the GUI may be on another machine entirely, and
		// even where it mounts the same share it can see a different size
		// or quota. Both getters are cache-backed, so a stats poll never
		// costs a filesystem round trip (Statistics.h).
		response->AddTag(CECTag(EC_TAG_STATS_TEMP_FREE_SPACE, (uint64)theStats::GetTempFreeSpace()));
		response->AddTag(
			CECTag(EC_TAG_STATS_INCOMING_FREE_SPACE, (uint64)theStats::GetIncomingFreeSpace()));
#ifdef ENABLE_VERSION_CHECK
		// Version-check result, relayed for amuleapi's /version "update"
		// object. Present only once a check has completed. OUTDATED is an
		// empty marker tag, present only when a newer release exists.
		if (theApp->IsVersionCheckDone()) {
			response->AddTag(
				CECTag(EC_TAG_GENERAL_VERSION_CHECK_LATEST, theApp->GetVersionCheckLatest()));
			response->AddTag(CECTag(EC_TAG_GENERAL_VERSION_CHECK_TIMESTAMP,
				(uint64)theApp->GetVersionCheckTimestamp()));
			if (theApp->IsVersionCheckOutdated()) {
				response->AddTag(CECEmptyTag(EC_TAG_GENERAL_VERSION_CHECK_OUTDATED));
			}
		}
#endif
	/* fall through */
	case EC_DETAIL_WEB:
	case EC_DETAIL_CMD:
		response->AddTag(CECTag(EC_TAG_STATS_UL_SPEED, (uint32)theStats::GetUploadRate()));
		response->AddTag(CECTag(EC_TAG_STATS_DL_SPEED, (uint32)(theStats::GetDownloadRate())));
		response->AddTag(
			CECTag(EC_TAG_STATS_UL_SPEED_LIMIT, (uint32)(thePrefs::GetMaxUpload() * 1024.0)));
		response->AddTag(
			CECTag(EC_TAG_STATS_DL_SPEED_LIMIT, (uint32)(thePrefs::GetMaxDownload() * 1024.0)));
		response->AddTag(
			CECTag(EC_TAG_STATS_UL_QUEUE_LEN, /*(uint32)*/ theStats::GetWaitingUserCount()));
		response->AddTag(
			CECTag(EC_TAG_STATS_TOTAL_SRC_COUNT, /*(uint32)*/ theStats::GetFoundSources()));
		// User/Filecounts
		{
			uint32 totaluser = 0, totalfile = 0;
			theApp->serverlist->GetUserFileStatus(totaluser, totalfile);
			response->AddTag(CECTag(EC_TAG_STATS_ED2K_USERS, totaluser));
			response->AddTag(
				CECTag(EC_TAG_STATS_KAD_USERS, Kademlia::CKademlia::GetKademliaUsers()));
			response->AddTag(CECTag(EC_TAG_STATS_ED2K_FILES, totalfile));
			response->AddTag(
				CECTag(EC_TAG_STATS_KAD_FILES, Kademlia::CKademlia::GetKademliaFiles()));
			response->AddTag(CECTag(EC_TAG_STATS_KAD_NODES, CStatistics::GetKadNodes()));
		}
		// Kad stats
		if (Kademlia::CKademlia::IsConnected()) {
			response->AddTag(CECTag(EC_TAG_STATS_KAD_FIREWALLED_UDP,
				Kademlia::CUDPFirewallTester::IsFirewalledUDP(true)));
			response->AddTag(CECTag(EC_TAG_STATS_KAD_INDEXED_SOURCES,
				Kademlia::CKademlia::GetIndexed()->m_totalIndexSource));
			response->AddTag(CECTag(EC_TAG_STATS_KAD_INDEXED_KEYWORDS,
				Kademlia::CKademlia::GetIndexed()->m_totalIndexKeyword));
			response->AddTag(CECTag(EC_TAG_STATS_KAD_INDEXED_NOTES,
				Kademlia::CKademlia::GetIndexed()->m_totalIndexNotes));
			response->AddTag(CECTag(EC_TAG_STATS_KAD_INDEXED_LOAD,
				Kademlia::CKademlia::GetIndexed()->m_totalIndexLoad));
			response->AddTag(CECTag(EC_TAG_STATS_KAD_IP_ADDRESS,
				wxUINT32_SWAP_ALWAYS(Kademlia::CKademlia::GetPrefs()->GetIPAddress())));
			response->AddTag(CECTag(
				EC_TAG_STATS_KAD_IN_LAN_MODE, Kademlia::CKademlia::IsRunningInLANMode()));
			response->AddTag(
				CECTag(EC_TAG_STATS_BUDDY_STATUS, theApp->clientlist->GetBuddyStatus()));
			uint32 BuddyIP = 0;
			uint16 BuddyPort = 0;
			CUpDownClient *Buddy = theApp->clientlist->GetBuddy();
			if (Buddy) {
				BuddyIP = Buddy->GetIP();
				BuddyPort = Buddy->GetUDPPort();
			}
			response->AddTag(CECTag(EC_TAG_STATS_BUDDY_IP, BuddyIP));
			response->AddTag(CECTag(EC_TAG_STATS_BUDDY_PORT, BuddyPort));
		}
	case EC_DETAIL_UPDATE:
		break;
	};

	return response;
}

// Serialise the daemon's shared-directory configuration: one EC_TAG_SHAREDDIR
// per configured root, the path as its string value, with an
// EC_TAG_SHAREDDIR_RECURSIVE subtag on the roots whose entire subtree is
// shared. Only the two *intent* lists travel — shareddir.dat is the runtime
// union (explicit + expanded recursive) that the daemon regenerates itself, so
// sending it would just invite a remote client to edit a derived artefact.
static CECPacket *Get_EC_Response_GetSharedDirs()
{
	CECPacket *response = new CECPacket(EC_OP_GET_SHARED_DIRS);

	for (const CPath &dir : theApp->glob_prefs->shareddir_explicit_list) {
		response->AddTag(CECTag(EC_TAG_SHAREDDIR, dir.GetRaw()));
	}
	for (const CPath &dir : theApp->glob_prefs->shareddir_recursive_list) {
		CECTag dirTag(EC_TAG_SHAREDDIR, dir.GetRaw());
		dirTag.AddTag(CECTag(EC_TAG_SHAREDDIR_RECURSIVE, (uint8)1));
		response->AddTag(dirTag);
	}

	return response;
}

// Replace the shared-directory configuration with the client's list, persist
// both intent files and rescan. Paths are validated here because a remote GUI
// cannot browse this host's filesystem to check them — a typo would otherwise
// become a silently dead share. Rejected paths are reported back individually
// (EC_TAG_SHAREDDIR_REJECTED + a numeric reason the client translates, so the
// daemon's locale never leaks into the user's UI); every path that *did*
// validate is still applied, so one bad entry doesn't discard the whole edit.
namespace
{

// The client's resume cursor: it already holds everything up to this id.
// Absent or 0 means "everything you still have".
uint32 ChatCursorFrom(const CECPacket *request)
{
	const CECTag *tag = request->GetTagByName(EC_TAG_CHAT_MSG_ID);
	return tag ? static_cast<uint32>(tag->GetInt()) : 0;
}

// One EC_TAG_CHAT_SESSION container: identity, plus every message newer than
// `cursor`. A session with nothing new still encodes (with no message
// children) — that is how a late-connecting client learns it exists.
CECTag EncodeChatSession(const CChatSessionStore::Session &session, uint32 cursor)
{
	CECTag tag(EC_TAG_CHAT_SESSION, session.gui_id);
	tag.AddTag(CECTag(EC_TAG_CHAT_PEER_NAME, session.name));
	tag.AddTag(CECTag(EC_TAG_CHAT_MSG_ID, session.LastMsgId()));

	// Link the live peer and the friend entry when they exist, so a client
	// can join against its own /clients and /friends views without a lookup
	// of its own. Both are omitted when absent rather than sent as 0.
	if (const CUpDownClient *client = theApp->clientlist->FindClientByIP(session.ip, session.port)) {
		tag.AddTag(CECTag(EC_TAG_CLIENT, client->ECID()));
	}
	if (const CFriend *f = theApp->friendlist->FindFriend(CMD4Hash(), session.ip, session.port)) {
		tag.AddTag(CECTag(EC_TAG_FRIEND, f->ECID()));
	}

	for (const CChatSessionStore::Message &msg : session.messages) {
		if (msg.id <= cursor) {
			continue;
		}
		CECTag msgTag(EC_TAG_CHAT_MESSAGE, msg.text);
		msgTag.AddTag(CECTag(EC_TAG_CHAT_MSG_ID, msg.id));
		msgTag.AddTag(CECTag(EC_TAG_CHAT_DIRECTION, msg.direction));
		msgTag.AddTag(CECTag(EC_TAG_CHAT_TIMESTAMP, msg.timestamp));
		tag.AddTag(msgTag);
	}
	return tag;
}

// Resolve an EC_OP_CHAT_SEND target to a GUI_ID. Three addressing modes so a
// caller can reply without a lookup (CHAT_CLIENT_ID), address a live peer it
// already has (CLIENT), or reach a friend who is currently OFFLINE (FRIEND) —
// the last one resolves through the friend's stored ip:port, which is what
// makes messaging an offline friend work at all.
bool ResolveChatTarget(const CECPacket *request, uint64 &out_gui_id)
{
	if (const CECTag *tag = request->GetTagByName(EC_TAG_CHAT_CLIENT_ID)) {
		out_gui_id = tag->GetInt();
		return out_gui_id != 0;
	}
	if (const CECTag *tag = request->GetTagByName(EC_TAG_CLIENT)) {
		const CUpDownClient *client = theApp->clientlist->FindClientByECID(tag->GetInt());
		if (!client) {
			return false;
		}
		out_gui_id = GUI_ID(client->GetIP(), client->GetUserPort());
		return true;
	}
	if (const CECTag *tag = request->GetTagByName(EC_TAG_FRIEND)) {
		CFriend *f = theApp->friendlist->FindFriend(tag->GetInt());
		if (!f || !f->GetIP() || !f->GetPort()) {
			return false;
		}
		out_gui_id = GUI_ID(f->GetIP(), f->GetPort());
		return true;
	}
	return false;
}

} // namespace

static CECPacket *Get_EC_Response_SetSharedDirs(const CECPacket *request)
{
	CECPacket *response = new CECPacket(EC_OP_SET_SHARED_DIRS);

	CPreferences::PathList explicitDirs;
	CPreferences::PathList recursiveDirs;

	for (const CECTag &tag : *request) {
		if (tag.GetTagName() != EC_TAG_SHAREDDIR) {
			continue;
		}
		const wxString rawPath = tag.GetStringData();
		const CPath path(rawPath);
		// EC_SHAREDDIR_ERR_*: 1 = missing or not a directory, 2 = unreadable.
		uint8 reason = 0;
		if (!path.IsOk() || !path.DirExists()) {
			reason = 1;
		} else if (!wxFileName::IsDirReadable(path.GetRaw())) {
			reason = 2;
		}
		if (reason != 0) {
			CECTag rejected(EC_TAG_SHAREDDIR_REJECTED, rawPath);
			rejected.AddTag(CECTag(EC_TAG_SHAREDDIR_ERROR, reason));
			response->AddTag(rejected);
			continue;
		}
		const CECTag *recursiveTag = tag.GetTagByName(EC_TAG_SHAREDDIR_RECURSIVE);
		if (recursiveTag != nullptr && recursiveTag->GetInt() != 0) {
			recursiveDirs.push_back(path);
		} else {
			explicitDirs.push_back(path);
		}
	}

	theApp->glob_prefs->shareddir_explicit_list = explicitDirs;
	theApp->glob_prefs->shareddir_recursive_list = recursiveDirs;
	// The union (shareddir.dat) has to be refreshed here too, not just the two
	// intent lists. ReloadSharedFolders reconciles against the union on disk and
	// drops any explicit root missing from it — that is how it honours external
	// edits — so leaving a stale union would make it trim the roots we just
	// added and persist the trimmed result. Seed it with the roots; the reload
	// re-expands the recursive ones and regenerates the full union.
	CPreferences::PathList unionDirs = explicitDirs;
	unionDirs.insert(unionDirs.end(), recursiveDirs.begin(), recursiveDirs.end());
	theApp->glob_prefs->shareddir_list = unionDirs;
	// Persist before reloading: FindSharedFiles starts by re-reading the
	// intent files from disk, so the in-memory lists alone wouldn't stick.
	theApp->glob_prefs->SaveSharedFolders();
	// The lists above are written and persisted synchronously, so the reply
	// still means "directories accepted and saved"; only the rescan is
	// deferred to the next Process() tick.
	theApp->sharedfiles->RequestReload();

	return response;
}

static CECPacket *Get_EC_Response_GetSharedFiles(const CECPacket *request,
	CFileEncoderMap &encoders,
	uint64 &io_lastEcGenSeen,
	bool partial_update_active,
	std::set<uint32> &io_lastSentFileIds,
	std::set<uint32> &io_sentWithDetailIds)
{
	wxASSERT(request->GetOpCode() == EC_OP_GET_SHARED_FILES);

	CECPacket *response = new CECPacket(EC_OP_SHARED_FILES);

	EC_DETAIL_LEVEL detail_level = request->GetDetailLevel();
	//
	// request can contain list of queried items
	CTagSet<uint32, EC_TAG_KNOWNFILE> queryitems(request);

	encoders.UpdateEncoders();

	// Skip-unchanged + EC_TAG_FILE_REMOVED is wired only for the
	// `EC_DETAIL_UPDATE` polling path that amuleweb uses (`EC_OP_GET_
	// SHARED_FILES` re-issued each cycle with an encoder-retained diff
	// state) and only when the client opted into the partial-update
	// protocol at auth. `EC_DETAIL_FULL` callers (amulecmd `show shared`,
	// any one-shot query) still get every alive file as a full tag.
	const bool skip_unchanged_path = partial_update_active && detail_level == EC_DETAIL_UPDATE;
	const uint64 ec_snapshot = skip_unchanged_path ? CKnownFile::GetGlobalECGen() : 0;
	const uint64 ec_threshold = io_lastEcGenSeen;

	// Snapshot the shared-file list once. GetFileByIndex() does an O(N)
	// std::advance over the underlying std::map and re-acquires list_mut
	// on every call -- looping it N times is O(N^2) and pegs the main
	// thread for tens of minutes on users with tens of thousands of
	// shared files (issue #666).
	std::vector<CKnownFile *> snapshot;
	theApp->sharedfiles->CopyFileList(snapshot);

	// Snapshot the alive set for the partial-update removal diff below.
	std::set<uint32> current_file_ids;

	for (std::vector<CKnownFile *>::const_iterator it = snapshot.begin(); it != snapshot.end(); ++it) {
		const CKnownFile *cur_file = *it;

		if (!cur_file || (!queryitems.empty() && !queryitems.count(cur_file->ECID()))) {
			continue;
		}
		const uint32 ecid = cur_file->ECID();
		if (skip_unchanged_path) {
			current_file_ids.insert(ecid);
			if (cur_file->GetECGen() <= ec_threshold && io_sentWithDetailIds.count(ecid)) {
				// Client already has the latest exported view of
				// this file; absence here is "no change", not
				// "deleted" — see `EC_TAG_FILE_REMOVED` emission
				// below. The `io_sentWithDetailIds` gate prevents
				// silently skipping ECIDs the client has never
				// received with full detail (#808-class ghost).
				continue;
			}
		}

		CEC_SharedFile_Tag filetag(cur_file, detail_level);
		CKnownFile_Encoder *enc = encoders[ecid];
		if (!enc) {
			// UpdateEncoders reads the list generations unlocked, so a file
			// added on a worker thread (PartFileConvert's import, a completing
			// partfile) between that read and this handler's own snapshot can
			// be present here with no encoder built for it yet. Skip it this
			// cycle -- it stays in current_file_ids, so it is not reported as
			// removed -- and the next poll's reconcile sees the bumped
			// generation and builds it. Dereferencing NULL here would crash
			// the daemon.
			continue;
		}
		if (detail_level != EC_DETAIL_UPDATE) {
			enc->ResetEncoder();
		}
		enc->Encode(&filetag);
		response->AddTag(filetag);
		if (skip_unchanged_path) {
			io_sentWithDetailIds.insert(ecid);
		}
	}

	if (skip_unchanged_path) {
		// One EC_TAG_FILE_REMOVED per file that was in the previous
		// response but is no longer alive on the server.
		for (std::set<uint32>::const_iterator it = io_lastSentFileIds.begin();
			it != io_lastSentFileIds.end();
			++it) {
			if (!current_file_ids.count(*it)) {
				response->AddTag(CECTag(EC_TAG_FILE_REMOVED, *it));
			}
		}
		io_lastSentFileIds.swap(current_file_ids);
		io_lastEcGenSeen = ec_snapshot;
	}
	return response;
}

/**
 * The credit store, one tag per peer we have ever exchanged data with.
 *
 * Sent whole rather than incrementally: it changes only when a peer connects
 * or disconnects, the client asks for it once when the page is opened, and
 * every other EC bulk request works the same way. Metadata fields are emitted
 * only when the store actually has them -- a record written before aMule kept
 * that information carries a hash, totals and a last-seen date and nothing
 * else, and the client renders the gaps as blanks.
 */
static CECPacket *Get_EC_Response_ClientHistory()
{
	CECPacket *response = new CECPacket(EC_OP_CLIENT_HISTORY);
	if (!theApp->clientcredits) {
		return response;
	}

	std::vector<CClientCredits *> credits;
	theApp->clientcredits->GetAllCredits(credits);
	for (const CClientCredits *cur : credits) {
		const CreditStruct *data = cur->GetDataStruct();
		CECTag entry(EC_TAG_CLIENT, data->key);
		entry.AddTag(CECTag(EC_TAG_CLIENT_UPLOAD_TOTAL, cur->GetUploadedTotal()));
		entry.AddTag(CECTag(EC_TAG_CLIENT_DOWNLOAD_TOTAL, cur->GetDownloadedTotal()));
		entry.AddTag(CECTag(EC_TAG_CLIENT_LAST_SEEN, data->nLastSeen));

		if (cur->HasMeta()) {
			const ClientMetaStruct &meta = cur->GetMeta();
			entry.AddTag(CECTag(EC_TAG_CLIENT_FIRST_SEEN, meta.firstSeen));
			entry.AddTag(CECTag(EC_TAG_CLIENT_SESSIONS, meta.sessions));
			if (!meta.name.IsEmpty()) {
				entry.AddTag(CECTag(EC_TAG_CLIENT_NAME, meta.name));
			}
			entry.AddTag(CECTag(EC_TAG_CLIENT_USER_IP, meta.lastIP));
			entry.AddTag(CECTag(EC_TAG_CLIENT_USER_PORT, meta.lastPort));
			entry.AddTag(CECTag(EC_TAG_CLIENT_KAD_PORT, meta.kadPort));
			entry.AddTag(CECTag(EC_TAG_CLIENT_SOFTWARE, meta.clientSoft));
			// The generic major.minor.update form, not the per-software
			// rendering ReGetClientSoft() produces. That one branches on
			// the client type and is built from locals inside the
			// handshake, so reproducing it here would mean either
			// duplicating it or refactoring the handshake -- and the
			// difference only shows on the few clients with a bespoke
			// format (lPhant, eMule+), in a history row.
			entry.AddTag(CECTag(EC_TAG_CLIENT_SOFT_VER_STR,
				CFormat(wxT("v%u.%u.%u")) % (meta.version / 100000) %
					((meta.version % 100000) / 1000) % ((meta.version % 1000) / 100)));
			entry.AddTag(CECTag(EC_TAG_CLIENT_FROM, meta.sourceFrom));
			entry.AddTag(CECTag(EC_TAG_CLIENT_OBFUSCATION_STATUS, meta.obfuscation));
#ifdef ENABLE_IP2COUNTRY
			// Resolved here for the same reason live peers are (see
			// CEC_UpDownClient_Tag): amulegui has no GeoIP database of its
			// own, so a country it is not told is a country it cannot show.
			// Emitted even when empty, so tag-present means "the daemon
			// looked" and tag-absent means "the daemon has no GeoIP" --
			// only for records that carry an address to look up.
			if (theApp->GetIP2Country() && theApp->GetIP2Country()->IsEnabled()) {
				entry.AddTag(CECTag(EC_TAG_CLIENT_COUNTRY,
					theApp->GetIP2Country()->GetCountryCode(meta.lastIP)));
			}
#endif
		}
		response->AddTag(entry);
	}
	return response;
}

static CECPacket *Get_EC_Response_GetUpdate(CFileEncoderMap &encoders,
	CObjTagMap &tagmap,
	uint64 &io_lastEcGenSeen,
	bool partial_update_active,
	std::vector<uint32> &io_lastSentFileIds)
{
	CECPacket *response = new CECPacket(EC_OP_SHARED_FILES);

	// Snapshot the global EC generation now. Any file whose `m_ecGen`
	// exceeds the caller's `m_lastEcGenSeen` has been touched by a
	// `MarkECChanged()` hook since the last response for this client and
	// is sent through the encoder; anything older is unchanged from the
	// client's point of view and skipped. Reading the snapshot before the
	// iteration means files that change mid-loop are picked up on the
	// next request (their `m_ecGen` will exceed our snapshot).
	const uint64 ec_snapshot = CKnownFile::GetGlobalECGen();
	const uint64 ec_threshold = io_lastEcGenSeen;

	// Freshly-created encoders this cycle. Every entry is an ECID whose
	// prior encoder was either destroyed (file dropped from m_Files_map /
	// downloadqueue) or never existed. In both cases the peer's mirrored
	// state for that ECID is empty, so any INC_UPDATE the ctor would
	// suppress against a cached value in `tagmap.GetValueMap(ecid)` produces
	// a hash-less tag that the client rejects (`amule-remote-gui.cpp` #808
	// guard). Drop the cache so the file re-appears in full detail on this
	// response. The encoder's own sent-with-detail mask needs no clearing:
	// a freshly-built encoder starts at zero by construction.
	std::set<uint32> freshEcids;
	encoders.UpdateEncoders(&freshEcids);
	for (uint32 ecid : freshEcids) {
		tagmap.EraseValueMap(ecid);
	}

	// The IDs of all files currently alive on the server, ascending -- the
	// encoder map is keyed by ECID, so iterating it below appends them in
	// order. Used by the partial-update path to diff against the previous
	// cycle and synthesize `EC_TAG_FILE_REMOVED` markers; a legacy client
	// infers removal from absence instead, so for one of those this is never
	// read and is not worth building.
	// The removal merge needs that list ascending, and it gets it for free
	// only because the encoder map is ordered by ECID. That guarantee lives on
	// the container: CFileEncoderMap keeps its entries sorted by ECID and
	// asserts that invariant at the end of every reconcile, so the ordering
	// is checked where it is established rather than assumed here.
	std::vector<uint32> current_file_ids;
	if (partial_update_active) {
		current_file_ids.reserve(encoders.size());
	}

	for (CFileEncoderMap::iterator it = encoders.begin(); it != encoders.end(); ++it) {
		const CKnownFile *cur_file = it->second->GetFile();
		const uint32 ecid = cur_file->ECID();
		if (partial_update_active) {
			current_file_ids.push_back(ecid);
		}

		if (cur_file->GetECGen() <= ec_threshold && it->second->WasSentOnUpdatePath()) {
			// Nothing exported has changed since the client's last
			// view of this file AND the client has previously
			// received this ECID with full detail. Two paths
			// depending on whether the client negotiated partial-
			// update at auth time:
			if (partial_update_active) {
				// New protocol: skip the file entirely. The client
				// only deletes when it sees an explicit
				// `EC_TAG_FILE_REMOVED` (emitted below), so absence
				// here is correctly interpreted as "no change".
				continue;
			}
			// Legacy clients (amulegui / amuleweb on master) treat
			// any file missing from the response as deleted, then
			// re-add it on the next full-sweep cycle — wedging the
			// GUI on big libraries (#713). Emit a 5-byte alive
			// marker (`EC_TAG_KNOWNFILE` / `EC_TAG_PARTFILE` with
			// the ECID and no children); the client's
			// `if (tag->HasChildTags()) ProcessItemUpdate(...)`
			// already treats childless tags as a no-op update but
			// still records the file as present.
			const ec_tagname_t tagname =
				it->second->IsPartFile_Encoder() ? EC_TAG_PARTFILE : EC_TAG_KNOWNFILE;
			response->AddTag(CECTag(tagname, ecid));
			continue;
		}
		// Fall-through path: either m_ecGen > ec_threshold (file
		// changed since the client's last view) OR the ECID is new
		// to this client. In both cases the client needs the full
		// payload — alive-markers / silent skip would produce a
		// ghost entry (#808) when the metadata never reached the
		// client.
		CValueMap &valuemap = tagmap.GetValueMap(ecid);
		// Completed cleared Partfiles are still stored as CPartfile,
		// but encoded as KnownFile, so we have to check the encoder type
		// instead of the file type.
		if (it->second->IsPartFile_Encoder()) {
			CEC_PartFile_Tag filetag(
				static_cast<const CPartFile *>(cur_file), EC_DETAIL_INC_UPDATE, &valuemap);
			// Add information if partfile is shared
			filetag.AddTag(EC_TAG_PARTFILE_SHARED, it->second->IsShared(), &valuemap);

			// `it->second` is this ECID's encoder already; looking it
			// up again by key would be a second tree descent per file.
			CPartFile_Encoder *enc = static_cast<CPartFile_Encoder *>(it->second);
			enc->Encode(&filetag);
			response->AddTag(filetag);
		} else {
			CEC_SharedFile_Tag filetag(cur_file, EC_DETAIL_INC_UPDATE, &valuemap);
			it->second->Encode(&filetag);
			response->AddTag(filetag);
		}
		it->second->MarkSentOnUpdatePath();
	}

	if (partial_update_active) {
		// Partial-update protocol: emit one `EC_TAG_FILE_REMOVED` per
		// file that was in the previous response but is no longer
		// alive on the server. Replaces the legacy client's bulk
		// "anything missing == deleted" inference.
		std::vector<uint32> removed;
		ComputeRemovedIds(io_lastSentFileIds, current_file_ids, removed);
		for (uint32 ecid : removed) {
			response->AddTag(CECTag(EC_TAG_FILE_REMOVED, ecid));
		}
		io_lastSentFileIds.swap(current_file_ids);
	}

	io_lastEcGenSeen = ec_snapshot;

	// Add clients
	CECEmptyTag clients(EC_TAG_CLIENT);
	const CClientList::IDMap &clientList = theApp->clientlist->GetClientList();
	bool onlyTransmittingClients = thePrefs::IsTransmitOnlyUploadingClients();
	for (CClientList::IDMap::const_iterator it = clientList.begin(); it != clientList.end(); ++it) {
		const CUpDownClient *cur_client = it->second.GetClient();
		if (onlyTransmittingClients && !cur_client->IsDownloading()) {
			// For poor CPU cores only transmit uploading clients. This will save a lot of CPU.
			// Set ExternalConnect/TransmitOnlyUploadingClients to 1 for it.
			continue;
		}
		CValueMap &valuemap = tagmap.GetValueMap(cur_client->ECID());
		clients.AddTag(CEC_UpDownClient_Tag(cur_client, EC_DETAIL_INC_UPDATE, &valuemap));
	}
	response->AddTag(clients);

	// Add servers
	CECEmptyTag servers(EC_TAG_SERVER);
	std::vector<const CServer *> serverlist = theApp->serverlist->CopySnapshot();
	uint32 nrServers = serverlist.size();
	for (uint32 i = 0; i < nrServers; i++) {
		const CServer *cur_server = serverlist[i];
		CValueMap &valuemap = tagmap.GetValueMap(cur_server->ECID());
		servers.AddTag(CEC_Server_Tag(cur_server, &valuemap));
	}
	response->AddTag(servers);

	// Add friends
	CECEmptyTag friends(EC_TAG_FRIEND);
	for (CFriendList::const_iterator it = theApp->friendlist->begin(); it != theApp->friendlist->end();
		++it) {
		const CFriend *cur_friend = *it;
		CValueMap &valuemap = tagmap.GetValueMap(cur_friend->ECID());
		friends.AddTag(CEC_Friend_Tag(cur_friend, &valuemap));
	}
	response->AddTag(friends);

	return response;
}

static CECPacket *Get_EC_Response_GetClientQueue(const CECPacket *request, CObjTagMap &tagmap, int op)
{
	CECPacket *response = new CECPacket(op);

	EC_DETAIL_LEVEL detail_level = request->GetDetailLevel();

	//
	// request can contain list of queried items
	// (not for incremental update of course)
	CTagSet<uint32, EC_TAG_CLIENT> queryitems(request);

	const CClientRefList &clients = theApp->uploadqueue->GetUploadingList();
	CClientRefList::const_iterator it = clients.begin();
	for (; it != clients.end(); ++it) {
		CUpDownClient *cur_client = it->GetClient();

		if (!cur_client) { // shouldn't happen
			continue;
		}
		if (!queryitems.empty() && !queryitems.count(cur_client->ECID())) {
			continue;
		}
		CValueMap *valuemap = NULL;
		if (detail_level == EC_DETAIL_INC_UPDATE) {
			valuemap = &tagmap.GetValueMap(cur_client->ECID());
		}
		CEC_UpDownClient_Tag cli_tag(cur_client, detail_level, valuemap);

		response->AddTag(cli_tag);
	}

	return response;
}

static CECPacket *Get_EC_Response_GetDownloadQueue(const CECPacket *request,
	CFileEncoderMap &encoders,
	uint64 &io_lastEcGenSeen,
	bool partial_update_active,
	std::set<uint32> &io_lastSentFileIds,
	std::set<uint32> &io_sentWithDetailIds)
{
	CECPacket *response = new CECPacket(EC_OP_DLOAD_QUEUE);

	EC_DETAIL_LEVEL detail_level = request->GetDetailLevel();
	//
	// request can contain list of queried items
	CTagSet<uint32, EC_TAG_PARTFILE> queryitems(request);

	encoders.UpdateEncoders();

	// Skip-unchanged + EC_TAG_FILE_REMOVED is wired only for the
	// `EC_DETAIL_UPDATE` polling path that amuleweb uses, and only when
	// the client opted into the partial-update protocol at auth. Other
	// callers still get every alive file as a full tag.
	const bool skip_unchanged_path = partial_update_active && detail_level == EC_DETAIL_UPDATE;
	const uint64 ec_snapshot = skip_unchanged_path ? CKnownFile::GetGlobalECGen() : 0;
	const uint64 ec_threshold = io_lastEcGenSeen;

	// Snapshot once to avoid re-locking downloadqueue's mutex on every
	// iteration (see Get_EC_Response_GetSharedFiles for the matching
	// shared-files fix in issue #666).
	std::vector<CPartFile *> snapshot;
	theApp->downloadqueue->CopyFileList(snapshot);

	std::set<uint32> current_file_ids;

	for (std::vector<CPartFile *>::const_iterator it = snapshot.begin(); it != snapshot.end(); ++it) {
		CPartFile *cur_file = *it;

		if (!queryitems.empty() && !queryitems.count(cur_file->ECID())) {
			continue;
		}
		const uint32 ecid = cur_file->ECID();
		if (skip_unchanged_path) {
			current_file_ids.insert(ecid);
			if (cur_file->GetECGen() <= ec_threshold && io_sentWithDetailIds.count(ecid)) {
				// Client already has the latest exported view of
				// this partfile; absence here is "no change",
				// not "deleted" — see `EC_TAG_FILE_REMOVED`
				// emission below. The `io_sentWithDetailIds`
				// gate prevents silently skipping ECIDs the
				// client has never received with full detail
				// (#808-class ghost).
				continue;
			}
		}

		CEC_PartFile_Tag filetag(cur_file, detail_level);

		CPartFile_Encoder *enc = static_cast<CPartFile_Encoder *>(encoders[ecid]);
		if (!enc) {
			// See the matching note in Get_EC_Response_GetSharedFiles.
			continue;
		}
		if (detail_level != EC_DETAIL_UPDATE) {
			enc->ResetEncoder();
		}
		enc->Encode(&filetag);

		response->AddTag(filetag);
		if (skip_unchanged_path) {
			io_sentWithDetailIds.insert(ecid);
		}
	}

	if (skip_unchanged_path) {
		for (std::set<uint32>::const_iterator it = io_lastSentFileIds.begin();
			it != io_lastSentFileIds.end();
			++it) {
			if (!current_file_ids.count(*it)) {
				response->AddTag(CECTag(EC_TAG_FILE_REMOVED, *it));
			}
		}
		io_lastSentFileIds.swap(current_file_ids);
		io_lastEcGenSeen = ec_snapshot;
	}
	return response;
}

// Build a CEC_SharedFile_Tag for the per-file cache. The output is
// self-contained — the encoder is reset before each Encode call in
// FULL mode (see Get_EC_Response_GetSharedFiles), so a local encoder
// suffices. Caller owns the returned tag.
static CECTag *BuildSharedFileCacheTag(const void *file_v)
{
	const CKnownFile *cur_file = static_cast<const CKnownFile *>(file_v);
	CEC_SharedFile_Tag *filetag = new CEC_SharedFile_Tag(cur_file, EC_DETAIL_FULL);
	CKnownFile_Encoder enc(cur_file);
	enc.ResetEncoder();
	enc.Encode(filetag);
	return filetag;
}

// Same shape for partfiles.
static CECTag *BuildPartFileCacheTag(const void *file_v)
{
	const CPartFile *cur_file = static_cast<const CPartFile *>(file_v);
	CEC_PartFile_Tag *filetag = new CEC_PartFile_Tag(cur_file, EC_DETAIL_FULL);
	CPartFile_Encoder enc(cur_file);
	enc.ResetEncoder();
	enc.Encode(filetag);
	return filetag;
}

// Two daemon-wide caches. Each holds one pre-serialized blob per file
// in its domain, freshness-stamped with the file's m_ecGen at build
// time. A request rebuilds only entries whose file gen has advanced
// past the cached gen — the same per-file freshness primitive the
// INC_UPDATE path uses.
static CECFullResponseCache s_sharedFilesFullCache(BuildSharedFileCacheTag);
static CECFullResponseCache s_downloadQueueFullCache(BuildPartFileCacheTag);

static CECPacket *Get_EC_Response_PartFile_Cmd(const CECPacket *request)
{
	CECPacket *response = NULL;

	// request can contain multiple files.
	for (CECPacket::const_iterator it1 = request->begin(); it1 != request->end(); ++it1) {
		const CECTag &hashtag = *it1;

		wxASSERT(hashtag.GetTagName() == EC_TAG_PARTFILE);

		CMD4Hash hash = hashtag.GetMD4Data();
		CPartFile *pfile = theApp->downloadqueue->GetFileByID(hash);

		if (!pfile) {
			AddLogLineN(CFormat(_("Remote PartFile command failed: FileHash not found: %s")) %
				    hash.Encode());
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING,
				CFormat(wxString(wxTRANSLATE("FileHash not found: %s"))) % hash.Encode()));
			// return response;
			break;
		}
		switch (request->GetOpCode()) {
		case EC_OP_PARTFILE_SWAP_A4AF_THIS:
			CoreNotify_PartFile_Swap_A4AF(pfile);
			break;
		case EC_OP_PARTFILE_SWAP_A4AF_THIS_AUTO:
			CoreNotify_PartFile_Swap_A4AF_Auto(pfile);
			break;
		case EC_OP_PARTFILE_SWAP_A4AF_OTHERS:
			CoreNotify_PartFile_Swap_A4AF_Others(pfile);
			break;
		case EC_OP_PARTFILE_PAUSE:
			pfile->PauseFile();
			break;
		case EC_OP_PARTFILE_RESUME:
			pfile->ResumeFile();
			pfile->SavePartFile();
			break;
		case EC_OP_PARTFILE_STOP:
			pfile->StopFile();
			break;
		case EC_OP_PARTFILE_PRIO_SET: {
			uint8 prio = hashtag.GetFirstTagSafe()->GetInt();
			if (prio == PR_AUTO) {
				pfile->SetAutoDownPriority(1);
			} else {
				pfile->SetAutoDownPriority(0);
				pfile->SetDownPriority(prio);
			}
		} break;
		case EC_OP_PARTFILE_DELETE:
			if (thePrefs::StartNextFile() && (pfile->GetStatus() != PS_PAUSED)) {
				theApp->downloadqueue->StartNextFile(pfile);
			}
			pfile->Delete();
			break;

		case EC_OP_PARTFILE_SET_CAT:
			pfile->SetCategory(hashtag.GetFirstTagSafe()->GetInt());
			break;

		default:
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(
				CECTag(EC_TAG_STRING, wxTRANSLATE("OOPS! OpCode processing error!")));
			break;
		}
	}
	if (!response) {
		response = new CECPacket(EC_OP_NOOP);
	}
	return response;
}

static CECPacket *Get_EC_Response_Server_Add(const CECPacket *request)
{
	CECPacket *response = NULL;

	wxString full_addr = request->GetTagByNameSafe(EC_TAG_SERVER_ADDRESS)->GetStringData();
	wxString name = request->GetTagByNameSafe(EC_TAG_SERVER_NAME)->GetStringData();

	wxString s_ip = full_addr.Left(full_addr.Find(':'));
	wxString s_port = full_addr.Mid(full_addr.Find(':') + 1);

	long port = StrToULong(s_port);
	CServer *toadd = new CServer(port, s_ip);
	toadd->SetListName(name.IsEmpty() ? full_addr : name);

	if (theApp->AddServer(toadd, true)) {
		response = new CECPacket(EC_OP_NOOP);
	} else {
		response = new CECPacket(EC_OP_FAILED);
		// wxTRANSLATE (not _()) so the wire string stays English; the API
		// contract is English text / C-locale numbers, and webapi relays this
		// verbatim. Consistent with the other EC_OP_FAILED replies here.
		response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Server not added")));
		delete toadd;
	}

	return response;
}

static CECPacket *Get_EC_Response_Server(const CECPacket *request)
{
	CECPacket *response = NULL;
	const CECTag *srv_tag = request->GetTagByName(EC_TAG_SERVER);
	CServer *srv = 0;
	if (srv_tag) {
		srv = theApp->serverlist->GetServerByIPTCP(
			srv_tag->GetIPv4Data().IP(), srv_tag->GetIPv4Data().m_port);
		// server tag passed, but server not found
		if (!srv) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING,
				CFormat(wxString(wxTRANSLATE("server not found: %s"))) %
					srv_tag->GetIPv4Data().StringIP()));
			return response;
		}
	}
	switch (request->GetOpCode()) {
	case EC_OP_SERVER_DISCONNECT:
		theApp->serverconnect->Disconnect();
		response = new CECPacket(EC_OP_NOOP);
		break;
	case EC_OP_SERVER_REMOVE:
		if (srv) {
			theApp->serverlist->RemoveServer(srv);
			response = new CECPacket(EC_OP_NOOP);
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(
				CECTag(EC_TAG_STRING, wxTRANSLATE("need to define server to be removed")));
		}
		break;
	case EC_OP_SERVER_CONNECT:
		if (thePrefs::GetNetworkED2K()) {
			if (srv) {
				theApp->serverconnect->ConnectToServer(srv);
				response = new CECPacket(EC_OP_NOOP);
			} else {
				theApp->serverconnect->ConnectToAnyServer();
				response = new CECPacket(EC_OP_NOOP);
			}
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(
				CECTag(EC_TAG_STRING, wxTRANSLATE("eD2k is disabled in preferences.")));
		}
		break;
	}
	if (!response) {
		response = new CECPacket(EC_OP_FAILED);
		response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("OOPS! OpCode processing error!")));
	}
	return response;
}

// Allocate + register a browse ("View Files") search ID in the shared EC search
// ring (defined below, after the registry). Reuses the ed2k bottom-half
// allocator: a browse is just another addressable result set, so it draws from
// the same disjoint id space and LRU eviction as a normal search — no separate
// range needed. Defined after s_ecSearches.
// The one invariant this file's kind reporting rests on: SearchType is cast
// straight to uint8 for EC_TAG_SEARCH_LIFECYCLE_KIND, so its members have to
// carry the EC_SEARCH_TYPE numbers. A comment saying so is what let a browse
// nearly be given 3, which is EC_SEARCH_WEB -- state it where the compiler
// checks it instead.
static_assert(static_cast<int>(LocalSearch) == EC_SEARCH_LOCAL,
	"SearchType and EC_SEARCH_TYPE must agree: LocalSearch");
static_assert(static_cast<int>(GlobalSearch) == EC_SEARCH_GLOBAL,
	"SearchType and EC_SEARCH_TYPE must agree: GlobalSearch");
static_assert(
	static_cast<int>(KadSearch) == EC_SEARCH_KAD, "SearchType and EC_SEARCH_TYPE must agree: KadSearch");
static_assert(static_cast<int>(BrowseSearch) == EC_SEARCH_BROWSE,
	"SearchType and EC_SEARCH_TYPE must agree: BrowseSearch");

static uint32 AllocateBrowseSearchId();

// Reply to a browse request. Multi-search clients get the allocated search ID
// (and the echoed optimistic ref) so amuleGUI can rekey its tab; legacy clients
// get the historical empty acknowledgement.
static CECPacket *BuildBrowseReply(uint32 browseId, const CECTag *reftag)
{
	if (browseId == 0) {
		return new CECPacket(EC_OP_NOOP);
	}
	// EC_OP_STRINGS with both SEARCH_ID + SEARCH_REF is exactly what the search
	// START reply sends, so amuleGUI's CSearchListRem::HandlePacket rekeys the
	// optimistic browse tab and starts polling its progress via the same path.
	CECPacket *reply = new CECPacket(EC_OP_STRINGS);
	reply->AddTag(CECTag(EC_TAG_SEARCH_ID, browseId));
	if (reftag) {
		reply->AddTag(CECTag(EC_TAG_SEARCH_REF, static_cast<uint32>(reftag->GetInt())));
	}
	return reply;
}

static CECPacket *Get_EC_Response_Friend(const CECPacket *request, bool multiSearch)
{
	CECPacket *response = NULL;
	const CECTag *tag = request->GetTagByName(EC_TAG_FRIEND_ADD);
	if (tag) {
		const CECTag *subtag = tag->GetTagByName(EC_TAG_CLIENT);
		if (subtag) {
			CUpDownClient *client = theApp->clientlist->FindClientByECID(subtag->GetInt());
			if (client) {
				theApp->friendlist->AddFriend(CCLIENTREF(
					client, "Get_EC_Response_Friend theApp->friendlist->AddFriend"));
				response = new CECPacket(EC_OP_NOOP);
			}
		} else {
			const CECTag *hashtag = tag->GetTagByName(EC_TAG_FRIEND_HASH);
			const CECTag *iptag = tag->GetTagByName(EC_TAG_FRIEND_IP);
			const CECTag *porttag = tag->GetTagByName(EC_TAG_FRIEND_PORT);
			const CECTag *nametag = tag->GetTagByName(EC_TAG_FRIEND_NAME);
			if (hashtag && iptag && porttag && nametag) {
				theApp->friendlist->AddFriend(hashtag->GetMD4Data(),
					iptag->GetInt(),
					porttag->GetInt(),
					nametag->GetStringData());
				response = new CECPacket(EC_OP_NOOP);
			}
		}
	} else if ((tag = request->GetTagByName(EC_TAG_FRIEND_REMOVE))) {
		const CECTag *subtag = tag->GetTagByName(EC_TAG_FRIEND);
		if (subtag) {
			CFriend *Friend = theApp->friendlist->FindFriend(subtag->GetInt());
			if (Friend) {
				theApp->friendlist->RemoveFriend(Friend);
			}
			// Idempotent: the desired end state of REMOVE is "friend
			// not in the list", which is already true if FindFriend
			// returned null (transient sync skew between amulegui's
			// local view and the daemon's m_FriendList). Returning
			// EC_OP_FAILED here forces the GUI into a resend / hang
			// loop on the stale ECID.
			response = new CECPacket(EC_OP_NOOP);
		}
	} else if ((tag = request->GetTagByName(EC_TAG_FRIEND_FRIENDSLOT))) {
		const CECTag *subtag = tag->GetTagByName(EC_TAG_FRIEND);
		if (subtag) {
			CFriend *Friend = theApp->friendlist->FindFriend(subtag->GetInt());
			if (Friend) {
				theApp->friendlist->SetFriendSlot(Friend, tag->GetInt() != 0);
				response = new CECPacket(EC_OP_NOOP);
			}
		}
	} else if ((tag = request->GetTagByName(EC_TAG_FRIEND_SHARED))) {
		// Browse ("View Files") over EC. For a multi-search-capable client the
		// daemon allocates a real, wire-safe search ID, registers it in the ring
		// and pins it on the target client so ProcessSharedFileList files the
		// returned listing under it; the reply carries that ID (echoing the
		// GUI's optimistic EC_TAG_SEARCH_REF) so amuleGUI rekeys its browse tab.
		// Legacy clients keep the old fire-and-forget EC_OP_NOOP (they can't
		// display a browse anyway).
		//
		// The ID is allocated per branch, once the target peer is known and
		// only if it has no browse running -- see browseInFlightId below.
		const CECTag *reftag = tag->GetTagByName(EC_TAG_SEARCH_REF);
		// A browse failure needs the correlation token for the same reason a
		// failed search start does: amuleGUI created its optimistic browse tab
		// (EnsureBrowseTab) before sending and has no other way to tell which
		// browse this verdict answers, so without the echo the tab is stranded.
		// "Client not found." is the ordinary case -- the peer gets reaped
		// between the user seeing the row and clicking View Files (got3nks,
		// PR #680 review).
		auto browseFailure = [reftag](const wxString &msg) {
			CECPacket *fail = new CECPacket(EC_OP_FAILED);
			fail->AddTag(CECTag(EC_TAG_STRING, msg));
			if (reftag) {
				fail->AddTag(
					CECTag(EC_TAG_SEARCH_REF, static_cast<uint32>(reftag->GetInt())));
			}
			return fail;
		};
		// A browse of a peer that is already being browsed joins that browse
		// instead of minting a second identity for it. CUpDownClient::
		// RequestSharedFileList() declines to re-ask a peer that is still
		// answering, so a freshly allocated ID would never be stamped with a
		// lifecycle -- while SetBrowseSearchId has already repointed every
		// later status write away from the first ID, leaving that one
		// BROWSE_IN_PROGRESS with nothing able to terminalize it. There is
		// exactly one browse per client, so there can only be one ID to
		// report on. Returns 0 when the peer has no browse running, i.e. when
		// the caller should allocate as usual.
		auto browseInFlightId = [](const CUpDownClient *peer) -> uint32 {
			return peer != nullptr ? theApp->browsemanager->SearchIdFor(peer) : 0;
		};
		// Same reply as a fresh browse, pointing at the browse that is really
		// running: no allocation (the second ID would be stranded), no
		// RegisterBrowseSearch (it would restamp m_searchStartTimes), no
		// repoint, no re-request. A legacy client still gets its historical
		// EC_OP_NOOP -- and, unlike before, no longer clears the pinned ID of
		// a multi-search client's in-flight browse on its way past.
		auto joinBrowse = [&](uint32 inFlightId) {
			return BuildBrowseReply(multiSearch ? inFlightId : 0, reftag);
		};
		const CECTag *subtag = tag->GetTagByName(EC_TAG_FRIEND);
		if (subtag) {
			CFriend *Friend = theApp->friendlist->FindFriend(subtag->GetInt());
			if (Friend) {
				// The linked client need not exist yet -- CFriendList::
				// RequestSharedFileList creates it -- and without one there
				// is by definition no browse to join.
				const CClientRef &linked = Friend->GetLinkedClient();
				const uint32 inFlight =
					browseInFlightId(linked.IsLinked() ? linked.GetClient() : nullptr);
				if (inFlight) {
					response = joinBrowse(inFlight);
				} else {
					const uint32 browseId = multiSearch ? AllocateBrowseSearchId() : 0;
					// Describe the browse before the results start arriving,
					// so the search list can report it as one (kind + peer
					// name) rather than as a nameless search of the scalar
					// kind.
					if (browseId) {
						theApp->searchlist->RegisterBrowseSearch(
							browseId, Friend->GetName(), subtag->GetInt());
					}
					theApp->friendlist->RequestSharedFileList(Friend, browseId);
					response = BuildBrowseReply(browseId, reftag);
				}
			} else {
				response = browseFailure(wxTRANSLATE("Friend not found."));
			}
		} else if ((subtag = tag->GetTagByName(EC_TAG_CLIENT))) {
			CUpDownClient *client = theApp->clientlist->FindClientByECID(subtag->GetInt());
			if (client) {
				const uint32 inFlight = browseInFlightId(client);
				if (inFlight) {
					response = joinBrowse(inFlight);
				} else {
					const uint32 browseId = multiSearch ? AllocateBrowseSearchId() : 0;
					if (browseId) {
						theApp->searchlist->RegisterBrowseSearch(
							browseId, client->GetUserName(), client->ECID());
					}
					client->SetBrowseSearchId(browseId);
					client->RequestSharedFileList();
					response = BuildBrowseReply(browseId, reftag);
				}
			} else {
				response = browseFailure(wxTRANSLATE("Client not found."));
			}
		} else {
			response = browseFailure(
				wxTRANSLATE("EC_TAG_FRIEND_SHARED requires EC_TAG_FRIEND or EC_TAG_CLIENT."));
		}
	}

	if (!response) {
		response = new CECPacket(EC_OP_FAILED);
		response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("OOPS! OpCode processing error!")));
	}
	return response;
}

namespace
{
// Global multi-search registry, shared across all EC connections (EC runs
// synchronously on the main thread, so no locking is needed). Bounds the
// daemon's retained EC searches to kMaxEcSearches, evicting the
// least-recently-touched — which also stops a still-running Kad search via
// RemoveResults. Legacy (non-multi) clients bypass this entirely and keep
// using the single 0xffffffff sentinel bucket.
constexpr std::size_t kMaxEcSearches = 20;

class CEcSearchRegistry
{
public:
	// Register a just-started search as most-recently-used and current,
	// evicting the least-recently-used if over capacity.
	void Register(uint32 id)
	{
		Touch(id);
		m_current = id;
		while (m_lru.size() > kMaxEcSearches) {
			uint32 victim = m_lru.back();
			m_lru.pop_back();
			// Frees the result bucket and stops a still-running Kad search.
			theApp->searchlist->RemoveResults(victim);
			if (m_current == victim) {
				m_current = 0;
			}
		}
	}

	// Move an accessed search to most-recently-used.
	void Touch(uint32 id)
	{
		m_lru.remove(id);
		m_lru.push_front(id);
	}

	bool Has(uint32 id) const { return std::find(m_lru.begin(), m_lru.end(), id) != m_lru.end(); }

	// Drop id from the ring/current without touching core search state.
	// For a search this registry tracks, call alongside the caller's own
	// (unconditional) CSearchList::RemoveResults, instead of Close(), when
	// the id may or may not be one this registry knows about.
	void Forget(uint32 id)
	{
		m_lru.remove(id);
		if (m_current == id) {
			m_current = 0;
		}
	}

	// Most-recently-started search (0 = none); the no-arg default target.
	uint32 Current() const { return m_current; }

	// All active search IDs (MRU order). Used by the INC_UPDATE union poll
	// so amulegui's single result container holds every open search at once.
	const std::list<uint32> &ActiveIds() const { return m_lru; }

private:
	std::list<uint32> m_lru; // front = most-recently-used
	uint32 m_current = 0;
};

CEcSearchRegistry s_ecSearches;
} // namespace

void RegisterRestoredSearch(uint32 searchID)
{
	s_ecSearches.Register(searchID);
}

static uint32 AllocateBrowseSearchId()
{
	uint32 id = theApp->searchlist->AllocateEd2kId();
	s_ecSearches.Register(id);
	return id;
}

static CECPacket *Get_EC_Response_Search_Results(const CECPacket *request,
	bool partial_update_active,
	std::set<uint32> &io_lastSentSearchIds,
	wxUIntPtr searchID)
{
	CECPacket *response = new CECPacket(EC_OP_SEARCH_RESULTS);

	EC_DETAIL_LEVEL detail_level = request->GetDetailLevel();
	//
	// request can contain list of queried items
	CTagSet<uint32, EC_TAG_SEARCHFILE> queryitems(request);

	// `EC_TAG_FILE_REMOVED` tombstoning is wired only for the
	// `EC_DETAIL_UPDATE` polling path that amuleweb uses, and only when
	// the client opted into the partial-update protocol at auth. The
	// `EC_DETAIL_INC_UPDATE` dispatch (amulegui) takes the tagmap
	// overload below; `EC_DETAIL_FULL` callers (amulecmd `search` and
	// amuleweb's Phase-3 follow-up `req_full`, which defaults to FULL)
	// remain unchanged. The `queryitems.empty()` check excludes
	// per-ID subset queries: tombstoning is meaningful only when the
	// client is polling the whole set.
	const bool tombstone_path =
		partial_update_active && detail_level == EC_DETAIL_UPDATE && queryitems.empty();

	// Result grouping (issue #431): a caller that wants the same-hash/
	// same-size-but-different-filename children (the expandable tree the
	// GUI shows) opts in by adding an empty `EC_TAG_SEARCH_PARENT` flag
	// to the request. Without it this path stays parents-only, so
	// amulecmd `search` and amuleweb are unchanged. Children carry their
	// parent's ECID via `EC_TAG_SEARCH_PARENT` in CEC_SearchFile_Tag, so
	// the client can rebuild the tree.
	const bool want_children = request->GetTagByName(EC_TAG_SEARCH_PARENT) != nullptr;

	std::set<uint32> current_ids;

	const CSearchResultList &list = theApp->searchlist->GetSearchResults(searchID);
	CSearchResultList::const_iterator it = list.begin();
	while (it != list.end()) {
		CSearchFile *sf = *it++;
		if (!queryitems.empty() && !queryitems.count(sf->ECID())) {
			continue;
		}
		if (tombstone_path) {
			current_ids.insert(sf->ECID());
		}
		response->AddTag(CEC_SearchFile_Tag(sf, detail_level));
		if (want_children && sf->HasChildren()) {
			for (CSearchFile *sfc : sf->GetChildren()) {
				if (tombstone_path) {
					current_ids.insert(sfc->ECID());
				}
				response->AddTag(CEC_SearchFile_Tag(sfc, detail_level));
			}
		}
	}

	if (tombstone_path) {
		// One `EC_TAG_FILE_REMOVED` per result that was in the previous
		// response but is no longer in the daemon's searchlist —
		// typically because the user started a new search, which clears
		// the list via `EC_OP_SEARCH_START` → `searchlist->RemoveResults`.
		// Without these markers, amuleweb's
		// `UpdatableItemsContainer::ProcessUpdate` takes the partial-
		// update branch (it expects explicit deletions) and sees no
		// signal to drop the old results, so they accumulate across
		// searches (#31, regression from ee1d92b75).
		for (std::set<uint32>::const_iterator i = io_lastSentSearchIds.begin();
			i != io_lastSentSearchIds.end();
			++i) {
			if (!current_ids.count(*i)) {
				response->AddTag(CECTag(EC_TAG_FILE_REMOVED, *i));
			}
		}
		io_lastSentSearchIds.swap(current_ids);
	}
	return response;
}

static CECPacket *Get_EC_Response_Search_Results(CObjTagMap &tagmap, wxUIntPtr searchID)
{
	CECPacket *response = new CECPacket(EC_OP_SEARCH_RESULTS);

	const CSearchResultList &list = theApp->searchlist->GetSearchResults(searchID);
	CSearchResultList::const_iterator it = list.begin();
	while (it != list.end()) {
		CSearchFile *sf = *it++;
		CValueMap &valuemap = tagmap.GetValueMap(sf->ECID());
		response->AddTag(CEC_SearchFile_Tag(sf, EC_DETAIL_INC_UPDATE, &valuemap));
		// Add the children
		if (sf->HasChildren()) {
			const CSearchResultList &children = sf->GetChildren();
			for (size_t i = 0; i < children.size(); ++i) {
				CSearchFile *sfc = children.at(i);
				CValueMap &valuemap1 = tagmap.GetValueMap(sfc->ECID());
				response->AddTag(CEC_SearchFile_Tag(sfc, EC_DETAIL_INC_UPDATE, &valuemap1));
			}
		}
	}
	return response;
}

// Multi-search INC_UPDATE union poll (amulegui): amulegui runs every open
// search through a single result container, so emit the results of *all* active
// searches in one reply, tagging each with its EC_TAG_SEARCH_ID. The client
// routes each result to the right tab by that ID (mirrors the monolithic GUI,
// which demuxes by search ID), and its container's bulk-delete-on-poll works
// correctly across the union. Only reached for m_multiSearchActive clients.
//
// Enumerates CSearchList::GetKnownSearchIds() -- every search AND "View Files"
// browse tab the core holds, started by the monolithic GUI or by any EC client
// -- rather than s_ecSearches.ActiveIds(),
// which only ever holds EC-initiated searches (Register() is called from
// exactly one place, the EC_OP_SEARCH_START handler). A monolithic-started
// search's results would otherwise never reach amulegui even once
// Get_EC_Response_Search_List (below) learned to enumerate it: the two have
// to agree on the same set, or a discovered tab appears and never fills.
// Browses need their own second source here (but NOT in
// Get_EC_Response_Search_List -- see that function) since a browse tab is
// not a CSearchList search and so is absent from m_searchStrings; without
// it, a browse's own STRINGS reply (BuildBrowseReply) tells the client to
// expect results on this id, but this union never sends any (got3nks, PR
// #680 review). s_ecSearches keeps governing EC-client lifecycle and
// eviction only -- folding monolithic searches into that 20-entry LRU would
// let unrelated EC traffic evict, and so stop, a local user's own
// still-running Kad search.
static CECPacket *Get_EC_Response_Search_Results_Union(
	CObjTagMap &tagmap, bool partial_update_active, std::set<uint32> &io_lastSentResultIds)
{
	CECPacket *response = new CECPacket(EC_OP_SEARCH_RESULTS);
	// Incremental: unchanged fields are diffed out via the per-connection
	// valuemap (keyed by the globally-unique ECID). Safe now that amulegui's
	// container retains its items across searches (it no longer flushes on a
	// new search), so a result is never re-created from a diffed tag — no
	// ghosts.
	//
	// Every result the client is currently believed to hold, so the
	// partial-update path below can synthesize removals by diffing against
	// the previous cycle instead of relying on absence.
	std::set<uint32> current_ids;

	auto emitOne = [&](CSearchFile *sf, uint32 sid) {
		const uint32 ecid = sf->ECID();
		current_ids.insert(ecid);
		const bool known = io_lastSentResultIds.count(ecid) != 0;
		CValueMap &valuemap = tagmap.GetValueMap(ecid);
		// The owning search ID never changes for a result, so it only has to
		// travel once per connection -- but only once removal is explicit.
		// A legacy client deletes anything missing from the reply and would
		// then re-create the item from a later diffed tag, which must still
		// carry the ID to be attributable to a tab.
		const uint32 attribute_sid = (partial_update_active && known) ? 0 : sid;
		CEC_SearchFile_Tag tag(sf, EC_DETAIL_INC_UPDATE, &valuemap, attribute_sid);
		if (partial_update_active && known && !tag.HasChildTags()) {
			// Nothing about this result changed since the client's last view.
			// Absence no longer implies deletion for this client (it deletes
			// only on an explicit EC_TAG_FILE_REMOVED emitted below), so the
			// whole tag can go. This is what makes an idle search cost
			// nothing: previously every result re-sent its envelope plus its
			// search ID on every poll, forever.
			return;
		}
		response->AddTag(tag);
	};

	auto emitResultsFor = [&](uint32 sid) {
		const CSearchResultList &list = theApp->searchlist->GetSearchResults(sid);
		for (CSearchFile *sf : list) {
			emitOne(sf, sid);
			if (sf->HasChildren()) {
				for (CSearchFile *sfc : sf->GetChildren()) {
					emitOne(sfc, sid);
				}
			}
		}
	};
	// GetKnownSearchIds() covers browses too: RequestSharedFileList registers
	// every one of them, by either route, before the request goes out, and
	// RemoveResults drops the registration and the browse together. The second
	// source this loop used to have emitted each browse a second time.
	for (const auto &entry : theApp->searchlist->GetKnownSearchIds()) {
		emitResultsFor(entry.first);
	}

	if (partial_update_active) {
		// One EC_TAG_FILE_REMOVED per result the client was told about that
		// the core no longer holds -- a closed or evicted search, or a
		// results list replaced by a new search on the same ID. Reuses the
		// knownfile tombstone rather than inventing a second one: the
		// meaning ("this ECID is gone") and the payload (the ECID) are
		// identical, and the search container reads it the same way.
		for (uint32 id : io_lastSentResultIds) {
			if (!current_ids.count(id)) {
				response->AddTag(CECTag(EC_TAG_FILE_REMOVED, id));
				// Drop the diff state with the result. ECIDs are handed out
				// monotonically so reuse is not expected, but a stale
				// valuemap would diff away the very fields a re-created
				// result needs, leaving the client a tag it cannot use --
				// the same hazard `freshEcids` guards on the file path.
				tagmap.EraseValueMap(id);
			}
		}
		io_lastSentResultIds.swap(current_ids);
	}
	return response;
}

// Enumerates every search the core currently holds
// (CSearchList::GetKnownSearchIds()) so a client that never started any of
// them locally -- a freshly (re)connected amulegui, a stateless amuleapi
// request, or a search typed directly into the monolithic GUI -- can discover
// what to ask about and build tabs for it. One entry per known search:
// EC_TAG_SEARCH_ID as the entry's own value, with name/kind/state as children.
// Only reached for m_multiSearchActive clients (see the EC_OP_SEARCH_LIST case
// below): a legacy client has no concept of more than the single 0xffffffff
// sentinel search, so there is nothing meaningful to enumerate for it.
//
// Browses are enumerated here too. This reverses PR #680, which kept them out
// on the grounds that a browse is inherently per-request and could only
// surface as a bogus, nameless search tab: PR #914 gave a browse a name, a
// recorded kind and its peer's ECID (RegisterBrowseSearch), which is exactly
// what lets a remote GUI rebuild it as a browse tab instead
// (CSearchListRem::HandlePacket in amule-remote-gui.cpp branches on
// BrowseSearch + EC_TAG_CLIENT), and made a local browse discoverable at all.
// RegisterBrowseSearch writing m_searchStrings is what puts them in reach of
// GetKnownSearchIds(); it is deliberate, not incidental.
// amuleapi's SearchKindToString/SearchLifecycleStateToString (Api.cpp) decode
// the two wire values written below from raw numeric literals, since amuleapi
// cannot include SearchList.h. This file can see both CSearchList's enums and
// the EC ones, so it is the one place that can catch a reorder at compile
// time instead of amuleapi silently mislabelling a search.
static_assert(CSearchList::SEARCH_LIFECYCLE_IDLE == 0 && CSearchList::SEARCH_LIFECYCLE_RUNNING == 1 &&
		      CSearchList::SEARCH_LIFECYCLE_FINISHED == 2,
	"CSearchList::SearchLifecycleState numeric values must stay in sync with "
	"the wire values EC_TAG_SEARCH_LIFECYCLE_STATE carries and Api.cpp's "
	"SearchLifecycleStateToString decodes");
static_assert(static_cast<int>(LocalSearch) == static_cast<int>(EC_SEARCH_LOCAL) &&
		      static_cast<int>(GlobalSearch) == static_cast<int>(EC_SEARCH_GLOBAL) &&
		      static_cast<int>(KadSearch) == static_cast<int>(EC_SEARCH_KAD),
	"CSearchList::SearchType must stay numerically aligned with EC_SEARCH_TYPE "
	"since EC_TAG_SEARCH_LIFECYCLE_KIND carries a SearchType value that "
	"amuleapi's SearchKindToString (Api.cpp) decodes against EC_SEARCH_*");

static CECPacket *Get_EC_Response_Search_List()
{
	CECPacket *response = new CECPacket(EC_OP_SEARCH_LIST);

	for (const auto &known : theApp->searchlist->GetKnownSearchIds()) {
		uint32 sid = known.first;
		CECTag entry(EC_TAG_SEARCH_ID, sid);
		// known.second is the same string GetSearchStringById(sid) would
		// look up -- already have it from this map entry, no need to re-find.
		entry.AddTag(EC_TAG_SEARCH_NAME, known.second);
		// A browse also carries the peer it is listing: without it a remote
		// GUI cannot build a browse tab at all, since a tab with no ecid is
		// by definition not a browse (CSearchListCtrl::IsBrowse).
		const uint32 browsePeer = theApp->searchlist->GetBrowsePeerEcid(sid);
		if (browsePeer) {
			entry.AddTag(CECTag(EC_TAG_CLIENT, browsePeer));
		}
		entry.AddTag(EC_TAG_SEARCH_LIFECYCLE_KIND,
			static_cast<uint64_t>(
				static_cast<uint8>(theApp->searchlist->GetSearchLifecycleKindById(sid))));
		entry.AddTag(EC_TAG_SEARCH_LIFECYCLE_STATE,
			static_cast<uint64_t>(
				static_cast<uint8>(theApp->searchlist->GetSearchLifecycleStateById(sid))));
		// How many hits this search is holding. A client that adopts the whole
		// listing and fetches results per tab on activation has no other way to
		// label a tab it has not opened yet -- and eagerly fetching every
		// search's results to learn one integer each is the thing the lazy
		// fetch exists to avoid. Same tag, same number, as the progress and
		// results replies already carry (an O(1) map lookup on the index).
		entry.AddTag(CECTag(EC_TAG_SEARCH_RESULT_COUNT,
			static_cast<uint32>(theApp->searchlist->GetSearchResults(sid).size())));
		// And the percent, so a client discovering a *running* search reports
		// the daemon's real ramp from first sight rather than 0 until the next
		// poll. Both tags are already allocated and already emitted on the
		// per-id progress reply; this listing was simply the one reply that
		// left them out.
		entry.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_PERCENT,
			theApp->searchlist->GetSearchLifecyclePercentById(sid)));
		response->AddTag(entry);
	}

	return response;
}

// Emit one search's progress into `out`: the reply packet itself for a request
// naming a single `EC_TAG_SEARCH_ID`, or one child entry per search for the
// union form below. Both callers share this function so the two shapes cannot
// drift -- whatever a per-id poll reports is exactly what a union child
// reports, which is what lets a client switch between them without any
// second decode path.
//
// A browse ("View Files") ID is not a CSearchList search: report its lifecycle
// from the persisted browse state (browsing / finished / failed) + bar, keyed
// by search ID, plus the running result count so amuleGUI's tab marker and hit
// count update. Reading the persisted state -- rather than the browsing client,
// which is transient and, for a browse that fails on disconnect, reaped before
// the next poll -- means the terminal "failed" still reaches amuleGUI so its
// tab marker flips instead of sticking at "browsing".
// EC_TAG_SEARCH_BROWSE_STATUS is the discriminator the GUI branches on before
// the normal progress decode.
//
// EC_TAG_SEARCH_STATUS MUST be added first in both branches: the GUI reads the
// reply's (or the entry's) first tag via GetFirstTagSafe.
static void AppendSearchProgress(CECTag &out, wxUIntPtr sid)
{
	if (theApp->browsemanager->Has(static_cast<uint32>(sid))) {
		const browse::State bstate = theApp->browsemanager->StateOf(static_cast<uint32>(sid));
		const uint8 browseStatus =
			static_cast<uint8>(bstate == browse::State::InProgress ? BROWSE_IN_PROGRESS
					   : bstate == browse::State::Finished ? BROWSE_FINISHED
									       : BROWSE_FAILED);
		// Bar value (0..100 running, 0xffff done/failed) so amuleGUI drives
		// the browse tab's gauge via the same UpdateSearchProgress path as
		// a search.
		const uint16 bar = static_cast<uint16>(theApp->searchlist->GetSearchBarStatusById(sid));
		out.AddTag(CECTag(EC_TAG_SEARCH_STATUS, bar));
		out.AddTag(CECTag(EC_TAG_SEARCH_BROWSE_STATUS, browseStatus));
		out.AddTag(CECTag(EC_TAG_SEARCH_ID, static_cast<uint32>(sid)));
		out.AddTag(CECTag(EC_TAG_SEARCH_RESULT_COUNT,
			static_cast<uint32>(theApp->searchlist->GetSearchResults(sid).size())));
		// Also emit the standard lifecycle tags (mapped from the browse
		// status) so amuleapi / amuleweb consume a browse through their
		// existing SEARCH_PROGRESS handling with no special-casing:
		// browsing -> RUNNING, finished/failed -> FINISHED. Percent is the
		// dir-based bar value (0..100), snapped to 100 once terminal.
		const bool browsing = browseStatus == BROWSE_IN_PROGRESS;
		out.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_STATE,
			static_cast<uint8>(browsing ? CSearchList::SEARCH_LIFECYCLE_RUNNING
						    : CSearchList::SEARCH_LIFECYCLE_FINISHED)));
		out.AddTag(CECTag(
			EC_TAG_SEARCH_LIFECYCLE_PERCENT, static_cast<uint8>(bar == 0xffff ? 100 : bar)));
		return;
	}
	// Per-ID lifecycle. STATE / PERCENT / RESULT_COUNT are addressed by the
	// search ID.
	const CSearchList::SearchLifecycleState st = theApp->searchlist->GetSearchLifecycleStateById(sid);
	const uint8 pct = theApp->searchlist->GetSearchLifecyclePercentById(sid);
	// EC_TAG_SEARCH_STATUS: the overloaded sentinel the GUI decodes in
	// Search_Update_Progress — a finished Kad search reports 0xfffe (clears the
	// "!" marker + resets the bar), a finished ed2k search 0xffff, otherwise the
	// running percent. Shared with the monolithic bar via GetSearchBarStatusById.
	out.AddTag(CECTag(EC_TAG_SEARCH_STATUS, theApp->searchlist->GetSearchBarStatusById(sid)));
	// Echo the ID so the client can confirm which search this is for.
	out.AddTag(CECTag(EC_TAG_SEARCH_ID, static_cast<uint32>(sid)));
	out.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_STATE, static_cast<uint8>(st)));
	// Per-id kind (not the scalar): a multi-search client polls each tab by id
	// and needs THIS search's real type, e.g. to enable the Kad-only "More"
	// button on the right tab.
	out.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_KIND,
		static_cast<uint8>(theApp->searchlist->GetSearchLifecycleKindById(sid))));
	out.AddTag(CECTag(EC_TAG_SEARCH_RESULT_COUNT,
		static_cast<uint32>(theApp->searchlist->GetSearchResults(sid).size())));
	out.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_PERCENT, pct));
}

// Progress for every search this connection could hold a tab for, one child
// per search, so a client with N open tabs polls once instead of N times.
//
// Enumerates searches *and* browse ids -- unlike Get_EC_Response_Search_List,
// which is deliberately narrow because it drives tab *discovery* and folding
// browses in there would invent a bogus search tab per open browse. Nothing is
// discovered here: a client only ever looks up ids it already has tabs for, so
// including browses is exactly right and is what lets a "View Files" tab share
// the one poll.
//
// There is no EC_TAG_SEARCH_EXPIRED in the union. The per-id form needs it
// because a reply about one id is otherwise indistinguishable from silence;
// here the whole set is present, so a client treats any tab whose id is absent
// as expired -- which also catches an LRU eviction the per-id form only
// notices when it happens to poll that id.
static CECPacket *Get_EC_Response_Search_Progress_Union(const CECPacket *request)
{
	CECPacket *response = new CECPacket(EC_OP_SEARCH_PROGRESS);

	// The ids the client is actually tracking, when it names them. It costs
	// nothing to carry -- they ride in the one request either way -- and it
	// keeps the LRU meaning exactly what it meant before: Touch marks the
	// searches a client still has open. Touching everything the daemon holds
	// instead would make an abandoned search as protected as a live tab, so the
	// search evicted by an overflowing ring stops being the least-used one.
	std::vector<uint32> wanted;
	for (const CECTag &tag : *request) {
		if (tag.GetTagName() == EC_TAG_SEARCH_ID) {
			wanted.push_back(static_cast<uint32>(tag.GetInt()));
		}
	}

	auto emitOne = [&](wxUIntPtr sid) {
		// Same guard as the per-id path: Touch() on an id the registry does
		// not know would silently insert it, growing the ring past
		// kMaxEcSearches for ids Register() never admitted.
		if (s_ecSearches.Has(static_cast<uint32>(sid))) {
			s_ecSearches.Touch(static_cast<uint32>(sid));
		}
		CECTag entry(EC_TAG_SEARCH_ID, static_cast<uint32>(sid));
		AppendSearchProgress(entry, sid);
		response->AddTag(entry);
	};

	if (!wanted.empty()) {
		// Answer about the ids the client named, resolving each one exactly as
		// the per-id form does. Deliberately NOT a walk of the daemon's own
		// maps filtered by these ids: a Kad search is addressed by an id with
		// the high bit set (0x80000001...), which is not the key those maps are
		// stored under, so filtering silently dropped every Kad search from the
		// reply and the client read that absence as an expiry.
		for (uint32 want : wanted) {
			// The gate the per-id path uses -- the core's own knowledge, which
			// covers browse ids too (see IsKnownSearchId). An id that fails it
			// is left out, and its absence is how the client learns it expired.
			if (want == 0 || !theApp->searchlist->IsKnownSearchId(want)) {
				continue;
			}
			emitOne(want);
		}
		return response;
	}

	// No ids named: report everything the daemon holds. Used by a stateless
	// caller that has no tracked set to enumerate.
	// Browses included; see the union poll above.
	for (const auto &known : theApp->searchlist->GetKnownSearchIds()) {
		emitOne(known.first);
	}

	return response;
}

static CECPacket *Get_EC_Response_Search_Results_Download(const CECPacket *request)
{
	CECPacket *response = new CECPacket(EC_OP_STRINGS);
	for (CECPacket::const_iterator it = request->begin(); it != request->end(); ++it) {
		const CECTag &tag = *it;
		// Read the category by name (both amulegui and amuleapi send it as
		// EC_TAG_PARTFILE_CAT) rather than "first child", so an optional
		// EC_TAG_SEARCHFILE selector below can ride alongside it.
		uint8 category = 0;
		if (const CECTag *catTag = tag.GetTagByName(EC_TAG_PARTFILE_CAT)) {
			category = catTag->GetInt();
		}
		// Issue #431: an optional EC_TAG_SEARCHFILE child carries a search
		// result's ECID, selecting one specific same-hash/different-name
		// grouped result so it downloads under that chosen filename.
		// Without it, download the first result matching the hash (the
		// parent) — the unchanged behaviour.
		if (const CECTag *ecidTag = tag.GetTagByName(EC_TAG_SEARCHFILE)) {
			theApp->searchlist->AddFileToDownloadByEcid(ecidTag->GetInt(), category);
		} else {
			theApp->searchlist->AddFileToDownloadByHash(tag.GetMD4Data(), category);
		}
	}
	return response;
}

static CECPacket *Get_EC_Response_Search_Stop(const CECPacket *request, bool multiSearch)
{
	CECPacket *reply = new CECPacket(EC_OP_MISC_DATA);
	if (multiSearch) {
		// Per-ID stop. No ID => the most-recently-started search. Gate on the
		// core's own knowledge (CSearchList::IsKnownSearchId), not
		// s_ecSearches: a monolithic-started search is known to the core but
		// was never Register()'d into that EC-only registry, so gating on
		// the registry alone silently no-ops both Stop and Close for it --
		// the search never actually goes away and reappears as a tab on the
		// next connect.
		const CECTag *idTag = request->GetTagByName(EC_TAG_SEARCH_ID);
		uint32 sid = idTag ? static_cast<uint32>(idTag->GetInt()) : s_ecSearches.Current();
		if (sid != 0 && theApp->searchlist->IsKnownSearchId(sid)) {
			if (request->GetTagByName(EC_TAG_SEARCH_CLOSE)) {
				// Tab close: stop activity, free results, drop from the ring.
				// Free the core's state unconditionally -- that is what
				// actually stops a running Kad search and erases the
				// per-id maps (including m_searchStrings, which is what
				// stops the search reappearing as a discovered tab). Only
				// touch the registry's own bookkeeping (LRU/current) when
				// it's a search the registry actually tracks -- same
				// discipline as guarding Touch() behind Has() last round.
				theApp->searchlist->RemoveResults(sid);
				if (s_ecSearches.Has(sid)) {
					s_ecSearches.Forget(sid);
				}
			} else {
				// Stop button: halt activity but keep the results.
				theApp->searchlist->StopSearchById(sid);
			}
		}
	} else {
		theApp->searchlist->StopSearch();
	}
	return reply;
}

static CECPacket *Get_EC_Response_Search_Request_More(const CECPacket *request, bool multiSearch)
{
	// "More" button (Kad-only): re-ask already-queried peers for a wider result
	// frontier for one search. RequestMoreResults logs what actually happened
	// (the single source of truth shared with the monolithic GUI) and that line
	// is forwarded back to amuleGUI over EC; the reply carries the other half,
	// whether a LATER press could still widen the search, which is what a
	// client greys its control on.
	CECPacket *reply = new CECPacket(EC_OP_MISC_DATA);
	// Per-ID. No ID => the most-recently-started search. Gate on the core's
	// own knowledge (CSearchList::IsKnownSearchId), not s_ecSearches -- see
	// the matching comment in Get_EC_Response_Search_Stop: a monolithic-
	// started Kad search's "More results" button would otherwise silently
	// do nothing.
	const CECTag *idTag = request->GetTagByName(EC_TAG_SEARCH_ID);
	uint32 sid =
		idTag ? static_cast<uint32>(idTag->GetInt()) : (multiSearch ? s_ecSearches.Current() : 0);
	bool reaskable = false;
	if (sid != 0 && (!multiSearch || theApp->searchlist->IsKnownSearchId(sid))) {
		reaskable = theApp->searchlist->RequestMoreResults(sid);
	}
	// Emitted on BOTH paths, including the unknown-id early-out above (which
	// leaves it false -- a search the core does not hold is terminal by
	// definition). That way an absent tag means exactly one thing to the
	// client: a daemon older than this reply, whose answer is unknown rather
	// than "exhausted".
	reply->AddTag(CECTag(EC_TAG_SEARCH_MORE_REASKABLE, static_cast<uint8>(reaskable ? 1 : 0)));
	// Echo which search the verdict is about. The request may not have named
	// one (a client without multi-search leaves it to s_ecSearches.Current()),
	// and a client with several tabs open cannot attribute a bare reply to any
	// of them -- the replies are not correlated any other way.
	if (sid != 0) {
		reply->AddTag(CECTag(EC_TAG_SEARCH_ID, sid));
	}
	return reply;
}

static CECPacket *Get_EC_Response_Search(const CECPacket *request, bool multiSearch)
{
	wxString response;

	const CEC_Search_Tag *search_request =
		static_cast<const CEC_Search_Tag *>(request->GetFirstTagSafe());

	CSearchList::CSearchParams params;
	params.searchString = search_request->SearchText();
	params.typeText = search_request->SearchFileType();
	params.extension = search_request->SearchExt();
	params.minSize = search_request->MinSize();
	params.maxSize = search_request->MaxSize();
	params.availability = search_request->Avail();

	EC_SEARCH_TYPE search_type = search_request->SearchType();
	uint32 op = EC_OP_FAILED;
	uint32 search_id = 0xffffffff;
	bool started = false;

	if (search_type == EC_SEARCH_WEB) {
		response = wxTRANSLATE("WebSearch from remote interface makes no sense.");
	} else if (search_type == EC_SEARCH_BROWSE) {
		// Reported by the daemon, never accepted here: a browse is started
		// by EC_OP_FRIEND, and the mapping below would otherwise fall
		// through its final ternary and quietly start a local search.
		response = wxTRANSLATE("A browse is not a search that can be started.");
	} else {
		SearchType core_search_type = (search_type == EC_SEARCH_GLOBAL) ? GlobalSearch
					      : (search_type == EC_SEARCH_KAD)  ? KadSearch
										: LocalSearch;

		if (multiSearch) {
			// START is additive — it does not stop sibling searches. But
			// ed2k (local/global) share a single in-flight slot and file
			// their results under the scalar m_currentSearch, so starting a
			// NEW ed2k search must finalize any in-flight ed2k search first;
			// otherwise its late UDP results would land in the new search's
			// bucket. A Kad search uses its own ID and its own machinery, so
			// it must NOT disturb a running ed2k search — the two coexist
			// (starting a Kad search here used to kill an in-flight global
			// search, which then returned zero results).
			if (core_search_type != KadSearch) {
				theApp->searchlist->StopInFlightEd2kSearch();
			}
			// The daemon allocates the ID (no sentinel): ed2k gets a
			// bottom-half ID; a Kad search self-allocates a top-half ID
			// inside StartNewSearch, which overwrites this seed and we read
			// the real ID back.
			search_id = theApp->searchlist->AllocateEd2kId();
		} else {
			// Legacy single-search sentinel path (unchanged): wipe the one
			// bucket and reuse 0xffffffff.
			theApp->searchlist->RemoveResults(0xffffffff);
			search_id = 0xffffffff;
		}

		wxString error = theApp->searchlist->StartNewSearch(&search_id, core_search_type, params);
		if (!error.IsEmpty()) {
			response = error;
		} else {
			response = wxTRANSLATE("Search in progress. Refetch results in a moment!");
			op = EC_OP_STRINGS;
			started = true;
			if (multiSearch) {
				// Register whatever ID the core settled on (the Kad ID for a
				// Kad search) as most-recently-used + current, evicting the
				// least-recently-used search if over the ring's cap.
				s_ecSearches.Register(search_id);
			}
		}
	}

	CECPacket *reply = new CECPacket(op);
	// error or search in progress
	reply->AddTag(CECTag(EC_TAG_STRING, response));
	if (multiSearch && started) {
		// Hand the daemon-allocated ID back so the client can address this
		// search.
		reply->AddTag(CECTag(EC_TAG_SEARCH_ID, search_id));
	}
	if (multiSearch) {
		// Echo the client's correlation token (if any) on BOTH outcomes, not
		// just success: the client created its optimistic tab before sending
		// and has no other way to tell which start this verdict answers. On
		// failure it needs the token to drop that phantom tab and release the
		// discovery deferral -- otherwise the id sits in m_pendingSearchStarts
		// until the user happens to close exactly that tab, with an
		// EC_OP_SEARCH_LIST round trip every tick and discovery off in the
		// meantime. Same principle as echoing the id on EC_TAG_SEARCH_EXPIRED:
		// a verdict the client cannot correlate is one it cannot act on
		// (got3nks, PR #680 review).
		const CECTag *ref = request->GetTagByName(EC_TAG_SEARCH_REF);
		if (ref) {
			reply->AddTag(CECTag(EC_TAG_SEARCH_REF, static_cast<uint32>(ref->GetInt())));
		}
	}

	return reply;
}

static CECPacket *Get_EC_Response_Set_SharedFile_Prio(const CECPacket *request)
{
	CECPacket *response = new CECPacket(EC_OP_NOOP);
	for (CECPacket::const_iterator it = request->begin(); it != request->end(); ++it) {
		const CECTag &tag = *it;
		CMD4Hash hash = tag.GetMD4Data();
		uint8 prio = tag.GetFirstTagSafe()->GetInt();
		CKnownFile *cur_file = theApp->sharedfiles->GetFileByID(hash);
		if (!cur_file) {
			continue;
		}
		if (prio == PR_AUTO) {
			cur_file->SetAutoUpPriority(1);
			cur_file->UpdateAutoUpPriority();
		} else {
			cur_file->SetAutoUpPriority(0);
			cur_file->SetUpPriority(prio);
		}
		Notify_SharedFilesUpdateItem(cur_file);
	}

	return response;
}

void CPartFile_Encoder::Encode(CECTag *parent)
{
	//
	// Source part frequencies
	//
	CKnownFile_Encoder::Encode(parent);

	//
	// Gaps
	//
	const CGapList &gaplist = m_PartFile()->GetGapList();
	const size_t gap_list_size = gaplist.size();
	ArrayOfUInts64 gaps;
	gaps.reserve(gap_list_size * 2);

	for (CGapList::const_iterator curr_pos = gaplist.begin(); curr_pos != gaplist.end(); ++curr_pos) {
		gaps.push_back(curr_pos.start());
		gaps.push_back(curr_pos.end());
	}

	int gap_enc_size = 0;
	bool changed;
	const uint8 *gap_enc_data = m_gap_status.Encode(gaps, gap_enc_size, changed);
	if (changed) {
		parent->AddTag(CECTag(EC_TAG_PARTFILE_GAP_STATUS, gap_enc_size, (void *)gap_enc_data));
	}
	delete[] gap_enc_data;

	//
	// Requested blocks
	//
	ArrayOfUInts64 req_buffer;
	const CPartFile::CReqBlockPtrList &requestedblocks = m_PartFile()->GetRequestedBlockList();
	CPartFile::CReqBlockPtrList::const_iterator curr_pos2 = requestedblocks.begin();

	for (; curr_pos2 != requestedblocks.end(); ++curr_pos2) {
		Requested_Block_Struct *block = *curr_pos2;
		req_buffer.push_back(block->StartOffset);
		req_buffer.push_back(block->EndOffset);
	}
	int req_enc_size = 0;
	const uint8 *req_enc_data = m_req_status.Encode(req_buffer, req_enc_size, changed);
	if (changed) {
		parent->AddTag(CECTag(EC_TAG_PARTFILE_REQ_STATUS, req_enc_size, (void *)req_enc_data));
	}
	delete[] req_enc_data;

	//
	// Source names
	//
	// First count occurrence of all source names
	//
	CECEmptyTag sourceNames(EC_TAG_PARTFILE_SOURCE_NAMES);
	typedef std::map<wxString, int> strIntMap;
	strIntMap nameMap;
	const CPartFile::SourceSet &sources = m_PartFile()->GetSourceList();
	for (CPartFile::SourceSet::const_iterator it = sources.begin(); it != sources.end(); ++it) {
		const CClientRef &cur_src = *it;
		if (cur_src.GetRequestFile() != m_file || cur_src.GetClientFilename().Length() == 0) {
			continue;
		}
		const wxString &name = cur_src.GetClientFilename();
		strIntMap::iterator itm = nameMap.find(name);
		if (itm == nameMap.end()) {
			nameMap[name] = 1;
		} else {
			itm->second++;
		}
	}
	//
	// Go through our last list
	//
	for (SourcenameItemMap::iterator it1 = m_sourcenameItemMap.begin();
		it1 != m_sourcenameItemMap.end();) {
		SourcenameItemMap::iterator it2 = it1++;
		strIntMap::iterator itm = nameMap.find(it2->second.name);
		if (itm == nameMap.end()) {
			// name doesn't exist anymore, tell client to forget it
			CECTag tag(EC_TAG_PARTFILE_SOURCE_NAMES, it2->first);
			tag.AddTag(CECIntTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, 0));
			sourceNames.AddTag(tag);
			// and forget it
			m_sourcenameItemMap.erase(it2);
		} else {
			// update count if it changed
			if (it2->second.count != itm->second) {
				CECTag tag(EC_TAG_PARTFILE_SOURCE_NAMES, it2->first);
				tag.AddTag(CECIntTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, itm->second));
				sourceNames.AddTag(tag);
				it2->second.count = itm->second;
			}
			// remove it from nameMap so that only new names are left there
			nameMap.erase(itm);
		}
	}
	//
	// Add new names
	//
	for (strIntMap::iterator it3 = nameMap.begin(); it3 != nameMap.end(); ++it3) {
		int id = ++m_sourcenameID;
		CECIntTag tag(EC_TAG_PARTFILE_SOURCE_NAMES, id);
		tag.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES, it3->first));
		tag.AddTag(CECIntTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, it3->second));
		sourceNames.AddTag(tag);
		// remember it
		m_sourcenameItemMap[id] = SourcenameItem(it3->first, it3->second);
	}
	if (sourceNames.HasChildTags()) {
		parent->AddTag(sourceNames);
	}
}

void CPartFile_Encoder::ResetEncoder()
{
	CKnownFile_Encoder::ResetEncoder();
	m_gap_status.ResetEncoder();
	m_req_status.ResetEncoder();
}

void CKnownFile_Encoder::Encode(CECTag *parent)
{
	//
	// Source part frequencies
	//
	// Reference to the availability list
	const ArrayOfUInts16 &list = m_file->IsPartFile()
					     ? static_cast<const CPartFile *>(m_file)->m_SrcpartFrequency
					     : m_file->m_AvailPartFrequency;
	// Don't add tag if available parts aren't populated yet.
	if (!list.empty()) {
		int part_enc_size;
		bool changed;
		const uint8 *part_enc_data = m_enc_data.Encode(list, part_enc_size, changed);
		if (changed) {
			parent->AddTag(CECTag(EC_TAG_PARTFILE_PART_STATUS, part_enc_size, part_enc_data));
		}
		delete[] part_enc_data;
	}
}

static CECPacket *GetStatsGraphs(const CECPacket *request)
{
	CECPacket *response = NULL;

	switch (request->GetDetailLevel()) {
	case EC_DETAIL_WEB:
	case EC_DETAIL_FULL: {
		double dTimestamp = 0.0;
		if (request->GetTagByName(EC_TAG_STATSGRAPH_LAST) != NULL) {
			dTimestamp = request->GetTagByName(EC_TAG_STATSGRAPH_LAST)->GetDoubleData();
		}
		uint16 nScale = request->GetTagByNameSafe(EC_TAG_STATSGRAPH_SCALE)->GetInt();
		uint16 nMaxPoints = request->GetTagByNameSafe(EC_TAG_STATSGRAPH_WIDTH)->GetInt();
		uint32 *graphData = NULL;
		uint32 *connData = NULL;
		uint64 sessionDl = 0, sessionUl = 0, sessionKad = 0;
		double sessionTimespan = 0.0;
		unsigned int numPoints = theApp->m_statistics->GetHistoryForGui(nMaxPoints,
			(double)nScale,
			&dTimestamp,
			&graphData,
			&connData,
			sessionDl,
			sessionUl,
			sessionKad,
			sessionTimespan);
		if (numPoints) {
			response = new CECPacket(EC_OP_STATSGRAPHS);
			response->AddTag(
				CECTag(EC_TAG_STATSGRAPH_DATA, 4 * numPoints * sizeof(uint32), graphData));
			// Per-point active uploads / active downloads. Older
			// amulegui builds simply ignore the unknown tag.
			response->AddTag(CECTag(
				EC_TAG_STATSGRAPH_DATA_CONN, 2 * numPoints * sizeof(uint32), connData));
			delete[] graphData;
			delete[] connData;
			// Latest session totals — let amulegui compute the same
			// kBytesReceived / sTimestamp session average monolithic
			// shows, instead of falling back to a GUI-local integral.
			response->AddTag(CECTag(EC_TAG_STATSGRAPH_SESSION_DL, sessionDl));
			response->AddTag(CECTag(EC_TAG_STATSGRAPH_SESSION_UL, sessionUl));
			response->AddTag(CECTag(EC_TAG_STATSGRAPH_SESSION_KAD, sessionKad));
			response->AddTag(CECTag(EC_TAG_STATSGRAPH_SESSION_TIMESPAN, sessionTimespan));
			// How deep we can actually answer at the requested scale, so
			// the client can cap its next request instead of guessing.
			// Over-asking gets a record repeated rather than an error,
			// and without timestamps on the wire the client cannot see it.
			response->AddTag(
				CECTag(EC_TAG_STATSGRAPH_DEPTH, (uint16)CStatistics::GetPointsPerRange()));
			response->AddTag(CECTag(EC_TAG_STATSGRAPH_LAST, dTimestamp));
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("No points for graph.")));
		}
		break;
	}
	case EC_DETAIL_INC_UPDATE:
	case EC_DETAIL_UPDATE:
	case EC_DETAIL_CMD:
		// No graphs
		response = new CECPacket(EC_OP_FAILED);
		response->AddTag(CECTag(
			EC_TAG_STRING, wxTRANSLATE("Your client is not configured for this detail level.")));
		break;
	}
	if (!response) {
		response = new CECPacket(EC_OP_FAILED);
		// Unknown reason
	}

	return response;
}

CECPacket *CECServerSocket::ProcessRequest2(const CECPacket *request)
{

	if (!request) {
		return 0;
	}

	CECPacket *response = NULL;

	switch (request->GetOpCode()) {
	//
	// Misc commands
	//
	case EC_OP_SHUTDOWN:
		if (!theApp->IsOnShutDown()) {
			response = new CECPacket(EC_OP_NOOP);
			AddLogLineC(_("External Connection: shutdown requested"));
#ifndef AMULE_DAEMON
			{
				wxCloseEvent evt;
				evt.SetCanVeto(false);
				theApp->ShutDown(evt);
			}
#else
			theApp->ExitMainLoop();
#endif
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Already shutting down.")));
		}
		break;
	case EC_OP_ADD_LINK: {
		// Aggregate the per-link results into a single response: until
		// #206 was filed, every iteration overwrote the previous response,
		// so a batch of N-1 successes followed by one failure looked like
		// a total failure to the caller (and vice versa).
		int successCount = 0;
		int failCount = 0;
		for (CECPacket::const_iterator it = request->begin(); it != request->end(); ++it) {
			const CECTag &tag = *it;
			wxString link = tag.GetStringData();
			int category = 0;
			const CECTag *cattag = tag.GetTagByName(EC_TAG_PARTFILE_CAT);
			if (cattag) {
				category = cattag->GetInt();
			}
			AddLogLineC(CFormat(_("ExternalConn: adding link '%s'.")) % link);
			if (theApp->downloadqueue->AddLink(link, category)) {
				++successCount;
			} else {
				// Per-link error reasons are already printed by AddLink().
				++failCount;
			}
		}
		if (failCount == 0) {
			response = new CECPacket(EC_OP_NOOP);
		} else if (successCount == 0) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(
				CECTag(EC_TAG_STRING, wxTRANSLATE("Invalid link or already on list.")));
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING,
				CFormat(wxString(
					wxTRANSLATE("%d of %d links failed (invalid or already on list)."))) %
					failCount % (failCount + successCount)));
		}
		break;
	}
	//
	// Status requests
	//
	case EC_OP_STAT_REQ:
		response = Get_EC_Response_StatRequest(request, m_LoggerAccess);
		response->AddTag(CEC_ConnState_Tag(request->GetDetailLevel()));
		break;
	case EC_OP_VERSION_CHECK:
		// On-demand version check trigger (amuleapi's POST /version/check).
		// Fire-and-forget: StartVersionCheck() kicks off the async fetch and
		// the result is relayed later via the stats response. EC_OP_NOOP =
		// accepted; EC_OP_FAILED = throttled or (compiled out) unavailable.
#ifdef ENABLE_VERSION_CHECK
		if (theApp->StartVersionCheck()) {
			response = new CECPacket(EC_OP_NOOP);
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(
				EC_TAG_STRING, wxTRANSLATE("Version check throttled; try again shortly.")));
		}
#else
		response = new CECPacket(EC_OP_FAILED);
		response->AddTag(
			CECTag(EC_TAG_STRING, wxTRANSLATE("Version check is not available on this daemon.")));
#endif
		break;
	case EC_OP_GET_CONNSTATE:
		response = new CECPacket(EC_OP_MISC_DATA);
		response->AddTag(CEC_ConnState_Tag(request->GetDetailLevel()));
		break;
	//
	//
	//
	case EC_OP_GET_SHARED_FILES:
		if (request->GetDetailLevel() == EC_DETAIL_FULL &&
			CTagSet<uint32, EC_TAG_KNOWNFILE>(request).empty() &&
			(m_my_flags & EC_FLAG_UTF8_NUMBERS) && (m_my_flags & EC_FLAG_LARGE_TAG_COUNT)) {
			// Per-file bytes cache. Daemon-wide map<ECID, bytes>
			// keyed off CKnownFile::s_globalEcGen; rebuilds only
			// per-file entries whose gen advanced since last use.
			// Concatenated and written through the connection's
			// socket with the same per-connection compression
			// machinery WritePacket uses.
			std::vector<CKnownFile *> snapshot;
			theApp->sharedfiles->CopyFileList(snapshot);
			std::vector<CECFullResponseCache::FileRef> refs;
			refs.reserve(snapshot.size());
			for (size_t i = 0; i < snapshot.size(); ++i) {
				if (!snapshot[i])
					continue;
				CECFullResponseCache::FileRef r;
				r.file = snapshot[i];
				r.ecid = snapshot[i]->ECID();
				r.gen = snapshot[i]->GetECGen();
				refs.push_back(r);
			}
			std::vector<std::shared_ptr<const std::vector<unsigned char>>> blobs =
				s_sharedFilesFullCache.GetBlobs(refs);
			s_sharedFilesFullCache.PruneOutsideOf(refs);
			SendCachedBodyResponse(EC_OP_SHARED_FILES, blobs);
			return NULL;
		}
		if (request->GetDetailLevel() != EC_DETAIL_INC_UPDATE) {
			response = Get_EC_Response_GetSharedFiles(request,
				m_FileEncoder,
				m_lastEcGenSeenShared,
				m_partialUpdateActive,
				m_lastSentSharedFileIds,
				m_sentWithDetailIdsShared);
		}
		break;
	case EC_OP_GET_DLOAD_QUEUE:
		if (request->GetDetailLevel() == EC_DETAIL_FULL &&
			CTagSet<uint32, EC_TAG_PARTFILE>(request).empty() &&
			(m_my_flags & EC_FLAG_UTF8_NUMBERS) && (m_my_flags & EC_FLAG_LARGE_TAG_COUNT)) {
			std::vector<CPartFile *> snapshot;
			theApp->downloadqueue->CopyFileList(snapshot);
			std::vector<CECFullResponseCache::FileRef> refs;
			refs.reserve(snapshot.size());
			for (size_t i = 0; i < snapshot.size(); ++i) {
				if (!snapshot[i])
					continue;
				CECFullResponseCache::FileRef r;
				r.file = snapshot[i];
				r.ecid = snapshot[i]->ECID();
				r.gen = snapshot[i]->GetECGen();
				refs.push_back(r);
			}
			std::vector<std::shared_ptr<const std::vector<unsigned char>>> blobs =
				s_downloadQueueFullCache.GetBlobs(refs);
			s_downloadQueueFullCache.PruneOutsideOf(refs);
			SendCachedBodyResponse(EC_OP_DLOAD_QUEUE, blobs);
			return NULL;
		}
		if (request->GetDetailLevel() != EC_DETAIL_INC_UPDATE) {
			response = Get_EC_Response_GetDownloadQueue(request,
				m_FileEncoder,
				m_lastEcGenSeenPart,
				m_partialUpdateActive,
				m_lastSentPartFileIds,
				m_sentWithDetailIdsPart);
		}
		break;
	case EC_OP_GET_CLIENT_HISTORY:
		response = Get_EC_Response_ClientHistory();
		break;

	//
	// This will evolve into an update-all for inc tags
	//
	case EC_OP_GET_UPDATE:
		if (request->GetDetailLevel() == EC_DETAIL_INC_UPDATE) {
			response = Get_EC_Response_GetUpdate(m_FileEncoder,
				m_obj_tagmap,
				m_lastEcGenSeen,
				m_partialUpdateActive,
				m_lastSentFileIds);
		}
		break;
	case EC_OP_GET_ULOAD_QUEUE:
		response = Get_EC_Response_GetClientQueue(request, m_obj_tagmap, EC_OP_ULOAD_QUEUE);
		break;
	case EC_OP_PARTFILE_SWAP_A4AF_THIS:
	case EC_OP_PARTFILE_SWAP_A4AF_THIS_AUTO:
	case EC_OP_PARTFILE_SWAP_A4AF_OTHERS:
	case EC_OP_PARTFILE_PAUSE:
	case EC_OP_PARTFILE_RESUME:
	case EC_OP_PARTFILE_STOP:
	case EC_OP_PARTFILE_PRIO_SET:
	case EC_OP_PARTFILE_DELETE:
	case EC_OP_PARTFILE_SET_CAT:
		response = Get_EC_Response_PartFile_Cmd(request);
		break;
	case EC_OP_SHAREDFILES_RELOAD:
		// Scheduled, not performed: the walk would otherwise run inline in
		// this handler and every EC client would wait out the whole thing.
		theApp->sharedfiles->RequestReload();
		response = new CECPacket(EC_OP_NOOP);
		break;
	case EC_OP_GET_SHARED_DIRS:
		response = Get_EC_Response_GetSharedDirs();
		break;
	case EC_OP_SET_SHARED_DIRS:
		response = Get_EC_Response_SetSharedDirs(request);
		break;
	case EC_OP_SHARED_SET_PRIO:
		response = Get_EC_Response_Set_SharedFile_Prio(request);
		break;
	case EC_OP_RENAME_FILE: {
		CMD4Hash fileHash = request->GetTagByNameSafe(EC_TAG_KNOWNFILE)->GetMD4Data();
		wxString newName = request->GetTagByNameSafe(EC_TAG_PARTFILE_NAME)->GetStringData();
		// search first in downloadqueue - it might be in known files as well
		CKnownFile *file = theApp->downloadqueue->GetFileByID(fileHash);
		if (!file) {
			file = theApp->knownfiles->FindKnownFileByID(fileHash);
		}
		if (!file) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("File not found.")));
			break;
		}
		if (newName.IsEmpty()) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Invalid file name.")));
			break;
		}

		if (theApp->sharedfiles->RenameFile(file, CPath(newName))) {
			response = new CECPacket(EC_OP_NOOP);
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Unable to rename file.")));
		}

		break;
	}
	case EC_OP_CLEAR_COMPLETED: {
		ListOfUInts32 toClear;
		for (CECTag::const_iterator it = request->begin(); it != request->end(); ++it) {
			if (it->GetTagName() == EC_TAG_ECID) {
				toClear.push_back(it->GetInt());
			}
		}
		theApp->downloadqueue->ClearCompleted(toClear);
		response = new CECPacket(EC_OP_NOOP);
		break;
	}
	case EC_OP_CLIENT_SWAP_TO_ANOTHER_FILE: {
		// Report what happened rather than answering NOOP either way. The swap
		// is best-effort in the core -- it refuses a peer that is actively
		// sending, and a peer that is not an A4AF source of the target has
		// nowhere to go -- and a caller that cannot tell those apart from
		// success can only guess. amulegui registers a null handler for this
		// op and discards the reply whatever its opcode, so nothing on that
		// side needs to change.
		uint32 idClient = request->GetTagByNameSafe(EC_TAG_CLIENT)->GetInt();
		CUpDownClient *client = theApp->clientlist->FindClientByECID(idClient);
		CMD4Hash idFile = request->GetTagByNameSafe(EC_TAG_PARTFILE)->GetMD4Data();
		CPartFile *file = theApp->downloadqueue->GetFileByID(idFile);
		if (!client) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Client not found.")));
		} else if (!file) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("File not found.")));
		} else if (!client->SwapToAnotherFile(true, false, false, file)) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(
				EC_TAG_STRING, wxTRANSLATE("Client could not be swapped to that file.")));
		} else {
			response = new CECPacket(EC_OP_NOOP);
		}
		break;
	}
	case EC_OP_SHARED_FILE_SET_COMMENT: {
		CMD4Hash hash = request->GetTagByNameSafe(EC_TAG_KNOWNFILE)->GetMD4Data();
		CKnownFile *file = theApp->sharedfiles->GetFileByID(hash);
		if (file) {
			wxString newComment =
				request->GetTagByNameSafe(EC_TAG_KNOWNFILE_COMMENT)->GetStringData();
			uint8 newRating = request->GetTagByNameSafe(EC_TAG_KNOWNFILE_RATING)->GetInt();
			CoreNotify_KnownFile_Comment_Set(file, newComment, newRating);
		}
		response = new CECPacket(EC_OP_NOOP);
		break;
	}
	case EC_OP_VERIFY_LOCAL_DATA: {
		CMD4Hash hash = request->GetTagByNameSafe(EC_TAG_KNOWNFILE)->GetMD4Data();
		CKnownFile *file = theApp->sharedfiles->GetFileByID(hash);
		if (file) {
			theApp->sharedfiles->VerifyLocalData(file);
		}
		response = new CECPacket(EC_OP_NOOP);
		break;
	}
	case EC_OP_SHARED_FILE_SEARCH_KAD_NOTES: {
		CMD4Hash hash = request->GetTagByNameSafe(EC_TAG_KNOWNFILE)->GetMD4Data();
		// Notes are requested from the download-comments dialog or a search
		// result: try the download queue first, then the shared list, then the
		// current search results.
		CAbstractFile *file = theApp->downloadqueue->GetFileByID(hash);
		if (!file) {
			file = theApp->sharedfiles->GetFileByID(hash);
		}
		if (!file) {
			file = theApp->searchlist->GetSearchFileByID(hash);
		}
		if (file && file->RequestKadNoteSearch()) {
			// One Kad NOTES lookup runs per hash, but the same file can be shown
			// in several open search tabs (one CSearchFile each).
			// RequestKadNoteSearch set the running flag only on the object it ran
			// on; mirror it onto every same-hash search result so each tab shows
			// the in-flight lookup. The flag is cleared on all of them when the
			// CSearch ends, and the retrieved notes fan out the same way.
			std::vector<CSearchFile *> searchFiles;
			theApp->searchlist->GetAllSearchFilesByID(hash, searchFiles);
			for (CSearchFile *sf : searchFiles) {
				sf->SetKadCommentSearchRunning(true);
			}
		}
		response = new CECPacket(EC_OP_NOOP);
		break;
	}

	//
	// Server commands
	//
	case EC_OP_SERVER_ADD:
		response = Get_EC_Response_Server_Add(request);
		break;
	case EC_OP_SERVER_DISCONNECT:
	case EC_OP_SERVER_CONNECT:
	case EC_OP_SERVER_REMOVE:
		response = Get_EC_Response_Server(request);
		break;
	case EC_OP_GET_SERVER_LIST: {
		response = new CECPacket(EC_OP_SERVER_LIST);
		if (!thePrefs::GetNetworkED2K()) {
			// Kad only: just send an empty list
			break;
		}
		EC_DETAIL_LEVEL detail_level = request->GetDetailLevel();
		std::vector<const CServer *> servers = theApp->serverlist->CopySnapshot();
		for (std::vector<const CServer *>::const_iterator it = servers.begin(); it != servers.end();
			++it) {
			response->AddTag(CEC_Server_Tag(*it, detail_level));
		}
	} break;
	case EC_OP_SERVER_UPDATE_FROM_URL: {
		wxString url = request->GetFirstTagSafe()->GetStringData();

		// Save the new url, and update the UI (if not amuled).
		Notify_ServersURLChanged(url);
		thePrefs::SetEd2kServersUrl(url);

		theApp->serverlist->UpdateServerMetFromURL(url);
		response = new CECPacket(EC_OP_NOOP);
		break;
	}
	case EC_OP_SERVER_SET_STATIC_PRIO: {
		uint32 ecid = request->GetTagByNameSafe(EC_TAG_SERVER)->GetInt();
		CServer *server = theApp->serverlist->GetServerByECID(ecid);
		if (server) {
			const CECTag *staticTag = request->GetTagByName(EC_TAG_SERVER_STATIC);
			if (staticTag) {
				theApp->serverlist->SetStaticServer(server, staticTag->GetInt() > 0);
			}
			const CECTag *prioTag = request->GetTagByName(EC_TAG_SERVER_PRIO);
			if (prioTag) {
				theApp->serverlist->SetServerPrio(server, prioTag->GetInt());
			}
		}
		response = new CECPacket(EC_OP_NOOP);
		break;
	}
	//
	// Friends
	//
	case EC_OP_FRIEND:
		response = Get_EC_Response_Friend(request, m_multiSearchActive);
		break;

	//
	// IPFilter
	//
	case EC_OP_IPFILTER_RELOAD:
		NotifyAlways_IPFilter_Reload();
		response = new CECPacket(EC_OP_NOOP);
		break;

	case EC_OP_IPFILTER_UPDATE: {
		wxString url = request->GetFirstTagSafe()->GetStringData();
		if (url.IsEmpty()) {
			url = thePrefs::IPFilterURL();
		}
		NotifyAlways_IPFilter_Update(url);
		response = new CECPacket(EC_OP_NOOP);
		break;
	}
	//
	// Search
	//
	case EC_OP_SEARCH_START:
		response = Get_EC_Response_Search(request, m_multiSearchActive);
		break;

	case EC_OP_SEARCH_STOP:
		response = Get_EC_Response_Search_Stop(request, m_multiSearchActive);
		break;

	case EC_OP_SEARCH_REQUEST_MORE:
		response = Get_EC_Response_Search_Request_More(request, m_multiSearchActive);
		break;

	case EC_OP_SEARCH_LIST:
		// Legacy (non-multi) clients have only the single sentinel search
		// and no tab-per-id concept, so there is nothing to enumerate.
		response = m_multiSearchActive ? Get_EC_Response_Search_List()
					       : new CECPacket(EC_OP_SEARCH_LIST);
		break;

	case EC_OP_SEARCH_RESULTS: {
		const bool incUpdate = (request->GetDetailLevel() == EC_DETAIL_INC_UPDATE);
		// amulegui (multi + INC_UPDATE) polls all its searches at once: return
		// the union, each result tagged with its search ID (see the handler).
		if (m_multiSearchActive && incUpdate) {
			response = Get_EC_Response_Search_Results_Union(
				m_obj_tagmap, m_partialSearchActive, m_lastSentSearchResultIds);
			break;
		}
		// Otherwise address a specific search by EC_TAG_SEARCH_ID (no ID =>
		// the most-recently-started search) — amulecmd / amuleapi use FULL.
		// Legacy (non-multi) clients keep the single 0xffffffff sentinel.
		wxUIntPtr sid = 0xffffffff;
		if (m_multiSearchActive) {
			const CECTag *idTag = request->GetTagByName(EC_TAG_SEARCH_ID);
			uint32 want = idTag ? static_cast<uint32>(idTag->GetInt()) : s_ecSearches.Current();
			// Gate on the core's own knowledge (CSearchList::IsKnownSearchId),
			// not s_ecSearches: that registry only ever holds EC-initiated
			// searches (Register() runs from the EC_OP_SEARCH_START handler
			// alone), so gating on it reports a monolithic-started search as
			// expired even while it is still running.
			if (want == 0 || !theApp->searchlist->IsKnownSearchId(want)) {
				// Evicted or never-known: tell the client it expired rather
				// than returning a misleading empty result set.
				response = new CECPacket(EC_OP_SEARCH_RESULTS);
				response->AddTag(CECEmptyTag(EC_TAG_SEARCH_EXPIRED));
				break;
			}
			// Only bump the EC-session LRU for a search that registry
			// actually tracks -- Touch() on an id it doesn't know would
			// silently insert it (push_front has no "was it found" guard),
			// growing the ring past kMaxEcSearches for ids Register() never
			// admitted through its own eviction loop.
			if (s_ecSearches.Has(want)) {
				s_ecSearches.Touch(want);
			}
			sid = want;
		}
		if (incUpdate) {
			response = Get_EC_Response_Search_Results(m_obj_tagmap, sid);
		} else {
			response = Get_EC_Response_Search_Results(
				request, m_partialUpdateActive, m_lastSentSearchIds, sid);
		}
		break;
	}

	case EC_OP_SEARCH_PROGRESS: {
		// Union form, decided before anything is allocated. A client that
		// advertised EC_TAG_CAN_SEARCH_PROGRESS_UNION always gets the union
		// shape: naming ids narrows which searches come back, it does not opt
		// back into the single-search reply. Gating this on "no ids named"
		// instead would answer such a client about its FIRST id only and leave
		// every other search out, which it reads as an expiry.
		if (m_multiSearchActive && m_searchProgressUnionActive) {
			response = Get_EC_Response_Search_Progress_Union(request);
			break;
		}
		response = new CECPacket(EC_OP_SEARCH_PROGRESS);
		wxUIntPtr sid = 0xffffffff;
		if (m_multiSearchActive) {
			const CECTag *idTag = request->GetTagByName(EC_TAG_SEARCH_ID);
			uint32 want = idTag ? static_cast<uint32>(idTag->GetInt()) : s_ecSearches.Current();
			// See the matching comment in the EC_OP_SEARCH_RESULTS case above:
			// gate on the core's own knowledge, not the EC-only registry, so a
			// monolithic-started search's progress isn't reported as expired.
			if (want == 0 || !theApp->searchlist->IsKnownSearchId(want)) {
				response->AddTag(CECEmptyTag(EC_TAG_SEARCH_EXPIRED));
				// Echo the id this verdict is about. amulegui reads the
				// whole progress reply under `if (idTag)` -- it has to,
				// since it polls several searches and the replies are not
				// correlated any other way -- so an EXPIRED carrying no id
				// was silently unreadable: the tab for a search the core
				// had already freed stayed open forever, its results
				// dropped by the next union poll, leaving a tab whose rows
				// point at nothing (sorting/scrolling it does nothing).
				// `want` is safe to echo even for an unknown id: it is the
				// value the client itself just asked about (got3nks, PR
				// #680 review).
				if (want != 0) {
					response->AddTag(CECTag(EC_TAG_SEARCH_ID, want));
				}
				break;
			}
			if (s_ecSearches.Has(want)) {
				s_ecSearches.Touch(want);
			}
			sid = want;
			AppendSearchProgress(*response, sid);
			break;
		}
		// EC_TAG_SEARCH_STATUS: unchanged overloaded sentinel for pre-3.1
		// consumers (amulegui / amuleweb / amulecmd).
		response->AddTag(CECTag(EC_TAG_SEARCH_STATUS, theApp->searchlist->GetSearchProgress()));
		// New unambiguous lifecycle tags (3.1+). Modern consumers like
		// amuleapi prefer these and skip the sentinel decode entirely.
		response->AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_STATE,
			static_cast<uint8>(theApp->searchlist->GetSearchLifecycleState())));
		response->AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_KIND,
			static_cast<uint8>(theApp->searchlist->GetSearchLifecycleKind())));
		response->AddTag(CECTag(EC_TAG_SEARCH_RESULT_COUNT,
			static_cast<uint32>(theApp->searchlist->GetCurrentSearchResultCount())));
		// Unified 0..100 completion. Global = real server-queue percent;
		// Kad = cosmetic time-ramp; FINISHED snaps any kind to 100.
		response->AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_PERCENT,
			static_cast<uint8>(theApp->searchlist->GetSearchLifecyclePercent())));
		break;
	}

	case EC_OP_DOWNLOAD_SEARCH_RESULT:
		response = Get_EC_Response_Search_Results_Download(request);
		break;
	//
	// Preferences
	//
	case EC_OP_GET_PREFERENCES:
		response = new CEC_Prefs_Packet(
			request->GetTagByNameSafe(EC_TAG_SELECT_PREFS)->GetInt(), request->GetDetailLevel());
		break;
	case EC_OP_SET_PREFERENCES: {
		static_cast<const CEC_Prefs_Packet *>(request)->Apply();
		// Apply() left any amuleapi password the client sent sitting in
		// the preferences as a pending request; this is what turns it
		// into a stored, stretched record in amuleapi-passwords. Logged
		// rather than returned as an EC error: the rest of the
		// preferences applied fine, and failing the whole call would
		// misreport that.
		wxString credentialError;
		if (!AmuleApiCredentials::ApplyPrefs(credentialError)) {
			AddLogLineC(CFormat(_("Could not save the amuleapi password: %s")) % credentialError);
		}
		theApp->glob_prefs->Save();
		if (thePrefs::IsFilteringClients()) {
			theApp->clientlist->FilterQueues();
		}
		if (thePrefs::IsFilteringServers()) {
			theApp->serverlist->FilterServers();
		}
		if (!thePrefs::GetNetworkED2K() && theApp->IsConnectedED2K()) {
			theApp->DisconnectED2K();
		}
		if (!thePrefs::GetNetworkKademlia() && theApp->IsConnectedKad()) {
			theApp->StopKad();
		}
		response = new CECPacket(EC_OP_NOOP);
		break;
	}

	case EC_OP_CREATE_CATEGORY:
		if (request->GetTagCount() == 1) {
			CEC_Category_Tag tag(
				*static_cast<const CEC_Category_Tag *>(request->GetFirstTagSafe()));
			if (tag.Create()) {
				response = new CECPacket(EC_OP_NOOP);
			} else {
				response = new CECPacket(EC_OP_FAILED);
				response->AddTag(
					CECTag(EC_TAG_CATEGORY, theApp->glob_prefs->GetCatCount() - 1));
				response->AddTag(CECTag(EC_TAG_CATEGORY_PATH, tag.Path()));
			}
			Notify_CategoryAdded();
		} else {
			response = new CECPacket(EC_OP_NOOP);
		}
		break;
	case EC_OP_UPDATE_CATEGORY:
		if (request->GetTagCount() == 1) {
			CEC_Category_Tag tag(
				*static_cast<const CEC_Category_Tag *>(request->GetFirstTagSafe()));
			if (tag.Apply()) {
				response = new CECPacket(EC_OP_NOOP);
			} else {
				response = new CECPacket(EC_OP_FAILED);
				response->AddTag(CECTag(EC_TAG_CATEGORY, tag.GetInt()));
				response->AddTag(CECTag(EC_TAG_CATEGORY_PATH, tag.Path()));
			}
			Notify_CategoryUpdate(tag.GetInt());
		} else {
			response = new CECPacket(EC_OP_NOOP);
		}
		break;
	case EC_OP_DELETE_CATEGORY:
		if (request->GetTagCount() == 1) {
			uint32 cat = request->GetFirstTagSafe()->GetInt();
			// this does not only update the gui, but actually deletes the cat
			Notify_CategoryDelete(cat);
		}
		response = new CECPacket(EC_OP_NOOP);
		break;

	//
	// Logging
	//
	case EC_OP_ADDLOGLINE:
		// cppcheck-suppress duplicateBranch
		if (request->GetTagByName(EC_TAG_LOG_TO_STATUS) != NULL) {
			AddLogLineC(request->GetTagByNameSafe(EC_TAG_STRING)->GetStringData());
		} else {
			AddLogLineN(request->GetTagByNameSafe(EC_TAG_STRING)->GetStringData());
		}
		response = new CECPacket(EC_OP_NOOP);
		break;
	case EC_OP_ADDDEBUGLOGLINE:
		// cppcheck-suppress duplicateBranch
		if (request->GetTagByName(EC_TAG_LOG_TO_STATUS) != NULL) {
			AddDebugLogLineC(
				logGeneral, request->GetTagByNameSafe(EC_TAG_STRING)->GetStringData());
		} else {
			AddDebugLogLineN(
				logGeneral, request->GetTagByNameSafe(EC_TAG_STRING)->GetStringData());
		}
		response = new CECPacket(EC_OP_NOOP);
		break;
	case EC_OP_GET_LOG:
		response = new CECPacket(EC_OP_LOG);
		response->AddTag(CECTag(EC_TAG_STRING, theApp->GetLog(false)));
		break;
	case EC_OP_GET_DEBUGLOG:
		response = new CECPacket(EC_OP_DEBUGLOG);
		response->AddTag(CECTag(EC_TAG_STRING, theApp->GetDebugLog(false)));
		break;
	case EC_OP_RESET_LOG:
		theApp->GetLog(true);
		response = new CECPacket(EC_OP_NOOP);
		break;
	case EC_OP_RESET_DEBUGLOG:
		theApp->GetDebugLog(true);
		response = new CECPacket(EC_OP_NOOP);
		break;
	case EC_OP_GET_LAST_LOG_ENTRY: {
		wxString tmp = theApp->GetLog(false);
		if (tmp.Last() == '\n') {
			tmp.RemoveLast();
		}
		response = new CECPacket(EC_OP_LOG);
		response->AddTag(CECTag(EC_TAG_STRING, tmp.AfterLast('\n')));
	} break;
	case EC_OP_GET_SERVERINFO:
		response = new CECPacket(EC_OP_SERVERINFO);
		response->AddTag(CECTag(EC_TAG_STRING, theApp->GetServerLog(false)));
		break;
	case EC_OP_CLEAR_SERVERINFO:
		theApp->GetServerLog(true);
		response = new CECPacket(EC_OP_NOOP);
		break;
	case EC_OP_GET_CHAT_MESSAGES: {
		// Non-destructive backfill of ONE session's history, for a client
		// opening a conversation it has no transcript for. The polling path
		// is EC_OP_GET_CHAT_SESSIONS; this exists so opening an old tab does
		// not have to replay the whole store.
		const CECTag *idTag = request->GetTagByName(EC_TAG_CHAT_CLIENT_ID);
		if (!idTag) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Missing chat session id")));
			break;
		}
		const CChatSessionStore::Session *session = theApp->chatsessions->Find(idTag->GetInt());
		if (!session) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("No such chat session")));
			break;
		}
		response = new CECPacket(EC_OP_CHAT_MESSAGES);
		response->AddTag(CECTag(EC_TAG_CHAT_MSG_ID, theApp->chatsessions->LastMsgId()));
		response->AddTag(EncodeChatSession(*session, ChatCursorFrom(request)));
		break;
	}
	case EC_OP_GET_CHAT_SESSIONS: {
		// The per-tick workhorse: the session list AND every message newer
		// than the client's cursor in one roundtrip, so an idle connection
		// costs one small packet and a busy one needs no follow-up query.
		const uint32 cursor = ChatCursorFrom(request);
		response = new CECPacket(EC_OP_CHAT_SESSIONS);
		// Top-level cursor even when nothing came back, so a client can
		// advance past messages that were evicted rather than re-asking for
		// them forever.
		response->AddTag(CECTag(EC_TAG_CHAT_MSG_ID, theApp->chatsessions->LastMsgId()));
		for (const CChatSessionStore::Session *session : theApp->chatsessions->Sessions()) {
			// Sessions with nothing new are still listed, with no message
			// children: that is how a client that connected late learns the
			// session exists, and how every client learns a session it is
			// tracking was closed elsewhere (absence from this reply).
			response->AddTag(EncodeChatSession(*session, cursor));
		}
		break;
	}
	case EC_OP_CHAT_SEND: {
		const CECTag *textTag = request->GetTagByName(EC_TAG_CHAT);
		const wxString text = textTag ? textTag->GetStringData() : wxString();
		if (text.IsEmpty()) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Empty chat message")));
			break;
		}
		uint64 gui_id = 0;
		if (!ResolveChatTarget(request, gui_id)) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Unknown chat target")));
			break;
		}
		// Deliberately ignoring the bool: a false return means "queued while
		// connecting", not "failed" — the desktop optimistically prints
		// *** Connecting to Client *** — so turning it into EC_OP_FAILED
		// would report an error for a message that arrives moments later.
		theApp->clientlist->SendChatMessage(gui_id, text);
		response = new CECPacket(EC_OP_NOOP);
		response->AddTag(CECTag(EC_TAG_CHAT_CLIENT_ID, gui_id));
		response->AddTag(CECTag(EC_TAG_CHAT_MSG_ID, theApp->chatsessions->LastMsgId()));
		break;
	}
	case EC_OP_CHAT_CLOSE_SESSION: {
		const CECTag *idTag = request->GetTagByName(EC_TAG_CHAT_CLIENT_ID);
		if (!idTag) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Missing chat session id")));
			break;
		}
		const uint64 gui_id = idTag->GetInt();
		if (!theApp->chatsessions->CloseSession(gui_id)) {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("No such chat session")));
			break;
		}
		theApp->clientlist->SetChatState(gui_id, MS_NONE);
		// Closing is global, matching how closing a search tab destroys the
		// core bucket for every client: the next EC_OP_CHAT_SESSIONS reply
		// simply omits the session and each client drops its tab.
		Notify_Chat_SessionRemoved(gui_id);
		response = new CECPacket(EC_OP_NOOP);
		break;
	}
	//
	// Statistics
	//
	case EC_OP_GET_STATSGRAPHS:
		response = GetStatsGraphs(request);
		break;
	case EC_OP_GET_STATSTREE: {
		theApp->m_statistics->UpdateStatsTree();
		response = new CECPacket(EC_OP_STATSTREE);
		CECTag *tree =
			theStats::GetECStatTree(request->GetTagByNameSafe(EC_TAG_STATTREE_CAPPING)->GetInt());
		if (tree) {
			response->AddTag(*tree);
			delete tree;
		}
		if (request->GetDetailLevel() == EC_DETAIL_WEB) {
			response->AddTag(CECTag(EC_TAG_SERVER_VERSION, VERSION));
			response->AddTag(CECTag(EC_TAG_USER_NICK, thePrefs::GetUserNick()));
		}
		break;
	}

	//
	// Kad
	//
	case EC_OP_KAD_START:
		if (thePrefs::GetNetworkKademlia()) {
			response = new CECPacket(EC_OP_NOOP);
			if (!Kademlia::CKademlia::IsRunning()) {
				Kademlia::CKademlia::Start();
				theApp->ShowConnectionState();
			}
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(
				CECTag(EC_TAG_STRING, wxTRANSLATE("Kad is disabled in preferences.")));
		}
		break;
	case EC_OP_KAD_STOP:
		theApp->StopKad();
		theApp->ShowConnectionState();
		response = new CECPacket(EC_OP_NOOP);
		break;
	case EC_OP_KAD_UPDATE_FROM_URL: {
		wxString url = request->GetFirstTagSafe()->GetStringData();

		// Save the new url, and update the UI (if not amuled).
		Notify_NodesURLChanged(url);
		thePrefs::SetKadNodesUrl(url);

		theApp->UpdateNotesDat(url);
		response = new CECPacket(EC_OP_NOOP);
		break;
	}
	case EC_OP_KAD_BOOTSTRAP_FROM_IP:
		if (thePrefs::GetNetworkKademlia()) {
			theApp->BootstrapKad(request->GetTagByNameSafe(EC_TAG_BOOTSTRAP_IP)->GetInt(),
				request->GetTagByNameSafe(EC_TAG_BOOTSTRAP_PORT)->GetInt());
			theApp->ShowConnectionState();
			response = new CECPacket(EC_OP_NOOP);
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(
				CECTag(EC_TAG_STRING, wxTRANSLATE("Kad is disabled in preferences.")));
		}
		break;

	//
	// Networks
	// These requests are currently used only in the text client
	//
	case EC_OP_CONNECT:
		if (thePrefs::GetNetworkED2K()) {
			response = new CECPacket(EC_OP_STRINGS);
			if (theApp->IsConnectedED2K()) {
				response->AddTag(
					CECTag(EC_TAG_STRING, wxTRANSLATE("Already connected to eD2k.")));
			} else {
				theApp->serverconnect->ConnectToAnyServer();
				response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Connecting to eD2k...")));
			}
		}
		if (thePrefs::GetNetworkKademlia()) {
			if (!response) {
				response = new CECPacket(EC_OP_STRINGS);
			}
			if (theApp->IsConnectedKad()) {
				response->AddTag(
					CECTag(EC_TAG_STRING, wxTRANSLATE("Already connected to Kad.")));
			} else {
				theApp->StartKad();
				response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("Connecting to Kad...")));
			}
		}
		if (response) {
			theApp->ShowConnectionState();
		} else {
			response = new CECPacket(EC_OP_FAILED);
			response->AddTag(CECTag(EC_TAG_STRING, wxTRANSLATE("All networks are disabled.")));
		}
		break;
	case EC_OP_DISCONNECT:
		if (theApp->IsConnected()) {
			response = new CECPacket(EC_OP_STRINGS);
			if (theApp->IsConnectedED2K()) {
				theApp->serverconnect->Disconnect();
				response->AddTag(
					CECTag(EC_TAG_STRING, wxTRANSLATE("Disconnected from eD2k.")));
			}
			if (theApp->IsConnectedKad()) {
				theApp->StopKad();
				response->AddTag(
					CECTag(EC_TAG_STRING, wxTRANSLATE("Disconnected from Kad.")));
			}
			theApp->ShowConnectionState();
		} else {
			response = new CECPacket(EC_OP_NOOP);
		}
		break;
	}
	if (!response) {
		AddLogLineN(CFormat(_("External Connection: invalid opcode received: %#x")) %
			    request->GetOpCode());
		wxFAIL;
		response = new CECPacket(EC_OP_FAILED);
		response->AddTag(
			CECTag(EC_TAG_STRING, wxTRANSLATE("Invalid opcode (wrong protocol version?)")));
	}
	return response;
}

/*
 * Here notification-based EC. Notification will be sorted by priority for possible throttling.
 */

/*
 * Core general status
 */
ECStatusMsgSource::ECStatusMsgSource()
{
	m_last_ed2k_status_sent = 0xffffffff;
	m_last_kad_status_sent = 0xffffffff;
	m_server = (void *)0xffffffff;
}

uint32 ECStatusMsgSource::GetEd2kStatus()
{
	if (theApp->IsConnectedED2K()) {
		return theApp->GetED2KID();
	} else if (theApp->serverconnect->IsConnecting()) {
		return 1;
	} else {
		return 0;
	}
}

uint32 ECStatusMsgSource::GetKadStatus()
{
	if (theApp->IsConnectedKad()) {
		return 1;
	} else if (Kademlia::CKademlia::IsFirewalled()) {
		return 2;
	} else if (Kademlia::CKademlia::IsRunning()) {
		return 3;
	}
	return 0;
}

CECPacket *ECStatusMsgSource::GetNextPacket()
{
	if ((m_last_ed2k_status_sent != GetEd2kStatus()) || (m_last_kad_status_sent != GetKadStatus()) ||
		(m_server != theApp->serverconnect->GetCurrentServer())) {

		m_last_ed2k_status_sent = GetEd2kStatus();
		m_last_kad_status_sent = GetKadStatus();
		m_server = theApp->serverconnect->GetCurrentServer();

		CECPacket *response = new CECPacket(EC_OP_STATS);
		response->AddTag(CEC_ConnState_Tag(EC_DETAIL_UPDATE));
		return response;
	}
	return 0;
}

/*
 * Downloading files
 */
ECPartFileMsgSource::ECPartFileMsgSource()
{
	std::vector<CPartFile *> snapshot;
	theApp->downloadqueue->CopyFileList(snapshot);
	for (std::vector<CPartFile *>::const_iterator it = snapshot.begin(); it != snapshot.end(); ++it) {
		CPartFile *cur_file = *it;
		PARTFILE_STATUS status = { true, false, false, false, true, cur_file };
		m_dirty_status[cur_file->GetFileHash()] = status;
	}
}

void ECPartFileMsgSource::SetDirty(const CPartFile *file)
{
	CMD4Hash filehash = file->GetFileHash();
	if (m_dirty_status.find(filehash) != m_dirty_status.end()) {
		m_dirty_status[filehash].m_dirty = true;
		;
	}
}

void ECPartFileMsgSource::SetNew(const CPartFile *file)
{
	CMD4Hash filehash = file->GetFileHash();
	wxASSERT(m_dirty_status.find(filehash) == m_dirty_status.end());
	PARTFILE_STATUS status = { true, false, false, false, true, file };
	m_dirty_status[filehash] = status;
}

void ECPartFileMsgSource::SetCompleted(const CPartFile *file)
{
	CMD4Hash filehash = file->GetFileHash();
	wxASSERT(m_dirty_status.find(filehash) != m_dirty_status.end());

	m_dirty_status[filehash].m_finished = true;
}

void ECPartFileMsgSource::SetRemoved(const CPartFile *file)
{
	CMD4Hash filehash = file->GetFileHash();
	wxASSERT(m_dirty_status.find(filehash) != m_dirty_status.end());

	m_dirty_status[filehash].m_removed = true;
}

CECPacket *ECPartFileMsgSource::GetNextPacket()
{
	for (std::map<CMD4Hash, PARTFILE_STATUS>::iterator it = m_dirty_status.begin();
		it != m_dirty_status.end();
		it++) {
		if (it->second.m_new || it->second.m_dirty || it->second.m_removed) {
			CMD4Hash filehash = it->first;

			const CPartFile *partfile = it->second.m_file;

			CECPacket *packet = new CECPacket(EC_OP_DLOAD_QUEUE);
			if (it->second.m_removed) {
				CECTag tag(EC_TAG_PARTFILE, filehash);
				packet->AddTag(tag);
				m_dirty_status.erase(it);
			} else {
				CEC_PartFile_Tag tag(
					partfile, it->second.m_new ? EC_DETAIL_FULL : EC_DETAIL_UPDATE);
				packet->AddTag(tag);
			}
			m_dirty_status[filehash].m_new = false;
			m_dirty_status[filehash].m_dirty = false;

			return packet;
		}
	}
	return 0;
}

/*
 * Shared files - similar to downloading
 */
ECKnownFileMsgSource::ECKnownFileMsgSource()
{
	std::vector<CKnownFile *> snapshot;
	theApp->sharedfiles->CopyFileList(snapshot);
	for (std::vector<CKnownFile *>::const_iterator it = snapshot.begin(); it != snapshot.end(); ++it) {
		const CKnownFile *cur_file = *it;
		KNOWNFILE_STATUS status = { true, false, false, true, cur_file };
		m_dirty_status[cur_file->GetFileHash()] = status;
	}
}

void ECKnownFileMsgSource::SetDirty(const CKnownFile *file)
{
	CMD4Hash filehash = file->GetFileHash();
	if (m_dirty_status.find(filehash) != m_dirty_status.end()) {
		m_dirty_status[filehash].m_dirty = true;
		;
	}
}

void ECKnownFileMsgSource::SetNew(const CKnownFile *file)
{
	CMD4Hash filehash = file->GetFileHash();
	wxASSERT(m_dirty_status.find(filehash) == m_dirty_status.end());
	KNOWNFILE_STATUS status = { true, false, false, true, file };
	m_dirty_status[filehash] = status;
}

void ECKnownFileMsgSource::SetRemoved(const CKnownFile *file)
{
	CMD4Hash filehash = file->GetFileHash();
	wxASSERT(m_dirty_status.find(filehash) != m_dirty_status.end());

	m_dirty_status[filehash].m_removed = true;
}

CECPacket *ECKnownFileMsgSource::GetNextPacket()
{
	for (std::map<CMD4Hash, KNOWNFILE_STATUS>::iterator it = m_dirty_status.begin();
		it != m_dirty_status.end();
		it++) {
		if (it->second.m_new || it->second.m_dirty || it->second.m_removed) {
			CMD4Hash filehash = it->first;

			const CKnownFile *partfile = it->second.m_file;

			CECPacket *packet = new CECPacket(EC_OP_SHARED_FILES);
			if (it->second.m_removed) {
				CECTag tag(EC_TAG_PARTFILE, filehash);
				packet->AddTag(tag);
				m_dirty_status.erase(it);
			} else {
				CEC_SharedFile_Tag tag(
					partfile, it->second.m_new ? EC_DETAIL_FULL : EC_DETAIL_UPDATE);
				packet->AddTag(tag);
			}
			m_dirty_status[filehash].m_new = false;
			m_dirty_status[filehash].m_dirty = false;

			return packet;
		}
	}
	return 0;
}

/*
 * Notification about search status
 */
ECSearchMsgSource::ECSearchMsgSource() {}

CECPacket *ECSearchMsgSource::GetNextPacket()
{
	if (m_dirty_status.empty()) {
		return 0;
	}

	CECPacket *response = new CECPacket(EC_OP_SEARCH_RESULTS);
	for (std::map<CMD4Hash, SEARCHFILE_STATUS>::iterator it = m_dirty_status.begin();
		it != m_dirty_status.end();
		it++) {

		if (it->second.m_new) {
			response->AddTag(CEC_SearchFile_Tag(it->second.m_file, EC_DETAIL_FULL));
			it->second.m_new = false;
		} else if (it->second.m_dirty) {
			response->AddTag(CEC_SearchFile_Tag(it->second.m_file, EC_DETAIL_UPDATE));
		}
	}

	return response;
}

void ECSearchMsgSource::FlushStatus()
{
	m_dirty_status.clear();
}

void ECSearchMsgSource::SetDirty(const CSearchFile *file)
{
	if (m_dirty_status.count(file->GetFileHash())) {
		m_dirty_status[file->GetFileHash()].m_dirty = true;
	} else {
		m_dirty_status[file->GetFileHash()].m_new = true;
		m_dirty_status[file->GetFileHash()].m_dirty = true;
		m_dirty_status[file->GetFileHash()].m_child_dirty = true;
		m_dirty_status[file->GetFileHash()].m_file = file;
	}
}

void ECSearchMsgSource::SetChildDirty(const CSearchFile *file)
{
	m_dirty_status[file->GetFileHash()].m_child_dirty = true;
}

/*
 * Notification about uploading clients
 */
CECPacket *ECClientMsgSource::GetNextPacket()
{
	return 0;
}

//
// Notification iface per-client
//
ECNotifier::ECNotifier() {}

ECNotifier::~ECNotifier()
{
	while (m_msg_source.begin() != m_msg_source.end())
		Remove_EC_Client(m_msg_source.begin()->first);
}

CECPacket *ECNotifier::GetNextPacket(ECUpdateMsgSource *msg_source_array[])
{
	CECPacket *packet = 0;
	//
	// priority 0 is highest
	//
	for (int i = 0; i < EC_STATUS_LAST_PRIO; i++) {
		if ((packet = msg_source_array[i]->GetNextPacket()) != 0) {
			break;
		}
	}
	return packet;
}

CECPacket *ECNotifier::GetNextPacket(CECServerSocket *sock)
{
	//
	// OnOutput is called for a first time before
	// socket is registered
	//
	if (m_msg_source.count(sock)) {
		ECUpdateMsgSource **notifier_array = m_msg_source[sock];
		if (!notifier_array) {
			return 0;
		}
		CECPacket *packet = GetNextPacket(notifier_array);
		// printf("[EC] next update packet; opcode=%x\n",packet ? packet->GetOpCode() : 0xff);
		return packet;
	} else {
		return 0;
	}
}

//
// Interface to notification macros
//
void ECNotifier::DownloadFile_SetDirty(const CPartFile *file)
{
	for (std::map<CECServerSocket *, ECUpdateMsgSource **>::iterator i = m_msg_source.begin();
		i != m_msg_source.end();
		++i) {
		CECServerSocket *sock = i->first;
		if (sock->HaveNotificationSupport()) {
			ECUpdateMsgSource **notifier_array = i->second;
			static_cast<ECPartFileMsgSource *>(notifier_array[EC_PARTFILE])->SetDirty(file);
		}
	}
	NextPacketToSocket();
}

void ECNotifier::DownloadFile_RemoveFile(const CPartFile *file)
{
	for (std::map<CECServerSocket *, ECUpdateMsgSource **>::iterator i = m_msg_source.begin();
		i != m_msg_source.end();
		++i) {
		ECUpdateMsgSource **notifier_array = i->second;
		static_cast<ECPartFileMsgSource *>(notifier_array[EC_PARTFILE])->SetRemoved(file);
	}
	NextPacketToSocket();
}

void ECNotifier::DownloadFile_RemoveSource(const CPartFile *)
{
	// per-partfile source list is not supported (yet), and IMHO quite useless
}

void ECNotifier::DownloadFile_AddFile(const CPartFile *file)
{
	for (std::map<CECServerSocket *, ECUpdateMsgSource **>::iterator i = m_msg_source.begin();
		i != m_msg_source.end();
		++i) {
		ECUpdateMsgSource **notifier_array = i->second;
		static_cast<ECPartFileMsgSource *>(notifier_array[EC_PARTFILE])->SetNew(file);
	}
	NextPacketToSocket();
}

void ECNotifier::DownloadFile_AddSource(const CPartFile *)
{
	// per-partfile source list is not supported (yet), and IMHO quite useless
}

void ECNotifier::SharedFile_AddFile(const CKnownFile *file)
{
	for (std::map<CECServerSocket *, ECUpdateMsgSource **>::iterator i = m_msg_source.begin();
		i != m_msg_source.end();
		++i) {
		ECUpdateMsgSource **notifier_array = i->second;
		static_cast<ECKnownFileMsgSource *>(notifier_array[EC_KNOWN])->SetNew(file);
	}
	NextPacketToSocket();
}

void ECNotifier::SharedFile_RemoveFile(const CKnownFile *file)
{
	for (std::map<CECServerSocket *, ECUpdateMsgSource **>::iterator i = m_msg_source.begin();
		i != m_msg_source.end();
		++i) {
		ECUpdateMsgSource **notifier_array = i->second;
		static_cast<ECKnownFileMsgSource *>(notifier_array[EC_KNOWN])->SetRemoved(file);
	}
	NextPacketToSocket();
}

void ECNotifier::SharedFile_RemoveAllFiles()
{
	// need to figure out what to do here
}

void ECNotifier::Add_EC_Client(CECServerSocket *sock)
{
	ECUpdateMsgSource **notifier_array = new ECUpdateMsgSource *[EC_STATUS_LAST_PRIO];
	notifier_array[EC_STATUS] = new ECStatusMsgSource();
	notifier_array[EC_SEARCH] = new ECSearchMsgSource();
	notifier_array[EC_PARTFILE] = new ECPartFileMsgSource();
	notifier_array[EC_CLIENT] = new ECClientMsgSource();
	notifier_array[EC_KNOWN] = new ECKnownFileMsgSource();

	m_msg_source[sock] = notifier_array;
}

void ECNotifier::Remove_EC_Client(CECServerSocket *sock)
{
	if (m_msg_source.count(sock)) {
		ECUpdateMsgSource **notifier_array = m_msg_source[sock];

		m_msg_source.erase(sock);

		for (int i = 0; i < EC_STATUS_LAST_PRIO; i++) {
			delete notifier_array[i];
		}
		delete[] notifier_array;
	}
}

void ECNotifier::NextPacketToSocket()
{
	for (std::map<CECServerSocket *, ECUpdateMsgSource **>::iterator i = m_msg_source.begin();
		i != m_msg_source.end();
		++i) {
		CECServerSocket *sock = i->first;
		if (sock->HaveNotificationSupport() && !sock->DataPending()) {
			ECUpdateMsgSource **notifier_array = i->second;
			// Same ownership contract as WriteDoneAndQueueEmpty: the
			// CECPacket from GetNextPacket is caller-owned and
			// SendPacket only serialises it.  Wrap so it's freed at
			// scope exit (#765).
			CSmartPtr<CECPacket> packet(GetNextPacket(notifier_array));
			if (packet) {
				// printf("[EC] sending update packet; opcode=%x\n",packet->GetOpCode());
				sock->SendPacket(packet.get());
			}
		}
	}
}

// File_checked_for_headers
