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
#include <wx/textctrl.h>
#include <wx/utils.h> // Needed for wxLaunchDefaultBrowser

namespace
{
const int ID_CHECK_UPDATES = wxID_HIGHEST + 1;
const wxString RELEASES_URL = wxT("https://github.com/amule-org/amule/releases/latest");
} // namespace

CAboutDlg::CAboutDlg(wxWindow *parent)
: wxDialog(parent, wxID_ANY, _("About aMule"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
, m_aboutText(NULL)
, m_status(NULL)
, m_downloadLink(NULL)
, m_checkButton(NULL)
{
	// Reuse the exact strings the old wxMessageBox About used so their
	// existing translations carry over unchanged.
	wxString about;
#ifdef CLIENT_GUI
	about << _("aMule remote control ") << VERSION;
#else
	about << wxT("aMule ") << VERSION;
#endif
#ifdef GITDATE
	about << wxT("\n") << _("Snapshot:") << wxT(" ") << GITDATE;
#endif
	about << wxT("\n\n") << _("'All-Platform' p2p client based on eMule \n\n")
	      << _("Website: https://amule-org.github.io \n")
	      << _("Forum: https://github.com/amule-org/amule/discussions \n")
	      << _("Documentation: https://amule-org.github.io/docs \n")
	      << _("Issues: https://github.com/amule-org/amule/issues \n\n")
	      << _("Copyright (c) 2003-2026 aMule Team \n\n") << _("Part of aMule is based on \n")
	      << _("Kademlia: Peer-to-peer routing based on the XOR metric.\n")
	      << _(" Copyright (c) 2002-2011 Petar Maymounkov ( petar@maymounkov.org )\n")
	      << _("https://pdos.csail.mit.edu/~petar/papers/maymounkov-kademlia-lncs.pdf\n");

	// aMule logo on the left, matching the previous wxMessageBox About.
	const wxBitmap logoBmp = wxArtProvider::GetBitmap(wxT("amule:amule"), wxART_MESSAGE_BOX);

	// Selectable, URL-aware text area (the old static text/message box could
	// be neither selected nor clicked). Its background is blended into the
	// dialog so it still reads like a label, not an input field.
	m_aboutText = new wxTextCtrl(this,
		wxID_ANY,
		about,
		wxDefaultPosition,
		wxDefaultSize,
		wxTE_MULTILINE | wxTE_READONLY | wxTE_NO_VSCROLL | wxTE_AUTO_URL | wxBORDER_NONE);
	m_aboutText->SetBackgroundColour(GetBackgroundColour());
	// Size the control to its content so no scrollbar is needed.
	const int lines = static_cast<int>(about.Freq(wxT('\n'))) + 1;
	m_aboutText->SetMinSize(wxSize(560, (lines + 1) * m_aboutText->GetCharHeight()));

	// Update-check controls.
	m_status = new wxStaticText(this, wxID_ANY, _("Click to check for a newer version."));
	m_checkButton = new wxButton(this, ID_CHECK_UPDATES, _("Check for updates"));
	// Clickable download link, revealed only when a newer version is found.
	m_downloadLink = new wxHyperlinkCtrl(this, wxID_ANY, RELEASES_URL, RELEASES_URL);
	// Use the system hyperlink colour (what the auto-URLs in the text area
	// above use) for every state, dropping wxHyperlinkCtrl's hardcoded blue
	// plus its red rollover / purple visited defaults so the link is uniform.
	const wxColour linkColour = wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT);
	m_downloadLink->SetNormalColour(linkColour);
	m_downloadLink->SetHoverColour(linkColour);
	m_downloadLink->SetVisitedColour(linkColour);
	m_downloadLink->Hide();

	wxBoxSizer *checkRow = new wxBoxSizer(wxHORIZONTAL);
	checkRow->Add(m_status, wxSizerFlags(1).CenterVertical().Border(wxRIGHT, 10));
	checkRow->Add(m_checkButton, wxSizerFlags().CenterVertical());

	wxBoxSizer *right = new wxBoxSizer(wxVERTICAL);
	right->Add(m_aboutText, wxSizerFlags().Expand());
	right->Add(new wxStaticLine(this, wxID_ANY), wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 8));
	right->Add(checkRow, wxSizerFlags().Expand());
	right->Add(m_downloadLink, wxSizerFlags().Border(wxTOP, 4));

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

	Bind(wxEVT_BUTTON, &CAboutDlg::OnCheckClicked, this, ID_CHECK_UPDATES);
	Bind(wxEVT_VERSION_CHECK_DONE, &CAboutDlg::OnCheckDone, this);
	m_aboutText->Bind(wxEVT_TEXT_URL, &CAboutDlg::OnTextUrl, this);
}

void CAboutDlg::OnTextUrl(wxTextUrlEvent &evt)
{
	// Open the clicked URL in the default browser (on mouse-up, so a
	// selection drag doesn't launch it). wxTE_AUTO_URL drives this on the
	// platforms that support it; the text stays selectable everywhere.
	if (evt.GetMouseEvent().LeftUp()) {
		wxLaunchDefaultBrowser(m_aboutText->GetRange(evt.GetURLStart(), evt.GetURLEnd()));
	}
}

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
