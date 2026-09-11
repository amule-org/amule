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
 * The session key comes from an ephemeral X25519 exchange, so a recorded
 * session cannot be decrypted later even by someone who by then holds the EC
 * password: the keys that opened it existed only for its duration.
 *
 * A raw exchange authenticates nobody, so an active man in the middle could run
 * one exchange with each side and relay. The shared password closes that through
 * the confirmation tags below: each side proves it knows the password over the
 * exact handshake it saw, and a relay's two handshakes differ, so at least one
 * check fails. No certificates needed.
 *
 * No new dependency: the `ec` library already links Crypto++, which also backs
 * aMule's eD2k obfuscation, secure identification and the webserver's HMAC.
 */
namespace ECCrypt
{

/// Wire ids for the AEAD, carried in EC_TAG_AEAD_CIPHER.
enum Cipher : uint8_t
{
	Cipher_None = 0,
	/// Mandatory baseline: present in every Crypto++ we accept.
	Cipher_AES128_GCM = 1,
	/// Preferred where available: roughly 2.6x faster than AES-GCM without
	/// hardware AES, which is every Raspberry Pi up to the 4. Needs Crypto++
	/// 8.1, aMule's minimum, so it is always compiled in and just negotiated.
	Cipher_ChaCha20_Poly1305 = 2
};

/// Handshake nonce length, per side (EC_TAG_AEAD_{CLIENT,SERVER}_NONCE).
const size_t NONCE_TAG_LEN = 32;
/// AEAD tag appended to every sealed body.
const size_t AEAD_TAG_LEN = 16;

/// Whether this CPU runs AES in hardware *and* cryptopp will dispatch to it.
/// Both matter: where cryptopp's detection comes up empty it falls back to
/// table-based AES, so asking the CPU alone would prefer a software cipher.
bool HasHardwareAES();

/// Ciphers this build can actually do, strongest/fastest first. The server
/// picks the first entry the client also offered. AES leads when it is
/// hardware-backed, ChaCha20 otherwise -- see HasHardwareAES().
std::vector<uint8_t> SupportedCiphers();

/// SupportedCiphers() with the hardware question answered explicitly, so both
/// orderings are testable anywhere. An overload rather than a default argument:
/// a default would pull HasHardwareAES()'s definition into every caller.
std::vector<uint8_t> SupportedCiphers(bool preferAES);

/// Whether this build can do @a cipher at all.
bool IsCipherSupported(uint8_t cipher);

/// Human name for logging; "unknown" for anything unrecognised.
const char *CipherName(uint8_t cipher);

/// @a count cryptographically random bytes. Empty on failure.
std::vector<uint8_t> RandomBytes(size_t count);

/// Overwrite with a wipe the optimiser may not elide (unlike std::fill on a
/// buffer about to be freed), then clear. For private keys and the shared
/// secret, so a later memory disclosure cannot recover them.
void SecureWipe(std::vector<uint8_t> &v);
void SecureWipe(uint8_t *p, size_t n);

/**
 * HKDF-SHA256 (RFC 5869), extract-then-expand.
 *
 * Wraps Crypto++'s hkdf.h for the vector-in/vector-out shape used here, and
 * enforces the RFC length cap rather than letting it surface as an exception.
 */
std::vector<uint8_t> HkdfSha256(const std::vector<uint8_t> &ikm,
	const std::vector<uint8_t> &salt,
	const std::vector<uint8_t> &info,
	size_t outLen);

/// Length of an X25519 public key, private key and shared secret alike.
constexpr size_t X25519_KEY_LEN = 32;

/**
 * Generate an ephemeral X25519 key pair.
 *
 * The private key never leaves the process, is never written anywhere, and is
 * discarded once the session key is derived. That is what gives forward secrecy.
 *
 * @return false if randomness is unavailable; both outputs are cleared and the
 *         caller must stay in clear rather than use a predictable key.
 */
bool GenerateX25519KeyPair(std::vector<uint8_t> &privOut, std::vector<uint8_t> &pubOut);

/**
 * X25519 shared secret from our private key and the peer's public key.
 *
 * An all-zero secret, which a peer can force with a low-order point, is
 * rejected: it would key every such session identically.
 *
 * @return false on a malformed or degenerate peer key; @a sharedOut is cleared.
 */
bool X25519Agree(const std::vector<uint8_t> &priv,
	const std::vector<uint8_t> &peerPub,
	std::vector<uint8_t> &sharedOut);

/**
 * The handshake transcript both sides bind into their derivations.
 *
 * One definition rather than one per side: the ends must agree byte for byte,
 * and two copies of the same concatenation drift when a field is added later.
 *
 * The cipher list is length-prefixed so no two handshakes can flatten to the
 * same bytes; with a bare concatenation the boundary is only implied.
 */
std::vector<uint8_t> BuildTranscript(const std::vector<uint8_t> &offeredCiphers,
	uint8_t chosenCipher,
	const std::vector<uint8_t> &clientNonce,
	const std::vector<uint8_t> &serverNonce,
	const std::vector<uint8_t> &clientPub,
	const std::vector<uint8_t> &serverPub);

/**
 * Key-confirmation tag proving knowledge of the EC credential.
 *
 * The channel key comes from the ephemeral exchange alone, so the password no
 * longer stops an active man in the middle, who can complete two exchanges and
 * relay. The tag catches that: it binds the credential to the transcript, which
 * differs on the two legs of a relay, so at least one check fails.
 *
 * Built from the existing HKDF rather than a separate HMAC: extract-then-expand
 * with the credential as keying material is a MAC over the transcript, and
 * reusing it keeps the crypto surface to what is already reviewed.
 *
 * @param label direction tag, so the two sides' confirmations differ and one
 *              cannot be replayed as the other.
 */
std::vector<uint8_t> ConfirmTag(
	const std::vector<uint8_t> &secret, const std::vector<uint8_t> &transcript, const char *label);

/// Length of a confirmation tag.
constexpr size_t CONFIRM_TAG_LEN = 32;

/**
 * Constant-time equality for secrets.
 *
 * A compare that returns early leaks, through timing, how much of a guessed tag
 * was right, turning forgery into a per-byte search instead of a 2^256 one.
 */
bool ConstantTimeEquals(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b);

/**
 * One direction-aware AEAD session for a single EC connection.
 *
 * Keys are split per direction so the two packet counters cannot collide on a
 * nonce. The counter is implicit: TCP guarantees ordering and EC treats any
 * desync as fatal, so carrying it would cost 8 bytes per packet and buy nothing.
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
	 * @param ikm         the X25519 shared secret, deliberately not the
	 *                    credential: keying from something that outlives the
	 *                    session is what costs forward secrecy.
	 * @param transcript  handshake bytes bound into the derivation, so a tampered
	 *                    capability exchange yields a different key on each side
	 *                    and the first tag check fails. This is the downgrade
	 *                    defence; policy checks sit on top.
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
	// The EC write path builds a packet as CQueuedData chunks and back-patches the
	// length, so the packet is already in memory before it reaches the socket.
	// These seal chunks in place and append the tag rather than flattening into a
	// second buffer, which would double peak memory on exactly the huge responses
	// the 256 MB gate exists for. The length need not be known in advance: with
	// ZLIB the compressed size is only known once deflate finishes. Begin/Final
	// bracket one packet and advance that direction's counter.

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
