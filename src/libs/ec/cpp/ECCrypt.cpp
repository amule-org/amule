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
// adds aes/gcm/hmac/chachapoly to its usual set.
#define CRYPTOPP_INC_NEED_AEAD
#include "../../../CryptoPP_Inc.h"

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
	case Cipher_ChaCha20_Poly1305:
		return KEY_LEN_CHACHA;
	default:
		return 0;
	}
}

/**
 * A fresh cipher object for @a cipher, or null if this build cannot do it.
 *
 * Both AEADs derive from AuthenticatedSymmetricCipher, so the socket layer can
 * drive either one through the same incremental calls.
 */
std::unique_ptr<CryptoPP::AuthenticatedSymmetricCipher> MakeCipher(uint8_t cipher, bool encrypt)
{
	switch (cipher) {
	case Cipher_AES128_GCM:
		if (encrypt) {
			return std::unique_ptr<CryptoPP::AuthenticatedSymmetricCipher>(
				new CryptoPP::GCM<CryptoPP::AES>::Encryption);
		}
		return std::unique_ptr<CryptoPP::AuthenticatedSymmetricCipher>(
			new CryptoPP::GCM<CryptoPP::AES>::Decryption);
	case Cipher_ChaCha20_Poly1305:
		if (encrypt) {
			return std::unique_ptr<CryptoPP::AuthenticatedSymmetricCipher>(
				new CryptoPP::ChaCha20Poly1305::Encryption);
		}
		return std::unique_ptr<CryptoPP::AuthenticatedSymmetricCipher>(
			new CryptoPP::ChaCha20Poly1305::Decryption);
	default:
		return nullptr;
	}
}

} // namespace

std::vector<uint8_t> SupportedCiphers()
{
	std::vector<uint8_t> out;
	// Preferred first: the server picks the first entry the client also has.
	out.push_back(Cipher_ChaCha20_Poly1305);
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
		// The length guard above stays: DeriveKey throws on an over-long
		// request, and callers here expect an empty vector rather than an
		// exception.
		out.resize(outLen);
		CryptoPP::HKDF<CryptoPP::SHA256> hkdf;
		hkdf.DeriveKey(out.data(),
			out.size(),
			ikm.data(),
			ikm.size(),
			salt.data(),
			salt.size(),
			info.data(),
			info.size());
	} catch (const CryptoPP::Exception &) {
		out.clear();
	}
	return out;
}

/// The in-flight cipher objects, one per direction and per packet.
struct Session::StreamState
{
	std::unique_ptr<CryptoPP::AuthenticatedSymmetricCipher> sealer;
	std::unique_ptr<CryptoPP::AuthenticatedSymmetricCipher> opener;
};

Session::Session()
: m_stream(new StreamState)
{
}

Session::~Session() = default;

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

void Session::Reset()
{
	m_active = false;
	m_cipher = Cipher_None;
	m_txKey.clear();
	m_rxKey.clear();
	std::memset(m_txPrefix, 0, sizeof(m_txPrefix));
	std::memset(m_rxPrefix, 0, sizeof(m_rxPrefix));
	m_txCounter = 0;
	m_rxCounter = 0;
	m_stream->sealer.reset();
	m_stream->opener.reset();
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

bool Session::SealBegin()
{
	if (!m_active) {
		return false;
	}
	try {
		m_stream->sealer = MakeCipher(m_cipher, true);
		if (!m_stream->sealer) {
			return false;
		}
		const std::vector<uint8_t> nonce = BuildNonce(m_txPrefix, m_txCounter);
		m_stream->sealer->SetKeyWithIV(m_txKey.data(), m_txKey.size(), nonce.data(), nonce.size());
		return true;
	} catch (const CryptoPP::Exception &) {
		m_stream->sealer.reset();
		return false;
	}
}

bool Session::SealUpdate(uint8_t *data, size_t len)
{
	if (!m_stream->sealer) {
		return false;
	}
	if (len == 0) {
		return true;
	}
	try {
		m_stream->sealer->ProcessString(data, len);
		return true;
	} catch (const CryptoPP::Exception &) {
		m_stream->sealer.reset();
		return false;
	}
}

bool Session::SealFinal(uint8_t *tagOut)
{
	if (!m_stream->sealer) {
		return false;
	}
	try {
		m_stream->sealer->Final(tagOut);
		m_stream->sealer.reset();
		++m_txCounter;
		return true;
	} catch (const CryptoPP::Exception &) {
		m_stream->sealer.reset();
		return false;
	}
}

bool Session::OpenBegin()
{
	if (!m_active) {
		return false;
	}
	try {
		m_stream->opener = MakeCipher(m_cipher, false);
		if (!m_stream->opener) {
			return false;
		}
		const std::vector<uint8_t> nonce = BuildNonce(m_rxPrefix, m_rxCounter);
		m_stream->opener->SetKeyWithIV(m_rxKey.data(), m_rxKey.size(), nonce.data(), nonce.size());
		return true;
	} catch (const CryptoPP::Exception &) {
		m_stream->opener.reset();
		return false;
	}
}

bool Session::OpenUpdate(uint8_t *data, size_t len)
{
	if (!m_stream->opener) {
		return false;
	}
	if (len == 0) {
		return true;
	}
	try {
		m_stream->opener->ProcessString(data, len);
		return true;
	} catch (const CryptoPP::Exception &) {
		m_stream->opener.reset();
		return false;
	}
}

bool Session::OpenFinal(const uint8_t *tag)
{
	if (!m_stream->opener) {
		return false;
	}
	bool ok = false;
	try {
		ok = m_stream->opener->Verify(tag);
	} catch (const CryptoPP::Exception &) {
		ok = false;
	}
	m_stream->opener.reset();
	if (ok) {
		++m_rxCounter;
	}
	// On failure the counter deliberately does not advance: the stream is no
	// longer trustworthy and the caller drops the connection.
	return ok;
}

bool Session::Seal(const uint8_t *plain, size_t len, std::vector<uint8_t> &out)
{
	if (!SealBegin()) {
		return false;
	}
	out.assign(plain, plain + len);
	out.resize(len + AEAD_TAG_LEN, 0);
	if (!SealUpdate(out.data(), len) || !SealFinal(out.data() + len)) {
		out.clear();
		return false;
	}
	return true;
}

bool Session::Open(const uint8_t *sealed, size_t len, std::vector<uint8_t> &out)
{
	if (len < AEAD_TAG_LEN) {
		return false;
	}
	const size_t bodyLen = len - AEAD_TAG_LEN;
	if (!OpenBegin()) {
		return false;
	}
	out.assign(sealed, sealed + bodyLen);
	if (!OpenUpdate(out.data(), bodyLen) || !OpenFinal(sealed + bodyLen)) {
		out.clear();
		return false;
	}
	return true;
}

} // namespace ECCrypt
