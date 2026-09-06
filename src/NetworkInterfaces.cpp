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

#include "NetworkInterfaces.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#ifdef __WINDOWS__
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h> // inet_ntop
#include <unistd.h>    // close() for the per-address flag probe
// Every POSIX target other than Linux that this project builds for keeps the
// per-address IPv6 flags behind SIOCGIFAFLAG_IN6 instead of a /proc file. The
// header is BSD-family only, so it is reached by name rather than by "not
// Linux", which would break the first time somebody builds on Solaris.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
	defined(__DragonFly__)
#include <sys/ioctl.h>
#include <netinet6/in6_var.h> // SIOCGIFAFLAG_IN6 / IN6_IFF_TENTATIVE
#endif
#endif

namespace
{

typedef std::array<std::uint8_t, 16> Ipv6Bytes;

#if !defined(__WINDOWS__) && defined(__linux__)

//! IFA_F_TENTATIVE, spelled out rather than pulled from <linux/if_addr.h>:
//! that header is not reliably reachable from userspace across the libc
//! implementations this project builds against, and the value is ABI-stable.
const unsigned int LINUX_IFA_F_TENTATIVE = 0x40;

/**
 * Per-address IPv6 flags, as the kernel reports them.
 *
 * getifaddrs() lists which IPv6 addresses exist but not what state each one is
 * in, and on Linux this file is the only place the flags are exposed. Without
 * it an address still running duplicate address detection is indistinguishable
 * from a finished one.
 *
 * An address missing from this list is treated as flagless rather than
 * skipped: a container or a stripped-down root may not mount /proc, and losing
 * the whole enumeration there would be worse than losing the DAD distinction.
 */
std::vector<std::pair<Ipv6Bytes, unsigned int>> ReadIfInet6Flags()
{
	std::vector<std::pair<Ipv6Bytes, unsigned int>> result;

	std::FILE *file = std::fopen("/proc/net/if_inet6", "r");
	if (file == nullptr) {
		return result;
	}

	// One line per address: 32 hex digits, then index, prefix length, scope
	// and flags in hex, then the device name.
	char address[40] = { 0 };
	char device[64] = { 0 };
	unsigned int index = 0;
	unsigned int prefix = 0;
	unsigned int scope = 0;
	unsigned int flags = 0;
	while (std::fscanf(file, "%39s %x %x %x %x %63s", address, &index, &prefix, &scope, &flags, device) ==
		6) {
		if (std::strlen(address) != 32) {
			continue;
		}
		Ipv6Bytes bytes = {};
		bool parsed = true;
		for (std::size_t i = 0; i < bytes.size() && parsed; ++i) {
			char octet[3] = { address[i * 2], address[i * 2 + 1], '\0' };
			char *end = nullptr;
			const unsigned long value = std::strtoul(octet, &end, 16);
			if (end != octet + 2) {
				parsed = false;
				break;
			}
			bytes[i] = static_cast<std::uint8_t>(value);
		}
		if (parsed) {
			result.push_back(std::make_pair(bytes, flags));
		}
	}

	std::fclose(file);
	return result;
}

#endif // !__WINDOWS__ && __linux__

} // namespace

