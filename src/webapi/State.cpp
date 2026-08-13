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

#include <ctime>

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace webapi
{

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

CState::DashboardSnapshot CState::Dashboard() const
{
	// Single shared_lock acquisition: callers of /api/v0/status get
	// a coherent (status, kad, snapshot_at, ec_connected) tuple
	// instead of the four-separate-lock dance, which can interleave
	// with a refresher tick and make `kad.network` describe a
	// different tick than `ed2k.*` / `speeds.*`.
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	DashboardSnapshot out;
	out.status = m_status;
	out.kad = m_kad;
	out.snapshot_at = m_snapshot_at;
	out.ec_connected = m_ec_connected;
	return out;
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

std::vector<SearchResult> CState::Search(std::uint32_t search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<SearchResult> out;
	auto it = m_searches.find(ResolveSearchId(search_id));
	if (it == m_searches.end())
		return out;
	out.reserve(it->second.results.size());
	for (const auto &kv : it->second.results)
		out.push_back(kv.second);
	return out;
}

void CState::MutateSearch(
	std::uint32_t search_id, const std::function<void(std::map<std::uint32_t, SearchResult> &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(ResolveSearchId(search_id));
	if (it != m_searches.end())
		fn(it->second.results);
}

SearchProgressSnapshot CState::SearchProgress(std::uint32_t search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(ResolveSearchId(search_id));
	if (it == m_searches.end())
		return SearchProgressSnapshot{};
	return it->second.progress;
}

bool CState::HasSearch(std::uint32_t search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_searches.count(ResolveSearchId(search_id)) != 0;
}

std::uint32_t CState::CurrentSearchId() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_current_search_id;
}

std::vector<std::uint32_t> CState::ActiveSearchIds() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<std::uint32_t> out;
	for (const auto &kv : m_searches)
		if (kv.second.progress.active)
			out.push_back(kv.first);
	return out;
}

std::vector<std::uint32_t> CState::AllSearchIds() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<std::uint32_t> out;
	out.reserve(m_searches.size());
	for (const auto &kv : m_searches)
		out.push_back(kv.first);
	return out;
}

bool CState::FindSearchResultByHash(const std::string &hash_hex, SearchResult &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	for (const auto &skv : m_searches) {
		for (const auto &rkv : skv.second.results) {
			if (rkv.second.hash == hash_hex) {
				out = rkv.second;
				return true;
			}
		}
	}
	return false;
}

void CState::MarkSearchStarted(std::uint32_t search_id, const std::string &kind)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	SearchSlot &slot = m_searches[search_id];
	// generation is per-slot and monotonic: a restart of the same id (rare —
	// the daemon allocates fresh ids) keeps it climbing so EventDiff still fires.
	const auto next_generation = slot.progress.generation + 1;
	slot.results.clear();
	slot.progress = SearchProgressSnapshot{};
	slot.progress.active = true;
	slot.progress.kind = kind;
	slot.progress.generation = next_generation;
	slot.seq = ++m_search_seq;
	m_current_search_id = search_id;

	// Bound the retained slots: a client that never closes its searches would
	// otherwise accumulate one slot (with its result vector) per search for the
	// whole process lifetime. Evict oldest-first, but never the current search
	// or an active (still-polling) one — the surplus is always finished slots.
	while (m_searches.size() > kMaxSearchSlots) {
		auto victim = m_searches.end();
		for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
			if (it->first == m_current_search_id || it->second.progress.active) {
				continue;
			}
			if (victim == m_searches.end() || it->second.seq < victim->second.seq) {
				victim = it;
			}
		}
		if (victim == m_searches.end()) {
			break; // all remaining slots are active/current — nothing to evict
		}
		m_searches.erase(victim);
	}
}

void CState::MarkSearchDiscovered(std::uint32_t search_id, const std::string &kind)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	if (m_searches.count(search_id)) {
		// Already known (self-started, or discovered on an earlier
		// cache-miss check): leave its accumulated results/progress
		// alone. Re-seeding here would stomp whatever
		// WriteSearchProgress/ApplySearchFull already recorded for it
		// this session.
		return;
	}
	SearchSlot &slot = m_searches[search_id];
	slot.progress.active = true;
	slot.progress.kind = kind;
	slot.seq = ++m_search_seq;
	// Deliberately NOT touching m_current_search_id: a no-id GET
	// /search/results should keep meaning "the search THIS session
	// started", not silently jump to whatever was last discovered.

	// Same bound as MarkSearchStarted, for the same reason -- a busy core
	// with many concurrent searches shouldn't let discovery alone grow
	// this session's slot map without limit.
	while (m_searches.size() > kMaxSearchSlots) {
		auto victim = m_searches.end();
		for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
			if (it->first == m_current_search_id || it->second.progress.active) {
				continue;
			}
			if (victim == m_searches.end() || it->second.seq < victim->second.seq) {
				victim = it;
			}
		}
		if (victim == m_searches.end()) {
			break;
		}
		m_searches.erase(victim);
	}
}

