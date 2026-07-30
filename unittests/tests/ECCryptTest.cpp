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
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

//
// Coverage for the EC transport-encryption primitives.
//
// The properties here fail *silently* if they ever regress -- a reused nonce,
// a key derivation that stops mixing in one of its inputs, or a tag that stops
// being checked all still encrypt and decrypt happily while providing much
// less than they claim. So each one is asserted rather than assumed.
//
// Every test runs against every cipher the build supports, because which ones
// exist depends on the cryptopp version (ChaCha20-Poly1305 needs 8.1).
//

#include <muleunit/test.h>

#include "ECCrypt.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace muleunit;
using namespace ECCrypt;

DECLARE(ECCrypt)
static std::vector<uint8_t> Fill(size_t n, uint8_t v)
{
	return std::vector<uint8_t>(n, v);
}

/// Decode a hex string into bytes, so test vectors stay readable as vectors.
static std::vector<uint8_t> FromHex(const std::string &hex)
{
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), NULL, 16)));
	}
	return out;
}

static std::vector<uint8_t> Bytes(const std::string &s)
{
	return std::vector<uint8_t>(s.begin(), s.end());
}

/// A matched client/server pair sharing one secret.
static void MakePair(uint8_t cipher, Session &cli, Session &srv, uint8_t secret = 7)
{
	const std::vector<uint8_t> ikm = Fill(32, secret);
	const std::vector<uint8_t> sn = Fill(NONCE_TAG_LEN, 1);
	const std::vector<uint8_t> cn = Fill(NONCE_TAG_LEN, 2);
	const std::vector<uint8_t> transcript = Bytes("transcript");
	ASSERT_TRUE(cli.Init(cipher, ikm, sn, cn, transcript, false));
	ASSERT_TRUE(srv.Init(cipher, ikm, sn, cn, transcript, true));
}
END_DECLARE;

// --- HKDF -----------------------------------------------------------------

TEST(ECCrypt, HkdfMatchesRfc5869TestCase1)
{
	// RFC 5869 appendix A.1. An independent vector, not a self-consistency
	// check: this is what proves the hand-rolled extract/expand is really
	// HKDF and not merely stable.
	const std::vector<uint8_t> ikm = Fill(22, 0x0b);
	std::vector<uint8_t> salt, info;
	for (int i = 0x00; i <= 0x0c; ++i) {
		salt.push_back(static_cast<uint8_t>(i));
	}
	for (int i = 0xf0; i <= 0xf9; ++i) {
		info.push_back(static_cast<uint8_t>(i));
	}
	// Expected OKM, as hex: a byte-array literal here gets exploded to one
	// element per line by clang-format, which buries the vector it encodes.
	const std::vector<uint8_t> expected =
		FromHex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
			"34007208d5b887185865");

	const std::vector<uint8_t> okm = HkdfSha256(ikm, salt, info, expected.size());
	ASSERT_EQUALS((size_t)42, expected.size());
	ASSERT_TRUE(okm == expected);
}

TEST(ECCrypt, HkdfIsSensitiveToEveryInput)
{
	const std::vector<uint8_t> ikm = Fill(16, 1), salt = Fill(16, 2), info = Fill(4, 3);
	const std::vector<uint8_t> base = HkdfSha256(ikm, salt, info, 32);
	ASSERT_EQUALS((size_t)32, base.size());

	ASSERT_TRUE(HkdfSha256(Fill(16, 9), salt, info, 32) != base);
	ASSERT_TRUE(HkdfSha256(ikm, Fill(16, 9), info, 32) != base);
	ASSERT_TRUE(HkdfSha256(ikm, salt, Fill(4, 9), 32) != base);
	// Different length must not just be a prefix cut of the same stream --
	// it is, per RFC 5869, but assert the length contract explicitly.
	ASSERT_EQUALS((size_t)64, HkdfSha256(ikm, salt, info, 64).size());
}

TEST(ECCrypt, HkdfRejectsAbsurdLengths)
{
	const std::vector<uint8_t> ikm = Fill(16, 1);
	ASSERT_TRUE(HkdfSha256(ikm, ikm, ikm, 0).empty());
	ASSERT_TRUE(HkdfSha256(ikm, ikm, ikm, 255 * 32 + 1).empty());
}

