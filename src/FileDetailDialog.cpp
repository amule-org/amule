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

#include "muuli_wdr.h"          // Needed for ID_CLOSEWNDFD,...,IDC_APPLY
#include "FileDetailDialog.h"   // Interface declarations
#include "FileDetailListCtrl.h" // Needed for CFileDetailListCtrl
#include "CommentDialogLst.h"   // Needed for CCommentDialogLst
#include "PartFile.h"           // Needed for CPartFile
#include "amule.h"              // Needed for theApp
#include "SharedFileList.h"     // Needed for CSharedFileList
#include "OtherFunctions.h"
#include "DataToText.h" // Needed for PriorityToStr
#include "MuleColour.h"
#include <tags/FileTags.h> // Needed for FT_MEDIA_* metadata tag names

#include <set>

#define ID_MY_TIMER 1652

// IMPLEMENT_DYNAMIC(CFileDetailDialog, CDialog)
wxBEGIN_EVENT_TABLE(CFileDetailDialog, wxDialog)
	EVT_BUTTON(ID_CLOSEWNDFD, CFileDetailDialog::OnClosewnd)
	EVT_BUTTON(IDC_BUTTONSTRIP, CFileDetailDialog::OnBnClickedButtonStrip)
	EVT_BUTTON(IDC_TAKEOVER, CFileDetailDialog::OnBnClickedTakeOver)
	EVT_LIST_ITEM_ACTIVATED(IDC_LISTCTRLFILENAMES, CFileDetailDialog::OnListClickedTakeOver)
	EVT_BUTTON(IDC_CMTBT, CFileDetailDialog::OnBnClickedShowComment)
	EVT_TEXT(IDC_FILENAME, CFileDetailDialog::OnTextFileNameChange)
	EVT_BUTTON(IDC_APPLY_AND_CLOSE, CFileDetailDialog::OnBnClickedOk)
	EVT_BUTTON(IDC_APPLY, CFileDetailDialog::OnBnClickedApply)
	EVT_BUTTON(IDC_PREVFILE, CFileDetailDialog::OnBnClickedPrevFile)
	EVT_BUTTON(IDC_NEXTFILE, CFileDetailDialog::OnBnClickedNextFile)
	EVT_TIMER(ID_MY_TIMER, CFileDetailDialog::OnTimer)
wxEND_EVENT_TABLE()

namespace
{
// Registry of open CFileDetailDialog instances. See CCommentDialog.cpp
// for the rationale — the broadcast handler in GuiEvents.cpp iterates
// this on every CKnownFile destruction. UAF would otherwise fire from
// the 5-second update-timer's deref of m_file (issue #755, same family
// as #748).
std::set<CFileDetailDialog *> &OpenInstances()
{
	static std::set<CFileDetailDialog *> instances;
	return instances;
}
} // namespace

CFileDetailDialog::CFileDetailDialog(wxWindow *parent, std::vector<CKnownFile *> &files, int index)
: wxDialog(parent,
	  -1,
	  _("File Details"),
	  wxDefaultPosition,
	  wxDefaultSize,
	  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX | wxMINIMIZE_BOX)
, m_files(files)
, m_index(index)
, m_filenameChanged(false)
{
	m_timer.SetOwner(this, ID_MY_TIMER);
	m_timer.Start(5000);
	wxSizer *content = fileDetails(this, true);
	m_file = m_files[m_index];
	UpdateData(true);
	content->SetSizeHints(this);
	content->Show(this, true);
	OpenInstances().insert(this);
}

CFileDetailDialog::~CFileDetailDialog()
{
	OpenInstances().erase(this);
	m_timer.Stop();
}

