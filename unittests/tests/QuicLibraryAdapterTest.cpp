//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include <QuicLibraryAdapter.h>
#include <array>
#include <vector>
using namespace muleunit;
DECLARE_SIMPLE(QuicLibraryAdapterPolicy)
DECLARE_SIMPLE(QuicLibraryAdapterProof)
namespace
{
struct Credentials : IQuicTlsCredentials
{
};
struct Verifier : IQuicTlsVerifier
{
};
struct Session : IQuicTlsSession
{
	bool accepted = true, called = false, tls13 = false, mandatory = false;
	std::vector<uint8_t> alpn;
	bool ConfigureTls13Alpn(const IQuicTlsCredentials &,
		const IQuicTlsVerifier *verifier,
		const uint8_t *name,
		size_t length,
		bool isMandatory) override
	{
		called = true;
		tls13 = true;
		mandatory = isMandatory;
		alpn.assign(name, name + length);
		return accepted && verifier != nullptr;
	}
};
struct Events : IQuicLibraryCallbacks
{
	int connected = 0, payload = 0, auth = 0;
	void OnQuicConnected() override { ++connected; }
	void OnQuicPayload(const uint8_t *, size_t) override { ++payload; }
	void OnQuicAuthenticationFailure() override { ++auth; }
};
std::array<uint8_t, 16> Id(uint8_t x)
{
	std::array<uint8_t, 16> v{};
	v[0] = x;
	return v;
}
} // namespace
TEST(QuicLibraryAdapterPolicy, RejectsMissingTlsSessionOrPolicy)
{
	Events e;
	CQuicTlsPolicy policy;
	CQuicLibraryAdapter a(policy, Id(1), nullptr, &e);
	ASSERT_TRUE(!a.Configure());
	ASSERT_TRUE(!a.IsReady());
}
TEST(QuicLibraryAdapterPolicy, RequiresExactTls13MandatoryAlpnSetup)
{
	Events e;
	Credentials c;
	Verifier v;
	Session s;
	CQuicTlsPolicy policy{ &s, &c, &v };
	CQuicLibraryAdapter a(policy, Id(1), nullptr, &e);
	ASSERT_TRUE(a.Configure());
	ASSERT_TRUE(s.called);
	ASSERT_TRUE(s.tls13);
	ASSERT_TRUE(s.mandatory);
	ASSERT_TRUE(QuicNatt::IsQuicNattAlpn(s.alpn.data(), s.alpn.size()));
	const uint8_t wrong[] = "ed2k-ai-natt-quic-v2";
	ASSERT_TRUE(!a.OnHandshakeComplete(wrong, sizeof(wrong) - 1));
	ASSERT_EQUALS(0, e.connected);
}
TEST(QuicLibraryAdapterPolicy, RefusedTlsConfigurationFailsClosed)
{
	Events e;
	Credentials c;
	Verifier v;
	Session s;
	s.accepted = false;
	CQuicTlsPolicy policy{ &s, &c, &v };
	CQuicLibraryAdapter a(policy, Id(1), nullptr, &e);
	ASSERT_TRUE(!a.Configure());
	ASSERT_TRUE(!a.OnHandshakeComplete(nullptr, 0));
}
TEST(QuicLibraryAdapterProof, RejectsInvalidProofBeforeCallbacks)
{
	Events e;
	Credentials c;
	Verifier v;
	Session s;
	CQuicTlsPolicy policy{ &s, &c, &v };
	CQuicLibraryAdapter a(policy, Id(1), nullptr, &e);
	ASSERT_TRUE(a.Configure());
	const uint8_t alpn[] = "ed2k-ai-natt-quic-v1";
	ASSERT_TRUE(a.OnHandshakeComplete(alpn, sizeof(alpn) - 1));
	const uint8_t malformed[QuicNatt::EAQN1_PROOF_SIZE] = {};
	ASSERT_TRUE(!a.OnStreamData(malformed, sizeof(malformed)));
	ASSERT_EQUALS(0, e.connected);
	ASSERT_EQUALS(0, e.payload);
	ASSERT_EQUALS(1, e.auth);
}
TEST(QuicLibraryAdapterProof, AcceptsProofThenPayload)
{
	Events e;
	Credentials c;
	Verifier v;
	Session s;
	CQuicTlsPolicy policy{ &s, &c, &v };
	auto local = Id(1);
	auto peer = Id(2);
	CQuicLibraryAdapter a(policy, peer, &local, &e);
	ASSERT_TRUE(a.Configure());
	const uint8_t alpn[] = "ed2k-ai-natt-quic-v1";
	ASSERT_TRUE(a.OnHandshakeComplete(alpn, sizeof(alpn) - 1));
	auto proof = QuicNatt::BuildEaqn1Proof(local, &peer);
	ASSERT_TRUE(a.OnStreamData(proof.data(), proof.size()));
	ASSERT_EQUALS(1, e.connected);
	const uint8_t payload[] = { 7 };
	ASSERT_TRUE(a.OnStreamData(payload, 1));
	ASSERT_EQUALS(1, e.payload);
}
