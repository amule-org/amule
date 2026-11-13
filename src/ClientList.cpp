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

#include "ClientList.h"           // Interface declarations.
#include "ClientMatchPolicy.h"    // Needed for IsOtherPeerAtSameEndpoint
#include "KadBuddySearchPolicy.h" // Needed for BuddySearchTarget
#include "KadCallbackPolicy.h"
#include "KadServedBuddyPolicy.h" // Needed for the served-buddy limits

#include "BrowseManager.h"

#include <protocol/Protocols.h>
#include <protocol/ed2k/Constants.h>
#include <protocol/kad/Client2Client/UDP.h>
#include <protocol/kad/Constants.h>
#include <protocol/kad2/Client2Client/TCP.h>

#include "amule.h"            // Needed for theApp
#include "ChatSessionStore.h" // Needed for CChatSessionStore
#include "ClientTCPSocket.h"  // Needed for CClientTCPSocket
#include <algorithm>
#include <cstring>
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
#include "ClientUDPSocket.h"      // Needed to clear callback failure after verified HELLO
#include "LugdunumNatTraversal.h" // Needed for verified callback failure clearing
#endif
#include "DownloadQueue.h"  // Needed for CDownloadQueue
#include "UploadQueue.h"    // Needed for CUploadQueue
#include "SharedFileList.h" // Needed for CSharedFileList
#include "IPFilter.h"       // Needed for CIPFIlter
#include "updownclient.h"   // Needed for CUpDownClient
#include "Preferences.h"    // Needed for thePrefs
#include "Statistics.h"     // Needed for theStats
#include "Logger.h"
#include "GuiEvents.h" // Needed for Notify_*
#include "Packet.h"
#include "UtpDialPolicy.h" // Needed for simultaneous uTP tie-breaking

#include <common/Format.h>

#include "kademlia/kademlia/Search.h"
#include "kademlia/kademlia/SearchManager.h"
#include "kademlia/kademlia/UDPFirewallTester.h"
#include "kademlia/net/KademliaUDPListener.h"
#include "kademlia/routing/Contact.h"

CClientList::CClientList()
: m_deadSources(true)
{
	m_dwLastBannCleanUp = 0;
	m_dwLastTrackedCleanUp = 0;
	m_dwLastClientCleanUp = 0;
	m_nBuddyStatus = Disconnected;
}

CClientList::~CClientList()
{
	wxASSERT(m_clientList.empty());
}

CUpDownClient *CClientList::FindReusableClient(const CMD4Hash &hash, uint32 ip, uint16 port)
{
	return FindReusableClient(hash, CNetworkAddress::FromIPv4NetworkOrderOrAbsent(ip), port);
}

CUpDownClient *CClientList::FindReusableClient(
	const CMD4Hash &hash, const CNetworkAddress &address, uint16 port)
{
	// Every client at this address, not just the first. FindClientByIP() stops at the first
	// port match, which may be an unrelated client holding an address our peer used to have;
	// rejecting that one without looking further would allocate a new object on every call.
	auto range = m_ipList.equal_range(PeerAddressing::IndexKey(address));
	for (; range.first != range.second; ++range.first) {
		CUpDownClient *cur_client = range.first->second.GetClient();
		if (cur_client->GetUserPort() != port) {
			continue;
		}
		// An endpoint alone cannot establish continuity with a transient client.
		// Only an identified hash or an explicitly bound expected hash can.
		if (!hash.IsEmpty() &&
			(cur_client->GetUserHash() == hash ||
				(!cur_client->HasValidHash() && cur_client->GetChatPeer().Hash() == hash))) {
			return cur_client;
		}
	}
	return nullptr;
}

CClientRef CClientList::CreateForAddress(const CMD4Hash &hash, uint32 ip, uint16 port, const wxString &name)
{
	// Reuse the client we already hold for this peer, so a repeated action
	// does not stack up objects that no later lookup matches.
	CUpDownClient *client = nullptr;
	if (!hash.IsEmpty()) {
		const SourceList byHash = GetClientsByHash(hash);
		if (!byHash.empty()) {
			client = byHash.front().GetClient();
		}
	}
	if (client == nullptr) {
		// An address alone identifies a peer only while nothing contradicts it. A stored
		// address goes stale, and handing an unrelated client to CFriend::LinkClient()
		// copies the stranger's hash into the friend record and saves it, losing the friend
		// for good.
		client = FindReusableClient(hash, ip, port);
	}
	if (client != nullptr) {
		if (theApp->chatsessions && !client->HasValidHash()) {
			client->BindChatPeer(theApp->chatsessions->Open(
				hash, client->GetUserAddress(), client->GetUserPort(), name));
		}
		return CCLIENTREF(client, wxT("CClientList::CreateForAddress"));
	}

	if (ip == 0 || port == 0) {
		// Nothing held for this peer and nowhere to dial: an invented client would only
		// ever target 0.0.0.0, and AddClient() will not index a zero address, so the next
		// lookup would miss it and make another. Callers ask IsLinked() rather than
		// assuming they got one.
		return CClientRef();
	}

	client = new CUpDownClient(port, ip, 0, 0, nullptr, true, true);
	// The ctor only records the address to connect to, leaving GetIP() at 0. Seed it, or
	// anything that reads the peer's IP back -- a friend record saving itself, a menu deciding
	// whether it can message -- sees 0.0.0.0.
	client->SetIP(ip);
	client->SetUserName(name);
	if (theApp->chatsessions) {
		client->BindChatPeer(theApp->chatsessions->Open(hash, client->GetUserAddress(), port, name));
	}
	// The hash is deliberately NOT seeded. This client has never connected, so it carries no
	// credits and reports zero for every lifetime total, while a hash is exactly what makes the
	// Clients page treat it as a peer whose totals are known: it would publish those zeroes
	// over the stored Total Up / Down of the row that asked for it, and mark that row online.
	// The handshake sets the real hash when the peer answers.
	AddClient(client);
	return CCLIENTREF(client, wxT("CClientList::CreateForAddress"));
}

void CClientList::AddClient(CUpDownClient *toadd)
{
	if (toadd->GetClientState() == CS_NEW) {
		toadd->m_clientState = CS_LISTED;

		// We always add the ID/ptr pair, regardless of the actual ID value
		m_clientList.insert(IDMapPair(toadd->GetUserIDHybrid(),
			CCLIENTREF(toadd, "CClientList::AddClient m_clientList.insert")));

		m_ipList.Insert(
			toadd->GetUserAddress(), CCLIENTREF(toadd, "CClientList::AddClient m_ipList.insert"));

		// We only add the hash if it is valid
		if (toadd->HasValidHash()) {
			m_hashList.insert(HashMapPair(toadd->GetUserHash(),
				CCLIENTREF(toadd, "CClientList::AddClient m_hashList.insert")));
		}

		toadd->UpdateStats();
	}
}

void CClientList::RemoveClient(CUpDownClient *client)
{
	RemoveFromKadList(client);
	RemoveDirectCallback(client);
	// Drop any browse of this client: the manager holds a reference, and the client is going
	// away, so there is nothing left to report a result to. Guarded like the clientlist call in
	// CUpDownClient::Safe_Delete: clients are still being reaped while the app tears itself
	// down.
	if (theApp->browsemanager) {
		theApp->browsemanager->Forget(client);
	}

	if (RemoveIDFromList(client)) {
		RemoveIPFromList(client);
		RemoveHashFromList(client);
	}
}

void CClientList::UpdateClientID(CUpDownClient *client, uint32 newID)
{
	if ((client->GetClientState() != CS_LISTED) || (client->GetUserIDHybrid() == newID))
		return;

	RemoveIDFromList(client);

	m_clientList.insert(IDMapPair(newID, CCLIENTREF(client, "CClientList::UpdateClientID")));
}

void CClientList::UpdateClientIP(CUpDownClient *client, const CNetworkAddress &address)
{
	if (client->GetClientState() != CS_LISTED)
		return;

	m_ipList.Update(client->GetUserAddress(),
		address,
		CCLIENTREF(client, "CClientList::UpdateClientIP"),
		[client](const CClientRef &entry) { return entry.GetClient() == client; });
}

void CClientList::UpdateClientHash(CUpDownClient *client, const CMD4Hash &newHash)
{
	if ((client->GetClientState() != CS_LISTED) || (client->GetUserHash() == newHash))
		return;

	RemoveHashFromList(client);

	if (!newHash.IsEmpty()) {
		m_hashList.insert(HashMapPair(newHash, CCLIENTREF(client, "CClientList::UpdateClientHash")));
	}
}

bool CClientList::RemoveIDFromList(CUpDownClient *client)
{
	bool result = false;

	std::pair<IDMap::iterator, IDMap::iterator> range =
		m_clientList.equal_range(client->GetUserIDHybrid());

	for (; range.first != range.second; ++range.first) {
		if (client == range.first->second.GetClient()) {
			/* erase() will invalidate the iterator, but we're not using it anymore
			    anyway (notice the break;) */
			m_clientList.erase(range.first);
			result = true;

			break;
		}
	}

	return result;
}

void CClientList::RemoveIPFromList(CUpDownClient *client)
{
	m_ipList.Remove(client->GetUserAddress(),
		[client](const CClientRef &entry) { return entry.GetClient() == client; });
}

void CClientList::RemoveHashFromList(CUpDownClient *client)
{
	if (!client->HasValidHash()) {
		return;
	}

	std::pair<HashMap::iterator, HashMap::iterator> range = m_hashList.equal_range(client->GetUserHash());

	for (; range.first != range.second; ++range.first) {
		if (client == range.first->second.GetClient()) {
			/* erase() will invalidate the iterator, but we're not using it anymore
			    anyway (notice the break;) */
			m_hashList.erase(range.first);
			break;
		}
	}
}

