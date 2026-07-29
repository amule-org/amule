//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2004-2011 Angel Vidal (Kry) ( kry@amule.org / http://www.amule.org )
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

#include "KadDlg.h"
#include "muuli_wdr.h"
#include "OScopeCtrl.h"
#include "OtherFunctions.h"
#include "HTTPDownload.h"
#include "Logger.h"
#include "amule.h"
#include "Preferences.h"
#include "StatisticsDlg.h"
#include "ColorFrameCtrl.h"
#include "amuleDlg.h"
#include "MuleColour.h"
#include "Statistics.h"

#ifndef CLIENT_GUI
#include "kademlia/kademlia/Kademlia.h"
#endif

wxBEGIN_EVENT_TABLE(CKadDlg, wxPanel)
	EVT_TEXT(ID_NODE_IP, CKadDlg::OnFieldsChange)
	EVT_TEXT(ID_NODE_PORT, CKadDlg::OnFieldsChange)

	EVT_TEXT_ENTER(IDC_NODESLISTURL, CKadDlg::OnBnClickedUpdateNodeList)

	EVT_BUTTON(ID_NODECONNECT, CKadDlg::OnBnClickedBootstrapClient)
	EVT_BUTTON(ID_KADDISCONNECT, CKadDlg::OnBnClickedDisconnectKad)
	EVT_BUTTON(ID_UPDATEKADLIST, CKadDlg::OnBnClickedUpdateNodeList)
wxEND_EVENT_TABLE()

CKadDlg::CKadDlg(wxWindow *pParent)
: wxPanel(pParent, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL, "kadwnd")
{
	m_kad_scope = NULL;
}

void CKadDlg::Init()
{
	m_kad_scope = CastChild("kadScope", COScopeCtrl);
	m_kad_scope->SetRanges(0.0, thePrefs::GetStatsMax());
	m_kad_scope->SetYUnits("Nodes");

	SetUpdatePeriod(thePrefs::GetTrafficOMeterInterval());
	SetGraphColors();

	UpdateConnectButton();
}

void CKadDlg::UpdateConnectButton()
{
	wxButton *button = CastChild(ID_KADDISCONNECT, wxButton);
	wxCHECK_RET(button, "'ID_KADDISCONNECT' widget not found");

	EConnButtonState state;
	if (theApp->IsConnectedKad()) {
		state = ConnButtonConnected;
	} else if (theApp->IsKadRunning()) {
		state = ConnButtonConnecting;
	} else {
		state = ConnButtonOff;
	}

	// _("Kad") matches the translatable tab label (muuli_wdr.cpp's
	// NetDialog); see CServerWnd::UpdateED2KConnectButton's ED2K equivalent.
	SetConnectButtonState(button, state, thePrefs::GetNetworkKademlia(), _("Kad"));
}

void CKadDlg::SetUpdatePeriod(int step)
{
	// this gets called after the value in Preferences/Statistics/Update delay has been changed
	if (step == 0) {
		m_kad_scope->Stop();
	} else {
		m_kad_scope->Reset(step);
	}
}

void CKadDlg::SetGraphColors()
{
	static const char aTrend[] = { 2, 1, 0 };
	static const int aRes[] = { IDC_C0, IDC_C0_3, IDC_C0_2 };

	m_kad_scope->SetBackgroundColor(CStatisticsDlg::getColors(0));
	m_kad_scope->SetGridColor(CStatisticsDlg::getColors(1));

	for (size_t i = 0; i < 3; ++i) {
		m_kad_scope->SetPlotColor(CStatisticsDlg::getColors(12 + i), aTrend[i]);

		CColorFrameCtrl *ctrl = CastChild(aRes[i], CColorFrameCtrl);
		ctrl->SetBackgroundBrushColour(CMuleColour(CStatisticsDlg::getColors(12 + i)));
		ctrl->SetFrameBrushColour(*wxBLACK);
	}
}

void CKadDlg::UpdateGraph(const GraphUpdateInfo &update)
{
	std::vector<float *> v(3);
	v[0] = const_cast<float *>(&update.kadnodes[0]);
	v[1] = const_cast<float *>(&update.kadnodes[1]);
	v[2] = const_cast<float *>(&update.kadnodes[2]);
	const std::vector<float *> &apfKad(v);
	unsigned nodeCount = static_cast<unsigned>(update.kadnodes[2]);

	if (!IsShownOnScreen()) {
		m_kad_scope->DelayPoints();
	} else {
		// Check the current node-count to see if we should increase the graph height
		if (m_kad_scope->GetUpperLimit() < update.kadnodes[2]) {
			// Grow the limit by 50 sized increments. The integer ceiling-to-50 is
			// intentional; a whole number is what we want for the axis range.
			// NOLINTNEXTLINE(bugprone-integer-division)
			m_kad_scope->SetRanges(0.0, ((nodeCount + 49) / 50) * 50);
		}

		m_kad_scope->AppendPoints(update.timestamp, apfKad);
	}
}

