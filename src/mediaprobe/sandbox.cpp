// SPDX-License-Identifier: GPL-2.0-or-later
#include "sandbox.h"

#ifndef _WIN32
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <cstddef>
#include <cstdint>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#if defined(__s390x__) && defined(__GLIBC__)
#include <malloc.h>
#endif
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <vector>
#elif defined(__APPLE__)
#include <sandbox.h>
#endif

namespace probe::sandbox
{
namespace
{
bool closeOtherFiles()
{
#if defined(__linux__) || defined(__APPLE__)
#if defined(__linux__) && defined(__NR_close_range)
	if (syscall(__NR_close_range, 3U, UINT_MAX, 0U) == 0)
		return true;
#endif

		// Older kernels: enumerate before installing the filter. No descriptors from
		// the core, including sockets and writable files, may survive into parsing.
#ifdef __linux__
	DIR *dir = opendir("/proc/self/fd");
#else
	DIR *dir = opendir("/dev/fd");
#endif
	if (dir == nullptr)
		return false;
	const int ownFd = dirfd(dir);
	bool complete = ownFd >= 0;
	while (complete) {
		errno = 0;
		const auto *entry = readdir(dir);
		if (entry == nullptr) {
			complete = errno == 0;
			break;
		}
		char *end = nullptr;
		const long fd = std::strtol(entry->d_name, &end, 10);
		if (*end == '\0' && fd >= 3 && fd <= INT_MAX && fd != ownFd)
			complete = close(static_cast<int>(fd)) == 0;
	}
	const int closed = closedir(dir);
	return complete && closed == 0;
#else
	return false;
#endif
}

#ifdef __linux__
bool filterSyscalls()
{
	// A filter for the wrong ABI can interpret a dangerous syscall as an allowed
	// one. Unsupported ABIs fail closed instead of using another architecture's IDs.
#if defined(__x86_64__) && !defined(__ILP32__)
	constexpr uint32_t arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
	constexpr uint32_t arch = AUDIT_ARCH_AARCH64;
#elif defined(__i386__)
	constexpr uint32_t arch = AUDIT_ARCH_I386;
#elif defined(__arm__) && defined(__ARM_EABI__)
#ifdef __ARMEB__
	constexpr uint32_t arch = AUDIT_ARCH_ARMEB;
#else
	constexpr uint32_t arch = AUDIT_ARCH_ARM;
#endif
#elif defined(__powerpc64__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	constexpr uint32_t arch = AUDIT_ARCH_PPC64LE;
#else
	constexpr uint32_t arch = AUDIT_ARCH_PPC64;
#endif
#elif defined(__riscv) && __riscv_xlen == 64
	constexpr uint32_t arch = AUDIT_ARCH_RISCV64;
#elif defined(__s390x__)
	constexpr uint32_t arch = AUDIT_ARCH_S390X;
#else
	std::fprintf(stderr, "media-probe sandbox: unsupported Linux syscall ABI\n");
	return false;
#endif
#if (defined(__x86_64__) && !defined(__ILP32__)) || defined(__aarch64__) || defined(__i386__) || \
	(defined(__arm__) && defined(__ARM_EABI__)) || defined(__powerpc64__) || \
	(defined(__riscv) && __riscv_xlen == 64) || defined(__s390x__)
	// seccomp arguments are 64-bit even on 32-bit ABIs. BPF word loads must
	// inspect the low word on both byte orders (not the zero high word on s390x).
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	constexpr uint32_t lowWord = 4;
#else
	constexpr uint32_t lowWord = 0;
#endif
	std::vector<sock_filter> code;
	const auto statement = [&](uint16_t op, uint32_t value) { code.push_back(BPF_STMT(op, value)); };
	const auto jump = [&](uint32_t value, uint8_t yes, uint8_t no) {
		code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, value, yes, no));
	};
	const auto allow = [&](uint32_t nr) {
		jump(nr, 0, 1);
		statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	};
	const auto allowFd = [&](uint32_t nr, uint32_t fd) {
		jump(nr, 0, 4);
		statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0]) + lowWord);
		jump(fd, 0, 1);
		statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
		statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
	};
	statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
	jump(arch, 1, 0);
	statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
	statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
	allowFd(__NR_read, STDIN_FILENO);
	allowFd(__NR_readv, STDIN_FILENO);
#ifdef __NR_pread64
	allowFd(__NR_pread64, STDIN_FILENO);
#endif
	allowFd(__NR_lseek, STDIN_FILENO);
#ifdef __NR__llseek
	allowFd(__NR__llseek, STDIN_FILENO);
