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

#include "ECCrypt.h"

// Go through the tree's cryptopp wrapper rather than including the headers
// directly: it carries the deprecation pragmas those headers need under this
// project's -Werror settings, and knows the include prefix. CRYPTOPP_INC_NEED_AEAD
// adds aes/gcm/hmac (+ chachapoly where the version allows) to its usual set.
#define CRYPTOPP_INC_NEED_AEAD
#include "../../../CryptoPP_Inc.h"

#ifdef CRYPTOPP_INC_HAVE_CHACHA20POLY1305
#define EC_HAVE_CHACHA20POLY1305 1
#endif

#include <algorithm>
#include <cstring>

namespace ECCrypt
{

namespace
{
const size_t AEAD_NONCE_LEN = 12; // 4-byte derived prefix + 8-byte counter
const size_t KEY_LEN_AES128 = 16;
const size_t KEY_LEN_CHACHA = 32;
const size_t SHA256_LEN = 32;

/// Key length the negotiated cipher wants, 0 if we cannot do it.
size_t KeyLenFor(uint8_t cipher)
{
	switch (cipher) {
	case Cipher_AES128_GCM:
		return KEY_LEN_AES128;
#ifdef EC_HAVE_CHACHA20POLY1305
	case Cipher_ChaCha20_Poly1305:
		return KEY_LEN_CHACHA;
#endif
	default:
		return 0;
	}
}

/**
 * One-shot seal/open, dispatched on the cipher.
 *
 * Crypto++ signals a failed tag check by returning false from
 * DecryptAndVerify, but it throws on malformed parameters, so everything is
 * wrapped: a crypto failure must surface as a protocol error on one connection,
 * never as an exception escaping into the socket layer.
 */
bool AeadSeal(uint8_t cipher,
	const std::vector<uint8_t> &key,
	const std::vector<uint8_t> &nonce,
	const uint8_t *plain,
	size_t len,
	std::vector<uint8_t> &out)
{
	try {
		out.assign(len + AEAD_TAG_LEN, 0);
		switch (cipher) {
		case Cipher_AES128_GCM: {
			CryptoPP::GCM<CryptoPP::AES>::Encryption enc;
			enc.SetKeyWithIV(key.data(), key.size(), nonce.data(), nonce.size());
			enc.EncryptAndAuthenticate(out.data(),
				out.data() + len,
				AEAD_TAG_LEN,
				nonce.data(),
				static_cast<int>(nonce.size()),
				nullptr,
				0,
				plain,
				len);
			return true;
		}
#ifdef EC_HAVE_CHACHA20POLY1305
		case Cipher_ChaCha20_Poly1305: {
			CryptoPP::ChaCha20Poly1305::Encryption enc;
			enc.SetKeyWithIV(key.data(), key.size(), nonce.data(), nonce.size());
			enc.EncryptAndAuthenticate(out.data(),
				out.data() + len,
				AEAD_TAG_LEN,
				nonce.data(),
				static_cast<int>(nonce.size()),
				nullptr,
				0,
				plain,
				len);
			return true;
		}
#endif
		default:
			return false;
		}
	} catch (const CryptoPP::Exception &) {
		return false;
	}
}

bool AeadOpen(uint8_t cipher,
	const std::vector<uint8_t> &key,
	const std::vector<uint8_t> &nonce,
	const uint8_t *sealed,
	size_t len,
	std::vector<uint8_t> &out)
{
	if (len < AEAD_TAG_LEN) {
		return false;
	}
	const size_t bodyLen = len - AEAD_TAG_LEN;
	try {
		out.assign(bodyLen, 0);
		switch (cipher) {
		case Cipher_AES128_GCM: {
			CryptoPP::GCM<CryptoPP::AES>::Decryption dec;
			dec.SetKeyWithIV(key.data(), key.size(), nonce.data(), nonce.size());
			return dec.DecryptAndVerify(out.data(),
				sealed + bodyLen,
				AEAD_TAG_LEN,
				nonce.data(),
				static_cast<int>(nonce.size()),
				nullptr,
				0,
				sealed,
				bodyLen);
		}
#ifdef EC_HAVE_CHACHA20POLY1305
		case Cipher_ChaCha20_Poly1305: {
			CryptoPP::ChaCha20Poly1305::Decryption dec;
			dec.SetKeyWithIV(key.data(), key.size(), nonce.data(), nonce.size());
			return dec.DecryptAndVerify(out.data(),
				sealed + bodyLen,
				AEAD_TAG_LEN,
				nonce.data(),
				static_cast<int>(nonce.size()),
				nullptr,
				0,
				sealed,
				bodyLen);
		}
#endif
		default:
			return false;
		}
	} catch (const CryptoPP::Exception &) {
		return false;
	}
}

} // namespace

std::vector<uint8_t> SupportedCiphers()
{
	std::vector<uint8_t> out;
#ifdef EC_HAVE_CHACHA20POLY1305
	// Preferred first: the server picks the first entry the client also has.
	out.push_back(Cipher_ChaCha20_Poly1305);
#endif
	out.push_back(Cipher_AES128_GCM);
	return out;
}

bool IsCipherSupported(uint8_t cipher)
{
	return KeyLenFor(cipher) != 0;
}

const char *CipherName(uint8_t cipher)
{
	switch (cipher) {
	case Cipher_AES128_GCM:
		return "AES-128-GCM";
	case Cipher_ChaCha20_Poly1305:
		return "ChaCha20-Poly1305";
	case Cipher_None:
		return "none";
	default:
		return "unknown";
	}
}

std::vector<uint8_t> RandomBytes(size_t count)
{
	std::vector<uint8_t> out(count, 0);
	try {
		CryptoPP::AutoSeededRandomPool rng;
		rng.GenerateBlock(out.data(), out.size());
	} catch (const CryptoPP::Exception &) {
		out.clear();
	}
	return out;
}

std::vector<uint8_t> HkdfSha256(const std::vector<uint8_t> &ikm,
	const std::vector<uint8_t> &salt,
	const std::vector<uint8_t> &info,
	size_t outLen)
{
	std::vector<uint8_t> out;
	// RFC 5869 caps the output at 255 hash lengths; nothing here comes close,
	// but honour it rather than silently truncating the counter byte.
	if (outLen == 0 || outLen > 255 * SHA256_LEN) {
		return out;
	}
	try {
		// Extract: PRK = HMAC(salt, ikm). An empty salt means a block of zeros.
		std::vector<uint8_t> effectiveSalt = salt;
		if (effectiveSalt.empty()) {
			effectiveSalt.assign(SHA256_LEN, 0);
		}
		uint8_t prk[SHA256_LEN];
		{
			CryptoPP::HMAC<CryptoPP::SHA256> hmac(effectiveSalt.data(), effectiveSalt.size());
			if (!ikm.empty()) {
				hmac.Update(ikm.data(), ikm.size());
			}
			hmac.Final(prk);
		}

		// Expand: T(n) = HMAC(PRK, T(n-1) || info || n)
		out.reserve(outLen);
		std::vector<uint8_t> prev;
		uint8_t counter = 1;
		while (out.size() < outLen) {
			CryptoPP::HMAC<CryptoPP::SHA256> hmac(prk, sizeof(prk));
			if (!prev.empty()) {
				hmac.Update(prev.data(), prev.size());
			}
			if (!info.empty()) {
				hmac.Update(info.data(), info.size());
			}
			hmac.Update(&counter, 1);
			uint8_t block[SHA256_LEN];
			hmac.Final(block);
			prev.assign(block, block + SHA256_LEN);
			const size_t take = std::min(outLen - out.size(), SHA256_LEN);
			out.insert(out.end(), block, block + take);
			++counter;
		}
	} catch (const CryptoPP::Exception &) {
		out.clear();
	}
	return out;
}

bool Session::Init(uint8_t cipher,
	const std::vector<uint8_t> &ikm,
	const std::vector<uint8_t> &serverNonce,
	const std::vector<uint8_t> &clientNonce,
	const std::vector<uint8_t> &transcript,
	bool isServer)
{
	m_active = false;
	const size_t keyLen = KeyLenFor(cipher);
	if (keyLen == 0 || serverNonce.size() != NONCE_TAG_LEN || clientNonce.size() != NONCE_TAG_LEN) {
		return false;
	}

	// salt = server nonce || client nonce; both sides contribute, so neither
	// can pin the derivation on its own.
	std::vector<uint8_t> salt;
	salt.reserve(serverNonce.size() + clientNonce.size());
	salt.insert(salt.end(), serverNonce.begin(), serverNonce.end());
	salt.insert(salt.end(), clientNonce.begin(), clientNonce.end());

	// info = label || cipher || transcript. Binding the transcript is what
	// makes a stripped capability tag fail closed rather than downgrade.
	static const char LABEL[] = "aMule EC AEAD v1";
	std::vector<uint8_t> info(LABEL, LABEL + sizeof(LABEL) - 1);
	info.push_back(cipher);
	info.insert(info.end(), transcript.begin(), transcript.end());

	const size_t need = keyLen * 2 + 8; // two keys + two 4-byte nonce prefixes
	const std::vector<uint8_t> okm = HkdfSha256(ikm, salt, info, need);
	if (okm.size() != need) {
		return false;
	}

	// Client-to-server material first, then server-to-client; each side picks
	// its transmit half from the same layout.
	const uint8_t *c2sKey = okm.data();
	const uint8_t *s2cKey = okm.data() + keyLen;
	const uint8_t *c2sPrefix = okm.data() + keyLen * 2;
	const uint8_t *s2cPrefix = okm.data() + keyLen * 2 + 4;

	if (isServer) {
		m_txKey.assign(s2cKey, s2cKey + keyLen);
		m_rxKey.assign(c2sKey, c2sKey + keyLen);
		std::memcpy(m_txPrefix, s2cPrefix, sizeof(m_txPrefix));
		std::memcpy(m_rxPrefix, c2sPrefix, sizeof(m_rxPrefix));
	} else {
		m_txKey.assign(c2sKey, c2sKey + keyLen);
		m_rxKey.assign(s2cKey, s2cKey + keyLen);
		std::memcpy(m_txPrefix, c2sPrefix, sizeof(m_txPrefix));
		std::memcpy(m_rxPrefix, s2cPrefix, sizeof(m_rxPrefix));
	}

	m_cipher = cipher;
	m_txCounter = 0;
	m_rxCounter = 0;
	m_active = true;
	return true;
}

std::vector<uint8_t> Session::BuildNonce(const uint8_t *prefix, uint64_t counter) const
{
	std::vector<uint8_t> nonce(AEAD_NONCE_LEN, 0);
	std::memcpy(nonce.data(), prefix, 4);
	for (int i = 0; i < 8; ++i) {
		// big-endian counter in the low 8 bytes
		nonce[4 + i] = static_cast<uint8_t>((counter >> (8 * (7 - i))) & 0xFF);
	}
	return nonce;
}

bool Session::Seal(const uint8_t *plain, size_t len, std::vector<uint8_t> &out)
{
	if (!m_active) {
		return false;
	}
	const std::vector<uint8_t> nonce = BuildNonce(m_txPrefix, m_txCounter);
	if (!AeadSeal(m_cipher, m_txKey, nonce, plain, len, out)) {
		return false;
	}
	++m_txCounter;
	return true;
}

bool Session::Open(const uint8_t *sealed, size_t len, std::vector<uint8_t> &out)
{
	if (!m_active) {
		return false;
	}
	const std::vector<uint8_t> nonce = BuildNonce(m_rxPrefix, m_rxCounter);
	if (!AeadOpen(m_cipher, m_rxKey, nonce, sealed, len, out)) {
		// Do NOT advance the counter: a failed open means the stream is no
		// longer trustworthy, and the caller drops the connection.
		return false;
	}
	++m_rxCounter;
	return true;
}

} // namespace ECCrypt