CUpDownClient *CClientList::FindMatchingClient(CUpDownClient *client, const CUpDownClient *excluded)
{
	typedef std::pair<IDMap::const_iterator, IDMap::const_iterator> IDMapIteratorPair;
	wxCHECK(client, NULL);

	const CNetworkAddress userIP = PeerAddressing::IndexKey(client->GetUserAddress());
	const uint32 userID = client->GetUserIDHybrid();
	const uint16 userPort = client->GetUserPort();
	const uint16 userKadPort = client->GetKadPort();

	// Excluded, or another peer behind the same address (IsOtherPeerAtSameEndpoint).
	const auto skip = [client, excluded](const CUpDownClient *other) {
		if (other == excluded) {
			return true;
		}
		SClientMatchFacts facts;
		facts.incomingHasHash = client->HasValidHash();
		facts.candidateHasHash = other->HasValidHash();
		facts.incomingHashFromHello = client->GetCreditsHash() != nullptr;
		facts.candidateHashFromHello = other->GetCreditsHash() != nullptr;
		facts.sameHash = client->GetUserHash() == other->GetUserHash();
		return IsOtherPeerAtSameEndpoint(facts);
	};

	// LowID clients need a different set of checks
	if (client->HasLowID()) {
		// User is firewalled ... Must do two checks.
		if (PeerAddressing::IsIndexable(userIP) && (userPort || userKadPort)) {
			auto range = m_ipList.equal_range(userIP);

			for (; range.first != range.second; ++range.first) {
				CUpDownClient *other = range.first->second.GetClient();
				if (skip(other)) {
					continue;
				}
				wxASSERT(userIP == other->GetUserAddress());

				if (userPort && (userPort == other->GetUserPort())) {
					return other;
				} else if (userKadPort && (userKadPort == other->GetKadPort())) {
					return other;
				}
			}
		}

		const uint32 serverIP = client->GetServerIP();
		const uint32 serverPort = client->GetServerPort();
		if (userID && serverIP && serverPort) {
			IDMapIteratorPair range = m_clientList.equal_range(userID);

			for (; range.first != range.second; ++range.first) {
				CUpDownClient *other = range.first->second.GetClient();
				if (skip(other)) {
					continue;
				}
				wxASSERT(userID == other->GetUserIDHybrid());

				// For lowid, we also have to check the server
				if (serverIP == other->GetServerIP()) {
					if (serverPort == other->GetServerPort()) {
						return other;
					}
				}
			}
		}
	} else if (userPort || userKadPort) {
		// Check by address first, then by ID. Within each range all TCP-port
		// matches take precedence over Kad-port matches, as in the IPv4 index.
		const auto findByPort = [userPort, userKadPort, &skip](const auto &range) -> CUpDownClient * {
			if (userPort) {
				for (auto it = range.first; it != range.second; ++it) {
					if (!skip(it->second.GetClient()) &&
						userPort == it->second.GetUserPort()) {
						return it->second.GetClient();
					}
				}
			}
			if (userKadPort) {
				for (auto it = range.first; it != range.second; ++it) {
					if (!skip(it->second.GetClient()) &&
						userKadPort == it->second.GetClient()->GetKadPort()) {
						return it->second.GetClient();
					}
				}
			}
			return nullptr;
		};

		if (PeerAddressing::IsIndexable(userIP)) {
			if (CUpDownClient *match = findByPort(m_ipList.equal_range(userIP))) {
				return match;
			}
		}
		if (userID) {
			if (CUpDownClient *match = findByPort(m_clientList.equal_range(userID))) {
				return match;
			}
		}
	}

	// If anything else fails, then we look at hashes
	if (client->HasValidHash()) {
		std::pair<HashMap::iterator, HashMap::iterator> range =
			m_hashList.equal_range(client->GetUserHash());

		for (; range.first != range.second; ++range.first) {
			if (range.first->second.GetClient() != excluded) {
				return range.first->second.GetClient();
			}
		}
	}

	return NULL;
}

uint32 CClientList::GetClientCount() const
{
	return m_clientList.size();
}

void CClientList::DeleteAll()
{
	m_ipList.clear();
	m_hashList.clear();

	while (!m_clientList.empty()) {
		IDMap::iterator it = m_clientList.begin();

		// Will call the removal of the item on this same class
		it->second.GetClient()->Disconnected("Removed while deleting all from ClientList.");
		it->second.GetClient()->Safe_Delete();
	}
}

bool CClientList::AttachToAlreadyKnown(
	CUpDownClient **client, CClientTCPSocket *sender, bool *senderDiscarded, bool matchOthersOnly)
{
	CUpDownClient *tocheck = (*client);

	CUpDownClient *found_client = FindMatchingClient(tocheck, matchOthersOnly ? tocheck : nullptr);
#ifdef AMULE_UTP_TRANSPORT
	const uint32_t incomingSynWinnerToken = sender != nullptr ? sender->TakeUtpIncomingSynWinner() : 0;
	CUpDownClient *outgoingSynCandidate =
		incomingSynWinnerToken != 0 ? FindClientByECID(incomingSynWinnerToken) : nullptr;
	bool incomingSynWinner = false;
	if (found_client != nullptr && tocheck != found_client) {
		incomingSynWinner = incomingSynWinnerToken == found_client->ECID();
	}
#endif

	if (tocheck == found_client) {
		// We found the same client instance (client may have sent more than one OP_HELLO). do not
		// delete that client!
		return true;
	}

	// Decided before the attach changes anything: the known client's own data stays as it was, and
	// its contact times out and moves on as when nobody answers.
	if (found_client != nullptr && sender != nullptr &&
		PeerAddressing::DecideHelloAttach(found_client->IsRouteUnproven(),
			found_client->HasValidHash() && tocheck->HasValidHash(),
			found_client->GetUserHash() == tocheck->GetUserHash()) ==
			PeerAddressing::EHelloAttach::KeepApart) {
		theStats::AddSourceRouteEvent(ESourceRouteEvent::OtherHostKeptApart);
		AddDebugLogLineN(logSourceRoutes,
			CFormat("Another host (hash %s) answered unproven route to source %s at %s: kept "
				"apart") %
				tocheck->GetUserHash().Encode() % found_client->GetUserHash().Encode() %
				tocheck->GetFullIP());
		found_client->NoteOtherHostOnRoute();
		return false;
	}

	if (found_client != NULL) {
#ifdef AMULE_UTP_TRANSPORT
		if (outgoingSynCandidate != nullptr && outgoingSynCandidate != found_client &&
			outgoingSynCandidate != tocheck) {
			bool candidateMatchesHello = false;
#if defined(ENABLE_NATT_SERVER_COORDINATION)
			if (outgoingSynCandidate->IsLugdunumCallbackDial()) {
				const LugdunumNatTraversal::CallbackFailureKey failureKey =
					outgoingSynCandidate->GetLugdunumCallbackFailureKey();
				candidateMatchesHello =
					LugdunumNatTraversal::ShouldClearCallbackFailureAfterHello(
						failureKey, found_client->GetUserIDHybrid());
				if (candidateMatchesHello) {
					LugdunumNatTraversal::ClearCallbackFailureAfterHello(failureKey,
						found_client->GetUserIDHybrid(),
						[this](const LugdunumNatTraversal::CallbackFailureKey &key) {
							if (theApp->clientudp != nullptr) {
								theApp->clientudp
									->ClearLugdunumNatCallbackFailure(
										key);
							}
						});
					uint32 requesterId = 0;
					LugdunumNatTraversal::CallbackFailureKey ignoredKey{};
					outgoingSynCandidate->TakeLugdunumCallbackRequestOnFailure(
						requesterId, ignoredKey);
				}
			}
#endif
			if (!candidateMatchesHello && outgoingSynCandidate->HasValidHash() &&
				found_client->HasValidHash()) {
				candidateMatchesHello =
					std::memcmp(outgoingSynCandidate->GetUserHash().GetHash(),
						found_client->GetUserHash().GetHash(),
						16) == 0;
			}
			if (candidateMatchesHello) {
				outgoingSynCandidate->Safe_Delete();
			}
		}
#endif
		if (sender) {
			bool socketAttachedBySwap = false;
			if (found_client->GetSocket()) {
				if (found_client->IsConnected() &&
					(found_client->GetUserAddress() != tocheck->GetUserAddress() ||
						found_client->GetUserPort() != tocheck->GetUserPort())) {
					// if found_client is connected and has the IS_IDENTIFIED, it's safe
					// to say that the other one is a bad guy -- unless it arrived from
					// another address of the same host, which a ban by address would
					// lock out for good. That connection is still refused below; only
					// the ban is skipped, so a forged hash from that /64 gains nothing
					const CNetworkAddress advertisedIPv6 =
						found_client->HasModIPv6()
							? CNetworkAddress::FromIPv6Bytes(
								  found_client->GetModIPv6())
							: CNetworkAddress::Absent();
					// A dual-stack peer reaches us over either family, and its IPv4 and
					// IPv6 cannot be matched to one host: refused below, but no ban.
					if (found_client->IsIdentified() &&
						!PeerAddressing::IsSameHostAddress(
							found_client->GetUserAddress(),
							tocheck->GetUserAddress(),
							advertisedIPv6) &&
						!PeerAddressing::IsOtherFamily(found_client->GetUserAddress(),
							tocheck->GetUserAddress())) {
#ifdef AMULE_UTP_TRANSPORT
						// A uTP stream we accepted proves its source only as far as a
						// guessable 16-bit ack does: refuse it, but ban no address.
						if (sender != nullptr && sender->IsUtpInbound()) {
							AddDebugLogLineN(logClient,
								"Client: " + tocheck->GetUserName() + "(" +
									tocheck->GetFullIP() +
									"), refused: uTP, hash in use");
							return false;
						}
#endif
						AddDebugLogLineN(logClient,
							"Client: " + tocheck->GetUserName() + "(" +
								tocheck->GetFullIP() +
								"), Banreason: Userhash invalid");
						tocheck->Ban();
						return false;
					}

					AddDebugLogLineN(logClient,
						"WARNING! Found matching client, to a currently connected "
						"client: " +
							tocheck->GetUserName() + "(" + tocheck->GetFullIP() +
							") and " + found_client->GetUserName() + "(" +
							found_client->GetFullIP() + ")");
					return false;
				}
				CClientTCPSocket *foundSocket = found_client->GetSocket();
#ifdef AMULE_UTP_TRANSPORT
				if (foundSocket->HasTransport() && sender->HasTransport() &&
					foundSocket->IsTransportInbound() != sender->IsTransportInbound()) {
					const SUtpHelloAttachmentResult attachment =
						AttachUtpSocketAfterVerifiedHello(
							incomingSynWinner,
							foundSocket->IsTransportInbound(),
							found_client->HasValidHash() &&
								tocheck->HasValidHash(),
							thePrefs::GetUserHash().GetHash(),
							tocheck->GetUserHash().GetHash(),
							found_client->GetDownloadState() == DS_CONNECTED,
							[&]() {
								found_client
									->NoteFileRequestLostToSocketSwap();
							},
							[&]() { foundSocket->Safe_Delete(); },
							[&]() { found_client->SetSocket(sender); });
					if (attachment.decision.keepFound) {
						// `client` aliases sender->m_client, so the survivor
						// must not be written back: ~CClientTCPSocket() would
						// then strip found_client of the socket just kept.
						// Safe_Delete() leaves that member null, and the
						// caller is told to stop touching the socket.
						sender->Safe_Delete();
						tocheck->Safe_Delete();
						if (senderDiscarded != nullptr) {
							*senderDiscarded = true;
						}
						return true;
					}
					socketAttachedBySwap = attachment.socketAttached;
				}
#endif
				if (!socketAttachedBySwap) {
					foundSocket->Safe_Delete();
				}
			}
#ifdef AMULE_UTP_TRANSPORT
			if (!socketAttachedBySwap) {
				found_client->SetSocket(sender);
			}
#else
			found_client->SetSocket(sender);
#endif
			tocheck->SetSocket(NULL);
		}
		*client = 0;
		// An incoming hello sets its own routes; a source list copy has nothing else.
		if (sender == nullptr) {
			found_client->AdoptContactRoutes(*tocheck);
		} else {
			found_client->AdoptKadBuddy(*tocheck);
		}
		tocheck->Safe_Delete();
		*client = found_client;
		return true;
	}

	return false;
}