void CFileDetailDialog::DropReferencesTo(const CKnownFile *file)
{
	for (CFileDetailDialog *d : OpenInstances()) {
		// Strip the file from m_files first so Next/Prev navigation
		// doesn't re-select it. m_files is a reference to the
		// caller's vector (CDownloadListCtrl's stack-allocated list
		// of selected files), so erasing here mutates the caller's
		// state too — that's fine; the caller holds it only for the
		// duration of the modal dialog and the dialog is what owns
		// the visible UI sourcing from it.
		for (std::vector<CKnownFile *>::iterator it = d->m_files.begin(); it != d->m_files.end();
			/* manual ++ */) {
			if (*it == file) {
				ptrdiff_t offset = it - d->m_files.begin();
				it = d->m_files.erase(it);
				if (d->m_index > offset) {
					--d->m_index;
				} else if (d->m_index == offset) {
					// The active file is the one being
					// destroyed. The dialog will dismiss
					// below, so the index value won't be
					// read again.
				}
			} else {
				++it;
			}
		}
		// Dismiss if the active file vanished. Stop the update
		// timer first so the next tick doesn't try to deref
		// m_file before EndModal unwinds.
		if (d->m_file == file) {
			d->m_file = NULL;
			d->m_timer.Stop();
			d->EndModal(0);
		}
	}
}

void CFileDetailDialog::OnTimer(wxTimerEvent &WXUNUSED(evt))
{
	UpdateData(false);
}

void CFileDetailDialog::OnClosewnd(wxCommandEvent &WXUNUSED(evt))
{
	EndModal(0);
}

