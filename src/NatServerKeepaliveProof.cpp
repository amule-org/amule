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

#include "NatServerKeepaliveProof.h"

#ifdef ENABLE_NATT_SERVER_COORDINATION
#include "CryptoPP_Inc.h"
#include "WarningsPush_CryptoPP.h"
#include CRYPTO_HEADER(hmac.h)
#include "WarningsPop.h"

namespace NatServerHolePunch
{

std::vector<std::uint8_t> BuildKeepaliveProof(
	const Hash &ownUserHash, const Hash &sessionToken, std::uint32_t counter)
{
	std::vector<std::uint8_t> payload(kKeepaliveProofSize);
	std::copy(ownUserHash.begin(), ownUserHash.end(), payload.begin());
	const std::size_t signedSize = ownUserHash.size() + 4;
	Detail::WriteLE(payload.data() + ownUserHash.size(), counter, 4);
	CryptoPP::HMAC<CryptoPP::SHA256> hmac(sessionToken.data(), sessionToken.size());
	hmac.CalculateTruncatedDigest(
		payload.data() + signedSize, kKeepaliveProofMacSize, payload.data(), signedSize);
	return payload;
}

} // namespace NatServerHolePunch
#endif // ENABLE_NATT_SERVER_COORDINATION
// File_checked_for_headers
