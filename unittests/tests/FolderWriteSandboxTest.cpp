// This file is part of aMule. Licensed under GPL-2.0-or-later.
#include "FolderWriteSandbox.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

int main()
{
#ifdef __linux__
	char base[] = "/tmp/amule-folder-write-XXXXXX";
	if (!mkdtemp(base)) {
		return 1;
	}
	const std::string root(base);
	const std::string allowed = root + "/downloads";
	const std::string readonly = root + "/shares";
	mkdir(allowed.c_str(), 0700);
	mkdir(readonly.c_str(), 0700);
	const std::string privateFile = readonly + "/budget.xlsx";
	{
		std::ofstream file(privateFile);
		file << "private document";
	}
	// Files opened before restriction are outside Landlock's guarantees. The app
	// applies the policy before opening downloads or shared files.
	const pid_t child = fork();
	if (child == 0) {
		std::string error;
		if (!FolderWriteSandbox::Restrict({ allowed }, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			_exit(77);
		}
		auto denied = [](int result) { return result == -1 && errno == EACCES; };
		int fd = open(privateFile.c_str(), O_RDONLY);
		if (fd < 0) {
			_exit(2);
		}
		close(fd);
		if (!denied(open(privateFile.c_str(), O_WRONLY))) {
			_exit(3);
		}
		if (!denied(truncate(privateFile.c_str(), 0))) {
			_exit(4);
		}
		if (!denied(unlink(privateFile.c_str()))) {
			_exit(5);
		}
		if (!denied(open((readonly + "/new").c_str(), O_CREAT | O_WRONLY, 0600))) {
			_exit(6);
		}
		if (!denied(rename(privateFile.c_str(), (allowed + "/stolen.xlsx").c_str()))) {
			_exit(7);
		}
		fd = open((allowed + "/download.part").c_str(), O_CREAT | O_WRONLY, 0600);
		if (fd < 0 || write(fd, "data", 4) != 4) {
			_exit(8);
		}
		close(fd);
		if (rename((allowed + "/download.part").c_str(), (allowed + "/completed").c_str()) != 0) {
			_exit(9);
		}
		if (!denied(symlink(privateFile.c_str(), (readonly + "/link").c_str()))) {
			_exit(10);
		}
		// A symlink placed in a writable folder does not grant write access to its target.
		if (symlink(privateFile.c_str(), (allowed + "/link").c_str()) != 0) {
			_exit(11);
		}
		if (!denied(open((allowed + "/link").c_str(), O_WRONLY))) {
			_exit(12);
		}
		if (!denied(chmod(privateFile.c_str(), 0777))) {
			_exit(13);
		}
		if (!denied(setxattr(privateFile.c_str(), "user.test", "changed", 7, 0))) {
			_exit(14);
		}
		_exit(0);
	}
	int status = 0;
	if (child < 0 || waitpid(child, &status, 0) != child) {
		return 1;
	}
	unlink((allowed + "/completed").c_str());
	unlink((allowed + "/link").c_str());
	unlink(privateFile.c_str());
	rmdir(allowed.c_str());
	rmdir(readonly.c_str());
	rmdir(root.c_str());
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#else
	return 77;
#endif
}
