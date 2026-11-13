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

#ifndef CLIENTLIST_H
#define CLIENTLIST_H

#include "DeadSourceList.h"
#include "BanRecord.h" // Needed for CBanRecord // Needed for CDeadSourceList
#include "ClientRef.h"
#include "KadBuddyRetry.h"
#include "KadBuddyStats.h"
#include "ChatSessionStore.h"
#include "CanonicalPeerIndex.h"
#include "TrackedClientRecord.h"
#include "kademlia/kademlia/FirewallCheckRequests.h"

#include <deque>
#include <set>

class CUpDownClient;
class CClientTCPSocket;
class CMD4Hash;
class CProtocolPeerIdentity;
namespace Kademlia
{
class CContact;
class CUInt128;
} // namespace Kademlia

enum buddyState
{
	Disconnected,
	Connecting,
	Connected
};

#define BAN_CLEANUP_TIME 1200000 // 20 min

/**
 * Manages existing clients: tracks existing, banned, dead and dying ones, and matches a new
 * client instance against those already known so duplicates are avoided.
 */
class CClientList
{
public:
	CClientList();

	~CClientList();

	/**
	 * Adds a client to the global list of clients.
	 */
	void AddClient(CUpDownClient *toadd);

	/**
	 * The client at this address that could still be the peer with this hash, or nullptr. Skips
	 * any that identifies as somebody else.
	 */
	CUpDownClient *FindReusableClient(const CMD4Hash &hash, uint32 ip, uint16 port);
	CUpDownClient *FindReusableClient(const CMD4Hash &hash, const CNetworkAddress &address, uint16 port);

	/**
	 * A client for the peer at this address, added to the list.
	 *
	 * For the actions that inherently mean "go talk to this peer" -- browsing its shared files,
	 * opening a chat -- when we are not already connected to it and only hold its last known
	 * address. Creating the object does not connect: the request the caller makes next is what
	 * opens a connection.
	 *
	 * Returns the client already held for this peer, matched by hash and failing that by
	 * address, so repeating an action reuses the object the previous one made instead of
	 * stacking up unreachable duplicates.
	 */
	CClientRef CreateForAddress(const CMD4Hash &hash, uint32 ip, uint16 port, const wxString &name);

	/**
	 * Removes a client from the client lists. To be called from CUpDownClient::Safe_Delete only.
	 */
	void RemoveClient(CUpDownClient *client);

	/**
	 * Updates the recorded address before the client changes it.
	 * Present addresses are indexed canonically; the uint32 adapter treats zero as absent.
	 */
	void UpdateClientIP(CUpDownClient *client, const CNetworkAddress &address);

	/**
	 * Updates the recorded ID of the specified client, before it actually changes its ID.
	 * Unlike the IP and hash versions, this always ensures there is an entry, whatever newID is.
	 */
	void UpdateClientID(CUpDownClient *client, uint32 newID);

	/**
	 * Updates the recorded hash of the specified client, before it actually changes its
	 * user-hash. An entry is only added when the new hash is valid.
	 */
	void UpdateClientHash(CUpDownClient *client, const CMD4Hash &newHash);

	/**
	 * Returns the number of listed clients.
	 */
	uint32 GetClientCount() const;

	/**
	 * Deletes all tracked clients.
	 */
	void DeleteAll();

	/**
	 * Replaces a new client instance with an already existing client, if one exists.
	 *
	 * Call this when a new client instance has been created: it is compared against all existing
	 * clients, and on a match the new instance is deleted and the pointer set to the existing
	 * one.
	 */
	//! `senderDiscarded`, when given, reports that `sender` was destroyed here and
	//! `*client` left null: the caller owns that socket and must stop using it.
	// matchOthersOnly is for a client that is already listed, so its own entries cannot count
	// as the known client it is looking for.
	bool AttachToAlreadyKnown(CUpDownClient **client,
		CClientTCPSocket *sender,
		bool *senderDiscarded = nullptr,
		bool matchOthersOnly = false);

	/**
	 * Finds a client with the specified ip and port.
	 */
	CUpDownClient *FindClientByIP(uint32 clientip, uint16 port);
	CUpDownClient *FindClientByIP(const CNetworkAddress &address, uint16 port);

	/**
	 * As FindClientByIP(), for a callback we are asked to dial: skips a client whose route
	 * another host answered. That is a NAT neighbour's client, not the one at that endpoint now.
	 */
	CUpDownClient *FindCallbackClientByIP(uint32 clientip, uint16 port);