CUpDownClient *CClientList::FindClientByIP(uint32 clientip, uint16 port)
{
	return FindClientByIP(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(clientip), port);
}

CUpDownClient *CClientList::FindClientByIP(const CNetworkAddress &address, uint16 port)
{
	return FindClientAtEndpoint(address, port, false);
}

CUpDownClient *CClientList::FindCallbackClientByIP(uint32 clientip, uint16 port)
{
	return FindClientAtEndpoint(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(clientip), port, true);
}

CUpDownClient *CClientList::FindClientAtEndpoint(
	const CNetworkAddress &address, uint16 port, bool forCallback)
{
	auto range = m_ipList.equal_range(PeerAddressing::IndexKey(address));

	for (; range.first != range.second; ++range.first) {
		CUpDownClient *cur_client = range.first->second.GetClient();
		if (cur_client->GetUserPort() == port &&
			!SkipsClientAtEndpoint(forCallback, cur_client->IsRouteAnsweredByOtherHost())) {
			return cur_client;
		}
	}

	return NULL;
}

CUpDownClient *CClientList::FindClientByProtocolPeerIdentity(const CProtocolPeerIdentity &identity)
{
	// HighID and LowID clients sit in m_clientList under the hybrid the identity was built from;
	// a HighID source has no user address until its hello, so m_ipList would miss it.
	CUpDownClient *match = nullptr;
	bool ambiguous = false;
	const auto consider = [&](const CClientRef &entry) {
		CUpDownClient *client = entry.GetClient();
		if (client->GetProtocolPeerIdentity() != identity)
			return;
		ambiguous = ambiguous || match != nullptr;
		match = client;
	};
	if (const auto hybrid = identity.TryGetUserIDHybrid()) {
		for (auto range = m_clientList.equal_range(*hybrid); range.first != range.second;
			++range.first)
			consider(range.first->second);
	} else if (identity.GetKind() == CProtocolPeerIdentity::Kind::NativeIPv6) {
		for (auto range = m_ipList.equal_range(identity.Address()); range.first != range.second;
			++range.first)
			consider(range.first->second);
	}
	return ambiguous ? nullptr : match;
}

CUpDownClient *CClientList::FindClientByIP(uint32 clientip)
{
	return FindClientByIP(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(clientip));
}

CUpDownClient *CClientList::FindClientByIP(const CNetworkAddress &address)
{
	auto range = m_ipList.equal_range(PeerAddressing::IndexKey(address));

	return (range.first != range.second) ? range.first->second.GetClient() : NULL;
}

CUpDownClient *CClientList::FindClientByECID(uint32 ecid) const
{
	for (IDMap::const_iterator it = m_clientList.begin(); it != m_clientList.end(); ++it) {
		if (it->second.ECID() == ecid) {
			return it->second.GetClient();
		}
	}

	return NULL;
}

bool CClientList::IsIPAlreadyKnown(uint32_t ip)
{
	return FindClientByIP(ip) != nullptr;
}

bool CClientList::ComparePriorUserhash(const CNetworkAddress &address, uint16 nPort, void *pNewHash)
{
	return m_trackedClientsList.Compare(address, nPort, pNewHash);
}

void CClientList::AddTrackClient(CUpDownClient *toadd)
{
	const CNetworkAddress address = toadd->GetTrackingAddress();
	if (address.IsAbsent()) {
		return;
	}
	m_trackedClientsList.Add(address, toadd->GetUserPort(), toadd->GetCreditsHash(), ::GetTickCount64());
}

