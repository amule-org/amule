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

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/optional.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>


namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace asio  = boost::asio;
using tcp       = boost::asio::ip::tcp;


namespace {

// Per-connection session. Reads one request, hands it to the user
// handler, writes the response, closes. No keep-alive — Phase 2's
// surface is too small to benefit, and a one-shot model keeps the
// state machine here trivially auditable.
//
// Phase 8: if the streaming_resolver matches the parsed request, the
// session takes a different path — writes the HTTP response head
// then runs the streaming handler on a worker thread, which can push
// chunks indefinitely via the Writer interface. The session keeps
// the socket open until the handler returns or the peer
// disconnects.
class Session : public std::enable_shared_from_this<Session> {
public:
	Session(tcp::socket socket,
	        CHttpServer::Handler handler,
	        CHttpServer::StreamingResolver streaming_resolver,
	        CHttpServer::StreamingHandler  streaming_handler)
		: m_stream(std::move(socket)),
		  m_handler(std::move(handler)),
		  m_streaming_resolver(std::move(streaming_resolver)),
		  m_streaming_handler(std::move(streaming_handler))
	{}

	~Session()
	{
		// The worker thread captures `shared_from_this()`, so by the
		// time this destructor runs the worker has already returned
		// (last ref dropped). The std::thread handle still needs to
		// be either joined or detached before destruct or the
		// standard library aborts. We can't join() from inside the
		// worker's own call stack (deadlock), and at this point we
		// know the worker has exited; detach() is safe and cleans
		// up the thread handle without blocking.
		if (m_stream_worker.joinable()) m_stream_worker.detach();
	}

