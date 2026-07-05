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

#include "HttpServer.h"

#include "JsonWriter.h"

#include <wx/string.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/optional.hpp>

#include <zlib.h>

#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace
{

// Per-connection session. Reads one request, hands it to the user
// handler, writes the response, closes. No keep-alive — the API
// surface is too small to benefit, and the one-shot model keeps the
// state machine trivially auditable. If the streaming_resolver
// matches the parsed request, the session takes a different path:
// writes the response head and runs the streaming handler on a
// worker thread, which can push chunks indefinitely via the Writer
// interface until the handler returns or the peer disconnects.
//
// Process-wide cap on concurrent SSE subscribers. Each session
// spawns one OS thread, so without a cap a non-loopback bind turns
// the thread-per-connection model into a DoS amplifier. The cap is
// sized for the single-operator dashboard pattern: a handful of
// browser tabs + the odd shell script. Refused sessions get
// `503 Service Unavailable` + a `Retry-After` hint inside the
// streaming dispatch path before the worker thread is created.
constexpr int kMaxConcurrentStreamingSessions = 32;
std::atomic<int> g_streaming_session_count{ 0 };

// Bodies smaller than this are sent uncompressed. Below ~250 bytes the
// gzip header (~10 bytes) + trailer (~8 bytes) plus the deflate block
// framing eats most of any ratio gain, and error/heartbeat payloads
// are common in that size range.
constexpr size_t kGzipMinBodyBytes = 256;

// Case-insensitive token search for "gzip" in an Accept-Encoding header
// value. Real clients (curl, browsers) send "gzip, deflate, br" or
// "gzip;q=1.0" — no legitimate client sends "gzip;q=0" (which per
// RFC 9110 means "explicitly not gzip") so we don't parse q-values;
// presence of the token is treated as accept. The `x-gzip` legacy
// alias is not honoured, which is fine because no client emitting it
// today would fail to also accept plain identity.
bool AcceptsGzip(const std::string &accept_encoding)
{
	if (accept_encoding.empty()) {
		return false;
	}
	auto is_boundary = [](char c) { return c == '\0' || c == ',' || c == ' ' || c == '\t' || c == ';'; };
	// Lowercase once so ::find is O(n) not O(n log n).
	std::string lc;
	lc.reserve(accept_encoding.size());
	for (char c : accept_encoding) {
		lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	for (size_t p = 0; (p = lc.find("gzip", p)) != std::string::npos; p += 4) {
		const bool left_ok = (p == 0) || is_boundary(lc[p - 1]);
		const char right = (p + 4 < lc.size()) ? lc[p + 4] : '\0';
		if (left_ok && is_boundary(right)) {
			return true;
		}
	}
	return false;
}

// One-shot gzip encoder for regular (non-streaming) response bodies.
// Returns false on any zlib error; the caller then serves the response
// uncompressed rather than 500ing, since a transient zlib failure
// shouldn't turn into a user-visible outage.
bool GzipOnce(const std::string &in, std::string &out)
{
	z_stream zs{};
	// windowBits = 15 + 16 → deflate with gzip wrapper (header + CRC
	// trailer). mem_level 8, default strategy — matches HTTP server
	// convention (nginx, Apache mod_deflate ship these defaults).
	if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return false;
	}
	zs.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(in.data()));
	zs.avail_in = static_cast<uInt>(in.size());
	// deflateBound is an upper bound on Z_FINISH output size; safe
	// to resize once and slice.
	const uLong bound = deflateBound(&zs, static_cast<uLong>(in.size()));
	out.resize(bound);
	// std::string::data() is const-qualified pre-C++17; `&out[0]` is
	// non-const in every standard we build against.
	zs.next_out = reinterpret_cast<Bytef *>(&out[0]);
	zs.avail_out = static_cast<uInt>(bound);
	const int rc = deflate(&zs, Z_FINISH);
	if (rc != Z_STREAM_END) {
		deflateEnd(&zs);
		out.clear();
		return false;
	}
	out.resize(zs.total_out);
	deflateEnd(&zs);
	return true;
}

// Streaming gzip encoder for SSE bodies. One instance per SSE session;
// Z_SYNC_FLUSH after each Compress() call emits the deflate block
// boundary immediately so the browser sees events live rather than
// after a compression buffer fills.
//
// The deflate dictionary is shared across events, so repeated JSON
// keys / hash prefixes / priority strings across events compress
// against the same reference — this is where the big ratio comes
// from on delta-heavy SSE streams.
class SseGzipStream
{
public:
	SseGzipStream() = default;
	// Non-copyable / non-movable: `m_z` holds internal pointers that
	// zlib manages; a copy would double-free at destruction, and a
	// move would leave the source in a state deflateEnd can't safely
	// handle. Owned by SocketWriter (held via shared_ptr elsewhere),
	// so no copy or move is needed in practice.
	SseGzipStream(const SseGzipStream &) = delete;
	SseGzipStream &operator=(const SseGzipStream &) = delete;
	SseGzipStream(SseGzipStream &&) = delete;
	SseGzipStream &operator=(SseGzipStream &&) = delete;

