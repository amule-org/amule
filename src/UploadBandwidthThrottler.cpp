//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "UploadBandwidthThrottler.h"

#include <protocol/ed2k/Constants.h>
#include <common/Macros.h>
#include <common/Constants.h>

#include <cmath>
#include "OtherFunctions.h"
#include "ThrottledSocket.h"
#include "Logger.h"
#include "Preferences.h"
#include "Statistics.h"
#include "amule.h"
#include "UploadDiskIOThread.h"

/////////////////////////////////////

/**
 * The constructor starts the thread.
 */
UploadBandwidthThrottler::UploadBandwidthThrottler()
: wxThread(wxTHREAD_JOINABLE)
, m_newDataCondition(m_newDataMutex)
{
	m_SentBytesSinceLastCall = 0;
	m_SentBytesSinceLastCallOverhead = 0;

	m_doRun = true;

	Create();
	Run();
}

/**
 * The destructor stops the thread, or does nothing if it has already stopped.
 */
UploadBandwidthThrottler::~UploadBandwidthThrottler()
{
	EndThread();
}

/**
 * Called by the disk I/O thread when it has put new data on a socket queue. Wakes the throttler
 * immediately instead of waiting for its next sleep interval. eMule ref:
 * UploadBandwidthThrottler.cpp:795
 */
void UploadBandwidthThrottler::NewUploadDataAvailable()
{
	wxMutexLocker lock(m_newDataMutex);
	m_newDataCondition.Signal();
}

/**
 * Bytes put on the sockets since the last call to this method, including control-packet overhead.
 * Resets the counter.
 */
uint64 UploadBandwidthThrottler::GetNumberOfSentBytesSinceLastCallAndReset()
{
	wxMutexLocker lock(m_sendLocker);

	uint64 numberOfSentBytesSinceLastCall = m_SentBytesSinceLastCall;
	m_SentBytesSinceLastCall = 0;

	return numberOfSentBytesSinceLastCall;
}

/**
 * Bytes put on the sockets since the last call to this method, excluding control-packet overhead.
 * Resets the counter.
 */
uint64 UploadBandwidthThrottler::GetNumberOfSentBytesOverheadSinceLastCallAndReset()
{
	wxMutexLocker lock(m_sendLocker);

	uint64 numberOfSentBytesSinceLastCall = m_SentBytesSinceLastCallOverhead;
	m_SentBytesSinceLastCallOverhead = 0;

	return numberOfSentBytesSinceLastCall;
}

/**
 * Add a socket to the list of sockets that have upload slots. The main thread continuously calls
 * send on these, so they can work off their queues; they are served in list order, so the top
 * socket gets the bandwidth first.
 *
 * Adding a socket several times without removing it in between is possible but should be avoided.
 *
 * @param index where to insert the socket. An index past the end means last.
 * @param socket the socket to add. NULL does nothing.
 */
void UploadBandwidthThrottler::AddToStandardList(uint32 index, ThrottledFileSocket *socket)
{
	if (socket) {
		wxMutexLocker lock(m_sendLocker);

		RemoveFromStandardListNoLock(socket);
		if (index > (uint32)m_StandardOrder_list.size()) {
			index = m_StandardOrder_list.size();
		}

		m_StandardOrder_list.insert(m_StandardOrder_list.begin() + index, socket);
	}
}

/**
 * Remove a socket from the list of sockets that have upload slots. If it was mistakenly added
 * several times, every entry for it is removed. A socket not in the list does nothing.
 */
bool UploadBandwidthThrottler::RemoveFromStandardList(ThrottledFileSocket *socket)
{
	wxMutexLocker lock(m_sendLocker);

	return RemoveFromStandardListNoLock(socket);
}

/**
 * Remove a socket from the list of sockets that have upload slots. NOT THREADSAFE: this is the
 * internal form, which does not take m_sendLocker, so only call it while already holding that lock.
 * A socket not in the list does nothing.
 */
bool UploadBandwidthThrottler::RemoveFromStandardListNoLock(ThrottledFileSocket *socket)
{
	return (EraseFirstValue(m_StandardOrder_list, socket) > 0);
}

/**
 * Notifies the send thread that it should try to call controlpacket send for @a socket.
 *
 * May be called several times for the same socket without a send in between. Duplicates are not
 * filtered: calling Send() again costs less CPU than filtering, since the second call finds the
 * work already done and returns.
 */
