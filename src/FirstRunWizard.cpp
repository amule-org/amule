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

#include "FirstRunWizard.h"

#ifndef AMULE_DAEMON

#include "config.h" // Needed for ENABLE_UPNP

#include <wx/wizard.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>
#include <wx/button.h>
#include <wx/dirdlg.h>
#include <wx/valtext.h>

#include "amule.h"       // Needed for theApp / glob_prefs
#include "Preferences.h" // Needed for thePrefs
#include <common/Format.h>
#include <common/Path.h> // Needed for CPath

namespace
{
// A predefined connection profile. Each profile carries two distinct
// pairs of kByte/s values:
//   * the suggested rate *limits* (uploadKBs / downloadKBs, 0 ==
//     unlimited), which fill the two spin controls and become
//     MaxUpload / MaxDownload; and
//   * the raw line *capacity* (uploadCapKBs / downloadCapKBs), which
//     becomes MaxGraphUploadRate / MaxGraphDownloadRate -- the value
//     that scales the statistics graphs and feeds the dynamic-upload
//     logic. Capacity is never 0, so a fast (unlimited-limit) line
//     still gets sensibly scaled graphs instead of the old default.
// The upload limit is ~80% of the raw upstream capacity (leaving ACK /
// protocol headroom), while the download limit is left unlimited (0)
// and only the download capacity carries the line's raw downstream rate.
struct ConnectionProfile
{
	const char *label;
	int uploadKBs;	    // upload limit (0 == unlimited)
	int downloadKBs;    // download limit (0 == unlimited)
	int uploadCapKBs;   // raw upstream line capacity
	int downloadCapKBs; // raw downstream line capacity
};

// Labels are wxTRANSLATE-marked (so xgettext extracts them) and
// translated at display time with wxGetTranslation below.
const ConnectionProfile s_profiles[] = {
	{wxTRANSLATE("Mobile / 4G–5G (20 / 5 Mbit)"), 500, 0, 625, 2500},
	{wxTRANSLATE("ADSL (24 / 1 Mbit)"), 100, 0, 125, 3000},
	{wxTRANSLATE("VDSL (50 / 10 Mbit)"), 1000, 0, 1250, 6250},
	{wxTRANSLATE("Cable (200 / 20 Mbit)"), 2000, 0, 2500, 25000},
	{wxTRANSLATE("Fibre 300 (300 / 100 Mbit)"), 10000, 0, 12500, 37500},
	{wxTRANSLATE("Fibre Gigabit (1000 / 1000 Mbit)"), 0, 0, 125000, 125000},
};

const size_t s_profileCount = sizeof(s_profiles) / sizeof(s_profiles[0]);

// Derived "max sources / max connections" recommendation tiers, keyed
// on the upload limit (kByte/s). eMule derives these the same way: a
// slow uplink can only usefully feed a handful of peers, so capping
// sources and connections keeps a small line from being swamped by
// half-open connection overhead, while a fat uplink is allowed many
// more. An unlimited (0) upload limit is treated as a fast line.
struct DerivedLimits
{
	int maxConnections;
	int maxConnectionsPer5Sec;
	int maxSourcesPerFile;
};

DerivedLimits DeriveLimits(int uploadKBs)
{
	const int up = (uploadKBs <= 0) ? 1000 : uploadKBs;
	if (up < 4) {
		return {80, 7, 120};
	} else if (up < 7) {
		return {120, 10, 200};
	} else if (up < 13) {
		return {200, 15, 300};
	} else if (up < 25) {
		return {300, 20, 400};
	} else if (up < 50) {
		return {400, 30, 500};
	}
	return {500, 50, 1000};
}

enum
{
	ID_FRW_SPEED = wxID_HIGHEST + 1,
	ID_FRW_UPLOAD,
	ID_FRW_BROWSE_INCOMING,
	ID_FRW_BROWSE_TEMP
};

class CFirstRunWizard : public wxWizard
{
public:
	CFirstRunWizard(wxWindow *parent, bool needServerMet, bool needNodesDat);