void CFileDetailDialog::UpdateData(bool resetFilename)
{
	wxString bufferS;

	// A file is "downloading" only while it is an incomplete partfile.
	// CPartFile::IsPartFile() already returns false once complete (the object
	// itself may linger as a CPartFile until the next restart), so this is the
	// authoritative test — not the concrete type. Works in amulegui too, where
	// the proxy CPartFile overrides IsPartFile() identically.
	CPartFile *part = m_file->IsPartFile() ? static_cast<CPartFile *>(m_file) : nullptr;

	// --- Common fields (present on every CKnownFile) ---
	CastChild(IDC_FNAME, wxStaticText)
		->SetLabel(MakeStringEscaped(m_file->GetFileName().TruncatePath(60)));
	// "met-File": the .part.met path for a partfile, else the on-disk full path.
	CPath metPath = part ? part->GetFullName() : m_file->GetFilePath().JoinPaths(m_file->GetFileName());
	CastChild(IDC_METFILE, wxStaticText)->SetLabel(MakeStringEscaped(metPath.TruncatePath(60, true)));

	if (resetFilename) {
		resetValueForFilenameTextEdit();
	}

	CastChild(IDC_FHASH, wxStaticText)->SetLabel(m_file->GetFileHash().Encode());
	bufferS = CFormat("%u bytes (%s)") % m_file->GetFileSize() % CastItoXBytes(m_file->GetFileSize());
	CastChild(IDC_FSIZE, wxControl)->SetLabel(bufferS);

	// --- Download section (only an in-progress partfile; the panel is hidden
	//     otherwise, so these getters are never called on a plain CKnownFile) ---
	if (part) {
		CastChild(IDC_PFSTATUS, wxControl)->SetLabel(part->getPartfileStatus());
		bufferS = CFormat("%i (%i)") % part->GetPartCount() % part->GetHashCount();
		CastChild(IDC_PARTCOUNT, wxControl)->SetLabel(bufferS);
		CastChild(IDC_TRANSFERRED, wxControl)->SetLabel(CastItoXBytes(part->GetTransferred()));
		CastChild(IDC_FD_STATS1, wxControl)->SetLabel(CastItoXBytes(part->GetLostDueToCorruption()));
		CastChild(IDC_FD_STATS2, wxControl)->SetLabel(CastItoXBytes(part->GetGainDueToCompression()));
		CastChild(IDC_FD_STATS3, wxControl)
			->SetLabel(CastItoIShort(part->TotalPacketsSavedDueToICH()));
		CastChild(IDC_COMPLSIZE, wxControl)->SetLabel(CastItoXBytes(part->GetCompletedSize()));
		bufferS = CFormat(_("%.1f%% done")) % part->GetPercentCompleted();
		CastChild(IDC_PROCCOMPL, wxControl)->SetLabel(bufferS);
		bufferS = CFormat(_("%.2f kB/s")) % part->GetKBpsDown();
		CastChild(IDC_DATARATE, wxControl)->SetLabel(bufferS);
		bufferS = CFormat("%i") % part->GetSourceCount();
		CastChild(IDC_SOURCECOUNT, wxControl)->SetLabel(bufferS);
		bufferS = CFormat("%i") % part->GetTransferingSrcCount();
		CastChild(IDC_SOURCECOUNT2, wxControl)->SetLabel(bufferS);
		bufferS = CFormat("%i (%.1f%%)") % part->GetAvailablePartCount() %
			  ((part->GetAvailablePartCount() * 100.0) / part->GetPartCount());
		CastChild(IDC_PARTAVAILABLE, wxControl)->SetLabel(bufferS);
		bufferS = CastSecondsToHM(part->GetDlActiveTime());
		CastChild(IDC_DLACTIVETIME, wxControl)->SetLabel(bufferS);

		if (part->lastseencomplete == 0) {
			bufferS = wxString(_("Unknown")).MakeLower();
		} else {
			wxDateTime last_seen(part->lastseencomplete);
			bufferS = last_seen.FormatISODate() + " " + last_seen.FormatISOTime();
		}
		CastChild(IDC_LASTSEENCOMPL, wxControl)->SetLabel(bufferS);
	}

	// --- Sharing section (present on every CKnownFile; the counters and share
	//     timestamps ride over EC, so this populates in amulegui too) ---
	bufferS =
		CFormat("%u (%u)") % m_file->statistic.GetRequests() % m_file->statistic.GetAllTimeRequests();
	CastChild(IDC_FD_SHARE_REQ, wxControl)->SetLabel(bufferS);
	bufferS = CFormat("%u (%u)") % m_file->statistic.GetAccepts() % m_file->statistic.GetAllTimeAccepts();
	CastChild(IDC_FD_SHARE_ACC, wxControl)->SetLabel(bufferS);
	bufferS = CFormat("%s (%s)") % CastItoXBytes(m_file->statistic.GetTransferred()) %
		  CastItoXBytes(m_file->statistic.GetAllTimeTransferred());
	CastChild(IDC_FD_SHARE_XFER, wxControl)->SetLabel(bufferS);
	double ratio = m_file->GetFileSize() ? (double)m_file->statistic.GetAllTimeTransferred() /
						       (double)m_file->GetFileSize()
					     : 0.0;
	CastChild(IDC_FD_SHARE_RATIO, wxControl)->SetLabel(CFormat("%.2f") % ratio);
	CastChild(IDC_FD_SHARE_COMPLSRC, wxControl)
		->SetLabel(CFormat("%u") % m_file->m_nCompleteSourcesCount);
	CastChild(IDC_FD_SHARE_ONQUEUE, wxControl)->SetLabel(CFormat("%u") % m_file->GetQueuedCount());
	CastChild(IDC_FD_SHARE_UPPRIO, wxControl)
		->SetLabel(PriorityToStr(m_file->GetUpPriority(), m_file->IsAutoUpPriority()));
	bufferS = CFormat(_("%.2f kB/s")) % (m_file->GetUploadDatarate() / 1024.0);
	CastChild(IDC_FD_SHARE_UPSPEED, wxControl)->SetLabel(bufferS);
	CastChild(IDC_FD_SHARE_UPCOUNT, wxControl)
		->SetLabel(CFormat("%u") % m_file->GetTransferringClientCount());
	if (m_file->GetDateShared() == 0) {
		bufferS = wxString(_("Unknown")).MakeLower();
	} else {
		wxDateTime ds(m_file->GetDateShared());
		bufferS = ds.FormatISODate() + " " + ds.FormatISOTime();
	}
	CastChild(IDC_FD_SHARE_SINCE, wxControl)->SetLabel(bufferS);
	if (m_file->GetLastUpload() == 0) {
		bufferS = wxString(_("Unknown")).MakeLower();
	} else {
		wxDateTime lu(m_file->GetLastUpload());
		bufferS = lu.FormatISODate() + " " + lu.FormatISOTime();
	}
	CastChild(IDC_FD_SHARE_LASTUP, wxControl)->SetLabel(bufferS);

	// Media Info (issue #418): populate from FT_MEDIA_* when the file has
	// probed metadata; the labels stay at their "N/A" default otherwise.
	// Works identically in the monolithic and remote (amulegui) builds —
	// the remote proxy stores the same FT_MEDIA_* tags off EC.
	if (m_file->GetMetaDataVer() != 0) {
		CastChild(IDC_FD_MEDIA_LENGTH, wxControl)
			->SetLabel(CastSecondsToHM(m_file->GetIntTagValue(FT_MEDIA_LENGTH)));
		CastChild(IDC_FD_MEDIA_BITRATE, wxControl)
			->SetLabel(CFormat(wxT("%u kbps")) % m_file->GetIntTagValue(FT_MEDIA_BITRATE));
		CastChild(IDC_FD_MEDIA_CODEC, wxControl)
			->SetLabel(FormatMediaCodec(m_file->GetStrTagValue(FT_MEDIA_CODEC)));
		CastChild(IDC_FD_MEDIA_ARTIST, wxControl)->SetLabel(m_file->GetStrTagValue(FT_MEDIA_ARTIST));
		CastChild(IDC_FD_MEDIA_ALBUM, wxControl)->SetLabel(m_file->GetStrTagValue(FT_MEDIA_ALBUM));
		CastChild(IDC_FD_MEDIA_TITLE, wxControl)->SetLabel(m_file->GetStrTagValue(FT_MEDIA_TITLE));
	}

	// --- Section visibility, driven by the file's own state (not by which list
	//     opened the dialog). Download rows show only for an in-progress
	//     partfile; sharing rows show for any file that actually shares data
	//     (every completed file, or a partfile with at least one complete part). ---
	bool showDownload = (part != nullptr);
	bool showSharing = (part == nullptr) || (part->GetCompletedSize() > 0);
	bool relayout = false;
	wxWindow *dlPanel = FindWindow(IDC_FD_DOWNLOAD_PANEL);
	if (dlPanel && dlPanel->IsShown() != showDownload) {
		dlPanel->Show(showDownload);
		relayout = true;
	}
	wxWindow *shPanel = FindWindow(IDC_FD_SHARING_PANEL);
	if (shPanel && shPanel->IsShown() != showSharing) {
		shPanel->Show(showSharing);
		relayout = true;
	}
	if (relayout && GetSizer()) {
		GetSizer()->Layout();
		Fit();
	}

	setEnableForApplyButton();
	// "Show all comments" lists source comments (a download concept) and the
	// dialog it opens takes a CPartFile — so enable it only for an in-progress
	// partfile, when there are comments or Kad can still be queried (#434).
	FileRatingList list;
	if (part) {
		part->GetRatingAndComments(list);
	}
	CastChild(IDC_CMTBT, wxControl)->Enable(part && (!list.empty() || theApp->IsConnectedKad()));
	FillSourcenameList();
	Layout();
}