void CState::WriteSearchProgress(std::uint32_t search_id, SearchProgressSnapshot s)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(ResolveSearchId(search_id));
	if (it != m_searches.end())
		it->second.progress = std::move(s);
}

void CState::CloseSearch(std::uint32_t search_id)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	const auto id = ResolveSearchId(search_id);
	m_searches.erase(id);
	if (m_current_search_id == id) {
		// No reliable "next most-recent" once the current search is gone (Kad
		// ids sort above ed2k ids, so highest-key is not newest); fall back to
		// no current until the next POST /search.
		m_current_search_id = 0;
	}
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
	// .
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
	for (const auto &kv : m_servers)
		out.push_back(kv.second);
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

void CState::MutateServers(const std::function<void(std::map<std::uint32_t, ServerSnapshot> &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_servers);
}

std::vector<FileSnapshot> CState::Downloads() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<FileSnapshot> out;
	out.reserve(m_files.size());
	for (const auto &kv : m_files) {
		if (kv.second.is_downloading)
			out.push_back(kv.second);
	}
	return out;
}

std::vector<FileSnapshot> CState::Shared() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<FileSnapshot> out;
	out.reserve(m_files.size());
	for (const auto &kv : m_files) {
		if (kv.second.is_shared)
			out.push_back(kv.second);
	}
	return out;
}

std::vector<FileSnapshot> CState::Files() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<FileSnapshot> out;
	out.reserve(m_files.size());
	for (const auto &kv : m_files)
		out.push_back(kv.second);
	return out;
}

std::vector<ClientSnapshot> CState::Clients() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<ClientSnapshot> out;
	out.reserve(m_clients.size());
	for (const auto &kv : m_clients)
		out.push_back(kv.second);
	return out;
}

bool CState::KnownClientsLoaded() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_known_loaded;
}

void CState::SetKnownClients(std::vector<KnownClientSnapshot> &&rows)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_known_clients = std::move(rows);
	m_known_of_hash.clear();
	m_known_online.clear();
	for (std::size_t i = 0; i < m_known_clients.size(); ++i)
		m_known_of_hash[m_known_clients[i].user_hash] = i;
	m_known_loaded = true;
	// The fetch describes the store as of a moment ago; the peers connected
	// right now are already more current than parts of it.
	ReconcileKnownClientsLocked();
}

void CState::InvalidateKnownClients()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_known_clients.clear();
	m_known_clients.shrink_to_fit();
	m_known_of_hash.clear();
	m_known_online.clear();
	m_known_loaded = false;
}

void CState::ReconcileKnownClients()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	if (!m_known_loaded)
		return;
	ReconcileKnownClientsLocked();
}

void CState::ReconcileKnownClientsLocked()
{
	const std::time_t now = std::time(nullptr);
	std::set<std::size_t> still_online;

	for (const auto &kv : m_clients) {
		const ClientSnapshot &c = kv.second;
		if (c.user_hash.empty())
			continue;

		auto it = m_known_of_hash.find(c.user_hash);
		if (it == m_known_of_hash.end()) {
			// A peer met since the store was read. The daemon wrote its
			// credit record when the peer said hello, stamping first-seen
			// and counting the session, so this reconstructs what it wrote
			// rather than inventing anything.
			KnownClientSnapshot k;
			k.user_hash = c.user_hash;
			k.first_seen = now;
			k.sessions = 1;
			m_known_clients.push_back(std::move(k));
			it = m_known_of_hash.emplace(c.user_hash, m_known_clients.size() - 1).first;
		}

		KnownClientSnapshot &k = m_known_clients[it->second];
		still_online.insert(it->second);
		k.online = true;
		k.total_uploaded = c.xfer_up_total;
		k.total_downloaded = c.xfer_down_total;
		// Identity, when the peer in front of us knows more than the record.
		// A record only gains a name once the core writes its metadata, so a
		// peer we have never finished a session with is otherwise nameless.
		// Guarded on the live name being known: a peer mid-handshake has none
		// and must not blank a stored one.
		if (!c.client_name.empty()) {
			k.client_name = c.client_name;
			k.ip = c.ip;
			k.port = c.port;
			k.kad_port = c.kad_port;
			k.country_code = c.country_code;
			k.software = c.software;
			k.version = c.software_version;
			k.source_origin = c.source_origin;
			k.obfuscation = c.obfuscation_status;
		}
	}

	// Whoever was online last tick and is not in this one has gone. Found
	// through the online set, so this costs the number of departures rather
	// than a walk of the store.
	for (const std::size_t idx : m_known_online) {
		if (still_online.count(idx) != 0)
			continue;
		m_known_clients[idx].online = false;
		// Seen until this moment, which is what the core writes to the record
		// at its own disconnect handling. The stored value is the *previous*
		// disconnect, so leaving it would show a peer that was here a second
		// ago as last seen months back.
		m_known_clients[idx].last_seen = now;
	}
	m_known_online.swap(still_online);
}

