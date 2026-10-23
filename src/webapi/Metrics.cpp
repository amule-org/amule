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
		w.Family("transferred_bytes_alltime",
			"Persisted lifetime transferred bytes; may reset when daemon statistics are reset.",
			"counter");
		if (s.total_sent_bytes.present)
			w.Sample("transferred_bytes_alltime",
				s.total_sent_bytes.value,
				"{direction=\"upload\"}");
		if (s.total_received_bytes.present)
			w.Sample("transferred_bytes_alltime",
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
	return w.out.str();
}
} // namespace webapi