void UploadBandwidthThrottler::QueueForSendingControlPacket(ThrottledControlSocket *socket, bool hasSent)
{
	bool wasEmpty = false;
	{
		wxMutexLocker lock(m_tempQueueLocker);

		if (m_doRun) {
			wasEmpty = m_TempControlQueue_list.empty() && m_TempControlQueueFirst_list.empty();
			if (hasSent) {
				m_TempControlQueueFirst_list.push_back(socket);
			} else {
				m_TempControlQueue_list.push_back(socket);
			}
		}
	}

	// Wake the throttler when the temp control queue goes from empty to non-empty. Without the
	// signal its adaptive backoff (extraSleepTime *= 5 per idle tick, capped at 1 s) lets the
	// thread doze through newly-queued packets, adding 5-25 ms of latency to every freshly-
	// built OP_REQUESTPARTS -- which directly caps per-peer download throughput on Windows,
	// where peer ramp is already sensitive to ACK clock jitter.
	//
	// Gating on that transition rather than on every queue add matches the CBatchDrainNotifier
	// pattern: one wake per drain cycle, so bursty enqueues from one SendBlockRequests fan-out
	// coalesce into a single signal. The disk I/O thread already wakes the throttler the same
	// way for file data.
	if (wasEmpty) {
		wxMutexLocker lock(m_newDataMutex);
		m_newDataCondition.Signal();
	}
}

/**
 * Remove @a socket from all lists and queues, making it safe to erase or delete and stopping the
 * main thread calling send() for it.
 */
void UploadBandwidthThrottler::DoRemoveFromAllQueues(ThrottledControlSocket *socket)
{
	if (m_doRun) {
		EraseValue(m_ControlQueue_list, socket);
		EraseValue(m_ControlQueueFirst_list, socket);

		wxMutexLocker lock(m_tempQueueLocker);
		EraseValue(m_TempControlQueue_list, socket);
		EraseValue(m_TempControlQueueFirst_list, socket);
	}
}

void UploadBandwidthThrottler::RemoveFromAllQueues(ThrottledControlSocket *socket)
{
	wxMutexLocker lock(m_sendLocker);

	DoRemoveFromAllQueues(socket);
}

void UploadBandwidthThrottler::RemoveFromAllQueues(ThrottledFileSocket *socket)
{
	wxMutexLocker lock(m_sendLocker);

	if (m_doRun) {
		DoRemoveFromAllQueues(socket);

		RemoveFromStandardListNoLock(socket);
	}
}

/**
 * Make the thread exit. Does not return until the thread has stopped looping, which guarantees it
 * will not touch the CEMSockets afterwards.
 */
void UploadBandwidthThrottler::EndThread()
{
	if (m_doRun) { // do it only once
		{
			wxMutexLocker lock(m_sendLocker);

			// signal the thread to stop looping and exit.
			m_doRun = false;
		}

		Wait();
	}
}

/**
 * The thread method that handles calling send for the individual sockets.
 *
 * Control packets are always tried first. Any bandwidth left over goes to the upload-slot sockets
 * in priority order, until the loop runs out. No upload slot is left without a send for longer than
 * a defined time (two seconds). Always returns 0.
 */
