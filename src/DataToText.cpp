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

#include <wx/intl.h>

#include "DataToText.h"
#include "Constants.h"
#include "SourceRoutePolicy.h" // Needed for EContactRouteReport
#include <common/Format.h>     // Needed for CFormat
#include <protocol/ed2k/ClientSoftware.h>

wxString PriorityToStr(int priority, bool isAuto)
{
	if (isAuto) {
		switch (priority) {
		case PR_LOW:
			return _("Auto [Lo]");
		case PR_NORMAL:
			return _("Auto [No]");
		case PR_HIGH:
			return _("Auto [Hi]");
		}
	} else {
		switch (priority) {
		case PR_VERY_LOW:
			return _("Very low");
		case PR_LOW:
			return _("Low");
		case PR_NORMAL:
			return _("Normal");
		case PR_HIGH:
			return _("High");
		case PR_VERYHIGH:
			return _("Very High");
		case PR_POWERSHARE:
			return _("Release");
		}
	}

	wxFAIL;

	return _("Unknown");
}

wxString DownloadStateToStr(int state, bool queueFull)
{
	switch (state) {
	case DS_CONNECTING:
		return _("Connecting");
	case DS_CONNECTED:
		return _("Asking");
	case DS_WAITCALLBACK:
		return _("Connecting via server");
	case DS_ONQUEUE:
		return (queueFull ? _("Queue Full") : _("On Queue"));
	case DS_DOWNLOADING:
		return _("Downloading");
	case DS_REQHASHSET:
		return _("Receiving hashset");
	case DS_NONEEDEDPARTS:
		return _("No needed parts");
	case DS_LOWTOLOWIP:
		return _("Cannot connect LowID to LowID");
	case DS_TOOMANYCONNS:
		return _("Too many connections");
	case DS_NONE:
		return _("Unknown");
	case DS_WAITCALLBACKKAD:
		return _("Connecting via Kad");
	case DS_TOOMANYCONNSKAD:
		return _("Too many Kad connections");
	case DS_BANNED:
		return _("Banned");
	case DS_ERROR:
		return _("Connection Error");
	case DS_REMOTEQUEUEFULL:
		return _("Remote Queue Full");
	}

	wxFAIL;

	return _("Unknown");
}

const wxString GetSoftName(unsigned int software_ident)
{
	switch (software_ident) {
	case SO_OLDEMULE:
	case SO_EMULE:
		return "eMule";
	case SO_CDONKEY:
		return "cDonkey";
	case SO_LXMULE:
		return "(l/x)Mule";
	case SO_AMULE:
		return "aMule";
	case SO_SHAREAZA:
	case SO_NEW_SHAREAZA:
	case SO_NEW2_SHAREAZA:
		return "Shareaza";
	case SO_EMULEPLUS:
		return "eMule+";
	case SO_HYDRANODE:
		return "HydraNode";
	case SO_MLDONKEY:
		return wxTRANSLATE("Old MLDonkey");
	case SO_NEW_MLDONKEY:
	case SO_NEW2_MLDONKEY:
		return wxTRANSLATE("New MLDonkey");
	case SO_LPHANT:
		return "lphant";
	case SO_EDONKEYHYBRID:
		return "eDonkeyHybrid";
	case SO_EDONKEY:
		return "eDonkey";
	case SO_UNKNOWN:
		return wxTRANSLATE("Unknown");
	case SO_COMPAT_UNK:
		return wxTRANSLATE("eMule Compatible");
	default:
		return "";
	}
}

wxString GetSoftName(unsigned int software_ident, const wxString &modVersion)
{
	if (software_ident != SO_EMULE && software_ident != SO_OLDEMULE && !modVersion.IsEmpty()) {
		return modVersion;
	}
	return GetSoftName(software_ident);
}

wxString FormatClientSoftware(const wxString &name, const wxString &modVersion, const wxString &version)
{
	wxString label = name;
	// ReGetClientSoft() already includes the mod in its decorated "Fake eMule" labels.
	if (!modVersion.IsEmpty() && name != modVersion &&
		!(name.StartsWith(modVersion + " (") && name.EndsWith(")"))) {
		label += " (" + modVersion + ")";
	}
	if (!version.IsEmpty()) {
		if (!label.IsEmpty()) {
			label += " ";
		}
		label += version;
	}
	return label;
}

