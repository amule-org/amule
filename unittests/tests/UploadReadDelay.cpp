// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
// Linux-only fault injection. No delay or storage-identity hooks enter production.
// Define both libc symbol spellings even when the daemon uses large-file aliases.
#undef _FILE_OFFSET_BITS
#define _LARGEFILE64_SOURCE 1
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

namespace
{
std::atomic<std::uintptr_t> mappedRead{ 0 };
std::atomic<std::uintptr_t> heapRead{ 0 };
std::atomic<std::uintptr_t> ownedMapping{ 0 };
std::atomic<long> mappingThread{ 0 };

bool Armed()
{
	const char *arm = std::getenv("AMULE_TEST_READ_ARM");
	return arm && access(arm, F_OK) == 0;
}

bool Stage(const char *stage)
{
	const char *requested = std::getenv("AMULE_TEST_READ_STAGE");
	return std::strcmp(requested ? requested : "read", stage) == 0;
}

bool Matches(const char *path)
{
	const char *exact = std::getenv("AMULE_TEST_READ_PATH");
	const char *prefix = std::getenv("AMULE_TEST_READ_PREFIX");
	return (exact && std::strcmp(path, exact) == 0) ||
	       (prefix && std::strncmp(path, prefix, std::strlen(prefix)) == 0);
}

bool MatchesDescriptor(int fd)
{
	char link[64];
	char path[PATH_MAX];
	std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	const auto length = readlink(link, path, sizeof(path) - 1);
	if (length < 0) {
		return false;
	}
	path[length] = '\0';
	return Matches(path);
}

void Gate(const char *path, std::uint64_t offset)
{
	static std::atomic<bool> disabled{ false };
	if (disabled || !Armed()) {
		return;
	}
	if (!Matches(path)) {
		return;
	}
	const char *notify = std::getenv("AMULE_TEST_READ_NOTIFY_FD");
	const char *release = std::getenv("AMULE_TEST_READ_RELEASE_FD");
	if (!notify || !release) {
		_exit(98);
	}
	if (write(std::atoi(notify), &offset, sizeof(offset)) != sizeof(offset)) {
		_exit(98);
	}
	pollfd gate{ std::atoi(release), POLLIN, 0 };
	int ready;
	do {
		// Diagnostics may deliberately hold a read through the first 30-second
		// report, with a 40-second assertion deadline. Leave scheduling margin.
		ready = poll(&gate, 1, 60000);
	} while (ready < 0 && errno == EINTR);
	if (ready <= 0) {
		_exit(98);
	}
	char token;
	ssize_t released;
	do {
		released = syscall(SYS_read, gate.fd, &token, 1);
	} while (released < 0 && errno == EINTR);
	if (released == 0) {
		disabled = true;
	} else if (released != 1) {
		_exit(98);
	}
}

const char *MountTable(const char *path)
{
	const char *fixture = std::getenv("AMULE_TEST_MOUNTINFO");
	return fixture && std::strcmp(path, "/proc/self/mountinfo") == 0 ? fixture : path;
}
} // namespace

extern "C" ssize_t read(int fd, void *buffer, size_t count)
{
	if (Stage("read") && Armed()) {
		char link[64];
		char path[PATH_MAX];
		std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
		const auto length = readlink(link, path, sizeof(path) - 1);
		if (length >= 0) {
			path[length] = '\0';
			Gate(path, static_cast<std::uint64_t>(lseek(fd, 0, SEEK_CUR)));
		}
	}
	if (Stage("heap") && Armed() && count == 184320 && MatchesDescriptor(fd)) {
		heapRead = reinterpret_cast<std::uintptr_t>(buffer);
	}
	return syscall(SYS_read, fd, buffer, count);
}

extern "C" int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, mode_t);
		va_end(args);
	}
	if (Stage("open")) {
		Gate(path, UINT64_MAX);
	}
	return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}

extern "C" int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, mode_t);
		va_end(args);
	}
	if (Stage("open")) {
		Gate(path, UINT64_MAX);
	}
	return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags | O_LARGEFILE, mode));
}

