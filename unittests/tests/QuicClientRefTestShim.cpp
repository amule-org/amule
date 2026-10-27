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