wxString FormatClientSoftware(
	unsigned int software_ident, const wxString &modVersion, const wxString &version)
{
	return FormatClientSoftware(GetSoftName(software_ident, modVersion), modVersion, version);
}

wxString OriginToText(unsigned int source_from)
{
	switch ((ESourceFrom)source_from) {
	case SF_LOCAL_SERVER:
		return wxTRANSLATE("Local Server");
	case SF_REMOTE_SERVER:
		return wxTRANSLATE("Remote Server");
	case SF_KADEMLIA:
		return wxTRANSLATE("Kad");
	case SF_SOURCE_EXCHANGE:
		return wxTRANSLATE("Source Exchange");
	case SF_PASSIVE:
		return wxTRANSLATE("Passive");
	case SF_LINK:
		return wxTRANSLATE("Link");
	case SF_SOURCE_SEEDS:
		return wxTRANSLATE("Source Seeds");
	case SF_SEARCH_RESULT:
		return wxTRANSLATE("Search Result");
	case SF_NONE:
	default:
		return wxTRANSLATE("Unknown");
	}
}

/**
 * Every channel a source has been seen on, joined in enum order: "Local Server + Kad". @a primary
 * is its own ESourceFrom, which names the cell on its own while nothing has been merged in.
 */
wxString OriginsToText(unsigned int originMask, unsigned int primary)
{
	wxString joined;
	for (int from = SF_LOCAL_SERVER; from <= SF_SEARCH_RESULT; ++from) {
		if ((originMask & SourceFromBit((ESourceFrom)from)) == 0) {
			continue;
		}
		if (!joined.IsEmpty()) {
			joined += wxT(" + ");
		}
		joined += wxGetTranslation(OriginToText(from));
	}
	return joined.IsEmpty() ? wxGetTranslation(OriginToText(primary)) : joined;
}

/**
 * The way a source is being contacted, as a noun phrase for ContactRouteStatus()'s sentences.
 * Empty before the first contact attempt, and for a route this build does not know.
 */
wxString ContactRouteToText(unsigned int route)
{
	switch ((EContactRouteReport)route) {
	case EContactRouteReport::Dial:
		return wxTRANSLATE("a direct connection");
	case EContactRouteReport::DirectUdpCallback:
		return wxTRANSLATE("a direct callback");
	case EContactRouteReport::ServerCallback:
		return wxTRANSLATE("our server");
	case EContactRouteReport::KadBuddy:
		return wxTRANSLATE("the Kad buddy");
	case EContactRouteReport::ForeignNatCallback:
		return wxTRANSLATE("the source's own server");
	case EContactRouteReport::ServerHolePunch:
		return wxTRANSLATE("a server hole punch");
	case EContactRouteReport::LugdunumCallback:
		return wxTRANSLATE("a Lugdunum callback");
	case EContactRouteReport::None:
		break;
	}
	return wxEmptyString;
}

/**
 * What the source list says while a contact is in flight: the route, and whether it is one the
 * source kept for when the others failed, or one no hello has proved. A fallback is always
 * unproven as well, so the two never have to be said together.
 */
wxString ContactRouteStatus(unsigned int route, bool unproven, bool fromFallback)
{
	const wxString name = wxGetTranslation(ContactRouteToText(route));
	if (name.IsEmpty()) {
		return wxEmptyString;
	}
	if (fromFallback) {
		return CFormat(_("Connecting via %s, on a fallback route")) % name;
	}
	if (unproven) {
		return CFormat(_("Connecting via %s, on an unproven route")) % name;
	}
	return CFormat(_("Connecting via %s")) % name;
}

wxString GetConversionState(unsigned int state)
{
	switch (state) {
	case CONV_OK:
		return _("Completed");
	case CONV_INPROGRESS:
		return _("In progress");
	case CONV_OUTOFDISKSPACE:
		return _("ERROR: Out of disk space");
	case CONV_PARTMETNOTFOUND:
		return _("ERROR: Partmet not found");
	case CONV_IOERROR:
		return _("ERROR: IO error!");
	case CONV_FAILED:
		return _("ERROR: Failed!");
	case CONV_QUEUE:
		return _("Queued");
	case CONV_ALREADYEXISTS:
		return _("Already downloading");
	case CONV_BADFORMAT:
		return _("Unknown or bad tempfile format.");
	default:
		return "?";
	}
}
// File_checked_for_headers
