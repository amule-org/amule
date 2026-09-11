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

#ifndef UTPTRANSPORTFAILURE_H
#define UTPTRANSPORTFAILURE_H

/**
 * How a uTP stream ended.
 *
 * Three of these are ends the peer chose and two are ends the transport
 * reports, and the reason they are one enum rather than a bool is that the
 * caller acts differently on each: a refused connection says the peer is
 * reachable and declined, a timeout says nothing about the peer at all, and a
 * reset says the connection existed and no longer does. Collapsing them is how
 * a dead peer and a firewalled one become indistinguishable in the source list.
 *
 * @c Eof and @c Destroying are terminal but are @b not failures. A peer that
 * closes cleanly after sending what it owed has not failed, and reporting it as
 * an error would penalise it in exactly the accounting a clean close should
 * leave alone.
 */
enum class EUtpTransportFailure
{
	//! No end yet: the stream is live, or ended cleanly.
	None,
	//! The peer answered and declined the connection.
	Refused,
	//! No answer within the transport's own retransmission budget.
	TimedOut,
	//! The connection existed and the peer tore it down.
	Reset,
	//! The peer finished sending. Not an error.
	Eof,
	//! The library is destroying the socket. Not an error.
	Destroying
};

inline bool IsUtpFailure(EUtpTransportFailure failure) noexcept
{
	return failure == EUtpTransportFailure::Refused || failure == EUtpTransportFailure::TimedOut ||
	       failure == EUtpTransportFailure::Reset;
}

inline bool IsUtpTerminal(EUtpTransportFailure failure) noexcept
{
	return failure != EUtpTransportFailure::None;
}

/**
 * The close bookkeeping, as a type rather than as a rule.
 *
 * libutp's @c utp_close() must be called exactly once per socket, and the
 * pointer is invalid the moment @c UTP_STATE_DESTROYING arrives. Both halves of
 * that are easy to get wrong by hand -- a destructor that closes, plus a state
 * callback that closes, is a double free that only appears when a peer
 * disconnects at the wrong moment.
 *
 * So the permission to close is consumed rather than checked: Take() answers
 * true once and false forever after, and there is no way to ask "was it closed"
 * and then close, which is the shape that races.
 */
class CUtpCloseOnce
{
public:
	//! True exactly once, for the caller that owns the close.
	bool Take() noexcept
	{
		const bool first = !m_taken;
		m_taken = true;
		return first;
	}

	//! True once the close has been handed out. For assertions, not for deciding.
	bool Taken() const noexcept { return m_taken; }

private:
	bool m_taken = false;
};

#endif // UTPTRANSPORTFAILURE_H
// File_checked_for_headers
