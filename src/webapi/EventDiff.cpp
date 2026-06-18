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

#include "EventDiff.h"

#include "EventBus.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>


namespace webapi {


namespace {

// Minimal JSON string escaper. JsonWriter (libwebcommon) is the
// canonical formatter for response bodies, but the event-data
// payloads we emit here are small and predictable — a few KB at
// most — and keeping the diff path independent of CJsonWriter
// avoids dragging wxString into the bus path. Quote-escape only the
// characters JSON disallows: backslash, double-quote, and the C0
// controls. Tab/CR/LF appear in amule log lines so we encode them
// explicitly.
std::string EscJson(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
			case '\\': out += "\\\\"; break;
			case '"':  out += "\\\""; break;
			case '\b': out += "\\b";  break;
			case '\f': out += "\\f";  break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (c < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					out += static_cast<char>(c);
				}
		}
	}
	return out;
}


std::string ToJson(const DownloadSnapshot &d)
{
	std::ostringstream o;
	o << "{"
	  << "\"ecid\":" << d.ecid
	  << ",\"hash\":\"" << EscJson(d.hash) << "\""
	  << ",\"name\":\"" << EscJson(d.name) << "\""
	  << ",\"size\":" << d.size
	  << ",\"size_done\":" << d.size_done
	  << ",\"size_xfer\":" << d.size_xfer
	  << ",\"speed_bps\":" << d.speed_bps
	  << ",\"status\":\"" << EscJson(d.status) << "\""
	  << ",\"priority\":\"" << EscJson(d.priority) << "\""
	  << ",\"priority_auto\":" << (d.priority_auto ? "true" : "false")
	  << ",\"category\":" << d.category
	  << ",\"sources\":{"
	    << "\"total\":" << d.sources_total
	    << ",\"not_current\":" << d.sources_not_current
	    << ",\"transferring\":" << d.sources_transferring
	    << ",\"a4af\":" << d.sources_a4af
	    << "}"
	  << ",\"progress\":{\"percent\":" << d.percent << "}"
	  << "}";
	return o.str();
}


std::string ToJson(const SharedSnapshot &s)
{
	std::ostringstream o;
	o << "{"
	  << "\"ecid\":" << s.ecid
	  << ",\"hash\":\"" << EscJson(s.hash) << "\""
	  << ",\"name\":\"" << EscJson(s.name) << "\""
	  << ",\"size\":" << s.size
	  << ",\"priority\":\"" << EscJson(s.priority) << "\""
	  << ",\"complete_sources\":" << s.complete_sources
	  << "}";
	return o.str();
}


std::string ToJson(const ServerSnapshot &s)
{
	std::ostringstream o;
	o << "{"
	  << "\"ecid\":" << s.ecid
	  << ",\"name\":\"" << EscJson(s.name) << "\""
	  << ",\"address\":\"" << EscJson(s.address) << "\""
	  << ",\"users\":" << s.users
	  << ",\"files\":" << s.files
	  << ",\"priority\":\"" << EscJson(s.priority) << "\""
	  << ",\"static\":" << (s.is_static ? "true" : "false")
	  << "}";
	return o.str();
}


std::string ToJson(const ClientSnapshot &c)
{
	std::ostringstream o;
	o << "{"
	  << "\"ecid\":" << c.ecid
	  << ",\"client_name\":\"" << EscJson(c.client_name) << "\""
	  << ",\"user_hash\":\"" << EscJson(c.user_hash) << "\""
	  << ",\"ip\":\"" << EscJson(c.ip) << "\""
	  << ",\"port\":" << c.port
	  << ",\"software\":\"" << EscJson(c.software) << "\""
	  << ",\"upload_state\":\"" << EscJson(c.upload_state) << "\""
	  << ",\"download_state\":\"" << EscJson(c.download_state) << "\""
	  << ",\"upload_speed_bps\":" << c.upload_speed_bps
	  << ",\"download_speed_bps\":" << c.download_speed_bps
	  << "}";
	return o.str();
}


std::string ToJson(const StatusSnapshot &s)
{
	std::ostringstream o;
	o << "{"
	  << "\"ed2k_state\":\"" << EscJson(s.ed2k_state) << "\""
	  << ",\"kad_state\":\"" << EscJson(s.kad_state) << "\""
	  << ",\"ed2k_lowid\":" << (s.ed2k_lowid ? "true" : "false")
	  << ",\"kad_firewalled\":" << (s.kad_firewalled ? "true" : "false")
	  << ",\"server_name\":\"" << EscJson(s.server_name) << "\""
	  << ",\"server_ip\":\"" << EscJson(s.server_ip) << "\""
	  << ",\"server_port\":" << s.server_port
	  << ",\"download_bps\":" << s.download_bps
	  << ",\"upload_bps\":" << s.upload_bps
	  << ",\"ul_queue_len\":" << s.ul_queue_len
	  << ",\"total_src_count\":" << s.total_src_count
	  << "}";
	return o.str();
}


