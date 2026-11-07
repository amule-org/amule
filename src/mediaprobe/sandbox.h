// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>

namespace probe::sandbox
{
inline constexpr std::size_t kMemoryLimitBytes = 1024ULL * 1024 * 1024;
inline constexpr std::size_t kOutputFileLimitBytes = 256 * 1024;
inline constexpr std::size_t kReplyLimitBytes = 64 * 1024;
inline constexpr std::size_t kDiagnosticLimitBytes = 8 * 1024;
inline constexpr std::size_t kLoggedDiagnosticLimitBytes = 2 * 1024;
inline constexpr unsigned kTimeoutMs = 30000;
inline constexpr int kSetupFailed = 13;

// Call after trusted runtime initialization, before the protocol header or any
// media reads. Failure is infrastructure failure, never a file verdict.
bool enter();

#ifdef _WIN32
// Windows needs a restricted primary token at process creation. The outer helper
// only launches/waits; the inner helper drops its startup impersonation token.
int launch();
#endif
} // namespace probe::sandbox