void CClientList::Process()
{
	const uint64 cur_tick = ::GetTickCount64();

	if (m_dwLastBannCleanUp + BAN_CLEANUP_TIME < cur_tick) {
		m_dwLastBannCleanUp = cur_tick;

		// One decrement per entry the sweep actually dropped. The record counts them
		// because it is the only thing that knows which were lapsed.
		const std::size_t dropped = m_bannedList.DropLapsed(cur_tick);
		for (std::size_t i = 0; i < dropped; ++i) {
			theStats::RemoveBannedClient();
		}
	}

	if (m_dwLastTrackedCleanUp + TRACKED_CLEANUP_TIME < cur_tick) {
		m_dwLastTrackedCleanUp = cur_tick;

		m_trackedClientsList.DropLapsed(cur_tick, KEEPTRACK_TIME);
	}

	// Try to connect to the clients in m_KadList. If connected, remove them from the list and
	// send a message back to Kad so we can send an ACK; if not, the client is removed and the
	// socket timeout deletes the object.

	// buddy is just a flag that is used to make sure we are still connected or connecting to a buddy.
	buddyState buddy = Disconnected;

	CClientRefSet::iterator current_it = m_KadSources.begin();
	while (current_it != m_KadSources.end()) {
		CUpDownClient *cur_client = current_it->GetClient();
		++current_it; // Won't be used anymore till while loop
		if (!Kademlia::CKademlia::IsRunning()) {
			// Clear out this list if we stop running Kad.
			// Setting the Kad state to KS_NONE causes it to be removed in the switch below.
			cur_client->SetKadState(KS_NONE);
		}
		switch (cur_client->GetKadState()) {
		case KS_QUEUED_FWCHECK:
		case KS_QUEUED_FWCHECK_UDP:
			// Another client asked us to try to connect to them to check their firewalled status.
			cur_client->TryToConnect(true);
			break;

		case KS_CONNECTING_FWCHECK:
			// Ignore this state as we are just waiting for results.
			break;

		case KS_FWCHECK_UDP:
		case KS_CONNECTING_FWCHECK_UDP:
			// We want a UDP firewallcheck from this client and are just waiting to get connected
			// to send the request
			break;

		case KS_CONNECTED_FWCHECK:
			// We successfully connected to the client.
			// We now send a ack to let them know.
			if (cur_client->GetKadVersion() >= 7) {
				// The result is now sent over TCP instead of UDP, because UDP fails
				// if our internal port is unreachable. We want the TCP test result
				// regardless of UDP being firewalled; the new UDP state and test
				// take care of the rest.
				wxASSERT(cur_client->IsConnected());
				AddDebugLogLineN(logLocalClient,
					"Local Client: OP_KAD_FWTCPCHECK_ACK to " +
						Uint32toStringIP(cur_client->GetIP()));
				CPacket *packet = new CPacket(OP_KAD_FWTCPCHECK_ACK, 0, OP_EMULEPROT);
				cur_client->SafeSendPacket(packet);
			} else {
				AddDebugLogLineN(logClientKadUDP,
					"KadFirewalledAckRes to " +
						Uint32_16toStringIP_Port(
							cur_client->GetIP(), cur_client->GetKadPort()));
				Kademlia::CKademlia::GetUDPListener()->SendNullPacket(
					KADEMLIA_FIREWALLED_ACK_RES,
					wxUINT32_SWAP_ALWAYS(cur_client->GetIP()),
					cur_client->GetKadPort(),
					0,
					NULL);
			}
			// We are done with this client. Set Kad status to KS_NONE and it will be removed in
			// the next cycle.
			cur_client->SetKadState(KS_NONE);
			break;

		case KS_INCOMING_BUDDY:
			// A firewalled client wants us to be his buddy. If it linked through another
			// offer, we already have a buddy, or it took too long to connect, set Kad state
			// to KS_NONE and it is removed next cycle; if not, this client changes to
			// KS_CONNECTED_BUDDY when it connects.
			if (IsServedBuddyID(cur_client->GetBuddyID())) {
				++m_buddyStats.serving.offersDuplicate;
				AddDebugLogLineN(logKadBuddy,
					"Buddy offer to " + cur_client->GetFullIP() +
						" dropped: the node linked through another offer");
				cur_client->SetKadState(KS_NONE);
			} else if (!AdmitsServedBuddy(cur_client->IsServedBuddyNatT())) {
				++m_buddyStats.serving.offersSuperseded;
				AddDebugLogLineN(logKadBuddy,
					"Buddy offer to " + cur_client->GetFullIP() +
						" withdrawn: the slots it may take filled first");
				cur_client->SetKadState(KS_NONE);
			} else if (cur_client->HasBuddyRequestExpired()) {
				++m_buddyStats.serving.offersExpired;
				AddDebugLogLineN(logKadBuddy,
					"Buddy offer to " + cur_client->GetFullIP() +
						" expired: the node did not connect in time");
				cur_client->SetKadState(KS_NONE);
			}
			break;

		case KS_QUEUED_BUDDY:
			// We are firewalled and want this client as a buddy, but only if we are not
			// already trying another. Already connected to a buddy: set KS_NONE and it
			// goes next cycle. Already trying one: ignore this client, since the
			// attempt in flight may still fail.
			if (m_nBuddyStatus == Disconnected) {
				buddy = Connecting;
				m_nBuddyStatus = Connecting;
				++m_buddyStats.own.connectAttempts;
				AddDebugLogLineN(
					logKadBuddy, "Connecting to buddy " + cur_client->GetFullIP());
				cur_client->SetKadState(KS_CONNECTING_BUDDY);
				cur_client->TryToConnect(true);
				Notify_ServerUpdateED2KInfo();
			} else {
				if (m_nBuddyStatus == Connected) {
					cur_client->SetKadState(KS_NONE);
				}
			}
			break;

		case KS_CONNECTING_BUDDY:
			// We are trying to connect to this client. It should not happen, but make
			// sure we are not already connected to a buddy -- if we are, set KS_NONE
			// for next cycle -- and otherwise flag connecting.
			if (m_nBuddyStatus == Connected) {
				cur_client->SetKadState(KS_NONE);
			} else {
				wxASSERT(m_nBuddyStatus == Connecting);
				buddy = Connecting;
			}
			break;

		case KS_CONNECTED_BUDDY:
			if (cur_client->IsServedBuddy()) {
				// As stock eMule: a node with a HighID is reached directly and needs no
				// buddy.
				if (!cur_client->HasLowID()) {
					if (FindServedLink(cur_client) == nullptr) {
						++m_buddyStats.serving.releasedExtra;
					}
					AddDebugLogLineN(logKadBuddy,
						"Released buddy " + cur_client->GetFullIP() +
							": it has a HighID");
					cur_client->SetKadState(KS_NONE);
					break;
				}
				if (FindServedLink(cur_client) != nullptr) {
					break;
				}
				const bool sameIP = std::any_of(m_servedRefs.begin(),
					m_servedRefs.end(),
					[cur_client](const auto &served) {
						return served.second.GetClient()->GetIP() ==
						       cur_client->GetIP();
					});
				if (sameIP || !AdmitsServedBuddy(cur_client->IsServedBuddyNatT())) {
					++m_buddyStats.serving.releasedExtra;
					AddDebugLogLineN(logKadBuddy,
						"Released extra buddy " + cur_client->GetFullIP() +
							(sameIP ? wxString(": we already serve a buddy at "
									   "that address")
								: wxString(": the slots it may take are in "
									   "use")));
					cur_client->SetKadState(KS_NONE);
					break;
				}
				m_servedRefs.emplace(cur_client,
					CCLIENTREF(cur_client, "CClientList::Process served buddy"));
				StartServedLink(cur_client, cur_tick);
				Notify_ServerUpdateED2KInfo();
				break;
			}
			// Connected in the same cycle as our buddy. Without buddy state, an idle socket
			// closes on the ordinary timeout and a transfer on it carries on. A linked buddy
			// that is on its way out is about to be replaced by this client instead.
			if (m_nBuddyStatus == Connected && m_pBuddy.IsLinked() &&
				m_pBuddy.GetClient() != cur_client &&
				m_pBuddy.GetClient()->GetKadState() == KS_CONNECTED_BUDDY) {
				AddDebugLogLineN(logKadBuddy,
					"Released extra buddy " + cur_client->GetFullIP() + ": " +
						m_pBuddy.GetClient()->GetFullIP() + " is already linked");
				cur_client->SetKadState(KS_NONE);
				break;
			}

			// A potential connected buddy client wanting to me in the Kad network
			// We set our flag to connected to make sure things are still working correctly.
			buddy = Connected;

			// If m_nBuddyStatus is not connected already, we set this client as our buddy!
			if (m_nBuddyStatus != Connected) {
				m_pBuddy.Link(cur_client CLIENT_DEBUGSTRING(
					"CClientList::Process KS_CONNECTED_BUDDY m_pBuddy.Link"));
				m_nBuddyStatus = Connected;
				m_buddyLinkedAt = cur_tick;
				++m_buddyStats.own.connected;
				AddDebugLogLineN(logKadBuddy,
					"Connected to buddy " + cur_client->GetFullIP() + " (" +
						cur_client->GetClientVerString() + ")");
				Notify_ServerUpdateED2KInfo();
			}
			if (m_pBuddy.GetClient() == cur_client && theApp->IsFirewalled() &&
				cur_client->IsBuddyPingDue()) {
				cur_client->SendBuddyPing();
			}
			break;

		default:
			RemoveFromKadList(cur_client);
		}
	}

	// We either never had a buddy, or lost our buddy..
	if (buddy == Disconnected) {
		if (m_nBuddyStatus != Disconnected || m_pBuddy.IsLinked()) {
			const bool udpFirewalled = Kademlia::CUDPFirewallTester::IsFirewalledUDP(true);
			if (m_nBuddyStatus == Connected && Kademlia::CKademlia::IsRunning() &&
				Kademlia::NeedsBuddy(theApp->IsFirewalled(),
					udpFirewalled,
					Kademlia::CUDPFirewallTester::IsVerified())) {
				// We are a lowID client and we just lost our buddy.
				// Go ahead and instantly try to find a new buddy.
				Kademlia::CKademlia::GetPrefs()->SetFindBuddy();
			}
			ReleaseBuddy();
		}
	}

	AccountBuddyTime(cur_tick);
	LogBuddySummary(cur_tick);

	if (Kademlia::CKademlia::IsConnected()) {
		const bool udpFirewalled = Kademlia::CUDPFirewallTester::IsFirewalledUDP(true);
		// we only need a buddy if direct callback is not available
		if (Kademlia::NeedsBuddy(Kademlia::CKademlia::IsFirewalled(),
			    udpFirewalled,
			    Kademlia::CUDPFirewallTester::IsVerified())) {
			// Kad buddies do not work with RequireCrypt, so it is disabled here. Buddy
			// connections themselves have supported obfuscation since eMule 0.49a, but
			// callback requests do not, so we could not answer one under RequireCrypt.
			Kademlia::CPrefs *kadPrefs = Kademlia::CKademlia::GetPrefs();
			if (m_nBuddyStatus == Disconnected && kadPrefs->IsFindBuddyDue() &&
				!thePrefs::IsClientCryptLayerRequired()) {
				if (theApp->sharedfiles->GetFileCount() == 0) {
					// With nothing shared, no one calls us back. The search stays due
					// until a file is shared, also if Kad connects before hashing ends.
					if (!m_buddySearchHeld) {
						m_buddySearchHeld = true;
						++m_buddyStats.own.searchesHeld;
						AddDebugLogLineN(
							logKadBuddy, "Buddy search held: no shared file");
					}
				} else {
					kadPrefs->SetFindBuddy(false);
					m_buddySearchHeld = false;
					// We are a firewalled client with no buddy, and we have waited long
					// enough to trust that status.
					const Kademlia::CUInt128 target =
						Kademlia::BuddySearchTarget(kadPrefs->GetKadID(),
							m_buddyStats.own.searches == 0,
							Kademlia::CUInt128(Kademlia::CUInt128(0u), 0));
					if (!Kademlia::CSearchManager::PrepareLookup(
						    Kademlia::CSearch::FINDBUDDY, true, target)) {
						// A search for this target still runs. Retry next time.
						kadPrefs->SetFindBuddy();
					} else {
						++m_buddyStats.own.searches;
						AddDebugLogLineN(logKadBuddy,
							"Started a buddy search for " + target.ToHexString());
					}
				}
			}
		} else {
			if (m_pBuddy.IsLinked()) {
				// If a buddy is not firewalled either, someone has fixed their
				// firewall or stopped saturating their line, so set KS_NONE and let
				// the next cycle clear it up.
				if (!m_pBuddy.HasLowID()) {
					m_pBuddy.GetClient()->SetKadState(KS_NONE);
				}
			}
		}
	} else {
		if (m_pBuddy.IsLinked()) {
			// We are not connected anymore. Just set this buddy to KS_NONE and things will be
			// cleared out on next cycle.
			m_pBuddy.GetClient()->SetKadState(KS_NONE);
		}
		for (auto &served : m_servedRefs) {
			served.second.GetClient()->SetKadState(KS_NONE);
		}
	}

	CleanUpClientList();
	ProcessDirectCallbackList();
	theApp->browsemanager->Process(cur_tick);
}

void CClientList::AddBannedClient(uint32 dwIP)
{
	AddBannedClient(CNetworkAddress::FromIPv4NetworkOrder(dwIP));
}

void CClientList::AddBannedClient(const CNetworkAddress &address)
{
	// Counted only when the address was not already banned. Ban() overwrote the tick on an
	// address already present and counted it again, and CUpDownClient::SetSpammer(true) calls
	// Ban() with no IsBanned() check, so a client banned for aggressiveness and later flagged
	// as a spammer counted twice while UnBan() gave back one.
	if (m_bannedList.Ban(address, ::GetTickCount64())) {
		theStats::AddBannedClient();
	}
}

bool CClientList::IsBannedClient(uint32 dwIP)
{
	return IsBannedClient(CNetworkAddress::FromIPv4NetworkOrder(dwIP));
}

