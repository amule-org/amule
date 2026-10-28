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
 * Whether an inbound connection that opened with a plain ed2k header must be refused.
 *
 * A transport that already obfuscates its stream satisfies "require encryption". A connection used
 * for a server or Kad firewall check is exempt, so refusing it cannot leave a node LowID, unless
 * the strict option ignores even those.
 */
constexpr bool MustRefusePlainConnection(
	bool required, bool strict, bool transportObfuscates, bool isTestConnection)
{
	return required && !transportObfuscates && (strict || !isTestConnection);
}
} // namespace EncryptionRequirement

#endif // AMULE_ENCRYPTION_REQUIREMENT_H