	/**
	 * Finds a client with the specified ip, returning the first if several share it.
	 */
	CUpDownClient *FindClientByIP(uint32 clientip);
	CUpDownClient *FindClientByIP(const CNetworkAddress &address);

	/**
	 * The one listed client whose GetProtocolPeerIdentity() equals @p identity, or null when there is
	 * none or more than one. HighID and LowID are looked up by hybrid ID, native IPv6 by address.
	 */
	CUpDownClient *FindClientByProtocolPeerIdentity(const CProtocolPeerIdentity &identity);

	/**
	 * Finds a client with the specified ECID.
	 */
	CUpDownClient *FindClientByECID(uint32 ecid) const;

	/**
	 * Adds a client to the list of tracked clients, so it stays known after deletion and port or
	 * hash changes can be spotted.
	 */
	void AddTrackClient(CUpDownClient *toadd);

	/**
	 * Checks if a client has changed its user-hash.
	 */
	bool ComparePriorUserhash(const CNetworkAddress &address, uint16 nPort, void *pNewHash);

	/**
	 * Bans an IP address for 2 hours.
	 */
	void AddBannedClient(uint32 dwIP);
	void AddBannedClient(const CNetworkAddress &address);

	/**
	 * @return True if the IP is banned.
	 */
	bool IsBannedClient(uint32 dwIP);
	bool IsBannedClient(const CNetworkAddress &address);

	/**
	 * Unbans an IP address, if it has been banned.
	 */
	void RemoveBannedClient(uint32 dwIP);
	void RemoveBannedClient(const CNetworkAddress &address);

	/**
	 * Main loop: cleans the various lists and deletes pending clients on the deletion queue.
	 */
	void Process();

	/**
	 * Removes all clients filtered by the current IPFilter. Call after changing the IPFilter
	 * list, or connections to illegal IPs would be left in place, bypassing the filter.
	 */
	void FilterQueues();

	//! The type of the list used to store client-pointers for a couple of tasks.
	typedef std::deque<CClientRef> SourceList;

	/**
	 * Returns the clients with the specified user-hash, provided it is a valid non-empty one.
	 * An empty hash simply finds nothing.
	 */
	SourceList GetClientsByHash(const CMD4Hash &hash);

	/**
	 * Returns clients with the canonical address. Absence finds nothing;
	 * the legacy integer adapter treats zero as absent.
	 */
	SourceList GetClientsByIP(unsigned long ip);
	SourceList GetClientsByIP(const CNetworkAddress &address);

	//! The type of the list used to store legacy user IDs.
	typedef std::multimap<uint32, CClientRef> IDMap;
	//! The pairs of the user ID list.
	typedef std::pair<uint32, CClientRef> IDMapPair;

	/**
	 * @return The complete list of clients.
	 */
	const IDMap &GetClientList();

#ifdef ENABLE_IPV6
	//! Mark eligible connected peers to receive the current IPv6 on their next outbound write.
	void MarkIPv6ChangeNotificationPending();
#endif

	/**
	 * Adds a source to the list of dead sources.
	 */
	void AddDeadSource(const CUpDownClient *client);

	/**
	 * @return True if the source is recorded as dead. A dead source is not a valid source and
	 * must not be added to partfiles.
	 */
	bool IsDeadSource(const CUpDownClient *client);

	/**
	 * Sends to a stable peer hash. Unavailable is distinct from queued while connecting.
	 */
	enum class ChatSendResult
	{
		Unavailable,
		Queued,
		Sent
	};
	// `legacyRoute`: see CChatSessionStore::AddOutgoing().
	ChatSendResult SendChatMessage(
		const CChatPeer &peer, const wxString &message, uint64 legacyRoute = 0);
	CUpDownClient *FindChatClient(const CChatPeer &peer) const;
	CChatPeer ResolveLegacyChatPeer(uint64 gui_id) const;

	/**
	 * Stops a chat session with a client.
	 */
	void SetChatState(const CChatPeer &peer, uint8 state);
	void CloseChatSession(const CChatPeer &peer);