	bool Init()
	{
		if (deflateInit2(&m_z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
			Z_OK) {
			return false;
		}
		m_inited = true;
		return true;
	}
	~SseGzipStream()
	{
		if (m_inited) {
			deflateEnd(&m_z);
		}
	}

	// Deflate `in`, appending compressed bytes to `out`. Uses
	// Z_SYNC_FLUSH so accumulated bytes are byte-aligned and emitted
	// immediately. Returns false on zlib error.
	bool CompressSync(const std::string &in, std::string &out)
	{
		if (!m_inited) {
			return false;
		}
		m_z.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(in.data()));
		m_z.avail_in = static_cast<uInt>(in.size());
		Bytef scratch[16384];
		while (true) {
			m_z.next_out = scratch;
			m_z.avail_out = sizeof(scratch);
			const int rc = deflate(&m_z, Z_SYNC_FLUSH);
			if (rc != Z_OK && rc != Z_BUF_ERROR) {
				return false;
			}
			out.append(reinterpret_cast<char *>(scratch), sizeof(scratch) - m_z.avail_out);
			// Z_SYNC_FLUSH is complete when the flush block has
			// been emitted AND all input consumed. `avail_out > 0`
			// means we didn't hit the scratch boundary during the
			// current call, so deflate had room to finish flushing.
			if (m_z.avail_in == 0 && m_z.avail_out > 0) {
				break;
			}
		}
		return true;
	}

	// Emit any pending Z_FINISH trailer bytes (gzip CRC + length).
	// Called from the SSE worker's exit path before the chunked-
	// terminator so the browser sees a complete gzip stream. Safe to
	// call multiple times — subsequent calls return empty output.
	bool Finish(std::string &out)
	{
		if (!m_inited || m_finished) {
			return true;
		}
		m_finished = true;
		m_z.next_in = nullptr;
		m_z.avail_in = 0;
		Bytef scratch[4096];
		while (true) {
			m_z.next_out = scratch;
			m_z.avail_out = sizeof(scratch);
			const int rc = deflate(&m_z, Z_FINISH);
			out.append(reinterpret_cast<char *>(scratch), sizeof(scratch) - m_z.avail_out);
			if (rc == Z_STREAM_END) {
				return true;
			}
			if (rc != Z_OK && rc != Z_BUF_ERROR) {
				return false;
			}
		}
	}

private:
	z_stream m_z{};
	bool m_inited = false;
	bool m_finished = false;
};

class Session;

// Live SSE session registry. Listener::Stop walks this on shutdown
// to cancel each session's socket I/O — without it, a worker
// blocked in a synchronous Beast write to a slow peer never sees
// the io_context.stop() and the daemon hangs forever on exit.
// weak_ptrs so dead sessions self-clean.
std::mutex g_live_streams_mu;
std::vector<std::weak_ptr<Session>> g_live_streams;

class Session : public std::enable_shared_from_this<Session>
{
public:
	Session(tcp::socket socket,
		CHttpServer::Handler handler,
		CHttpServer::StreamingResolver streaming_resolver,
		CHttpServer::StreamingHandler streaming_handler,
		CHttpServer::StreamingPreflight streaming_preflight)
	: m_stream(std::move(socket))
	, m_handler(std::move(handler))
	, m_streaming_resolver(std::move(streaming_resolver))
	, m_streaming_handler(std::move(streaming_handler))
	, m_streaming_preflight(std::move(streaming_preflight))
	{
	}

	~Session()
	{
		// Worker captures `shared_from_this()` and sets
		// `m_worker_exited` true on the way out (via the RAII
		// WorkerExitMarker below). By the time this dtor runs the
		// last ref must have been dropped, so the worker must have
		// exited; join() from inside the worker's own call stack
		// would deadlock, detach() is safe.
		//
		// Hand-rolled if + std::abort instead of assert() so the
		// invariant is enforced in Release too (NDEBUG strips
		// assert).
		if (m_stream_worker.joinable()) {
			if (!m_worker_exited.load(std::memory_order_acquire)) {
				std::cerr << "amuleapi: FATAL Session dtor reached "
					     "with worker still running\n";
				std::abort();
			}
			m_stream_worker.detach();
		}
		// Release the session slot. Decrement only fires if we
		// actually acquired one (DispatchStreaming sets the flag).
		if (m_session_slot_held) {
			g_streaming_session_count.fetch_sub(1, std::memory_order_acq_rel);
		}
	}

	void Start() { DoRead(); }

