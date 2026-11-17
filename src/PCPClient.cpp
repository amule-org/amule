#include "PCPClient.h"
#include "PCPProtocol.h"
#include "CryptoPP_Inc.h"

#include <boost/asio.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace pcp
{
using boost::asio::ip::udp;
using Clock = std::chrono::steady_clock;

struct Client::State : std::enable_shared_from_this<Client::State>
{
	State(udp::endpoint gateway, boost::asio::ip::address_v6 local)
	: work(boost::asio::make_work_guard(context))
	, socket(context)
	, timer(context)
	, localAddress(local)
	{
		boost::system::error_code error;
		if (!gateway.address().is_v6() || local.is_unspecified() || local.is_multicast()) {
			return;
		}
		socket.open(udp::v6(), error);
		if (!error) {
			socket.bind(udp::endpoint(local, 0), error);
		}
		if (!error) {
			socket.connect(gateway, error);
		}
		if (!error) {
			socket.non_blocking(true, error);
		}
		ready = !error;
	}

	void Start()
	{
		if (ready) {
			Receive();
		}
		thread = std::thread([self = shared_from_this()] { self->context.run(); });
	}

	void Receive()
	{
		socket.async_receive(boost::asio::buffer(buffer),
			[self = shared_from_this()](boost::system::error_code error, std::size_t size) {
				if (self->stopped.load()) {
					return;
				}
				if (!error && self->request) {
					const std::vector<std::uint8_t> bytes(
						self->buffer.begin(), self->buffer.begin() + size);
					const auto response = DecodeMapResponse(bytes, *self->request);
					if (response) {
						self->Accept(*response);
					}
				}
				if (error != boost::asio::error::operation_aborted) {
					self->Receive();
				}
			});
	}

	void Accept(const Response &response)
	{
		if (haveEpoch && static_cast<std::uint64_t>(response.epoch) + 2 < lastEpoch) {
			lastEpoch = response.epoch;
			Publish(false, 0);
			retryExponent = 0;
			Send();
			return;
		}
		if (response.result == 0 && (!response.mapping || response.mapping->lifetime == 0)) {
			return;
		}
		haveEpoch = true;
		lastEpoch = response.epoch;
		if (response.result != 0) {
			Arm(std::chrono::seconds(response.lifetime == 0 ? 30 : response.lifetime));
			return;
		}
		const auto granted = response.mapping->lifetime;
		Publish(true, granted);
		retryExponent = 0;
		// The server's grant, not the requested hour, controls renewal (RFC 6887 section 11.2.1).
		Arm(std::chrono::milliseconds(
			static_cast<std::uint64_t>(granted) * random.GenerateWord32(500, 625)));
	}

	void Publish(bool active, std::uint32_t lifetime)
	{
		std::lock_guard<std::mutex> lock(snapshotMutex);
		snapshot.active = active;
		snapshot.internalPort = request ? request->internalPort : 0;
		snapshot.grantedLifetime = lifetime;
		snapshot.generation = generation;
		expires = active ? Clock::now() + std::chrono::seconds(lifetime) : Clock::time_point{};
	}

	void Arm(std::chrono::milliseconds delay)
	{
		timer.expires_after(delay);
		timer.async_wait(
			[self = shared_from_this(), version = generation](boost::system::error_code error) {
				if (!error && !self->stopped.load() && self->generation == version) {
					self->Send();
				}
			});
	}

	void Send()
	{
		if (stopped.load() || !request || !ready) {
			return;
		}
		const auto encoded = EncodeMapRequest(*request);
		if (!encoded) {
			return;
		}
		auto bytes = std::make_shared<std::vector<std::uint8_t>>(*encoded);
		socket.async_send(boost::asio::buffer(*bytes),
			[self = shared_from_this(), bytes](boost::system::error_code, std::size_t) {});
		const auto seconds = std::min<std::uint64_t>(3ULL << std::min(retryExponent, 10U), 1024);
		retryExponent = std::min(retryExponent + 1, 10U);
		Arm(std::chrono::milliseconds(seconds * random.GenerateWord32(900, 1100)));
	}

	void Delete()
	{
		if (!request || !ready) {
			return;
		}
		auto deletion = *request;
		deletion.lifetime = 0;
		const auto bytes = EncodeMapRequest(deletion);
		if (bytes) {
			// Best effort on a nonblocking socket: shutdown never waits for a gateway reply.
			boost::system::error_code error;
			socket.send(boost::asio::buffer(*bytes), 0, error);
		}
	}

	void Drop()
	{
		timer.cancel();
		Delete();
		request.reset();
		haveEpoch = false;
		retryExponent = 0;
		++generation;
		Publish(false, 0);
	}

	void Apply(std::uint16_t port, boost::asio::ip::address_v6 external, std::uint16_t externalPort)
	{
		if (!ready || stopped.load()) {
			return;
		}
		if (request && request->internalPort == port &&
			request->suggestedExternalPort == externalPort &&
			request->suggestedExternalAddress == external.to_bytes()) {
			return;
		}
		Drop();
		// This backend opens a firewall pinhole, not a translated or third-party mapping.
		if (port == 0 || externalPort != port || external != localAddress) {
			return;
		}
		Request desired;
		try {
			random.GenerateBlock(desired.nonce.data(), desired.nonce.size());
		} catch (const CryptoPP::Exception &) {
			return;
		}
		desired.clientAddress = localAddress.to_bytes();
		desired.suggestedExternalAddress = external.to_bytes();
		desired.internalPort = port;
		desired.suggestedExternalPort = externalPort;
		desired.lifetime = 3600;
		desired.preferFailure = true;
		request = desired;
		Publish(false, 0);
		Send();
	}

	void Stop()
	{
		std::lock_guard<std::mutex> lock(stopMutex);
		if (!thread.joinable()) {
			return;
		}
		stopped.store(true);
		boost::asio::post(context, [self = shared_from_this()] {
			self->Drop();
			boost::system::error_code error;
			self->socket.cancel(error);
			self->socket.close(error);
			self->work.reset();
		});
		thread.join();
	}

	boost::asio::io_context context;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
	udp::socket socket;
	boost::asio::steady_timer timer;
	const boost::asio::ip::address_v6 localAddress;
	CryptoPP::AutoSeededRandomPool random;
	std::array<std::uint8_t, 1100> buffer{};
	std::optional<Request> request;
	std::uint32_t lastEpoch = 0;
	std::uint64_t generation = 0;
	unsigned retryExponent = 0;
	bool haveEpoch = false;
	bool ready = false;
	std::atomic<bool> stopped{ false };
	mutable std::mutex snapshotMutex;
	LeaseSnapshot snapshot;
	Clock::time_point expires;
	std::mutex stopMutex;
	std::thread thread;
};

Client::Client(udp::endpoint gateway, boost::asio::ip::address_v6 localAddress)
: m_state(std::make_shared<State>(gateway, localAddress))
{
	m_state->Start();
}

Client::~Client()
{
	Stop();
}

void Client::SetMapping(
	std::uint16_t internalPort, boost::asio::ip::address_v6 externalAddress, std::uint16_t externalPort)
{
	const auto state = m_state;
	// Serialize admission with Stop so no callback retains State after its event loop has exited.
	std::lock_guard<std::mutex> lock(state->stopMutex);
	if (state->stopped.load()) {
		return;
	}
	boost::asio::post(state->context, [state, internalPort, externalAddress, externalPort] {
		state->Apply(internalPort, externalAddress, externalPort);
	});
}

void Client::ClearMapping()
{
	SetMapping(0, m_state->localAddress, 0);
}

LeaseSnapshot Client::Snapshot() const
{
	std::lock_guard<std::mutex> lock(m_state->snapshotMutex);
	auto snapshot = m_state->snapshot;
	if (m_state->stopped.load() || Clock::now() >= m_state->expires) {
		snapshot.active = false;
	}
	return snapshot;
}

void Client::Stop()
{
	m_state->Stop();
}
} // namespace pcp
