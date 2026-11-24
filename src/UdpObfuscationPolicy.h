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

#ifndef UDPOBFUSCATIONPOLICY_H
#define UDPOBFUSCATIONPOLICY_H

#include <array>
#include <cstddef>
#include <cstdint>

enum class EUdpSendForm
{
	Plain,
	Obfuscated,
	Drop
};

/**
 * Whether a queued UDP packet is given an obfuscation header. Either a crypt target or, for Kad, a
 * receiver key is needed to derive the key.
 *
 * Kad does not follow the user's crypt layer preference, as in eMule: the receiver key that proves
 * our address to a peer travels inside that header, so a node that stripped it could not prove
 * itself to anyone. The preference still decides the ed2k client traffic, which carries our user
 * hash and nothing a peer needs back.
 */
inline bool ObfuscateQueuedPacket(bool requested,
	bool haveCryptTarget,
	bool kad,
	std::uint32_t receiverVerifyKey,
	bool clientCryptLayerSupported)
{
	if (!requested || !(haveCryptTarget || (kad && receiverVerifyKey != 0))) {
		return false;
	}
	return kad || clientCryptLayerSupported;
}

/**
 * Our IP in a client UDP key. The receiver rebuilds the key from the datagram's source, so only an
 * address the peer sees us at will do: its per-peer key IP, else our verified public IP. Never the
 * host-name fallback of GetPublicIP(), which is a LAN or loopback address behind NAT. 0: no key.
 */
inline std::uint32_t ClientUdpKeyIP(std::uint32_t perPeerKeyIP, std::uint32_t verifiedPublicIP)
{
	return perPeerKeyIP != 0 ? perPeerKeyIP : verifiedPublicIP;
}

/**
 * How a queued UDP packet leaves. Client obfuscation needs our IP in the key (ClientUdpKeyIP()), so an
 * ordinary packet falls back to plain without one. A packet that must be obfuscated (one carrying our
 * user hash) is dropped instead: the public IP can be lost between queueing and sending.
 */
inline EUdpSendForm DecideUdpSendForm(bool encrypt, bool requireObfuscation, bool kad, std::uint32_t keyIP)
{
	if (encrypt && (keyIP != 0 || kad)) {
		return EUdpSendForm::Obfuscated;
	}
	return requireObfuscation ? EUdpSendForm::Drop : EUdpSendForm::Plain;
}

struct ServerUdpKeyCandidates
{
	std::array<std::uint32_t, 2> keys{};
	std::size_t count = 0;
};

/**
 * The keys to try on an obfuscated datagram from a server, in order. The crypt ping challenge keys
 * only the status ping reply. Everything else the server sends while that reply is pending, such as
 * a source or search answer or a NAT callback, uses its UDP key.
 */
inline ServerUdpKeyCandidates ServerUdpReceiveKeys(
	bool cryptPingReplyPending, std::uint32_t challenge, std::uint32_t serverKeyUdp)
{
	ServerUdpKeyCandidates candidates;
	if (cryptPingReplyPending && challenge != 0) {
		candidates.keys[candidates.count++] = challenge;
	}
	if (serverKeyUdp != 0 && (candidates.count == 0 || candidates.keys[0] != serverKeyUdp)) {
		candidates.keys[candidates.count++] = serverKeyUdp;
	}
	return candidates;
}

/**
 * The result of the first key that decrypts, or bufLen when none does. decrypt(key) must return
 * bufLen and leave the buffer unchanged when it fails, as DecryptReceivedServer() does.
 */
template <typename Decrypt>
int DecryptWithServerUdpKeys(const ServerUdpKeyCandidates &candidates, int bufLen, Decrypt decrypt)
{
	for (std::size_t index = 0; index != candidates.count; ++index) {
		const int result = decrypt(candidates.keys[index]);
		if (result < bufLen) {
			return result;
		}
	}
	return bufLen;
}

#endif // UDPOBFUSCATIONPOLICY_H