	uint8 GetBuddyStatus() const { return m_nBuddyStatus; }
	// This must be used on CreateKadSourceLink and if we ever add the columns
	// on shared files control.
	CUpDownClient *GetBuddy() { return m_pBuddy.GetClient(); }
	uint32 GetBuddyIP();
	uint16 GetBuddyPort();
	bool RequestTCP(Kademlia::CContact *contact, uint8_t connectOptions);
	void RequestBuddy(Kademlia::CContact *contact, uint8_t connectOptions);
	bool CanRequestBuddy(uint32_t ip) const;
	void RecordBuddyFailure(uint32_t ip);
	enum class EIncomingBuddy
	{
		Offered,
		Full,     // Every slot this requester may take is linked.
		SameIP,   // We already serve, or have offered to, that address.
		Conflict, // We already know that client, or it is us.
	};
	EIncomingBuddy IncomingBuddy(Kademlia::CContact *contact, Kademlia::CUInt128 *buddyID, bool natT);

	//! How many buddies we serve at most: the setting, capped by the connection limit.
	uint32 GetServedBuddyLimit() const;
	//! Whether a callback request from `sender` may reach `buddy` now: one a minute per sender.
	bool AllowBuddyCallback(const CUpDownClient *buddy, uint32 sender);
	//! Whether `bytes` more fit the buddy's byte budget for this minute.
	bool HasBuddyRelayBudget(const CUpDownClient *buddy, uint32 bytes) const;
	bool IsServedBuddyLinked(const CUpDownClient *client) const
	{
		return FindServedLink(client) != nullptr;
	}
	Kademlia::CBuddyStats &GetBuddyStats() { return m_buddyStats; }
	const Kademlia::CBuddyStats &GetBuddyStats() const { return m_buddyStats; }
	uint32 GetServedBuddyCount() const { return static_cast<uint32>(m_servedLinks.size()); }
	//! Buddy requests we answered whose node has not connected yet.
	uint32 GetPendingBuddyCount() const;
	//! Summed over the buddies we serve now.
	uint64 GetServedLinkedMs() const;
	//! How long our own buddy has been linked, 0 without one.
	uint64 GetOwnBuddyLinkedMs() const;
	//! The served buddy whose BuddyID matches in either layout, or nullptr.
	CUpDownClient *FindServedBuddy(const uint8_t *buddyID) const;
	void RecordBuddyRelay(const CUpDownClient *buddy, bool callback, uint32 bytes);
	//! A callback or reask that no served buddy matched.
	void RecordBuddyRelayDrop(bool callback, const uint8_t *buddyID);
	//! One line per rate-limited buddy packet, up to a few a minute. `counted` is the limiter's
	//! count for the sender, 0 when its table was full. `buddyID` may be nullptr.
	void LogBuddyPacketLimited(const wxString &what,
		const wxString &from,
		uint32 counted,
		uint32 perMinute,
		const uint8_t *buddyID);
	//! The same, for a callback request over the limit of one a minute per sender and buddy.
	void LogBuddyCallbackLimited(const wxString &from, const uint8_t *buddyID);
	void LogBuddyDisconnect(const CUpDownClient *client, const wxString &reason);
	void RecordBuddyTimeout(const CUpDownClient *client);
	void RemoveFromKadList(CUpDownClient *torem);
	void AddToKadList(CUpDownClient *toadd);
	bool DoRequestFirewallCheckUDP(const Kademlia::CContact &contact);

	void AddKadFirewallRequest(uint32 ip);
	bool IsKadFirewallCheckIP(uint32 ip) const;
	bool TakeKadFirewallReply(uint32 ip, Kademlia::CFirewallCheckRequests::Reply reply);

	// Direct Callback list
	void AddDirectCallbackClient(CUpDownClient *toAdd);
	void RemoveDirectCallback(CUpDownClient *toRemove)
	{
		m_currentDirectCallbacks.remove(CCLIENTREF(toRemove, ""));
	}
	void AddTrackCallbackRequests(uint32_t ip);
	void AddTrackCallbackRequests(const CNetworkAddress &address);
	bool AllowCallbackRequest(uint32_t ip) const;
	bool AllowCallbackRequest(const CNetworkAddress &address) const;

protected:
	/* Avoids unwanted clients staying in the client list forever */
	void CleanUpClientList();

	void ProcessDirectCallbackList();

private:
	/**
	 * Finds the first client matching the specified one, or NULL. Uses the same checks as
	 * CUpDownClient::Compare, but without the overhead.
	 */
	CUpDownClient *FindMatchingClient(CUpDownClient *client, const CUpDownClient *excluded = nullptr);

	CUpDownClient *FindClientAtEndpoint(const CNetworkAddress &address, uint16 port, bool forCallback);