extern "C" FILE *fopen(const char *path, const char *mode)
{
	if (Stage("discovery")) {
		Gate(path, UINT64_MAX - 1);
	}
	static const auto original =
		reinterpret_cast<FILE *(*)(const char *, const char *)>(dlsym(RTLD_NEXT, "fopen"));
	return original(MountTable(path), mode);
}

extern "C" FILE *fopen64(const char *path, const char *mode)
{
	if (Stage("discovery")) {
		Gate(path, UINT64_MAX - 1);
	}
	static const auto original =
		reinterpret_cast<FILE *(*)(const char *, const char *)>(dlsym(RTLD_NEXT, "fopen64"));
	return original(MountTable(path), mode);
}

// Hold packet construction after its upload-list snapshot, without production hooks.
extern "C" int compress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen, int level)
{
	if (Stage("heap") && Armed() && sourceLen == 184320) {
		if (heapRead.load() != reinterpret_cast<std::uintptr_t>(source)) {
			_exit(97); // packet construction must reuse the original heap read buffer
		}
		Gate(std::getenv("AMULE_TEST_READ_PATH"), sourceLen);
	}
	if (Stage("pack") && sourceLen == 184320) {
		Gate("upload-packet-build", sourceLen);
	}
	static const auto original = reinterpret_cast<decltype(&compress2)>(dlsym(RTLD_NEXT, "compress2"));
	return original(dest, destLen, source, sourceLen, level);
}

namespace
{
void TrackMapping(void *address, int fd)
{
	if (address != MAP_FAILED && fd >= 0 && Stage("copy") && Armed() && MatchesDescriptor(fd)) {
		mappedRead = reinterpret_cast<std::uintptr_t>(address);
		ownedMapping = mappedRead.load();
		mappingThread = syscall(SYS_gettid);
	}
}

void CopyGate(const void *source, size_t length)
{
	// memcpy's nonnull contract lets optimized builds remove a source-null
	// check. Zero-length packet copies must never look like the empty mapping
	// sentinel, and unrelated fixture stages must not inspect copy ownership.
	if (length == 0 || !Stage("copy") || !Armed()) {
		return;
	}
	auto address = reinterpret_cast<std::uintptr_t>(source);
	if (address != 0 && mappedRead.compare_exchange_strong(address, 0)) {
		Gate(std::getenv("AMULE_TEST_READ_PATH"), 0);
	}
}
} // namespace

extern "C" void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
	static const auto original = reinterpret_cast<decltype(&mmap)>(dlsym(RTLD_NEXT, "mmap"));
	void *mapped = original(address, length, protection, flags, fd, offset);
	TrackMapping(mapped, fd);
	return mapped;
}

extern "C" void *mmap64(void *address, size_t length, int protection, int flags, int fd, off64_t offset)
{
	static const auto original = reinterpret_cast<decltype(&mmap64)>(dlsym(RTLD_NEXT, "mmap64"));
	void *mapped = original(address, length, protection, flags, fd, offset);
	TrackMapping(mapped, fd);
	return mapped;
}

extern "C" void *memmove(void *destination, const void *source, size_t length)
{
	static const auto original = reinterpret_cast<decltype(&memmove)>(dlsym(RTLD_NEXT, "memmove"));
	CopyGate(source, length);
	return original(destination, source, length);
}

extern "C" void *memcpy(void *destination, const void *source, size_t length)
{
	static const auto original = reinterpret_cast<decltype(&memcpy)>(dlsym(RTLD_NEXT, "memcpy"));
	CopyGate(source, length);
	return original(destination, source, length);
}

extern "C" int munmap(void *address, size_t length)
{
	static const auto original = reinterpret_cast<decltype(&munmap)>(dlsym(RTLD_NEXT, "munmap"));
	auto mapped = reinterpret_cast<std::uintptr_t>(address);
	if (mapped != 0 && ownedMapping.compare_exchange_strong(mapped, 0) &&
		syscall(SYS_gettid) != mappingThread.load()) {
		_exit(96); // both successful and failed mappings belong to the read worker
	}
	return original(address, length);
}
