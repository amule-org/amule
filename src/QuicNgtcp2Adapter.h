//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef QUICNGTCP2ADAPTER_H
#define QUICNGTCP2ADAPTER_H

#include "QuicContext.h"
#include "QuicTls.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class CQuicSocketTransport;
class IQuicStreamAcceptor;

struct CQuicTlsPolicy
{
	const IQuicTlsCredentials *credentials = nullptr;
};

struct CQuicInitialMetadata
{
	uint32_t version;
	std::string destinationCid;
	std::string sourceCid;
};

class IQuicDatagramSink
{
public:
	virtual ~IQuicDatagramSink() = default;
	virtual bool SendDatagram(const uint8_t *, size_t, const CNetworkAddress &, uint16_t) = 0;
};

class IQuicNgtcp2Engine
{
public:
	using Handle = void *;
	virtual ~IQuicNgtcp2Engine() = default;
	//! @p nowMs stamps ngtcp2_settings::initial_ts, the base the handshake timeout deadline is
	//! measured from -- without it the deadline would be relative to time zero, already passed
	//! for any connection created after this process's first few seconds of life.
	virtual Handle CreateServer(const CQuicTlsPolicy &,
		const CNetworkAddress &,
		uint16_t,
		const CQuicInitialMetadata &,
		uint64_t nowMs) = 0;
	//! @p nowMs is the same monotonic millisecond tick CQuicContext::ProcessDatagram() received.
	virtual bool Read(Handle, const uint8_t *, size_t, uint64_t nowMs) = 0;
	virtual bool Flush(
		Handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t, uint64_t nowMs) = 0;
	//! Backed by ngtcp2_conn_get_scid(): the source connection IDs issued and not retired.
	virtual bool OwnsConnectionId(Handle, const std::string &) const = 0;
	//! The connection ID this handle issued at creation. Empty if none.
	virtual std::string GetIssuedConnectionId(Handle) const = 0;
	//! The connection-level flow-control window this handle advertised to the peer at
	//! creation (ngtcp2_transport_params::initial_max_data). 0 if no connection.
	virtual uint64_t GetAdvertisedReadWindow(Handle) const = 0;
	//! Bytes received so far on the peer's first opened stream, and clears them. There is
	//! exactly one stream per connection by design (see QuicSocketTransport.h): eD2k always
	//! replies on the peer's own stream and never opens one of its own.
	virtual std::vector<uint8_t> DrainStreamData(Handle) = 0;
	//! Reopens @p bytes of stream- and connection-level flow control, as the application
	//! drains what DrainStreamData() returned (ngtcp2_conn_extend_max_stream_offset() +
	//! ngtcp2_conn_extend_max_offset()). A no-op if no stream has opened yet.
	virtual void ExtendStreamReadWindow(Handle, size_t bytes) = 0;
	//! Services this connection's RFC 9002 loss-detection/retransmission and idle timers, and
	//! sends anything that produces (ngtcp2_conn_get_expiry()/handle_expiry()). False only if
	//! the timer handling was fatal (e.g. the idle timeout elapsed): the caller must then close
	//! the connection rather than keep ticking it.
	virtual bool Tick(Handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t, uint64_t nowMs) = 0;
	//! True once the peer has opened its one stream (QuicSocketTransport.h). The point at which
	//! a connection has something worth handing to the rest of aMule.
	virtual bool HasOpenStream(Handle) const = 0;
	//! From this call on, new stream bytes go straight to @p transport (still non-owning: the
	//! caller keeps it alive) instead of being buffered for DrainStreamData(). Whatever
	//! DrainStreamData() had not yet returned is not retroactively delivered -- the caller must
	//! drain once more immediately before attaching, or accept that race as the hand-off boundary.
	virtual void AttachTransport(Handle, CQuicSocketTransport *transport) = 0;
	//! Writes @p data on the peer's stream and, if that produces a packet, sends it through
	//! @p sink immediately -- there is no separate buffering step, since ngtcp2_conn_writev_stream()
	//! both accepts stream data and may produce output in the same call. Returns the number of
	//! bytes of @p data actually accepted (matching IQuicStreamOperations::WriteStream()'s
	//! contract), or a negative value on a fatal connection error.
	virtual std::ptrdiff_t WriteStreamData(Handle,
		const uint8_t *data,
		size_t length,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		uint64_t nowMs) = 0;
	//! Abruptly closes the peer's stream (ngtcp2_conn_shutdown_stream()). This resets the stream
	//! and discards anything still unacknowledged -- reserved for genuine errors (ConnectionInfo
	//! itself was already fatal somehow); CloseStreamGracefully() below is the one a clean,
	//! voluntary close should use instead. IsStreamEnded() is what notices the stream is gone
	//! either way (reset or a clean close) and is how the owning connection actually decides to
	//! end itself over it.
	virtual void ShutdownStream(Handle) = 0;
	//! Sends an empty STREAM frame with FIN set, ending the write side of this connection's one
	//! stream without discarding anything: unlike ShutdownStream(), whatever is already in
	//! ConnectionInfo::unackedSendChunks keeps being retransmitted normally until acknowledged,
	//! exactly as if the application had simply stopped writing. This is what a voluntary,
	//! clean close (CQuicSocketTransport::Close()) should ask for -- the whole reason a
	//! connection's one stream ending is distinct from an application error in the first place.
	virtual void CloseStreamGracefully(
		Handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t, uint64_t nowMs) = 0;
	//! True once this connection's one stream has ended -- cleanly (ngtcp2's stream_close) or by
	//! reset (stream_reset), from either side. With exactly one stream per connection by design
	//! (QuicSocketTransport.h), there is nothing left to use the connection for past this point.
	virtual bool IsStreamEnded(Handle) const = 0;
	//! Sends CONNECTION_CLOSE and destroys the connection, in one call: ngtcp2 forbids calling
	//! ngtcp2_conn_write_connection_close() from inside a callback, so this cannot be folded into
	//! the stream_close/stream_reset callbacks IsStreamEnded() reads -- the caller makes this call
	//! once Read()/Flush()/Tick() has already returned.
	virtual void EndConnection(
		Handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t, uint64_t nowMs) = 0;
	//! Tells the attached transport, if any, that it may be able to write again -- the peer
	//! extending flow control, or congestion easing, are both things only noticed by processing
	//! a datagram or a tick, never by the write attempt that was blocked in the first place.
	//! Cheap to call unconditionally: a transport with nothing queued just returns immediately.
	virtual void NotifyWritable(Handle) = 0;
	virtual void Destroy(Handle) = 0;
};

