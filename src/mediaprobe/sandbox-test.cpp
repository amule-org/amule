// SPDX-License-Identifier: GPL-2.0-or-later
// Native denial tests use the very same sandbox as the production helper.
#include "sandbox.h"
#include <cerrno>
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
#include <sys/socket.h>
#include <sys/stat.h>
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
	const bool child = argc > 1 && std::string(argv[argc - 1]) == "--sandbox-child";
	if (child)
		--argc;
	else
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
	if (action == "read-file" || action == "write-file") {
		FILE *file = std::fopen(path.c_str(), action == "read-file" ? "rb" : "wb");
		blocked = file == nullptr;
		if (file != nullptr)
			std::fclose(file);
	} else if (action == "network") {
#ifdef _WIN32
		const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		blocked = socket == INVALID_SOCKET;
		if (!blocked)
			closesocket(socket);
#else
		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		blocked = fd < 0;
		if (!blocked)
			close(fd);
#endif
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
		struct stat info;
		blocked = fstat(std::atoi(path.c_str()), &info) < 0 && errno == EBADF;
#endif
	} else if (action == "unix-network") {
#ifndef _WIN32
		const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		blocked = fd < 0;
		if (!blocked)
			close(fd);
#endif
	} else if (action == "openat") {
#ifdef __linux__
		const long fd = syscall(__NR_openat, AT_FDCWD, path.c_str(), O_RDONLY, 0);
		blocked = fd < 0;
		if (!blocked)
			close(static_cast<int>(fd));
#endif
	} else if (action == "allocate") {
		// Catch a filter that accidentally prevents ordinary parser allocations.
		std::string memory(4 * 1024 * 1024, 'x');
		return memory.back() == 'x' ? 0 : 1;
	}
	std::fputs(blocked ? "blocked\n" : "UNRESTRICTED\n", stdout);
	return blocked ? 0 : 1;
}
