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

#include "EMSocket.h" // Interface declarations.

#include <protocol/Protocols.h>
#include <protocol/ed2k/Constants.h>

#include "Packet.h" // Needed for CPacket
#include "amule.h"
#include "DownloadBandwidthThrottler.h"
#include "GetTickCount.h"
#include "UploadBandwidthThrottler.h"
#include "Logger.h"
#include "Preferences.h"
#include "ScopedPtr.h"

const uint32 MAX_PACKET_SIZE = 2000000;

// cppcheck-suppress uninitMemberVar CEMSocket::pendingHeader
CEMSocket::CEMSocket(const CProxyData *ProxyData)
: CEncryptedStreamSocket(MULE_SOCKET_NOWAIT, ProxyData)
{
	// If an interface has been specified,
	// then we need to bind to it.
	if (!thePrefs::GetAddress().IsEmpty()) {
		amuleIPV4Address host;

		// No need to warn on a failure to assign the hostname; amule.cpp already does at
		// startup.
		if (host.Hostname(thePrefs::GetAddress())) {
			SetLocal(host);
		}
	}

	byConnected = ES_NOTCONNECTED;
	m_uTimeOut = CONNECTION_TIMEOUT; // default timeout for ed2k sockets

	// Download rate control: the bucket is global (CDownloadBandwidthThrottler); only the per-
	// socket pause flag lives here.
	pendingOnReceive = false;

	// Download partial header
	pendingHeaderSize = 0;

	// Download partial packet
	pendingPacket = NULL;
	pendingPacketSize = 0;

	// Upload control
	sendbuffer = NULL;
	sendblen = 0;
	sent = 0;

	m_currentPacket_is_controlpacket = false;
	m_currentPackageIsFromPartFile = false;

	m_numberOfSentBytesCompleteFile = 0;
	m_numberOfSentBytesPartFile = 0;
	m_numberOfSentBytesControlPacket = 0;

	const uint64 now = ::GetTickCount64();
	lastCalledSend = now;
	lastSent = now - 1000;

	m_bAccelerateUpload = false;

	m_actualPayloadSize = 0;
	m_actualPayloadSizeSent = 0;

	m_bBusy = false;
	m_hasSent = false;

	lastFinishedStandard = 0;
}

CEMSocket::~CEMSocket()
{
	// need to be locked here to know that the other methods
	// won't be in the middle of things
	{
		std::lock_guard<std::mutex> lock(m_sendLocker);
		byConnected = ES_DISCONNECTED;
	}

	// now that we know no other method will keep adding to the queue
	// we can remove ourself from the queue
	if (theApp->uploadBandwidthThrottler) {
		theApp->uploadBandwidthThrottler->RemoveFromAllQueues(this);
	}
	CDownloadBandwidthThrottler::Get().Forget(this);

	ClearQueues();
}

void CEMSocket::ClearQueues()
{
	std::lock_guard<std::mutex> lock(m_sendLocker);

	DeleteContents(m_control_queue);

	{
		CStdPacketQueue::iterator it = m_standard_queue.begin();
		for (; it != m_standard_queue.end(); ++it) {
			delete it->packet;
		}
		m_standard_queue.clear();
	}

	// Download rate control: see header.
	pendingOnReceive = false;

	// Download partial header
	pendingHeaderSize = 0;

	// Download partial packet
	delete[] pendingPacket;
	pendingPacket = NULL;
	pendingPacketSize = 0;

	// Upload control
	delete[] sendbuffer;
	sendbuffer = NULL;
	sendblen = 0;
	sent = 0;
}

void CEMSocket::OnClose(int WXUNUSED(nErrorCode))
{
	// need to be locked here to know that the other methods
	// won't be in the middle of things
	{
		std::lock_guard<std::mutex> lock(m_sendLocker);
		byConnected = ES_DISCONNECTED;
	}

	// now that we know no other method will keep adding to the queue
	// we can remove ourself from the queue
	theApp->uploadBandwidthThrottler->RemoveFromAllQueues(this);
	CDownloadBandwidthThrottler::Get().Forget(this);

	ClearQueues();
}