class CQuicNgtcp2Factory final : public IQuicConnectionFactory
{
public:
	//! localIdentity is the local eD2k client's 16-byte userhash: what this side's half of the
	//! EAQN1 proof exchange (QuicNattProtocol.h) asserts as "who we are" to every peer this
	//! factory admits. Taken by value rather than reaching for thePrefs::GetUserHash() inside
	//! the connection itself, so this engine stays testable without theApp -- the same reason
	//! credentials and the sink arrive as constructor arguments instead.
	//! @p transportClock is forwarded to every CQuicSocketTransport this factory's connections
	//! accept a stream onto (QuicSocketTransport.h's own clock injection) -- default (nullptr)
	//! leaves each transport on its own default, the real clock. Tests substitute a fake so a
	//! transport's Flush()/Close() stay on the same synthetic timeline as the engine calls
	//! driving the test.
	CQuicNgtcp2Factory(const CQuicTlsPolicy &,
		std::shared_ptr<IQuicDatagramSink>,
		std::shared_ptr<IQuicNgtcp2Engine>,
		const std::array<uint8_t, 16> &localIdentity,
		std::function<uint64_t()> transportClock = nullptr);
	//! Held, not owned: the caller (CClientUDPSocket) keeps the acceptor alive for at least as
	//! long as this factory. Null refuses every stream's hand-off, which is the state before an
	//! acceptor exists -- the connection itself is still admitted and still works.
	void SetAcceptor(IQuicStreamAcceptor *acceptor);
	std::unique_ptr<IQuicConnection> CreateInbound(const uint8_t *,
		size_t,
		const CNetworkAddress &,
		uint16_t,
		const std::string &,
		uint64_t nowMs) override;

private:
	CQuicTlsPolicy m_policy;
	std::shared_ptr<IQuicDatagramSink> m_sink;
	std::array<uint8_t, 16> m_localIdentity;
	std::shared_ptr<IQuicNgtcp2Engine> m_engine;
	std::function<uint64_t()> m_transportClock;
	IQuicStreamAcceptor *m_acceptor = nullptr;
};

//! The real ngtcp2/GnuTLS-backed engine every production connection uses. Tests inject their own
//! fake IQuicNgtcp2Engine instead; this one is never fail-closed by design past CreateServer()'s
//! own precondition checks (credentials present and GnuTLS-backed).
std::shared_ptr<IQuicNgtcp2Engine> CreateProductionQuicNgtcp2Engine();

#endif
