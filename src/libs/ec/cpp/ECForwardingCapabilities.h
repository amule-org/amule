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
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
// If not, see <https://www.gnu.org/licenses/>.
//

#ifndef EC_FORWARDING_CAPABILITIES_H
#define EC_FORWARDING_CAPABILITIES_H

#include "ECTag.h"   // Needed for CECTag
#include "ECCodes.h" // Needed for EC_TAG_GENERAL_*_AVAILABLE

/**
 * Forwarding-capability signals for the GENERAL preferences packet.
 *
 * This header is the single seam shared by the real CEC_Prefs_Packet constructor
 * (emission, core side) and its CLIENT_GUI apply path (decoding, remote GUI side),
 * and by ECPcpCapabilityTest. Both directions delegate to the same functions so the
 * wire shape tested is the wire shape produced.
 *
 * Capability is a compile-time property of each build: these inline functions expand
 * where they are inlined, so every app sees its own defines. Header-only on purpose:
 * the ec library is built once without app defines, while the capability depends on
 * the including build.
 */
namespace ECForwardingCapabilities
{

/** Whether this build can perform UPnP port forwarding (ENABLE_UPNP). */
inline bool UpnpAvailable()
{
#ifdef ENABLE_UPNP
	return true;
#else
	return false;
#endif
}

/**
 * Whether this build can perform IPv6 PCP forwarding: IPv6 compiled in, a core
 * build (the remote GUI must not advertise its own capability as the daemon's)
 * and a platform with a supported PCP backend.
 */
inline bool PcpAvailable()
{
#if defined(ENABLE_IPV6) && !defined(CLIENT_GUI) && \
	(defined(__linux__) || defined(__APPLE__) || defined(_WIN32))
	return true;
#else
	return false;
#endif
}

/**
 * Adds the UPnP and PCP availability tags to a GENERAL preferences tag. Both tags are
 * always emitted as bools by 3.1+ daemons, so a client can tell "compiled out" (false)
 * from an old daemon predating the tag (absent). The two tags stay distinct signals:
 * UPnP-specific controls gate on the UPnP tag alone.
 */
inline void AddCapabilityTags(CECTag &generalPrefs)
{
	generalPrefs.AddTag(CECTag(EC_TAG_GENERAL_UPNP_AVAILABLE, UpnpAvailable()));
	generalPrefs.AddTag(CECTag(EC_TAG_GENERAL_PCP_AVAILABLE, PcpAvailable()));
}

/**
 * Reads the capability tags of a received GENERAL preferences tag and reports each
 * result through the matching setter. Legacy semantics: a pre-3.1 daemon omits the
 * tags, so absence means unavailable and only an explicit true enables. The setters
 * are called exactly once per capability, in every case.
 */
inline void ApplyCapabilityTags(
	const CECTag &generalPrefs, void (*setUpnpAvailable)(bool), void (*setPcpAvailable)(bool))
{
	if (const CECTag *upnp = generalPrefs.GetTagByName(EC_TAG_GENERAL_UPNP_AVAILABLE)) {
		setUpnpAvailable(upnp->GetInt() != 0);
	} else {
		setUpnpAvailable(false);
	}
	if (const CECTag *pcp = generalPrefs.GetTagByName(EC_TAG_GENERAL_PCP_AVAILABLE)) {
		setPcpAvailable(pcp->GetInt() != 0);
	} else {
		setPcpAvailable(false);
	}
}

} // namespace ECForwardingCapabilities

#endif /* EC_FORWARDING_CAPABILITIES_H */