bool CClientList::IsBannedClient(const CNetworkAddress &address)
{
	// A lapsed ban is dropped inside the lookup, so the decrement has to
	// follow what the lookup did rather than the answer it gave.
	bool dropped = false;
	const bool banned = m_bannedList.IsBanned(address, ::GetTickCount64(), &dropped);
	if (dropped) {
		theStats::RemoveBannedClient();
	}
	return banned;
}

void CClientList::RemoveBannedClient(uint32 dwIP)
{
	RemoveBannedClient(CNetworkAddress::FromIPv4NetworkOrder(dwIP));
}

void CClientList::RemoveBannedClient(const CNetworkAddress &address)
{
	// The mirror of the add path: erase() removed nothing when the address
	// was not banned, and the count followed the call anyway.
	if (m_bannedList.Unban(address)) {
		theStats::RemoveBannedClient();
	}
}

void CClientList::FilterQueues()
{
	for (AddressMap::const_iterator it = m_ipList.begin(); it != m_ipList.end();) {
		AddressMap::const_iterator tmp = it++; // Don't change this to a ++it!
		CUpDownClient *client = tmp->second.GetClient();
		if (theApp->ipfilter->IsFiltered(tmp->first)) {
			client->Disconnected("Filtered by IPFilter");
			client->Safe_Delete();
		}
	}
}

CClientList::SourceList CClientList::GetClientsByHash(const CMD4Hash &hash)
{
	SourceList results;

	std::pair<HashMap::iterator, HashMap::iterator> range = m_hashList.equal_range(hash);

	for (; range.first != range.second; ++range.first) {
		results.push_back(range.first->second);
	}

	return results;
}

CClientList::SourceList CClientList::GetClientsByIP(unsigned long ip)
{
	return GetClientsByIP(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(ip));
}

CClientList::SourceList CClientList::GetClientsByIP(const CNetworkAddress &address)
{
	SourceList results;

	auto range = m_ipList.equal_range(PeerAddressing::IndexKey(address));

	for (; range.first != range.second; range.first++) {
		results.push_back(range.first->second);
	}

	return results;
}

const CClientList::IDMap &CClientList::GetClientList()
{
	return m_clientList;
}

#ifdef ENABLE_IPV6
void CClientList::MarkIPv6ChangeNotificationPending()
{
	for (const auto &entry : m_clientList) {
		entry.second.GetClient()->MarkIPv6ChangeNotificationPending();
	}
}
#endif

void CClientList::AddDeadSource(const CUpDownClient *client)
{
	m_deadSources.AddDeadSource(client);
}

bool CClientList::IsDeadSource(const CUpDownClient *client)
{
	return m_deadSources.IsDeadSource(client);
}

CUpDownClient *CClientList::FindChatClient(const CChatPeer &peer) const
{
	if (peer.IsEmpty()) {
		return nullptr;
	}
	CUpDownClient *found = nullptr;
	for (const auto &entry : m_clientList) {
		CUpDownClient *client = entry.second.GetClient();
		if (client->GetChatPeer() == peer &&
			(!client->HasValidHash() || client->GetUserHash() == peer.Hash())) {
			if (client->IsConnected()) {
				return client;
			}
			found = client;
		}
	}
	return found;
}

CChatPeer CClientList::ResolveLegacyChatPeer(uint64 gui_id) const
{
	CChatPeer peer;
	if (!gui_id || !IP_FROM_GUI_ID(gui_id) || !PORT_FROM_GUI_ID(gui_id)) {
		return peer;
	}
	bool ownedByConversation = false;
	if (theApp->chatsessions) {
		bool ambiguous = false;
		if (const auto *session = theApp->chatsessions->FindLegacy(gui_id, &ambiguous)) {
			peer = session->peer;
			ownedByConversation = true;
		} else if (ambiguous) {
			return CChatPeer();
		}
	}
	for (const auto &entry : m_clientList) {
		CUpDownClient *client = entry.second.GetClient();
		if (client->GetIP() == IP_FROM_GUI_ID(gui_id) &&
			client->GetUserPort() == PORT_FROM_GUI_ID(gui_id)) {
			if (!client->HasValidHash()) {
				continue; // Transient occupants do not contradict an identified session.
			}
			if (!peer.IsEmpty() && peer.Hash() != client->GetUserHash()) {
				return CChatPeer();
			}
			peer = client->GetChatPeer();
		}
	}
	// A conversation that owns this GUI_ID reaches its peer wherever the peer is now: a legacy
	// client replying under a route the peer has since left means that peer, as in 3.1.0.
	const CUpDownClient *client = FindChatClient(peer);
	if (!ownedByConversation && client &&
		(!client->GetUserAddress().IsIPv4() || client->GetIP() != IP_FROM_GUI_ID(gui_id) ||
			client->GetUserPort() != PORT_FROM_GUI_ID(gui_id))) {
		return CChatPeer();
	}
	if (peer.IsEmpty()) {
		// An explicit legacy address target may be a peer the daemon has never seen. Keep the
		// route as a provisional identity so SendChatMessage() can create the dialing client;
		// this is not a claim that the endpoint identifies the peer permanently.
		return CChatPeer(CMD4Hash(),
			CNetworkAddress::FromIPv4NetworkOrderOrAbsent(IP_FROM_GUI_ID(gui_id)),
			PORT_FROM_GUI_ID(gui_id));
	}
	return peer;
}

CClientList::ChatSendResult CClientList::SendChatMessage(
	const CChatPeer &peer, const wxString &message, uint64 legacyRoute)
{
	if (peer.IsEmpty() ||
		(peer.Hash().IsEmpty() && (!theApp->chatsessions || !theApp->chatsessions->Find(peer)))) {
		return ChatSendResult::Unavailable;
	}
	CUpDownClient *client = FindChatClient(peer);
	CClientRef routeClient;
	if (!client && theApp->chatsessions) {
		const auto *session = theApp->chatsessions->Find(peer);
		const auto address = session ? session->address : peer.Address();
		const uint16 port = session ? session->port : peer.Port();
		if (address.IsIPv4()) {
			const wxString name = session ? session->name : wxString();
			if (peer.Hash().IsEmpty() && port && address.ToIPv4NetworkOrderOrZero()) {
				// Reconnect this handle, not a new route-only session at the same endpoint.
				client = new CUpDownClient(
					port, address.ToIPv4NetworkOrderOrZero(), 0, 0, nullptr, true, true);
				client->SetIP(address.ToIPv4NetworkOrderOrZero());
				client->SetUserName(name);
				client->BindChatPeer(peer);
				AddClient(client);
				routeClient = CCLIENTREF(client, wxT("CClientList::SendChatMessage"));
			} else {
				routeClient = CreateForAddress(
					peer.Hash(), address.ToIPv4NetworkOrderOrZero(), port, name);
			}
			client = routeClient.GetClient();
			if (client && !client->HasValidHash()) {
				client->BindChatPeer(peer);
			}
		}
	}
	if (!client || (client->HasValidHash() && client->GetUserHash() != peer.Hash())) {
		return ChatSendResult::Unavailable;
	}
	// Record before sending, and regardless of the result: a false return from
	// CUpDownClient::SendChatMessage means "queued while connecting", not "failed", so gating
	// the store on it would drop exactly the messages a slow peer receives a moment later.
	if (theApp->chatsessions) {
		theApp->chatsessions->AddOutgoing(
			peer, message, client->GetUserAddress(), client->GetUserPort(), legacyRoute);
	}
	return client->SendChatMessage(message) ? ChatSendResult::Sent : ChatSendResult::Queued;
}

void CClientList::CloseChatSession(const CChatPeer &peer)
{
	if (theApp->chatsessions) {
		theApp->chatsessions->CloseSession(peer);
	}
	SetChatState(peer, MS_NONE);
	Notify_Chat_SessionRemoved(peer);
}

void CClientList::SetChatState(const CChatPeer &peer, uint8 state)
{
	CUpDownClient *client = FindChatClient(peer);
	if (client) {
		client->SetChatState(state);
	}
}

/* Kad stuff */

bool CClientList::RequestTCP(Kademlia::CContact *contact, uint8_t connectOptions)
{
	uint32_t nContactIP = wxUINT32_SWAP_ALWAYS(contact->GetIPAddress());
	// don't connect ourself
	if (theApp->GetPublicIP() == nContactIP && thePrefs::GetPort() == contact->GetTCPPort()) {
		return false;
	}

	CUpDownClient *pNewClient = FindClientByIP(nContactIP, contact->GetTCPPort());

	if (!pNewClient) {
		// #warning Do we actually have to check friendstate here?
		pNewClient = new CUpDownClient(
			contact->GetTCPPort(), contact->GetIPAddress(), 0, 0, NULL, false, true);
	} else if (pNewClient->GetKadState() != KS_NONE) {
		return false; // already busy with this client in some way (probably buddy stuff), don't mess
			      // with it
	}

	pNewClient->SetKadPort(contact->GetUDPPort());
	pNewClient->SetKadState(KS_QUEUED_FWCHECK);
	if (contact->GetClientID() != 0) {
		uint8_t ID[16];
		contact->GetClientID().ToByteArray(ID);
		pNewClient->SetUserHash(CMD4Hash(ID));
		pNewClient->SetConnectOptions(connectOptions, true, false);
	}
	AddToKadList(pNewClient); // This was a direct adding, but I like to check duplicates
	AddClient(pNewClient);
	return true;
}

bool CClientList::CanRequestBuddy(uint32_t ip) const
{
	return m_buddyRetry.CanRequest(ip, ::GetTickCount64());
}

void CClientList::RecordBuddyFailure(uint32_t ip)
{
	m_buddyRetry.RecordFailure(ip, ::GetTickCount64());
	++m_buddyStats.own.connectFailures;
	AddDebugLogLineN(logKadBuddy,
		"Buddy connection to " + Uint32toStringIP(ip) + " failed; it is not asked again for an hour");
}