// --- cipher availability --------------------------------------------------

TEST(ECCrypt, AesGcmIsAlwaysAvailable)
{
	// The mandatory baseline: it must exist in every supported cryptopp, or a
	// connection could negotiate nothing at all.
	ASSERT_TRUE(IsCipherSupported(Cipher_AES128_GCM));
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	ASSERT_TRUE(!ciphers.empty());
	bool hasAes = false;
	for (size_t i = 0; i < ciphers.size(); ++i) {
		if (ciphers[i] == Cipher_AES128_GCM) {
			hasAes = true;
		}
	}
	ASSERT_TRUE(hasAes);
	ASSERT_TRUE(!IsCipherSupported(Cipher_None));
	ASSERT_TRUE(!IsCipherSupported(0xFF));
}

TEST(ECCrypt, PreferredCipherComesFirst)
{
	// The daemon picks the first mutually supported entry, so ordering is the
	// preference. Where ChaCha exists it must outrank AES: without hardware
	// AES it is substantially faster, which is the Raspberry Pi case.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	if (IsCipherSupported(Cipher_ChaCha20_Poly1305)) {
		ASSERT_EQUALS((int)Cipher_ChaCha20_Poly1305, (int)ciphers[0]);
	} else {
		ASSERT_EQUALS((int)Cipher_AES128_GCM, (int)ciphers[0]);
	}
}

// --- session setup --------------------------------------------------------

TEST(ECCrypt, InitRejectsBadParameters)
{
	Session s;
	const std::vector<uint8_t> ikm = Fill(32, 7);
	const std::vector<uint8_t> good = Fill(NONCE_TAG_LEN, 1);
	const std::vector<uint8_t> shortNonce = Fill(NONCE_TAG_LEN - 1, 1);
	const std::vector<uint8_t> empty;

	ASSERT_TRUE(!s.Init(Cipher_None, ikm, good, good, empty, false));
	ASSERT_TRUE(!s.Init(0xFF, ikm, good, good, empty, false));
	ASSERT_TRUE(!s.Init(Cipher_AES128_GCM, ikm, shortNonce, good, empty, false));
	ASSERT_TRUE(!s.Init(Cipher_AES128_GCM, ikm, good, shortNonce, empty, false));
	ASSERT_TRUE(!s.IsActive());
}

TEST(ECCrypt, ResetClearsKeyMaterial)
{
	// A socket object can be reused for a fresh connection; carrying keys or
	// counters across would reuse a nonce.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);
		ASSERT_TRUE(cli.IsActive());
		cli.Reset();
		ASSERT_TRUE(!cli.IsActive());
		ASSERT_EQUALS((int)Cipher_None, (int)cli.GetCipher());

		std::vector<uint8_t> out;
		ASSERT_TRUE(!cli.Seal((const uint8_t *)"x", 1, out));
	}
}

// --- round trip -----------------------------------------------------------

TEST(ECCrypt, RoundTripBothDirections)
{
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);

		const std::string msg = "EC_OP_STAT_REQ body";
		std::vector<uint8_t> sealed, opened;

		ASSERT_TRUE(cli.Seal((const uint8_t *)msg.data(), msg.size(), sealed));
		ASSERT_EQUALS(msg.size() + Session::Overhead(), sealed.size());
		// The plaintext must not survive in the ciphertext.
		ASSERT_TRUE(std::memcmp(sealed.data(), msg.data(), msg.size()) != 0);
		ASSERT_TRUE(srv.Open(sealed.data(), sealed.size(), opened));
		ASSERT_EQUALS(msg.size(), opened.size());
		ASSERT_TRUE(std::memcmp(opened.data(), msg.data(), msg.size()) == 0);

		// and back the other way, which uses the opposite key
		std::vector<uint8_t> sealed2, opened2;
		ASSERT_TRUE(srv.Seal((const uint8_t *)msg.data(), msg.size(), sealed2));
		ASSERT_TRUE(cli.Open(sealed2.data(), sealed2.size(), opened2));
		ASSERT_TRUE(std::memcmp(opened2.data(), msg.data(), msg.size()) == 0);
	}
}

