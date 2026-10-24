// This file is part of aMule. Licensed under GPL-2.0-or-later.
#include "FolderWriteSandbox.h"

#ifdef __linux__
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#endif
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <cstddef>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef __linux__
namespace
{
// Landlock does not mediate chmod/chown/timestamps/xattrs. Deny explicit
// metadata mutations after startup, including io_uring equivalents. Ordinary
// creation, content writes and automatic mtime updates remain available in
// writable roots. Reject compat ABIs so they cannot bypass the syscall filter.
bool RestrictMetadata(std::string &error)
{
#if defined(__x86_64__) || defined(__aarch64__)
#ifdef __x86_64__
	const unsigned architecture = AUDIT_ARCH_X86_64;
#else
	const unsigned architecture = AUDIT_ARCH_AARCH64;
#endif
	std::vector<sock_filter> filter = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, architecture, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES) };
	const unsigned blocked[] = {
#ifdef __NR_chmod
		__NR_chmod,
#endif
#ifdef __NR_chown
		__NR_chown,
#endif
#ifdef __NR_lchown
		__NR_lchown,
#endif
#ifdef __NR_utime
		__NR_utime,
#endif
#ifdef __NR_utimes
		__NR_utimes,
#endif
#ifdef __NR_futimesat
		__NR_futimesat,
#endif
		__NR_fchmod,
		__NR_fchmodat,
		__NR_fchown,
		__NR_fchownat,
		__NR_utimensat,
		__NR_setxattr,
		__NR_lsetxattr,
		__NR_fsetxattr,
		__NR_removexattr,
		__NR_lremovexattr,
		__NR_fremovexattr,
		// These syscall numbers are shared by x86_64 and aarch64, including
		// builds made with older SDK headers running on a newer host kernel.
		452, // fchmodat2
		463, // setxattrat
		466, // removexattrat
		469, // file_setattr
		425,
		426,
		427 // io_uring_setup/enter/register
	};
	for (const unsigned operation : blocked) {
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, operation, 0, 1));
		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES));
	}
	filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
	const sock_fprog program = { static_cast<unsigned short>(filter.size()), filter.data() };
	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
		error = std::strerror(errno);
		return false;
	}
	return true;
#else
	error = "Read-only folder metadata protection requires x86_64 or aarch64.";
	return false;
#endif
}
} // namespace
#endif

bool FolderWriteSandbox::Restrict(const std::vector<std::string> &writableRoots, std::string &error)
{
#if defined(__linux__) && defined(SYS_landlock_create_ruleset) && defined(LANDLOCK_ACCESS_FS_TRUNCATE)
	const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 3) {
		error = "Read-only folder access requires Linux Landlock ABI 3 or newer (Linux 6.2+).";
		return false;
	}
	const uint64_t mutations = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
				   LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
				   LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
				   LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
				   LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM |
				   LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
	landlock_ruleset_attr ruleset = {};
	ruleset.handled_access_fs = mutations;
	// This operation returns an int file descriptor, or -1 on failure.
	const int rules =
		static_cast<int>(syscall(SYS_landlock_create_ruleset, &ruleset, sizeof(ruleset), 0));
	if (rules < 0) {
		error = std::strerror(errno);
		return false;
	}
	bool ok = true;
	for (const auto &root : writableRoots) {
		const int path = open(root.c_str(), O_PATH | O_CLOEXEC);
		if (path < 0) {
			// Offline/revoked destinations remain configured but receive no write rule.
			if (errno == ENOENT || errno == EACCES) {
				continue;
			}
			error = root + ": " + std::strerror(errno);
			ok = false;
			break;
		}
		struct stat status = {};
		if (fstat(path, &status) != 0) {
			error = std::strerror(errno);
			close(path);
			ok = false;
			break;
		}
		landlock_path_beneath_attr rule = {};
		rule.parent_fd = path;
		rule.allowed_access = S_ISDIR(status.st_mode)
					      ? mutations
					      : (LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE);
		if (syscall(SYS_landlock_add_rule, rules, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) != 0) {
			error = root + ": " + std::strerror(errno);
			ok = false;
		}
		close(path);
		if (!ok) {
			break;
		}
	}
	if (ok && (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
			  syscall(SYS_landlock_restrict_self, rules, 0) != 0)) {
		error = std::strerror(errno);
		ok = false;
	}
	close(rules);
	return ok && RestrictMetadata(error);
#else
	(void)writableRoots;
	error = "This build does not support Linux Landlock filesystem restrictions.";
	return false;
#endif
}
