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

#include "Metrics.h"
#include "State.h"

#include <locale>
#include <sstream>

namespace webapi
{
namespace
{
std::string EscapeLabel(const std::string &value)
{
	std::string out;
	for (char c : value) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

// Exact, where streaming a double would round to six significant digits.
std::string MillisecondsAsSeconds(std::uint64_t milliseconds)
{
	const std::string fraction = std::to_string(milliseconds % 1000);
	return std::to_string(milliseconds / 1000) + '.' + std::string(3 - fraction.size(), '0') + fraction;
}

class Writer
{
public:
	Writer() { out.imbue(std::locale::classic()); }
	void Family(const char *name, const char *help, const char *type = "gauge")
	{
		out << "# HELP amule_" << name << ' ' << help << "\n# TYPE amule_" << name << ' ' << type
		    << '\n';
	}
	template <typename T> void Sample(const char *name, T value, const std::string &labels = "")
	{
		out << "amule_" << name << labels << ' ' << value << '\n';
	}
	template <typename T> void Gauge(const char *name, const char *help, T value)
	{
		Family(name, help);
		Sample(name, value);
	}
	std::ostringstream out;
};
} // namespace

std::string SerializeMetrics(const MetricsSnapshot &snapshot, const std::string &version)
{
	Writer w;
	w.Family("build_info", "Versions of amuleapi and the connected daemon.");
	w.Sample("build_info",
		1,
		"{version=\"" + EscapeLabel(version) + "\",daemon_version=\"" +
			EscapeLabel(snapshot.daemon_version) + "\"}");
	w.Gauge("ec_connected",
		"Whether the last EC refresh completed successfully.",
		snapshot.ec_connected ? 1 : 0);
	if (snapshot.snapshot_at != 0)
		w.Gauge("snapshot_timestamp_seconds",
			"Unix timestamp of the last successful refresh.",
			snapshot.snapshot_at);
	if (!snapshot.ec_connected || snapshot.snapshot_at == 0)
		return w.out.str();

	const auto &s = snapshot.status;
	const auto &k = snapshot.kad;
	w.Family("network_connected", "Whether the daemon is connected to a network.");
	w.Sample("network_connected", s.ed2k_state == "connected" ? 1 : 0, "{network=\"ed2k\"}");
	w.Sample("network_connected", s.kad_state == "connected" ? 1 : 0, "{network=\"kad\"}");
	if (s.ed2k_state == "connected")
		w.Gauge("ed2k_high_id",
			"Whether the connected daemon has an eD2k HighID.",
			s.ed2k_high_id ? 1 : 0);
	if (s.has_kad_firewalled_tcp || k.has_firewalled_udp) {
		w.Family("kad_firewalled", "Whether Kad is firewalled for the indicated protocol.");
		if (s.has_kad_firewalled_tcp)
			w.Sample("kad_firewalled", s.kad_firewalled_tcp ? 1 : 0, "{protocol=\"tcp\"}");
		if (k.has_firewalled_udp)
			w.Sample("kad_firewalled", k.firewalled_udp ? 1 : 0, "{protocol=\"udp\"}");
	}
	w.Family("transfer_rate_bytes_per_second", "Transfer rate; overhead is additional to data.");
	w.Sample("transfer_rate_bytes_per_second",
		s.upload_bytes_per_second,
		"{direction=\"upload\",kind=\"data\"}");
	w.Sample("transfer_rate_bytes_per_second",
		s.download_bytes_per_second,
		"{direction=\"download\",kind=\"data\"}");
	w.Sample("transfer_rate_bytes_per_second",
		s.upload_overhead_bytes_per_second,
		"{direction=\"upload\",kind=\"overhead\"}");
	w.Sample("transfer_rate_bytes_per_second",
		s.download_overhead_bytes_per_second,
		"{direction=\"download\",kind=\"overhead\"}");
	if (s.upload_limit_bytes_per_second.present || s.download_limit_bytes_per_second.present) {
		w.Family("speed_limit_bytes_per_second",
			"Configured transfer limit in bytes per second; zero means unlimited.");
		if (s.upload_limit_bytes_per_second.present)
			w.Sample("speed_limit_bytes_per_second",
				s.upload_limit_bytes_per_second.value,
				"{direction=\"upload\"}");
		if (s.download_limit_bytes_per_second.present)
			w.Sample("speed_limit_bytes_per_second",
				s.download_limit_bytes_per_second.value,
				"{direction=\"download\"}");
	}
	if (s.total_sent_bytes.present || s.total_received_bytes.present) {
		w.Family("lifetime_transferred_bytes_total",
			"Persisted lifetime transferred bytes; may reset when daemon statistics are reset.",
			"counter");
		if (s.total_sent_bytes.present)
			w.Sample("lifetime_transferred_bytes_total",
				s.total_sent_bytes.value,
				"{direction=\"upload\"}");
		if (s.total_received_bytes.present)
			w.Sample("lifetime_transferred_bytes_total",
				s.total_received_bytes.value,
				"{direction=\"download\"}");
	}
	if (s.session_sent_bytes.present || s.session_received_bytes.present) {
		w.Family("transferred_bytes_total", "Bytes transferred in the daemon session.", "counter");
		if (s.session_sent_bytes.present)
			w.Sample("transferred_bytes_total",
				s.session_sent_bytes.value,
				"{direction=\"upload\"}");
		if (s.session_received_bytes.present)
			w.Sample("transferred_bytes_total",
				s.session_received_bytes.value,
				"{direction=\"download\"}");
	}
	if (s.active_connections.present)
		w.Gauge("active_connections", "Active daemon connections.", s.active_connections.value);
	if (s.uptime_seconds.present)
		w.Gauge("uptime_seconds", "Daemon uptime in seconds.", s.uptime_seconds.value);
	if (s.cpu_milliseconds.present) {
		w.Family("cpu_seconds_total", "CPU time used by the daemon process.", "counter");
		w.Sample("cpu_seconds_total", MillisecondsAsSeconds(s.cpu_milliseconds.value));
	}
	if (s.resident_memory_bytes.present)
		w.Gauge("resident_memory_bytes",
			"Physical memory used by the daemon process.",
			s.resident_memory_bytes.value);
	w.Gauge("active_uploads", "Clients currently uploading.", snapshot.active_uploads);
	w.Gauge("servers", "Servers in the daemon server list.", snapshot.servers);
	if (snapshot.connected_server_users.present)
		w.Gauge("connected_server_users",
			"Users on the connected eD2k server.",
			snapshot.connected_server_users.value);
	w.Gauge("upload_queue_length", "Clients waiting in the upload queue.", s.ul_queue_len);
	w.Gauge("sources_found", "Total sources found for downloads.", s.total_src_count);
	if (s.shared_file_count.present)
		w.Gauge("shared_files",
			"Number of shared files reported by the daemon.",
			s.shared_file_count.value);
	if (s.banned_client_count.present)
		w.Gauge("banned_clients", "Currently banned clients.", s.banned_client_count.value);
	w.Family("downloads", "Downloads by status, including completed downloads awaiting removal.");
	const auto &names = MetricsSnapshot::DownloadStatuses();
	for (std::size_t i = 0; i < names.size(); ++i)
		w.Sample("downloads", snapshot.downloads[i], std::string("{status=\"") + names[i] + "\"}");
	if (s.temp_free_bytes >= 0 || s.incoming_free_bytes >= 0) {
		w.Family("disk_free_bytes", "Free bytes on the configured directory filesystem.");
		if (s.temp_free_bytes >= 0)
			w.Sample("disk_free_bytes", s.temp_free_bytes, "{dir=\"temp\"}");
		if (s.incoming_free_bytes >= 0)
			w.Sample("disk_free_bytes", s.incoming_free_bytes, "{dir=\"incoming\"}");
	}
	if (s.has_ed2k_network) {
		w.Gauge("ed2k_network_users", "Users across the known eD2k server list.", s.ed2k_users);
		w.Gauge("ed2k_network_files", "Files across the known eD2k server list.", s.ed2k_files);
	}
	if (k.has_network) {
		w.Gauge("kad_network_users", "Estimated users in the Kad network.", k.users);
		w.Gauge("kad_network_files", "Estimated files in the Kad network.", k.files);
		w.Gauge("kad_network_nodes", "Nodes in the local Kad routing table.", k.nodes);
	}
	if (k.has_indexed) {
		// Load has a different unit from indexed item counts; keep it a separate family.
		w.Family("kad_indexed", "Locally indexed Kad items by type.");
		w.Sample("kad_indexed", k.indexed_sources, "{type=\"sources\"}");
		w.Sample("kad_indexed", k.indexed_keywords, "{type=\"keywords\"}");
		w.Sample("kad_indexed", k.indexed_notes, "{type=\"notes\"}");
		w.Gauge("kad_index_load", "Kad index load reported by the daemon.", k.indexed_load);
	}
	if (k.has_served) {
		w.Gauge("kad_buddies_served",
			"Firewalled nodes this node serves as their Kad buddy.",
			k.served_count);
		w.Gauge("kad_buddies_served_limit",
			"Most firewalled nodes this node serves at once.",
			k.served_limit);
	}
	if (k.has_buddy_counters) {
		const auto &b = k.buddy_serving;
		w.Gauge("kad_buddy_pending_offers",
			"Buddy offers whose node has not connected yet.",
			b.pending_count);
		w.Family("kad_buddy_requests_total",
			"Buddy requests from firewalled nodes, by result.",
			"counter");
		w.Sample("kad_buddy_requests_total", b.requests.offered, "{result=\"offered\"}");
		w.Sample("kad_buddy_requests_total",
			b.requests.refused_firewalled,
			"{result=\"refused_firewalled\"}");
		w.Sample("kad_buddy_requests_total", b.requests.refused_full, "{result=\"refused_full\"}");
		w.Sample("kad_buddy_requests_total",
			b.requests.refused_conflict,
			"{result=\"refused_conflict\"}");
		w.Sample("kad_buddy_requests_total",
			b.requests.refused_same_ip,
			"{result=\"refused_same_ip\"}");
		w.Sample("kad_buddy_requests_total", b.requests.rate_limited, "{result=\"rate_limited\"}");
		w.Family("kad_buddy_natt_requests_total",
			"Buddy requests that set a NAT-T bit in their connect options.",
			"counter");
		w.Sample("kad_buddy_natt_requests_total", b.requests.natt);
		w.Family("kad_buddy_offers_unused_total",
			"Buddy offers whose node did not become our buddy.",
			"counter");
		w.Sample("kad_buddy_offers_unused_total", b.requests.expired, "{reason=\"expired\"}");
		w.Sample("kad_buddy_offers_unused_total", b.requests.superseded, "{reason=\"superseded\"}");
		w.Sample("kad_buddy_offers_unused_total", b.requests.duplicate, "{reason=\"duplicate\"}");
		w.Sample("kad_buddy_offers_unused_total", b.requests.evicted, "{reason=\"evicted\"}");
		w.Family("kad_buddy_served_events_total",
			"Changes to the buddies this node serves.",
			"counter");
		w.Sample("kad_buddy_served_events_total", b.buddies.started, "{event=\"started\"}");
		w.Sample("kad_buddy_served_events_total",
			b.buddies.released_extra,
			"{event=\"released_extra\"}");
		w.Sample("kad_buddy_served_events_total", b.buddies.ended, "{event=\"ended\"}");
		w.Sample("kad_buddy_served_events_total", b.buddies.timed_out, "{event=\"timed_out\"}");
		w.Family("kad_buddy_served_seconds_total",
			"Time spent serving buddies, summed over them.",
			"counter");
		w.Sample("kad_buddy_served_seconds_total", b.served_seconds);
		w.Family("kad_buddy_relays_total", "Packets relayed to served buddies.", "counter");
		w.Sample("kad_buddy_relays_total", b.callbacks.relayed, "{kind=\"callback\"}");
		w.Sample("kad_buddy_relays_total", b.reasks.relayed, "{kind=\"reask\"}");
		w.Family("kad_buddy_relayed_bytes_total", "Bytes relayed to served buddies.", "counter");
		w.Sample("kad_buddy_relayed_bytes_total", b.callbacks.relayed_bytes, "{kind=\"callback\"}");
		w.Sample("kad_buddy_relayed_bytes_total", b.reasks.relayed_bytes, "{kind=\"reask\"}");
		w.Family("kad_buddy_relay_drops_total",
			"Packets for a served buddy that were not relayed.",
			"counter");
		w.Sample("kad_buddy_relay_drops_total",
			b.callbacks.no_buddy,
			"{kind=\"callback\",reason=\"no_buddy\"}");
		w.Sample("kad_buddy_relay_drops_total",
			b.callbacks.mismatched,
			"{kind=\"callback\",reason=\"mismatched\"}");
		w.Sample("kad_buddy_relay_drops_total",
			b.callbacks.rate_limited,
			"{kind=\"callback\",reason=\"rate_limited\"}");
		w.Sample("kad_buddy_relay_drops_total",
			b.callbacks.over_budget,
			"{kind=\"callback\",reason=\"over_budget\"}");
		w.Sample("kad_buddy_relay_drops_total",
			b.reasks.no_buddy,
			"{kind=\"reask\",reason=\"no_buddy\"}");
		w.Sample("kad_buddy_relay_drops_total",
			b.reasks.mismatched,
			"{kind=\"reask\",reason=\"mismatched\"}");
		w.Sample("kad_buddy_relay_drops_total",
			b.reasks.malformed,
			"{kind=\"reask\",reason=\"malformed\"}");
		w.Sample("kad_buddy_relay_drops_total",
			b.reasks.over_budget,
			"{kind=\"reask\",reason=\"over_budget\"}");
		w.Gauge("kad_buddy_peak_relayed_bytes_per_minute",
			"Most bytes relayed to one served buddy in one minute.",
			b.peak_relayed_bytes_per_minute);
		w.Gauge("kad_buddy_peak_callback_requests_per_minute",
			"Most callback requests from one client in one minute.",
			b.callbacks.peak_requests_per_minute);
		const auto &o = k.buddy_own;
		w.Family("kad_buddy_searches_total", "Searches for a buddy of our own.", "counter");
		w.Sample("kad_buddy_searches_total", o.search_count);
		w.Family("kad_buddy_searches_held_total",
			"Due searches for a buddy of our own held for a shared file.",
			"counter");
		w.Sample("kad_buddy_searches_held_total", o.held_search_count);
		w.Family("kad_buddy_own_requests_total", "Buddy requests for a buddy of our own.", "counter");
		w.Sample("kad_buddy_own_requests_total", o.requests.sent, "{result=\"sent\"}");
		w.Sample("kad_buddy_own_requests_total", o.requests.skipped, "{result=\"skipped\"}");
		w.Sample("kad_buddy_own_requests_total", o.requests.offered, "{result=\"offered\"}");
		w.Family("kad_buddy_own_connections_total", "Connections to an offered buddy.", "counter");
		w.Sample(
			"kad_buddy_own_connections_total", o.connections.attempted, "{result=\"attempted\"}");
		w.Sample("kad_buddy_own_connections_total", o.connections.failed, "{result=\"failed\"}");
		w.Family("kad_buddy_own_events_total", "Changes to our own buddy.", "counter");
		w.Sample("kad_buddy_own_events_total", o.buddies.started, "{event=\"started\"}");
		w.Sample("kad_buddy_own_events_total", o.buddies.ended, "{event=\"ended\"}");
		w.Sample("kad_buddy_own_events_total", o.buddies.timed_out, "{event=\"timed_out\"}");
		w.Family("kad_buddy_own_received_total", "Packets our buddy relayed to us.", "counter");
		w.Sample("kad_buddy_own_received_total", o.received_callback_count, "{kind=\"callback\"}");
		w.Sample("kad_buddy_own_received_total", o.received_reask_count, "{kind=\"reask\"}");
		w.Family("kad_buddy_own_seconds_total",
			"Time a buddy was needed, missing while needed, or connected.",
			"counter");
		w.Sample("kad_buddy_own_seconds_total", o.needed_seconds, "{state=\"needed\"}");
		w.Sample("kad_buddy_own_seconds_total", o.missing_seconds, "{state=\"missing\"}");
		w.Sample("kad_buddy_own_seconds_total", o.connected_seconds, "{state=\"connected\"}");
	}
	return w.out.str();
}
} // namespace webapi
