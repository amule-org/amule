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

#ifndef UTPSTREAMACCEPTOR_H
#define UTPSTREAMACCEPTOR_H

#include "UtpContext.h"

//! Why an accepted uTP stream was, or was not, taken.
enum class EUtpAdmission
{
	Admit,
	ShuttingDown,
	TooManySockets,
	TooManyHalfOpen,
	NoAddress,
	Filtered,
	Banned
};

/**
 * Inbound streams whose peer has sent no data yet. A SYN proves no source address, so this budget is
 * all a spoofer can hold, like a TCP listen backlog. A peer a rendezvous announced is not held to it.
 */
constexpr std::size_t kMaxHalfOpenUtpStreams = 8;

/**
 * The admission decision, separated from gathering the facts it needs.
 *
 * What CListenSocket::OnAccept and CClientTCPSocket::InitNetworkData ask for TCP, without the
 * listener's exception while connecting to a server: that exists for the server's TCP callback test,
 * and uTP has none. Its counterpart here is a rendezvous peer, whose caller passes no half-open
 * count and a limit without the five-second interval.
 */
constexpr EUtpAdmission DecideUtpAdmission(
	bool running, bool tooManySockets, std::size_t halfOpen, uint32_t ip, bool filtered, bool banned)
{
	if (!running) {
		return EUtpAdmission::ShuttingDown;
	}
	if (tooManySockets) {
		return EUtpAdmission::TooManySockets;
	}
	if (halfOpen >= kMaxHalfOpenUtpStreams) {
		return EUtpAdmission::TooManyHalfOpen;
	}
	if (ip == 0) {
		return EUtpAdmission::NoAddress;
	}
	if (filtered) {
		return EUtpAdmission::Filtered;
	}
	if (banned) {
		return EUtpAdmission::Banned;
	}
	return EUtpAdmission::Admit;
}

//! Gathers those facts from the application and acts on the decision.
class CUtpStreamAcceptor : public IUtpStreamAcceptor
{
public:
	bool AdmitsSyn(uint32_t ip, uint16_t port, std::size_t halfOpen) override;
	SUtpIncomingSynDecision DecideIncomingSyn(
		uint32_t ip, uint16_t port, const SUtpSynCandidateInfo &candidate) override;
	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport, uint32_t ip, uint16_t port) override;
	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport,
		uint32_t ip,
		uint16_t port,
		const SUtpIncomingSynDecision &decision) override;
};

#endif // UTPSTREAMACCEPTOR_H
// File_checked_for_headers