	// Called by Listener::Stop from a foreign thread to cancel any
	// in-flight Beast write. Posts the close onto the stream's own
	// executor so we don't race the worker thread's last write —
	// boost::asio::tcp::socket is NOT thread-safe outside its
	// strand. The post is fire-and-forget; we don't wait for the
	// close to complete. The worker's writer.Alive() check picks up
	// the dead m_stream_alive flag, returns, and the session's last
	// shared_ptr ref drops, triggering destruction.
	void RequestCancel()
	{
		// Atomic flip first so writer.Alive() observes the cancel
		// even if the strand-posted close is delayed.
		m_stream_alive.store(false, std::memory_order_release);
		auto self = shared_from_this();
		boost::asio::post(m_stream.get_executor(), [self] {
			beast::error_code ec;
			beast::get_lowest_layer(self->m_stream).socket().close(ec);
		});
	}

private:
	void DoRead()
	{
		// Reset the parser each request — without it, calling DoRead a
		// second time on the same stream blocks forever with a partial
		// state. only reads one request, but leaving the reset
		// in keeps the read loop forward-compatible if keep-alive is
		// turned on later.
		m_parser.emplace();
		// 1 MiB request cap — bigger than any sensible REST POST body
		// (login JSON is ~64 bytes, etc.) but well under "someone is
		// trying to exhaust memory by upload-pumping us".
		m_parser->body_limit(1024 * 1024);
		// 16 KiB header cap. Beast's default header_limit varies
		// across versions (8 KiB in current upstream, larger on
		// older releases). A drip-feed attacker who slowly streams
		// header lines can otherwise grow the flat_buffer until the
		// 10 s read timeout catches them — but that's 10 s × pool
		// of concurrent peers. Cap headers explicitly so the
		// per-peer memory ceiling is fixed regardless of upstream
		// defaults: 16 KiB is well over any legitimate request
		// (Authorization + a few Accept headers is < 2 KiB) and
		// catches the drip-feed within ~1 KiB instead of ~MB.
		m_parser->header_limit(16 * 1024);
		// 10 s read timeout. amuleapi runs against localhost/LAN; a
		// real client never takes 10 s to send a 1 KiB request.
		m_stream.expires_after(std::chrono::seconds(10));

		auto self = shared_from_this();
		http::async_read(
			m_stream, m_buffer, *m_parser, [self](beast::error_code ec, std::size_t bytes) {
				(void)bytes;
				if (ec == http::error::end_of_stream) {
					self->DoClose();
					return;
				}
				if (ec) {
					// Read error (timeout, peer close, framing error) —
					// stay quiet. amuleapi-side log noise from health-
					// check probes ("000 errors are normal" in our
					// curl-tests README) isn't worth the line per
					// connection.
					self->DoClose();
					return;
				}
				self->Dispatch();
			});
	}

	void Dispatch()
	{
		const auto &req = m_parser->get();

		CHttpServer::Request r;
		r.method = std::string(req.method_string());
		r.target = std::string(req.target());
		r.body = req.body();
		for (const auto &h : req) {
			r.headers.emplace(std::string(h.name_string()), std::string(h.value()));
		}
		// Content-Encoding negotiation is per-request state, not per-
		// response. Sample once here so both the regular WriteResponse
		// path and the streaming SocketWriter see the same decision;
		// the header can't legally change between the two.
		m_accepts_gzip = AcceptsGzip(std::string(req[http::field::accept_encoding]));
		// Remote endpoint for rate-limiting. `.address()` returns a
		// boost::asio::ip::address which `.to_string()`-es to the
		// canonical IPv4 / IPv6 form ("192.0.2.1", "::1", "fe80::%lo0"...).
		// Wrapped in an error_code overload so a half-closed socket
		// doesn't throw — empty `remote_addr` falls through to "no
		// per-IP bucket" in the rate limiter, which is the safe default.
		{
			beast::error_code ec;
			const auto ep = m_stream.socket().remote_endpoint(ec);
			if (!ec)
				r.remote_addr = ep.address().to_string();
		}

		// streaming dispatch. The streaming_resolver is
		// invoked synchronously and short-circuits the standard
		// request→response→close path when it returns true.
		if (m_streaming_resolver && m_streaming_handler && m_streaming_resolver(r)) {
			DispatchStreaming(std::move(r));
			return;
		}

		CHttpServer::Response resp;
		try {
			resp = m_handler(r);
		} catch (const std::exception &e) {
			// Handler exceptions become 500s; body shape matches the
			// rest of the error contract.
			//
			// Info-disclosure: e.what() can carry caller-supplied
			// bytes (picojson echoes the offending input character;
			// a future header-driven throw could reflect
			// Authorization or Cookie fragments). Keep the body
			// generic; log detail to stderr.
			std::cerr << "amuleapi: 500 from handler: " << e.what() << "\n";
			resp.status = 500;
			resp.content_type = "application/json";
			resp.body = "{\"error\":{\"code\":\"internal\","
				    "\"message\":\"internal server error\"}}";
		}

		WriteResponse(std::move(resp));
	}