// CFileDetailDialog message handlers

void CFileDetailDialog::FillSourcenameList()
{
	CFileDetailListCtrl *pmyListCtrl;
	int itempos;
	int inserted = 0;
	pmyListCtrl = CastChild(IDC_LISTCTRLFILENAMES, CFileDetailListCtrl);

	// The source-name list is a download-only view (how sources name the file).
	// A plain shared file has none, so clear any rows a prior file left behind
	// (Next/Prev) — freeing their item data — and bail before the partfile-only
	// source accessors below.
	CPartFile *part = m_file->IsPartFile() ? static_cast<CPartFile *>(m_file) : nullptr;
	if (!part) {
		for (int i = 0; i < pmyListCtrl->GetItemCount(); ++i) {
			delete reinterpret_cast<SourcenameItem *>(pmyListCtrl->GetItemData(i));
		}
		pmyListCtrl->DeleteAllItems();
		return;
	}

	// reset
	for (int i = 0; i < pmyListCtrl->GetItemCount(); i++) {
		SourcenameItem *item = reinterpret_cast<SourcenameItem *>(pmyListCtrl->GetItemData(i));
		item->count = 0;
	}

	// update
#ifdef CLIENT_GUI
	const SourcenameItemMap &sources = part->GetSourcenameItemMap();
	for (SourcenameItemMap::const_iterator it = sources.begin(); it != sources.end(); ++it) {
		const SourcenameItem &cur_src = it->second;
		itempos = pmyListCtrl->FindItem(-1, cur_src.name);
		if (itempos == -1) {
			int itemid = pmyListCtrl->InsertItem(0, cur_src.name);
			SourcenameItem *item = new SourcenameItem(cur_src.name, cur_src.count);
			pmyListCtrl->SetItemPtrData(0, reinterpret_cast<wxUIntPtr>(item));
			// background.. argh -- PA: was in old version - do we still need this?
			wxListItem tmpitem;
			tmpitem.m_itemId = itemid;
			tmpitem.SetBackgroundColour(CMuleColour(wxSYS_COLOUR_LISTBOX));
			pmyListCtrl->SetItem(tmpitem);
			inserted++;
		} else {
			SourcenameItem *item =
				reinterpret_cast<SourcenameItem *>(pmyListCtrl->GetItemData(itempos));
			item->count = cur_src.count;
		}
	}
#else  // CLIENT_GUI
	const CKnownFile::SourceSet &sources = part->GetSourceList();
	CKnownFile::SourceSet::const_iterator it = sources.begin();
	for (; it != sources.end(); ++it) {
		const CClientRef &cur_src = *it;
		if (cur_src.GetRequestFile() != part || cur_src.GetClientFilename().Length() == 0) {
			continue;
		}

		itempos = pmyListCtrl->FindItem(-1, cur_src.GetClientFilename());
		if (itempos == -1) {
			int itemid = pmyListCtrl->InsertItem(0, cur_src.GetClientFilename());
			SourcenameItem *item = new SourcenameItem(cur_src.GetClientFilename(), 1);
			pmyListCtrl->SetItemPtrData(0, reinterpret_cast<wxUIntPtr>(item));
			// background.. argh -- PA: was in old version - do we still need this?
			wxListItem tmpitem;
			tmpitem.m_itemId = itemid;
			tmpitem.SetBackgroundColour(CMuleColour(wxSYS_COLOUR_LISTBOX));
			pmyListCtrl->SetItem(tmpitem);
			inserted++;
		} else {
			SourcenameItem *item =
				reinterpret_cast<SourcenameItem *>(pmyListCtrl->GetItemData(itempos));
			item->count++;
		}
	}
#endif // CLIENT_GUI

	// remove 0'er and update counts
	for (int i = 0; i < pmyListCtrl->GetItemCount(); ++i) {
		SourcenameItem *item = reinterpret_cast<SourcenameItem *>(pmyListCtrl->GetItemData(i));
		if (item->count == 0) {
			delete item;
			pmyListCtrl->DeleteItem(i);
			i--; // PA: one step back is enough, no need to go back to 0
		} else {
			pmyListCtrl->SetItem(i, 1, CFormat("%i") % item->count);
		}
	}

	if (inserted) {
		pmyListCtrl->SortList();
	}
	// no need to call Layout() here, it's called in UpdateData()
}