void CEMSocket::OnReceive(int nErrorCode)
{
	if (nErrorCode) {
		if (LastError()) {
			OnError(nErrorCode);
			return;
		}
	}

	if (byConnected == ES_DISCONNECTED) {
		return;
	} else {
		byConnected = ES_CONNECTED; // ES_DISCONNECTED, ES_NOTCONNECTED, ES_CONNECTED
	}

	uint32 ret;
	do {
		uint32 readMax;
		uint8_t *buf;
		if (pendingHeaderSize < PACKET_HEADER_SIZE) {
			delete[] pendingPacket;
			pendingPacket = NULL;
			buf = pendingHeader + pendingHeaderSize;
			readMax = PACKET_HEADER_SIZE - pendingHeaderSize;
		} else if (pendingPacket == NULL) {
			pendingPacketSize = 0;
			readMax = CPacket::GetPacketSizeFromHeader(pendingHeader);
			if (readMax > MAX_PACKET_SIZE) {
				pendingHeaderSize = 0;
				OnError(ERR_TOOBIG);
				return;
			}
			pendingPacket = new uint8_t[readMax + 1];
			buf = pendingPacket;
		} else {
			buf = pendingPacket + pendingPacketSize;
			readMax = CPacket::GetPacketSizeFromHeader(pendingHeader) - pendingPacketSize;
		}

		// Reserve from the global download budget only when we actually intend to read
		// something. readMax can legitimately be 0 here for empty-payload packets, and the
		// do-while iteration still has to fall through to the packet-processing block
		// below, so Reserve(0) must not short-circuit that path. Sockets that opt out via
		// IsDownloadThrottled() (server control sockets) skip the reservation and do not
		// count against the cap.
		const bool throttled = IsDownloadThrottled();
		uint32 grantedBytes = 0;
		ret = 0;
		if (readMax) {
			if (throttled) {
				grantedBytes = CDownloadBandwidthThrottler::Get().Reserve(readMax);
				if (grantedBytes == 0) {
					// Bucket exhausted; resume on the next tick refill.
					// Register for that wake-up rather than relying on
					// something else to tick this socket: a socket we are only
					// browsing belongs to no download, so nothing else would.
					pendingOnReceive = true;
					CDownloadBandwidthThrottler::Get().PauseUntilRefill(this);
					return;
				}
				readMax = grantedBytes;
			}

			std::lock_guard<std::mutex> lock(m_sendLocker);
			ret = Read(buf, readMax);
			if (BlocksRead()) {
				if (throttled) {
					CDownloadBandwidthThrottler::Get().Refund(grantedBytes);
				}
				pendingOnReceive = true;
				return;
			}
			if (LastError() || ret == 0) {
				if (throttled) {
					CDownloadBandwidthThrottler::Get().Refund(grantedBytes);
				}
				return;
			}
		}

		// Refund the slice we reserved but didn't actually read so the
		// leftover stays available to other peers in the same tick.
		if (throttled && grantedBytes > (uint32)ret) {
			CDownloadBandwidthThrottler::Get().Refund(grantedBytes - ret);
		}

		// CPU load improvement
		// Detect if the socket's buffer is empty (or the size did match...)
		pendingOnReceive = (ret == readMax);

		if (pendingHeaderSize >= PACKET_HEADER_SIZE) {
			pendingPacketSize += ret;
			if (pendingPacketSize >= CPacket::GetPacketSizeFromHeader(pendingHeader)) {
				CScopedPtr<CPacket> packet(new CPacket(pendingHeader, pendingPacket));
				pendingPacket = NULL;
				pendingPacketSize = 0;
				pendingHeaderSize = 0;

				// Bugfix We still need to check for a valid protocol
				// Remark: the default eMule v0.26b had removed this test......
				switch (packet->GetProtocol()) {
				case OP_EDONKEYPROT:
				case OP_PACKEDPROT:
				case OP_EMULEPROT:
				case OP_ED2KV2HEADER:
				case OP_ED2KV2PACKEDPROT:
					break;
				default:
					OnError(ERR_WRONGHEADER);
					return;
				}

				PacketReceived(packet.get());
			}
		} else {
			pendingHeaderSize += ret;
		}
	} while (ret && pendingHeaderSize >= PACKET_HEADER_SIZE);
}

void CEMSocket::WakeIfPaused()
{
	if (pendingOnReceive) {
		// Re-enter the read loop. OnReceive() consults the global
		// CDownloadBandwidthThrottler for fresh budget; if the bucket is still empty,
		// pendingOnReceive stays set and we retry next tick.
		OnReceive(0);
	}
}