// Coarse equality — every field. For Phase 8b we treat any change as
// "_updated" (emit the full new snapshot). v0.2 could introduce
// per-field deltas if a real consumer reports wanting them.
bool Equal(const DownloadSnapshot &a, const DownloadSnapshot &b)
{
	return a.hash == b.hash && a.name == b.name && a.size == b.size
	    && a.size_done == b.size_done && a.size_xfer == b.size_xfer
	    && a.speed_bps == b.speed_bps && a.status == b.status
	    && a.priority == b.priority && a.priority_auto == b.priority_auto
	    && a.category == b.category
	    && a.sources_total == b.sources_total
	    && a.sources_not_current == b.sources_not_current
	    && a.sources_transferring == b.sources_transferring
	    && a.sources_a4af == b.sources_a4af
	    && a.percent == b.percent;
}
bool Equal(const SharedSnapshot &a, const SharedSnapshot &b)
{
	return a.hash == b.hash && a.name == b.name && a.size == b.size
	    && a.priority == b.priority
	    && a.complete_sources == b.complete_sources;
}
bool Equal(const ServerSnapshot &a, const ServerSnapshot &b)
{
	return a.name == b.name && a.address == b.address
	    && a.users == b.users && a.files == b.files
	    && a.priority == b.priority && a.is_static == b.is_static;
}
bool Equal(const ClientSnapshot &a, const ClientSnapshot &b)
{
	return a.upload_state == b.upload_state
	    && a.download_state == b.download_state
	    && a.upload_speed_bps == b.upload_speed_bps
	    && a.download_speed_bps == b.download_speed_bps
	    && a.client_name == b.client_name;
}
bool Equal(const StatusSnapshot &a, const StatusSnapshot &b)
{
	return a.ed2k_state == b.ed2k_state && a.kad_state == b.kad_state
	    && a.ed2k_lowid == b.ed2k_lowid
	    && a.kad_firewalled == b.kad_firewalled
	    && a.server_name == b.server_name
	    && a.server_ip == b.server_ip
	    && a.server_port == b.server_port
	    && a.download_bps == b.download_bps
	    && a.upload_bps == b.upload_bps
	    && a.ul_queue_len == b.ul_queue_len
	    && a.total_src_count == b.total_src_count;
}


// Generic map-diff helper. Walks both old and new, emitting:
//   - `<base>_removed` for keys in old missing from new (data: identity-only)
//   - `<base>_added`   for keys in new missing from old (data: full ToJson)
//   - `<base>_updated` for shared keys whose values differ (data: full ToJson)
//
// `removed_id_payload_fn` formats the identity-only `_removed` payload
// — usually `{"hash": "..."}` for hash-keyed (downloads, shared) or
// `{"ecid": N}` for ECID-keyed (servers, clients).
template <class Map, class IdentityFn>
void DiffMap(CEventBus &bus, const std::string &base,
             const Map &old_items, const Map &new_items,
             IdentityFn removed_id_payload_fn)
{
	for (const auto &kv : old_items) {
		if (new_items.find(kv.first) == new_items.end()) {
			bus.Publish(base + "_removed",
			            removed_id_payload_fn(kv.second));
		}
	}
	for (const auto &kv : new_items) {
		const auto it = old_items.find(kv.first);
		if (it == old_items.end()) {
			bus.Publish(base + "_added", ToJson(kv.second));
		} else if (!Equal(it->second, kv.second)) {
			bus.Publish(base + "_updated", ToJson(kv.second));
		}
	}
}


// For hash-keyed types (downloads / shared) emit removed payloads as
// `{"hash":"..."}` so consumers can drop the cache entry without
// needing the old object.
std::string RemovedHashPayload(const DownloadSnapshot &d)
{
	return "{\"hash\":\"" + EscJson(d.hash) + "\"}";
}
std::string RemovedHashPayload(const SharedSnapshot &s)
{
	return "{\"hash\":\"" + EscJson(s.hash) + "\"}";
}
// ECID-keyed types (servers / clients): same shape, ECID payload.
std::string RemovedEcidPayload(const ServerSnapshot &s)
{
	std::ostringstream o;
	o << "{\"ecid\":" << s.ecid << "}";
	return o.str();
}
std::string RemovedEcidPayload(const ClientSnapshot &c)
{
	std::ostringstream o;
	o << "{\"ecid\":" << c.ecid << "}";
	return o.str();
}