void *UploadBandwidthThrottler::Entry()
{
	const uint32 TIME_BETWEEN_UPLOAD_LOOPS = 1;

	uint64 lastLoopTick = GetTickCount64();
	// Bytes to spend in current cycle. If we spend more this becomes negative and causes a wait next
	// time.
	sint32 bytesToSpend = 0;
	uint32 allowedDataRate;
	uint32 rememberedSlotCounter = 0;
	uint32 extraSleepTime = TIME_BETWEEN_UPLOAD_LOOPS;

	while (m_doRun && !TestDestroy()) {
		uint64 timeSinceLastLoop = GetTickCount64() - lastLoopTick;

		if (thePrefs::GetMaxUpload() == UNLIMITED) {
			// MaxUpload=0 means literal unlimited -- bypass the per-iteration rate cap
			// so SendFileAndControlData() is never throttled.
			allowedDataRate = UNLIMITED_RATE;
		} else {
			allowedDataRate = thePrefs::GetMaxUpload() * 1024;
		}

		uint32 minFragSize = 1300;
		uint32 doubleSendSize =
			minFragSize * 2; // send two packages at a time so they can share an ACK
		if (allowedDataRate < 6 * 1024) {
			minFragSize = 536;
			doubleSendSize = minFragSize; // don't send two packages at a time at very low speeds
						      // to give them a smoother load
		}

		uint64 sleepTime;
		if (bytesToSpend < 1) {
			// We have sent more than allowed in last cycle so we have to wait now
			// until we can send at least 1 byte.
			sleepTime = std::max((-bytesToSpend + 1) * 1000 / allowedDataRate +
						     2, // add 2 ms to allow for rounding inaccuracies
				extraSleepTime);
		} else {
			// We could send at once, but sleep a while to not suck up all cpu
			sleepTime = extraSleepTime;
		}

		if (timeSinceLastLoop < sleepTime) {
			// wxCondition::WaitTimeout in place of eMule's WaitForSingleObject.
			// Wakes early if the disk I/O thread signals NewUploadDataAvailable().
			wxMutexLocker lock(m_newDataMutex);
			m_newDataCondition.WaitTimeout(sleepTime - timeSinceLastLoop);
		}

		// Check after sleep in case the thread has been signaled to end
		if (!m_doRun || TestDestroy()) {
			break;
		}

		const uint64 thisLoopTick = GetTickCount64();
		timeSinceLastLoop = thisLoopTick - lastLoopTick;
		lastLoopTick = thisLoopTick;

		if (timeSinceLastLoop > sleepTime + 2000) {
			AddDebugLogLineN(logGeneral,
				CFormat("UploadBandwidthThrottler: Time since last loop too long. time: %ims "
					"wanted: %ims Max: %ims") %
					timeSinceLastLoop % sleepTime % (sleepTime + 2000));

			timeSinceLastLoop = sleepTime + 2000;
		}

		// Calculate how many bytes we can spend. In UNLIMITED mode allowedDataRate is
		// UINT_MAX (~4 GB/s), which would overflow the sint32 bytesToSpend accumulator once
		// multiplied by timeSinceLastLoop, so the budget rate is capped at 1 GB/s -- still
		// far above any real uplink.
		const uint32 bytesToSpendRate =
			(allowedDataRate == UNLIMITED_RATE) ? (1024u * 1024u * 1024u) : allowedDataRate;
		bytesToSpend += (sint32)(bytesToSpendRate / 1000.0 * timeSinceLastLoop);

		if (bytesToSpend >= 1) {
			sint32 spentBytes = 0;
			sint32 spentOverhead = 0;

			wxMutexLocker sendLock(m_sendLocker);

			{
				wxMutexLocker queueLock(m_tempQueueLocker);

				// are there any sockets in m_TempControlQueue_list? Move them to normal
				// m_ControlQueue_list;
				m_ControlQueueFirst_list.insert(m_ControlQueueFirst_list.end(),
					m_TempControlQueueFirst_list.begin(),
					m_TempControlQueueFirst_list.end());

				m_ControlQueue_list.insert(m_ControlQueue_list.end(),
					m_TempControlQueue_list.begin(),
					m_TempControlQueue_list.end());

				m_TempControlQueue_list.clear();
				m_TempControlQueueFirst_list.clear();
			}

			// Send any queued up control packets first
			while (spentBytes < bytesToSpend &&
				(!m_ControlQueueFirst_list.empty() || !m_ControlQueue_list.empty())) {
				ThrottledControlSocket *socket = NULL;

				if (!m_ControlQueueFirst_list.empty()) {
					socket = m_ControlQueueFirst_list.front();
					m_ControlQueueFirst_list.pop_front();
				} else if (!m_ControlQueue_list.empty()) {
					socket = m_ControlQueue_list.front();
					m_ControlQueue_list.pop_front();
				}

				if (socket != NULL) {
					SocketSentBytes socketSentBytes = socket->SendControlData(
						bytesToSpend - spentBytes, minFragSize);
					spentBytes += socketSentBytes.sentBytesControlPackets +
						      socketSentBytes.sentBytesStandardPackets;
					spentOverhead += socketSentBytes.sentBytesControlPackets;
				}
			}

			// Check if any sockets haven't gotten data for a long time. Then trickle them a
			// package.
			uint32 slots = m_StandardOrder_list.size();
			for (uint32 slotCounter = 0; slotCounter < slots; slotCounter++) {
				ThrottledFileSocket *socket = m_StandardOrder_list[slotCounter];

				if (socket != NULL) {
					if (thisLoopTick - socket->GetLastCalledSend() > SEC2MS(1)) {
						// trickle
						uint32 neededBytes = socket->GetNeededBytes();

						if (neededBytes > 0) {
							SocketSentBytes socketSentBytes =
								socket->SendFileAndControlData(
									neededBytes, minFragSize);
							spentBytes +=
								socketSentBytes.sentBytesControlPackets +
								socketSentBytes.sentBytesStandardPackets;
							spentOverhead +=
								socketSentBytes.sentBytesControlPackets;
						}
					}
				} else {
					AddDebugLogLineN(logGeneral,
						CFormat("There was a NULL socket in the "
							"UploadBandwidthThrottler Standard list (trickle)! "
							"Prevented usage. Index: %i Size: %i") %
							slotCounter % m_StandardOrder_list.size());
				}
			}

			// Give available bandwidth to slots, starting with the one we ended with
			// last time. Two passes: the first gives packets of doubleSendSize, the
			// second as much as possible, starting from the last slot of the first
			// pass.
			for (uint32 slotCounter = 0; (slotCounter < slots * 2) && spentBytes < bytesToSpend;
				slotCounter++) {
				if (rememberedSlotCounter >= slots) { // wrap around pointer
					rememberedSlotCounter = 0;
				}

				uint32 data = (slotCounter < slots - 1)
						      ? doubleSendSize               // pass 1
						      : (bytesToSpend - spentBytes); // pass 2

				ThrottledFileSocket *socket = m_StandardOrder_list[rememberedSlotCounter];

				if (socket != NULL) {
					SocketSentBytes socketSentBytes =
						socket->SendFileAndControlData(data, doubleSendSize);
					spentBytes += socketSentBytes.sentBytesControlPackets +
						      socketSentBytes.sentBytesStandardPackets;
					spentOverhead += socketSentBytes.sentBytesControlPackets;
				} else {
					AddDebugLogLineN(logGeneral,
						CFormat("There was a NULL socket in the "
							"UploadBandwidthThrottler Standard list "
							"(equal-for-all)! Prevented usage. Index: %i Size: "
							"%i") %
							rememberedSlotCounter % m_StandardOrder_list.size());
				}

				rememberedSlotCounter++;
			}

			// Do some limiting of what we keep for the next loop.
			bytesToSpend -= spentBytes;
			sint32 minBytesToSpend = (slots + 1) * minFragSize;

			if (bytesToSpend < -minBytesToSpend) {
				bytesToSpend = -minBytesToSpend;
			} else {
				sint32 bandwidthSavedTolerance = slots * 512 + 1;
				if (bytesToSpend > bandwidthSavedTolerance) {
					bytesToSpend = bandwidthSavedTolerance;
				}
			}

			m_SentBytesSinceLastCall += spentBytes;
			m_SentBytesSinceLastCallOverhead += spentOverhead;

			if (spentBytes == 0) { // spentBytes includes the overhead
				extraSleepTime = std::min<uint32>(extraSleepTime * 5, 1000); // 1s at most
			} else {
				extraSleepTime = TIME_BETWEEN_UPLOAD_LOOPS;

				// eMule ref: EMSocket.cpp:602 -- SocketAvailable(). Wake the disk
				// I/O thread whenever payload bytes were actually sent, so it can
				// refill the socket queue without waiting for its 100 ms
				// WaitTimeout.
				if (spentBytes > spentOverhead && theApp->uploadDiskIOThread != NULL) {
					theApp->uploadDiskIOThread->SocketNeedsMoreData();
				}
			}
		}
	}

	{
		wxMutexLocker queueLock(m_tempQueueLocker);
		m_TempControlQueue_list.clear();
		m_TempControlQueueFirst_list.clear();
	}

	wxMutexLocker sendLock(m_sendLocker);
	m_ControlQueue_list.clear();
	m_StandardOrder_list.clear();

	return 0;
}
// File_checked_for_headers
