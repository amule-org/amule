//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

#include "BrowseManager.h"

#include "GuiEvents.h"
#include "Logger.h"
#include "updownclient.h"

#include <common/Format.h>

#include <wx/intl.h> // _()

bool CBrowseManager::Start(
	CUpDownClient *client, std::uint32_t searchId, std::uint32_t peerEcid, std::uint64_t now)
{
	if (!m_store.Start(client, searchId, peerEcid, now)) {
		return false;
	}
	m_clients[searchId].Link(client CLIENT_DEBUGSTRING("CBrowseManager::Start"));
	return true;
}

void CBrowseManager::OnRequestSent(CUpDownClient *client, std::uint64_t now)
{
	m_store.Touch(client, now);
}

void CBrowseManager::OnDirectoryList(CUpDownClient *client, int dirCount, std::uint64_t now)
{
	// Read the ID first, in its own statement: the argument order of a call is
	// unspecified, and a peer answering "no directories" completes the browse
	// here -- after which SearchIdFor no longer answers and the announcement
	// would be dropped on whichever compiler evaluated right-to-left.
	const std::uint32_t searchId = m_store.SearchIdFor(client);
	Perform(searchId, m_store.OnDirectoryList(client, dirCount, now));
}

void CBrowseManager::OnListingReceived(CUpDownClient *client, std::uint64_t now)
{
	// The ID has to be read before the call: completing the browse is exactly
	// what stops SearchIdFor from answering.
	const std::uint32_t searchId = m_store.SearchIdFor(client);
	Perform(searchId, m_store.OnListingReceived(client, now));
}

void CBrowseManager::Fail(CUpDownClient *client)
{
	const std::uint32_t searchId = m_store.SearchIdFor(client);
	Perform(searchId, m_store.Fail(client));
}

void CBrowseManager::Finish(CUpDownClient *client)
{
	const std::uint32_t searchId = m_store.SearchIdFor(client);
	Perform(searchId, m_store.Finish(client));
}

void CBrowseManager::Forget(CUpDownClient *client)
{
	const std::uint32_t searchId = m_store.SearchIdFor(client);
	// Ordered: the failure is reported while the reference that names the peer
	// is still held, and only then released.
	for (const browse::Effect effect : m_store.Forget(client)) {
		Perform(searchId, effect);
	}
}

void CBrowseManager::Remove(std::uint32_t searchId)
{
	m_store.Remove(searchId);
	m_clients.erase(searchId);
}

void CBrowseManager::Process(std::uint64_t now)
{
	for (const auto &todo : m_store.Tick(now)) {
		Perform(todo.first, todo.second);
	}
}

void CBrowseManager::Perform(std::uint32_t searchId, browse::Effect effect)
{
	if (searchId == 0 || effect == browse::Effect::Nothing) {
		return;
	}
	switch (effect) {
	case browse::Effect::Nothing:
		break;
	case browse::Effect::ReleaseClient:
		// Terminal and announced; the peer is free to be reaped. The record
		// stays until its search is freed.
		m_clients.erase(searchId);
		break;
	case browse::Effect::AnnounceFailure: {
		const auto it = m_clients.find(searchId);
		AddLogLineC(CFormat(_("Failed to retrieve shared files from user '%s'")) %
			    (it != m_clients.end() && it->second.IsLinked()
					    ? it->second.GetClient()->GetUserName()
					    : wxString()));
		Announce(searchId);
		break;
	}
	case browse::Effect::Announce:
		Announce(searchId);
		break;
	}
}

void CBrowseManager::Announce(std::uint32_t searchId)
{
	// The GUI's tab marker, and nothing else: every other consumer -- the EC
	// progress reply, the EC search listing, the monolithic bar -- reads this
	// manager rather than holding a copy to be kept in step.
	Notify_Browse_Status(static_cast<std::uint64_t>(searchId),
		m_store.StateOf(searchId) == browse::State::Finished ? BROWSE_FINISHED : BROWSE_FAILED);
}

std::uint32_t CBrowseManager::SearchIdFor(const CUpDownClient *client) const
{
	return m_store.SearchIdFor(client);
}

bool CBrowseManager::Has(std::uint32_t searchId) const
{
	return m_store.Has(searchId);
}

browse::State CBrowseManager::StateOf(std::uint32_t searchId) const
{
	return m_store.StateOf(searchId);
}

std::uint16_t CBrowseManager::BarValue(std::uint32_t searchId) const
{
	return m_store.BarValue(searchId);
}

std::vector<std::uint32_t> CBrowseManager::Ids() const
{
	return m_store.Ids();
}
