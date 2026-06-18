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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include "State.h"

#include <utility>


namespace webapi {


bool CState::HasFirstSnapshot() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_has_first_snapshot;
}


std::time_t CState::SnapshotAt() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_snapshot_at;
}


bool CState::EcConnected() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_ec_connected;
}


StatusSnapshot CState::Status() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_status;
}


KadSnapshot CState::Kad() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_kad;
}


PreferencesSnapshot CState::Preferences() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_preferences;
}


std::vector<CategorySnapshot> CState::Categories() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_categories;
}


std::vector<std::string> CState::AmuleLog() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_amule_log_lines;
}


ServerInfoLog CState::ServerInfo() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_server_info;
}


StatsTreeNode CState::StatsTree() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_stats_tree;
}


StatsGraphs CState::Graphs() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_graphs;
}


std::vector<SearchResult> CState::Search() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<SearchResult> out;
	out.reserve(m_search.size());
	for (const auto &kv : m_search) out.push_back(kv.second);
	return out;
}


void CState::MutateSearch(const std::function<
	void(std::map<std::uint32_t, SearchResult> &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_search);
}


void CState::WriteStatsTree(StatsTreeNode t)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_stats_tree = std::move(t);
}


void CState::WriteGraphs(StatsGraphs g)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_graphs = std::move(g);
}


void CState::AppendAmuleLog(std::vector<std::string> new_lines)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	// No cap — see State.h comment above the `m_amule_log_lines`
	// declaration. Operators can truncate via DELETE /logs/amule
	// (Phase 5 mutation).
	m_amule_log_lines.insert(m_amule_log_lines.end(),
		std::make_move_iterator(new_lines.begin()),
		std::make_move_iterator(new_lines.end()));
}


void CState::ClearAmuleLog()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_amule_log_lines.clear();
}


void CState::WriteServerInfo(ServerInfoLog s)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_server_info = std::move(s);
}


std::vector<ServerSnapshot> CState::Servers() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<ServerSnapshot> out;
	out.reserve(m_servers.size());
	for (const auto &kv : m_servers) out.push_back(kv.second);
	return out;
}


void CState::WriteStatus(StatusSnapshot s)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_status = std::move(s);
}


void CState::WriteKad(KadSnapshot k)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_kad = std::move(k);
}


void CState::WritePreferences(PreferencesSnapshot p)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_preferences = std::move(p);
}


void CState::WriteCategories(std::vector<CategorySnapshot> c)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_categories = std::move(c);
}


void CState::MutateServers(const std::function<
	void(std::map<std::uint32_t, ServerSnapshot> &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_servers);
}


std::vector<DownloadSnapshot> CState::Downloads() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<DownloadSnapshot> out;
	out.reserve(m_downloads.size());
	for (const auto &kv : m_downloads) out.push_back(kv.second);
	return out;
}


std::vector<ClientSnapshot> CState::Clients() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<ClientSnapshot> out;
	out.reserve(m_clients.size());
	for (const auto &kv : m_clients) out.push_back(kv.second);
	return out;
}


std::vector<SharedSnapshot> CState::Shared() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<SharedSnapshot> out;
	out.reserve(m_shared.size());
	for (const auto &kv : m_shared) out.push_back(kv.second);
	return out;
}


bool CState::FindDownload(const std::string &hash_hex,
                          DownloadSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	for (const auto &kv : m_downloads) {
		if (kv.second.hash == hash_hex) {
			out = kv.second;
			return true;
		}
	}
	return false;
}


void CState::MutateDownloads(const std::function<
	void(std::map<std::uint32_t, DownloadSnapshot> &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_downloads);
}


void CState::MutateClients(const std::function<
	void(std::map<std::uint32_t, ClientSnapshot> &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_clients);
}


void CState::MutateShared(const std::function<
	void(std::map<std::uint32_t, SharedSnapshot> &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_shared);
}


void CState::ResetLists()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_downloads.clear();
	m_clients.clear();
	m_shared.clear();
	m_servers.clear();
	m_categories.clear();
	m_search.clear();
	// Logs + stats_tree + graphs survive EC reconnects on purpose —
	// operator can see "EC disconnected at HH:MM" alongside earlier
	// graph traffic; stats_tree's counters are amuled-uptime not
	// amuleapi-tick scoped.
}


void CState::MarkTickSuccess()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_has_first_snapshot = true;
	m_ec_connected       = true;
	m_snapshot_at        = std::time(nullptr);
}


void CState::MarkTickFailure()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	// Deliberately preserve m_snapshot_at — clients hitting /status
	// during a transient EC blip get the stale `snapshot_at` next to
	// `ec_connected=false`, so they can tell how stale the cached
	// data is. Resetting it to `now` would lie about freshness.
	m_ec_connected = false;
}


}  // namespace webapi
