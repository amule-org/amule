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

#include "ServerListModel.h"

#include <algorithm> // Needed for std::find

#ifdef GEOIP_GUI
#include "CountryDisplay.h" // Needed for GetDisplayCountryCode
#endif
#include <common/Format.h> // Needed for CFormat

#include "OtherFunctions.h" // Needed for CastSecondsToHM
#include "Server.h"         // Needed for CServer and SRV_PR_*
#include "ServerListCtrl.h" // Needed for CServerListCtrl

CServerListModel::CServerListModel(CServerListCtrl *owner)
: m_owner(owner)
{
}

void CServerListModel::AddServer(CServer *server)
{
	if (!server || m_index.count(server)) {
		return;
	}
	m_servers.push_back(server);
	m_index.insert(server);
	MarkDirty();
}

void CServerListModel::RemoveServer(CServer *server)
{
	if (!server || !m_index.erase(server)) {
		return;
	}
	m_servers.erase(std::find(m_servers.begin(), m_servers.end(), server));
	MarkDirty();
}

void CServerListModel::RefreshServer(CServer *server)
{
	if (!server) {
		return;
	}
	if (!m_index.count(server)) {
		AddServer(server);
		return;
	}
	MarkDirty();
}

void CServerListModel::ClearAll()
{
	if (m_servers.empty()) {
		return;
	}
	m_servers.clear();
	m_index.clear();
	MarkDirty();
}

bool CServerListModel::HasServer(CServer *server) const
{
	return server && m_index.count(server) != 0;
}

bool CServerListModel::FlushPending()
{
	if (!m_pendingReset) {
		return false;
	}
	m_pendingReset = false;
	Cleared();
	return true;
}

unsigned int CServerListModel::GetColumnCount() const
{
	return COL_COUNT;
}

wxString CServerListModel::GetColumnType(unsigned int col) const
{
	return (col == COL_NAME) ? wxString("wxDataViewIconText") : wxString("string");
}

void CServerListModel::GetValue(wxVariant &variant, const wxDataViewItem &item, unsigned int col) const
{
	const CServer *server = ToServer(item);

	switch (col) {
	case COL_NAME: {
		wxIcon icon;
#ifdef GEOIP_GUI
		// Host country as a flag icon: no icon at all for an unresolved
		// server, rather than the "? - " prefix this used to show.
		wxString code;
		if (GetDisplayCountryCode(
			    server->IsCountryFromCore(), server->GetCountryCode(), server->GetIP(), code) &&
			!code.IsEmpty()) {
			icon = m_owner->FlagIcon(code);
		}
#endif // GEOIP_GUI
		variant << wxDataViewIconText(server->GetListName(), icon);
		break;
	}

	case COL_ADDR:
		variant = server->GetAddress();
		break;

	case COL_PORT:
		if (server->GetAuxPortsList().IsEmpty()) {
			variant = wxString(CFormat("%u") % server->GetPort());
		} else {
			variant =
				wxString(CFormat("%u (%s)") % server->GetPort() % server->GetAuxPortsList());
		}
		break;

	case COL_DESC:
		variant = server->GetDescription();
		break;

	case COL_PING:
		variant = server->GetPing()
				  ? CastSecondsToHM(server->GetPing() / 1000, server->GetPing() % 1000)
				  : wxString();
		break;

	case COL_USERS:
		variant = server->GetUsers() ? wxString(CFormat("%u") % server->GetUsers()) : wxString();
		break;

	case COL_FILES:
		variant = server->GetFiles() ? wxString(CFormat("%u") % server->GetFiles()) : wxString();
		break;

	case COL_PRIO:
		switch (server->GetPreferences()) {
		case SRV_PR_LOW:
			variant = wxString(_("Low"));
			break;
		case SRV_PR_NORMAL:
			variant = wxString(_("Normal"));
			break;
		case SRV_PR_HIGH:
			variant = wxString(_("High"));
			break;
		default:
			variant = wxString("---"); // this should never happen
			break;
		}
		break;

	case COL_FAILS:
		variant = wxString(CFormat("%u") % server->GetFailedCount());
		break;

	case COL_STATIC:
		variant = server->IsStaticMember() ? wxString(_("Yes")) : wxString(_("No"));
		break;

	case COL_VERSION:
		variant = server->GetVersion();
		break;

#if !defined(CLIENT_GUI)
	case COL_TCPFLAGS: {
		wxString flags;
		if (server->GetTCPFlags() & SRV_TCPFLG_COMPRESSION) {
			flags += "c";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_NEWTAGS) {
			flags += "n";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_UNICODE) {
			flags += "u";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_RELATEDSEARCH) {
			flags += "r";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_TYPETAGINTEGER) {
			flags += "t";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_LARGEFILES) {
			flags += "l";
		}
		if (server->GetTCPFlags() & SRV_TCPFLG_TCPOBFUSCATION) {
			flags += "o";
		}
		variant = flags;
		break;
	}

	case COL_UDPFLAGS: {
		wxString flags;
		if (server->GetUDPFlags() & SRV_UDPFLG_EXT_GETSOURCES) {
			flags += "g";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_EXT_GETFILES) {
			flags += "f";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_NEWTAGS) {
			flags += "n";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_UNICODE) {
			flags += "u";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_EXT_GETSOURCES2) {
			flags += "G";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_LARGEFILES) {
			flags += "l";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_UDPOBFUSCATION) {
			flags += "o";
		}
		if (server->GetUDPFlags() & SRV_UDPFLG_TCPOBFUSCATION) {
			flags += "O";
		}
		variant = flags;
		break;
	}
#endif

	default:
		break;
	}
}

bool CServerListModel::SetValue(
	const wxVariant &WXUNUSED(variant), const wxDataViewItem &WXUNUSED(item), unsigned int WXUNUSED(col))
{
	// Every column is read-only; values only ever change from the core
	// side, via RefreshServer().
	return false;
}

bool CServerListModel::GetAttr(
	const wxDataViewItem &item, unsigned int WXUNUSED(col), wxDataViewItemAttr &attr) const
{
	if (m_owner->IsConnected(ToServer(item))) {
		attr.SetBold(true);
		return true;
	}
	return false;
}

wxDataViewItem CServerListModel::GetParent(const wxDataViewItem &WXUNUSED(item)) const
{
	// Flat list: every server is a top-level row.
	return wxDataViewItem();
}

bool CServerListModel::IsContainer(const wxDataViewItem &item) const
{
	return !item.IsOk(); // invisible root only -- no row ever has children
}

unsigned int CServerListModel::GetChildren(const wxDataViewItem &item, wxDataViewItemArray &children) const
{
	if (item.IsOk()) {
		return 0;
	}
	for (CServer *server : m_servers) {
		children.Add(ToItem(server));
	}
	return static_cast<unsigned int>(m_servers.size());
}

int CServerListModel::Compare(const wxDataViewItem &item1,
	const wxDataViewItem &item2,
	unsigned int WXUNUSED(column),
	bool WXUNUSED(ascending)) const
{
	// The requested (column, ascending) pair reflects only the native
	// header click; the full sort state (including any tie-break chain)
	// is tracked on the control and applied here regardless -- see
	// CServerListCtrl::CompareServers.
	return m_owner->CompareServers(ToServer(item1), ToServer(item2));
}