	// Streaming path. Writes the response head, then spawns a worker
	// thread for the streaming handler. Session stays alive across
	// the worker via the `shared_from_this()` capture; when the
	// worker exits, the lambda releases the last ref and Session
	// destructs (joining a thread that has already exited, a no-op).
	//
	// Short 503 for concurrent-session cap + other exhaustion paths.
	void WriteCapRefusal()
	{
		CHttpServer::Response refused;
		refused.status = 503;
		refused.content_type = "application/json";
		refused.headers["Retry-After"] = "10";
		refused.body = "{\"error\":{\"code\":\"sessions_exhausted\","
			       "\"message\":\"too many concurrent streaming sessions; "
			       "retry in a few seconds\"}}";
		WriteResponse(std::move(refused));
	}

	void DispatchStreaming(CHttpServer::Request r)
	{
		// Preflight runs synchronously on the I/O thread BEFORE the
		// slot is claimed and BEFORE a worker thread is spawned. If
		// it rejects (returns a Response), unauthenticated peers
		// can't tie up a streaming slot for the read-timeout window:
		// 32 unauth TCP holds × 10 s read timeout = a 320-session-
		// second pool stall. With preflight, an unauth request
		// burns one short HTTP exchange and goes away. Empty
		// preflight (default) preserves the prior contract.
		if (m_streaming_preflight) {
			boost::optional<CHttpServer::Response> rej = m_streaming_preflight(r);
			if (rej) {
				WriteResponse(std::move(*rej));
				return;
			}
		}

		// Acquire a session slot before doing any thread-spawn or
		// long-lived work. fetch_add returns the OLD value, so we
		// hold the slot iff that old value was strictly below the
		// cap. Otherwise we roll back and refuse the connection.
		const int prior_count = g_streaming_session_count.fetch_add(1, std::memory_order_acq_rel);
		if (prior_count >= kMaxConcurrentStreamingSessions) {
			g_streaming_session_count.fetch_sub(1, std::memory_order_acq_rel);
			WriteCapRefusal();
			return;
		}
		m_session_slot_held = true;
		// Disable read timeout — SSE connections are long-lived.
		m_stream.expires_never();
		m_stream_alive.store(true, std::memory_order_release);

		// Register the session so Listener::Stop can cancel its
		// socket on shutdown. A worker blocked inside a synchronous
		// Beast write to a slow peer otherwise pins the daemon at
		// exit — the io_context.stop() doesn't unblock a write
		// already in flight. Closing the underlying socket from
		// outside makes the write fail with EPIPE and the worker
		// returns to its writer.Alive() check, exits, releases the
		// last ref.
		{
			std::lock_guard<std::mutex> g(g_live_streams_mu);
			g_live_streams.emplace_back(shared_from_this());
		}

		// Spawn the worker thread that runs the streaming handler.
		// The worker captures `self` so the Session stays alive
		// across its run, and references to the head out-params (held
		// on the heap so the SocketWriter can read them at first-
		// write time).
		auto handler = m_streaming_handler;
		auto self = shared_from_this();
		// Head data — owned by the worker thread, referenced by the
		// SocketWriter (via SocketWriter::HeadData). Defaults set
		// here; handler can overwrite before calling writer.Write
		// the first time.
		auto head = std::make_shared<SocketWriter::HeadData>();
		head->headers["Cache-Control"] = "no-cache";
		head->headers["Connection"] = "keep-alive";
		// nginx (and many other reverse proxies) buffer response
		// bodies by default when they detect chunked-transfer +
		// text-ish content, which stalls SSE delivery entirely — the
		// operator sees "events show up in bursts every N seconds"
		// with N depending on how large the proxy's default buffer
		// is. `X-Accel-Buffering: no` is nginx's opt-out (also
		// respected by ingress-nginx / OpenResty); harmless on
		// backends that don't recognise it. Emitted regardless of
		// gzip status because the buffering problem is orthogonal.
		head->headers["X-Accel-Buffering"] = "no";

		auto writer = std::make_shared<SocketWriter>(self, head, m_accepts_gzip);

		// One std::thread per streaming session — cheap at expected
		// scale (1–5 concurrent SSE subscribers) and keeps the drain
		// synchronous so `since_id` ordering is trivial.
		//
		// **The default `BindAddress=127.0.0.1` is load-bearing.**
		// Non-loopback bind + unauth peer = thread-per-connection DoS
		// amplifier. PreflightEvents (auth before slot claim, before
		// thread spawn) bounds pre-auth cost to one HTTP exchange.
		m_stream_worker = std::thread([self, handler, writer, head, r = std::move(r)]() mutable {
			// RAII guard: tip the worker-exited flag on EVERY exit
			// path out of this lambda, including a future refactor
			// that adds an early `return` after the catch block.
			// The Session destructor's std::abort() guard only
			// fires if this flag is true, so missing the flip on
			// some path would tear down a still-running thread.
			struct WorkerExitMarker
			{
				std::shared_ptr<Session> s;
				~WorkerExitMarker()
				{
					s->m_worker_exited.store(true, std::memory_order_release);
				}
			} marker{ self };
			try {
				handler(r, *writer, head->status, head->content_type, head->headers);
			} catch (const std::exception &) {
				// Streaming handler exceptions are silent — close
				// quietly.
			}
			// If the handler returned without writing anything (e.g.
			// auth-rejected on a HEAD probe), still emit the head so
			// the client sees the right status code.
			writer->EnsureHeadWritten();
			// Emit gzip trailer (Z_FINISH) if compressing, BEFORE
			// DoClose writes the chunked-terminator. Otherwise the
			// browser's gzip decoder sees a truncated stream at
			// end-of-response and raises a decoding error.
			writer->Finalize();
			self->DoClose();
			// `marker` runs here, flipping m_worker_exited and
			// dropping `self` only AFTER the flag is set — so the
			// dtor's check observes the post-exit state.
		});
	}