	// Runs the wizard modally. Returns true if the user pressed Finish.
	bool RunIt() { return RunWizard(m_firstPage); }

	// Pushes the collected choices into the live preferences, saves
	// them, and reports which bootstrap downloads were requested.
	void Apply(FirstRunWizard::Result &res);

private:
	wxWizardPageSimple *BuildNickPage();
	wxWizardPageSimple *BuildConnectionPage();
	wxWizardPageSimple *BuildNetworkPage();
	wxWizardPageSimple *BuildBootstrapPage();
	wxWizardPageSimple *BuildFoldersPage();

	void UpdateDerivedLabel();

	void OnSpeedChoice(wxCommandEvent &evt);
	void OnUploadChanged(wxSpinEvent &evt);
	void OnBrowseIncoming(wxCommandEvent &evt);
	void OnBrowseTemp(wxCommandEvent &evt);

	bool m_needServerMet;
	bool m_needNodesDat;

	wxWizardPageSimple *m_firstPage = NULL;

	wxTextCtrl *m_nickCtrl = NULL;
	wxChoice *m_speedCtrl = NULL;
	wxSpinCtrl *m_uploadCtrl = NULL;
	wxSpinCtrl *m_downloadCtrl = NULL;
	wxStaticText *m_derivedLabel = NULL;
	wxCheckBox *m_ed2kCtrl = NULL;
	wxCheckBox *m_kadCtrl = NULL;
	wxSpinCtrl *m_tcpPortCtrl = NULL;
	wxSpinCtrl *m_udpPortCtrl = NULL;
#ifdef ENABLE_UPNP
	wxCheckBox *m_upnpCtrl = NULL;
#endif
	wxCheckBox *m_serverMetCtrl = NULL;
	wxCheckBox *m_nodesDatCtrl = NULL;
	wxTextCtrl *m_incomingCtrl = NULL;
	wxTextCtrl *m_tempCtrl = NULL;
};

CFirstRunWizard::CFirstRunWizard(wxWindow *parent, bool needServerMet, bool needNodesDat)
	: wxWizard(parent, wxID_ANY, _("aMule first-run setup")),
	  m_needServerMet(needServerMet),
	  m_needNodesDat(needNodesDat)
{
	wxWizardPageSimple *nick = BuildNickPage();
	wxWizardPageSimple *conn = BuildConnectionPage();
	wxWizardPageSimple *net = BuildNetworkPage();
	wxWizardPageSimple *boot = BuildBootstrapPage();
	wxWizardPageSimple *folders = BuildFoldersPage();

	wxWizardPageSimple::Chain(nick, conn);
	wxWizardPageSimple::Chain(conn, net);
	wxWizardPageSimple::Chain(net, boot);
	wxWizardPageSimple::Chain(boot, folders);

	m_firstPage = nick;

	GetPageAreaSizer()->Add(nick);
	UpdateDerivedLabel();
}

wxWizardPageSimple *CFirstRunWizard::BuildNickPage()
{
	wxWizardPageSimple *page = new wxWizardPageSimple(this);
	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

	sizer->Add(new wxStaticText(page, wxID_ANY, _("Welcome to aMule!")), 0, wxBOTTOM, 8);
	sizer->Add(new wxStaticText(page,
			   wxID_ANY,
			   _("This short setup will get you connected. You can change\n"
			     "any of these settings later in Preferences.")),
		0,
		wxBOTTOM,
		12);

	sizer->Add(new wxStaticText(page, wxID_ANY, _("Choose a nickname other peers will see:")),
		0,
		wxBOTTOM,
		4);
	m_nickCtrl = new wxTextCtrl(page, wxID_ANY, thePrefs::GetUserNick());
	sizer->Add(m_nickCtrl, 0, wxEXPAND);

	page->SetSizer(sizer);
	return page;
}

wxWizardPageSimple *CFirstRunWizard::BuildConnectionPage()
{
	wxWizardPageSimple *page = new wxWizardPageSimple(this);
	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

	sizer->Add(new wxStaticText(page, wxID_ANY, _("Connection & bandwidth")), 0, wxBOTTOM, 8);
	sizer->Add(new wxStaticText(page,
			   wxID_ANY,
			   _("Pick the option that best matches your Internet connection,\n"
			     "or set the upload and download limits directly below.")),
		0,
		wxBOTTOM,
		12);

	sizer->Add(new wxStaticText(page, wxID_ANY, _("Connection speed:")), 0, wxBOTTOM, 4);
	m_speedCtrl = new wxChoice(page, ID_FRW_SPEED);
	for (size_t i = 0; i < s_profileCount; ++i) {
		m_speedCtrl->Append(wxGetTranslation(s_profiles[i].label));
	}
	m_speedCtrl->Append(_("Other / set manually"));
	m_speedCtrl->SetSelection((int)s_profileCount); // "Other" until a profile is chosen
	sizer->Add(m_speedCtrl, 0, wxEXPAND | wxBOTTOM, 12);

	wxFlexGridSizer *grid = new wxFlexGridSizer(2, 2, 6, 8);
	grid->AddGrowableCol(1);

	grid->Add(new wxStaticText(page, wxID_ANY, _("Upload limit (kB/s, 0 = unlimited):")),
		0,
		wxALIGN_CENTER_VERTICAL);
	m_uploadCtrl = new wxSpinCtrl(page, ID_FRW_UPLOAD);
	m_uploadCtrl->SetRange(0, 100000);
	m_uploadCtrl->SetValue(thePrefs::GetMaxUpload());
	grid->Add(m_uploadCtrl, 0, wxEXPAND);

	grid->Add(new wxStaticText(page, wxID_ANY, _("Download limit (kB/s, 0 = unlimited):")),
		0,
		wxALIGN_CENTER_VERTICAL);
	m_downloadCtrl = new wxSpinCtrl(page, wxID_ANY);
	m_downloadCtrl->SetRange(0, 100000);
	m_downloadCtrl->SetValue(thePrefs::GetMaxDownload());
	grid->Add(m_downloadCtrl, 0, wxEXPAND);

	sizer->Add(grid, 0, wxEXPAND | wxBOTTOM, 12);

	m_derivedLabel = new wxStaticText(page, wxID_ANY, wxEmptyString);
	sizer->Add(m_derivedLabel, 0, wxEXPAND);

	page->SetSizer(sizer);

	m_speedCtrl->Bind(wxEVT_CHOICE, &CFirstRunWizard::OnSpeedChoice, this);
	m_uploadCtrl->Bind(wxEVT_SPINCTRL, &CFirstRunWizard::OnUploadChanged, this);

	return page;
}

wxWizardPageSimple *CFirstRunWizard::BuildNetworkPage()
{
	wxWizardPageSimple *page = new wxWizardPageSimple(this);
	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

	sizer->Add(new wxStaticText(page, wxID_ANY, _("Networks & ports")), 0, wxBOTTOM, 8);
	sizer->Add(new wxStaticText(page,
			   wxID_ANY,
			   _("aMule can use two networks. We recommend keeping both enabled.")),
		0,
		wxBOTTOM,
		8);

	// Labels reuse the existing Networks-panel strings (PreferencesConnectionTab)
	// so translators don't get wizard-only duplicates; the guidance sentence
	// above carries the "server-based / serverless" explanation.
	m_ed2kCtrl = new wxCheckBox(page, wxID_ANY, _("ED2K"));
	m_ed2kCtrl->SetValue(thePrefs::GetNetworkED2K());
	sizer->Add(m_ed2kCtrl, 0, wxBOTTOM, 4);

	m_kadCtrl = new wxCheckBox(page, wxID_ANY, _("Kademlia"));
	m_kadCtrl->SetValue(thePrefs::GetNetworkKademlia());
	sizer->Add(m_kadCtrl, 0, wxBOTTOM, 12);

	wxFlexGridSizer *grid = new wxFlexGridSizer(2, 2, 6, 8);
	grid->AddGrowableCol(1);

	// Port labels reuse the existing Ports-panel strings rather than coining
	// new wizard-only wording for the same controls.
	grid->Add(new wxStaticText(page, wxID_ANY, _("Standard TCP Port ")),
		0,
		wxALIGN_CENTER_VERTICAL);
	m_tcpPortCtrl = new wxSpinCtrl(page, wxID_ANY);
	m_tcpPortCtrl->SetRange(1, 65535);
	m_tcpPortCtrl->SetValue(thePrefs::GetPort());
	grid->Add(m_tcpPortCtrl, 0, wxEXPAND);

	grid->Add(new wxStaticText(page, wxID_ANY, _("Extended UDP port (Kad / global search) ")),
		0,
		wxALIGN_CENTER_VERTICAL);
	m_udpPortCtrl = new wxSpinCtrl(page, wxID_ANY);
	m_udpPortCtrl->SetRange(1, 65535);
	m_udpPortCtrl->SetValue(thePrefs::GetUDPPort());
	grid->Add(m_udpPortCtrl, 0, wxEXPAND);

	sizer->Add(grid, 0, wxEXPAND | wxBOTTOM, 12);

#ifdef ENABLE_UPNP
	sizer->Add(new wxStaticLine(page), 0, wxEXPAND | wxBOTTOM, 8);
	m_upnpCtrl = new wxCheckBox(page, wxID_ANY, _("Enable UPnP for router port forwarding"));
	m_upnpCtrl->SetValue(true);
	sizer->Add(m_upnpCtrl, 0, wxBOTTOM, 4);
	sizer->Add(new wxStaticText(page,
			   wxID_ANY,
			   _("Lets aMule ask your router to forward the ports above so other\n"
			     "peers can reach you (a \"HighID\"). Turn off if your router\n"
			     "doesn't support UPnP or you forward ports manually.")),
		0,
		wxLEFT,
		20);
#endif

	page->SetSizer(sizer);
	return page;
}

wxWizardPageSimple *CFirstRunWizard::BuildBootstrapPage()
{
	wxWizardPageSimple *page = new wxWizardPageSimple(this);
	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

	sizer->Add(new wxStaticText(page, wxID_ANY, _("Bootstrap files")), 0, wxBOTTOM, 8);

	if (m_needServerMet || m_needNodesDat) {
		sizer->Add(new wxStaticText(page,
				   wxID_ANY,
				   _("To get started, aMule needs to know about some peers.\n"
				     "Select which lists to download now:")),
			0,
			wxBOTTOM,
			8);

		if (m_needServerMet) {
			m_serverMetCtrl =
				new wxCheckBox(page, wxID_ANY, _("eD2k server list (server.met)"));
			m_serverMetCtrl->SetValue(true);
			sizer->Add(m_serverMetCtrl, 0, wxBOTTOM, 4);
		}
		if (m_needNodesDat) {
			m_nodesDatCtrl =
				new wxCheckBox(page, wxID_ANY, _("Kad bootstrap nodes (nodes.dat)"));
			m_nodesDatCtrl->SetValue(true);
			sizer->Add(m_nodesDatCtrl, 0, wxBOTTOM, 4);
		}
	} else {
		sizer->Add(new wxStaticText(page,
				   wxID_ANY,
				   _("Your peer lists are already in place — nothing to download.")),
			0);
	}

	page->SetSizer(sizer);
	return page;
}

wxWizardPageSimple *CFirstRunWizard::BuildFoldersPage()
{
	wxWizardPageSimple *page = new wxWizardPageSimple(this);
	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

	sizer->Add(new wxStaticText(page, wxID_ANY, _("Folders (optional)")), 0, wxBOTTOM, 8);
	sizer->Add(new wxStaticText(page,
			   wxID_ANY,
			   _("The defaults are fine for most people. Change them if you\n"
			     "want downloads on a different (e.g. larger) disk.")),
		0,
		wxBOTTOM,
		12);

	// Folder labels and Browse buttons reuse the existing Directories-panel
	// strings (PreferencesDirectoriesTab) instead of new wizard-only wording.
	sizer->Add(new wxStaticText(page, wxID_ANY, _("Destination folder for downloads")),
		0,
		wxBOTTOM,
		4);
	wxBoxSizer *incRow = new wxBoxSizer(wxHORIZONTAL);
	m_incomingCtrl = new wxTextCtrl(page, wxID_ANY, thePrefs::GetIncomingDir().GetRaw());
	incRow->Add(m_incomingCtrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	incRow->Add(new wxButton(page, ID_FRW_BROWSE_INCOMING, _("Browse")), 0);
	sizer->Add(incRow, 0, wxEXPAND | wxBOTTOM, 4);

	// Warn that everything dropped in the Incoming folder is shared, reusing
	// the same hint shown on the Directories preferences panel.
	sizer->Add(new wxStaticText(page, wxID_ANY, _("(All files in this folder are shared with other peers)")),
		0,
		wxBOTTOM,
		12);

	sizer->Add(new wxStaticText(page, wxID_ANY, _("Folder for temporary download files")),
		0,
		wxBOTTOM,
		4);
	wxBoxSizer *tmpRow = new wxBoxSizer(wxHORIZONTAL);
	m_tempCtrl = new wxTextCtrl(page, wxID_ANY, thePrefs::GetTempDir().GetRaw());
	tmpRow->Add(m_tempCtrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	tmpRow->Add(new wxButton(page, ID_FRW_BROWSE_TEMP, _("Browse")), 0);
	sizer->Add(tmpRow, 0, wxEXPAND);

	page->SetSizer(sizer);

	page->Bind(wxEVT_BUTTON, &CFirstRunWizard::OnBrowseIncoming, this, ID_FRW_BROWSE_INCOMING);
	page->Bind(wxEVT_BUTTON, &CFirstRunWizard::OnBrowseTemp, this, ID_FRW_BROWSE_TEMP);

	return page;
}

void CFirstRunWizard::UpdateDerivedLabel()
{
	if (!m_derivedLabel || !m_uploadCtrl) {
		return;
	}
	DerivedLimits d = DeriveLimits(m_uploadCtrl->GetValue());
	m_derivedLabel->SetLabel(CFormat(_("Based on this, aMule will allow up to %i sources per file\n"
					   "and %i client connections."))
		% d.maxSourcesPerFile % d.maxConnections);
}

void CFirstRunWizard::OnSpeedChoice(wxCommandEvent &WXUNUSED(evt))
{
	int sel = m_speedCtrl->GetSelection();
	if (sel >= 0 && sel < (int)s_profileCount) {
		m_uploadCtrl->SetValue(s_profiles[sel].uploadKBs);
		m_downloadCtrl->SetValue(s_profiles[sel].downloadKBs);
		UpdateDerivedLabel();
	}
}

void CFirstRunWizard::OnUploadChanged(wxSpinEvent &WXUNUSED(evt))
{
	// The user typed an upload value by hand — detach the profile
	// choice so it doesn't claim a preset that no longer matches.
	m_speedCtrl->SetSelection((int)s_profileCount);
	UpdateDerivedLabel();
}

void CFirstRunWizard::OnBrowseIncoming(wxCommandEvent &WXUNUSED(evt))
{
	wxString dir = ::wxDirSelector(
		_("Destination folder for downloads"), m_incomingCtrl->GetValue(), 0, wxDefaultPosition, this);
	if (!dir.IsEmpty()) {
		m_incomingCtrl->SetValue(dir);
	}
}

void CFirstRunWizard::OnBrowseTemp(wxCommandEvent &WXUNUSED(evt))
{
	wxString dir = ::wxDirSelector(
		_("Folder for temporary download files"), m_tempCtrl->GetValue(), 0, wxDefaultPosition, this);
	if (!dir.IsEmpty()) {
		m_tempCtrl->SetValue(dir);
	}
}

void CFirstRunWizard::Apply(FirstRunWizard::Result &res)
{
	// Nickname — keep the existing default rather than allowing an
	// empty nick.
	wxString nick = m_nickCtrl->GetValue().Trim().Trim(false);
	if (!nick.IsEmpty()) {
		thePrefs::SetUserNick(nick);
	}

	// Bandwidth + the derived peer limits.
	const int up = m_uploadCtrl->GetValue();
	const int down = m_downloadCtrl->GetValue();
	thePrefs::SetMaxUpload(up);
	thePrefs::SetMaxDownload(down);

	// Line capacity (MaxGraphUploadRate / MaxGraphDownloadRate) is a
	// separate value from the limits above: it scales the statistics
	// graphs and feeds the dynamic-upload logic. If a preset is still
	// selected, take its raw line capacity; otherwise the user typed
	// values by hand, so estimate from the limits (upload limits sit
	// ~80% below the raw rate, i.e. cap = limit / 0.8 = limit * 5 / 4).
	// A 0 (unlimited) manual limit tells us nothing, so the existing
	// capacity is kept rather than zeroing it.
	int upCap = 0;
	int downCap = 0;
	const int sel = m_speedCtrl->GetSelection();
	if (sel >= 0 && sel < (int)s_profileCount) {
		upCap = s_profiles[sel].uploadCapKBs;
		downCap = s_profiles[sel].downloadCapKBs;
	} else {
		upCap = (up > 0) ? (up * 5 + 3) / 4 : (int)thePrefs::GetMaxGraphUploadRate();
		downCap = (down > 0) ? down : (int)thePrefs::GetMaxGraphDownloadRate();
	}
	if (upCap > 0) {
		thePrefs::SetMaxGraphUploadRate(upCap);
	}
	if (downCap > 0) {
		thePrefs::SetMaxGraphDownloadRate(downCap);
	}

	DerivedLimits d = DeriveLimits(up);
	thePrefs::SetMaxConnections(d.maxConnections);
	thePrefs::SetMaxConsPerFive(d.maxConnectionsPer5Sec);
	thePrefs::SetMaxSourcesPerFile(d.maxSourcesPerFile);

	// Networks + ports.
	thePrefs::SetNetworkED2K(m_ed2kCtrl->GetValue());
	thePrefs::SetNetworkKademlia(m_kadCtrl->GetValue());
	thePrefs::SetPort((uint16)m_tcpPortCtrl->GetValue());
	thePrefs::SetUDPPort((uint16)m_udpPortCtrl->GetValue());
#ifdef ENABLE_UPNP
	thePrefs::SetUPnPEnabled(m_upnpCtrl->GetValue());
#endif

	// Folders — only override the defaults if the user typed something.
	wxString inc = m_incomingCtrl->GetValue().Trim().Trim(false);
	if (!inc.IsEmpty()) {
		thePrefs::SetIncomingDir(CPath(inc));
	}
	wxString tmp = m_tempCtrl->GetValue().Trim().Trim(false);
	if (!tmp.IsEmpty()) {
		thePrefs::SetTempDir(CPath(tmp));
	}

	// Record that the wizard ran to completion, so it is not shown again
	// on the next launch. This is only reached when the user pressed
	// Finish (Run() calls Apply() only then), so a cancelled run leaves
	// the flag false and the wizard reappears next time.
	thePrefs::SetFirstRunWizardDone(true);

	if (theApp->glob_prefs) {
		theApp->glob_prefs->Save();
	}

	// Bootstrap downloads (only offered for files that are missing).
	res.downloadServerMet = m_serverMetCtrl && m_serverMetCtrl->GetValue();
	res.downloadNodesDat = m_nodesDatCtrl && m_nodesDatCtrl->GetValue();
}

} // anonymous namespace

namespace FirstRunWizard
{
Result Run(wxWindow *parent, bool needServerMet, bool needNodesDat)
{
	Result res;

	CFirstRunWizard wizard(parent, needServerMet, needNodesDat);
	res.finished = wizard.RunIt();
	if (res.finished) {
		wizard.Apply(res);
	}
	return res;
}
} // namespace FirstRunWizard

#endif // !AMULE_DAEMON
