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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include "AtomicFile.h"

#include <cstdio>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <fstream>
#endif

namespace webcommon
{

bool WriteFileAtomic0600(const std::string &path, const std::string &body)
{
#ifndef _WIN32
	const std::string tmp = path + ".tmp";

	const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return false;
	}
	::fchmod(fd, S_IRUSR | S_IWUSR); // belt+braces against odd umasks

	std::size_t written = 0;
	while (written < body.size()) {
		const ssize_t n = ::write(fd, body.data() + written, body.size() - written);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			::close(fd);
			::unlink(tmp.c_str());
			return false;
		}
		written += static_cast<std::size_t>(n);
	}
	if (::fsync(fd) != 0) {
		::close(fd);
		::unlink(tmp.c_str());
		return false;
	}
	if (::close(fd) != 0) {
		::unlink(tmp.c_str());
		return false;
	}
	if (::rename(tmp.c_str(), path.c_str()) != 0) {
		::unlink(tmp.c_str());
		return false;
	}
	return true;
#else
	// Windows: no permission story to enforce, so this is a plain
	// truncating write. Kept atomic-ish only in the sense that a failed
	// write is reported; callers holding a secret are warned in the
	// header that confidentiality is not provided here.
	std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
	if (!f.is_open()) {
		return false;
	}
	if (!body.empty()) {
		f.write(body.data(), static_cast<std::streamsize>(body.size()));
	}
	f.flush();
	const bool ok = f.good();
	f.close();
	return ok;
#endif
}

} // namespace webcommon