	// Per-streaming-session Writer that marshals writes onto the
	// socket. Defers writing the HTTP response head until the first
	// Write call — that's when the streaming handler has finalised
	// status / content_type / headers via the out-params we pass it.
	class SocketWriter : public CHttpServer::Writer
	{
	public:
		struct HeadData
		{
			unsigned status = 200;
			std::string content_type = "text/event-stream";
			std::map<std::string, std::string> headers;
		};

		SocketWriter(std::shared_ptr<Session> session,
			std::shared_ptr<Session::SocketWriter::HeadData> head,
			bool wants_gzip)
		: m_session(std::move(session))
		, m_head(std::move(head))
		, m_wants_gzip(wants_gzip)
		{
		}

		bool Write(const std::string &chunk) override
		{
			if (!m_session->m_stream_alive.load(std::memory_order_acquire)) {
				return false;
			}
			if (!EnsureHeadWritten()) {
				return false;
			}

			// SSE wire shape uses chunked transfer encoding; each
			// "chunk" written here is a single HTTP/1.1 chunk frame:
			//  <hex-size-of-chunk>\r\n<chunk-bytes>\r\n
			//
			// Zero-length chunks would terminate the message (per
			// RFC 7230 §4.1) so we skip them — the heartbeat path
			// always passes at least ": keepalive\n\n" anyway.
			if (chunk.empty()) {
				return true;
			}
			// gzip path: run the SSE bytes through the persistent
			// deflate stream with Z_SYNC_FLUSH so the block is
			// emitted immediately (browser sees the event live) and
			// the dictionary carries across events (repeated JSON
			// keys / hash prefixes compress to a few bits). On a
			// deflate error we fall through to the uncompressed
			// path for THIS chunk — but zlib doesn't recover its
			// stream state after an error mid-session, so future
			// chunks would also be broken; we tear the session
			// down by returning false rather than silently emit a
			// mid-stream encoding mismatch that the browser would
			// abort on anyway.
			const std::string *payload = &chunk;
			std::string compressed;
			if (m_wants_gzip) {
				if (!m_gzip.CompressSync(chunk, compressed)) {
					m_session->m_stream_alive.store(false, std::memory_order_release);
					return false;
				}
				// Z_SYNC_FLUSH on an empty input still emits the
				// 5-byte block-boundary marker, so `compressed`
				// is never empty here for a non-empty `chunk`.
				payload = &compressed;
			}
			std::ostringstream framed;
			framed << std::hex << payload->size() << "\r\n" << *payload << "\r\n";
			const std::string out = framed.str();

			std::lock_guard<std::mutex> g(m_session->m_socket_mu);
			beast::error_code ec;
			asio::write(m_session->m_stream.socket(), asio::buffer(out), ec);
			if (ec) {
				m_session->m_stream_alive.store(false, std::memory_order_release);
				return false;
			}
			return true;
		}