#endif
	// musl stdio uses vectored I/O. Both APIs obey the same descriptor rules;
	// sendfile, splice, descriptor duplication and acquisition remain denied.
	for (const auto nr : { __NR_write, __NR_writev }) {
		jump(nr, 0, 5);
		statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0]) + lowWord);
		jump(STDOUT_FILENO, 2, 0);
		jump(STDERR_FILENO, 1, 0);
		statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
		statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	}
	// Allocations are anonymous and cannot introduce executable memory or map
	// another inherited file. Executable mappings already loaded by the runtime stay.
	const auto allowAnonymousMap = [&](uint32_t nr) {
		jump(nr, 0, 6);
		statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[3]) + lowWord);
		code.push_back(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, MAP_ANONYMOUS, 0, 3));
		statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[2]) + lowWord);
		code.push_back(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 1, 0));
		statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
		statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
	};
	// Old mmap on i386/ARM/s390 passes a pointer to its arguments. Seccomp
	// cannot inspect that memory; never treat those calls as the six-argument ABI.
#if defined(__NR_mmap) && !defined(__i386__) && !defined(__arm__) && !defined(__s390x__)
	allowAnonymousMap(__NR_mmap);
#endif
#if defined(__NR_mmap2) && !defined(__s390x__)
	allowAnonymousMap(__NR_mmap2);
#endif
	// These operate on this process or its already-open descriptors. All path
	// opens, networking (including Unix sockets), process creation, exec, ptrace,
	// signals to other processes and io_uring remain denied by default.
	for (const auto nr : { __NR_brk,
		     __NR_munmap,
		     __NR_mremap,
		     __NR_madvise,
		     __NR_futex,
		     __NR_close,
		     __NR_exit,
		     __NR_exit_group,
		     __NR_rt_sigaction,
		     __NR_rt_sigprocmask,
		     __NR_rt_sigreturn,
		     __NR_sigaltstack,
		     __NR_getpid,
		     __NR_gettid,
		     __NR_sched_yield })
		allow(nr);
#ifdef __NR_fstat
	allow(__NR_fstat);
#endif
#ifdef __NR_fstat64
	allow(__NR_fstat64);
#endif
#ifdef __NR_clock_gettime
	allow(__NR_clock_gettime);
#endif
#ifdef __NR_clock_gettime64
	allow(__NR_clock_gettime64);
#endif
#ifdef __NR_futex_time64
	allow(__NR_futex_time64);
#endif
#ifdef __NR_getrandom
	allow(__NR_getrandom);
#endif
	statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
	sock_fprog program{ static_cast<unsigned short>(code.size()), code.data() };
	return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
	       prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
#endif
}
#endif
} // namespace

bool enter()
{
	struct stat input;
	if (fstat(STDIN_FILENO, &input) != 0 || !S_ISREG(input.st_mode) ||
		(fcntl(STDIN_FILENO, F_GETFL) & O_ACCMODE) != O_RDONLY) {
		std::fprintf(stderr,
			"media-probe sandbox: stdin must be a read-only regular file (errno %d)\n",
			errno);
		return false;
	}
	if (!closeOtherFiles()) {
		std::fprintf(stderr,
			"media-probe sandbox: cannot close inherited descriptors (errno %d)\n",
			errno);
		return false;
	}
#ifdef __linux__
	// s390x has only pointer-argument mmap. Keep it denied and have glibc
	// allocate from its brk heap instead; the same address-space cap still holds.
#if defined(__s390x__) && defined(__GLIBC__)
	if (mallopt(M_MMAP_MAX, 0) == 0) {
		std::fprintf(stderr, "media-probe sandbox: cannot configure bounded brk allocations\n");
		return false;
	}
#endif
	// Do not let another same-user process trace the confined helper for its data.
	if (prctl(PR_SET_DUMPABLE, 0) != 0)
		return false;
	const bool installed = filterSyscalls();
	if (!installed)
		std::fprintf(stderr,
			"media-probe sandbox: seccomp unavailable or installation denied (errno %d)\n",
			errno);
	return installed;
#elif defined(__APPLE__)
	// Applied after dyld/runtime startup. Pre-opened stdin/stdout/stderr are the
	// only file capabilities. No path, network, Mach service or process grants.
	char *error = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	const int rc = sandbox_init("(version 1)(deny default)(allow sysctl-read)", 0, &error);
	if (rc != 0)
		std::fprintf(stderr,
			"media-probe sandbox: Seatbelt installation failed: %.1024s\n",
			error != nullptr ? error : "no diagnostic");
	if (error != nullptr)
		sandbox_free_error(error);
#pragma clang diagnostic pop
	return rc == 0;
#else
	return false;
#endif
}
} // namespace probe::sandbox
#endif