uint32 CClientList::GetPendingBuddyCount() const
{
	return static_cast<uint32>(std::count_if(m_KadSources.begin(),
		m_KadSources.end(),
		[](const CClientRef &ref) { return ref.GetClient()->GetKadState() == KS_INCOMING_BUDDY; }));
}

uint64 CClientList::GetServedLinkedMs() const
{
	const uint64 now = ::GetTickCount64();
	uint64 total = 0;
	for (const Kademlia::CServedBuddyLink &link : m_servedLinks) {
		total += now - link.linkedAt;
	}
	return total;
}

uint64 CClientList::GetOwnBuddyLinkedMs() const
{
	return m_pBuddy.IsLinked() ? ::GetTickCount64() - m_buddyLinkedAt : 0;
}

const Kademlia::CServedBuddyLink *CClientList::FindServedLink(const CUpDownClient *client) const
{
	for (const Kademlia::CServedBuddyLink &link : m_servedLinks) {
		if (link.client == client) {
			return &link;
		}
	}
	return nullptr;
}

bool CClientList::IsServedBuddyID(const uint8_t *buddyID) const
{
	return std::any_of(m_servedLinks.begin(),
		m_servedLinks.end(),
		[buddyID](const Kademlia::CServedBuddyLink &link) {
			return std::memcmp(link.buddyId.data(), buddyID, link.buddyId.size()) == 0;
		});
}

CUpDownClient *CClientList::FindServedBuddy(const uint8_t *buddyID) const
{
	for (const Kademlia::CServedBuddyLink &link : m_servedLinks) {
		if (Kademlia::MatchesBuddyIdEitherLayout(buddyID, link.buddyId.data())) {
			const auto ref = m_servedRefs.find(static_cast<const CUpDownClient *>(link.client));
			return ref != m_servedRefs.end() ? ref->second.GetClient() : nullptr;
		}
	}
	return nullptr;
}

void CClientList::EndServedLink(const CUpDownClient *client)
{
	const uint64 now = ::GetTickCount64();
	const auto served = std::find_if(m_servedLinks.begin(),
		m_servedLinks.end(),
		[client](const Kademlia::CServedBuddyLink &link) { return link.client == client; });
	if (served == m_servedLinks.end()) {
		return;
	}
	const uint64 linkedMs = now - served->linkedAt;
	++m_buddyStats.serving.ended;
	m_buddyStats.serving.endedMs += linkedMs;
	AddDebugLogLineN(logKadBuddy,
		wxString::FromUTF8(("Stopped serving buddy " + served->address + " (" + served->software +
				    ") after " + Kademlia::FormatBuddyDuration(linkedMs) + ": " +
				    Kademlia::FormatServedBuddyRelays(*served))
					   .c_str()));
	Kademlia::CDepartedBuddy departed;
	departed.buddyId = served->buddyId;
	departed.address = served->address;
	departed.endedAt = now;
	departed.links = served->linkNumber;
	m_departedBuddies.Record(departed, DepartedBuddiesPerSlot * GetServedBuddyLimit());
	m_servedLinks.erase(served);
	m_servedRefs.erase(client);
	Notify_ServerUpdateED2KInfo();
}

void CClientList::StartServedLink(CUpDownClient *client, uint64 now)
{
	Kademlia::CServedBuddyLink link;
	link.client = client;
	link.buddyId = Kademlia::MakeBuddyId(client->GetBuddyID());
	link.address = std::string(client->GetFullIP().utf8_str());
	link.software = std::string(client->GetClientVerString().utf8_str());
	link.linkedAt = now;
	link.ip = client->GetIP();
	link.natT = client->IsServedBuddyNatT();
	const std::optional<Kademlia::CDepartedBuddy> before =
		m_departedBuddies.Find(link.buddyId.data(), now);
	if (before) {
		link.linkNumber = before->links + 1;
	}
	m_servedLinks.push_back(link);
	++m_buddyStats.serving.started;
	if (theLogger.IsEnabled(logKadBuddy)) {
		std::string line = "Serving buddy " + link.address + " (" + link.software + "), " +
				   std::to_string(m_servedLinks.size()) + " of " +
				   std::to_string(GetServedBuddyLimit()) + (link.natT ? ", NAT-T" : "") +
				   ", BuddyID " + Kademlia::BuddyIdHex(link.buddyId.data());
		if (before) {
			line += ", again: link " + std::to_string(link.linkNumber) + ", the last one ended " +
				Kademlia::FormatBuddyDuration(Kademlia::CDepartedBuddies::Age(*before, now)) +
				" ago";
		}
		AddDebugLogLineN(logKadBuddy, wxString::FromUTF8(line.c_str()));
	}
}

void CClientList::RecordBuddyRelay(const CUpDownClient *buddy, bool callback, uint32 bytes)
{
	Kademlia::CBuddyStats::Serving &serving = m_buddyStats.serving;
	if (callback) {
		++serving.relays.callbacks;
		serving.relays.callbackBytes += bytes;
	} else {
		++serving.relays.reasks;
		serving.relays.reaskBytes += bytes;
	}
	for (Kademlia::CServedBuddyLink &link : m_servedLinks) {
		if (link.client == buddy) {
			serving.peakRelayBytesPerMinute = std::max(serving.peakRelayBytesPerMinute,
				link.Relay(callback, bytes, ::GetTickCount64()));
			break;
		}
	}
}

void CClientList::RecordBuddyRelayDrop(bool callback, const uint8_t *buddyID)
{
	Kademlia::CBuddyRelayCounts &relays = m_buddyStats.serving.relays;
	const bool noBuddy = m_servedLinks.empty();
	if (callback) {
		++(noBuddy ? relays.callbacksNoBuddy : relays.callbacksMismatched);
	} else {
		++(noBuddy ? relays.reasksNoBuddy : relays.reasksMismatched);
	}
	const uint64 now = ::GetTickCount64();
	if (const std::optional<Kademlia::CDepartedBuddy> departed = m_departedBuddies.Find(buddyID, now)) {
		++(callback ? relays.callbacksDeparted : relays.reasksDeparted);
		m_departedNote =
			departed->address + ", " +
			Kademlia::FormatBuddyDuration(Kademlia::CDepartedBuddies::Age(*departed, now)) +
			" ago";
	} else if (theLogger.IsEnabled(logKadBuddy)) {
		// The BuddyID as received, in the sender's layout, so a later run can tell whose it was.
		AddDebugLogLineN(logKadBuddy,
			wxString(callback ? "Callback" : "Reask") + " for an unknown BuddyID " +
				wxString::FromAscii(Kademlia::BuddyIdHex(buddyID).c_str()));
	}
}

wxString CClientList::DescribeBuddyTarget(const uint8_t *buddyID) const
{
	for (const Kademlia::CServedBuddyLink &link : m_servedLinks) {
		if (Kademlia::MatchesBuddyIdEitherLayout(buddyID, link.buddyId.data())) {
			return wxString::FromUTF8(("buddy " + link.address).c_str());
		}
	}
	const uint64 now = ::GetTickCount64();
	if (const std::optional<Kademlia::CDepartedBuddy> departed = m_departedBuddies.Find(buddyID, now)) {
		return wxString::FromUTF8((
			"buddy " + departed->address + ", which left " +
			Kademlia::FormatBuddyDuration(Kademlia::CDepartedBuddies::Age(*departed, now)) +
			" ago")
						  .c_str());
	}
	return "an unknown buddy";
}

void CClientList::LogBuddyPacketLimited(
	const wxString &what, const wxString &from, uint32 counted, uint32 perMinute, const uint8_t *buddyID)
{
	if (!CanLogBuddyDrop()) {
		return;
	}
	wxString line = what + " from " + from;
	if (counted == 0) {
		line += " dropped: the rate limiter tracks too many senders";
	} else {
		line += CFormat(" dropped over the rate limit of %u a minute (%u counted)") % perMinute %
			counted;
	}
	LogBuddyDrop(line, buddyID);
}

void CClientList::LogBuddyCallbackLimited(const wxString &from, const uint8_t *buddyID)
{
	if (!CanLogBuddyDrop()) {
		return;
	}
	LogBuddyDrop(
		"Callback request from " + from + " dropped: one a minute from each sender for each buddy",
		buddyID);
}

bool CClientList::CanLogBuddyDrop() const
{
	return theLogger.IsEnabled(logKadBuddy) && m_limitedLinesThisMinute < MaxLimitedLinesPerMinute;
}

// Callers check CanLogBuddyDrop() first, so a flood builds no line.
void CClientList::LogBuddyDrop(wxString line, const uint8_t *buddyID)
{
	++m_limitedLinesThisMinute;
	if (buddyID != nullptr) {
		line += ", for " + DescribeBuddyTarget(buddyID);
	}
	AddDebugLogLineN(logKadBuddy, line);
}

void CClientList::LogBuddyDisconnect(const CUpDownClient *client, const wxString &reason)
{
	if (!theLogger.IsEnabled(logKadBuddy)) {
		return;
	}
	const uint64 now = ::GetTickCount64();
	const Kademlia::CBuddyLiveness &liveness = client->GetBuddyLiveness();
	wxString line = "Buddy " + client->GetFullIP() + " disconnected";
	wxString detail;
	if (const Kademlia::CServedBuddyLink *link = FindServedLink(client)) {
		line += " after " +
			wxString::FromUTF8(Kademlia::FormatBuddyDuration(now - link->linkedAt).c_str());
		detail = CFormat(", pings %u") % liveness.PingsReceived();
	} else if (m_pBuddy.GetClient() == client) {
		line += " after " +
			wxString::FromUTF8(Kademlia::FormatBuddyDuration(now - m_buddyLinkedAt).c_str());
		detail = CFormat(", pongs %u") % liveness.PongsReceived();
	}
	wxString why = reason;
	why.Trim();
	line += ": " + why + ". Last packet " +
		wxString::FromUTF8(Kademlia::FormatBuddyDuration(liveness.SinceReceived(now)).c_str()) +
		" ago" + detail;
	AddDebugLogLineN(logKadBuddy, line);
}

