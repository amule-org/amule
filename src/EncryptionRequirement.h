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

#ifndef AMULE_ENCRYPTION_REQUIREMENT_H
#define AMULE_ENCRYPTION_REQUIREMENT_H

namespace EncryptionRequirement
{
/**
 * Whether an inbound connection that opened with a plain ed2k header must be refused while
 * "require encryption" is on.
 *
 * A transport that already obfuscates its stream satisfies "require encryption". A connection used
 * for a server or Kad firewall check is exempt, so refusing it cannot leave a node LowID, unless
 * the strict option ignores even those.
 */
constexpr bool MustRefusePlainConnection(bool strict, bool transportObfuscates, bool isTestConnection)
{
	return !transportObfuscates && (strict || !isTestConnection);
}

//! Our crypt settings and a peer's, for deciding whether we may dial it.
struct DialFacts
{
	bool weSupport = false;
	bool weRequire = false;
	bool weRequireStrict = false;
	bool peerSupports = false;
	bool peerRequires = false;
	//! A Kad UDP firewall check. Its contact has no user hash, so the connection can only be plain.
	bool isUdpFirewallCheck = false;
};

/**
 * Whether the crypt settings on either side rule out dialling the peer.
 *
 * The Kad UDP firewall check is exempt from our "require encryption", like inbound test
 * connections. Refusing it leaves Kad UDP unverified, and then we refuse every buddy request.
 */
constexpr bool CryptSettingsForbidDial(const DialFacts &f)
{
	return (f.peerRequires && !f.weSupport) ||
	       (f.weRequire && !f.peerSupports &&
		       MustRefusePlainConnection(f.weRequireStrict, false, f.isUdpFirewallCheck));
}
} // namespace EncryptionRequirement

#endif // AMULE_ENCRYPTION_REQUIREMENT_H
