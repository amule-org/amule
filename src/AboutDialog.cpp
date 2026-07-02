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

#include "AboutDialog.h"

#include "config.h" // Needed for VERSION, GITDATE

#include <common/Format.h> // Needed for CFormat

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/hyperlink.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>

namespace
{
#ifdef ENABLE_VERSION_CHECK
const int ID_CHECK_UPDATES = wxID_HIGHEST + 1;
const wxString RELEASES_URL = wxT("https://github.com/amule-org/amule/releases/latest");
#endif // ENABLE_VERSION_CHECK

// A hyperlink whose colour is uniform (system link colour) in every state —
// dropping wxHyperlinkCtrl's red rollover / purple visited defaults so links
// look native and consistent on GTK / macOS / MSW.
wxHyperlinkCtrl *MakeLink(wxWindow *parent, const wxString &url)
{
	wxHyperlinkCtrl *link = new wxHyperlinkCtrl(parent, wxID_ANY, url, url);
	const wxColour linkColour = wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT);
	link->SetNormalColour(linkColour);
	link->SetHoverColour(linkColour);
	link->SetVisitedColour(linkColour);
	return link;
}
} // namespace

CAboutDlg::CAboutDlg(wxWindow *parent)
: wxDialog(parent, wxID_ANY, _("About aMule"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
#ifdef ENABLE_VERSION_CHECK
, m_status(NULL)
, m_downloadLink(NULL)
, m_checkButton(NULL)
#endif
{
	// Header (version + description) and the credits block reuse the exact
	// strings the old wxMessageBox About used, so their existing translations
	// carry over unchanged; trailing whitespace is trimmed for the layout.
	wxString head;
#ifdef CLIENT_GUI
	head << _("aMule remote control ") << VERSION;
#else
	head << wxT("aMule ") << VERSION;
#endif
#ifdef GITDATE
	head << wxT("\n") << _("Snapshot:") << wxT(" ") << GITDATE;
#endif
	head << wxT("\n\n") << _("'All-Platform' p2p client based on eMule \n\n");
	head.Trim();

	wxString credits;
	credits << _("Copyright (c) 2003-2026 aMule Team \n\n") << _("Part of aMule is based on \n")
		<< _("Kademlia: Peer-to-peer routing based on the XOR metric.\n")
		<< _(" Copyright (c) 2002-2011 Petar Maymounkov ( petar@maymounkov.org )\n");
	credits.Trim();

	// aMule logo on the left, matching the previous wxMessageBox About.
	const wxBitmap logoBmp = wxArtProvider::GetBitmap(wxT("amule:amule"), wxART_MESSAGE_BOX);

	// The project links, as native clickable hyperlinks.
	wxFlexGridSizer *linkGrid = new wxFlexGridSizer(2, wxSize(6, 2));
	const struct
	{
		wxString label;
		wxString url;
	} projectLinks[] = {
		{ _("Website:"), wxT("https://amule-org.github.io") },
		{ _("Forum:"), wxT("https://github.com/amule-org/amule/discussions") },
		{ _("Documentation:"), wxT("https://amule-org.github.io/docs") },
		{ _("Issues:"), wxT("https://github.com/amule-org/amule/issues") },
	};
	for (const auto &l : projectLinks) {
		linkGrid->Add(new wxStaticText(this, wxID_ANY, l.label), wxSizerFlags().CenterVertical());
		linkGrid->Add(MakeLink(this, l.url), wxSizerFlags().CenterVertical());
	}

#ifdef ENABLE_VERSION_CHECK
	// Update-check controls.
	m_status = new wxStaticText(this, wxID_ANY, _("Click to check for a newer version."));
	m_checkButton = new wxButton(this, ID_CHECK_UPDATES, _("Check for updates"));
	// Clickable download link, revealed only when a newer version is found.
	m_downloadLink = MakeLink(this, RELEASES_URL);
	m_downloadLink->Hide();

	wxBoxSizer *checkRow = new wxBoxSizer(wxHORIZONTAL);
	checkRow->Add(m_status, wxSizerFlags(1).CenterVertical().Border(wxRIGHT, 10));
	checkRow->Add(m_checkButton, wxSizerFlags().CenterVertical());
#endif // ENABLE_VERSION_CHECK

	wxBoxSizer *right = new wxBoxSizer(wxVERTICAL);
	right->Add(new wxStaticText(this, wxID_ANY, head));
	right->Add(linkGrid, wxSizerFlags().Border(wxTOP | wxBOTTOM, 8));
	right->Add(new wxStaticText(this, wxID_ANY, credits));
	right->Add(
		MakeLink(this, wxT("https://pdos.csail.mit.edu/~petar/papers/maymounkov-kademlia-lncs.pdf")),
		wxSizerFlags().Border(wxTOP, 2));
#ifdef ENABLE_VERSION_CHECK
	right->Add(new wxStaticLine(this, wxID_ANY), wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 10));
	right->Add(checkRow, wxSizerFlags().Expand());
	right->Add(m_downloadLink, wxSizerFlags().Border(wxTOP, 4));
#endif // ENABLE_VERSION_CHECK

	wxBoxSizer *topRow = new wxBoxSizer(wxHORIZONTAL);
	if (logoBmp.IsOk()) {
		topRow->Add(
			new wxStaticBitmap(this, wxID_ANY, logoBmp), wxSizerFlags().Top().Border(wxALL, 10));
	}
	topRow->Add(right, wxSizerFlags(1).Border(wxALL, 10));

	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	top->Add(topRow, wxSizerFlags(1).Expand());
	top->Add(CreateButtonSizer(wxOK), wxSizerFlags().Right().Border(wxALL, 10));

	SetSizerAndFit(top);
	Centre();

#ifdef ENABLE_VERSION_CHECK
	Bind(wxEVT_BUTTON, &CAboutDlg::OnCheckClicked, this, ID_CHECK_UPDATES);
	Bind(wxEVT_VERSION_CHECK_DONE, &CAboutDlg::OnCheckDone, this);
#endif // ENABLE_VERSION_CHECK
}

#ifdef ENABLE_VERSION_CHECK

void CAboutDlg::OnCheckClicked(wxCommandEvent &WXUNUSED(evt))
{
	m_checkButton->Enable(false);
	m_downloadLink->Hide();
	m_status->SetLabel(_("Checking for updates..."));
	Layout();
	m_check.Start(this, wxID_ANY);
}

void CAboutDlg::OnCheckDone(wxCommandEvent &evt)
{
	m_checkButton->Enable(true);
	switch (evt.GetInt()) {
	case CVersionCheck::UpToDate:
		m_status->SetLabel(_("You are running the latest version."));
		m_downloadLink->Hide();
		break;
	case CVersionCheck::Outdated:
		m_status->SetLabel(CFormat(_("A new version (%s) is available:")) % m_check.LatestVersion());
		m_downloadLink->Show();
		break;
	default:
		m_status->SetLabel(_("Could not check for updates. Please try again later."));
		m_downloadLink->Hide();
		break;
	}
	// The status/link can change height; relayout so the dialog resizes.
	GetSizer()->Layout();
	Fit();
}
#endif // ENABLE_VERSION_CHECK
