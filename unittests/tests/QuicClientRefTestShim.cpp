//
// This file is part of the aMule Project.
//
// Test-only CClientRef definitions for focused QUIC adapter targets. These targets
// intentionally do not link the application-wide client hierarchy.
//

#include <ClientRef.h>

#include <unordered_map>

namespace
{
std::unordered_map<const CClientRef *, CUpDownClient *> clientRefs;
}

#ifdef DEBUG_ZOMBIE_CLIENTS
CClientRef::CClientRef(CUpDownClient *client, wxString)
#else
CClientRef::CClientRef(CUpDownClient *client)
#endif
{
	clientRefs[this] = client;
}

// Needed for real std::vector<SQuicCapsHoldoff> growth (SQuicCapsHoldoff holds a CClientRef
// owner): not just a dead-code vtable entry, so it must actually copy the mapped client.
CClientRef::CClientRef(const CClientRef &other)
{
	auto it = clientRefs.find(&other);
	clientRefs[this] = it == clientRefs.end() ? nullptr : it->second;
}

#ifdef DEBUG_ZOMBIE_CLIENTS
void CClientRef::Link(CUpDownClient *client, wxString)
#else
void CClientRef::Link(CUpDownClient *client)
#endif
{
	clientRefs[this] = client;
}

void CClientRef::Unlink()
{
	clientRefs.erase(this);
}

// Never actually invoked by the QUIC holdoff tests (no OnPacketReceived()/ProcessPacket() code
// path runs), only linked because CClientUDPSocket's vtable pins those methods alive.
uint32 CClientRef::GetIP() const
{
	return 0;
}
uint16_t CClientRef::GetUserPort() const
{
	return 0;
}

CUpDownClient *CClientRef::GetClientChecked()
{
	auto it = clientRefs.find(this);
	return it == clientRefs.end() ? nullptr : it->second;
}

void QuicClientRefTestForget(CUpDownClient *client)
{
	for (auto it = clientRefs.begin(); it != clientRefs.end();) {
		if (it->second == client) {
			it = clientRefs.erase(it);
		} else {
			++it;
		}
	}
}
