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

#include "UtpLibraryAdapter.h"
#include <libutp/utp.h>

namespace
{
class CUtpLibraryAdapter final : public IUtpLibrary
{
public:
	~CUtpLibraryAdapter() override { Destroy(); }
	bool Create(IUtpDatagramSink &sink) override
	{
		if (m_context) {
			return true;
		}
		m_context = utp_init(2);
		if (!m_context) {
			return false;
		}
		utp_context_set_userdata(m_context, &sink);
		utp_set_callback(m_context, UTP_SENDTO, SendTo);
		utp_set_callback(m_context, UTP_GET_UDP_MTU, GetUdpMtu);
		utp_set_callback(m_context, UTP_GET_UDP_OVERHEAD, GetUdpOverhead);
		// Leaving UTP_ON_ACCEPT unset refuses inbound SYNs, so this slice cannot serve streams.
		return true;
	}
	void Destroy() override
	{
		if (m_context) {
			utp_destroy(m_context);
			m_context = nullptr;
		}
	}
	bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) override
	{
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		auto *bytes = reinterpret_cast<uint8_t *>(&address.sin_addr.s_addr);
		for (unsigned i = 0; i < 4; ++i) {
			bytes[i] = static_cast<uint8_t>(ip >> (8 * i));
		}
		return utp_process_udp(m_context,
			       payload,
			       length,
			       reinterpret_cast<const sockaddr *>(&address),
			       sizeof(address)) != 0;
	}
	void IssueDeferredAcks() override { utp_issue_deferred_acks(m_context); }
	void CheckTimeouts() override { utp_check_timeouts(m_context); }

private:
	static uint64 SendTo(utp_callback_arguments *args)
	{
		if (!args->address || args->address->sa_family != AF_INET ||
			args->address_len < sizeof(sockaddr_in)) {
			return 0;
		}
		const auto *address = reinterpret_cast<const sockaddr_in *>(args->address);
		const auto *bytes = reinterpret_cast<const uint8_t *>(&address->sin_addr.s_addr);
		uint32_t ip = 0;
		for (unsigned i = 0; i < 4; ++i) {
			ip |= static_cast<uint32_t>(bytes[i]) << (8 * i);
		}
		auto *sink = static_cast<IUtpDatagramSink *>(utp_context_get_userdata(args->context));
		sink->SendUtpDatagram(args->buf, args->len, ip, ntohs(address->sin_port));
		return 0;
	}
	static uint64 GetUdpMtu(utp_callback_arguments *args)
	{
		return UtpUdpMtu(args->address->sa_family == AF_INET6);
	}
	static uint64 GetUdpOverhead(utp_callback_arguments *args)
	{
		return UtpUdpOverhead(args->address->sa_family == AF_INET6);
	}
	utp_context *m_context = nullptr;
};
} // namespace

std::unique_ptr<IUtpLibrary> CreateUtpLibrary()
{
	return std::make_unique<CUtpLibraryAdapter>();
}
// File_checked_for_headers
