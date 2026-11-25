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

#ifndef SERVER_CALLBACK_REQUESTS_H
#define SERVER_CALLBACK_REQUESTS_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>

/**
 * Our OP_CALLBACKREQUESTs waiting for their peer to connect, one entry per request: the server
 * answers each one it refuses. A refusal (OP_CALLBACK_FAIL) names no request, so it is known to
 * be a request's own only while that request is the one out.
 */
template <typename Client> class CServerCallbackRequests
{
public:
	//! @a windowMs: how long a request can still be answered.
	explicit CServerCallbackRequests(std::uint64_t windowMs)
	: m_windowMs(windowMs)
	{
	}

	void Add(const Client &client, std::uint64_t now)
	{
		Prune(now);
		m_requests.push_back({ client, now + m_windowMs });
	}

	//! Every request for @a client: it connected, or it is gone.
	void Remove(const Client &client)
	{
		m_requests.remove_if([&client](const Request &request) { return request.client == client; });
	}

	//! The client a refusal at @a now is for, when only one request is out.
	std::optional<Client> TakeRefused(std::uint64_t now)
	{
		Prune(now);
		if (m_requests.size() != 1) {
			return std::nullopt;
		}
		const Client client = m_requests.front().client;
		m_requests.clear();
		return client;
	}

	std::size_t Count(std::uint64_t now)
	{
		Prune(now);
		return m_requests.size();
	}

private:
	struct Request
	{
		Client client;
		std::uint64_t deadline;
	};

	void Prune(std::uint64_t now)
	{
		m_requests.remove_if([now](const Request &request) { return request.deadline < now; });
	}

	std::uint64_t m_windowMs;
	std::list<Request> m_requests;
};

#endif // SERVER_CALLBACK_REQUESTS_H