// Build an ECID-keyed map from the vector view that CState exposes.
// The cache's internal layout is std::map<ECID, Snapshot>; the public
// accessor returns std::vector<Snapshot>. For diffing we want
// random-access-by-ECID, so we lift it back into a map. Cheap — O(N)
// with N typically <1000 per substruct.
template <class Snap>
std::map<std::uint32_t, Snap> ByEcid(const std::vector<Snap> &v)
{
	std::map<std::uint32_t, Snap> m;
	for (const auto &x : v) m.emplace(x.ecid, x);
	return m;
}

}  // namespace


namespace {

// Single-writer invariant: only the wxApp refresher tick mutates
// LastSeenState + publishes diffs. Anything else (a future inline-
// refresh-then-publish, a debug recompute, etc.) is a silent
// concurrency bug — events get duplicated/dropped depending on
// which order the threads landed. Capture the first caller's
// thread id and abort hard on any subsequent caller from a
// different thread. Hard-abort (not assert) so the check survives
// -DNDEBUG and ships in every Release / RelWithDebInfo binary.
std::atomic<std::thread::id> g_publisher_thread;

void EnforceSinglePublisher()
{
	const std::thread::id self = std::this_thread::get_id();
	std::thread::id expected;
	if (g_publisher_thread.compare_exchange_strong(expected, self)) {
		return;          // first caller — claimed it
	}
	if (expected == self) return;
	std::cerr << "amuleapi: EmitDiffsAndUpdate called from two "
	             "different threads; this breaks the single-writer "
	             "invariant on LastSeenState and the EventBus.\n";
	std::abort();
}

}  // namespace


void EmitDiffsAndUpdate(CEventBus &bus,
                        LastSeenState &prev,
                        const CState &state)
{
	EnforceSinglePublisher();
	// Snapshot the current state under its read locks. Each accessor
	// takes the shared_timed_mutex shared, copies, and returns.
	auto new_downloads = ByEcid(state.Downloads());
	auto new_shared    = ByEcid(state.Shared());
	auto new_servers   = ByEcid(state.Servers());
	auto new_clients   = ByEcid(state.Clients());
	auto new_status    = state.Status();

	// Diff each substruct.
	DiffMap(bus, "download", prev.downloads, new_downloads,
		[](const DownloadSnapshot &d) {
			return RemovedHashPayload(d);
		});
	DiffMap(bus, "shared", prev.shared, new_shared,
		[](const SharedSnapshot &s) {
			return RemovedHashPayload(s);
		});
	DiffMap(bus, "server", prev.servers, new_servers,
		[](const ServerSnapshot &s) {
			return RemovedEcidPayload(s);
		});
	DiffMap(bus, "client", prev.clients, new_clients,
		[](const ClientSnapshot &c) {
			return RemovedEcidPayload(c);
		});

	// /status: one event when anything changes. Cold-start gates on
	// `status_initialised` so we don't blast a status_changed on the
	// very first tick (the SSE subscribers already see the current
	// state via REST; the *change* events are what they're here
	// for).
	if (!prev.status_initialised) {
		bus.Publish("status_changed", ToJson(new_status));
		prev.status_initialised = true;
	} else if (!Equal(prev.status, new_status)) {
		bus.Publish("status_changed", ToJson(new_status));
	}

	// Snapshot the new state for next tick's diff baseline.
	prev.downloads = std::move(new_downloads);
	prev.shared    = std::move(new_shared);
	prev.servers   = std::move(new_servers);
	prev.clients   = std::move(new_clients);
	prev.status    = std::move(new_status);

	// Phase 8d: log_appended. CState::AmuleLog() is append-only
	// (CState.cpp:142-151) so a strictly-increasing size means the
	// refresher just appended the tail. First tick records the
	// size baseline silently — clients GET /api/v0/logs/amule for
	// the historical buffer; the event channel is for live tail
	// only. A truncation (size decreased) silently resyncs the
	// counter; the only path that truncates today is a future
	// `DELETE /logs/amule` mutation, and clients refetch on that
	// regardless.
	const auto amule_log = state.AmuleLog();
	if (!prev.amule_log_initialised) {
		prev.amule_log_count = amule_log.size();
		prev.amule_log_initialised = true;
	} else if (amule_log.size() < prev.amule_log_count) {
		prev.amule_log_count = amule_log.size();
	} else if (amule_log.size() > prev.amule_log_count) {
		std::ostringstream payload;
		payload << "{\"lines\":[";
		bool first = true;
		for (std::size_t i = prev.amule_log_count; i < amule_log.size(); ++i) {
			if (!first) payload << ",";
			first = false;
			payload << "\"" << EscJson(amule_log[i]) << "\"";
		}
		payload << "]}";
		bus.Publish("log_appended", payload.str());
		prev.amule_log_count = amule_log.size();
	}
}


}  // namespace webapi
