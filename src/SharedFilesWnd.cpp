//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include <wx/config.h>
#include <wx/gauge.h> // Do_not_auto_remove (win32)
#include <wx/radiobut.h>

#include "SharedFilesWnd.h" // Interface declarations
#include "SharedFilesCtrl.h"
#include "SharedFilePeersListCtrl.h"
#include "muuli_wdr.h"     // Needed for ID_SHFILELIST
#include "KnownFileList.h" // Needed for CKnownFileList
#include "KnownFile.h"     // Needed for CKnownFile
#include "amule.h"         // Needed for theApp
#include "UploadQueue.h"   // Needed for theApp->uploadqueue

wxBEGIN_EVENT_TABLE(CSharedFilesWnd, wxPanel)
	EVT_LIST_ITEM_SELECTED(ID_SHFILELIST, CSharedFilesWnd::OnItemSelectionChanged)
	EVT_LIST_ITEM_DESELECTED(ID_SHFILELIST, CSharedFilesWnd::OnItemSelectionChanged)
	EVT_BUTTON(ID_BTNRELSHARED, CSharedFilesWnd::OnBtnReloadShared)
	EVT_BUTTON(ID_SHAREDCLIENTTOGGLE, CSharedFilesWnd::OnToggleClientList)
	// The "show clients for" radio buttons are bound dynamically in the ctor.

	EVT_SPLITTER_SASH_POS_CHANGING(ID_SHARESSPLATTER, CSharedFilesWnd::OnSashPositionChanging)
wxEND_EVENT_TABLE()

CSharedFilesWnd::CSharedFilesWnd(wxWindow *pParent)
: wxPanel(pParent, -1)
{
	wxSizer *content = sharedfilesDlg(this, true);
	content->Show(this, true);

	m_bar_requests = CastChild("popbar", wxGauge);
	m_bar_accepted = CastChild("popbarAccept", wxGauge);
	m_bar_transfer = CastChild("popbarTrans", wxGauge);
	m_radioShowAll = CastChild("showClientsAll", wxRadioButton);
	m_radioShowSelected = CastChild("showClientsSelected", wxRadioButton);
	m_radioShowUploading = CastChild("showClientsUploading", wxRadioButton);
	m_radioShowAll->Bind(wxEVT_RADIOBUTTON, &CSharedFilesWnd::OnSelectClientsMode, this);
	m_radioShowSelected->Bind(wxEVT_RADIOBUTTON, &CSharedFilesWnd::OnSelectClientsMode, this);
	m_radioShowUploading->Bind(wxEVT_RADIOBUTTON, &CSharedFilesWnd::OnSelectClientsMode, this);
	sharedfilesctrl = CastChild("sharedFilesCt", CSharedFilesCtrl);
	peerslistctrl = CastChild(ID_SHAREDCLIENTLIST, CSharedFilePeersListCtrl);
	wxASSERT(sharedfilesctrl);
	wxASSERT(peerslistctrl);
	m_prepared = false;

	m_splitter = 0;

	wxConfigBase *config = wxConfigBase::Get();

	// Check if the clientlist is hidden
	bool show = true;
	config->Read("/GUI/SharedWnd/ShowClientList", &show, true);
	peerslistctrl->SetShowing(show);
	// Load the last used splitter position
	m_splitter = config->Read("/GUI/SharedWnd/Splitter", 463l);
	m_clientShow = (EClientShow)config->Read("/GUI/SharedWnd/ClientShowMode", ClientShowAll);
	SetClientShowMode(m_clientShow);
}

CSharedFilesWnd::EClientShow CSharedFilesWnd::GetClientShowMode() const
{
	if (m_radioShowSelected->GetValue()) {
		return ClientShowSelected;
	}
	if (m_radioShowUploading->GetValue()) {
		return ClientShowUploading;
	}
	return ClientShowAll;
}