	void Start() { DoRead(); }

private:
	void DoRead()
	{
		// Reset the parser each request — without it, calling DoRead a
		// second time on the same stream blocks forever with a partial
		// state. Phase 2 only reads one request, but leaving the reset
		// in keeps the read loop forward-compatible if keep-alive is
		// turned on later.
		m_parser.emplace();
		// 1 MiB request cap — bigger than any sensible REST POST body
		// (login JSON is ~64 bytes, etc.) but well under "someone is
		// trying to exhaust memory by upload-pumping us".
		m_parser->body_limit(1024 * 1024);
		// 10 s read timeout. amuleapi runs against localhost/LAN; a
		// real client never takes 10 s to send a 1 KiB request.
		m_stream.expires_after(std::chrono::seconds(10));

		auto self = shared_from_this();
		http::async_read(m_stream, m_buffer, *m_parser,
			[self](beast::error_code ec, std::size_t bytes) {
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
		r.body   = req.body();
		for (const auto &h : req) {
			r.headers.emplace(std::string(h.name_string()),
			                  std::string(h.value()));
		}
		// Remote endpoint for rate-limiting. `.address()` returns a
		// boost::asio::ip::address which `.to_string()`-es to the
		// canonical IPv4 / IPv6 form ("192.0.2.1", "::1", "fe80::%lo0"...).
		// Wrapped in an error_code overload so a half-closed socket
		// doesn't throw — empty `remote_addr` falls through to "no
		// per-IP bucket" in the rate limiter, which is the safe default.
		{
			beast::error_code ec;
			const auto ep = m_stream.socket().remote_endpoint(ec);
			if (!ec) r.remote_addr = ep.address().to_string();
		}

		// Phase 8: streaming dispatch. The streaming_resolver is
		// invoked synchronously and short-circuits the standard
		// request→response→close path when it returns true.
		if (m_streaming_resolver && m_streaming_handler
		    && m_streaming_resolver(r)) {
			DispatchStreaming(std::move(r));
			return;
		}

		CHttpServer::Response resp;
		try {
			resp = m_handler(r);
		} catch (const std::exception &e) {
			// Handler-internal exceptions become 500s. The body shape
			// matches the rest of the error contract; clients reading
			// `error.code` continue to parse uniformly.
			resp.status       = 500;
			resp.content_type = "application/json";
			resp.body = std::string(
				"{\"error\":{\"code\":\"internal\","
				"\"message\":\"") + e.what() + "\"}}";
		}

		WriteResponse(std::move(resp));
	}


	// Phase 8 streaming path. Writes the response head, then spawns a
	// worker thread that runs the streaming handler. The worker's
	// Writer pushes chunks via a mutex-guarded synchronous write to
	// the socket; "alive" status is observable from any thread via
	// the atomic `m_stream_alive` flag.
	//
	// The worker thread is owned by the Session and joined in the
	// dtor. The Session keeps itself alive across the worker's
	// lifetime by capturing `shared_from_this()` in the worker
	// lambda; when the worker exits, the lambda's destruction
	// releases the last reference and the Session destructs (which
	// joins the thread — a no-op because the worker already exited).
	void DispatchStreaming(CHttpServer::Request r)
	{
		// Disable read timeout — SSE connections are long-lived.
		m_stream.expires_never();
		m_stream_alive.store(true, std::memory_order_release);

		// Spawn the worker thread that runs the streaming handler.
		// The worker captures `self` so the Session stays alive
		// across its run, and references to the head out-params (held
		// on the heap so the SocketWriter can read them at first-
		// write time).
		auto handler = m_streaming_handler;
		auto self    = shared_from_this();
		// Head data — owned by the worker thread, referenced by the
		// SocketWriter (via SocketWriter::HeadData). Defaults set
		// here; handler can overwrite before calling writer.Write
		// the first time.
		auto head = std::make_shared<SocketWriter::HeadData>();
		head->headers["Cache-Control"] = "no-cache";
		head->headers["Connection"]    = "keep-alive";

		auto writer = std::make_shared<SocketWriter>(self, head);

		m_stream_worker = std::thread([self, handler, writer, head,
		                               r = std::move(r)]() mutable {
			try {
				handler(r, *writer,
				        head->status, head->content_type,
				        head->headers);
			} catch (const std::exception &) {
				// Streaming handler exceptions are silent — close
				// quietly.
			}
			// If the handler returned without writing anything (e.g.
			// auth-rejected on a HEAD probe), still emit the head so
			// the client sees the right status code.
			writer->EnsureHeadWritten();
			self->DoClose();
		});
	}

	// Per-streaming-session Writer that marshals writes onto the
	// socket. Defers writing the HTTP response head until the first
	// Write call — that's when the streaming handler has finalised
	// status / content_type / headers via the out-params we pass it.
	class SocketWriter : public CHttpServer::Writer {
	public:
		struct HeadData {
			unsigned    status       = 200;
			std::string content_type = "text/event-stream";
			std::map<std::string, std::string> headers;
		};

		SocketWriter(std::shared_ptr<Session> session,
		             std::shared_ptr<Session::SocketWriter::HeadData> head)
			: m_session(std::move(session)),
			  m_head(std::move(head)) {}

		bool Write(const std::string &chunk) override
		{
			if (!m_session->m_stream_alive.load(std::memory_order_acquire)) {
				return false;
			}
			if (!EnsureHeadWritten()) return false;

			// SSE wire shape uses chunked transfer encoding; each
			// "chunk" written here is a single HTTP/1.1 chunk frame:
			//   <hex-size-of-chunk>\r\n<chunk-bytes>\r\n
			//
			// Zero-length chunks would terminate the message (per
			// RFC 7230 §4.1) so we skip them — the heartbeat path
			// always passes at least ": keepalive\n\n" anyway.
			if (chunk.empty()) return true;
			std::ostringstream framed;
			framed << std::hex << chunk.size() << "\r\n"
			       << chunk << "\r\n";
			const std::string out = framed.str();

			std::lock_guard<std::mutex> g(m_session->m_socket_mu);
			beast::error_code ec;
			asio::write(m_session->m_stream.socket(),
				asio::buffer(out), ec);
			if (ec) {
				m_session->m_stream_alive.store(false,
					std::memory_order_release);
				return false;
			}
			return true;
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
			std::ostringstream head;
			head << "HTTP/1.1 " << m_head->status << " ";
			switch (m_head->status) {
				case 200: head << "OK"; break;
				case 401: head << "Unauthorized"; break;
				case 403: head << "Forbidden"; break;
				case 404: head << "Not Found"; break;
				default:  head << "OK"; break;
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
			const bool chunked = (m_head->status >= 200
			                      && m_head->status < 300);
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
			asio::write(m_session->m_stream.socket(),
				asio::buffer(head_bytes), ec);
			if (ec) {
				m_session->m_stream_alive.store(false,
					std::memory_order_release);
				return false;
			}
			return true;
		}

	private:
		std::shared_ptr<Session> m_session;
		std::shared_ptr<HeadData> m_head;
		std::atomic<bool>        m_head_written{false};
	};

	void WriteResponse(CHttpServer::Response &&resp)
	{
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
		http::async_write(m_stream, *m_response,
			[self](beast::error_code ec, std::size_t) {
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
			asio::write(m_stream.socket(),
				asio::buffer(std::string("0\r\n\r\n")), ec);
		}
		beast::error_code ec;
		m_stream.socket().shutdown(tcp::socket::shutdown_send, ec);
		// `ec` deliberately discarded — peer may have already gone
		// away.
	}

	beast::tcp_stream                                m_stream;
	beast::flat_buffer                               m_buffer{8192};
	boost::optional<http::request_parser<http::string_body>> m_parser;
	boost::optional<http::response<http::string_body>> m_response;
	CHttpServer::Handler                             m_handler;

	// Phase 8 streaming state.
	CHttpServer::StreamingResolver  m_streaming_resolver;
	CHttpServer::StreamingHandler   m_streaming_handler;
	std::atomic<bool>               m_stream_alive{false};
	std::mutex                      m_socket_mu;
	std::thread                     m_stream_worker;
};


// Accept loop. One Listener per HttpServer; spawns a Session per
// connection via shared_from_this.
class Listener : public std::enable_shared_from_this<Listener> {
public:
	Listener(asio::io_context &ioc, tcp::endpoint endpoint,
	         CHttpServer::Handler handler,
	         CHttpServer::StreamingResolver streaming_resolver,
	         CHttpServer::StreamingHandler  streaming_handler)
		: m_ioc(ioc),
		  m_acceptor(asio::make_strand(ioc)),
		  m_handler(std::move(handler)),
		  m_streaming_resolver(std::move(streaming_resolver)),
		  m_streaming_handler(std::move(streaming_handler))
	{
		beast::error_code ec;
		m_acceptor.open(endpoint.protocol(), ec);
		if (ec) { m_error = ec.message(); return; }
		m_acceptor.set_option(asio::socket_base::reuse_address(true), ec);
		if (ec) { m_error = ec.message(); return; }
		m_acceptor.bind(endpoint, ec);
		if (ec) { m_error = ec.message(); return; }
		m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
		if (ec) { m_error = ec.message(); return; }
	}

	bool Ok() const { return m_error.empty(); }
	const std::string &Error() const { return m_error; }

	void Run() { DoAccept(); }
	void Stop()
	{
		beast::error_code ec;
		m_acceptor.close(ec);
	}

private:
	void DoAccept()
	{
		auto self = shared_from_this();
		m_acceptor.async_accept(asio::make_strand(m_ioc),
			[self](beast::error_code ec, tcp::socket socket) {
				if (!ec) {
					std::make_shared<Session>(
						std::move(socket),
						self->m_handler,
						self->m_streaming_resolver,
						self->m_streaming_handler)->Start();
				}
				// Loop unless the acceptor has been closed. operation_aborted
				// fires on Stop() and signals "exit cleanly".
				if (ec != asio::error::operation_aborted) {
					self->DoAccept();
				}
			});
	}

	asio::io_context              &m_ioc;
	tcp::acceptor                  m_acceptor;
	CHttpServer::Handler           m_handler;
	CHttpServer::StreamingResolver m_streaming_resolver;
	CHttpServer::StreamingHandler  m_streaming_handler;
	std::string                    m_error;
};

}  // namespace


struct CHttpServer::Impl {
	asio::io_context           ioc{1};
	std::shared_ptr<Listener>  listener;
	std::thread                thread;
	std::atomic<bool>          running{false};
};


CHttpServer::CHttpServer()  = default;

CHttpServer::~CHttpServer()
{
	Stop();
}


bool CHttpServer::Start(const std::string &bind_address,
                        unsigned           port,
                        Handler            handler,
                        StreamingResolver  streaming_resolver,
                        StreamingHandler   streaming_handler)
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

	m_impl->listener = std::make_shared<Listener>(
		m_impl->ioc, endpoint, std::move(handler),
		std::move(streaming_resolver), std::move(streaming_handler));
	if (!m_impl->listener->Ok()) {
		m_lastError = "bind to " + bind_address + ":" + std::to_string(port)
			+ " failed: " + m_impl->listener->Error();
		m_impl.reset();
		return false;
	}
	m_impl->listener->Run();

	m_impl->running.store(true, std::memory_order_release);
	m_impl->thread = std::thread([this]{
		try {
			m_impl->ioc.run();
		} catch (const std::exception &e) {
			// io_context exception propagation: the server thread dies
			// quietly. Catch + log to stderr so an operator running in
			// foreground sees a one-line cause; daemon mode loses the
			// message (Phase 3+: route through Logging.File).
			std::cerr << "amuleapi: HTTP I/O loop exited on exception: "
			          << e.what() << std::endl;
		}
		m_impl->running.store(false, std::memory_order_release);
	});
	return true;
}


void CHttpServer::Stop()
{
	if (!m_impl) return;
	if (m_impl->listener) m_impl->listener->Stop();
	m_impl->ioc.stop();
	if (m_impl->thread.joinable()) m_impl->thread.join();
	m_impl.reset();
}
