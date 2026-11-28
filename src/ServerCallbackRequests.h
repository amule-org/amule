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

//! An OP_CALLBACK_FAIL that names its request: <target LowID 4 LE><reason 1>. ed2k-server and
//! eNode-go send it for OP_IPV6_CALLBACK_REQUEST, and for OP_CALLBACKREQUEST too since
//! andrey23127/ed2k-server#63 and eNode-go v0.5.30. Other servers refuse OP_CALLBACKREQUEST with an
//! empty one.
struct SServerCallbackRefusal
{
	std::uint32_t targetId = 0;
	std::uint8_t reason = 0;
};

inline std::optional<SServerCallbackRefusal> ParseServerCallbackRefusal(
	const std::uint8_t *data, std::size_t size)
{
	if (data == nullptr || size < 5) {
		return std::nullopt;
	}
	SServerCallbackRefusal refusal;
	refusal.targetId = static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
			   (static_cast<std::uint32_t>(data[2]) << 16) |
			   (static_cast<std::uint32_t>(data[3]) << 24);
	refusal.reason = data[4];
	return refusal;
}

//! ed2k-server's reasons, one table for both requests. OP_CALLBACKREQUEST gets only 5, 7 and 8.
namespace ServerCallbackRefusal
{
constexpr std::uint8_t NotOffered = 1;
constexpr std::uint8_t RequesterHighID = 2;
constexpr std::uint8_t RequesterNotVerified = 3;
constexpr std::uint8_t SelfTarget = 4;
constexpr std::uint8_t TargetGone = 5;
constexpr std::uint8_t TargetNotOptedIn = 6;
constexpr std::uint8_t TargetUnreachable = 7;
constexpr std::uint8_t RateLimited = 8;
} // namespace ServerCallbackRefusal

inline const char *ServerCallbackRefusalText(std::uint8_t reason)
{
	switch (reason) {
	case ServerCallbackRefusal::NotOffered:
		return "not offered to us";
	case ServerCallbackRefusal::RequesterHighID:
		return "we are HighID";
	case ServerCallbackRefusal::RequesterNotVerified:
		return "our IPv6 is not verified";
	case ServerCallbackRefusal::SelfTarget:
		return "the source is us";
	case ServerCallbackRefusal::TargetGone:
		return "the source is gone";
	case ServerCallbackRefusal::TargetNotOptedIn:
		return "the source did not opt in";
	case ServerCallbackRefusal::TargetUnreachable:
		return "the source is unreachable";
	case ServerCallbackRefusal::RateLimited:
		return "over the relay allowance";
	default:
		return "unknown reason";
	}
}

/**
 * Our callback requests to the server waiting for their peer to connect, one entry per request:
 * the server answers each one it refuses. An empty refusal (OP_CALLBACK_FAIL) names no request, so
 * it is known to be a request's own only while that request is the one out; the long one names its
 * target (TakeFirst()).
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

	//! The first live request whose client @a matches, removed.
	template <typename Matches> std::optional<Client> TakeFirst(std::uint64_t now, const Matches &matches)
	{
		const auto found =
			std::find_if(m_requests.begin(), m_requests.end(), [&](const Request &request) {
				return request.deadline >= now && matches(request.client);
			});
		if (found == m_requests.end()) {
			return std::nullopt;
		}
		const Client client = found->client;
		m_requests.erase(found);
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
