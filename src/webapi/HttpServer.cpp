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

#include <Etag.h> // webcommon::WithCodingSuffix

#include <wx/string.h>

// See the note in LibSocketAsio.cpp: Boost 1.92's asio trips
// -Wdeprecated-copy-with-user-provided-dtor, which this build treats as an
// error. Suppressed across the includes only.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#endif
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/optional.hpp>
// Inside the suppression block on purpose: it pulls in the same asio headers
// the lines above do, and reaching them first from an unguarded include would
// resurrect the -Wdeprecated-copy-with-user-provided-dtor error.
#include "RangeFileBody.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// strncasecmp lives in <strings.h> on POSIX (glibc also exposes it via
// <string.h>, but musl/BSDs don't), and MSVC spells it _strnicmp. The same
// shim Api.cpp and libwebcommon/HeaderParse.cpp carry: this file reads a
// header name case-insensitively on the transport error paths, and it should
// not depend on the declaration arriving transitively through someone else's
// include.
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#include <zlib.h>

#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstring>
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

// See HttpServer.h.
//
// Factored out deliberately. This logic existed in three places -- the regular
// response writer, the SSE head writer, and the dispatcher's CORS pass -- and
// two consecutive rounds of fixes each landed in a different two of the three,
// leaving the untouched copy with the old overwrite behaviour. One definition
// means a fix cannot land in only some of the writers.
void AppendHeaderToken(std::map<std::string, std::string> &headers, const char *name, const char *token)
{
	auto it = headers.find(name);
	if (it == headers.end() || it->second.empty()) {
		headers[name] = token;
		return;
	}
	// Token-by-token so a short name cannot match inside a longer one.
	const std::string &cur = it->second;
	std::size_t pos = 0;
	while (pos <= cur.size()) {
		const std::size_t comma = cur.find(',', pos);
		const std::size_t end = (comma == std::string::npos) ? cur.size() : comma;
		std::size_t b = pos, e = end;
		while (b < e && (cur[b] == ' ' || cur[b] == '\t'))
			++b;
		while (e > b && (cur[e - 1] == ' ' || cur[e - 1] == '\t'))
			--e;
		if (cur.compare(b, e - b, token) == 0)
			return;
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
	it->second = cur + ", " + token;
}

// Bodies smaller than this are sent uncompressed. Below ~250 bytes the
// gzip header (~10 bytes) + trailer (~8 bytes) plus the deflate block
// framing eats most of any ratio gain, and error/heartbeat payloads
// are common in that size range.
constexpr size_t kGzipMinBodyBytes = 256;

// Media types whose payload is already entropy-coded. Running deflate
// over a PNG / JPEG / WebP / GIF / woff2 buys a percent at best and
// routinely grows the body, so the flag artwork and any future binary
// asset ship as-is. Prefix match on the type token — the check runs
// before any "; charset=" parameter would matter, and none of these
// carry one.
bool IsPrecompressedType(const std::string &content_type)
{
	static const char *const kTypes[] = { "image/png",
		"image/jpeg",
		"image/gif",
		"image/webp",
		"font/woff",
		"font/woff2",
		"application/zip",
		"application/gzip" };
	for (const char *type : kTypes) {
		if (content_type.compare(0, std::strlen(type), type) == 0) {
			return true;
		}
	}
	return false;
}

// See HttpServer.h.
bool WillCompressBody(
	bool accepts_gzip, std::size_t body_size, const std::string &content_type, bool already_encoded)
{
	return accepts_gzip && body_size >= kGzipMinBodyBytes && !already_encoded &&
	       !IsPrecompressedType(content_type);
}

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

// Size of the handler worker pool. Non-streaming request handlers run here
// instead of on the single io_context thread, so a handler that blocks — a
// synchronous EC roundtrip stalled up to the EC read timeout — can never
// freeze accept or other connections. Sized with headroom over amuleapi's
// typical concurrency (a handful of clients) so non-EC requests keep a free
// worker even while several handlers are parked on a stalled EC roundtrip.
constexpr int kHandlerPoolThreads = 16;
std::atomic<int> g_streaming_session_count{ 0 };

// Process-wide cap on concurrent file-backed responses (Response::file), which
// is a budget of its own -- independent of both the 16 handler-pool threads and
// the 32 SSE slots, because it protects a different resource from either.
//
// Unlike the SSE path this costs no OS thread: the body is written by
// async_write on the io_context, so a file response in flight holds a socket
// and a 64 KiB buffer and nothing else. That is the whole reason serving
// multi-GB content out of amuleapi is affordable at all, and it is also why the
// number here is NOT about threads. What it is about is that these responses
// read from the same spindles the hasher and the ed2k uploader are already
// using, and push bytes down the same NIC; six concurrent whole-file reads is
// roughly where a single mechanical disk stops doing sequential I/O and starts
// seeking between streams. Small enough to keep aMule's own transfers usable,
// large enough for a browser (which opens a handful of parallel range requests
// per media element) plus a second client.
//
// Over the cap the transport answers 503 + Retry-After, exactly as the SSE
// dispatch refuses a 33rd subscriber.
//
// Six is the right DEFAULT and the wrong ceiling. The reasoning above is a
// single-spindle one, and it is also a per-address one that a reverse proxy
// erases: behind nginx every client arrives from one remote_addr, so this is a
// global budget with no per-user fairness in it. A NAS with an SSD and four
// household devices wants more; a Pi serving off the same USB disk it downloads
// to may want less. So the number is `[Streaming]/MaxConcurrentFileResponses`
// in amuleapi.conf, defaulting here.
constexpr int kDefaultMaxConcurrentFileResponses = 6;
// Written once by SetMaxConcurrentFileResponses before Start() binds the
// listener, and only read afterwards. Atomic rather than plain int because the
// reader is the io_context thread while the writer is whatever thread
// configured the server; the ordering is established by the listener not
// existing yet, but the object still has to be race-free to be well-defined.
std::atomic<int> g_max_concurrent_file_responses{ kDefaultMaxConcurrentFileResponses };
std::atomic<int> g_file_response_count{ 0 };

// Largest request header block we accept. The read buffer below is sized
// from this: the whole header has to sit in the buffer at once, so a buffer
// smaller than the parser's header_limit means the buffer overflows first
// and the limit never fires. They were 8 KiB and 16 KiB respectively, which
// made the documented 16 KiB cap unreachable and the header_limit branch
// dead code.
constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
// Slack for the request line and the parser's own bookkeeping.
constexpr std::size_t kReadBufferBytes = kMaxHeaderBytes + 2 * 1024;

// Ceiling on how much of a rejected upload we read and throw away before
// closing. Enough to clear the in-flight window for a normal client that
// wrote its body before reading our answer, without letting a peer that
// keeps pumping hold the session open.
constexpr std::size_t kMaxDrainBytes = 4 * 1024 * 1024;

// Idle deadline on a file response: the longest a file transfer may go without
// the socket accepting a single further byte before the transport gives up and
// closes, releasing the concurrency slot the response holds.
//
// A PROGRESS deadline, not a total one. The 20 s request expiry cannot be left
// armed across a file body -- a legitimate multi-GB download over a slow link
// runs for hours -- but leaving the stream on expires_never() means a peer that
// advertises a zero receive window and then stops reading pins its slot until
// the process restarts: TCP does not time that out on its own, it just keeps
// probing with the persist timer, and neither TCP_USER_TIMEOUT nor keepalive is
// set on these sockets. Six such peers -- a phone that slept mid-download will
// do it, no malice required -- would permanently 503 the endpoint. Re-arming
// per write attempt bounds the stalled peer while a slow one, which by
// definition keeps making progress, resets the clock every time it does.
//
// 120 s is deliberately far past any real link. Each async_write_some completes
// as soon as the socket accepts ANY bytes of the pending chunk, so exceeding
// this means the peer's window stayed shut for two full minutes; even the
// pessimistic reading, a whole 64 KiB chunk taking longer than this, works out
// to under 4.5 kbit/s of goodput, well below dial-up. It is a stall detector,
// not a rate limit.
constexpr std::chrono::seconds kFileWriteIdleTimeout{ 120 };

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
		std::shared_ptr<asio::thread_pool> handler_pool,
		CHttpServer::Handler handler,
		CHttpServer::StreamingResolver streaming_resolver,
		CHttpServer::StreamingHandler streaming_handler,
		CHttpServer::StreamingPreflight streaming_preflight,
		CHttpServer::CorsStamper cors_stamper)
	: m_stream(std::move(socket))
	, m_request_timer(m_stream.get_executor())
	, m_handler_pool(std::move(handler_pool))
	, m_handler(std::move(handler))
	, m_streaming_resolver(std::move(streaming_resolver))
	, m_streaming_handler(std::move(streaming_handler))
	, m_streaming_preflight(std::move(streaming_preflight))
	, m_cors_stamper(std::move(cors_stamper))
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
		// Same accounting for the file-response budget, and released from
		// the same place for the same reason: the destructor is the ONE
		// point every exit path funnels through. A file write can end by
		// completing, by erroring mid-body, by the peer vanishing, or by
		// Listener::Stop closing the socket underneath it -- releasing the
		// slot in the write completion handler would cover the first of
		// those and leak on the rest, and a leaked slot here is permanent,
		// since nothing ever resets the counter.
		if (m_file_slot_held) {
			g_file_response_count.fetch_sub(1, std::memory_order_acq_rel);
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
		// Per-request, like the parser. m_answered decides which of the
		// timeout timer and the read handler gets to answer; the rest
		// describe how THIS request must be written and drained. All of
		// them are latched today only because a connection serves one
		// request -- the moment the keep-alive path above is turned on,
		// a stale m_answered drops every later request, a stale
		// m_drain_before_close makes an ordinary response drain, a
		// stale m_head_probe_chunked mislabels its framing, and a
		// stale m_drained shortens the next drain budget. Reset them
		// together so the set cannot drift apart again.
		m_answered.store(false, std::memory_order_relaxed);
		m_head_only = false;
		m_head_probe_chunked = false;
		m_drain_before_close = false;
		m_drained = 0;
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
		m_parser->header_limit(kMaxHeaderBytes);
		// 10 s read budget. amuleapi runs against localhost/LAN; a
		// real client never takes 10 s to send a 1 KiB request.
		//
		// Two timers, deliberately. beast::tcp_stream's own expiry
		// CLOSES the socket, so by the time the read handler sees
		// beast::error::timeout there is nothing left to answer on --
		// which is why a stalled request used to look identical to a
		// crashed daemon. Our own timer fires first and gets to send a
		// 408; the stream's expiry stays as the hard backstop for the
		// case where writing that 408 also stalls.
		m_stream.expires_after(std::chrono::seconds(20));
		m_request_timer.expires_after(std::chrono::seconds(10));
		{
			auto self = shared_from_this();
			m_request_timer.async_wait([self](const beast::error_code &tec) {
				// operation_aborted = the read completed and cancelled
				// us, which is the normal path.
				if (tec == asio::error::operation_aborted)
					return;
				if (self->m_answered.exchange(true))
					return;
				// Same method recovery as the limit paths: this
				// path never reaches Dispatch(), where
				// m_head_only is normally set, so without it a
				// HEAD drip-feeding its headers gets the 408
				// envelope as content.
				if (self->m_parser->get().method() == http::verb::head) {
					self->m_head_only = true;
				}
				// No drain here: http::async_read is still
				// outstanding on this socket, and a second
				// read alongside it is an overlapping read.
				// Retire it instead -- writing the envelope
				// does not, and a peer that ignores the FIN
				// would otherwise pin this Session and its fd
				// until the stream backstop, which is twice
				// the budget the single timer used to enforce.
				{
					beast::error_code cancel_ec;
					self->m_stream.socket().cancel(cancel_ec);
				}
				self->WriteAndClose(408,
					"request_timeout",
					"the request was not completed within 10 s",
					/*drain=*/false);
			});
		}

		auto self = shared_from_this();
		http::async_read(
			m_stream, m_buffer, *m_parser, [self](beast::error_code ec, std::size_t bytes) {
				(void)bytes;
				self->m_request_timer.cancel();
				// The timeout timer got there first and has already
				// written a 408; anything we do now would be a second
				// response on the same connection.
				if (self->m_answered.exchange(true))
					return;
				if (ec == http::error::end_of_stream) {
					self->DoClose();
					return;
				}
				if (ec) {
					// Recover the method from whatever the parser
					// managed to read before it gave up. Dispatch()
					// is where m_head_only is normally set and these
					// paths never reach it, so without this a HEAD
					// rejected at the body or header cap answers with
					// the error envelope as content -- the exact
					// violation this commit removes everywhere else.
					if (self->m_parser->get().method() == http::verb::head) {
						self->m_head_only = true;
					}
					// Three of these are limits WE imposed, and a
					// caller cannot tell a silent close from "daemon
					// crashed" or "firewall ate it" — every other
					// rejection on this surface is a typed JSON
					// envelope, so answer these the same way before
					// closing. Any other read error (peer vanished,
					// framing garbage) stays quiet: there is nobody
					// left to tell, and health-check probes would
					// otherwise cost a log line per connection.
					if (ec == http::error::body_limit) {
						self->WriteAndClose(413,
							"payload_too_large",
							"request body exceeds the 1 MiB limit",
							/*drain=*/true);
						return;
					}
					// buffer_overflow is the same condition seen
					// from the buffer's side: whichever of the two
					// binds first, the caller sent more header than
					// we accept.
					if (ec == http::error::header_limit ||
						ec == http::error::buffer_overflow) {
						self->WriteAndClose(431,
							"headers_too_large",
							"request headers exceed the 16 KiB limit",
							/*drain=*/true);
						return;
					}
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
		// HEAD is answered with the GET headers and no content. Recorded
		// here because WriteResponse runs after the parser has moved on.
		m_head_only = (r.method == "HEAD");
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

		// Run the handler on the worker pool, NOT the io_context thread, so
		// a handler that blocks (a synchronous EC roundtrip stalled up to
		// the EC read timeout) can never freeze accept or other sessions.
		// Disarm the read timeout first: the handler may legitimately take
		// up to the EC read-timeout budget, and the 10 s read timer would
		// otherwise close the socket out from under it. The response is
		// posted back to the session strand (m_stream's executor) so all
		// socket I/O stays on the io_context thread.
		m_stream.expires_never();
		auto self = shared_from_this();
		auto reqp = std::make_shared<CHttpServer::Request>(std::move(r));
		boost::asio::post(m_handler_pool->get_executor(), [self, reqp]() {
			CHttpServer::Response resp;
			try {
				resp = self->m_handler(*reqp);
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
				resp.body = "{\"error\":{\"code\":\"internal_error\","
					    "\"message\":\"internal server error\"}}";
				// The dispatcher's CORS pass died with the handler,
				// so stamp it here: a cross-origin client should be
				// able to read a 500 for the same reason it can read
				// a 4xx.
				if (self->m_cors_stamper) {
					self->m_cors_stamper(
						resp.headers, self->FindHeaderCaseInsensitiveRaw("Origin"));
				}
			}
			auto out = std::make_shared<CHttpServer::Response>(std::move(resp));
			boost::asio::post(self->m_stream.get_executor(),
				[self, out]() { self->WriteResponse(std::move(*out)); });
		});
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
		// Transport-built, so the dispatcher never sees it. Without the
		// stamp this is one more reply a cross-origin client cannot
		// read -- the same defect just fixed for 408/413/431.
		if (m_cors_stamper) {
			m_cors_stamper(refused.headers, FindHeaderCaseInsensitiveRaw("Origin"));
		}
		refused.body = "{\"error\":{\"code\":\"too_many_streams\","
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
		// Not keep-alive. DoClose() writes the chunked terminator and
		// then shuts the socket down, so a client that reads the stream
		// to its clean end and returns the socket to a pool fails its
		// next request on it -- the same reason every ordinary response
		// says close. It also kept the HEAD probe, which does say
		// close, contradicting the GET it stands for.
		head->headers["Connection"] = "close";
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
				} else {
					m_wants_gzip = false;
				}
			}
			// Outside the gzip branch on purpose. Vary describes what
			// the RESPONSE VARIES ON, not what this particular response
			// was encoded as, so it belongs on the identity reply too --
			// otherwise a cache keyed on the non-gzip answer serves it
			// to a client that asked for gzip. WriteResponse adds it
			// unconditionally, so leaving it inside the branch also put
			// a plain SSE GET at odds with the HEAD probe for the same
			// URL, which goes through that writer.
			AppendHeaderToken(m_head->headers, "Vary", "Accept-Encoding");
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

	// Typed error straight from the transport layer, for the read-side
	// limits that never reach a handler. Hand-built rather than routed
	// through the dispatcher's ErrorResponse: at this point the request
	// was never parsed, so there is no route and no auth context.
	// Origin off the parser rather than a parsed Request: on these paths
	// the request never got far enough to build one. Returns "" when the
	// header block never arrived, which the stamper treats as no-origin.
	std::string FindHeaderCaseInsensitiveRaw(const char *name) const
	{
		if (!m_parser) {
			return std::string();
		}
		const auto &msg = m_parser->get();
		for (const auto &h : msg) {
			if (h.name_string().size() == std::strlen(name) &&
				strncasecmp(std::string(h.name_string()).c_str(), name, std::strlen(name)) ==
					0) {
				return std::string(h.value());
			}
		}
		return std::string();
	}

	void WriteAndClose(unsigned status, const char *code, const char *message, bool drain)
	{
		CHttpServer::Response r;
		r.status = status;
		r.content_type = "application/json";
		r.body = std::string("{\"error\":{\"code\":\"") + code + "\",\"message\":\"" + message +
			 "\"}}";
		// `drain` only where the composed read has already completed.
		// The size limits fire while the peer may still be writing (a
		// client that sends its whole body before reading anything is
		// the normal case for a POST), and closing with unread data
		// queued lets the stack emit an RST that discards the response
		// just written -- the caller then sees a connection reset
		// instead of the 413 explaining it. The timeout path is the
		// opposite case: http::async_read is still outstanding there,
		// and a second read on the same socket is an overlapping read,
		// which asio forbids.
		// The only replies on the surface built without a parsed request,
		// so the dispatcher's CORS pass never sees them. Stamp here or a
		// cross-origin browser client gets an opaque failure instead of
		// the typed envelope the docs promise.
		if (m_cors_stamper) {
			m_cors_stamper(r.headers, FindHeaderCaseInsensitiveRaw("Origin"));
		}
		m_drain_before_close = drain;
		// Tell the peer not to reuse this connection. Without it a
		// pooling client can read the error, keep the socket, and
		// leave us parked in the drain read with nothing to collect.
		r.headers["Connection"] = "close";
		WriteResponse(std::move(r));
	}

	// Read and discard whatever the peer is still sending, then close.
	// Bounded three ways: the stream's expiry (which is why this reads
	// through the stream rather than the socket), a byte cap, and the
	// `Connection: close` the error carries.
	// Arms the drain deadline and starts the loop. The deadline is set
	// ONCE, here: expires_after inside the loop would re-arm on every
	// read and bound only the gap between them, so a peer trickling a
	// byte just under the limit apart could hold the Session and its fd
	// until the byte cap -- effectively forever.
	void StartDrainThenClose()
	{
		// Short budget of its own, not the request budget. The point of
		// the drain is to collect what is ALREADY in flight so the
		// close does not RST away the error we just wrote -- not to
		// wait for a peer that has finished talking. A client that sent
		// an oversized header has said everything it intends to and is
		// now waiting on us, so draining to EOF would have both sides
		// waiting until the request expiry broke the tie, which reads
		// to the caller as a hang rather than a 431.
		m_stream.expires_after(std::chrono::seconds(1));
		DrainThenClose();
	}

	void DrainThenClose()
	{
		auto self = shared_from_this();
		// Through the beast stream, not the raw socket: the raw socket
		// ignores the stream's expiry, so a peer that neither sends nor
		// closes would park this read forever and leak the Session with
		// its fd.
		m_stream.async_read_some(
			m_drain_buffer.prepare(4096), [self](beast::error_code ec, std::size_t n) {
				self->m_drained += n;
				if (ec || n == 0 || self->m_drained > kMaxDrainBytes) {
					self->DoClose();
					return;
				}
				// Loop, do NOT re-arm: the deadline set in
				// StartDrainThenClose is the total budget, and
				// re-arming here would reset it on every read.
				self->DrainThenClose();
			});
	}

	// Transport-built error reply for the file path. Kept separate from the
	// handler's own error shapes because these are written after the handler
	// has already returned a perfectly good response, and the body must still
	// match the error contract the rest of the surface uses.
	//
	// The CORS stamp matters for the same reason it does on 408/413/431: the
	// dispatcher's pass never ran on a reply the transport invented, and
	// without it these are the only answers on the surface a cross-origin
	// browser client sees as an opaque failure instead of a typed envelope.
	void WriteFileTransportError(unsigned status, const char *code, const char *message, bool retry_after)
	{
		CHttpServer::Response err;
		err.status = status;
		err.content_type = "application/json";
		if (retry_after) {
			err.headers["Retry-After"] = "10";
		}
		if (m_cors_stamper) {
			m_cors_stamper(err.headers, FindHeaderCaseInsensitiveRaw("Origin"));
		}
		err.body = std::string("{\"error\":{\"code\":\"") + code + "\",\"message\":\"" + message +
			   "\"}}";
		// `err.file` is unset, so this lands on the ordinary buffered path
		// and cannot recurse back into here.
		WriteResponse(std::move(err));
	}

	// File-backed response: serialise a bounded byte window straight off disk
	// instead of out of resp.body. Split from WriteResponse rather than folded
	// into it because almost none of that function applies -- there is no body
	// in memory to deflate, nothing to reconcile an ETag against, and the
	// message type differs, which decides the serializer at compile time.
	//
	// Everything else about the reply is deliberately identical to the normal
	// path: same Server header, same Connection: close default, same handler
	// header pass, same Vary, same close-after-write.
	void WriteFileResponse(CHttpServer::Response &&resp)
	{
		// A HEAD is exempt from the concurrency budget. The cap exists to
		// bound concurrent BYTES off the disk and onto the NIC, and a HEAD
		// moves none: the serializer runs in split mode, so writer::get is
		// never called and the file is opened and seeked but never read.
		// Spending a download slot on a metadata probe would let a burst of
		// HEADs lock out the transfers the cap is there to protect.
		if (!m_head_only) {
			// Claim the slot before opening anything. fetch_add returns
			// the OLD value, so the slot is ours iff that value was
			// strictly below the cap; otherwise roll back and refuse.
			const int prior = g_file_response_count.fetch_add(1, std::memory_order_acq_rel);
			if (prior >= g_max_concurrent_file_responses.load(std::memory_order_relaxed)) {
				g_file_response_count.fetch_sub(1, std::memory_order_acq_rel);
				WriteFileTransportError(503,
					"file_responses_exhausted",
					"too many concurrent file transfers; retry in a few seconds",
					true);
				return;
			}
			// From here on the destructor owns the release. Set the flag
			// BEFORE anything that can fail, so an early return below
			// still gives the slot back.
			m_file_slot_held = true;
		}

		m_file_response.emplace();
		auto &out = *m_file_response;

		// The handler already resolved and containment-checked the path; the
		// transport opens it blindly. Both failures below are 500s and not
		// something more specific on purpose -- a 404 here would mean the
		// handler said yes to a path that then vanished, which is a race or a
		// bug, not a client error. They are still safe to answer, because
		// nothing has been written to the socket yet: the headers are
		// committed at the first async_write below, not before.
		beast::error_code ec;
		out.body().Open(resp.file->fs_path.c_str(), ec);
		if (!ec) {
			// Rejects, rather than clamps, a window that does not fit the
			// file. The handler has usually already put a Content-Range on
			// the response describing exactly this window, and shipping a
			// different one would make the reply contradict itself.
			out.body().SetWindow(resp.file->first, resp.file->last, ec);
		}
		if (ec) {
			m_file_response.reset();
			WriteFileTransportError(
				500, "internal_error", "could not read the requested file", false);
			return;
		}

		// Emitted even though a file response never varies by encoding: the
		// same URL can legitimately answer from the buffered path too (a 304,
		// a 416, an error envelope), and those DO vary. An intermediary that
		// cached this one without the dimension would then serve it in place
		// of a form that differs.
		AppendHeaderToken(resp.headers, "Vary", "Accept-Encoding");

		out.version(11);
		out.result(resp.status);
		out.set(http::field::server, "amuleapi");
		// Same one-request-per-connection contract as everywhere else;
		// DoClose() shuts the socket down after this write.
		if (resp.headers.find("Connection") == resp.headers.end()) {
			out.set(http::field::connection, "close");
		}
		// Same opt-out the SSE head sets, for the mirror-image reason. nginx
		// buffers proxied responses by default and spools anything larger
		// than `proxy_buffers` to disk, up to `proxy_max_temp_file_size` --
		// 1 GB out of the box. On this path that means the proxy writes a
		// second copy of the file to its own disk before the client sees the
		// first byte, which both delays the response and moves the I/O cost
		// this transport was built to avoid onto the proxy host. Set here
		// rather than by the handler because it is a property of streaming a
		// large body off disk, not of one route: any future file-serving
		// endpoint inherits it. Written before the handler's own header pass
		// below, so a handler that has a reason to buffer can still override.
		out.set("X-Accel-Buffering", "no");
		if (!resp.content_type.empty()) {
			out.set(http::field::content_type, resp.content_type);
		}
		for (const auto &h : resp.headers) {
			out.set(h.first, h.second);
		}
		// Content-Length comes from RangeFileBody::size, i.e. the WINDOW
		// length rather than the file length or the bytes-to-EOF that stock
		// file_body would have reported. This is the number a HEAD has to
		// answer with as well, and it gets it from the same call.
		out.prepare_payload();

		// The read timeout was disarmed in Dispatch (m_stream.expires_never()
		// before the handler ran), and the body is NOT written back under it:
		// a legitimate multi-GB download over a slow link takes far longer
		// than the 20 s stream expiry, so a total deadline would kill exactly
		// the transfers this endpoint exists for.
		//
		// What bounds a stall instead is kFileWriteIdleTimeout, re-armed by
		// WriteFileChunk before every write attempt -- which is why the body
		// goes out as a WriteFileChunk loop over async_write_some rather than
		// as one async_write: a single async_write offers no per-chunk hook to
		// re-arm from. See the constant for why a progress deadline is
		// required here and why TCP does not supply one.
		auto self = shared_from_this();
		m_file_serializer.emplace(out);
		if (m_head_only) {
			// RFC 9110 9.3.2 again: headers only, on any status. split(true)
			// stops the serializer after the header, so writer::get never
			// runs and not one byte of the file is read -- while
			// Content-Length still reports what the GET would have sent,
			// which is what 40-http-conformance.sh asserts.
			m_file_serializer->split(true);
			// Same deadline on the header-only write. It is a few hundred
			// bytes, but a peer that opens a connection, sends HEAD and
			// then refuses to read holds a file-response slot just as
			// firmly as one that stalls a GET.
			m_stream.expires_after(kFileWriteIdleTimeout);
			http::async_write_header(
				m_stream, *m_file_serializer, [self](beast::error_code ec, std::size_t) {
					(void)ec;
					if (self->m_drain_before_close) {
						self->StartDrainThenClose();
						return;
					}
					self->DoClose();
				});
			return;
		}
		WriteFileChunk();
	}

	// One turn of the file-body write loop: re-arm the progress deadline, hand
	// the serializer's next buffer to the socket, and come back for the rest.
	//
	// This is what `http::async_write` does internally, unrolled so the
	// deadline can be re-armed between turns. async_write_some completes as
	// soon as the socket accepts any bytes at all, so each turn is a real
	// observation that the peer is still draining its receive window, and the
	// deadline measures the gap between two such observations -- the definition
	// of a progress deadline. A peer that stops reading never completes a turn,
	// its expiry fires, beast::tcp_stream closes the socket underneath the
	// pending write, the handler runs with beast::error::timeout, and the slot
	// goes back with ~Session.
	void WriteFileChunk()
	{
		auto self = shared_from_this();
		m_stream.expires_after(kFileWriteIdleTimeout);
		http::async_write_some(
			m_stream, *m_file_serializer, [self](beast::error_code ec, std::size_t) {
				// `ec` discarded as on the buffered path: a peer that
				// walked away mid-download is the normal end of a media
				// request, not something to log -- and so is the timeout
				// above, which is the same event with a slower peer. The
				// slot this response holds is released by ~Session when
				// `self` drops at the end of the chain.
				if (!ec && !self->m_file_serializer->is_done()) {
					self->WriteFileChunk();
					return;
				}
				if (self->m_drain_before_close) {
					self->StartDrainThenClose();
					return;
				}
				self->DoClose();
			});
	}

	void WriteResponse(CHttpServer::Response &&resp)
	{
		// A file-backed response shares none of the machinery below: it must
		// not be gzipped (deflating a byte range breaks the accounting its
		// Content-Range describes, and shared content is normally already
		// entropy-coded), and it has no in-memory body for the ETag
		// reconciliation to measure. Branch before any of it runs, so the
		// deflate path below cannot be reached with a body that is not there.
		if (resp.file) {
			WriteFileResponse(std::move(resp));
			return;
		}

		// Regular (non-streaming) response gzip encoding. Gated by:
		//  * client Accept-Encoding contains gzip,
		//  * body is at or above kGzipMinBodyBytes -- the bound is
		//    inclusive; header overhead is a significant fraction of
		//    anything smaller,
		//  * handler didn't already set Content-Encoding (a future
		//    pre-gzipped static asset path would use that hook),
		//  * body isn't already entropy-coded (PNG flags, images and
		//    fonts out of the static tree).
		// deflate() fallback: on any zlib error we ship the original
		// uncompressed body rather than 500 — better degraded than
		// broken. Content-Length is set correctly by
		// prepare_payload() based on the post-swap body.
		//
		// Vary: Accept-Encoding is added regardless of whether *this*
		// response was compressed, so any intermediary cache keys the
		// entry correctly across clients that do / don't send the
		// header.
		// HEAD is compressed too, even though the bytes are then
		// discarded. Skipping it saves a deflate per probe and costs
		// correctness: HEAD must describe what the equivalent GET would
		// return, so with Accept-Encoding: gzip that means the SAME
		// Content-Encoding and the SAME Content-Length. Diverging binds
		// one strong ETag to two codings and, per RFC 9111 4.3.5, lets
		// each probe invalidate the stored gzip response. The deflate
		// is the cheaper of the two.
		if (WillCompressBody(m_accepts_gzip,
			    resp.body.size(),
			    resp.content_type,
			    resp.headers.find("Content-Encoding") != resp.headers.end())) {
			std::string compressed;
			const bool gzipped = GzipOnce(resp.body, compressed);
			if (gzipped) {
				resp.body = std::move(compressed);
				resp.headers["Content-Encoding"] = "gzip";
			}
			// Reconcile the validator with the coding that ACTUALLY
			// shipped. The hash upstream is taken before compression,
			// so without a marker both codings of a resource carry the
			// same strong ETag; the dispatcher stamps it in advance
			// because a 304 has no body left here to measure. When
			// that prediction holds this is a no-op. When deflate
			// fails the body ships as identity and the prediction is
			// now wrong, and this takes the suffix back off -- leaving
			// it would bind a gzip validator to identity bytes, which
			// is the exact mispairing the suffix exists to prevent.
			auto et = resp.headers.find("ETag");
			if (et != resp.headers.end())
				et->second = webcommon::WithCodingSuffix(et->second, gzipped);
		}
		// Append, never overwrite: the dispatcher may already have set
		// `Vary: Origin` for CORS, and replacing it would drop the
		// encoding dimension from a response whose ETag was computed
		// before compression.
		AppendHeaderToken(resp.headers, "Vary", "Accept-Encoding");

		m_response.emplace();
		m_response->version(11);
		m_response->result(resp.status);
		m_response->set(http::field::server, "amuleapi");
		// One request per connection: DoClose() shuts the socket down
		// after every response, so HTTP/1.1's default persistence is a
		// promise this server does not keep. Say so on every reply
		// instead of on the handful that happened to set it, or a
		// pooling client keeps the socket and fails its next request on
		// it. Handlers that set it themselves are left alone.
		if (resp.headers.find("Connection") == resp.headers.end()) {
			m_response->set(http::field::connection, "close");
		}
		// A Content-Type whose value is not a media type is malformed, so
		// omit the header entirely on the bodiless replies (204, 304)
		// whose handlers deliberately cleared it.
		if (!resp.content_type.empty()) {
			m_response->set(http::field::content_type, resp.content_type);
		}
		for (const auto &h : resp.headers) {
			m_response->set(h.first, h.second);
		}
		// The /events HEAD probe is the one response that stands in for
		// a chunked stream; the dispatcher marks it by content type.
		if (m_head_only && resp.content_type == "text/event-stream") {
			m_head_probe_chunked = true;
		}
		m_response->body() = std::move(resp.body);
		m_response->prepare_payload();
		if (m_head_probe_chunked) {
			// Mirror the framing a GET on this endpoint advertises.
			// prepare_payload() sizes from the (empty) body and stamps
			// `Content-Length: 0`, which would describe a zero-length
			// document rather than the unbounded stream the caller
			// asked about; chunked() ahead of it does not survive,
			// since the body size is known. Set the framing directly,
			// after. Only the header is written, so nothing has to
			// honour it.
			m_response->erase(http::field::content_length);
			m_response->set(http::field::transfer_encoding, "chunked");
		}

		auto self = shared_from_this();
		if (m_head_only) {
			// RFC 9110 §9.3.2 — a HEAD response carries no content, on
			// any status. prepare_payload() has already sized
			// Content-Length from the full body, so serializing the
			// header alone reports what a GET would return without
			// putting a byte of it on the wire.
			m_serializer.emplace(*m_response);
			m_serializer->split(true);
			http::async_write_header(
				m_stream, *m_serializer, [self](beast::error_code ec, std::size_t) {
					(void)ec;
					// Same drain as the body path. A HEAD rejected at
					// the header cap has just as much unread data
					// queued as a GET does, and closing on top of it
					// lets the RST discard the 431 that was just
					// written -- intermittently, which is why a single
					// probe can pass and hide it.
					if (self->m_drain_before_close) {
						self->StartDrainThenClose();
						return;
					}
					self->DoClose();
				});
			return;
		}
		http::async_write(m_stream, *m_response, [self](beast::error_code ec, std::size_t) {
			(void)ec;
			if (self->m_drain_before_close) {
				self->StartDrainThenClose();
				return;
			}
			self->DoClose();
		});
	}

	void DoClose()
	{
		// Drop the request timer first. It holds a shared_ptr to this
		// Session, so leaving it armed keeps a closed connection alive
		// for the rest of its 10 s and delays process teardown by the
		// same amount. Posted rather than cancelled inline: DoClose also
		// runs on the SSE worker thread, and the timer belongs to the
		// io_context executor -- every other foreign-thread touch in
		// this file goes through asio::post for the same reason.
		{
			auto self = shared_from_this();
			asio::post(m_stream.get_executor(), [self]() { self->m_request_timer.cancel(); });
		}
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
	// Fires before the stream's own expiry so a stalled request can be
	// answered instead of silently dropped; see DoRead.
	asio::steady_timer m_request_timer;
	// Worker pool that runs the (non-streaming) request handler off the
	// io_context thread. Shared with the server; declared before m_handler
	// so the init list stays in declaration order.
	std::shared_ptr<asio::thread_pool> m_handler_pool;
	beast::flat_buffer m_buffer{ kReadBufferBytes };
	boost::optional<http::request_parser<http::string_body>> m_parser;
	boost::optional<http::response<http::string_body>> m_response;
	// Header-only serializer, used for HEAD so Content-Length still
	// reports the GET size while no content reaches the wire.
	boost::optional<http::response_serializer<http::string_body>> m_serializer;
	// The file-backed alternative to the pair above, engaged only when the
	// handler set Response::file. Two separate pairs rather than a variant
	// body because the message type is baked into the serializer at compile
	// time; only one of the two is ever engaged on a given connection.
	//
	// Both optionals reserve their storage INLINE in every Session whether or
	// not they are ever engaged, and response_serializer holds its `writer` by
	// value -- so while the writer kept its 64 KiB read buffer as a member
	// array, every session on the process paid for it, an hours-long SSE
	// connection included. sizeof(Session) was 67432 bytes; the writer now
	// heap-allocates that buffer on first get() (RangeFileBody.h) and it is
	// 1904. Anything added to RangeFileBody::writer lands here again, by value,
	// on every connection.
	boost::optional<http::response<RangeFileBody>> m_file_response;
	boost::optional<http::response_serializer<RangeFileBody>> m_file_serializer;
	// Whether this session is accounted against g_file_response_count; see
	// the destructor.
	bool m_file_slot_held = false;
	CHttpServer::Handler m_handler;

	// streaming state.
	CHttpServer::StreamingResolver m_streaming_resolver;
	CHttpServer::StreamingHandler m_streaming_handler;
	CHttpServer::StreamingPreflight m_streaming_preflight;
	CHttpServer::CorsStamper m_cors_stamper;
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
	bool m_head_only = false;
	// The /events HEAD probe answers for a chunked stream, so it is framed
	// as one rather than as a zero-length body; see DispatchStreaming.
	bool m_head_probe_chunked = false;
	// Set on the transport-level error replies, which are written while the
	// peer may still be uploading; see WriteAndClose.
	bool m_drain_before_close = false;
	std::size_t m_drained = 0;
	beast::flat_buffer m_drain_buffer;
	// One response per connection, whichever of the two paths wins.
	std::atomic<bool> m_answered{ false };
};

// Accept loop. One Listener per HttpServer; spawns a Session per
// connection via shared_from_this.
class Listener : public std::enable_shared_from_this<Listener>
{
public:
	Listener(asio::io_context &ioc,
		std::shared_ptr<asio::thread_pool> handler_pool,
		tcp::endpoint endpoint,
		CHttpServer::Handler handler,
		CHttpServer::StreamingResolver streaming_resolver,
		CHttpServer::StreamingHandler streaming_handler,
		CHttpServer::StreamingPreflight streaming_preflight,
		CHttpServer::CorsStamper cors_stamper)
	: m_ioc(ioc)
	, m_acceptor(asio::make_strand(ioc))
	, m_handler_pool(std::move(handler_pool))
	, m_handler(std::move(handler))
	, m_streaming_resolver(std::move(streaming_resolver))
	, m_streaming_handler(std::move(streaming_handler))
	, m_streaming_preflight(std::move(streaming_preflight))
	, m_cors_stamper(std::move(cors_stamper))
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
						self->m_handler_pool,
						self->m_handler,
						self->m_streaming_resolver,
						self->m_streaming_handler,
						self->m_streaming_preflight,
						self->m_cors_stamper)
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
	std::shared_ptr<asio::thread_pool> m_handler_pool;
	CHttpServer::Handler m_handler;
	CHttpServer::StreamingResolver m_streaming_resolver;
	CHttpServer::StreamingHandler m_streaming_handler;
	CHttpServer::StreamingPreflight m_streaming_preflight;
	CHttpServer::CorsStamper m_cors_stamper;
	std::string m_error;
};

} // namespace