TEST(ECCrypt, DirectionKeysDiffer)
{
	// Both directions share a counter space; if they shared a key too, the
	// two streams would reuse nonces against the same key.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);

		const std::string msg = "same plaintext, same counter";
		std::vector<uint8_t> fromClient, fromServer;
		ASSERT_TRUE(cli.Seal((const uint8_t *)msg.data(), msg.size(), fromClient));
		ASSERT_TRUE(srv.Seal((const uint8_t *)msg.data(), msg.size(), fromServer));
		ASSERT_EQUALS(fromClient.size(), fromServer.size());
		ASSERT_TRUE(fromClient != fromServer);
	}
}

TEST(ECCrypt, CounterAdvancesSoIdenticalPlaintextsDiffer)
{
	// The nonce is implicit, so nothing on the wire would reveal a counter
	// that stopped advancing -- but the ciphertext would repeat.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);

		const std::string msg = "identical";
		std::vector<uint8_t> a, b;
		ASSERT_TRUE(cli.Seal((const uint8_t *)msg.data(), msg.size(), a));
		ASSERT_TRUE(cli.Seal((const uint8_t *)msg.data(), msg.size(), b));
		ASSERT_TRUE(a != b);

		std::vector<uint8_t> o1, o2;
		ASSERT_TRUE(srv.Open(a.data(), a.size(), o1));
		ASSERT_TRUE(srv.Open(b.data(), b.size(), o2));
		ASSERT_TRUE(o1 == o2);
	}
}

TEST(ECCrypt, ManyPacketsStayInLockstep)
{
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);
		for (int n = 0; n < 250; ++n) {
			char buf[40];
			const int len = snprintf(buf, sizeof(buf), "packet %d", n);
			std::vector<uint8_t> sealed, opened;
			ASSERT_TRUE(cli.Seal((const uint8_t *)buf, (size_t)len, sealed));
			ASSERT_TRUE(srv.Open(sealed.data(), sealed.size(), opened));
			ASSERT_EQUALS((size_t)len, opened.size());
			ASSERT_TRUE(std::memcmp(opened.data(), buf, (size_t)len) == 0);
		}
	}
}

TEST(ECCrypt, EmptyBodyIsLegal)
{
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);
		std::vector<uint8_t> sealed, opened;
		ASSERT_TRUE(cli.Seal(NULL, 0, sealed));
		ASSERT_EQUALS(Session::Overhead(), sealed.size());
		ASSERT_TRUE(srv.Open(sealed.data(), sealed.size(), opened));
		ASSERT_TRUE(opened.empty());
	}
}

// --- the properties that matter -------------------------------------------

TEST(ECCrypt, TamperedCiphertextIsRejected)
{
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		for (int flip = 0; flip < 3; ++flip) {
			Session cli, srv;
			MakePair(ciphers[i], cli, srv);
			const std::string msg = "a packet worth modifying in flight";
			std::vector<uint8_t> sealed, opened;
			ASSERT_TRUE(cli.Seal((const uint8_t *)msg.data(), msg.size(), sealed));

			// first byte of body, last byte of body, and a byte of the tag
			const size_t body = sealed.size() - Session::Overhead();
			const size_t at = (flip == 0) ? 0 : (flip == 1) ? body - 1 : body + 3;
			sealed[at] ^= 0x01;
			ASSERT_TRUE(!srv.Open(sealed.data(), sealed.size(), opened));
		}
	}
}

TEST(ECCrypt, TruncatedBodyIsRejected)
{
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);
		std::vector<uint8_t> sealed, opened;
		ASSERT_TRUE(cli.Seal((const uint8_t *)"abcdefgh", 8, sealed));

		ASSERT_TRUE(!srv.Open(sealed.data(), sealed.size() - 1, opened));
		// Shorter than a bare tag must be refused, not read out of bounds.
		ASSERT_TRUE(!srv.Open(sealed.data(), Session::Overhead() - 1, opened));
		ASSERT_TRUE(!srv.Open(sealed.data(), 0, opened));
	}
}