std::vector<NetworkInterface> DetectNetworkInterfaces()
{
	std::vector<NetworkInterface> result;

	// Find the entry for @a name, appending one if this is the first time we
	// have seen it. Needed because the POSIX enumeration lists one node per
	// address family, so a dual-stack interface appears more than once.
	auto entryFor = [&result](const wxString &name) -> NetworkInterface & {
		for (NetworkInterface &iface : result) {
			if (iface.name == name) {
				return iface;
			}
		}
		result.emplace_back();
		result.back().name = name;
		return result.back();
	};

#ifdef __WINDOWS__
	ULONG size = 15000;
	std::vector<uint8_t> buf(size);
	// Unicast addresses are deliberately NOT skipped here: they are what
	// fills the "bind to IP" drop-down.
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	PIP_ADAPTER_ADDRESSES aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&buf[0]);
	ULONG ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &size);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&buf[0]);
		ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &size);
	}
	if (ret == NO_ERROR) {
		for (PIP_ADAPTER_ADDRESSES p = aa; p != nullptr; p = p->Next) {
			if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK || p->FriendlyName == nullptr) {
				continue;
			}
			NetworkInterface &iface = entryFor(wxString(p->FriendlyName));
			// An IPv6-only adapter reports its index in Ipv6IfIndex and
			// leaves IfIndex zero, so both have to be consulted.
			if (iface.index == 0) {
				iface.index = p->IfIndex != 0 ? p->IfIndex : p->Ipv6IfIndex;
			}
			for (PIP_ADAPTER_UNICAST_ADDRESS ua = p->FirstUnicastAddress; ua != nullptr;
				ua = ua->Next) {
				const sockaddr *sa = ua->Address.lpSockaddr;
				if (sa == nullptr) {
					continue;
				}
				if (sa->sa_family == AF_INET) {
					char text[INET_ADDRSTRLEN] = { 0 };
					const sockaddr_in *sin = reinterpret_cast<const sockaddr_in *>(sa);
					if (::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) !=
						nullptr) {
						iface.ipv4.Add(wxString::FromUTF8(text));
					}
				} else if (sa->sa_family == AF_INET6) {
					// Anything but Preferred is an address we cannot claim
					// as ours: Tentative is still in duplicate address
					// detection, Duplicate lost it, Invalid and Deprecated
					// are on their way out.
					if (ua->DadState != IpDadStatePreferred) {
						continue;
					}
					const sockaddr_in6 *sin6 = reinterpret_cast<const sockaddr_in6 *>(sa);
					Ipv6Bytes bytes = {};
					std::memcpy(bytes.data(), &sin6->sin6_addr, bytes.size());
					iface.ipv6.push_back(bytes);
				}
			}
		}
	}
#else
	struct ifaddrs *ifaces = nullptr;
	if (getifaddrs(&ifaces) == 0) {
#ifdef __linux__
		const std::vector<std::pair<Ipv6Bytes, unsigned int>> inet6Flags = ReadIfInet6Flags();
#elif defined(SIOCGIFAFLAG_IN6)
		// One socket for the whole walk: the ioctl needs a handle of the
		// right family, and opening one per address would turn an
		// enumeration into a syscall storm.
		const int flagSocket = ::socket(AF_INET6, SOCK_DGRAM, 0);
#endif
		for (struct ifaddrs *p = ifaces; p != nullptr; p = p->ifa_next) {
			if (p->ifa_name == nullptr || (p->ifa_flags & IFF_LOOPBACK)) {
				continue;
			}
			NetworkInterface &iface = entryFor(wxString::FromUTF8(p->ifa_name));
			if (iface.index == 0) {
				iface.index = ::if_nametoindex(p->ifa_name);
			}
			if (p->ifa_addr == nullptr) {
				continue;
			}
			if (p->ifa_addr->sa_family == AF_INET) {
				char text[INET_ADDRSTRLEN] = { 0 };
				const sockaddr_in *sin = reinterpret_cast<const sockaddr_in *>(p->ifa_addr);
				if (::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) != nullptr) {
					iface.ipv4.Add(wxString::FromUTF8(text));
				}
			} else if (p->ifa_addr->sa_family == AF_INET6) {
				const sockaddr_in6 *sin6 =
					reinterpret_cast<const sockaddr_in6 *>(p->ifa_addr);
				Ipv6Bytes bytes = {};
				std::memcpy(bytes.data(), &sin6->sin6_addr, bytes.size());
#ifdef __linux__
				bool tentative = false;
				for (const auto &entry : inet6Flags) {
					if (entry.first == bytes) {
						tentative = (entry.second & LINUX_IFA_F_TENTATIVE) != 0;
						break;
					}
				}
				if (tentative) {
					continue;
				}
#elif defined(SIOCGIFAFLAG_IN6)
				if (flagSocket >= 0) {
					struct in6_ifreq request;
					std::memset(&request, 0, sizeof(request));
					std::strncpy(
						request.ifr_name, p->ifa_name, sizeof(request.ifr_name) - 1);
					std::memcpy(&request.ifr_addr, sin6, sizeof(request.ifr_addr));
					if (::ioctl(flagSocket, SIOCGIFAFLAG_IN6, &request) == 0 &&
						(request.ifr_ifru.ifru_flags6 &
							(IN6_IFF_TENTATIVE | IN6_IFF_DUPLICATED)) != 0) {
						continue;
					}
				}
#endif
				iface.ipv6.push_back(bytes);
			}
		}
#if !defined(__linux__) && defined(SIOCGIFAFLAG_IN6)
		if (flagSocket >= 0) {
			::close(flagSocket);
		}
#endif
		freeifaddrs(ifaces);
	}
#endif
	return result;
}
