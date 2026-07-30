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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef ECCRYPT_H
#define ECCRYPT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Authenticated encryption for the External Connect packet layer.
 *
 * Both endpoints already share the EC password, so the session key is derived
 * from it rather than from a key exchange: an attacker who does not know the
 * password cannot derive the key and therefore cannot forge a tag, which is
 * what makes this resistant to an active man in the middle without needing any
 * certificate handling. The trade-off is no forward secrecy -- a password
 * compromise later would decrypt a recording made earlier.
 *
 * Nothing here is new dependency surface: the `ec` library already links
 * Crypto++ (`NEED_LIB_EC` implies `NEED_LIB_CRYPTO`), which is also where
 * aMule's eD2k obfuscation, secure identification and the webserver's HMAC
 * already come from.
 */
namespace ECCrypt
{

/// Wire ids for the AEAD, carried in EC_TAG_AEAD_CIPHER.
enum Cipher : uint8_t
{
	Cipher_None = 0,
	/// Mandatory baseline: present in every Crypto++ down to the 5.6 floor.
	Cipher_AES128_GCM = 1,
	/// Preferred where available. Roughly 2.6x faster than AES-GCM in a
	/// Crypto++ build without hardware AES -- which is every Raspberry Pi up
	/// to and including the 4, whose Cortex-A53/A72 have no ARMv8 crypto
	/// extensions. Needs Crypto++ 8.1, so it is compiled in conditionally
	/// and negotiated rather than assumed.
	Cipher_ChaCha20_Poly1305 = 2
};

/// Handshake nonce length, per side (EC_TAG_AEAD_{CLIENT,SERVER}_NONCE).
const size_t NONCE_TAG_LEN = 32;
/// AEAD tag appended to every sealed body.
const size_t AEAD_TAG_LEN = 16;

/// Ciphers this build can actually do, strongest/fastest first. The server
/// picks the first entry the client also offered.
std::vector<uint8_t> SupportedCiphers();

/// Whether this build can do @a cipher at all.
bool IsCipherSupported(uint8_t cipher);

/// Human name for logging; "unknown" for anything unrecognised.
const char *CipherName(uint8_t cipher);

/// @a count cryptographically random bytes. Empty on failure.
std::vector<uint8_t> RandomBytes(size_t count);

/**
 * HKDF-SHA256 (RFC 5869), extract-then-expand.
 *
 * Hand-rolled over HMAC-SHA256 rather than using Crypto++'s `hkdf.h`, which
 * only appeared in 5.6.3 while aMule's declared floor is 5.6.0. HMAC-SHA256 is
 * present in every version and already used elsewhere in the tree.
 */
std::vector<uint8_t> HkdfSha256(const std::vector<uint8_t> &ikm,
	const std::vector<uint8_t> &salt,
	const std::vector<uint8_t> &info,
	size_t outLen);

/**
 * One direction-aware AEAD session for a single EC connection.
 *
 * Keys are split per direction so the two packet counters can never produce a
 * colliding nonce. The counter is implicit: it is not carried on the wire,
 * because TCP already guarantees ordering and the EC framing treats any desync
 * as fatal regardless, so an explicit counter would cost 8 bytes per packet and
 * buy nothing.
 */
class Session
{
public:
	Session();
	~Session();
	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	/**
	 * Derive the session keys.
	 *
	 * @param cipher       negotiated cipher id.
	 * @param ikm          the shared secret (the stored EC password hash).
	 * @param serverNonce  NONCE_TAG_LEN bytes from the daemon.
	 * @param clientNonce  NONCE_TAG_LEN bytes from the client.
	 * @param transcript   handshake bytes bound into the derivation, so a
	 *                     tampered capability exchange yields a different key
	 *                     on each side and the first tag check fails. This is
	 *                     the actual downgrade defence; policy checks sit on
	 *                     top of it.
	 * @param isServer     which direction key to seal with.
	 * @return false if the cipher is unsupported or a nonce is the wrong size.
	 */
	bool Init(uint8_t cipher,
		const std::vector<uint8_t> &ikm,
		const std::vector<uint8_t> &serverNonce,
		const std::vector<uint8_t> &clientNonce,
		const std::vector<uint8_t> &transcript,
		bool isServer);

	bool IsActive() const { return m_active; }

	/// Forget all key material. Used when a socket object is reused for a
	/// fresh connection (amulegui reconnect).
	void Reset();

	uint8_t GetCipher() const { return m_cipher; }

	/// Seal @a len bytes into @a out (ciphertext followed by the tag).
	/// Convenience wrapper over the streaming calls below.
	bool Seal(const uint8_t *plain, size_t len, std::vector<uint8_t> &out);

	/// Open a sealed body. Fails on a bad tag, which is the tamper signal.
	bool Open(const uint8_t *sealed, size_t len, std::vector<uint8_t> &out);

	// --- streaming, in place -------------------------------------------
	//
	// The EC write path builds a packet as a list of CQueuedData chunks and
	// only then back-patches the length, so the whole packet is already in
	// memory before anything reaches the socket. These let the chunks be
	// sealed where they lie and the tag appended, instead of flattening the
	// body into a second buffer -- which would double peak memory on exactly
	// the huge responses the 256 MB receive gate exists for.
	//
	// Total length need not be known in advance, which matters because with
	// ZLIB the compressed size is only known once deflate has finished.
	//
	// Begin/Final bracket one packet and advance that direction's counter.

	bool SealBegin();
	bool SealUpdate(uint8_t *data, size_t len);
	/// Writes AEAD_TAG_LEN bytes to @a tagOut.
	bool SealFinal(uint8_t *tagOut);

	bool OpenBegin();
	bool OpenUpdate(uint8_t *data, size_t len);
	/// @a tag is AEAD_TAG_LEN bytes. False means the body was tampered with.
	bool OpenFinal(const uint8_t *tag);

	/// Bytes a sealed body adds over its plaintext.
	static size_t Overhead() { return AEAD_TAG_LEN; }

private:
	/// Holds the in-flight cryptopp cipher objects. Opaque so that including
	/// this header does not drag cryptopp into every consumer -- ECSocket.h
	/// includes it, and that reaches most of the EC library.
	struct StreamState;

	std::vector<uint8_t> BuildNonce(const uint8_t *prefix, uint64_t counter) const;

	bool m_active = false;
	uint8_t m_cipher = Cipher_None;
	std::vector<uint8_t> m_txKey;
	std::vector<uint8_t> m_rxKey;
	uint8_t m_txPrefix[4] = { 0, 0, 0, 0 };
	uint8_t m_rxPrefix[4] = { 0, 0, 0, 0 };
	uint64_t m_txCounter = 0;
	uint64_t m_rxCounter = 0;
	std::unique_ptr<StreamState> m_stream;
};

} // namespace ECCrypt

#endif // ECCRYPT_H