		// Emit any Z_FINISH trailer bytes (gzip CRC + length) as a
		// final chunked frame. Called from the SSE worker's exit
		// path BEFORE DoClose so the browser sees a well-terminated
		// gzip stream when it expected one. Safe on non-gzip
		// sessions (no-op), safe to call multiple times (idempotent
		// via SseGzipStream::m_finished).
		void Finalize()
		{
			if (!m_wants_gzip) {
				return;
			}
			if (!m_session->m_stream_alive.load(std::memory_order_acquire)) {
				return;
			}
			std::string tail;
			if (!m_gzip.Finish(tail) || tail.empty()) {
				return;
			}
			std::ostringstream framed;
			framed << std::hex << tail.size() << "\r\n" << tail << "\r\n";
			const std::string out = framed.str();

			std::lock_guard<std::mutex> g(m_session->m_socket_mu);
			beast::error_code ec;
			asio::write(m_session->m_stream.socket(), asio::buffer(out), ec);
			if (ec) {
				m_session->m_stream_alive.store(false, std::memory_order_release);
			}
		}

		bool Alive() const override
		{
			return m_session->m_stream_alive.load(std::memory_order_acquire);
		}

		// Idempotent: writes the head once, on first call. Returns
		// false if the underlying socket write failed.
		//
		// We build the head as raw bytes rather than going through
		// Beast's response<empty_body> + prepare_payload() — that
		// path emits `Content-Length: 0` and silently strips our
		// `Transfer-Encoding: chunked`, which forecloses the
		// streaming body that the SSE channel needs. Direct
		// string-formatted head dodges the conflict and is short
		// enough to audit at a glance.
		bool EnsureHeadWritten()
		{
			if (m_head_written.exchange(true, std::memory_order_acq_rel)) {
				return true;
			}
			// Late gzip init + header injection. Deferred to here so
			// a) SocketWriter's ctor stays trivial, and b) the
			// handler had a chance to override head->headers between
			// construction and first Write. If deflateInit2 itself
			// fails we downgrade to identity encoding for this
			// session rather than 500 the whole SSE — the head
			// hasn't gone out yet, so it's safe to erase the flag.
			if (m_wants_gzip) {
				if (m_gzip.Init()) {
					m_head->headers["Content-Encoding"] = "gzip";
					if (m_head->headers.find("Vary") == m_head->headers.end()) {
						m_head->headers["Vary"] = "Accept-Encoding";
					}
				} else {
					m_wants_gzip = false;
				}
			}
			std::ostringstream head;
			head << "HTTP/1.1 " << m_head->status << " ";
			switch (m_head->status) {
			case 200:
				head << "OK";
				break;
			case 401:
				head << "Unauthorized";
				break;
			case 403:
				head << "Forbidden";
				break;
			case 404:
				head << "Not Found";
				break;
			default:
				head << "OK";
				break;
			}
			head << "\r\n";
			head << "Server: amuleapi\r\n";
			head << "Content-Type: " << m_head->content_type << "\r\n";
			// Chunked when the body will actually stream — i.e.
			// success path. Error responses (401 / 403) are single-
			// shot: the handler emits one chunk-as-error-body then
			// returns, and we'd rather close-on-FIN than dangle a
			// chunked half-message. For those we omit
			// Transfer-Encoding so the response simply terminates
			// at connection close.
			const bool chunked = (m_head->status >= 200 && m_head->status < 300);
			if (chunked) {
				head << "Transfer-Encoding: chunked\r\n";
			}
			for (const auto &kv : m_head->headers) {
				head << kv.first << ": " << kv.second << "\r\n";
			}
			head << "\r\n";
			const std::string head_bytes = head.str();

			std::lock_guard<std::mutex> g(m_session->m_socket_mu);
			beast::error_code ec;
			asio::write(m_session->m_stream.socket(), asio::buffer(head_bytes), ec);
			if (ec) {
				m_session->m_stream_alive.store(false, std::memory_order_release);
				return false;
			}
			return true;
		}

	private:
		std::shared_ptr<Session> m_session;
		std::shared_ptr<HeadData> m_head;
		std::atomic<bool> m_head_written{ false };
		// Sampled once at construction from the request's Accept-
		// Encoding. Cleared at head-write time if deflateInit2
		// fails, so subsequent Write calls fall through to the
		// identity path without leaking a partially-init'd stream.
		bool m_wants_gzip;
		SseGzipStream m_gzip;
	};