void CFileDetailDialog::OnBnClickedShowComment(wxCommandEvent &WXUNUSED(evt))
{
	// The source-comment list dialog is partfile-scoped; the button is only
	// enabled for an in-progress partfile (see UpdateData), but guard anyway.
	if (m_file->IsPartFile()) {
		CCommentDialogLst(this, static_cast<CPartFile *>(m_file)).ShowModal();
	}
}

void CFileDetailDialog::resetValueForFilenameTextEdit()
{
	CastChild(IDC_FILENAME, wxTextCtrl)->SetValue(m_file->GetFileName().GetPrintable());
	m_filenameChanged = false;
	setEnableForApplyButton();
}

void CFileDetailDialog::setValueForFilenameTextEdit(const wxString &s)
{
	CastChild(IDC_FILENAME, wxTextCtrl)->SetValue(s);
	m_filenameChanged = true;
	setEnableForApplyButton();
}

void CFileDetailDialog::setEnableForApplyButton()
{
	bool enabled = m_file->GetStatus() != PS_COMPLETE && m_file->GetStatus() != PS_COMPLETING &&
		       m_filenameChanged;
	CastChild(IDC_APPLY, wxControl)->Enable(enabled);
	// Make OK button default so Text can be applied by hitting return
	CastChild(enabled ? IDC_APPLY_AND_CLOSE : ID_CLOSEWNDFD, wxButton)->SetDefault();
}

