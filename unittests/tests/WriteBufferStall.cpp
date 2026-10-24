// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
// Linux integration fixture: stall only .part writes in this disposable daemon.
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#include <sys/vfs.h>
#include <sys/uio.h>
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

namespace
{
bool PartWrite(int fd, unsigned segments, size_t bytes)
{
	char link[64];
	std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	char path[4096];
	const ssize_t length = readlink(link, path, sizeof(path) - 1);
	if (length <= 5 || std::memcmp(path + length - 5, ".part", 5) != 0) {
		return true;
	}
	const char *prefix = std::getenv("AMULE_TEST_WRITE_PREFIX");
	if (prefix != nullptr && (static_cast<size_t>(length) < std::strlen(prefix) ||
					 std::memcmp(path, prefix, std::strlen(prefix)) != 0)) {
		return true;
	}
	const char *trace = std::getenv("AMULE_TEST_WRITE_TRACE");
	if (trace != nullptr) {
		const int log = open(trace, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (log >= 0) {
			char line[80];
			const int size = std::snprintf(line, sizeof(line), "%u %zu\n", segments, bytes);
			using Write = ssize_t (*)(int, const void *, size_t);
			static auto realWrite = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "write"));
			if (realWrite(log, line, size) != size) {
				std::abort();
			}
			close(log);
		}
	}
	const char *gate = std::getenv("AMULE_TEST_WRITE_GATE");
	if (gate == nullptr || std::getenv("AMULE_TEST_SPACE_PATH") != nullptr) {
		return true;
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
		return false;
	}
	return true;
}
} // namespace

extern "C" ssize_t write(int fd, const void *data, size_t size)
{
	using Write = ssize_t (*)(int, const void *, size_t);
	static auto realWrite = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "write"));
	return PartWrite(fd, 1, size) ? realWrite(fd, data, size) : -1;
}
extern "C" ssize_t writev(int fd, const iovec *vectors, int count)
{
	using Write = ssize_t (*)(int, const iovec *, int);
	static auto realWrite = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "writev"));
	size_t bytes = 0;
	for (int i = 0; i < count; ++i) {
		bytes += vectors[i].iov_len;
	}
	return PartWrite(fd, count, bytes) ? realWrite(fd, vectors, count) : -1;
}