TEST(ECCrypt, WrongSharedSecretCannotOpen)
{
	// This is what defeats a man in the middle: without the password the
	// attacker cannot derive the key, so it cannot forge or read anything.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv, impostor;
		MakePair(ciphers[i], cli, srv, 7);
		const std::vector<uint8_t> wrong = Fill(32, 8);
		ASSERT_TRUE(impostor.Init(ciphers[i],
			wrong,
			Fill(NONCE_TAG_LEN, 1),
			Fill(NONCE_TAG_LEN, 2),
			Bytes("transcript"),
			true));

		std::vector<uint8_t> sealed, opened;
		ASSERT_TRUE(cli.Seal((const uint8_t *)"secret", 6, sealed));
		ASSERT_TRUE(!impostor.Open(sealed.data(), sealed.size(), opened));
	}
}

TEST(ECCrypt, TamperedTranscriptCannotOpen)
{
	// The downgrade defence. If an attacker edits the capability exchange the
	// two ends derive different keys, so the first sealed packet fails rather
	// than the connection quietly falling back to something weaker.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		const std::vector<uint8_t> ikm = Fill(32, 7);
		const std::vector<uint8_t> sn = Fill(NONCE_TAG_LEN, 1);
		const std::vector<uint8_t> cn = Fill(NONCE_TAG_LEN, 2);
		Session cli, srv;
		ASSERT_TRUE(cli.Init(ciphers[i], ikm, sn, cn, Bytes("offered: chacha,aes"), false));
		ASSERT_TRUE(srv.Init(ciphers[i], ikm, sn, cn, Bytes("offered: aes"), true));

		std::vector<uint8_t> sealed, opened;
		ASSERT_TRUE(cli.Seal((const uint8_t *)"payload", 7, sealed));
		ASSERT_TRUE(!srv.Open(sealed.data(), sealed.size(), opened));
	}
}

TEST(ECCrypt, DifferentNoncesGiveDifferentKeys)
{
	// Each connection must be independent, so a recording of one session is
	// useless against another even with the same password.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		const std::vector<uint8_t> ikm = Fill(32, 7);
		Session a, b;
		ASSERT_TRUE(a.Init(ciphers[i],
			ikm,
			Fill(NONCE_TAG_LEN, 1),
			Fill(NONCE_TAG_LEN, 2),
			std::vector<uint8_t>(),
			false));
		ASSERT_TRUE(b.Init(ciphers[i],
			ikm,
			Fill(NONCE_TAG_LEN, 3),
			Fill(NONCE_TAG_LEN, 2),
			std::vector<uint8_t>(),
			true));

		std::vector<uint8_t> sealed, opened;
		ASSERT_TRUE(a.Seal((const uint8_t *)"payload", 7, sealed));
		ASSERT_TRUE(!b.Open(sealed.data(), sealed.size(), opened));
	}
}

TEST(ECCrypt, FailedOpenHoldsTheCounter)
{
	// A rejected packet must not consume a counter slot, or one dropped
	// forgery would desynchronise the stream and break every later packet.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);
		std::vector<uint8_t> sealed, opened;
		ASSERT_TRUE(cli.Seal((const uint8_t *)"body", 4, sealed));

		std::vector<uint8_t> corrupted = sealed;
		corrupted[1] ^= 0x01;
		ASSERT_TRUE(!srv.Open(corrupted.data(), corrupted.size(), opened));
		// The pristine copy of the same packet must still open.
		ASSERT_TRUE(srv.Open(sealed.data(), sealed.size(), opened));
		ASSERT_EQUALS((size_t)4, opened.size());
	}
}

// --- streaming ------------------------------------------------------------