void CFileDetailDialog::OnTextFileNameChange(wxCommandEvent &WXUNUSED(evt))
{
	m_filenameChanged = true;
	setEnableForApplyButton();
}

void CFileDetailDialog::OnBnClickedOk(wxCommandEvent &evt)
{
	OnBnClickedApply(evt);
	OnClosewnd(evt);
}

void CFileDetailDialog::OnBnClickedApply(wxCommandEvent &WXUNUSED(evt))
{
	CPath fileName = CPath(CastChild(IDC_FILENAME, wxTextCtrl)->GetValue());

	if (fileName.IsOk() && (fileName != m_file->GetFileName())) {
		if (theApp->sharedfiles->RenameFile(m_file, fileName)) {
			FindWindow(IDC_FNAME)->SetLabel(
				MakeStringEscaped(m_file->GetFileName().GetPrintable()));
			CPath metPath = m_file->IsPartFile()
						? static_cast<CPartFile *>(m_file)->GetFullName()
						: m_file->GetFilePath().JoinPaths(m_file->GetFileName());
			FindWindow(IDC_METFILE)->SetLabel(metPath.GetPrintable());

			resetValueForFilenameTextEdit();

			Layout();
		}
	}
}

void CFileDetailDialog::OnBnClickedPrevFile(wxCommandEvent &)
{
	if (--m_index < 0) {
		m_index = m_files.size() - 1;
	}
	m_file = m_files[m_index];
	UpdateData(true);
}

void CFileDetailDialog::OnBnClickedNextFile(wxCommandEvent &)
{
	if (++m_index == (int)m_files.size()) {
		m_index = 0;
	}
	m_file = m_files[m_index];
	UpdateData(true);
}

static bool IsDigit(const wxChar ch)
{
	switch (ch) {
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		return true;
	}
	return false;
}

static bool IsWordSeparator(const wxChar ch)
{
	switch (ch) {
	case '.':
	case ',':
	case '(':
	case ')':
	case '[':
	case ']':
	case '{':
	case '}':
	case '-':
	case '"':
	case ' ':
		return true;
	}
	return false;
}

static void ReplaceWord(
	wxString &str, const wxString &replaceFrom, const wxString &replaceTo, bool numbers = false)
{
	unsigned int i = 0;
	unsigned int l = replaceFrom.Length();
	while (i < str.Length()) {
		if (str.Mid(i, l) == replaceFrom) {
			if ((i == 0 || IsWordSeparator(str.GetChar(i - 1))) &&
				((i == str.Length() - l || IsWordSeparator(str.GetChar(i + l))) ||
					(numbers && IsDigit(str.GetChar(i + l))))) {
				str.replace(i, l, replaceTo);
			}
			i += replaceTo.Length() - 1;
		}
		i++;
	}
}

