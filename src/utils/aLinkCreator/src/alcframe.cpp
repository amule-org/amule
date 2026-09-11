////////////////////////////////////////////////////////////////////////////////
/// Name:         AlcFrame Class
///
/// Purpose:      aMule ed2k link creator
///
/// Author:       ThePolish <thepolish@vipmail.ru>
///
/// Copyright (c) 2004-2011 ThePolish ( thepolish@vipmail.ru )
///
/// Copyright (c) 2004-2011 Marcelo Roberto Jimenez ( phoenix@amule.org )
///
/// Pixmaps from:
///	http://jimmac.musichall.cz/ikony.php3
///	http://www.everaldo.com
///	http://www.icomania.com
///
/// This program is free software; you can redistribute it and/or modify
/// it under the terms of the GNU General Public License as published by
/// (at your option) any later version.
///
/// This program is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// the Free Software Foundation; either version 2 of the License, or
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU General Public License for more details.
///
/// You should have received a copy of the GNU General Public License
/// along with this program; if not, write to the
/// Free Software Foundation, Inc.,
/// 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
////////////////////////////////////////////////////////////////////////////////

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/button.h>
#include <wx/icon.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textfile.h>
#include <wx/timer.h>
#include <wx/toolbar.h>
#include <wx/uri.h>
#include <wx/url.h>

#ifdef __WINDOWS__
#include <winerror.h>
#include <shlobj.h>
#elif defined(__WXMAC__)
#include <wx/intl.h>
#endif

#include "md4.h"
#include "ed2khash.h"
#include "alcframe.h"
#include "alcpix.h"
#include "alc.h"

