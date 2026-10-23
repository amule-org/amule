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

#ifndef ECSRP_H
#define ECSRP_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CECPacket;

namespace ECSRP
{
using Bytes = std::vector<uint8_t>;
constexpr size_t WIDTH = 384;
inline const Bytes PROFILE_BYTES{ 1, 15, 8, 1 };
enum Credential : uint8_t
{
	Password = 0,
	ApiToken = 1,
	WebToken = 2
};
// v1 deliberately supports one bounded profile. Never allocate using peer costs.
const char *Profile();
struct Verifier
{
	Bytes salt;
	Bytes value;
	std::string Encode() const;
	static Verifier Decode(const std::string &text);
	static Verifier Create(const std::string &digest);
	static Verifier Create(const std::string &digest, const Bytes &salt);
	bool Matches(const std::string &digest) const;
	bool Valid() const;
};

// Arithmetic is parameterized for the RFC 5054 Appendix B SHA-1/1024-bit test.
// Production callers use only the fixed v1 constructor defaults.
struct Arithmetic
{
	std::string modulus;
	unsigned int generator = 5;
	bool sha1 = false;
	Arithmetic();
	Bytes Public(const Bytes &exponent) const;
	Bytes ServerPublic(const Bytes &verifier, const Bytes &b) const;
	Bytes ClientSecret(const Bytes &A, const Bytes &B, const Bytes &a, const Bytes &x) const;
	Bytes ServerSecret(const Bytes &A, const Bytes &B, const Bytes &b, const Bytes &v) const;
};

class Client
{
public:
	Client();
	~Client();
	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;
	Bytes Start();
	Bytes Respond(const std::string &digest,
		uint8_t credential,
		const Bytes &salt,
		const Bytes &B,
		const Bytes &transcript);
	bool Verify(const Bytes &proof);
	void Reset();

private:
	Bytes m_a, m_A, m_expected;
};

class Server
{
public:
	Server() = default;
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;
	Bytes Start(const Verifier &verifier, uint8_t credential, const Bytes &A, const Bytes &transcript);
	Bytes Verify(const Bytes &proof);
	void Reset();

private:
	Bytes m_b, m_A, m_B, m_transcript;
	Verifier m_verifier;
	uint8_t m_credential = Password;
};

Bytes Field(const CECPacket &packet, uint16_t name, size_t size);
// Accept the non-empty password digests supported by the production clients.
bool IsPasswordDigest(const std::string &digest);
Bytes PasswordInput(const std::string &digest, const Bytes &salt);
struct Proofs
{
	Bytes client, server;
};
Proofs Evidence(const Bytes &shared,
	uint8_t credential,
	const Bytes &salt,
	const Bytes &transcript,
	const Bytes &A,
	const Bytes &B);
// Strict, fixed-size decoding; no secret material is returned on malformed input.
Bytes Unhex(const std::string &text, size_t size);
std::string Hex(const Bytes &bytes);
} // namespace ECSRP
#endif