struct CHttpServer::Impl
{
	asio::io_context ioc{ 1 };
	// Handlers run here, off the single io_context thread — see
	// kHandlerPoolThreads. shared_ptr so Sessions can keep it alive for an
	// in-flight handler; joined explicitly in Stop().
	std::shared_ptr<asio::thread_pool> handler_pool;
	std::shared_ptr<Listener> listener;
	std::thread thread;
	std::atomic<bool> running{ false };
};

CHttpServer::CHttpServer() = default;

CHttpServer::~CHttpServer()
{
	Stop();
}

// Every callback here is a sink: taken by value, std::move()d into Listener,
// which moves it on into its member. A const reference would force a copy at
// that member instead. performance-unnecessary-value-param flags four of the
// five anyway -- exactly the four carrying a default argument, including the
// two this change never touched -- so the suppression covers the whole list
// rather than singling out the two whose line numbers happened to move.
void CHttpServer::SetMaxConcurrentFileResponses(int max_responses)
{
	// Silently ignoring a bad value rather than clamping it: the config
	// layer has already rejected everything outside the sane band and fallen
	// back to the default, so anything that still gets here is a programming
	// error, and the default is a better answer to that than zero.
	if (max_responses > 0) {
		g_max_concurrent_file_responses.store(max_responses, std::memory_order_relaxed);
	}
}

