// SPDX-License-Identifier: GPL-2.0-or-later
// libFuzzer entry point: every parser is reachable through the format dispatch.
// Build with -DMEDIA_PROBE_FUZZ=ON using Clang; run with a seed corpus of media files.
#include "probe.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	probe::MemSource source(data, size);
	probe::Result result;
	probe::probe(source, result); // Malformed input is handled; anything else is a bug.
	return 0;
}
