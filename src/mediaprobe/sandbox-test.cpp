// SPDX-License-Identifier: GPL-2.0-or-later
// Native denial tests use the very same sandbox as the production helper.
#include "sandbox.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return 12;
	// Load the provider while startup permissions are still available. Denial
	// must hold even when socket code is already resident, not just fail to load.
	const SOCKET warm = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (warm == INVALID_SOCKET)
		return 12;
	closesocket(warm);
	const bool child = argc > 1 && std::string(argv[argc - 1]) == "--sandbox-child";
	if (child)
		--argc;
	else if (argc == 3 && std::string(argv[1]) == "launcher-setup-failure") {
		// Add an outer restriction that prevents the trusted launcher from
		// creating its private desktop. Exercise a real setup failure without
		// changing the production policy or allowing the parser to start.
		const HANDLE job = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_BASIC_UI_RESTRICTIONS ui{ JOB_OBJECT_UILIMIT_DESKTOP };
		if (job == nullptr || !AssignProcessToJobObject(job, GetCurrentProcess()) ||
			!SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui))) {
			std::fprintf(stderr,
				"sandbox test: cannot constrain launcher desktop (Windows error %lu)\n",
				GetLastError());
			if (job != nullptr)
				CloseHandle(job);
			return 99;
		}
		const int rc = probe::sandbox::launch();
		CloseHandle(job);
		return rc;
	} else
		return probe::sandbox::launch();