	void WriteResponse(CHttpServer::Response &&resp)
	{
		// Regular (non-streaming) response gzip encoding. Gated by:
		//  * client Accept-Encoding contains gzip,
		//  * body is above kGzipMinBodyBytes (header overhead is a
		//    significant fraction below that),
		//  * handler didn't already set Content-Encoding (a future
		//    pre-gzipped static asset path would use that hook).
		// deflate() fallback: on any zlib error we ship the original
		// uncompressed body rather than 500 — better degraded than
		// broken. Content-Length is set correctly by
		// prepare_payload() based on the post-swap body.
		//
		// Vary: Accept-Encoding is added regardless of whether *this*
		// response was compressed, so any intermediary cache keys the
		// entry correctly across clients that do / don't send the
		// header.
		if (m_accepts_gzip && resp.body.size() >= kGzipMinBodyBytes &&
			resp.headers.find("Content-Encoding") == resp.headers.end()) {
			std::string compressed;
			if (GzipOnce(resp.body, compressed)) {
				resp.body = std::move(compressed);
				resp.headers["Content-Encoding"] = "gzip";
			}
		}
		if (resp.headers.find("Vary") == resp.headers.end()) {
			resp.headers["Vary"] = "Accept-Encoding";
		}

		m_response.emplace();
		m_response->version(11);
		m_response->result(resp.status);
		m_response->set(http::field::server, "amuleapi");
		m_response->set(http::field::content_type, resp.content_type);
		for (const auto &h : resp.headers) {
			m_response->set(h.first, h.second);
		}
		m_response->body() = std::move(resp.body);
		m_response->prepare_payload();

		auto self = shared_from_this();
		http::async_write(m_stream, *m_response, [self](beast::error_code ec, std::size_t) {
			(void)ec;
			self->DoClose();
		});
	}

	void DoClose()
	{
		// If we were streaming, write the chunked-encoding terminator
		// (0-size chunk) before shutting down. Idempotent — if the
		// peer already closed, the write fails silently.
		if (m_stream_alive.exchange(false, std::memory_order_acq_rel)) {
			std::lock_guard<std::mutex> g(m_socket_mu);
			beast::error_code ec;
			asio::write(m_stream.socket(), asio::buffer(std::string("0\r\n\r\n")), ec);
		}
		beast::error_code ec;
		m_stream.socket().shutdown(tcp::socket::shutdown_send, ec);
		// `ec` deliberately discarded — peer may have already gone
		// away.
	}

	beast::tcp_stream m_stream;
	beast::flat_buffer m_buffer{ 8192 };
	boost::optional<http::request_parser<http::string_body>> m_parser;
	boost::optional<http::response<http::string_body>> m_response;
	CHttpServer::Handler m_handler;

	// streaming state.
	CHttpServer::StreamingResolver m_streaming_resolver;
	CHttpServer::StreamingHandler m_streaming_handler;
	CHttpServer::StreamingPreflight m_streaming_preflight;
	std::atomic<bool> m_stream_alive{ false };
	// Set true by the worker on exit. The Session destructor asserts
	// on it before detach()ing the thread handle (Session is shared-
	// ptr-owned by the worker, so dtor only runs after the last ref
	// drops — and that ref is held by the worker lambda, which only
	// releases it as a final statement).
	std::atomic<bool> m_worker_exited{ false };
	std::mutex m_socket_mu;
	std::thread m_stream_worker;
	// Whether this session is accounted against
	// g_streaming_session_count. Set in DispatchStreaming after a
	// successful slot acquisition; the dtor decrements iff this is
	// true so refused-cap sessions don't double-account.
	bool m_session_slot_held = false;
	// Sampled from the request's Accept-Encoding header in Dispatch.
	// Read by WriteResponse (single-shot body compression) and by
	// DispatchStreaming (per-event Z_SYNC_FLUSH on the SSE socket
	// writer). Kept as a plain bool because both readers run either on
	// the same thread that populated it or on a worker spawned after
	// the write, so no atomic is needed.
	bool m_accepts_gzip = false;
};

// Accept loop. One Listener per HttpServer; spawns a Session per
// connection via shared_from_this.
class Listener : public std::enable_shared_from_this<Listener>
{
public:
	Listener(asio::io_context &ioc,
		tcp::endpoint endpoint,
		CHttpServer::Handler handler,
		CHttpServer::StreamingResolver streaming_resolver,
		CHttpServer::StreamingHandler streaming_handler,
		CHttpServer::StreamingPreflight streaming_preflight)
	: m_ioc(ioc)
	, m_acceptor(asio::make_strand(ioc))
	, m_handler(std::move(handler))
	, m_streaming_resolver(std::move(streaming_resolver))
	, m_streaming_handler(std::move(streaming_handler))
	, m_streaming_preflight(std::move(streaming_preflight))
	{
		beast::error_code ec;
		m_acceptor.open(endpoint.protocol(), ec);
		if (ec) {
			m_error = ec.message();
			return;
		}
		m_acceptor.set_option(asio::socket_base::reuse_address(true), ec);
		if (ec) {
			m_error = ec.message();
			return;
		}
		m_acceptor.bind(endpoint, ec);
		if (ec) {
			m_error = ec.message();
			return;
		}
		m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
		if (ec) {
			m_error = ec.message();
			return;
		}
	}

	bool Ok() const { return m_error.empty(); }
	const std::string &Error() const { return m_error; }

