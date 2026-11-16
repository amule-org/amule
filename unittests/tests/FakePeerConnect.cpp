// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
// Linux integration fixture: a connect() to the IPv4 in AMULE_TEST_FAKE_PEER goes to 127.0.0.1 at
// the same port, so a test can answer for an address this host does not have.
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstdlib>
#include <cstring>

extern "C" int connect(int fd, const struct sockaddr *address, socklen_t length)
{
	using Connect = int (*)(int, const struct sockaddr *, socklen_t);
	static auto realConnect = reinterpret_cast<Connect>(dlsym(RTLD_NEXT, "connect"));
	const char *fake = std::getenv("AMULE_TEST_FAKE_PEER");
	in_addr fakeAddress{};
	if (fake == nullptr || address == nullptr || inet_pton(AF_INET, fake, &fakeAddress) != 1) {
		return realConnect(fd, address, length);
	}
	if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
		sockaddr_in target;
		std::memcpy(&target, address, sizeof(target));
		if (target.sin_addr.s_addr == fakeAddress.s_addr) {
			target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			return realConnect(fd, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
		}
	} else if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
		// A dual-stack socket dials IPv4 as ::ffff:a.b.c.d.
		sockaddr_in6 target;
		std::memcpy(&target, address, sizeof(target));
		if (IN6_IS_ADDR_V4MAPPED(&target.sin6_addr) &&
			std::memcmp(&target.sin6_addr.s6_addr[12], &fakeAddress, 4) == 0) {
			const in_addr loopback{ htonl(INADDR_LOOPBACK) };
			std::memcpy(&target.sin6_addr.s6_addr[12], &loopback, 4);
			return realConnect(fd, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
		}
	}
	return realConnect(fd, address, length);
}