void CClientList::RecordBuddyTimeout(const CUpDownClient *client)
{
	if (client->IsServedBuddy()) {
		++m_buddyStats.serving.timedOut;
	} else {
		++m_buddyStats.own.timedOut;
	}
	AddDebugLogLineN(logKadBuddy, "Buddy " + client->GetFullIP() + " timed out: it sent nothing");
}

void CClientList::ReleaseBuddy()
{
	const uint64 now = ::GetTickCount64();
	// Only our own buddy: served buddies are linked and ended in m_servedLinks.
	if (const CUpDownClient *buddy = m_pBuddy.GetClient()) {
		const uint64 linkedMs = now - m_buddyLinkedAt;
		++m_buddyStats.own.ended;
		m_buddyStats.own.endedMs += linkedMs;
		AddDebugLogLineN(logKadBuddy,
			"Lost buddy " + buddy->GetFullIP() + " after " +
				wxString::FromUTF8(Kademlia::FormatBuddyDuration(linkedMs).c_str()));
	}
	m_pBuddy.Unlink();
	m_nBuddyStatus = Disconnected;
	Notify_ServerUpdateED2KInfo();
}

void CClientList::AccountBuddyTime(uint64 now)
{
	const uint64 elapsed = m_lastBuddyAccounting != 0 ? now - m_lastBuddyAccounting : 0;
	m_lastBuddyAccounting = now;
	if (Kademlia::CKademlia::IsConnected() && Kademlia::NeedsBuddy(Kademlia::CKademlia::IsFirewalled(),
							  Kademlia::CUDPFirewallTester::IsFirewalledUDP(true),
							  Kademlia::CUDPFirewallTester::IsVerified())) {
		m_buddyStats.own.neededMs += elapsed;
		if (m_nBuddyStatus != Connected) {
			m_buddyStats.own.withoutBuddyMs += elapsed;
		}
	}
}

void CClientList::LogBuddySummary(uint64 now)
{
	if (now < m_nextBuddySummary) {
		return;
	}
	m_nextBuddySummary = now + Kademlia::CMinutePeak::WindowMs;
	// The baselines move even with the category off, so turning it on reports one minute.
	const Kademlia::CBuddyStats::Own &own = m_buddyStats.own;
	if (theLogger.IsEnabled(logKadBuddy)) {
		const std::string relays = Kademlia::FormatBuddyRelaySummary(
			m_buddyStats.serving.relays, m_loggedRelays, m_departedNote);
		if (!relays.empty()) {
			AddDebugLogLineN(logKadBuddy, wxString::FromUTF8(relays.c_str()));
		}
		const uint64 callbacks = own.callbacksReceived - m_loggedCallbacksReceived;
		const uint64 reasks = own.reasksReceived - m_loggedReasksReceived;
		if (callbacks != 0 || reasks != 0) {
			AddDebugLogLineN(logKadBuddy,
				CFormat("Received from our buddy in the last minute: callbacks %u, reasks "
					"%u") %
					static_cast<unsigned>(callbacks) % static_cast<unsigned>(reasks));
		}
	}
	m_loggedRelays = m_buddyStats.serving.relays;
	m_departedNote.clear();
	m_limitedLinesThisMinute = 0;
	m_loggedCallbacksReceived = own.callbacksReceived;
	m_loggedReasksReceived = own.reasksReceived;
}

void CClientList::RequestBuddy(Kademlia::CContact *contact, uint8_t connectOptions)
{
	uint32_t nContactIP = wxUINT32_SWAP_ALWAYS(contact->GetIPAddress());
	// Don't connect to ourself
	if (theApp->GetPublicIP() == nContactIP && thePrefs::GetPort() == contact->GetTCPPort()) {
		return;
	}

	CUpDownClient *pNewClient = FindClientByIP(nContactIP, contact->GetTCPPort());
	if (!pNewClient) {
		pNewClient = new CUpDownClient(
			contact->GetTCPPort(), contact->GetIPAddress(), 0, 0, NULL, false, true);
	} else if (pNewClient->GetKadState() != KS_NONE) {
		return; // already busy with this client in some way (probably fw stuff), don't mess with it
	} else if (IsKadFirewallCheckIP(nContactIP)) { // doing a kad firewall check with this IP, abort
		AddDebugLogLineN(logKadMain,
			"Kad TCP firewallcheck / Buddy request collision for IP " +
				Uint32toStringIP(nContactIP));
		return;
	}

	pNewClient->SetKadPort(contact->GetUDPPort());
	pNewClient->SetServedBuddy(false);
	pNewClient->SetKadState(KS_QUEUED_BUDDY);
	uint8_t ID[16];
	contact->GetClientID().ToByteArray(ID);
	pNewClient->SetUserHash(CMD4Hash(ID));
	pNewClient->SetConnectOptions(connectOptions, true, false);
	AddToKadList(pNewClient);
	AddClient(pNewClient);
}

CClientList::EIncomingBuddy CClientList::IncomingBuddy(
	Kademlia::CContact *contact, Kademlia::CUInt128 *buddyID, bool natT)
{
	if (!AdmitsServedBuddy(natT)) {
		return EIncomingBuddy::Full;
	}
	uint32_t nContactIP = wxUINT32_SWAP_ALWAYS(contact->GetIPAddress());
	// One buddy per address: the TCP connection proves the IP, so this bounds what one host can
	// take on this node. A subnet rule would refuse neighbours behind one carrier NAT.
	if (IsServedOrOfferedIP(nContactIP, contact->GetClientID())) {
		return EIncomingBuddy::SameIP;
	}
	// If aMule already knows this client, abort this.. It could cause conflicts.
	// Although the odds of this happening is very small, it could still happen.
	if (FindClientByIP(nContactIP, contact->GetTCPPort())) {
		return EIncomingBuddy::Conflict;
	} else if (IsKadFirewallCheckIP(nContactIP)) { // doing a kad firewall check with this IP, abort
		AddDebugLogLineN(logKadMain,
			"Kad TCP firewallcheck / Buddy request collision for IP " +
				Uint32toStringIP(nContactIP));
		return EIncomingBuddy::Conflict;
	}

	if (theApp->GetPublicIP() == nContactIP && thePrefs::GetPort() == contact->GetTCPPort()) {
		return EIncomingBuddy::Conflict; // don't connect ourself
	}

	// A pending offer holds no slot, so the list is capped: a forged request costs one UDP packet.
	if (GetPendingBuddyCount() >= GetServedBuddyLimit()) {
		EvictOldestBuddyOffer();
	}

	CUpDownClient *pNewClient =
		new CUpDownClient(contact->GetTCPPort(), contact->GetIPAddress(), 0, 0, NULL, false, true);
	pNewClient->SetKadPort(contact->GetUDPPort());
	pNewClient->SetServedBuddy(true);
	pNewClient->SetServedBuddyNatT(natT);
	pNewClient->SetKadState(KS_INCOMING_BUDDY);
	uint8_t ID[16];
	contact->GetClientID().ToByteArray(ID);
	pNewClient->SetUserHash(CMD4Hash(ID));
	buddyID->ToByteArray(ID);
	pNewClient->SetBuddyID(ID);
	AddToKadList(pNewClient);
	AddClient(pNewClient);
	return EIncomingBuddy::Offered;
}

uint32 CClientList::GetServedBuddyLimit() const
{
	return Kademlia::EffectiveServedBuddyLimit(
		thePrefs::GetKadMaxServedBuddies(), thePrefs::GetMaxConnections());
}

uint32 CClientList::ServedNatTCount() const
{
	return static_cast<uint32>(std::count_if(m_servedLinks.begin(),
		m_servedLinks.end(),
		[](const Kademlia::CServedBuddyLink &link) { return link.natT; }));
}

bool CClientList::AdmitsServedBuddy(bool natT) const
{
	const uint32 limit = GetServedBuddyLimit();
	return Kademlia::AdmitsServedBuddy(GetServedBuddyCount(),
		ServedNatTCount(),
		limit,
		Kademlia::ReservedNatTSlots(limit, RelaysKadRendezvous),
		natT);
}

bool CClientList::IsServedOrOfferedIP(uint32 ip, const Kademlia::CUInt128 &requester) const
{
	const bool served = std::any_of(m_servedRefs.begin(), m_servedRefs.end(), [ip](const auto &entry) {
		return entry.second.GetClient()->GetIP() == ip;
	});
	uint8_t hash[16];
	requester.ToByteArray(hash);
	const CMD4Hash requesterHash(hash);
	// A pending offer has no socket yet, so only its connect IP is set. The same node may ask
	// again when our offer was lost.
	return served ||
	       std::any_of(
		       m_KadSources.begin(), m_KadSources.end(), [ip, &requesterHash](const CClientRef &ref) {
			       const CUpDownClient *client = ref.GetClient();
			       return client->GetKadState() == KS_INCOMING_BUDDY &&
				      client->GetConnectIP() == ip && client->GetUserHash() != requesterHash;
		       });
}

void CClientList::EvictOldestBuddyOffer()
{
	const uint64 now = ::GetTickCount64();
	CUpDownClient *oldest = nullptr;
	for (const CClientRef &ref : m_KadSources) {
		CUpDownClient *pending = ref.GetClient();
		if (pending->GetKadState() == KS_INCOMING_BUDDY &&
			(oldest == nullptr || pending->GetBuddyLiveness().SinceReceived(now) >
						      oldest->GetBuddyLiveness().SinceReceived(now))) {
			oldest = pending;
		}
	}
	if (oldest != nullptr) {
		++m_buddyStats.serving.offersEvicted;
		AddDebugLogLineN(logKadBuddy,
			"Buddy offer to " + oldest->GetFullIP() + " dropped: a newer request took its place");
		oldest->SetKadState(KS_NONE);
	}
}