void CSharedFilesWnd::SetClientShowMode(EClientShow mode)
{
	switch (mode) {
	case ClientShowSelected:
		m_radioShowSelected->SetValue(true);
		break;
	case ClientShowUploading:
		m_radioShowUploading->SetValue(true);
		break;
	case ClientShowAll:
	default:
		m_radioShowAll->SetValue(true);
		break;
	}
}

CSharedFilesWnd::~CSharedFilesWnd()
{
	if (m_prepared) {
		wxConfigBase *config = wxConfigBase::Get();

		if (!peerslistctrl->GetShowing()) {
			// Save the splitter position
			config->Write("/GUI/SharedWnd/Splitter", m_splitter);

			// Save the visible status of the list
			config->Write("/GUI/SharedWnd/ShowClientList", false);
		} else {
			wxSplitterWindow *splitter = CastChild("sharedsplitterWnd", wxSplitterWindow);

			// Save the splitter position
			config->Write("/GUI/SharedWnd/Splitter", splitter->GetSashPosition());

			// Save the visible status of the list
			config->Write("/GUI/SharedWnd/ShowClientList", true);
		}
		config->Write("/GUI/SharedWnd/ClientShowMode", (int)m_clientShow);
	}
}

// Refresh just the stat bars/labels for the current selection. Cheap
// (iterates only the selected rows), so it can run on every selection change
// regardless of the client-show mode -- the client list below is the costly
// part and only depends on the selection in ClientShowSelected mode.
void CSharedFilesWnd::UpdateSelectionStats()
{
	if (sharedfilesctrl->IsSorting()) {
		return;
	}

	// Bars fill with this session's share of the file's all-time activity
	// (session / all-time -- the same two numbers as each label). Both come
	// from the per-file EC counters, so it is reliable in the remote GUI --
	// unlike a library-wide "total", which can't be gotten cheaply here -- and
	// the per-mille scale keeps TB-scale byte counts inside the gauge's int
	// range (raw bytes, even /1024, overflow it and blank the bar).
	m_bar_requests->SetRange(1000);
	m_bar_accepted->SetRange(1000);
	m_bar_transfer->SetRange(1000);

	uint32 session_requests = 0;
	uint32 session_accepted = 0;
	uint64 session_transferred = 0;
	uint32 all_requests = 0;
	uint32 all_accepted = 0;
	uint64 all_transferred = 0;
	int selectedCount = 0;

	long index = -1;
	while ((index = sharedfilesctrl->GetNextItem(index, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
		// Virtual-list control: rows map to files via the model, not per-item data.
		CKnownFile *file = sharedfilesctrl->FileAtRow(index);
		wxASSERT(file);
		session_requests += file->statistic.GetRequests();
		session_accepted += file->statistic.GetAccepts();
		session_transferred += file->statistic.GetTransferred();
		all_requests += file->statistic.GetAllTimeRequests();
		all_accepted += file->statistic.GetAllTimeAccepts();
		all_transferred += file->statistic.GetAllTimeTransferred();
		++selectedCount;
	}

	if (selectedCount == 0) {
		m_bar_requests->SetValue(0);
		CastChild(IDC_SREQUESTED, wxStaticText)->SetLabel("- / -");
		m_bar_accepted->SetValue(0);
		CastChild(IDC_SACCEPTED, wxStaticText)->SetLabel("- / -");
		m_bar_transfer->SetValue(0);
		CastChild(IDC_STRANSFERRED, wxStaticText)->SetLabel("- / -");
		return;
	}

	// Store text lengths, and layout() when the texts have grown. The label
	// always shows the selected file's real value; only the bar is a fraction.
	static uint32 lReq = 0, lAcc = 0, lTrans = 0;
	// session / all-time as a per-mille gauge fill; all-time >= session and both
	// are 64-bit, so this never overflows or exceeds the range.
	auto sharePerMille = [](uint64 sess, uint64 all) -> int {
		if (all == 0) {
			return 0;
		}
		uint64 pm = (uint64)1000 * sess / all;
		return (int)(pm > 1000 ? 1000 : pm);
	};
	// Requests
	m_bar_requests->SetValue(sharePerMille(session_requests, all_requests));
	wxString labelReq = CFormat("%d / %d") % session_requests % all_requests;
	CastChild(IDC_SREQUESTED, wxStaticText)->SetLabel(labelReq);

	// Accepted requests
	m_bar_accepted->SetValue(sharePerMille(session_accepted, all_accepted));
	wxString labelAcc = CFormat("%d / %d") % session_accepted % all_accepted;
	CastChild(IDC_SACCEPTED, wxStaticText)->SetLabel(labelAcc);

	// Transferred
	m_bar_transfer->SetValue(sharePerMille(session_transferred, all_transferred));
	wxString labelTrans = CastItoXBytes(session_transferred) + " / " + CastItoXBytes(all_transferred);
	CastChild(IDC_STRANSFERRED, wxStaticText)->SetLabel(labelTrans);

	if (labelReq.Len() > lReq || labelAcc.Len() > lAcc || labelTrans.Len() > lTrans) {
		lReq = labelReq.Len();
		lAcc = labelAcc.Len();
		lTrans = labelTrans.Len();
		s_sharedfilespeerHeader->Layout();
	}
}

void CSharedFilesWnd::SelectionUpdated()
{
	if (sharedfilesctrl->IsSorting()) {
		return;
	}

	UpdateSelectionStats();

	// The client list follows the client-show mode.
	CKnownFileVector fileVector;
	if (m_clientShow != ClientShowUploading) {
		long index = -1;
		int filter =
			(m_clientShow == ClientShowSelected) ? wxLIST_STATE_SELECTED : wxLIST_STATE_DONTCARE;
		while ((index = sharedfilesctrl->GetNextItem(index, wxLIST_NEXT_ALL, filter)) != -1) {
			fileVector.push_back(sharedfilesctrl->FileAtRow(index));
		}
		// ShowSources() requires the file vector sorted (see CGenericClientListCtrl).
		std::sort(fileVector.begin(), fileVector.end());
	} else {
		// The GenericClientListCtrl is designed to show clients associated with a KnownFile.
		// So the uploadqueue carries a special known file with all ongoing uploads in its
		// upload list. This is a hack, but easier than trying to bend the class into a shape
		// it was not intended for to show all clients currently uploading.
#ifdef CLIENT_GUI
		fileVector.push_back(theApp->m_allUploadingKnownFile);
#else
		fileVector.push_back(theApp->uploadqueue->GetAllUploadingKnownFile());
#endif
	}
	peerslistctrl->ShowSources(fileVector);

	Refresh();
	Layout();
}

void CSharedFilesWnd::OnBtnReloadShared(wxCommandEvent &WXUNUSED(evt))
{
	theApp->sharedfiles->Reload();
#ifndef CLIENT_GUI
	// remote gui will update display when data is back
	SelectionUpdated();
#endif
}

void CSharedFilesWnd::OnItemSelectionChanged(wxListEvent &evt)
{
	if (GetClientShowMode() == ClientShowSelected) {
		// The client list is selection-driven here, so do the full update.
		SelectionUpdated();
	} else {
		// Other modes: the client list shows all / all-uploading clients and is
		// not selection-driven, so only the stat panel needs refreshing -- and
		// immediately, instead of waiting for the file's next periodic update.
		UpdateSelectionStats();
	}

	evt.Skip();
}

void CSharedFilesWnd::RemoveAllSharedFiles()
{
	sharedfilesctrl->ClearList();
	sharedfilesctrl->ShowFilesCount();
	SelectionUpdated();
}

void CSharedFilesWnd::Prepare()
{
	if (m_prepared) {
		return;
	}
	m_prepared = true;
	wxSplitterWindow *splitter = CastChild("sharedsplitterWnd", wxSplitterWindow);
	int height = splitter->GetSize().GetHeight();
	int header_height = s_sharedfilespeerHeader->GetSize().GetHeight();

	if (m_splitter) {
		// Some sanity checking
		if (m_splitter < s_splitterMin) {
			m_splitter = s_splitterMin;
		} else if (m_splitter > height - header_height * 2) {
			m_splitter = height - header_height * 2;
		}
		splitter->SetSashPosition(m_splitter);
		m_splitter = 0;
	}

	if (!peerslistctrl->GetShowing()) {
		// use a toggle event to close it (calculate size, change button)
		peerslistctrl->SetShowing(true); // so it will be toggled to false
		wxCommandEvent evt1;
		OnToggleClientList(evt1);
	}
}

void CSharedFilesWnd::OnToggleClientList(wxCommandEvent &WXUNUSED(evt))
{
	wxSplitterWindow *splitter = CastChild("sharedsplitterWnd", wxSplitterWindow);
	wxBitmapButton *button = CastChild(ID_SHAREDCLIENTTOGGLE, wxBitmapButton);

	if (!peerslistctrl->GetShowing()) {
		splitter->SetSashPosition(m_splitter);
		m_splitter = 0;

		peerslistctrl->SetShowing(true);

		button->SetBitmapLabel(amuleDlgImages(10));
		button->SetBitmapFocus(amuleDlgImages(10));
		button->SetBitmapSelected(amuleDlgImages(10));
		button->SetBitmapHover(amuleDlgImages(10));
	} else {
		peerslistctrl->SetShowing(false);

		m_splitter = splitter->GetSashPosition();

		// Add the height of the listctrl to the top-window
		int height =
			peerslistctrl->GetSize().GetHeight() + splitter->GetWindow1()->GetSize().GetHeight();

		splitter->SetSashPosition(height);

		button->SetBitmapLabel(amuleDlgImages(11));
		button->SetBitmapFocus(amuleDlgImages(11));
		button->SetBitmapSelected(amuleDlgImages(11));
		button->SetBitmapHover(amuleDlgImages(11));
	}
}

void CSharedFilesWnd::OnSashPositionChanging(wxSplitterEvent &evt)
{
	if (evt.GetSashPosition() < s_splitterMin) {
		evt.SetSashPosition(s_splitterMin);
	} else {
		wxSplitterWindow *splitter = wxStaticCast(evt.GetEventObject(), wxSplitterWindow);
		wxCHECK_RET(splitter, "ERROR: NULL splitter in CSharedFilesWnd::OnSashPositionChanging");

		int height = splitter->GetSize().GetHeight();
		int header_height = s_sharedfilespeerHeader->GetSize().GetHeight();
		int mousey = wxGetMousePosition().y - splitter->GetScreenRect().GetTop();

		if (!peerslistctrl->GetShowing()) {
			// lower window hidden
			if (height - mousey < header_height * 2) {
				// no moving down if already hidden
				evt.Veto();
			} else {
				// show it
				m_splitter = mousey; // prevent jumping if it was minimized and is then shown
						     // by dragging the sash
				wxCommandEvent evt1;
				OnToggleClientList(evt1);
			}
		} else {
			// lower window showing
			if (height - mousey < header_height * 2) {
				// hide it
				wxCommandEvent evt1;
				OnToggleClientList(evt1);
			} else {
				// normal resize
				// If several events queue up, setting the sash to the current mouse position
				// will speed up things and make sash moving more smoothly.
				evt.SetSashPosition(mousey);
			}
		}
	}
}

void CSharedFilesWnd::OnSelectClientsMode(wxCommandEvent &WXUNUSED(evt))
{
	EClientShow clientShowLast = m_clientShow;
	m_clientShow = GetClientShowMode();

	if (m_clientShow != clientShowLast) {
		SelectionUpdated();
	}
}

// File_checked_for_headers