bool CState::FindDownload(const std::string &hash_hex, FileSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::uint32_t ecid = 0;
	if (!m_files.FindEcidByHash(hash_hex, ecid))
		return false;
	const auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_downloading)
		return false;
	out = it->second;
	return true;
}

bool CState::FindDownloadByEcid(std::uint32_t ecid, FileSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_downloading)
		return false;
	out = it->second;
	return true;
}

bool CState::FindShared(const std::string &hash_hex, FileSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::uint32_t ecid = 0;
	if (!m_files.FindEcidByHash(hash_hex, ecid))
		return false;
	const auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_shared)
		return false;
	out = it->second;
	return true;
}

bool CState::FindSharedByEcid(std::uint32_t ecid, FileSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_shared)
		return false;
	out = it->second;
	return true;
}

// MutateDownloads + MutateShared both lock + hand out m_files. Both
// walkers operate on the same unified map (and the same lock acquisition,
// when chained from a single tick); the callback decides which role
// flag to set or clear. The FileMap wrapper keeps its hash→ECID index
// in sync as the walker emplaces / erases, so there's no rebuild pass
// at the end of the mutate window.
void CState::MutateDownloads(const std::function<void(FileMap &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_files);
}

void CState::MutateShared(const std::function<void(FileMap &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_files);
}

void CState::MutateClients(const std::function<void(std::map<std::uint32_t, ClientSnapshot> &)> &fn)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_clients);
}

void CState::ResetLists()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_files.clear();
	m_clients.clear();
	m_servers.clear();
	m_categories.clear();
	// Drop all search slots on an EC reconnect: the daemon's per-connection
	// search registry is gone with the old connection, so every cached
	// search_id is stale. current falls back to none; the next POST /search
	// re-seeds. (EventDiff re-baselines its per-search state when a slot it was
	// tracking disappears, so no generation carry-over is needed here.)
	m_searches.clear();
	m_current_search_id = 0;
	// Logs + stats_tree + graphs survive EC reconnects on purpose —
	// operator can see "EC disconnected at HH:MM" alongside earlier
	// graph traffic; stats_tree's counters are amuled-uptime not
	// amuleapi-tick scoped.
}

void CState::MarkTickSuccess()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_has_first_snapshot = true;
	m_ec_connected = true;
	// `m_snapshot_at` is stamped at tick-END (here), not tick-start.
	// Clients reading `snapshot_at` therefore see "the wall-clock
	// moment the daemon finished assembling this snapshot", with the
	// tick's own duration as the implicit skew (typically 50-200 ms,
	// up to multi-second under EC-mutex contention). For coarse
	// freshness checks ("is this stale by more than 5 s?") that's
	// fine; if a future caller wants sub-second precision, document
	// the skew or stamp both tick_started_at and tick_ended_at.
	m_snapshot_at = std::time(nullptr);
}

void CState::MarkTickFailure()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	// Deliberately preserve m_snapshot_at — clients see stale
	// `snapshot_at` next to `ec_connected=false`, so they can tell
	// how stale the cache is. Resetting it to `now` would lie.
	//
	// Tick-atomicity: on failure CState may hold partial mutations
	// from earlier in the tick. The "tick = transaction" model is
	// atomic for events (EmitDiffsForEventBus is skipped on failure,
	// next-tick diff is against the prior-success baseline in
	// LastSeenState) but NOT atomic for state — no rollback. CState
	// is a best-effort cache for /status freshness; LastSeenState
	// is the authoritative event baseline.
	m_ec_connected = false;
}

} // namespace webapi
