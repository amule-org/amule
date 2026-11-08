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

#include <muleunit/test.h>

#include "SnapshotJson.h"
#include "State.h"

#include <JsonWriter.h>

#include <limits>
#include <string>

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(SnapshotJson)

TEST(SnapshotJson, DisconnectedDashboardContract)
{
	CJsonWriter w;
	WriteStatusObject(w, CState::DashboardSnapshot{});
	ASSERT_EQUALS(
		std::string(
			"{\"ec_connected\":false,\"search_all_supported\":false,"
			"\"ed2k\":{\"state\":\"disconnected\",\"high_id\":false,\"user_id\":0,"
			"\"public_ip\":null,\"connected_since_at\":0,\"server_name\":null,"
			"\"server_ip\":null,\"server_port\":null,\"network\":{\"user_count\":null,\"file_"
			"count\":null}},"
			"\"kad\":{\"state\":\"disabled\",\"firewalled_tcp\":null,\"connected_since_at\":0,"
			"\"network\":{\"user_count\":null,\"file_count\":null,\"node_count\":null}},"
			"\"speeds\":{\"download_speed_bytes_per_second\":0,\"upload_speed_bytes_per_second\":"
			"0,"
			"\"download_overhead_bytes_per_second\":0,\"upload_overhead_bytes_per_second\":0},"
			"\"disk\":{\"temp_free_bytes\":null,\"incoming_free_bytes\":null},"
			"\"queue\":{\"waiting_upload_client_count\":0,\"download_source_count\":0}}"),
		w.GetBuffer());
}

TEST(SnapshotJson, ConnectedDashboardContract)
{
	CState::DashboardSnapshot dashboard;
	StatusSnapshot &s = dashboard.status;
	s.search_all_supported = true;
	s.ed2k_state = "connected";
	s.ed2k_high_id = true;
	s.ed2k_user_id = 0x04030201;
	s.ed2k_public_ip = "1.2.3.4";
	s.ed2k_connected_since = 1700000001;
	s.server_name = "\"服务器\"\n\\\t";
	s.server_ip = "5.6.7.8";
	s.server_port = 4661;
	s.has_ed2k_network = true;
	s.ed2k_users = 101;
	s.ed2k_files = 202;
	s.kad_state = "connected";
	s.has_kad_firewalled_tcp = true;
	s.kad_firewalled_tcp = false;
	s.kad_connected_since = 1700000002;
	s.download_bytes_per_second = 1234;
	s.upload_bytes_per_second = 5678;
	s.download_overhead_bytes_per_second = 90;
	s.upload_overhead_bytes_per_second = 12;
	s.temp_free_bytes = 0;
	s.incoming_free_bytes = 48318382080LL;
	s.ul_queue_len = 42;
	s.total_src_count = 43;
	KadSnapshot &k = dashboard.kad;
	k.has_network = true;
	k.users = 303;
	k.files = 404;
	k.nodes = 505;
	dashboard.ec_connected = true;
	CJsonWriter w;
	WriteStatusObject(w, dashboard);
	ASSERT_EQUALS(
		std::string(
			"{\"ec_connected\":true,\"search_all_supported\":true,"
			"\"ed2k\":{\"state\":\"connected\",\"high_id\":true,\"user_id\":67305985,"
			"\"public_ip\":\"1.2.3.4\",\"connected_since_at\":1700000001,"
			"\"server_name\":\"\\\"服务器\\\"\\n\\\\\\t\",\"server_ip\":\"5.6.7.8\",\"server_"
			"port\":4661,"
			"\"network\":{\"user_count\":101,\"file_count\":202}},"
			"\"kad\":{\"state\":\"connected\",\"firewalled_tcp\":false,\"connected_since_at\":"
			"1700000002,"
			"\"network\":{\"user_count\":303,\"file_count\":404,\"node_count\":505}},"
			"\"speeds\":{\"download_speed_bytes_per_second\":1234,\"upload_speed_bytes_per_"
			"second\":5678,"
			"\"download_overhead_bytes_per_second\":90,\"upload_overhead_bytes_per_second\":12},"
			"\"disk\":{\"temp_free_bytes\":0,\"incoming_free_bytes\":48318382080},"
			"\"queue\":{\"waiting_upload_client_count\":42,\"download_source_count\":43}}"),
		w.GetBuffer());
}

TEST(SnapshotJson, UnknownFlagsHideStaleMeasurements)
{
	CState::DashboardSnapshot dashboard;
	StatusSnapshot &s = dashboard.status;
	s.server_name = "stale server";
	s.server_port = 4661;
	s.ed2k_users = 123;
	s.ed2k_files = 456;
	s.kad_firewalled_tcp = true;
	KadSnapshot &k = dashboard.kad;
	k.users = 789;
	k.files = 999;
	k.nodes = 2;
	CJsonWriter stale;
	WriteStatusObject(stale, dashboard);
	CJsonWriter empty;
	WriteStatusObject(empty, CState::DashboardSnapshot{});
	ASSERT_EQUALS(empty.GetBuffer(), stale.GetBuffer());
}

TEST(SnapshotJson, UnsignedStatusCountersKeepTheirFullRange)
{
	CState::DashboardSnapshot dashboard;
	StatusSnapshot &s = dashboard.status;
	const auto max = std::numeric_limits<std::uint64_t>::max();
	s.ed2k_connected_since = max;
	s.kad_connected_since = max;
	s.download_bytes_per_second = max;
	s.upload_bytes_per_second = max;
	s.download_overhead_bytes_per_second = max;
	s.upload_overhead_bytes_per_second = max;
	dashboard.ec_connected = true;
	CJsonWriter w;
	WriteStatusObject(w, dashboard);
	const std::string &body = w.GetBuffer();
	for (const char *key : { "connected_since_at",
		     "download_speed_bytes_per_second",
		     "upload_speed_bytes_per_second",
		     "download_overhead_bytes_per_second",
		     "upload_overhead_bytes_per_second" }) {
		ASSERT_TRUE(
			body.find(std::string("\"") + key + "\":18446744073709551615") != std::string::npos);
	}
	const std::string timestamp = "\"connected_since_at\":18446744073709551615";
	const auto first = body.find(timestamp);
	ASSERT_TRUE(first != std::string::npos);
	ASSERT_TRUE(body.find(timestamp, first + timestamp.size()) != std::string::npos);
	ASSERT_TRUE(body.find(":-1") == std::string::npos);
}

// Dashboard() also carries a per-tick timestamp. It must not change an otherwise
// identical status body, or every refresh would invalidate its HTTP ETag.
TEST(SnapshotJson, RefreshTimestampDoesNotChangeStatusBody)
{
	CState::DashboardSnapshot dashboard;
	dashboard.snapshot_at = 1700000001;
	CJsonWriter first;
	WriteStatusObject(first, dashboard);
	dashboard.snapshot_at = 1700000002;
	CJsonWriter next;
	WriteStatusObject(next, dashboard);
	ASSERT_EQUALS(first.GetBuffer(), next.GetBuffer());
}

TEST(SnapshotJson, KadNetworkCanBeEmbeddedInItsEndpointObject)
{
	KadSnapshot k;
	k.has_network = true;
	// A connected network with zero estimates is known, rather than null.
	CJsonWriter w;
	w.BeginObject();
	WriteKadNetworkObject(w, k);
	w.Key("next");
	w.ValueBool(true);
	w.EndObject();
	ASSERT_EQUALS(
		std::string(
			"{\"network\":{\"user_count\":0,\"file_count\":0,\"node_count\":0},\"next\":true}"),
		w.GetBuffer());
}