/**
 * Queues the packet up to be sent; another thread does the sending.
 *
 * A non-control packet may be refused when the socket decides its queue is full and @a forceAdd is
 * false; the caller then has to try again later.
 *
 * @param packet the packet to add to the queue.
 * @param delpacket true transfers responsibility for deleting the packet once sent.
 * @param controlpacket the packet is a control packet.
 * @param forceAdd add the packet even if the queue is full, so the call cannot refuse.
 * @return true if the packet was added to the queue.
 */
void CEMSocket::SendPacket(CPacket *packet, bool delpacket, bool controlpacket, uint32 actualPayloadSize)
{
	std::lock_guard<std::mutex> lock(m_sendLocker);

	if (byConnected == ES_DISCONNECTED) {
		if (delpacket) {
			delete packet;
		}
	} else {
		if (!delpacket) {
			packet = new CPacket(*packet);
		}

		if (controlpacket) {
			m_control_queue.push_back(packet);

			// queue up for controlpacket
			theApp->uploadBandwidthThrottler->QueueForSendingControlPacket(this, HasSent());
		} else {
			bool first = !((sendbuffer && !m_currentPacket_is_controlpacket) ||
				       !m_standard_queue.empty());
			StandardPacketQueueEntry queueEntry = { actualPayloadSize, packet };
			m_standard_queue.push_back(queueEntry);

			// reset timeout for the first time
			if (first) {
				lastFinishedStandard = ::GetTickCount64();
				m_bAccelerateUpload = true; // Always accelerate first packet in a block
			}
		}
	}
}

uint64 CEMSocket::GetSentBytesCompleteFileSinceLastCallAndReset()
{
	std::lock_guard<std::mutex> lock(m_sendLocker);

	uint64 sentBytes = m_numberOfSentBytesCompleteFile;
	m_numberOfSentBytesCompleteFile = 0;

	return sentBytes;
}

uint64 CEMSocket::GetSentBytesPartFileSinceLastCallAndReset()
{
	std::lock_guard<std::mutex> lock(m_sendLocker);

	uint64 sentBytes = m_numberOfSentBytesPartFile;
	m_numberOfSentBytesPartFile = 0;

	return sentBytes;
}

uint64 CEMSocket::GetSentBytesControlPacketSinceLastCallAndReset()
{
	std::lock_guard<std::mutex> lock(m_sendLocker);

	uint64 sentBytes = m_numberOfSentBytesControlPacket;
	m_numberOfSentBytesControlPacket = 0;

	return sentBytes;
}

uint64 CEMSocket::GetSentPayloadSinceLastCallAndReset()
{
	std::lock_guard<std::mutex> lock(m_sendLocker);

	uint64 sentBytes = m_actualPayloadSizeSent;
	m_actualPayloadSizeSent = 0;

	return sentBytes;
}

// Non-resetting peek at bytes sent since the last GetSentPayloadSinceLastCallAndReset(). Used by
// the disk I/O thread for a fresh view without consuming the counter SendBlockData() drains every
// CORE_TIMER_PERIOD. Must not be called with m_sendLocker already held.
uint64 CEMSocket::PeekSentPayload()
{
	std::lock_guard<std::mutex> lock(m_sendLocker);
	return m_actualPayloadSizeSent;
}

bool CEMSocket::HasQueues(bool bOnlyStandardPackets) const
{
	return sendbuffer != NULL || !m_standard_queue.empty() ||
	       (!bOnlyStandardPackets && !m_control_queue.empty());
}

void CEMSocket::OnSend(int nErrorCode)
{
	if (nErrorCode) {
		OnError(nErrorCode);
		return;
	}

	CEncryptedStreamSocket::OnSend(0);

	std::lock_guard<std::mutex> lock(m_sendLocker);
	m_bBusy = false;

	if (byConnected != ES_DISCONNECTED) {
		byConnected = ES_CONNECTED;

		if (m_currentPacket_is_controlpacket) {
			// queue up for control packet
			theApp->uploadBandwidthThrottler->QueueForSendingControlPacket(this, HasSent());
		}
	}
}