// NOLINTBEGIN(performance-unnecessary-value-param)
bool CHttpServer::Start(const std::string &bind_address,
	unsigned port,
	Handler handler,
	StreamingResolver streaming_resolver,
	StreamingHandler streaming_handler,
	StreamingPreflight streaming_preflight,
	CorsStamper cors_stamper)
// NOLINTEND(performance-unnecessary-value-param)
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

	m_impl->handler_pool = std::make_shared<asio::thread_pool>(kHandlerPoolThreads);
	m_impl->listener = std::make_shared<Listener>(m_impl->ioc,
		m_impl->handler_pool,
		endpoint,
		std::move(handler),
		std::move(streaming_resolver),
		std::move(streaming_handler),
		std::move(streaming_preflight),
		std::move(cors_stamper));
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
	// Drain the handler pool after the io_context thread is gone. stop()
	// abandons not-yet-started handler tasks; join() lets any in-flight
	// handler finish (bounded by the EC read timeout). Their response
	// trampolines were posted to the now-stopped io_context and are simply
	// discarded when it is destroyed in m_impl.reset() below — each Session
	// stays alive via the captured shared_ptr until then.
	if (m_impl->handler_pool) {
		m_impl->handler_pool->stop();
		m_impl->handler_pool->join();
	}
	m_impl.reset();
}