void CFileDetailDialog::OnBnClickedButtonStrip(wxCommandEvent &WXUNUSED(evt))
{
	wxString filename;
	filename = CastChild(IDC_FILENAME, wxTextCtrl)->GetValue();

	int extpos = filename.Find('.', true);
	wxString ext;
	if (extpos > 0) {
		// get the extension - we do not modify it except make it lowercase
		ext = filename.Mid(extpos);
		ext.MakeLower();
		// get rid of extension and replace . with space
		filename.Truncate(extpos);
		filename.Replace(".", " ");
	}

	// Replace Space-holders with Spaces
	filename.Replace("_", " ");
	filename.Replace("%20", " ");

	// Some additional formatting
	filename.Replace("hYPNOTiC", "");
	filename.MakeLower();
	filename.Replace("xxx", "XXX");
	//	filename.Replace("xdmnx", "");
	//	filename.Replace("pmp", "");
	//	filename.Replace("dws", "");
	filename.Replace("www pornreactor com", "");
	filename.Replace("sharereactor", "");
	filename.Replace("found via www filedonkey com", "");
	filename.Replace("deviance", "");
	filename.Replace("adunanza", "");
	filename.Replace("-ftv", "");
	filename.Replace("flt", "");
	filename.Replace("[]", "");
	filename.Replace("()", "");

	// Change CD, CD#, VCD{,#}, DVD{,#}, ISO, PC to uppercase
	ReplaceWord(filename, "cd", "CD", true);
	ReplaceWord(filename, "vcd", "VCD", true);
	ReplaceWord(filename, "dvd", "DVD", true);
	ReplaceWord(filename, "iso", "ISO", false);
	ReplaceWord(filename, "pc", "PC", false);

	// Make leading Caps
	// and delete 1+ spaces
	if (filename.Length() > 1) {
		bool last_char_space = true;
		bool last_char_wordseparator = true;
		unsigned int i = 0;

		do {
			wxChar c = filename.GetChar(i);
			if (c == ' ') {
				if (last_char_space) {
					filename.Remove(i, 1);
					i--;
				} else {
					last_char_space = true;
				}
			} else if (c == '.') {
				if (last_char_space && i > 0) {
					i--;
					filename.Remove(i, 1);
				}
				last_char_space = false;
			} else {
				if (last_char_wordseparator) {
					wxString tempStr(c);
					tempStr.MakeUpper();
					filename.SetChar(i, tempStr.GetChar(0));
					last_char_space = false;
				}
			}
			last_char_wordseparator = IsWordSeparator(c);
			i++;
		} while (i < filename.Length());

		if (last_char_space && i > 0) {
			filename.Remove(i - 1, 1);
		}
	}

	// should stay lowercase
	ReplaceWord(filename, "By", "by");

	// re-add extension
	filename += ext;

	setValueForFilenameTextEdit(filename);
}

void CFileDetailDialog::OnBnClickedTakeOver(wxCommandEvent &WXUNUSED(evt))
{
	CFileDetailListCtrl *pmyListCtrl;
	pmyListCtrl = CastChild(IDC_LISTCTRLFILENAMES, CFileDetailListCtrl);
	if (pmyListCtrl->GetSelectedItemCount() > 0) {
		// get first selected item (there is only one)
		long pos = pmyListCtrl->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
		if (pos != -1) { // shouldn't happen, we checked if something is selected
			setValueForFilenameTextEdit(pmyListCtrl->GetItemText(pos));
		}
	}
}

void CFileDetailDialog::OnListClickedTakeOver(wxListEvent &WXUNUSED(evt))
{
	wxCommandEvent ev;
	OnBnClickedTakeOver(ev);
}
// File_checked_for_headers
