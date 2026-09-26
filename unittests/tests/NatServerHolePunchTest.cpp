// Standalone C++17 test: no application, network, or test-framework dependencies.
#include "NatServerHolePunch.h"

#include <cstdlib>
#include <iostream>
#include <type_traits>

using namespace NatServerHolePunch;

static void Check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(EXIT_FAILURE);
	}
}

template <std::size_t N, typename Parser>
static void CheckLengths(const std::array<std::uint8_t, N> &bytes, Parser parse)
{
	for (std::size_t size = 0; size < N; ++size) {
		Check(!parse(bytes.data(), size), "truncated payload accepted");
	}
	std::array<std::uint8_t, N + 1> extended{};
	std::copy(bytes.begin(), bytes.end(), extended.begin());
	Check(!parse(extended.data(), extended.size()), "trailing byte accepted");
	Check(!parse(nullptr, N), "null payload accepted");
	Check(!parse(nullptr, 0), "empty null payload accepted");
}

int main()
{
	const std::array<std::uint8_t, 6> requestBytes{ { 0x78, 0x56, 0x34, 0x12, 0xCD, 0xAB } };
	Check(BuildRequest({ 0x12345678, 0xABCD }) == requestBytes, "request wire bytes");
	const auto request = ParseRequest(requestBytes.data(), requestBytes.size());
	Check(request && request->targetId == 0x12345678 && request->udpPort == 0xABCD, "request fields");
	CheckLengths(requestBytes, ParseRequest);

	const Hash hash{ { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } };
	const IPv4 ip{ { 192, 0, 2, 1 } };
	const std::array<std::uint8_t, 25> infoBytes{ { 192,
		0,
		2,
		1,
		0x36,
		0x12,
		0x78,
		0x56,
		0,
		1,
		2,
		3,
		4,
		5,
		6,
		7,
		8,
		9,
		10,
		11,
		12,
		13,
		14,
		15,
		0xFE } };
	Check(BuildInfo({ ip, 0x1236, 0x5678, hash, 0xFE }) == infoBytes, "info wire bytes");
	const auto info = ParseInfo(infoBytes.data(), infoBytes.size());
	Check(info && info->peerIP == ip && info->tcpPort == 0x1236 && info->udpPort == 0x5678 &&
			info->userHash == hash && info->role == 0xFE,
		"info fields and opaque role");
	CheckLengths(infoBytes, ParseInfo);

	const std::array<std::uint8_t, 5> failureBytes{ { 0xEF, 0xCD, 0xAB, 0x89, 0xFF } };
	Check(BuildFailure({ 0x89ABCDEF, 0xFF }) == failureBytes, "failure wire bytes");
	const auto failure = ParseFailure(failureBytes.data(), failureBytes.size());
	Check(failure && failure->targetId == 0x89ABCDEF && failure->reason == 0xFF,
		"unknown failure reason preserved");
	CheckLengths(failureBytes, ParseFailure);
	for (unsigned reason = 0; reason < 256; ++reason) {
		const auto bytes = BuildFailure({ 0, static_cast<std::uint8_t>(reason) });
		const auto parsed = ParseFailure(bytes.data(), bytes.size());
		Check(parsed && parsed->reason == reason, "opaque reason round trip");
	}

	Check(BuildKeepalive(hash) == hash, "keepalive wire bytes");
	const auto keepalive = ParseKeepalive(hash.data(), hash.size());
	Check(keepalive && *keepalive == hash, "keepalive hash");
	CheckLengths(hash, ParseKeepalive);

	static_assert(NatServerTcp::OP_LOWID_HOLEPUNCH_REQUEST == 0x60, "request opcode");
	static_assert(NatServerTcp::OP_LOWID_HOLEPUNCH_INFO == 0x61, "info opcode");
	static_assert(NatServerTcp::OP_LOWID_HOLEPUNCH_FAIL == 0x62, "failure opcode");
	static_assert(NatServerUdp::OP_NATT_KEEPALIVE == 0x9F, "keepalive opcode");
	static_assert(NatPeerUdp::OP_NATT_HOLEPUNCH == 0xB3, "peer opcode");
	static_assert(!std::is_same<NatServerTcp::Opcode, NatPeerUdp::Opcode>::value,
		"TCP and peer UDP namespaces must remain distinct");
	// TCP OP_ESERVER_BUDDY_REQUEST also uses 0xB3 in the external protocol.
	// Recognizing the UDP byte must never admit it as a server coordination opcode.
	for (unsigned opcode = 0; opcode < 256; ++opcode) {
		const auto byte = static_cast<std::uint8_t>(opcode);
		Check(IsServerTcpOpcode(byte) == (opcode >= 0x60 && opcode <= 0x62), "TCP namespace");
		Check(IsServerUdpOpcode(byte) == (opcode == 0x9F), "server UDP namespace");
		Check(IsPeerUdpOpcode(byte) == (opcode == 0xB3), "peer UDP namespace");
	}
	std::cout << "NatServerHolePunchTest passed\n";
	return EXIT_SUCCESS;
}
