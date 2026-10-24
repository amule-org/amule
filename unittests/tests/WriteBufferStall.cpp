// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
// Linux integration fixture: stall only .part writes in this disposable daemon.
#include <dlfcn.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <cerrno>
#include <fcntl.h>

extern "C" ssize_t write(int fd, const void *data, size_t size)
{
	using Write = ssize_t (*)(int, const void *, size_t);
	static auto realWrite = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "write"));
	const char *gate = std::getenv("AMULE_TEST_WRITE_GATE");
	if (gate != nullptr) {
		char link[64];
		std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
		char path[4096];
		const ssize_t length = readlink(link, path, sizeof(path) - 1);
		if (length > 5 && std::memcmp(path + length - 5, ".part", 5) == 0) {
			// The bound also permits cleanup if the parent test crashes.
			for (unsigned tick = 0; tick < 3000 && access(gate, F_OK) == 0; ++tick) {
				usleep(10000);
			}
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