bool CClientList::AllowBuddyCallback(const CUpDownClient *buddy, uint32 sender)
{
	for (Kademlia::CServedBuddyLink &link : m_servedLinks) {
		if (link.client == buddy) {
			return link.AllowCallbackFrom(sender, ::GetTickCount64());
		}
	}
	return true;
}

bool CClientList::HasBuddyRelayBudget(const CUpDownClient *buddy, uint32 bytes) const
{
	for (const Kademlia::CServedBuddyLink &link : m_servedLinks) {
		if (link.client == buddy) {
			return link.HasBudgetFor(
				bytes, Kademlia::ServedBuddyRelayBytesPerMinute, ::GetTickCount64());
		}
	}
	return true;
}

void CClientList::RemoveFromKadList(CUpDownClient *torem)
{
	wxCHECK_RET(torem, "NULL pointer in RemoveFromKadList");

	if (m_KadSources.erase(CCLIENTREF(torem, ""))) {
		EndServedLink(torem);
		if (torem == m_pBuddy.GetClient()) {
			// Deletion may clear the buddy before Process sees the disconnected state.
			// Request recovery here so it does not wait for the twenty-minute search timer.
			if (Kademlia::CKademlia::IsRunning() &&
				Kademlia::NeedsBuddy(theApp->IsFirewalled(),
					Kademlia::CUDPFirewallTester::IsFirewalledUDP(true),
					Kademlia::CUDPFirewallTester::IsVerified())) {
				Kademlia::CKademlia::GetPrefs()->SetFindBuddy();
			}
			ReleaseBuddy();
		}
	}
}

void CClientList::AddToKadList(CUpDownClient *toadd)
{
	wxCHECK_RET(toadd, "NULL pointer in AddToKadList");

	m_KadSources.insert(
		CCLIENTREF(toadd, "CClientList::AddToKadList")); // This will take care of duplicates.
}

bool CClientList::DoRequestFirewallCheckUDP(const Kademlia::CContact &contact)
{
	// first make sure we don't know this IP already from somewhere
	if (IsIPAlreadyKnown(wxUINT32_SWAP_ALWAYS(contact.GetIPAddress()))) {
		return false;
	}
	// Just create the client object, set the state and wait. TODO: we do not know the client's
	// userhash, so no obfuscated connection can be built and the check does not work under
	// "Require Obfuscation". The only somewhat acceptable fix is to use the KadID instead.
	CUpDownClient *pNewClient =
		new CUpDownClient(contact.GetTCPPort(), contact.GetIPAddress(), 0, 0, NULL, false, true);
	pNewClient->SetKadState(KS_QUEUED_FWCHECK_UDP);
	AddDebugLogLineN(
		logClient, "Selected client for UDP Firewallcheck: " + KadIPToString(contact.GetIPAddress()));
	AddToKadList(pNewClient);
	AddClient(pNewClient);
	wxASSERT(!pNewClient->SupportsDirectUDPCallback());
	return true;
}

void CClientList::CleanUpClientList()
{
	// Remove clients that are no longer needed, by time. CUpDownClient::Disconnected does this
	// check too, but misses the cases where a client changes state without being connected.
	// Doing it at every state change would be more effective but is not compatible with the
	// current code: there are points where a client has no state for a few lines, and nothing
	// is prepared for a client object going invalid while being worked on.
	const uint64 cur_tick = ::GetTickCount64();
	if (m_dwLastClientCleanUp + CLIENTLIST_CLEANUP_TIME < cur_tick) {
		m_dwLastClientCleanUp = cur_tick;
		DEBUG_ONLY(uint32 cDeleted = 0;)
		IDMap::iterator current_it = m_clientList.begin();
		while (current_it != m_clientList.end()) {
			CUpDownClient *pCurClient = current_it->second.GetClient();
			++current_it; // Won't be used till while loop again
			// Don't delete sources coming from source seeds for 10 mins,
			// to give them a chance to connect and become a useful source.
			if (pCurClient->GetSourceFrom() == SF_SOURCE_SEEDS &&
				cur_tick - theStats::GetStartTime() < MIN2MS(10))
				continue;
			if ((pCurClient->GetUploadState() == US_NONE ||
				    (pCurClient->GetUploadState() == US_BANNED && !pCurClient->IsBanned())) &&
				pCurClient->GetDownloadState() == DS_NONE &&
				pCurClient->GetChatState() == MS_NONE &&
				pCurClient->GetKadState() == KS_NONE && pCurClient->GetSocket() == NULL) {
				DEBUG_ONLY(cDeleted++;)
				pCurClient->Disconnected("Removed during ClientList cleanup.");
				pCurClient->Safe_Delete();
#ifdef __DEBUG__
			} else {
				if (!(pCurClient->GetUploadState() == US_NONE ||
					    (pCurClient->GetUploadState() == US_BANNED &&
						    !pCurClient->IsBanned()))) {
					AddDebugLogLineN(logProxy,
						CFormat("Debug: Not deleted client %p with up state: %i ") %
							static_cast<void *>(pCurClient) %
							pCurClient->GetUploadState());
				}
				if (!(pCurClient->GetDownloadState() == DS_NONE)) {
					AddDebugLogLineN(logProxy,
						CFormat("Debug: Not deleted client %p with down state: %i ") %
							static_cast<void *>(pCurClient) %
							pCurClient->GetDownloadState());
				}
				if (!(pCurClient->GetChatState() == MS_NONE)) {
					AddDebugLogLineN(logProxy,
						CFormat("Debug: Not deleted client %p with chat state: %i ") %
							static_cast<void *>(pCurClient) %
							pCurClient->GetChatState());
				}
				if (!(pCurClient->GetKadState() == KS_NONE)) {
					AddDebugLogLineN(logProxy,
						CFormat("Debug: Not deleted client %p with kad state: %i ip: "
							"%s") %
							static_cast<void *>(pCurClient) %
							(int)pCurClient->GetKadState() %
							pCurClient->GetFullIP());
				}
				if (!(pCurClient->GetSocket() == NULL)) {
					AddDebugLogLineN(logProxy,
						CFormat("Debug: Not deleted client %p: has socket") %
							static_cast<void *>(pCurClient));
				}
				AddDebugLogLineN(logProxy,
					CFormat("Debug: Not deleted client %p with kad version: %i") %
						static_cast<void *>(pCurClient) %
						pCurClient->GetKadVersion());
#endif
			}
		}
#ifdef __DEBUG__
		AddDebugLogLineN(logClient,
			CFormat("Cleaned ClientList, removed %i not used known clients") % cDeleted);
#endif
	}
}

void CClientList::AddKadFirewallRequest(uint32 ip)
{
	m_firewallCheckRequests.Add(ip, ::GetTickCount64());
}

bool CClientList::IsKadFirewallCheckIP(uint32 ip) const
{
	return m_firewallCheckRequests.Contains(ip, ::GetTickCount64());
}

bool CClientList::TakeKadFirewallReply(uint32 ip, Kademlia::CFirewallCheckRequests::Reply reply)
{
	return m_firewallCheckRequests.Take(ip, reply, ::GetTickCount64());
}

void CClientList::AddDirectCallbackClient(CUpDownClient *toAdd)
{
	wxASSERT(toAdd->GetDirectCallbackTimeout() != 0);
	if (toAdd->HasBeenDeleted()) {
		return;
	}
	for (DirectCallbackList::const_iterator it = m_currentDirectCallbacks.begin();
		it != m_currentDirectCallbacks.end();
		++it) {
		if (it->GetClient() == toAdd) {
			wxFAIL; // might happen very rarely on multiple connection tries, could be fixed in
				// the client class, till then it's not much of a problem though
			return;
		}
	}
	m_currentDirectCallbacks.push_back(CCLIENTREF(toAdd, "CClientList::AddDirectCallbackClient"));
}

void CClientList::ProcessDirectCallbackList()
{
	// we do check if any direct callbacks have timed out by now
	const uint64_t cur_tick = ::GetTickCount64();
	for (DirectCallbackList::iterator it = m_currentDirectCallbacks.begin();
		it != m_currentDirectCallbacks.end();) {
		DirectCallbackList::iterator it2 = it++;
		CUpDownClient *curClient = it2->GetClient();
		if (curClient->GetDirectCallbackTimeout() < cur_tick) {
			wxASSERT(curClient->GetDirectCallbackTimeout() != 0);
			m_currentDirectCallbacks.erase(it2);
			if (curClient->Disconnected("Direct Callback Timeout")) {
				curClient->Safe_Delete();
			}
		}
	}
}

void CClientList::AddTrackCallbackRequests(uint32_t ip)
{
	AddTrackCallbackRequests(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(ip));
}

void CClientList::AddTrackCallbackRequests(const CNetworkAddress &address)
{
	if (!PeerAddressing::CanRequestCallback(address)) {
		return;
	}
	uint64_t now = ::GetTickCount64();
	CallbackAddressAndTicks add = { PeerAddressing::RateLimitScope(address), now };
	m_directCallbackRequests.push_front(add);
	while (!m_directCallbackRequests.empty()) {
		if (now - m_directCallbackRequests.back().inserted >
			PeerAddressing::kCallbackRequestThrottleMs) {
			m_directCallbackRequests.pop_back();
		} else {
			break;
		}
	}
}

bool CClientList::AllowCallbackRequest(uint32_t ip) const
{
	return AllowCallbackRequest(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(ip));
}

bool CClientList::AllowCallbackRequest(const CNetworkAddress &address) const
{
	if (!PeerAddressing::CanRequestCallback(address)) {
		return false;
	}
	uint64_t now = ::GetTickCount64();
	for (const auto &request : m_directCallbackRequests) {
		if (PeerAddressing::IsCallbackRequestThrottled(
			    address, request.address, now - request.inserted)) {
			return false;
		}
	}
	return true;
}

uint32 CClientList::GetBuddyIP()
{
	return GetBuddy()->GetIP();
}

uint16 CClientList::GetBuddyPort()
{
	return GetBuddy()->GetUDPPort();
}

// File_checked_for_headers
