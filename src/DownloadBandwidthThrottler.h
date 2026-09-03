//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#ifndef DOWNLOADBANDWIDTHTHROTTLER_H
#define DOWNLOADBANDWIDTHTHROTTLER_H

#include "Types.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>

class CEMSocket;

// Global token bucket that enforces thePrefs::GetMaxDownload() as a literal
// byte/sec cap across all downloading sockets.
//
// Mirrors UploadBandwidthThrottler in role, but intentionally simpler:
// download is a PULL model where the kernel tells us when bytes are ready
// (asio OnReceive fires from the I/O thread), so there's no need for a
// dedicated throttler thread + condvar like the upload side. A shared
// atomic budget that every CEMSocket consults before each Read() is
// enough.
//
// Replaces the old per-peer ratio controller (DownloadQueue::Process +
// CUpDownClient::SetDownloadLimit), which never enforced MaxDownload as a
// literal cap and instead nudged each peer's transfer rate by ~5%/tick
// against its own current speed. With a single global bucket, fast peers
// can claim unused capacity from slow peers within the same tick
// (demand-aware redistribution); the only constraint is the global cap.
class CDownloadBandwidthThrottler
{
public:
	static CDownloadBandwidthThrottler &Get();

	// Refill the bucket at the start of each DownloadQueue tick.
	//   maxDownloadKBps == 0  -> unlimited mode (Reserve returns the full
	//                            request, no decrement).
	// Leftover from the previous tick is discarded: the cap is strict, not
	// burst-friendly. A burst-friendly accumulating bucket would let a
	// quiet period bank capacity and overshoot MaxDownload after data
	// resumes -- that's exactly the surprise the PR is trying to remove.
	void RefillBudget(uint32 maxDownloadKBps, uint32 tickPeriodMs);

	// Reserve up to wantBytes from the shared budget. Returns the actual
	// number of bytes the caller may read this round (in [0, wantBytes]).
	// Returning 0 means the bucket is exhausted; the caller should
	// suspend reads (set pendingOnReceive on the socket) and wait for the
	// next refill to wake it.
	uint32 Reserve(uint32 wantBytes);

	// Refund unused bytes to the shared budget. Used when Reserve granted
	// more than Read() actually returned (TCP partial reads, EOF, etc.) so
	// the unused capacity stays available to other peers in the same tick.
	void Refund(uint32 bytes);

	// A socket that suspended its read because Reserve() came back empty,
	// and the counterpart for one that goes away while suspended.
	//
	// The throttler is what stopped these reads, so it is what has to
	// restart them. Leaving that to whoever happens to tick the socket means
	// a socket nobody ticks -- one we are browsing rather than downloading
	// from -- never reads again: its half-received packet never completes,
	// and the peer looks like it answered nothing at all.
	// Registration is on an empty bucket, not on what the socket is doing,
	// so under a saturated cap this also collects sockets a part file will
	// wake anyway. Those get a second, cheap wake per tick (WakeIfPaused()
	// is a bool test when nothing is pending); telling the two apart from
	// here would mean teaching the throttler about download sources.
	void PauseUntilRefill(CEMSocket *socket);
	void Forget(CEMSocket *socket);

	// Restart the reads suspended before this call. Run once per tick after
	// RefillBudget(), and OUTSIDE any lock the read path can reach: waking
	// re-enters CEMSocket::OnReceive(), which parses packets and can run
	// most of the core.
	//
	// Only the sockets already waiting when it starts are woken. A socket
	// that exhausts the refilled bucket suspends again immediately, and
	// waking it a second time in the same pass would achieve nothing but
	// spin the main loop until the next refill, which cannot arrive while
	// that loop is blocked.
	void WakePaused();

private:
	CDownloadBandwidthThrottler() = default;

	std::atomic<int64_t> m_bytesAvailable{ 0 };
	std::atomic<bool> m_unlimited{ true };

	std::mutex m_pausedLock;
	// Suspended, waiting for the next refill.
	std::set<CEMSocket *> m_paused;
	// Being woken by the pass currently running. Held separately so a socket
	// that suspends again lands in m_paused and waits for the next tick,
	// while one that is destroyed mid-pass is dropped from here by Forget()
	// and never handed out.
	std::set<CEMSocket *> m_waking;
};

#endif // DOWNLOADBANDWIDTHTHROTTLER_H