void CKadDlg::UpdateNodeCount(unsigned nodeCount)
{
	wxStaticText *label = CastChild("nodesListLabel", wxStaticText);
	wxCHECK_RET(label, "Failed to find kad-nodes label");

	label->SetLabel(CFormat(_("Nodes (%u)")) % nodeCount);
	label->GetParent()->Layout();
}

// Enables or disables the node connect button depending on the contents of the text fields
void CKadDlg::OnFieldsChange(wxCommandEvent &WXUNUSED(evt))
{
	// These are the IDs of the search-fields
	int textfields[] = { ID_NODE_IP, ID_NODE_PORT };

	bool enable = true;
	for (int textfield : textfields) {
		enable &= !CastChild(textfield, wxTextCtrl)->GetValue().IsEmpty();
	}

	// Enable the node connect button if all fields contain text
	FindWindowById(ID_NODECONNECT)->Enable(enable);
}

void CKadDlg::OnBnClickedBootstrapClient(wxCommandEvent &WXUNUSED(evt))
{
	if (FindWindowById(ID_NODECONNECT)->IsEnabled()) {
		// Single "x.x.x.x" field (issue #402 review, matches the eD2k tab's
		// IDC_IPADDRESS). Trim first: the field's whole point is easier
		// copy-paste, and a paste commonly carries a leading/trailing space
		// that would otherwise fail as "Invalid ip to bootstrap". Octets are
		// reversed before StringIPtoUint32 because that function returns
		// anti-host order and Kad expects host order -- same trick the old
		// four-separate-fields version used, just built from one string
		// instead of four controls. wxSplit's third parameter (default '\\')
		// is an escape character, not meaningful for IPs; left at default.
		wxArrayString octets = wxSplit(dynamic_cast<wxTextCtrl *>(FindWindowById(ID_NODE_IP))
						       ->GetValue()
						       .Trim(true)
						       .Trim(false),
			'.');
		uint32 ip = 0;
		if (octets.GetCount() == 4) {
			ip = StringIPtoUint32(
				octets[3] + "." + octets[2] + "." + octets[1] + "." + octets[0]);
		}

		if (ip == 0) {
			wxMessageBox(
				_("Invalid ip to bootstrap"), _("WARNING"), wxOK | wxICON_EXCLAMATION, this);
		} else {
			unsigned long port;
			if (dynamic_cast<wxTextCtrl *>(FindWindowById(ID_NODE_PORT))
					->GetValue()
					.ToULong(&port)) {
				theApp->BootstrapKad(ip, port);
			} else {
				wxMessageBox(_("Invalid port to bootstrap"),
					_("WARNING"),
					wxOK | wxICON_EXCLAMATION,
					this);
			}
		}
	} else {
		wxMessageBox(
			_("Please fill all fields required"), _("Message"), wxOK | wxICON_INFORMATION, this);
	}
}

void CKadDlg::OnBnClickedDisconnectKad(wxCommandEvent &WXUNUSED(evt))
{
	// Doubles as Connect/Cancel/Disconnect depending on the button's current
	// state (see UpdateConnectButton()) -- StopKad() also covers the
	// "cancel while connecting" case, there's no separate abort path.
	if (theApp->IsConnectedKad() || theApp->IsKadRunning()) {
		theApp->StopKad();
	} else {
		theApp->StartKad();
	}
}

void CKadDlg::OnBnClickedUpdateNodeList(wxCommandEvent &WXUNUSED(evt))
{
	if (wxMessageBox(
		    wxString(_("Are you sure you want to download a new nodes.dat file?\n")) +
			    _("Doing so will remove your current nodes and restart Kademlia connection."),
		    _("Continue?"),
		    wxICON_EXCLAMATION | wxYES_NO | wxNO_DEFAULT,
		    this) == wxYES) {
		wxString strURL = dynamic_cast<wxTextCtrl *>(FindWindowById(IDC_NODESLISTURL))->GetValue();

		thePrefs::SetKadNodesUrl(strURL);
		theApp->UpdateNotesDat(strURL);
	}
}
// File_checked_for_headers