/**
 * Try to put queued-up data on the socket.
 *
 * Control packets have higher priority and are sent first where possible. A standard packet can be
 * split across several containers; all parts of a split packet must then be sent in a row, with no
 * control packet in between.
 *
 * @param maxNumberOfBytesToSend the most bytes this call may put on the socket.
 * @param onlyAllowedToSendControlPacket only try control packets. If a standard packet is in the
 * way and this socket is thought to be no longer an upload slot, it may be sent to clear the way,
 * but no new standard packet may be picked from the queue. Several split packets count as one, so
 * they can all be finished off if necessary.
 * @return the number of bytes actually put on the socket.
 */
SocketSentBytes CEMSocket::Send(
	uint32 maxNumberOfBytesToSend, uint32 minFragSize, bool onlyAllowedToSendControlPacket)
{
	std::lock_guard<std::mutex> lock(m_sendLocker);

	if (byConnected == ES_DISCONNECTED) {
		SocketSentBytes returnVal = { false, 0, 0 };
		return returnVal;
	} else if (m_bBusy && onlyAllowedToSendControlPacket) {
		SocketSentBytes returnVal = { true, 0, 0 };
		return returnVal;
	}

	bool anErrorHasOccured = false;
	uint32 sentStandardPacketBytesThisCall = 0;
	uint32 sentControlPacketBytesThisCall = 0;

	if (byConnected == ES_CONNECTED && IsEncryptionLayerReady() &&
		(!m_bBusy || onlyAllowedToSendControlPacket)) {

		if (minFragSize < 1) {
			minFragSize = 1;
		}

		maxNumberOfBytesToSend = GetNextFragSize(maxNumberOfBytesToSend, minFragSize);

		uint64 now = ::GetTickCount64();
		bool bWasLongTimeSinceSend = (now - lastSent) > 1000;

		lastCalledSend = now;

		while (sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall <
				maxNumberOfBytesToSend &&
			anErrorHasOccured == false && // don't send more than allowed. Also, there should have
						      // been no error in earlier loop
			(!m_control_queue.empty() || !m_standard_queue.empty() ||
				sendbuffer != NULL) &&              // there must exist something to send
			(onlyAllowedToSendControlPacket == false || // this means we are allowed to send both
								    // types of packets, so proceed
				(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall > 0 &&
					(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall) %
							minFragSize !=
						0) ||
				(sendbuffer == NULL &&
					!m_control_queue
						 .empty()) || // There's a control packet in queue, and we are
							      // not currently sending anything, so we will
							      // handle the control packet next
				(sendbuffer != NULL &&
					m_currentPacket_is_controlpacket ==
						true) || // We are in the progress of sending a control
							 // packet. We are always allowed to send those
				(sendbuffer != NULL && m_currentPacket_is_controlpacket == false &&
					bWasLongTimeSinceSend && !m_control_queue.empty() &&
					m_standard_queue.empty() &&
					(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall) <
						minFragSize) // We have waited to long to clean the current
							     // packet (which may be a standard packet in the
							     // way). Proceed whatever
							     // onlyAllowedToSendControlPacket says.
				)) {

			// If we are currently not in the progress of sending a packet, we will need to find
			// the next one to send
			if (sendbuffer == NULL) {
				CPacket *curPacket = NULL;
				if (!m_control_queue.empty()) {
					m_currentPacket_is_controlpacket = true;
					curPacket = m_control_queue.front();
					m_control_queue.pop_front();
				} else if (!m_standard_queue
						    .empty() /*&& onlyAllowedToSendControlPacket == false*/) {
					m_currentPacket_is_controlpacket = false;
					StandardPacketQueueEntry queueEntry = m_standard_queue.front();
					m_standard_queue.pop_front();
					curPacket = queueEntry.packet;
					m_actualPayloadSize = queueEntry.actualPayloadSize;

					// remember this for statistics purposes.
					m_currentPackageIsFromPartFile = curPacket->IsFromPF();
				} else {
					// Just to be safe; should not happen. Reaching this point
					// means something is wrong with the while condition above.
					wxFAIL;
					AddDebugLogLineC(logGeneral,
						"EMSocket: Couldn't get a new packet! There's an error in "
						"the first while condition in EMSocket::Send()");

					SocketSentBytes returnVal = { true,
						sentStandardPacketBytesThisCall,
						sentControlPacketBytesThisCall };
					return returnVal;
				}

				// We found a packet to send. Get the data to send from the
				// package container and dispose of the container.
				sendblen = curPacket->GetRealPacketSize();
				sendbuffer = curPacket->DetachPacket();
				sent = 0;
				delete curPacket;

				CryptPrepareSendData((uint8_t *)sendbuffer, sendblen);
			}

			// At this point sendbuffer holds a packet to send. Loop until the whole
			// packet is sent, the maximum bytes for this call is reached, or an error
			// occurs. NOTE: if a send would block (WOULDBLOCK), we return from this
			// method INSIDE the loop.
			while (sent < sendblen &&
				sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall <
					maxNumberOfBytesToSend &&
				(onlyAllowedToSendControlPacket ==
						false || // this means we are allowed to send both types of
							 // packets, so proceed
					m_currentPacket_is_controlpacket ||
					(bWasLongTimeSinceSend &&
						(sentStandardPacketBytesThisCall +
							sentControlPacketBytesThisCall) < minFragSize) ||
					(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall) %
							minFragSize !=
						0) &&
				anErrorHasOccured == false) {
				uint32 tosend = sendblen - sent;
				if (!onlyAllowedToSendControlPacket || m_currentPacket_is_controlpacket) {
					if (maxNumberOfBytesToSend >=
							sentStandardPacketBytesThisCall +
								sentControlPacketBytesThisCall &&
						tosend > maxNumberOfBytesToSend -
								 (sentStandardPacketBytesThisCall +
									 sentControlPacketBytesThisCall))
						tosend = maxNumberOfBytesToSend -
							 (sentStandardPacketBytesThisCall +
								 sentControlPacketBytesThisCall);
				} else if (bWasLongTimeSinceSend &&
					   (sentStandardPacketBytesThisCall +
						   sentControlPacketBytesThisCall) < minFragSize) {
					if (minFragSize >= sentStandardPacketBytesThisCall +
								   sentControlPacketBytesThisCall &&
						tosend >
							minFragSize - (sentStandardPacketBytesThisCall +
									      sentControlPacketBytesThisCall))
						tosend =
							minFragSize - (sentStandardPacketBytesThisCall +
									      sentControlPacketBytesThisCall);
				} else {
					uint32 nextFragMaxBytesToSent =
						GetNextFragSize(sentStandardPacketBytesThisCall +
									sentControlPacketBytesThisCall,
							minFragSize);
					if (nextFragMaxBytesToSent >=
							sentStandardPacketBytesThisCall +
								sentControlPacketBytesThisCall &&
						tosend > nextFragMaxBytesToSent -
								 (sentStandardPacketBytesThisCall +
									 sentControlPacketBytesThisCall))
						tosend = nextFragMaxBytesToSent -
							 (sentStandardPacketBytesThisCall +
								 sentControlPacketBytesThisCall);
				}
				wxASSERT(tosend != 0 && tosend <= sendblen - sent);

				lastSent = ::GetTickCount64();

				uint32 result = CEncryptedStreamSocket::Write(sendbuffer + sent, tosend);

				// Advance 'sent' before checking BlocksWrite(), which reflects "any
				// async_write currently in flight" rather than "did this Write()
				// succeed": a previous iteration's pending write may still be in
				// flight when the current one succeeds, so advancing first keeps
				// 'sent' from lagging and the same bytes from being re-sent.
				if (result > 0) {
					m_hasSent = true;
					sent += result;
					if (m_currentPacket_is_controlpacket == false) {
						sentStandardPacketBytesThisCall += result;
						if (m_currentPackageIsFromPartFile == true) {
							m_numberOfSentBytesPartFile += result;
						} else {
							m_numberOfSentBytesCompleteFile += result;
						}
					} else {
						sentControlPacketBytesThisCall += result;
						m_numberOfSentBytesControlPacket += result;
					}
				}

				if (BlocksWrite()) {
					m_bBusy = true;
					SocketSentBytes returnVal = { true,
						sentStandardPacketBytesThisCall,
						sentControlPacketBytesThisCall };
					return returnVal; // Send() blocked, onsend will be called when ready
							  // to send again
				} else if (LastError()) {
					// Send() gave an error
					anErrorHasOccured = true;
				} else {
					m_bBusy = false;
				}
			}

			if (sent == sendblen) {
				// we are done sending the current packet. Delete it and set
				// sendbuffer to NULL so a new packet can be fetched.
				delete[] sendbuffer;
				sendbuffer = NULL;
				sendblen = 0;

				if (!m_currentPacket_is_controlpacket) {
					m_actualPayloadSizeSent += m_actualPayloadSize;
					m_actualPayloadSize = 0;

					lastFinishedStandard = ::GetTickCount64(); // reset timeout
					m_bAccelerateUpload = false; // Safe until told otherwise
				}

				sent = 0;
			}
		}
	}

	if (onlyAllowedToSendControlPacket &&
		(!m_control_queue.empty() || (sendbuffer != NULL && m_currentPacket_is_controlpacket))) {
		// Enter the control packet send queue. We may enter it several times for the same
		// package, but that costs less overhead than ensuring we enter it only once.
		theApp->uploadBandwidthThrottler->QueueForSendingControlPacket(this, HasSent());
	}

	SocketSentBytes returnVal = {
		!anErrorHasOccured, sentStandardPacketBytesThisCall, sentControlPacketBytesThisCall
	};

	return returnVal;
}