#endif
	if (argc != 3)
		return 12;
	const std::string action = argv[1];
	const std::string path = argv[2];
	// Warm stdio up without reading media, as main.cpp does before confinement.
	if (std::fseek(stdin, 0, SEEK_END) || std::fseek(stdin, 0, SEEK_SET) || !probe::sandbox::enter())
		return 12;
	bool blocked = false;
	if (action == "read-input") {
		char input[4];
		return std::fread(input, 1, 4, stdin) == 4 && std::memcmp(input, "RIFF", 4) == 0 &&
				       std::fseek(stdin, 0, SEEK_SET) == 0
			       ? 0
			       : 1;
	}
	if (action == "vectored") {
#ifndef _WIN32
		char input[4] = {};
		iovec reads[] = { { input, 2 }, { input + 2, 2 } };
		char message[] = "vector ok\n";
		iovec writes[] = { { message, 7 }, { message + 7, 3 } };
#ifdef __linux__
		if (readv(STDOUT_FILENO, reads, 2) != -1 || errno != EPERM ||
			writev(STDIN_FILENO, writes, 2) != -1 || errno != EPERM)
			return 1;
#endif
		return readv(STDIN_FILENO, reads, 2) == 4 && std::memcmp(input, "RIFF", 4) == 0 &&
				       writev(STDOUT_FILENO, writes, 2) == 10
			       ? 0
			       : 1;
#endif
	} else if (action == "write-input") {
#ifdef _WIN32
		DWORD written = 0;
		blocked = !WriteFile(GetStdHandle(STD_INPUT_HANDLE), "X", 1, &written, nullptr) &&
			  GetLastError() == ERROR_ACCESS_DENIED;
#endif
	} else if (action == "resize-output") {
#ifdef _WIN32
		blocked = !SetEndOfFile(GetStdHandle(STD_OUTPUT_HANDLE));
#endif
	} else if (action == "overflow-output" || action == "overflow-errors") {
#ifdef _WIN32
		const auto handle =
			GetStdHandle(action == "overflow-output" ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
		const char bytes[4096] = {};
		for (unsigned i = 0; i < 256; ++i) {
			DWORD written = 0;
			if (!WriteFile(handle, bytes, sizeof(bytes), &written, nullptr) ||
				written != sizeof(bytes))
				return 1;
		}
		return 0;
#endif
	} else if (action == "read-file" || action == "write-file") {
		FILE *file = std::fopen(path.c_str(), action == "read-file" ? "rb" : "wb");
		blocked = file == nullptr;
		if (file != nullptr)
			std::fclose(file);
	} else if (action == "network" || action == "network-listen") {
		const auto fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
		blocked = fd == INVALID_SOCKET;
		const bool opened = !blocked;
#else
		blocked = fd < 0 && (errno == EPERM || errno == EACCES);
		const bool opened = fd >= 0;
#endif
		if (opened) {
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_port = htons(static_cast<unsigned short>(std::atoi(path.c_str())));
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef __APPLE__
			address.sin_len = static_cast<unsigned char>(sizeof(address));
#endif
			int rc;
			if (action == "network-listen") {
				rc = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
				if (rc == 0)
					rc = listen(fd, 1);
			} else {
				rc = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
			}
#ifdef _WIN32
			blocked = rc == SOCKET_ERROR && WSAGetLastError() == WSAEACCES;
			closesocket(fd);
#else
			blocked = rc < 0 && (errno == EPERM || errno == EACCES);
			close(fd);
#endif
		}
	} else if (action == "process") {
#ifdef _WIN32
		STARTUPINFOA info{};
		info.cb = sizeof(info);
		PROCESS_INFORMATION process{};
		blocked = !CreateProcessA(path.c_str(),
			nullptr,
			nullptr,
			nullptr,
			FALSE,
			CREATE_SUSPENDED | CREATE_NO_WINDOW,
			nullptr,
			nullptr,
			&info,
			&process);
		if (!blocked) {
			TerminateProcess(process.hProcess, 1);
			CloseHandle(process.hProcess);
			CloseHandle(process.hThread);
		}
#else
		const pid_t pid = fork();
		if (pid == 0)
			_exit(99);
		blocked = pid < 0;
#endif
	} else if (action == "exec") {
#ifndef _WIN32
		execl(path.c_str(), path.c_str(), nullptr);
		blocked = true;
#endif
	} else if (action == "inherited") {
#ifndef _WIN32
		// Test closure directly. Some 32-bit libc fstat wrappers use path-based
		// stat syscalls, which remain denied even for an already-closed descriptor.
		blocked = close(std::atoi(path.c_str())) < 0 && errno == EBADF;
#endif
	} else if (action == "unix-network") {
#ifndef _WIN32
		const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		blocked = fd < 0 && (errno == EPERM || errno == EACCES);
		if (fd >= 0) {
			sockaddr_un address{};
			address.sun_family = AF_UNIX;
			if (path.size() >= sizeof(address.sun_path))
				return 12;
			std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
#ifdef __APPLE__
			address.sun_len = static_cast<unsigned char>(sizeof(address));
#endif
			blocked = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 &&
				  (errno == EPERM || errno == EACCES);
			close(fd);
		}
#endif
	} else if (action == "openat") {
#ifdef __linux__
		const long fd = syscall(__NR_openat, AT_FDCWD, path.c_str(), O_RDONLY, 0);
		blocked = fd < 0;
		if (!blocked)
			close(static_cast<int>(fd));
#endif
	} else if (action == "map-file" || action == "map-executable") {
#ifdef __linux__
		const int protection = action == "map-executable" ? PROT_READ | PROT_EXEC : PROT_READ;
		const int flags = action == "map-executable" ? MAP_PRIVATE | MAP_ANONYMOUS : MAP_PRIVATE;
		// Inspect the kernel result: musl translates an anonymous mmap's EPERM
		// into ENOMEM. Calling the syscall avoids confusing that with no denial.
#if defined(__s390x__)
		unsigned long arguments[] = { 0,
			4096,
			static_cast<unsigned long>(protection),
			static_cast<unsigned long>(flags),
			STDIN_FILENO,
			0 };
		const long mapped = syscall(__NR_mmap, arguments);
#elif defined(__NR_mmap2)
		const long mapped = syscall(__NR_mmap2, nullptr, 4096, protection, flags, STDIN_FILENO, 0);
#else
		const long mapped = syscall(__NR_mmap, nullptr, 4096, protection, flags, STDIN_FILENO, 0);
#endif
		blocked = mapped == -1 && errno == EPERM;
		if (mapped != -1)
			munmap(reinterpret_cast<void *>(static_cast<uintptr_t>(mapped)), 4096);
#endif
	} else if (action == "allocate") {
		// Catch a filter that accidentally prevents ordinary parser allocations.
		std::string memory(4 * 1024 * 1024, 'x');
		return memory.back() == 'x' ? 0 : 1;
	}
	if (!blocked)
		std::fprintf(stderr,
			"sandbox test: %s did not produce the expected denial (errno %d)\n",
			action.c_str(),
			errno);
	std::fputs(blocked ? "blocked\n" : "UNRESTRICTED\n", stdout);
	return blocked ? 0 : 1;
}