TEST(ECCrypt, StreamedSealMatchesOneShot)
{
	// The socket path seals CQueuedData chunks in place; it must produce
	// exactly what the one-shot call would, or the two would be incompatible.
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session streamed, oneshot, dummy;
		MakePair(ciphers[i], streamed, dummy);
		MakePair(ciphers[i], oneshot, dummy);

		const char *parts[3] = { "EC_OP_SHARED_FILES|", "second chunk|", "third chunk tail" };
		std::string flat;
		std::vector<std::vector<uint8_t>> chunks;
		for (int p = 0; p < 3; ++p) {
			flat += parts[p];
			chunks.push_back(Bytes(parts[p]));
		}

		ASSERT_TRUE(streamed.SealBegin());
		for (size_t c = 0; c < chunks.size(); ++c) {
			ASSERT_TRUE(streamed.SealUpdate(chunks[c].data(), chunks[c].size()));
		}
		uint8_t tag[AEAD_TAG_LEN];
		ASSERT_TRUE(streamed.SealFinal(tag));

		std::vector<uint8_t> joined;
		for (size_t c = 0; c < chunks.size(); ++c) {
			joined.insert(joined.end(), chunks[c].begin(), chunks[c].end());
		}
		// In-place: the ciphertext is exactly as long as the plaintext.
		ASSERT_EQUALS(flat.size(), joined.size());

		std::vector<uint8_t> ref;
		ASSERT_TRUE(oneshot.Seal((const uint8_t *)flat.data(), flat.size(), ref));
		ASSERT_EQUALS(joined.size() + AEAD_TAG_LEN, ref.size());
		ASSERT_TRUE(std::memcmp(ref.data(), joined.data(), joined.size()) == 0);
		ASSERT_TRUE(std::memcmp(ref.data() + joined.size(), tag, AEAD_TAG_LEN) == 0);
	}
}

TEST(ECCrypt, StreamedOpenRecoversChunkedBody)
{
	const std::vector<uint8_t> ciphers = SupportedCiphers();
	for (size_t i = 0; i < ciphers.size(); ++i) {
		Session cli, srv;
		MakePair(ciphers[i], cli, srv);

		const std::string flat = "a body that arrives split across reads";
		std::vector<uint8_t> buf = Bytes(flat);
		uint8_t tag[AEAD_TAG_LEN];
		ASSERT_TRUE(cli.SealBegin());
		ASSERT_TRUE(cli.SealUpdate(buf.data(), buf.size()));
		ASSERT_TRUE(cli.SealFinal(tag));

		// open in two pieces, as the receive buffer might be walked
		const size_t half = buf.size() / 2;
		ASSERT_TRUE(srv.OpenBegin());
		ASSERT_TRUE(srv.OpenUpdate(buf.data(), half));
		ASSERT_TRUE(srv.OpenUpdate(buf.data() + half, buf.size() - half));
		ASSERT_TRUE(srv.OpenFinal(tag));
		ASSERT_TRUE(std::memcmp(buf.data(), flat.data(), flat.size()) == 0);
	}
}

TEST(ECCrypt, StreamingCallsRefuseWhenInactive)
{
	Session s;
	uint8_t tag[AEAD_TAG_LEN] = { 0 };
	std::vector<uint8_t> buf(4, 0);
	ASSERT_TRUE(!s.SealBegin());
	ASSERT_TRUE(!s.SealUpdate(buf.data(), buf.size()));
	ASSERT_TRUE(!s.SealFinal(tag));
	ASSERT_TRUE(!s.OpenBegin());
	ASSERT_TRUE(!s.OpenUpdate(buf.data(), buf.size()));
	ASSERT_TRUE(!s.OpenFinal(tag));
}

// --- randomness -----------------------------------------------------------

TEST(ECCrypt, RandomBytesAreTheRightSizeAndNotConstant)
{
	const std::vector<uint8_t> a = RandomBytes(NONCE_TAG_LEN);
	const std::vector<uint8_t> b = RandomBytes(NONCE_TAG_LEN);
	ASSERT_EQUALS(NONCE_TAG_LEN, a.size());
	ASSERT_EQUALS(NONCE_TAG_LEN, b.size());
	// Two draws colliding would mean the generator is not generating.
	ASSERT_TRUE(a != b);

	bool allZero = true;
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i] != 0) {
			allZero = false;
		}
	}
	ASSERT_TRUE(!allZero);
	ASSERT_TRUE(RandomBytes(0).empty());
}