uint32 CEMSocket::GetNextFragSize(uint32 current, uint32 minFragSize)
{
	if (current % minFragSize == 0) {
		return current;
	} else {
		return minFragSize * (current / minFragSize + 1);
	}
}

/**
 * Decides the minimum amount the socket needs to send to prevent a timeout. @author SlugFiller
 */
uint32 CEMSocket::GetNeededBytes()
{
	uint64 sendgap;

	uint64 timetotal;
	uint64 timeleft;
	uint64 sizeleft, sizetotal;

	{
		std::lock_guard<std::mutex> lock(m_sendLocker);

		if (byConnected == ES_DISCONNECTED) {
			return 0;
		}

		if (!((sendbuffer && !m_currentPacket_is_controlpacket) || !m_standard_queue.empty())) {
			// No standard packet to send. Even if data needs to be sent to prevent timeout,
			// there's nothing to send.
			return 0;
		}

		if (((sendbuffer && !m_currentPacket_is_controlpacket)) && !m_control_queue.empty())
			m_bAccelerateUpload =
				true; // We might be trying to send a block request, accelerate packet

		uint64 now = ::GetTickCount64();
		sendgap = now - lastCalledSend;

		timetotal = m_bAccelerateUpload ? 45000 : 90000;
		timeleft = now - lastFinishedStandard;
		if (sendbuffer && !m_currentPacket_is_controlpacket) {
			sizeleft = sendblen - sent;
			sizetotal = sendblen;
		} else {
			sizeleft = sizetotal = m_standard_queue.front().packet->GetRealPacketSize();
		}
	}

	if (timeleft >= timetotal)
		return sizeleft;
	timeleft = timetotal - timeleft;
	if (timeleft * sizetotal >= timetotal * sizeleft) {
		// don't use 'GetTimeOut' here in case the timeout value is high,
		if (sendgap > SEC2MS(20))
			return 1; // Don't let the socket itself time out - Might happen when switching from
				  // spread(non-focus) slot to trickle slot
		return 0;
	}
	uint64 decval = timeleft * sizetotal / timetotal;
	if (!decval)
		return sizeleft;
	if (decval < sizeleft)
		return sizeleft - decval + 1; // Round up
	else
		return 1;
}

/**
 * Removes every packet from the standard queue that need not be sent before the socket can send a
 * control packet.
 *
 * The current packet has to be finished first, and if it is part of a split packet then every part
 * of that packet must go out before a control packet can. So this keeps only the rest of a split
 * packet and removes everything after it. The control packet queue is untouched.
 */
void CEMSocket::TruncateQueues()
{
	std::lock_guard<std::mutex> lock(m_sendLocker);

	// Clear the standard queue totally
	// Please note! There may still be a standardpacket in the sendbuffer variable!
	CStdPacketQueue::iterator it = m_standard_queue.begin();
	for (; it != m_standard_queue.end(); ++it) {
		delete it->packet;
	}

	m_standard_queue.clear();
}

uint64 CEMSocket::GetTimeOut() const
{
	return m_uTimeOut;
}

void CEMSocket::SetTimeOut(uint64 uTimeOut)
{
	m_uTimeOut = uTimeOut;
}
// File_checked_for_headers
