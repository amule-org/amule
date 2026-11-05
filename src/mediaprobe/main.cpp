// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated media parser: input is an inherited, seekable regular file on stdin.
#include "probe.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <sys/types.h>
static_assert(sizeof(off_t) >= sizeof(int64_t), "Media probe requires 64-bit file offsets");
#endif

namespace
{
int seek(int64_t offset, int whence)
{
#ifdef _WIN32
	return _fseeki64(stdin, offset, whence);
#else
	return fseeko(stdin, offset, whence);
#endif
}

int64_t tell()
{
#ifdef _WIN32
	return _ftelli64(stdin);
#else
	return ftello(stdin);
#endif
}

class StdinSource : public probe::Source
{
public:
	explicit StdinSource(int64_t size)
	: m_size(size)
	{
	}
	int64_t size() const override { return m_size; }
	size_t readAt(int64_t offset, void *buffer, size_t n) override
	{
		if (seek(offset, SEEK_SET))
			return 0;
		return std::fread(buffer, 1, n, stdin);
	}

private:
	int64_t m_size;
};

void quoted(const std::string &s, bool json = true)
{
	std::cout << '"';
	// cleanText() already validates and bounds complete code points.
	for (unsigned char c : s) {
		if (c == '\\' || c == '"')
			std::cout << '\\' << c;
		else if (c == '\n')
			std::cout << "\\n";
		else if (c == '\r')
			std::cout << "\\r";
		else if (c == '\t')
			std::cout << (json ? "\\t" : "\t");
		else if (c >= 32 && c != 127)
			std::cout << c;
	}
	std::cout << '"';
}

uint32_t clamp32(double value)
{
	return value > 0 && std::isfinite(value) ? static_cast<uint32_t>(std::min(value, double(UINT32_MAX)))
						 : 0;
}
} // namespace

int main(int argc, char **argv)
{
	std::setlocale(LC_NUMERIC, "C");
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#else
	rlimit noCore{ 0, 0 };
	setrlimit(RLIMIT_CORE, &noCore);
	rlimit cpu{ 25, 25 };
	setrlimit(RLIMIT_CPU, &cpu);
	// stdout is a regular file the core reads, so 0 would block the reply. Four times the
	// core's 64 KiB reply cap still leaves no room for any real file write.
	rlimit writes{ 256 * 1024, 256 * 1024 };
	setrlimit(RLIMIT_FSIZE, &writes);
#if !defined(__SANITIZE_ADDRESS__)
	// ASan reserves a large shadow address space; ordinary builds have a 1 GiB cap.
#if defined(__has_feature)
#if !__has_feature(address_sanitizer)
	rlimit memory{ 1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024 };
	setrlimit(RLIMIT_AS, &memory);
#endif
#else
	rlimit memory{ 1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024 };
	setrlimit(RLIMIT_AS, &memory);
#endif
#endif
#endif
	bool json = false;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--json")
			json = true;
		else if (arg == "--version") {
			std::cout << "amule-media-probe 1; native\n";
			return 0;
		} else
			return 12;
	}
	if (seek(0, SEEK_END))
		return 12;
	const auto size = tell();
	if (size < 0 || seek(0, SEEK_SET))
		return 12;
	if (!json) {
		// Before parsing: a crash after this line is the file's doing, not a broken install.
		std::cout << "AMMP 1\n" << std::flush;
	}
	probe::Result r;
	int rc = 12;
	try {
		StdinSource source(size);
		rc = probe::probe(source, r);
	} catch (...) {
		return 12;
	}
	if (std::ferror(stdin))
		return 12;
	if (rc)
		return rc;
	// Whole seconds, rounded; the overall bitrate from the exact duration, truncated.
	const uint32_t length = clamp32(std::floor(r.seconds + 0.5));
	const uint32_t bitrate = r.seconds > 0 ? clamp32(double(size) * 8 / r.seconds / 1000) : 0;
	if (json) {
		std::cout << "{\"backend\":";
		quoted("native");
		std::cout << ",\"length_seconds\":" << length << ",\"bitrate_kbps\":" << bitrate;
		for (const auto &item : { std::make_pair("codec", r.codec),
			     { "artist", r.artist },
			     { "album", r.album },
			     { "title", r.title } }) {
			std::cout << ",\"" << item.first << "\":";
			quoted(item.second);
		}
		std::cout << "}\n";
	} else {
		// Versioned, fixed flat vocabulary reuses the core's tested escaping parser.
		std::cout << "format.duration=" << length << "\nformat.bit_rate=" << uint64_t(bitrate) * 1000
			  << '\n';
		std::cout << "streams.stream.0.codec_type=\"audio\"\nstreams.stream.0.codec_name=";
		quoted(r.codec);
		std::cout << '\n';
		for (const auto &item :
			{ std::make_pair("artist", r.artist), { "album", r.album }, { "title", r.title } }) {
			std::cout << "format.tags." << item.first << '=';
			quoted(item.second, false);
			std::cout << '\n';
		}
		std::cout << "END\n";
	}
	return 0;
}
