// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production AVIO callbacks without replacing the parser library.
#define main mediaProbeMain
#include "main.cpp"
#undef main

static int failure(int checkpoint)
{
	std::fprintf(stderr, "MediaProbeIOLimits failed at checkpoint %d\n", checkpoint);
	return checkpoint;
}

int main()
{
	// test_io.py supplies a seekable file explicitly, including on native Windows.
	Input input;
	input.size = 4096;
	uint8_t byte = 1;
	if (readInput(&input, &byte, 1) != 1 || byte != 0)
		return failure(15);
	if (seekInput(&input, -1, SEEK_SET) != AVERROR(EINVAL) ||
		seekInput(&input, 4097, SEEK_SET) != AVERROR(EINVAL))
		return failure(1);
	if (seekInput(&input, 0, AVSEEK_SIZE) != 4096)
		return failure(2);
	input.bytes = MaxRead;
	if (readInput(&input, &byte, 1) != AVERROR(EFBIG) || !input.exhausted)
		return failure(3);
	Input expired;
	expired.deadline = Clock::now() - std::chrono::seconds(1);
	if (readInput(&expired, &byte, 1) != AVERROR_EXIT || !interrupted(&expired))
		return failure(4);
	Input seeking;
	seeking.size = 4096;
	if (reposition(0, SEEK_SET))
		return failure(16);
	for (int64_t pos = 1; pos <= 1024; ++pos) {
		if (seekInput(&seeking, pos, SEEK_SET) != pos || seeking.exhausted)
			return failure(5);
	}
	for (unsigned i = 0; i < MaxBackwardSeeks; ++i) {
		if (seekInput(&seeking, 0, SEEK_SET) != 0)
			return failure(6);
	}
	if (seekInput(&seeking, 0, SEEK_SET) != AVERROR(EFBIG) || !seeking.exhausted)
		return failure(7);
	if (denyOpen(nullptr, nullptr, "file:/unavailable", 0, nullptr) != AVERROR(EACCES))
		return failure(8);
#ifdef _WIN32
	_close(_fileno(stdin));
#else
	close(fileno(stdin));
#endif
	std::clearerr(stdin);
	Input failedRead;
	failedRead.size = 4096;
	Result result;
	// The core treats exit 12 as retryable, retaining preview metadata and
	// avoiding a permanent known.met failure marker after a real read error.
	if (probe(failedRead, result) != 12)
		return failure(9);
	return 0;
}