AlcFrame::AlcFrame(const wxString &title)
: wxFrame((wxFrame *)NULL, -1, title)
{
#ifdef __WINDOWS__
	wxIcon icon("alc");
#else
	wxIcon icon;
	icon.CopyFromBitmap(AlcPix::getPixmap("alc"));
#endif
	SetIcon(icon);

	CreateStatusBar();
	SetStatusText(_("Welcome!"));

	m_progressBar = NULL;

	m_frameVBox = new wxBoxSizer(wxVERTICAL);

	m_mainPanel = new wxPanel(this, -1);

	m_mainPanelVBox = new wxBoxSizer(wxVERTICAL);

	m_staticLine = new wxStaticLine(m_mainPanel, -1);
	m_mainPanelVBox->Add(m_staticLine, wxSizerFlags().Expand().Border(wxALL, 0));

	m_inputSBox = new wxStaticBox(m_mainPanel, -1, _("Input parameters"));
	m_inputSBoxSizer = new wxStaticBoxSizer(m_inputSBox, wxHORIZONTAL);

	m_inputFlexSizer = new wxFlexGridSizer(6, 2, 5, 10);

	m_inputFlexSizer->AddGrowableCol(0);

	m_inputFileStaticText = new wxStaticText(
		m_mainPanel, -1, _("File to Hash"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE);

	m_inputAddStaticText = new wxStaticText(m_mainPanel,
		-1,
		_("Add Optional URLs for this file"),
		wxDefaultPosition,
		wxDefaultSize,
		wxALIGN_CENTRE);

	m_inputFileTextCtrl = new wxTextCtrl(m_mainPanel, -1, "", wxDefaultPosition, wxSize(300, -1));
	m_inputFileTextCtrl->SetToolTip(_("Enter here the file you want to compute the eD2k link"));

	m_inputAddTextCtrl = new wxTextCtrl(m_mainPanel, -1, "", wxDefaultPosition, wxSize(300, -1));
	m_inputAddTextCtrl->SetToolTip(_("Enter here the URL you want to add to the eD2k link: Add / at the "
					 "end to let aLinkCreator append the current file name"));

	m_inputUrlListBox = new wxListBox(m_mainPanel,
		-1,
		wxDefaultPosition,
		wxDefaultSize,
		0,
		NULL,
		wxLB_SINGLE | wxLB_NEEDED_SB | wxLB_HSCROLL);

	m_inputFileBrowseButton = new wxButton(m_mainPanel, ID_BROWSE_BUTTON, wxString(_("Browse")));

	m_inputAddButton = new wxButton(m_mainPanel, ID_ADD_BUTTON, wxString(_("Add")));

	m_buttonUrlVBox = new wxBoxSizer(wxVERTICAL);
	m_removeButton = new wxButton(m_mainPanel, ID_REMOVE_BUTTON, wxString(_("Remove")));
	m_clearButton = new wxButton(m_mainPanel, ID_CLEAR_BUTTON, wxString(_("Clear")));

	m_buttonUrlVBox->Add(m_removeButton, wxSizerFlags().Center().Border(wxTOP | wxBOTTOM, 5));
	m_buttonUrlVBox->Add(m_clearButton, wxSizerFlags().Center().Border(wxTOP | wxBOTTOM, 5));

	m_parthashesCheck =
		new wxCheckBox(m_mainPanel, ID_PARTHASHES_CHECK, _("Create link with part-hashes"));

	m_parthashesCheck->SetValue(false);

	m_parthashesCheck->SetToolTip(
		_("Help to spread new and rare files faster, at the cost of an increased link size"));

	m_inputFlexSizer->Add(m_inputFileStaticText, wxSizerFlags(1).Expand().Bottom().Border(wxTOP, 10));
	m_inputFlexSizer->Add(1, 1);

	m_inputFlexSizer->Add(m_inputFileTextCtrl, wxSizerFlags(1).Expand());
	m_inputFlexSizer->Add(m_inputFileBrowseButton, wxSizerFlags().Expand());

	m_inputFlexSizer->Add(m_inputAddStaticText, wxSizerFlags(1).Expand().Bottom().Border(wxTOP, 10));
	m_inputFlexSizer->Add(1, 1);

	m_inputFlexSizer->Add(m_inputAddTextCtrl, wxSizerFlags(1).Expand());
	m_inputFlexSizer->Add(m_inputAddButton, wxSizerFlags().Expand());

	m_inputFlexSizer->Add(m_inputUrlListBox, wxSizerFlags().Expand().Center());
	m_inputFlexSizer->Add(m_buttonUrlVBox, wxSizerFlags().Expand().Center());

	m_inputFlexSizer->Add(m_parthashesCheck, wxSizerFlags().Expand().Center().Border(wxTOP, 10));
	m_inputFlexSizer->Add(1, 1);

	m_inputSBoxSizer->Add(m_inputFlexSizer, wxSizerFlags(1).Expand().Center().Border(wxALL, 10));
	m_mainPanelVBox->Add(m_inputSBoxSizer, wxSizerFlags().Expand().Center().Border(wxALL, 10));

#ifdef WANT_MD4SUM
	m_md4HashSBox = new wxStaticBox(m_mainPanel, -1, _("MD4 File Hash"));
	m_md4HashSBoxSizer = new wxStaticBoxSizer(m_md4HashSBox, wxHORIZONTAL);

	m_md4HashTextCtrl =
		new wxTextCtrl(m_mainPanel, -1, "", wxDefaultPosition, wxDefaultSize, wxTE_READONLY);

	m_md4HashSBoxSizer->Add(m_md4HashTextCtrl, wxSizerFlags(1).Center().Border(wxALL, 5));
	m_mainPanelVBox->Add(m_md4HashSBoxSizer, wxSizerFlags().Expand().Border(wxALL, 10));
#endif

	m_e2kHashSBox = new wxStaticBox(m_mainPanel, -1, _("eD2k File Hash"));
	m_e2kHashSBoxSizer = new wxStaticBoxSizer(m_e2kHashSBox, wxHORIZONTAL);

	m_e2kHashTextCtrl =
		new wxTextCtrl(m_mainPanel, -1, "", wxDefaultPosition, wxDefaultSize, wxTE_READONLY);

	m_e2kHashSBoxSizer->Add(m_e2kHashTextCtrl, wxSizerFlags(1).Center().Border(wxALL, 5));
	m_mainPanelVBox->Add(m_e2kHashSBoxSizer, wxSizerFlags().Expand().Border(wxALL, 10));

	m_ed2kSBox = new wxStaticBox(m_mainPanel, -1, _("eD2k link"));
	m_ed2kSBoxSizer = new wxStaticBoxSizer(m_ed2kSBox, wxVERTICAL);

	m_ed2kTextCtrl = new wxTextCtrl(m_mainPanel,
		-1,
		"",
		wxDefaultPosition,
		wxSize(-1, 60),
		wxTE_MULTILINE | wxTE_READONLY | wxVSCROLL);

	m_ed2kSBoxSizer->Add(m_ed2kTextCtrl, wxSizerFlags(1).Expand().Border(wxALL, 5));
	m_mainPanelVBox->Add(m_ed2kSBoxSizer, wxSizerFlags(1).Expand().Border(wxALL, 10));

	m_buttonHBox = new wxBoxSizer(wxHORIZONTAL);
	m_startButton = new wxButton(m_mainPanel, ID_START_BUTTON, wxString(_("Start")));
	m_saveButton = new wxButton(m_mainPanel, ID_SAVEAS_BUTTON, wxString(_("Save")));
	m_copyButton = new wxButton(m_mainPanel, ID_COPY_BUTTON, wxString(_("Copy to clipboard")));
	m_closeButton = new wxButton(m_mainPanel, ID_EXIT_BUTTON, wxString(_("Exit")));

	m_buttonHBox->Add(m_copyButton, wxSizerFlags().Border(wxALL, 5));
	m_buttonHBox->Add(1, 1, 1);
	m_buttonHBox->Add(m_startButton, wxSizerFlags().Right().Border(wxALL, 5));
	m_buttonHBox->Add(m_saveButton, wxSizerFlags().Right().Border(wxALL, 5));
	m_buttonHBox->Add(m_closeButton, wxSizerFlags().Right().Border(wxALL, 5));

	m_mainPanelVBox->Add(m_buttonHBox, wxSizerFlags().Expand().Border(wxALL, 5));

	m_toolBarBitmaps[0] = AlcPix::getPixmap("open");
	m_toolBarBitmaps[1] = AlcPix::getPixmap("copy");
	m_toolBarBitmaps[2] = AlcPix::getPixmap("saveas");
	m_toolBarBitmaps[3] = AlcPix::getPixmap("about");

	m_toolbar = new wxToolBar(this, -1, wxDefaultPosition, wxDefaultSize, wxTB_HORIZONTAL | wxTB_FLAT);

	m_toolbar->SetToolBitmapSize(wxSize(32, 32));
	m_toolbar->SetMargins(2, 2);

	m_toolbar->AddTool(ID_BAR_OPEN,
		_("Open"),
		wxBitmapBundle(m_toolBarBitmaps[0]),
		_("Open a file to compute its eD2k link"));

	m_toolbar->AddTool(ID_BAR_COPY,
		_("Copy"),
		wxBitmapBundle(m_toolBarBitmaps[1]),
		_("Copy computed eD2k link to clipboard"));

	m_toolbar->AddTool(ID_BAR_SAVEAS,
		_("Save as"),
		wxBitmapBundle(m_toolBarBitmaps[2]),
		_("Save computed eD2k link to file"));

	m_toolbar->AddSeparator();

	m_toolbar->AddTool(
		ID_BAR_ABOUT, _("About"), wxBitmapBundle(m_toolBarBitmaps[3]), _("About aLinkCreator"));

	m_toolbar->Realize();

	SetToolBar(m_toolbar);

	m_mainPanel->SetAutoLayout(true);
	m_mainPanel->SetSizerAndFit(m_mainPanelVBox);

	m_frameVBox->Add(m_mainPanel, wxSizerFlags(1).Expand().Border(wxALL, 0));
	SetAutoLayout(true);
	SetSizerAndFit(m_frameVBox);

	m_startButton->SetFocus();
}

AlcFrame::~AlcFrame() {}

wxBEGIN_EVENT_TABLE(AlcFrame, wxFrame)
	EVT_TOOL(ID_BAR_OPEN, AlcFrame::OnBarOpen)
	EVT_TOOL(ID_BAR_SAVEAS, AlcFrame::OnBarSaveAs)
	EVT_TOOL(ID_BAR_COPY, AlcFrame::OnBarCopy)
	EVT_TOOL(ID_BAR_ABOUT, AlcFrame::OnBarAbout)
	EVT_BUTTON(ID_START_BUTTON, AlcFrame::OnStartButton)
	EVT_BUTTON(ID_EXIT_BUTTON, AlcFrame::OnCloseButton)
	EVT_BUTTON(ID_SAVEAS_BUTTON, AlcFrame::OnSaveAsButton)
	EVT_BUTTON(ID_COPY_BUTTON, AlcFrame::OnCopyButton)
	EVT_BUTTON(ID_BROWSE_BUTTON, AlcFrame::OnBrowseButton)
	EVT_BUTTON(ID_ADD_BUTTON, AlcFrame::OnAddUrlButton)
	EVT_BUTTON(ID_REMOVE_BUTTON, AlcFrame::OnRemoveUrlButton)
	EVT_BUTTON(ID_CLEAR_BUTTON, AlcFrame::OnClearUrlButton)
wxEND_EVENT_TABLE()

void AlcFrame::OnBarOpen(wxCommandEvent &WXUNUSED(event))
{
	SetFileToHash();
}

void AlcFrame::OnBrowseButton(wxCommandEvent &WXUNUSED(event))
{
	SetFileToHash();
}

void AlcFrame::SetFileToHash()
{
#ifdef __WINDOWS__
	wxString browseroot;
	LPITEMIDLIST pidl;
	HRESULT hr = SHGetSpecialFolderLocation(NULL, CSIDL_PERSONAL, &pidl);
	if (SUCCEEDED(hr)) {
		if (!SHGetPathFromIDList(pidl, wxStringBuffer(browseroot, MAX_PATH))) {
			browseroot = wxFileName::GetHomeDir();
		}
	} else {
		browseroot = wxFileName::GetHomeDir();
	}
	if (pidl) {
		LPMALLOC pMalloc;
		SHGetMalloc(&pMalloc);
		if (pMalloc) {
			pMalloc->Free(pidl);
			pMalloc->Release();
		}
	}
#elif defined(__WXMAC__)

	// ~/Documents always exists on macOS and matches what
	// FSFindFolder(kUserDomain, kDocumentsFolderType, ...) used to
	// return via the Carbon FSRef API (removed in 64-bit macOS).
	wxString browseroot;
	const char *home = getenv("HOME");
	if (home) {
		browseroot = wxString::FromUTF8(home) + "/Documents";
	} else {
		browseroot = wxFileName::GetHomeDir();
	}

#else
	wxString browseroot = wxFileName::GetHomeDir();
#endif
	const wxString &filename = wxFileSelector(_("Select the file you want to compute the eD2k link"),
		browseroot,
		"",
		"",
		"*.*",
		wxFD_OPEN | wxFD_FILE_MUST_EXIST,
		this);

	if (!filename.empty()) {
		m_inputFileTextCtrl->SetValue(filename);
	}
}

void AlcFrame::OnBarSaveAs(wxCommandEvent &WXUNUSED(event))
{
	SaveEd2kLinkToFile();
}

void AlcFrame::OnSaveAsButton(wxCommandEvent &WXUNUSED(event))
{
	SaveEd2kLinkToFile();
}

void AlcFrame::CopyEd2kLinkToClipBoard()
{
	wxString link = m_ed2kTextCtrl->GetValue();
	if (!link.IsEmpty()) {
		wxClipboardLocker clipLocker;
		if (!clipLocker) {
			wxLogError(_("Can't open the clipboard"));

			return;
		}

		wxTheClipboard->AddData(new wxTextDataObject(link));
	} else {
		SetStatusText(_("Nothing to copy for now !"));
	}
}

void AlcFrame::OnCopyButton(wxCommandEvent &WXUNUSED(event))
{
	CopyEd2kLinkToClipBoard();
}

void AlcFrame::OnBarCopy(wxCommandEvent &WXUNUSED(event))
{
	CopyEd2kLinkToClipBoard();
}

void AlcFrame::SaveEd2kLinkToFile()
{
	wxString link(m_ed2kTextCtrl->GetValue());

	if (!link.IsEmpty()) {
		const wxString &filename = wxFileSelector(_("Select the file to your computed eD2k link"),
			wxFileName::GetHomeDir(),
			"my_ed2k_link",
			"txt",
			"*.txt",
			wxFD_SAVE,
			this);

		if (!filename.empty()) {
			// Open file and let wxFile destructor close the file
			// Closing it explicitly may crash on Win32 ...
			wxFile file(filename, wxFile::write_append);
			if (!file.IsOpened()) {
				SetStatusText(_("Unable to open ") + filename);
				return;
			}
			file.Write(link + wxTextFile::GetEOL());
		} else {
			SetStatusText(_("Please, enter a non empty file name"));
		}
	} else {
		SetStatusText(_("Nothing to save for now !"));
	}
}

void AlcFrame::OnBarAbout(wxCommandEvent &WXUNUSED(event))
{
	wxMessageBox(_("aLinkCreator, the aMule eD2k link creator\n\n(c) 2004 ThePolish "
		       "<thepolish@vipmail.ru>\n\nPixmaps from http://www.everaldo.com and "
		       "http://www.icomania.com\nand http://jimmac.musichall.cz/ikony.php3\n\nDistributed "
		       "under GPL"),
		_("About aLinkCreator"),
		wxOK | wxCENTRE | wxICON_INFORMATION);
}

void AlcFrame::OnCloseButton(wxCommandEvent &WXUNUSED(event))
{
	Close(false);
}

/// Hook into MD4/ED2K routine
bool AlcFrame::Hook(int percent)
{
	// Update progress bar
	bool goAhead = ::wxGetApp().GetMainFrame()->m_progressBar->Update(percent);
	if (!goAhead) {
		// Destroying progressbar: no merci for croissants !
		::wxGetApp().GetMainFrame()->m_progressBar->Destroy();
		// Now, be paranoid
		delete ::wxGetApp().GetMainFrame()->m_progressBar;
		::wxGetApp().GetMainFrame()->m_progressBar = NULL;
	}

	return (goAhead);
}

/// Compute Hashes on Start Button
void AlcFrame::OnStartButton(wxCommandEvent &WXUNUSED(event))
{
	wxString filename = m_inputFileTextCtrl->GetValue();

	if (!filename.empty()) {
		// Chrono
		wxStopWatch chrono;

		// wxFileName needed for base name
		wxFileName fileToHash(filename);

		// Set waiting msg
		m_e2kHashTextCtrl->SetValue(_("Hashing..."));
		m_ed2kTextCtrl->SetValue(_("Hashing..."));

#ifdef WANT_MD4SUM
		// Create MD4 progress bar dialog
		m_progressBar = new wxProgressDialog(_("aLinkCreator is working for you"),
			_("Computing MD4 Hash..."),
			100,
			this,
			wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_REMAINING_TIME);
		m_md4HashTextCtrl->SetValue(_("Hashing..."));

		// Md4 hash
		MD4 md4;
		m_md4HashTextCtrl->SetValue(md4.calcMd4FromFile(filename, Hook));

		// Deleting MD4 progress bar dialog
		delete m_progressBar;
		m_progressBar = NULL;

#endif

		// Create ED2K progress bar dialog
		m_progressBar = new wxProgressDialog(_("aLinkCreator is working for you"),
			_("Computing eD2k Hashes..."),
			100,
			this,
			wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_REMAINING_TIME);

		// Compute ed2k Hash
		Ed2kHash hash;

		// Test the return value to see if was aborted.
		if (hash.SetED2KHashFromFile(filename, Hook)) {

			wxArrayString ed2kHash(hash.GetED2KHash());

			// Get URLs
			wxArrayString arrayOfUrls;
			wxString url;
			for (size_t i = 0; i < m_inputUrlListBox->GetCount(); ++i) {
				url = m_inputUrlListBox->GetString(i);
				if (url.Right(1) == "/") {
					url += fileToHash.GetFullName();
				}
				arrayOfUrls.Add(wxURI(url).BuildURI());
			}
			arrayOfUrls.Shrink(); // Reduce memory usage

			// Ed2k hash
			m_e2kHashTextCtrl->SetValue(ed2kHash.Last());

			// Ed2k link
			m_ed2kTextCtrl->SetValue(
				hash.GetED2KLink(m_parthashesCheck->IsChecked(), &arrayOfUrls));
		} else {
			// Set cancelled msg
			m_e2kHashTextCtrl->SetValue(_("Cancelled !"));
			m_ed2kTextCtrl->SetValue(_("Cancelled !"));
		}

		// Deleting progress bar dialog
		delete m_progressBar;
		m_progressBar = NULL;

		// Set status text
		SetStatusText(wxString::Format(_("Done in %.2f s"), chrono.Time() * .001));
	} else {
		// Set status text
		SetStatusText(_("Please, enter a non empty file name"));
	}
}

/// Add an URL to the URL list box
void AlcFrame::OnAddUrlButton(wxCommandEvent &WXUNUSED(event))
{
	wxString url(m_inputAddTextCtrl->GetValue());

	if (!url.IsEmpty()) {
		// Check if the URL already exist in list
		bool UrlNotExists = true;
		for (size_t i = 0; i < m_inputUrlListBox->GetCount(); ++i) {
			if (url == m_inputUrlListBox->GetString(i)) {
				UrlNotExists = false;
				break;
			}
		}

		// Add only a not already existent URL
		if (UrlNotExists) {
			m_inputUrlListBox->Append(wxURI(url).BuildURI());
			m_inputAddTextCtrl->SetValue("");
		} else {
			wxLogError(_("You have already added this URL !"));
		}
	} else {
		SetStatusText(_("Please, enter a non empty URL"));
	}
}

/// Remove the selected URL from the URL list box
void AlcFrame::OnRemoveUrlButton(wxCommandEvent &WXUNUSED(event))
{
	m_inputUrlListBox->Delete(m_inputUrlListBox->GetSelection());
}

/// Clear the URL list box
void AlcFrame::OnClearUrlButton(wxCommandEvent &WXUNUSED(event))
{
	m_inputUrlListBox->Clear();
}
// File_checked_for_headers
