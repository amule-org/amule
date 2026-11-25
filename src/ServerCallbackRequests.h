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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <vector>

/**
 * Our callback requests to the server waiting for their peer to connect, one entry per request:
 * the server answers each one it refuses. A refusal (OP_CALLBACK_FAIL) names no request, so it is
 * known to be a request's own only while that request is the one out.
 */
template <typename Client> class CServerCallbackRequests
{
public:
	//! @a windowMs: how long this request can still be answered.
	void Add(const Client &client, std::uint64_t now, std::uint64_t windowMs)
	{
		m_requests.push_back({ client, now + windowMs });
	}

	//! Every request for @a client: it connected, or it is gone.
	void Remove(const Client &client)
	{
		m_requests.remove_if([&client](const Request &request) { return request.client == client; });
	}

	void Clear() { m_requests.clear(); }

	//! The requests whose window ended before @a now, removed.
	std::vector<Client> TakeExpired(std::uint64_t now)
	{
		std::vector<Client> expired;
		m_requests.remove_if([now, &expired](const Request &request) {
			if (request.deadline >= now) {
				return false;
			}
			expired.push_back(request.client);
			return true;
		});
		return expired;
	}

	//! The client a refusal at @a now is for, when only one request is out.
	std::optional<Client> TakeRefused(std::uint64_t now)
	{
		const auto live = [now](const Request &request) { return request.deadline >= now; };
		if (std::count_if(m_requests.begin(), m_requests.end(), live) != 1) {
			return std::nullopt;
		}
		const auto refused = std::find_if(m_requests.begin(), m_requests.end(), live);
		const Client client = refused->client;
		m_requests.erase(refused);
		return client;
	}

	std::size_t Count(std::uint64_t now) const
	{
		return static_cast<std::size_t>(std::count_if(m_requests.begin(),
			m_requests.end(),
			[now](const Request &request) { return request.deadline >= now; }));
	}

private:
	struct Request
	{
		Client client;
		std::uint64_t deadline;
	};

	std::list<Request> m_requests;
};

#endif // SERVER_CALLBACK_REQUESTS_H
