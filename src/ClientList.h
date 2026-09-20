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
#include "CanonicalPeerIndex.h"
#include "TrackedClientRecord.h"

#include <deque>
#include <set>

class CUpDownClient;
class CClientTCPSocket;
class CMD4Hash;
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
	bool AttachToAlreadyKnown(
		CUpDownClient **client, CClientTCPSocket *sender, bool *senderDiscarded = nullptr);

	/**
	 * Finds a client with the specified ip and port.
	 */
	CUpDownClient *FindClientByIP(uint32 clientip, uint16 port);
	CUpDownClient *FindClientByIP(const CNetworkAddress &address, uint16 port);

	/**
	 * Finds a client with the specified ip, returning the first if several share it.
	 */
	CUpDownClient *FindClientByIP(uint32 clientip);
	CUpDownClient *FindClientByIP(const CNetworkAddress &address);

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
	ChatSendResult SendChatMessage(const CMD4Hash &peer, const wxString &message);
	CUpDownClient *FindChatClient(const CMD4Hash &peer) const;
	CMD4Hash ResolveLegacyChatPeer(uint64 gui_id) const;

	/**
	 * Stops a chat session with a client.
	 */
	void SetChatState(const CMD4Hash &peer, uint8 state);

	uint8 GetBuddyStatus() const { return m_nBuddyStatus; }
	// This must be used on CreateKadSourceLink and if we ever add the columns
	// on shared files control.
	CUpDownClient *GetBuddy() { return m_pBuddy.GetClient(); }
	uint32 GetBuddyIP();
	uint16 GetBuddyPort();
	bool RequestTCP(Kademlia::CContact *contact, uint8_t connectOptions);
	void RequestBuddy(Kademlia::CContact *contact, uint8_t connectOptions);
	bool IncomingBuddy(Kademlia::CContact *contact, Kademlia::CUInt128 *buddyID);
	void RemoveFromKadList(CUpDownClient *torem);
	void AddToKadList(CUpDownClient *toadd);
	bool DoRequestFirewallCheckUDP(const Kademlia::CContact &contact);

	void AddKadFirewallRequest(uint32 ip);
	bool IsKadFirewallCheckIP(uint32 ip) const;

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
	CUpDownClient *FindMatchingClient(CUpDownClient *client);

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

	typedef struct
	{
		uint32 ip;
		uint64 inserted;
	} IpAndTicks;
	typedef std::list<IpAndTicks> IpAndTicksList;
	IpAndTicksList m_firewallCheckRequests;

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
