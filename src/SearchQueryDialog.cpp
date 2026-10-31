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

#include "SearchQueryDialog.h"

#include "SearchMetadataPanel.h"
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

CSearchQueryDialog::CSearchQueryDialog(wxWindow *parent, const wxString &query)
: wxDialog(parent,
	  wxID_ANY,
	  _("Translate search query"),
	  wxDefaultPosition,
	  wxDefaultSize,
	  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	auto *layout = new wxBoxSizer(wxVERTICAL);
	auto *help = new wxStaticText(this,
		wxID_ANY,
		_("Use artist:, album:, title:, codec:, duration: and bitrate:. Quote values containing "
		  "spaces.\n"
		  "Example: jazz artist:\"Miles Davis\" duration:>=5m bitrate:>=192\n"
		  "Preview replaces the six media filters; other search options stay as they are."));
	help->Wrap(FromDIP(700));
	layout->Add(help, wxSizerFlags().Expand().Border(wxALL, 10));
	m_query = new wxTextCtrl(
		this, wxID_ANY, query, wxDefaultPosition, FromDIP(wxSize(700, 70)), wxTE_MULTILINE);
	m_query->SetName(_("Query to translate"));
	layout->Add(m_query, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT, 10));
	auto *translate = new wxButton(this, wxID_ANY, _("Preview"));
	layout->Add(translate, wxSizerFlags().Border(wxALL, 10));
	layout->Add(new wxStaticText(this, wxID_ANY, _("Keywords (editable preview)")),
		wxSizerFlags().Border(wxLEFT | wxRIGHT, 10));
	m_keywords = new wxTextCtrl(
		this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
	m_keywords->SetName(_("Preview keywords"));
	layout->Add(m_keywords, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 10));
	m_metadata = new CSearchMetadataPanel(this);
	layout->Add(m_metadata, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT, 10));
	m_error = new wxStaticText(this, wxID_ANY, wxEmptyString);
	layout->Add(m_error, wxSizerFlags().Expand().Border(wxALL, 10));
	auto *buttons = new wxStdDialogButtonSizer;
	m_apply = new wxButton(this, wxID_OK, _("Use preview"));
	m_apply->Enable(false);
	buttons->AddButton(m_apply);
	buttons->AddButton(new wxButton(this, wxID_CANCEL));
	buttons->Realize();
	layout->Add(buttons, wxSizerFlags().Right().Border(wxALL, 10));
	SetSizerAndFit(layout);
	SetMinSize(GetSize());
	CentreOnParent();
	// Enter in an editable preview must not regenerate it and discard manual edits.
	m_keywords->Bind(wxEVT_TEXT_ENTER, [](wxCommandEvent &) {});
	m_metadata->Bind(wxEVT_TEXT_ENTER, [](wxCommandEvent &) {});
	translate->SetDefault();
	translate->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { PreviewQuery(); });
	m_query->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { m_apply->Enable(false); });
	Bind(
		wxEVT_BUTTON,
		[this](wxCommandEvent &) {
			CSearchQueryPreview preview;
			if (ReadPreview(preview))
				EndModal(wxID_OK);
		},
		wxID_OK);
}

bool CSearchQueryDialog::PreviewQuery()
{
	CSearchQueryPreview preview;
	wxString error;
	const bool valid = TranslateSearchQuery(m_query->GetValue(), preview, error);
	m_error->SetLabel(error);
	m_error->Wrap(FromDIP(700));
	m_apply->Enable(valid);
	if (valid) {
		m_keywords->ChangeValue(preview.keywords);
		m_metadata->Set(preview.metadata);
	}
	Layout();
	return valid;
}

bool CSearchQueryDialog::ReadPreview(CSearchQueryPreview &preview)
{
	if (!m_apply->IsEnabled())
		return false;
	CSearchQueryPreview value;
	value.keywords = wxString(m_keywords->GetValue()).Trim(true).Trim(false);
	if (value.keywords.find(wxChar(0)) != wxString::npos) {
		m_error->SetLabel(_("Search input must not contain NUL characters."));
		m_keywords->SetFocus();
		Layout();
		return false;
	}
	m_metadata->Read(value.metadata);
	if (value.keywords.empty()) {
		m_error->SetLabel(_("Enter keywords before using the preview."));
		m_keywords->SetFocus();
		Layout();
		return false;
	}
	preview = value;
	m_error->SetLabel(wxEmptyString);
	Layout();
	return true;
}
