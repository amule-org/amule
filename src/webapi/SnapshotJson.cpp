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

#include "SnapshotJson.h"

#include <JsonWriter.h>

namespace webapi
{

// The Kad `network` rollup, byte-identical under `kad` on GET /status and on
// GET /kad: both read the same KadSnapshot from one Dashboard() acquisition.
void WriteKadNetworkObject(CJsonWriter &w, const KadSnapshot &k)
{
	w.Key("network");
	w.BeginObject();
	// null unless Kad is connected. user_count/file_count are the last estimate and
	// survive into `connecting`; node_count is our own routing-table size, measured at
	// 2 with Kad fully stopped, so not even the terminal state reaches 0. A number
	// here would claim knowledge of a network we are not on.
	WriteIntOrNull(w, "user_count", k.has_network, static_cast<int64_t>(k.users));
	WriteIntOrNull(w, "file_count", k.has_network, static_cast<int64_t>(k.files));
	WriteIntOrNull(w, "node_count", k.has_network, static_cast<int64_t>(k.nodes));
	w.EndObject();
}

void WriteStatusObject(CJsonWriter &w, const CState::DashboardSnapshot &dashboard)
{
	const StatusSnapshot &s = dashboard.status;
	const KadSnapshot &k = dashboard.kad;
	w.BeginObject();
	// No snapshot timestamp in the envelope: a value that moves every tick changes the
	// body bytes and defeats the ETag, so list endpoints would never see a cache hit.
	// `ec_connected` is the staleness signal, and the HTTP Date header carries
	// wall-clock for anyone who needs it.
	w.Key("ec_connected");
	w.ValueBool(dashboard.ec_connected);
	w.Key("search_all_supported");
	w.ValueBool(s.search_all_supported);

	w.Key("ed2k");
	w.BeginObject();
	w.Key("state");
	w.ValueString(wxString::FromUTF8(s.ed2k_state.c_str()));
	// Positive sense, matching the peer-side high_id on /clients/{ecid}. False for
	// a LowID and while disconnected alike, so read it with `state`.
	w.Key("high_id");
	w.ValueBool(s.ed2k_high_id);
	// Our server-assigned id; a HighID (>= 16777216) is our public address packed
	// LSB-first, which is where public_ip comes from. NOT the same encoding as the
	// peer-side ed2k_user_id on /clients/{ecid}, which byte-swaps a HighID, so the two
	// must not be fed through each other's decoder.
	w.Key("user_id");
	w.ValueInt(static_cast<int64_t>(s.ed2k_user_id));
	WriteStringOrNull(w, "public_ip", !s.ed2k_public_ip.empty(), s.ed2k_public_ip);
	// 0 when not connected -- gate on ed2k.state, not on this being nonzero.
	w.Key("connected_since_at");
	w.ValueUInt(s.ed2k_connected_since);
	// Null when not connected: ed2k.state already says whether there is a server, so
	// "" only ever meant "not connected", and a port on its own describes nothing.
	const bool has_server = !s.server_ip.empty();
	WriteStringOrNull(w, "server_name", has_server, s.server_name);
	WriteStringOrNull(w, "server_ip", has_server, s.server_ip);
	WriteIntOrNull(w, "server_port", has_server, static_cast<int64_t>(s.server_port));
	// Aggregate user and file counts across all connected ed2k servers, from the
	// same EC_OP_STAT_REQ response the kad counters ride on.
	w.Key("network");
	w.BeginObject();
	// null unless eD2k is connected: these sum the whole known server list, not
	// the server we are attached to, and nothing zeroes them on disconnect.
	WriteIntOrNull(w, "user_count", s.has_ed2k_network, static_cast<int64_t>(s.ed2k_users));
	WriteIntOrNull(w, "file_count", s.has_ed2k_network, static_cast<int64_t>(s.ed2k_files));
	w.EndObject();
	w.EndObject();

	w.Key("kad");
	w.BeginObject();
	w.Key("state");
	w.ValueString(wxString::FromUTF8(s.kad_state.c_str()));
	// Named for the transport because GET /kad reports it beside firewalled_udp,
	// a separate measurement rather than a refinement of this one.
	WriteBoolOrNull(w, "firewalled_tcp", s.has_kad_firewalled_tcp, s.kad_firewalled_tcp);
	// 0 when not connected -- gate on kad.state, not on this being nonzero.
	w.Key("connected_since_at");
	w.ValueUInt(s.kad_connected_since);
	// The same numbers GET /kad serves under `network`, through the same helper so the
	// two cannot drift. `k` was snapshotted in the same lock batch as `s`, so these
	// counters describe the same refresher tick as ed2k.* above.
	WriteKadNetworkObject(w, k);
	w.EndObject();

	w.Key("speeds");
	w.BeginObject();
	// `download_speed_bytes_per_second`, not `download_bytes_per_second`: the `speeds`
	// wrapper does not rename the quantity, and the short form reads close enough to
	// the cumulative `downloaded_bytes_total` to be misread as one.
	w.Key("download_speed_bytes_per_second");
	w.ValueUInt(s.download_bytes_per_second);
	w.Key("upload_speed_bytes_per_second");
	w.ValueUInt(s.upload_bytes_per_second);
	// Additive to the two above, not a subset: amuled counts protocol
	// overhead separately from payload.
	w.Key("download_overhead_bytes_per_second");
	w.ValueUInt(s.download_overhead_bytes_per_second);
	w.Key("upload_overhead_bytes_per_second");
	w.ValueUInt(s.upload_overhead_bytes_per_second);
	w.EndObject();

	// null rather than a number when the daemon has no figure: the -1 sentinel as
	// an unsigned value would read as 17 exabytes free, and 0 as a full disk.
	w.Key("disk");
	w.BeginObject();
	WriteIntOrNull(
		w, "temp_free_bytes", s.temp_free_bytes >= 0, static_cast<std::int64_t>(s.temp_free_bytes));
	WriteIntOrNull(w,
		"incoming_free_bytes",
		s.incoming_free_bytes >= 0,
		static_cast<std::int64_t>(s.incoming_free_bytes));
	w.EndObject();

	w.Key("queue");
	w.BeginObject();
	w.Key("waiting_upload_client_count");
	w.ValueInt(static_cast<int64_t>(s.ul_queue_len));
	w.Key("download_source_count");
	w.ValueInt(static_cast<int64_t>(s.total_src_count));
	w.EndObject();
	// Nickname is a /preferences field, not a /status one.
	w.EndObject();
}

} // namespace webapi