	void Run() { DoAccept(); }
	void Stop()
	{
		beast::error_code ec;
		m_acceptor.close(ec);
		// Cancel every live SSE session's socket so workers blocked
		// inside synchronous Beast writes return promptly. Without
		// this, a slow peer holds its worker thread inside the
		// write call until the kernel TCP timeout (~minutes) and
		// CHttpServer::Stop joins the io_context thread, which
		// joins indefinitely waiting for the workers.
		std::vector<std::shared_ptr<Session>> live;
		{
			std::lock_guard<std::mutex> g(g_live_streams_mu);
			live.reserve(g_live_streams.size());
			for (auto &w : g_live_streams) {
				if (auto s = w.lock())
					live.push_back(std::move(s));
			}
			g_live_streams.clear();
		}
		for (auto &s : live)
			s->RequestCancel();
	}

private:
	void DoAccept()
	{
		auto self = shared_from_this();
		m_acceptor.async_accept(
			asio::make_strand(m_ioc), [self](beast::error_code ec, tcp::socket socket) {
				if (!ec) {
					std::make_shared<Session>(std::move(socket),
						self->m_handler,
						self->m_streaming_resolver,
						self->m_streaming_handler,
						self->m_streaming_preflight)
						->Start();
				}
				// Loop unless the acceptor has been closed. operation_aborted
				// fires on Stop() and signals "exit cleanly".
				if (ec != asio::error::operation_aborted) {
					self->DoAccept();
				}
			});
	}

	asio::io_context &m_ioc;
	tcp::acceptor m_acceptor;
	CHttpServer::Handler m_handler;
	CHttpServer::StreamingResolver m_streaming_resolver;
	CHttpServer::StreamingHandler m_streaming_handler;
	CHttpServer::StreamingPreflight m_streaming_preflight;
	std::string m_error;
};

} // namespace

struct CHttpServer::Impl
{
	asio::io_context ioc{ 1 };
	std::shared_ptr<Listener> listener;
	std::thread thread;
	std::atomic<bool> running{ false };
};

CHttpServer::CHttpServer() = default;

CHttpServer::~CHttpServer()
{
	Stop();
}

bool CHttpServer::Start(const std::string &bind_address,
	unsigned port,
	Handler handler,
	StreamingResolver streaming_resolver,
	StreamingHandler streaming_handler,
	StreamingPreflight streaming_preflight)
{
	if (m_impl) {
		m_lastError = "HttpServer already started";
		return false;
	}
	m_impl = std::make_unique<Impl>();

	beast::error_code ec;
	const auto addr = asio::ip::make_address(bind_address, ec);
	if (ec) {
		m_lastError = "invalid bind address '" + bind_address + "': " + ec.message();
		m_impl.reset();
		return false;
	}
	tcp::endpoint endpoint(addr, static_cast<unsigned short>(port));

	// Bind hygiene warning. amuleapi's HTTP server uses a thread-
	// per-streaming-session model bounded by a process-wide cap
	// (kMaxConcurrentStreamingSessions). On loopback this is fine
	// — the only callers are the operator's own clients. Off
	// loopback, the same model is a DoS amplifier: any peer can
	// open enough preauth connections to consume the cap and lock
	// out legitimate subscribers. Surface a one-time WARN on
	// startup so an operator switching the bind catches this in
	// the daemon log; the SSE session cap still enforces the
	// upper bound regardless.
	if (!addr.is_loopback()) {
		std::cerr << "amuleapi: WARN BindAddress=" << bind_address
			  << " is not loopback. SSE sessions are capped at "
			  << kMaxConcurrentStreamingSessions
			  << " concurrent — beyond that the daemon returns "
			     "503. Put a reverse proxy in front for remote "
			     "access.\n";
	}

	m_impl->listener = std::make_shared<Listener>(m_impl->ioc,
		endpoint,
		std::move(handler),
		std::move(streaming_resolver),
		std::move(streaming_handler),
		std::move(streaming_preflight));
	if (!m_impl->listener->Ok()) {
		m_lastError = "bind to " + bind_address + ":" + std::to_string(port) +
			      " failed: " + m_impl->listener->Error();
		m_impl.reset();
		return false;
	}
	m_impl->listener->Run();

	m_impl->running.store(true, std::memory_order_release);
	m_impl->thread = std::thread([this] {
		try {
			m_impl->ioc.run();
		} catch (const std::exception &e) {
			// io_context exception propagation: the server thread dies
			// quietly. Catch + log to stderr so an operator running in
			// foreground sees a one-line cause; daemon mode loses the
			// message.
			std::cerr << "amuleapi: HTTP I/O loop exited on exception: " << e.what() << '\n';
		}
		m_impl->running.store(false, std::memory_order_release);
	});
	return true;
}

void CHttpServer::Stop()
{
	if (!m_impl)
		return;
	if (m_impl->listener)
		m_impl->listener->Stop();
	m_impl->ioc.stop();
	if (m_impl->thread.joinable())
		m_impl->thread.join();
	m_impl.reset();
}
