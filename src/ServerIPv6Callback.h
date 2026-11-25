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

#ifndef SERVER_IPV6_CALLBACK_H
#define SERVER_IPV6_CALLBACK_H

// The IPv6 server callback: a LowID requester whose IPv6 the server's dial-back reached asks it
// (OP_IPV6_CALLBACK_REQUEST) to have a LowID source dial that IPv6 (OP_CALLBACKREQUESTED_IPV6).

#include "ServerIPv6Verdict.h" // Needed for EServerIPv6Verdict
#include "SourceRoutePolicy.h" // Needed for kNatStepAnswerWindowMs

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

//! ST_IPV6_CALLBACK bit 0.
constexpr std::uint64_t IPV6_CALLBACK_FEATURE = 0x01;

//! How long our request waits for the source before the NAT-T steps. The server forwards at
//! once, and the source dials over plain TCP.
constexpr std::uint64_t kIPv6CallbackAnswerMs = 10'000;

struct SIPv6CallbackRequested
{
	std::array<std::uint8_t, 16> address{};
	std::uint16_t port = 0;
	std::optional<std::uint8_t> cryptOptions;
	std::optional<std::array<std::uint8_t, 16>> userHash;
};

//! <ipv6 16><port 2 LE>, and the crypt part of OP_CALLBACKREQUESTED when 17 more bytes follow.
inline std::optional<SIPv6CallbackRequested> ParseIPv6CallbackRequested(
	const std::uint8_t *data, std::size_t size)
{
	if (size < 18) {
		return std::nullopt;
	}
	SIPv6CallbackRequested parsed;
	for (std::size_t i = 0; i < parsed.address.size(); ++i) {
		parsed.address[i] = data[i];
	}
	parsed.port = static_cast<std::uint16_t>(data[16] | (data[17] << 8));
	if (size >= 35) {
		parsed.cryptOptions = data[18];
		std::array<std::uint8_t, 16> hash{};
		for (std::size_t i = 0; i < hash.size(); ++i) {
			hash[i] = data[19 + i];
		}
		parsed.userHash = hash;
	}
	return parsed;
}

struct SIPv6CallbackRequestFacts
{
	bool weAreLowID = false;
	bool sourceLowID = false;
	bool sourceOnOurServer = false;
	//! The server's last OP_SERVERIDENT carried ST_IPV6_CALLBACK bit 0.
	bool serverOffers = false;
	//! What its dial-back found for our IPv6.
	EServerIPv6Verdict verdict = EServerIPv6Verdict::Unknown;
};

//! The server forwards only an IPv6 its dial-back reached, so any other request is refused.
inline bool ShouldRequestIPv6Callback(const SIPv6CallbackRequestFacts &facts)
{
	return facts.weAreLowID && facts.sourceLowID && facts.sourceOnOurServer && facts.serverOffers &&
	       facts.verdict == EServerIPv6Verdict::Verified;
}

//! Opt in once a session, only where we can dial the IPv6 a request names.
inline bool ShouldOptInToIPv6Callback(bool offered, bool optedIn, bool advertisedIPv6, bool canDialIPv6)
{
	return offered && !optedIn && advertisedIPv6 && canDialIPv6;
}

//! A source left waiting LowID-to-LowID is asked again from @a nextTryAt on, but not while a NAT-T
//! step of ours can still be answered.
inline bool IsIPv6CallbackRetryDue(
	std::uint64_t now, std::uint64_t nextTryAt, bool natStepOut, std::uint64_t natStepAgeMs)
{
	return now >= nextTryAt && (!natStepOut || natStepAgeMs >= kNatStepAnswerWindowMs);
}

#endif // SERVER_IPV6_CALLBACK_H
