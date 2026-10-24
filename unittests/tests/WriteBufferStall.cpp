// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
// Linux integration fixture: stall only .part writes in this disposable daemon.
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#include <sys/vfs.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <cerrno>
#include <fcntl.h>

namespace
{
void Stall(const char *gate)
{
	const std::string entered = std::string(gate) + ".entered";
	const int marker = open(entered.c_str(), O_WRONLY | O_CREAT, 0600);
	if (marker >= 0) {
		close(marker);
	}
	for (unsigned tick = 0; tick < 3000 && access(gate, F_OK) == 0; ++tick) {
		usleep(10000);
	}
}
} // namespace

extern "C" int statfs64(const char *path, struct statfs64 *space) noexcept
{
	using Stat = int (*)(const char *, struct statfs64 *);
	static auto realStat = reinterpret_cast<Stat>(dlsym(RTLD_NEXT, "statfs64"));
	const char *target = std::getenv("AMULE_TEST_SPACE_PATH");
	const char *gate = std::getenv("AMULE_TEST_WRITE_GATE");
	if (target != nullptr && gate != nullptr && std::strcmp(path, target) == 0) {
		Stall(gate);
	}
	return realStat(path, space);
}

extern "C" ssize_t write(int fd, const void *data, size_t size)
{
	using Write = ssize_t (*)(int, const void *, size_t);
	static auto realWrite = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "write"));
	const char *gate = std::getenv("AMULE_TEST_WRITE_GATE");
	if (gate != nullptr && std::getenv("AMULE_TEST_SPACE_PATH") == nullptr) {
		char link[64];
		std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
		char path[4096];
		const ssize_t length = readlink(link, path, sizeof(path) - 1);
		if (length > 5 && std::memcmp(path + length - 5, ".part", 5) == 0) {
			const char *prefix = std::getenv("AMULE_TEST_WRITE_PREFIX");
			if (prefix != nullptr &&
				(static_cast<size_t>(length) < std::strlen(prefix) ||
					std::memcmp(path, prefix, std::strlen(prefix)) != 0)) {
				return realWrite(fd, data, size);
			}
			Stall(gate);
			const char *failure = std::getenv("AMULE_TEST_WRITE_ERROR");
			if (failure != nullptr && access(failure, F_OK) == 0) {
				const std::string observed = std::string(failure) + ".observed";
				const int marker = open(observed.c_str(), O_WRONLY | O_CREAT, 0600);
				if (marker >= 0) {
					close(marker);
				}
				errno = EIO;
				return -1;
			}
		}
	}
	return realWrite(fd, data, size);
}