	/**
	 * Check if we already know this IP.
	 */
	bool IsIPAlreadyKnown(uint32_t ip);

	/**
	 * Removes the client from the IP-list.
	 */
	void RemoveIPFromList(CUpDownClient *client);
	/**
	 * Removes the client from the ID-list.
	 */
	bool RemoveIDFromList(CUpDownClient *client);
	/**
	 * Removes the client from the hash-list.
	 */
	void RemoveHashFromList(CUpDownClient *client);

	//! The type of the list used to store user-hashes.
	typedef std::multimap<CMD4Hash, CClientRef> HashMap;
	//! The pairs of the Hash-list.
	typedef std::pair<CMD4Hash, CClientRef> HashMapPair;

	//! The map of clients with valid hashes
	HashMap m_hashList;

	//! Canonical peer addresses; absent peers have no entry.
	typedef CCanonicalPeerIndex<CClientRef> AddressMap;
	AddressMap m_ipList;

	//! The full lists of clients
	IDMap m_clientList;

	//! The banned addresses, and the pairing rule for theStats' banned count: every operation
	//! reports whether the set actually changed, and the counter follows that answer rather than
	//! the call. See src/BanRecord.h.
	CBanRecord m_bannedList;
	//! This variable is used to keep track of the last time the banned-list was pruned.
	uint64 m_dwLastBannCleanUp;

	//! This is the map of tracked clients.
	CTrackedClientRecord m_trackedClientsList;
	//! This keeps track of the last time the tracked-list was pruned.
	uint64 m_dwLastTrackedCleanUp;

	//! This keeps track of the last time the client-list was pruned.
	uint64 m_dwLastClientCleanUp;

	//! List of unusable sources.
	CDeadSourceList m_deadSources;

	/* Kad Stuff */
	CClientRefSet m_KadSources;
	CClientRef m_pBuddy;
	uint8 m_nBuddyStatus;
	Kademlia::CBuddyRetry m_buddyRetry;
	Kademlia::CBuddyStats m_buddyStats;
	bool m_buddySearchHeld = false; // A due buddy search waits for a shared file.
	std::vector<Kademlia::CServedBuddyLink> m_servedLinks;
	std::map<const CUpDownClient *, CClientRef> m_servedRefs; // Keeps each served client alive.
	Kademlia::CDepartedBuddies m_departedBuddies;
	// Covers the time before relays reach a buddy that left.
	static constexpr std::size_t DepartedBuddiesPerSlot = 2;
	// Slots are kept for NAT-T nodes only once we relay the Kad rendezvous they need.
	static constexpr bool RelaysKadRendezvous = false;
	std::string m_departedNote; // The latest drop for a buddy that left, this minute.
	unsigned m_limitedLinesThisMinute = 0;
	static constexpr unsigned MaxLimitedLinesPerMinute = 10;
	uint64 m_buddyLinkedAt = 0;
	uint64 m_lastBuddyAccounting = 0;
	uint64 m_nextBuddySummary = 0;
	Kademlia::CBuddyRelayCounts m_loggedRelays;
	uint64 m_loggedCallbacksReceived = 0;
	uint64 m_loggedReasksReceived = 0;

	void ReleaseBuddy();
	void StartServedLink(CUpDownClient *client, uint64 now);
	void EndServedLink(const CUpDownClient *client);
	uint32 ServedNatTCount() const;
	bool AdmitsServedBuddy(bool natT) const;
	bool IsServedOrOfferedIP(uint32 ip, const Kademlia::CUInt128 &requester) const;
	bool CanLogBuddyDrop() const;
	void LogBuddyDrop(wxString line, const uint8_t *buddyID);
	void EvictOldestBuddyOffer();
	const Kademlia::CServedBuddyLink *FindServedLink(const CUpDownClient *client) const;
	bool IsServedBuddyID(const uint8_t *buddyID) const;
	wxString DescribeBuddyTarget(const uint8_t *buddyID) const;
	void AccountBuddyTime(uint64 now);
	void LogBuddySummary(uint64 now);

	Kademlia::CFirewallCheckRequests m_firewallCheckRequests;

	typedef CClientRefList DirectCallbackList;
	DirectCallbackList m_currentDirectCallbacks;
	struct CallbackAddressAndTicks
	{
		CNetworkAddress address;
		uint64 inserted;
	};
	std::list<CallbackAddressAndTicks> m_directCallbackRequests;
};

#endif
// File_checked_for_headers
