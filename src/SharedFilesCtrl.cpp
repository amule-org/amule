//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "SharedFilesCtrl.h" // Interface declarations

#include <wx/file.h>    // Needed for wxFile
#include <wx/filedlg.h> // Needed for wxFileDialog

#include <common/MenuIDs.h>
#include <common/StringFunctions.h> // Needed for unicode2UTF8

#include "muuli_wdr.h"        // Needed for ID_SHFILELIST
#include "SharedFilesWnd.h"   // Needed for CSharedFilesWnd
#include "amuleDlg.h"         // Needed for CamuleDlg
#include "CommentDialog.h"    // Needed for CCommentDialog
#include "FileDetailDialog.h" // Needed for CFileDetailDialog
#include "PartFile.h"         // Needed for CPartFile
#include "FileLaunch.h"       // Needed for FileLaunch::Open / Reveal
#include "SharedFileList.h"   // Needed for CKnownFileMap
#include "amule.h"            // Needed for theApp
#include "ServerConnect.h"    // Needed for CServerConnect
#include "Preferences.h"      // Needed for thePrefs
#include "MuleBarRenderer.h"  // Needed for CBarFillSpec, CBarFillSpan
#include "DataToText.h"       // Needed for PriorityToStr
#include "Statistics.h"       // Needed for theStats (incoming free space)
#include "GuiEvents.h"        // Needed for CoreNotify_*
#include "DownloadQueue.h"    // Needed for CDownloadQueue
#include "TransferWnd.h"      // Needed for CTransferWnd
#include "Logger.h"           // Needed for AddLogLine
#include "OtherFunctions.h"   // Needed for FormatLocalDateTime, IsMediaProbeCandidate

namespace
{

// Why a shared file can or cannot be re-probed. One classifier rather than the
// same two conditions written at both call sites: the menu asks whether to
// enable the entry, the handler asks how many of a selection it is leaving out
// and why, and those answers must not be able to disagree.
enum class MediaRefreshEligibility
{
	Eligible,
	FeatureDisabled, //!< media metadata extraction is switched off in preferences
	Incomplete,      //!< an in-progress download has no complete file to read
	NotMedia,        //!< nothing for ffprobe to extract
};

MediaRefreshEligibility ClassifyForMediaRefresh(const CKnownFile *file)
{
	// First, because it decides the answer for every file at once: with the
	// feature off the scheduler drops the job before looking at the file, so
	// the entry would be enabled and do nothing at all -- which is exactly
	// what happens today and gives the user no clue why.
	//
	// Correct in amulegui too. The daemon sends this preference over EC as a
	// presence tag, and the receiving side maps an absent tag to false
	// (ApplyBoolean in ECSpecialMuleTags), so a remote GUI reports the
	// DAEMON's setting rather than its own default.
	if (!thePrefs::GetMediaMetadataEnabled()) {
		return MediaRefreshEligibility::FeatureDisabled;
	}
	// IsPartFile() is answered correctly in both binaries: amulegui files a
	// shared partfile into its shared list as the very CPartFile the download
	// queue holds, not a plain CKnownFile.
	if (file->IsPartFile()) {
		return MediaRefreshEligibility::Incomplete;
	}
	// The scheduler's own test, through the shared predicate in
	// OtherFunctions, so the view cannot offer what the core will drop.
	if (!IsMediaProbeCandidate(file->GetFileName())) {
		return MediaRefreshEligibility::NotMedia;
	}
	return MediaRefreshEligibility::Eligible;
}

} // namespace

wxBEGIN_EVENT_TABLE(CSharedFilesCtrl, CMuleVirtualDataViewCtrl)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CSharedFilesCtrl::OnItemRightClicked)
	EVT_MENU(MP_VIEW, CSharedFilesCtrl::OnOpenFile)
	EVT_MENU(MP_SHOWINFOLDER, CSharedFilesCtrl::OnShowInFolder)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CSharedFilesCtrl::OnItemActivated)

	EVT_MENU(MP_METINFO, CSharedFilesCtrl::OnViewFileDetails)

	EVT_MENU(MP_PRIOVERYLOW, CSharedFilesCtrl::OnSetPriority)
	EVT_MENU(MP_PRIOLOW, CSharedFilesCtrl::OnSetPriority)
	EVT_MENU(MP_PRIONORMAL, CSharedFilesCtrl::OnSetPriority)
	EVT_MENU(MP_PRIOHIGH, CSharedFilesCtrl::OnSetPriority)
	EVT_MENU(MP_PRIOVERYHIGH, CSharedFilesCtrl::OnSetPriority)
	EVT_MENU(MP_POWERSHARE, CSharedFilesCtrl::OnSetPriority)
	EVT_MENU(MP_PRIOAUTO, CSharedFilesCtrl::OnSetPriorityAuto)

	EVT_MENU(MP_CMT, CSharedFilesCtrl::OnEditComment)
	EVT_MENU(MP_ADDCOLLECTION, CSharedFilesCtrl::OnAddCollection)
	EVT_MENU(MP_EXPORTCOLLECTION, CSharedFilesCtrl::OnExportCollection)
	EVT_MENU(MP_GETMAGNETLINK, CSharedFilesCtrl::OnCreateURI)
	EVT_MENU(MP_GETED2KLINK, CSharedFilesCtrl::OnCreateURI)
	EVT_MENU(MP_GETSOURCEED2KLINK, CSharedFilesCtrl::OnCreateURI)
	EVT_MENU(MP_GETCRYPTSOURCEDED2KLINK, CSharedFilesCtrl::OnCreateURI)
	EVT_MENU(MP_GETHOSTNAMESOURCEED2KLINK, CSharedFilesCtrl::OnCreateURI)
	EVT_MENU(MP_GETHOSTNAMECRYPTSOURCEED2KLINK, CSharedFilesCtrl::OnCreateURI)
	EVT_MENU(MP_GETAICHED2KLINK, CSharedFilesCtrl::OnCreateURI)
	EVT_MENU(MP_GETAICHED2KLINKSRC, CSharedFilesCtrl::OnCreateURI)
	EVT_MENU(MP_RENAME, CSharedFilesCtrl::OnRename)
	EVT_MENU(MP_REFRESHMEDIAMETA, CSharedFilesCtrl::OnRefreshMediaMetadata)
	EVT_MENU(MP_WS, CSharedFilesCtrl::OnGetFeedback)
	EVT_MENU(MP_VERIFY, CSharedFilesCtrl::OnVerifyLocalData)
wxEND_EVENT_TABLE()

CSharedFilesCtrl::CSharedFilesCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags)
: CMuleVirtualDataViewCtrl(parent, id, pos, size, flags)
, m_inBulkUpdate(false)
, m_batchUpdate(false)
, m_batchFrozen(false)
, m_hashingRemaining(0)
, m_batchRowsAppended(false)
, m_shownSize(0)
{
	m_menu = nullptr;

	// File Name carries the rating/comment smiley through CMuleIconTextRenderer,
	// which reserves the icon slot only on the rows that have one -- wx's own
	// icon+text renderer indents every row without a smiley, which is why this
	// briefly lived in a column of its own. Obtained Parts is the availability
	// bar; the rest are plain text.
	const int colFlags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
	AddIconTextColumn(_("File Name"), COLUMN_SHARED_NAME, "N", 400, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Size"), COLUMN_SHARED_SIZE, "Z", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Type"), COLUMN_SHARED_TYPE, "Y", 90, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Priority"), COLUMN_SHARED_PRIO, "p", 70, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Requests"), COLUMN_SHARED_REQ, "Q", 80, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Accepted Requests"), COLUMN_SHARED_AREQ, "A", 80, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Transferred Data"), COLUMN_SHARED_TRA, "T", 120, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Share Ratio"), COLUMN_SHARED_RTIO, "R", 80, wxALIGN_LEFT, colFlags);
	AddBarColumn(_("Obtained Parts"), COLUMN_SHARED_PART, "P", 120, colFlags);
	AddTextColumn(_("Complete Sources"), COLUMN_SHARED_CMPL, "C", 120, wxALIGN_LEFT, colFlags);
	// FileID (== file hash) was dropped — the hash is on the details modal. The
	// three new columns reuse existing translations ("Speed", "Shared since",
	// "Last upload"); Directory Path stays but moves to the end.
	AddTextColumn(_("Speed"), COLUMN_SHARED_SPEED, "U", 90, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Shared since"), COLUMN_SHARED_SINCE, "H", 130, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Last upload"), COLUMN_SHARED_LASTUP, "L", 130, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Directory Path"), COLUMN_SHARED_PATH, "D", 430, wxALIGN_LEFT, colFlags);

	AppendSpacerColumn(COLUMN_SHARED_SPACER);

	AssociateVirtualModel();

	// Default sort is by name, ascending; LoadColumnSettings() replaces it
	// when the config has something saved.
	ApplySorting(COLUMN_SHARED_NAME, 0);

	m_columnStore.SetTableName("Shared");
	LoadColumnSettings();
	InitColumnState();
}

wxString CSharedFilesCtrl::GetOldColumnOrder() const
{
	return "N,Z,Y,p,Q,A,T,R,P,C,U,H,L,D";
}

CSharedFilesCtrl::~CSharedFilesCtrl() {}

void CSharedFilesCtrl::OnItemRightClicked(wxDataViewEvent &event)
{
	// Right-clicking a row outside the selection acts on that row alone.
	if (event.GetItem().IsOk()) {
		wxDataViewItemArray selection;
		GetSelections(selection);
		if (selection.Index(event.GetItem()) == wxNOT_FOUND) {
			UnselectAll();
			Select(event.GetItem());
		}
	}

	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (selected.empty()) {
		return;
	}
	// Bound re-checked by the handlers that use it (OnOpenFile,
	// OnShowInFolder): PopupMenu runs a nested event loop, so the shared-dir
	// watcher can mutate the list while the menu is open.
	m_menuItem = selected.front();

	if (m_menu == nullptr) {
		m_menu = new wxMenu(_("Shared Files"));
		wxMenu *prioMenu = new wxMenu();
		prioMenu->AppendCheckItem(MP_PRIOVERYLOW, _("Very low"));
		prioMenu->AppendCheckItem(MP_PRIOLOW, _("Low"));
		prioMenu->AppendCheckItem(MP_PRIONORMAL, _("Normal"));
		prioMenu->AppendCheckItem(MP_PRIOHIGH, _("High"));
		prioMenu->AppendCheckItem(MP_PRIOVERYHIGH, _("Very High"));
		prioMenu->AppendCheckItem(MP_POWERSHARE, _("Release"));
		prioMenu->AppendCheckItem(MP_PRIOAUTO, _("Auto"));

		m_menu->Append(0, _("Priority"), prioMenu);
		m_menu->AppendSeparator();

		m_menu->Append(MP_VIEW, _("&Open the file"));
		m_menu->Append(MP_SHOWINFOLDER, _("Show in file manager"));
		m_menu->AppendSeparator();
		m_menu->Append(MP_METINFO, _("Show file &details"));
		m_menu->AppendSeparator();

		CKnownFile *file = reinterpret_cast<CKnownFile *>(m_menuItem);
		if (file->GetFileComment().IsEmpty() && !file->GetFileRating()) {
			m_menu->Append(MP_CMT, _("Add Comment/Rating"));
		} else {
			m_menu->Append(MP_CMT, _("Edit Comment/Rating"));
		}

		m_menu->AppendSeparator();
		m_menu->Append(MP_RENAME, _("Rename"));
		m_menu->AppendSeparator();

		m_menu->Append(MP_VERIFY, _("Verify Local Data"));
		m_menu->Append(MP_REFRESHMEDIAMETA, _("Re-extract &media metadata"));
		m_menu->AppendSeparator();

		const bool isCollection = file->GetFileName().GetExt() == "emulecollection";
		if (isCollection) {
			m_menu->Append(MP_ADDCOLLECTION, _("Add files in collection to transfer list"));
			m_menu->AppendSeparator();
		}
		m_menu->Append(MP_GETMAGNETLINK, _("Copy magnet &URI to clipboard"));
		m_menu->Append(MP_GETED2KLINK, _("Copy eD2k &link to clipboard"));
		m_menu->Append(MP_GETSOURCEED2KLINK, _("Copy eD2k link to clipboard (&Source)"));
		m_menu->Append(MP_GETCRYPTSOURCEDED2KLINK,
			_("Copy eD2k link to clipboard (Source) (&With Crypt options)"));
		m_menu->Append(MP_GETHOSTNAMESOURCEED2KLINK, _("Copy eD2k link to clipboard (&Hostname)"));
		m_menu->Append(MP_GETHOSTNAMECRYPTSOURCEED2KLINK,
			_("Copy eD2k link to clipboard (Hostname) (With &Crypt options)"));
		m_menu->Append(MP_GETAICHED2KLINK, _("Copy eD2k link to clipboard (&AICH info)"));
		m_menu->Append(MP_GETAICHED2KLINKSRC, _("Copy eD2k link to clipboard (&AICH info + Source)"));
		m_menu->Append(MP_WS, _("Copy feedback to clipboard"));
		m_menu->AppendSeparator();
		m_menu->Append(MP_EXPORTCOLLECTION, _("Export selected files to an emulecollection"));

		// Offered only when the file is reachable from this host: the shared
		// list carries the daemon's directory in amulegui, which resolves here
		// only on a shared filesystem, and a locally shared file can have been
		// moved or deleted since it was hashed.
		// CSharedFileList shares PS_READY part files, so an entry here can be an
		// in-progress download. Gate it exactly as the Downloads list does:
		// a finished file of any type can be opened, an unfinished one only when
		// enough of the media is on disk to play.
		// IsPartFile() establishes the dynamic type, as in FileLaunch::ResolvePath.
		const bool previewable =
			file->IsPartFile() ? static_cast<CPartFile *>(file)->PreviewAvailable() : true;
		m_menu->SetLabel(
			MP_VIEW, file->IsPartFile() ? wxString(_("Preview")) : wxString(_("&Open the file")));
		// One filesystem check for both entries; see FileLaunch::GetAvailability.
		bool canOpen = false;
		bool canReveal = false;
		FileLaunch::GetAvailability(file, canOpen, canReveal);
		m_menu->Enable(MP_VIEW, previewable && canOpen);
		m_menu->Enable(MP_SHOWINFOLDER, canReveal);
		if (isCollection) {
			// Reading a collection means reading its bytes on this host, so it
			// needs the same reachability as opening the file. A part file is
			// excluded on top of that: it is a truncated collection, not a
			// usable one.
			m_menu->Enable(MP_ADDCOLLECTION, canOpen && !file->IsPartFile());
		}
		m_menu->Enable(MP_GETAICHED2KLINK, file->HasProperAICHHashSet());
		m_menu->Enable(MP_GETAICHED2KLINKSRC, file->HasProperAICHHashSet());
		m_menu->Enable(MP_GETHOSTNAMESOURCEED2KLINK, !thePrefs::GetYourHostname().IsEmpty());
		m_menu->Enable(MP_GETHOSTNAMECRYPTSOURCEED2KLINK, !thePrefs::GetYourHostname().IsEmpty());
		// Verifying a partfile is not supported: the hashset covers the
		// finished file, and CVerifyLocalDataTask bails on one anyway. Greyed
		// out rather than silently refused, so the state is visible before the
		// click instead of only in the log afterwards (amule-org/amule#1039).
		// IsPartFile() is answered correctly in both binaries: amulegui files a
		// shared partfile into its shared list as the very CPartFile the
		// download queue holds (amule-remote-gui.cpp), not a plain CKnownFile.
		m_menu->Enable(MP_VERIFY, !file->IsPartFile());
		// Same two conditions the scheduler applies, asked through the shared
		// predicate rather than a second copy of the rule: an in-progress
		// download has no complete file for ffprobe to read, and a file that
		// is not audio or video has nothing to extract. Greyed out rather
		// than accepted and silently dropped, so the state is visible before
		// the click (issue #1079).
		//
		// Built from the right-clicked row while the handler acts on the whole
		// selection, exactly as Verify Local Data does -- a mixed selection is
		// filtered there, and the dialog says what it left out.
		// From the selection, not from `file`: the handler refreshes every
		// eligible file in the selection and the confirmation accounts for the
		// rest, so the entry is available whenever there is anything to do.
		m_menu->Enable(MP_REFRESHMEDIAMETA, !PartitionForMediaRefresh().eligible.empty());

		int priority = file->IsAutoUpPriority() ? PR_AUTO : file->GetUpPriority();

		prioMenu->Check(MP_PRIOVERYLOW, priority == PR_VERY_LOW);
		prioMenu->Check(MP_PRIOLOW, priority == PR_LOW);
		prioMenu->Check(MP_PRIONORMAL, priority == PR_NORMAL);
		prioMenu->Check(MP_PRIOHIGH, priority == PR_HIGH);
		prioMenu->Check(MP_PRIOVERYHIGH, priority == PR_VERYHIGH);
		prioMenu->Check(MP_POWERSHARE, priority == PR_POWERSHARE);
		prioMenu->Check(MP_PRIOAUTO, priority == PR_AUTO);

		PopupMenu(m_menu, event.GetPosition());

		delete m_menu;

		m_menu = nullptr;
	}
}

void CSharedFilesCtrl::ShowFileDetailDialog(long focused)
{
	if (focused < 0) {
		return;
	}
	// Pass every listed file so the dialog's Next/Prev can walk the shared
	// list, anchored on the clicked row. Reuses the same CFileDetailDialog as
	// the downloads list; per-file state decides which sections it shows.
	std::vector<CKnownFile *> files;
	const long nrItems = ItemDataCount();
	files.reserve(nrItems);
	int index = 0;
	for (long i = 0; i < nrItems; i++) {
		if (i == focused) {
			index = static_cast<int>(files.size());
		}
		files.push_back(FileAtRow(i));
	}
	CFileDetailDialog(this, files, index).ShowModal();
}

void CSharedFilesCtrl::OnViewFileDetails(wxCommandEvent &WXUNUSED(event))
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (!selected.empty()) {
		ShowFileDetailDialog(RowOfData(selected.front()));
	}
}

void CSharedFilesCtrl::OnItemActivated(wxDataViewEvent &event)
{
	if (!event.GetItem().IsOk()) {
		return;
	}
	// Read the row straight off the event, as CDownloadListCtrl::
	// OnItemActivated does -- touching the selection here would discard
	// whatever multi-selection the user already had for no reason.
	const long row = GetModelRow(event.GetItem());
	CKnownFile *file = FileAtRow(row);
	if (!file) {
		return;
	}

	// A finished file of any type can be opened, an unfinished one
	// (CSharedFileList also carries PS_READY part files) only once enough of
	// the media is on disk to play. Same rule as CDownloadListCtrl::
	// OnItemActivated, which lists these very files while they are still
	// downloading -- see the note there. Anything else opens the file-details
	// modal, which is what every double-click in this list did before.
	const bool launchable = !file->IsPartFile() || static_cast<CPartFile *>(file)->PreviewAvailable();
	if (launchable && FileLaunch::CanOpen(file)) {
		FileLaunch::Open(file, this);
	} else {
		ShowFileDetailDialog(row);
	}
}

void CSharedFilesCtrl::OnVerifyLocalData(wxCommandEvent &WXUNUSED(event))
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		CKnownFile *file = reinterpret_cast<CKnownFile *>(data);
		// Still reachable with the menu item greyed out for partfiles: the
		// menu is built from the row that was right-clicked, while this acts
		// on the whole selection. Right-click a completed file with a
		// partfile also selected and the entry is enabled, but the partfile
		// still has to be turned away here.
		if (file->IsPartFile()) {
			AddLogLineN(
				CFormat(_("Verify Local Data on PartFile is currently not supported: %s")) %
				file->GetFileName());
		} else {
			theApp->sharedfiles->VerifyLocalData(file);
		}
	}
}

CSharedFilesCtrl::MediaRefreshSelection CSharedFilesCtrl::PartitionForMediaRefresh() const
{
	// The action applies to the SELECTION, so the menu has to be enabled from
	// the selection too. Judging it by the right-clicked row instead meant a
	// mixed selection greyed the entry out whenever the row under the cursor
	// happened to be the ineligible one -- select a .mp3 and a .zip,
	// right-click the .zip, and an action that would have refreshed the .mp3
	// was unavailable. Which row the cursor is over is not something the user
	// is choosing with; the selection is.
	MediaRefreshSelection out;
	for (wxUIntPtr data : GetSelectedItemData()) {
		CKnownFile *file = reinterpret_cast<CKnownFile *>(data);
		switch (ClassifyForMediaRefresh(file)) {
		case MediaRefreshEligibility::FeatureDisabled:
			// Global rather than per-file: with the feature off every file
			// lands here, the eligible list comes back empty, and the entry is
			// greyed for the whole selection.
			break;
		case MediaRefreshEligibility::Incomplete:
			++out.incomplete;
			break;
		case MediaRefreshEligibility::NotMedia:
			++out.notMedia;
			break;
		case MediaRefreshEligibility::Eligible:
			out.eligible.push_back(file->GetFileHash());
			break;
		}
	}
	return out;
}

void CSharedFilesCtrl::OnRefreshMediaMetadata(wxCommandEvent &WXUNUSED(event))
{
	const MediaRefreshSelection sel = PartitionForMediaRefresh();
	const std::vector<CMD4Hash> &eligible = sel.eligible;
	const unsigned incomplete = sel.incomplete;
	const unsigned notMedia = sel.notMedia;
	if (eligible.empty()) {
		return;
	}

	// Say what is being left out whether or not a dialog follows. Select one
	// .mp3 and ten .zip files and the dialog never appears, so without this the
	// ten are passed over in silence -- and "I selected eleven files and it
	// only did one" is precisely the kind of thing the log has to answer.
	// Verify Local Data, the model for this handler, reports every file it
	// refuses regardless of what else succeeded.
	if (incomplete > 0) {
		AddLogLineN(CFormat(wxPLURAL("Media metadata: skipping %u incomplete download",
				    "Media metadata: skipping %u incomplete downloads",
				    incomplete)) %
			    incomplete);
	}
	if (notMedia > 0) {
		AddLogLineN(CFormat(wxPLURAL("Media metadata: skipping %u file that is not audio or video",
				    "Media metadata: skipping %u files that are not audio or video",
				    notMedia)) %
			    notMedia);
	}

	// One file is the "check whether this fixes it" case and queues straight
	// away; a dialog there would cost a click on the common action. More than
	// one is where the warning earns its place -- the cost is roughly 13 ms
	// per file, so a large selection is real background work, and on slow
	// media (network mounts, spun-down disks) considerably more.
	if (eligible.size() > 1) {
		wxString message = CFormat(wxPLURAL("Re-extract media metadata for %u file?",
					   "Re-extract media metadata for %u files?",
					   eligible.size())) %
				   eligible.size();
		if (incomplete > 0) {
			message << wxT("\n\n")
				<< (CFormat(wxPLURAL("%u incomplete download will be skipped.",
					    "%u incomplete downloads will be skipped.",
					    incomplete)) %
					   incomplete);
		}
		if (notMedia > 0) {
			message << (incomplete > 0 ? wxT("\n") : wxT("\n\n"))
				<< (CFormat(wxPLURAL("%u file is not audio or video and will be skipped.",
					    "%u files are not audio or video and will be skipped.",
					    notMedia)) %
					   notMedia);
		}
		message << wxT("\n\n") << _("This runs in the background. Progress is reported in the log.");
		if (wxMessageBox(message, _("Re-extract media metadata"), wxYES_NO | wxICON_QUESTION, this) !=
			wxYES) {
			return;
		}
	}

	// One call for the whole set, not one per file: amulegui turns each into an
	// EC request, and the request fifo stalls the GUI's own polling past about
	// twenty in flight -- "select all, refresh" on a large share would put one
	// packet per shared file into the socket in a tight loop.
	theApp->sharedfiles->RefreshMediaMetadata(eligible);
}

void CSharedFilesCtrl::OnGetFeedback(wxCommandEvent &WXUNUSED(event))
{
	wxString feed;
	for (wxUIntPtr data : GetSelectedItemData()) {
		if (feed.IsEmpty()) {
			feed = CFormat(_("Feedback from: %s (%s)\n\n")) % thePrefs::GetUserNick() %
			       theApp->GetFullMuleVersion();
		} else {
			feed += "\n";
		}
		feed += reinterpret_cast<CKnownFile *>(data)->GetFeedback();
	}

	if (!feed.IsEmpty()) {
		theApp->CopyTextToClipboard(feed);
	}
}

wxString CSharedFilesCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	CKnownFile *file = reinterpret_cast<CKnownFile *>(item);

	switch (column) {
	case COLUMN_SHARED_NAME:
		return file->GetFileName().GetPrintable();

	case COLUMN_SHARED_SIZE:
		return CastItoXBytes(file->GetFileSize());

	case COLUMN_SHARED_TYPE:
		return GetFiletypeByName(file->GetFileName());

	case COLUMN_SHARED_PRIO:
		return PriorityToStr(file->GetUpPriority(), file->IsAutoUpPriority());

	case COLUMN_SHARED_REQ:
		return CFormat("%u (%u)") % file->statistic.GetRequests() %
		       file->statistic.GetAllTimeRequests();

	case COLUMN_SHARED_AREQ:
		return CFormat("%u (%u)") % file->statistic.GetAccepts() %
		       file->statistic.GetAllTimeAccepts();

	case COLUMN_SHARED_TRA:
		return CastItoXBytes(file->statistic.GetTransferred()) + " (" +
		       CastItoXBytes(file->statistic.GetAllTimeTransferred()) + ")";

	case COLUMN_SHARED_RTIO:
		return CFormat("%.2f") % (static_cast<double>(file->statistic.GetAllTimeTransferred()) /
						 static_cast<double>(file->GetFileSize()));

	case COLUMN_SHARED_CMPL:
		if (file->m_nCompleteSourcesCountLo == 0) {
			if (file->m_nCompleteSourcesCountHi) {
				return CFormat("< %u") % file->m_nCompleteSourcesCountHi;
			}
			return "0";
		} else if (file->m_nCompleteSourcesCountLo == file->m_nCompleteSourcesCountHi) {
			return CFormat("%u") % file->m_nCompleteSourcesCountLo;
		} else {
			return CFormat("%u - %u") % file->m_nCompleteSourcesCountLo %
			       file->m_nCompleteSourcesCountHi;
		}

	case COLUMN_SHARED_SPEED:
		// Live upload speed, adaptive KiB/s / MiB/s and blank below
		// 1 KiB/s — same formatting as the peers (client) list. Sorting
		// uses the raw byte/s value (see CompareItemData), not this string.
		if (file->GetUploadDatarate() >= 1024) {
			if (file->GetUploadDatarate() >= 1048576) {
				return CFormat(_("%.1f MiB/s")) % (file->GetUploadDatarate() / 1048576.0);
			}
			return CFormat(_("%.1f KiB/s")) % (file->GetUploadDatarate() / 1024.0);
		}
		return wxEmptyString;

	case COLUMN_SHARED_SINCE:
		if (file->GetDateShared()) {
			wxDateTime ds(file->GetDateShared());
			return FormatLocalDateTime(ds);
		}
		return wxEmptyString;

	case COLUMN_SHARED_LASTUP:
		if (file->GetLastUpload()) {
			wxDateTime lu(file->GetLastUpload());
			return FormatLocalDateTime(lu);
		}
		return wxEmptyString;

	case COLUMN_SHARED_PATH:
		// Status-agnostic: the Temp dir for a partfile, the
		// destination once completed (EC_TAG_KNOWNFILE_PATH in
		// the remote GUI).
		return file->GetFilePath().GetPrintable();

	default:
		return wxEmptyString;
	}
}

bool CSharedFilesCtrl::GetItemIcon(wxUIntPtr item, unsigned column, wxIcon &icon) const
{
	if (column != COLUMN_SHARED_NAME) {
		return false;
	}
	CKnownFile *file = reinterpret_cast<CKnownFile *>(item);
	if (!file->GetFileRating() && file->GetFileComment().Length() == 0) {
		return false;
	}

	int image = Client_CommentOnly_Smiley;
	if (file->GetFileRating()) {
		image = Client_InvalidRating_Smiley + file->GetFileRating() - 1;
	}
	wxASSERT(image >= Client_InvalidRating_Smiley);
	wxASSERT(image <= Client_CommentOnly_Smiley);

	icon = theApp->amuledlg->m_imagelist.GetIcon(image);
	return true;
}

void CSharedFilesCtrl::GetItemBarFill(wxUIntPtr item, unsigned column, CBarFillSpec &out) const
{
	if (column != COLUMN_SHARED_PART) {
		return;
	}
	CKnownFile *file = reinterpret_cast<CKnownFile *>(item);
	if (!file->GetPartCount()) {
		return;
	}

	std::vector<CBarFillSpan> spans;
	const bool bFlat = thePrefs::UseFlatBar();

	if (file->GetHashingProgress() > 0) {
		const CMuleColour crPending(255, 208, 0);
		const CMuleColour crFlatPending(255, 255, 100);
		const CMuleColour crProgress(0, 224, 0);
		const CMuleColour crFlatProgress(0, 150, 0);

		uint64 left = file->GetHashingProgress() * PARTSIZE;
		if (left < file->GetFileSize() - 1) {
			spans.push_back(
				{ left + 1, file->GetFileSize() - 1, bFlat ? crFlatPending : crPending });
		} else {
			left = file->GetFileSize() - 1;
		}
		// The amount already hashed, in green.
		spans.push_back({ 0, left, bFlat ? crFlatProgress : crProgress });
	} else {
		// Reference to the availability list
		const ArrayOfUInts16 &list = file->IsPartFile()
						     ? static_cast<CPartFile *>(file)->m_SrcpartFrequency
						     : file->m_AvailPartFrequency;

		uint64 end = 0;
		for (unsigned int i = 0; i < list.size(); ++i) {
			const uint64 start = PARTSIZE * static_cast<uint64>(i);
			end = PARTSIZE * static_cast<uint64>(i + 1);
			spans.push_back({ start,
				end,
				CMuleColour(list[i] ? 0 : 255,
					list[i] ? ((210 - (22 * (list[i] - 1)) < 0)
								  ? 0
								  : (210 - (22 * (list[i] - 1))))
						: 0,
					list[i] ? 255 : 0) });
		}
		spans.push_back({ end + 1, file->GetFileSize() - 1, CMuleColour(255, 0, 0) });
	}

	out = CBarFillSpec(item, file->GetFileSize(), std::move(spans));
}

bool CSharedFilesCtrl::AltSortAllowed(unsigned column) const
{
	switch (column) {
	case COLUMN_SHARED_REQ:
	case COLUMN_SHARED_AREQ:
	case COLUMN_SHARED_TRA:
		return true;

	default:
		return false;
	}
}

bool CSharedFilesCtrl::IsLiveSortColumn() const
{
	// Columns whose values change while sharing/uploading. Static columns
	// (name, size, type, priority, shared-since, path) never auto-resort.
	if (m_sort_orders.empty()) {
		return false;
	}
	switch (static_cast<int>(m_sort_orders.front().first)) {
	case COLUMN_SHARED_REQ:
	case COLUMN_SHARED_AREQ:
	case COLUMN_SHARED_TRA:
	case COLUMN_SHARED_RTIO:
	case COLUMN_SHARED_CMPL:
	case COLUMN_SHARED_SPEED:
	case COLUMN_SHARED_LASTUP:
		return true;
	default:
		return false;
	}
}

void CSharedFilesCtrl::ShowFileList()
{
	Freeze();

	// The rebuild renumbers every row, so keep selection + focus by item
	// identity -- otherwise editing the filter (which comes through here)
	// drops whatever the user had selected.
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();

	ClearItemData();

	std::vector<CKnownFile *> files;
	theApp->sharedfiles->CopyFileList(files);
	uint64 totalSize = 0;
	for (CKnownFile *file : files) {
		if (MatchesFilter(file->GetFileName().GetPrintable())) {
			AppendItemData(reinterpret_cast<wxUIntPtr>(file));
			totalSize += file->GetFileSize();
		}
	}
	m_shownSize = totalSize;
	FinishBulkLoad();
	SetSelectedItemData(selected);
	ShowFilesCount();

	Thaw();
}

void CSharedFilesCtrl::RebuildFilteredView()
{
	// Filter hook from CMuleVirtualDataViewCtrl: the master share is the
	// model, so the visible set is rebuilt from it in one pass.
	ShowFileList();
}

void CSharedFilesCtrl::BeginBatchUpdate()
{
	// During the batch ShowFile() appends the row (O(1)) instead of doing a
	// sorted AddItemData() (which rebuilds the whole row index per insert --
	// O(n), i.e. O(n^2) for a burst on a large share). Coalesce the repaints
	// (Freeze) and defer the single sort to EndBatchUpdate().
	Freeze();
	// The rows arrive unsorted and EndBatchUpdate() sorts them once, so the
	// header's sort key buys nothing here -- and on macOS it costs a re-query
	// and a full comparison pass per inserted row. See SuspendHeaderSort().
	SuspendHeaderSort();
	m_batchUpdate = true;
	m_batchFrozen = true;
}

void CSharedFilesCtrl::ThawForDisplay()
{
	// Only the freeze ends here; the batch runs on, so rows keep arriving as
	// O(1) appends. Whoever wants them ordered calls SortList().
	if (m_batchFrozen) {
		m_batchFrozen = false;
		Thaw();
	}
}

void CSharedFilesCtrl::SortIfRowsAppended()
{
	if (!m_batchRowsAppended) {
		return;
	}
	m_batchRowsAppended = false;
	if (m_startupDrain) {
		// The rows were appended without notifying the model, so this is the
		// first it hears of them: FinishBulkLoad() sorts and issues the single
		// Reset() that makes them exist for the control.
		FinishBulkLoad();
		ShowFilesCount();
	} else {
		SortList();
	}
}

void CSharedFilesCtrl::SetStartupDrainMode(bool on)
{
	if (m_startupDrain == on) {
		return;
	}
	m_startupDrain = on;
	if (!on) {
		// Anything appended since the last tick is still invisible to the
		// model; flush before the caller ends the batch.
		SortIfRowsAppended();
	}
}

void CSharedFilesCtrl::SetHashingCount(size_t remaining)
{
	if (m_hashingRemaining == remaining) {
		return;
	}
	m_hashingRemaining = remaining;
	ShowFilesCount();
}

void CSharedFilesCtrl::EndBatchUpdate(bool doSort)
{
	m_batchUpdate = false;
	// A poll that only updated rows in place (no new files) leaves the sort
	// order untouched, so skip the O(n log n) SortList entirely.
	if (doSort) {
		SortList();
	}
	m_batchRowsAppended = false;
	RestoreHeaderSort();
	// Skipped when ThawForDisplay() already ended the freeze: wx counts
	// Freeze/Thaw and an unbalanced Thaw() asserts.
	if (m_batchFrozen) {
		m_batchFrozen = false;
		Thaw();
	}
}

void CSharedFilesCtrl::RemoveFile(CKnownFile *toRemove)
{
	if (RowOfData(reinterpret_cast<wxUIntPtr>(toRemove)) == -1) {
		return;
	}
	RemoveItemData(reinterpret_cast<wxUIntPtr>(toRemove));
	m_shownSize -= toRemove->GetFileSize();
	ShowFilesCount();
}

void CSharedFilesCtrl::ClearList()
{
	ClearItemData();
}

void CSharedFilesCtrl::ShowFile(CKnownFile *file)
{
	// A newly-shared file that doesn't match the active filter stays hidden;
	// it'll appear if the filter is cleared/changed (which rebuilds the model).
	if (!MatchesFilter(file->GetFileName().GetPrintable())) {
		return;
	}

	if (m_batchUpdate) {
		// Batched poll (remote GUI): append at the end (O(1)) and let
		// EndBatchUpdate() sort once. AddItemData()'s sorted insert rebuilds
		// the whole row index on every call (O(n)), which turns a burst of
		// freshly-shared files into an O(n^2) freeze on a large share.
		// Mirrors CDownloadListCtrl::ShowFile(). AppendItemDataNow() keeps the
		// row index and item count live, so the interleaved reconcile prune
		// and in-place UpdateItem() during the same poll stay correct.
		const wxUIntPtr data = reinterpret_cast<wxUIntPtr>(file);
		if (RowOfData(data) == -1) {
			if (m_startupDrain) {
				// Silent append: the drain tick tells the model once, and
				// refreshes the label, for everything since the last one.
				AppendItemData(data);
			} else {
				AppendItemDataNow(data);
			}
			m_batchRowsAppended = true;
			m_shownSize += file->GetFileSize();
			if (!m_startupDrain) {
				ShowFilesCount();
			}
		}
		return;
	}
	DoShowFile(file, false);
}

void CSharedFilesCtrl::DoShowFile(CKnownFile *file, bool batch)
{
	if (batch) {
		// Bulk (re)load: append unsorted; ShowFileList() sorts + indexes once
		// at the end. No dedupe/count here, exactly as before.
		AppendItemData(reinterpret_cast<wxUIntPtr>(file));
		return;
	}

	// AddItemData() dedupes internally.
	if (RowOfData(reinterpret_cast<wxUIntPtr>(file)) == -1) {
		m_shownSize += file->GetFileSize();
	}
	AddItemData(reinterpret_cast<wxUIntPtr>(file));
	ShowFilesCount();
}

void CSharedFilesCtrl::OnSetPriority(wxCommandEvent &event)
{
	int priority = 0;

	switch (event.GetId()) {
	case MP_PRIOVERYLOW:
		priority = PR_VERY_LOW;
		break;
	case MP_PRIOLOW:
		priority = PR_LOW;
		break;
	case MP_PRIONORMAL:
		priority = PR_NORMAL;
		break;
	case MP_PRIOHIGH:
		priority = PR_HIGH;
		break;
	case MP_PRIOVERYHIGH:
		priority = PR_VERYHIGH;
		break;
	case MP_POWERSHARE:
		priority = PR_POWERSHARE;
		break;
	}

	for (wxUIntPtr data : GetSelectedItemData()) {
		CKnownFile *file = reinterpret_cast<CKnownFile *>(data);
		CoreNotify_KnownFile_Up_Prio_Set(file, priority);
		RefreshItemData(data);
	}
}

void CSharedFilesCtrl::OnSetPriorityAuto(wxCommandEvent &WXUNUSED(event))
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		CKnownFile *file = reinterpret_cast<CKnownFile *>(data);
		CoreNotify_KnownFile_Up_Prio_Auto(file);
		RefreshItemData(data);
	}
}

wxString CSharedFilesCtrl::LinkForFile(const CKnownFile *file, int menuId) const
{
	switch (menuId) {
	case MP_GETMAGNETLINK:
		return theApp->CreateMagnetLink(file);
	case MP_GETSOURCEED2KLINK:
		return theApp->CreateED2kLink(file, true);
	case MP_GETCRYPTSOURCEDED2KLINK:
		return theApp->CreateED2kLink(file, true, false, true);
	case MP_GETHOSTNAMESOURCEED2KLINK:
		return theApp->CreateED2kLink(file, true, true);
	case MP_GETHOSTNAMECRYPTSOURCEED2KLINK:
		return theApp->CreateED2kLink(file, true, true, true);
	case MP_GETAICHED2KLINK:
		return theApp->CreateED2kLink(file, false, false, false, true);
	case MP_GETAICHED2KLINKSRC:
		return theApp->CreateED2kLink(file, true, false, false, true);
	default:
		// MP_GETED2KLINK, and what the collection export asks for.
		return theApp->CreateED2kLink(file);
	}
}

wxString CSharedFilesCtrl::SelectedLinks(int menuId) const
{
	wxString links;

	for (wxUIntPtr data : GetSelectedItemData()) {
		const CKnownFile *file = reinterpret_cast<const CKnownFile *>(data);
		if (file != nullptr) {
			links += LinkForFile(file, menuId) + "\n";
		}
	}

	return links;
}

void CSharedFilesCtrl::OnCreateURI(wxCommandEvent &event)
{
	if (event.GetId() == MP_GETSOURCEED2KLINK || event.GetId() == MP_GETCRYPTSOURCEDED2KLINK) {
		if (!((theApp->IsConnectedED2K() && !theApp->serverconnect->IsLowID()) ||
			    (theApp->IsConnectedKad() && !theApp->IsFirewalledKad()))) {
			wxMessageBox(_("You need a HighID to create a valid sourcelink"),
				_("WARNING"),
				wxOK | wxICON_ERROR,
				this);
			return;
		}
	}

	wxString URIs = SelectedLinks(event.GetId());
	if (!URIs.IsEmpty()) {
		theApp->CopyTextToClipboard(URIs.RemoveLast());
	}
}

void CSharedFilesCtrl::OnExportCollection(wxCommandEvent &WXUNUSED(evt))
{
	const wxString caption = _("Export to emulecollection");

	// Same links the "Copy eD2k link to clipboard" item produces, for the
	// same selection: a text collection is a plain list of them, which is
	// what CMuleCollection and eMule both read back.
	const wxString links = SelectedLinks(MP_GETED2KLINK);
	if (links.IsEmpty()) {
		wxMessageBox(_("No files are selected to export."), caption, wxOK | wxICON_INFORMATION, this);
		return;
	}

	// Timestamped so repeated exports sit side by side instead of one
	// silently replacing the last. Sortable order, no characters that would
	// need escaping on any of the filesystems we support.
	const wxString defaultName =
		CFormat("amule-shared-%s.emulecollection") % wxDateTime::Now().Format("%Y%m%d-%H%M%S");

	wxString wildcard = _("eMule collections (*.emulecollection)");
	wildcard += "|*.emulecollection|";
	wildcard += _("All files");
	wildcard += "|*";
	wxFileDialog dialog(
		this, caption, wxEmptyString, defaultName, wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dialog.ShowModal() != wxID_OK) {
		return;
	}

	wxString path = dialog.GetPath();
	if (path.IsEmpty()) {
		return;
	}
	// Append the extension when the user picked the collection filter and left
	// it off. wxFD_OVERWRITE_PROMPT judged the name as typed, so ask again
	// here: without this, saving as "backup" over an existing
	// "backup.emulecollection" would overwrite it without a word.
	if (dialog.GetFilterIndex() == 0 && !path.Lower().EndsWith(".emulecollection")) {
		path += ".emulecollection";
		if (wxFileExists(path) &&
			wxMessageBox(CFormat(_("'%s' already exists. Overwrite it?")) % path,
				caption,
				wxYES_NO | wxICON_QUESTION,
				this) != wxYES) {
			return;
		}
	}

	// UTF-8 and no byte-order mark: aMule skips a BOM when reading a text
	// collection, but eMule reads one through CStdioFile in text mode, where
	// the mark would land in the first line and cost that entry.
	const wxCharBuffer utf8(unicode2UTF8(links));
	wxFile out;
	if (!out.Create(path, true) || out.Write(utf8.data(), utf8.length()) != utf8.length() ||
		!out.Close()) {
		// A failure part-way through leaves a truncated collection behind;
		// drop it rather than let it look like a good export.
		out.Close();
		wxRemoveFile(path);
		wxMessageBox(CFormat(_("Failed to write the collection file '%s'.")) % path,
			caption,
			wxOK | wxICON_ERROR,
			this);
		return;
	}

	// One string for the log line and the confirmation both, so the export
	// costs translators one plural form rather than two near-identical ones.
	const unsigned written = static_cast<unsigned>(links.Freq('\n'));
	const wxString message = CFormat(wxPLURAL("Exported %u shared file to '%s'.",
					 "Exported %u shared files to '%s'.",
					 static_cast<int>(written))) %
				 written % path;
	AddLogLineC(message);
	wxMessageBox(message, caption, wxOK | wxICON_INFORMATION, this);
}

void CSharedFilesCtrl::OnEditComment(wxCommandEvent &WXUNUSED(event))
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (!selected.empty()) {
		CKnownFile *file = reinterpret_cast<CKnownFile *>(selected.front());
		CCommentDialog dialog(this, file);
		dialog.ShowModal();
	}
}

int CSharedFilesCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const
{
	const CKnownFile *file1 = reinterpret_cast<const CKnownFile *>(data1);
	const CKnownFile *file2 = reinterpret_cast<const CKnownFile *>(data2);
	const int mod = modifier;

	switch (column) {
	// Sort by filename.
	case COLUMN_SHARED_NAME:
		return mod * CmpAny(file1->GetFileName(), file2->GetFileName());

	// Sort by filesize.
	case COLUMN_SHARED_SIZE:
		return mod * CmpAny(file1->GetFileSize(), file2->GetFileSize());

	// Sort by filetype.
	case COLUMN_SHARED_TYPE:
		return mod * GetFiletypeByName(file1->GetFileName())
				     .CmpNoCase(GetFiletypeByName(file2->GetFileName()));

	// Sort by priority.
	case COLUMN_SHARED_PRIO: {
		int8 prioA = file1->GetUpPriority();
		int8 prioB = file2->GetUpPriority();

		// Work-around for PR_VERY_LOW which has value 4. See KnownFile.h for that stupidity ...
		return mod * CmpAny((prioB != PR_VERY_LOW ? prioB : -1), (prioA != PR_VERY_LOW ? prioA : -1));
	}

	// Sort by Requests this session.
	case COLUMN_SHARED_REQ:
		if (alt) {
			return mod * CmpAny(file1->statistic.GetAllTimeRequests(),
					     file2->statistic.GetAllTimeRequests());
		} else {
			return mod * CmpAny(file1->statistic.GetRequests(), file2->statistic.GetRequests());
		}

	// Sort by accepted requests. Ascending.
	case COLUMN_SHARED_AREQ:
		if (alt) {
			return mod * CmpAny(file1->statistic.GetAllTimeAccepts(),
					     file2->statistic.GetAllTimeAccepts());
		} else {
			return mod * CmpAny(file1->statistic.GetAccepts(), file2->statistic.GetAccepts());
		}

	// Sort by transferred. Ascending.
	case COLUMN_SHARED_TRA:
		if (alt) {
			return mod * CmpAny(file1->statistic.GetAllTimeTransferred(),
					     file2->statistic.GetAllTimeTransferred());
		} else {
			return mod *
			       CmpAny(file1->statistic.GetTransferred(), file2->statistic.GetTransferred());
		}

	// Sort by Share Ratio. Ascending.
	case COLUMN_SHARED_RTIO:
		return mod * CmpAny(static_cast<double>(file1->statistic.GetAllTimeTransferred()) /
					     static_cast<double>(file1->GetFileSize()),
				     static_cast<double>(file2->statistic.GetAllTimeTransferred()) /
					     static_cast<double>(file2->GetFileSize()));

	// Complete sources asc
	case COLUMN_SHARED_CMPL:
		return mod * CmpAny(file1->m_nCompleteSourcesCount, file2->m_nCompleteSourcesCount);

	// Live upload speed asc
	case COLUMN_SHARED_SPEED:
		return mod * CmpAny(file1->GetUploadDatarate(), file2->GetUploadDatarate());

	// Shared-since date asc
	case COLUMN_SHARED_SINCE:
		return mod * CmpAny(file1->GetDateShared(), file2->GetDateShared());

	// Last-upload date asc
	case COLUMN_SHARED_LASTUP:
		return mod * CmpAny(file1->GetLastUpload(), file2->GetLastUpload());

	// Directory path asc (status-agnostic: the Temp dir for a partfile)
	case COLUMN_SHARED_PATH:
		return mod * CmpAny(file1->GetFilePath(), file2->GetFilePath());

	default:
		return 0;
	}
}

void CSharedFilesCtrl::UpdateItem(CKnownFile *toupdate)
{
	if (m_inBulkUpdate) {
		// Caller (e.g. CSharedFileList::ClearED2KPublishInfo) will
		// issue a single Refresh() in EndBulkUpdate(). Skipping the
		// per-row refresh here is what turns ClearED2KPublishInfo
		// from O(N²) into O(N) for users with thousands of shared
		// files. See #302.
		return;
	}
	const wxUIntPtr data = reinterpret_cast<wxUIntPtr>(toupdate);
	if (!HasItemData(data)) {
		return;
	}
	// Repaints the row and, if sorted by a live column, schedules the
	// throttled+idle-gated re-sort.
	RefreshItemData(data);

	if (IsItemDataSelected(data)) {
		theApp->amuledlg->m_sharedfileswnd->SelectionUpdated();
	}
}

void CSharedFilesCtrl::BeginBulkUpdate()
{
	m_inBulkUpdate = true;
}

void CSharedFilesCtrl::EndBulkUpdate()
{
	m_inBulkUpdate = false;
	// One full repaint covers every row whose data changed during the
	// bulk window. SelectionUpdated() refreshes the right-hand detail
	// panel in case the selected row's data changed.
	Refresh();
	if (theApp->amuledlg && theApp->amuledlg->m_sharedfileswnd) {
		theApp->amuledlg->m_sharedfileswnd->SelectionUpdated();
	}
}

uint64 CSharedFilesCtrl::ShownIncompleteBytes() const
{
	if (!theApp->downloadqueue) {
		return 0;
	}

	std::vector<CPartFile *> parts;
#ifdef CLIENT_GUI
	// CDownQueueRem is a std::map keyed by ECID, with no CopyFileList().
	// This file is compiled per executable rather than into muleappgui
	// (cmake/source-vars.cmake), so the two builds really do see their own
	// download queue here.
	parts.reserve(theApp->downloadqueue->size());
	for (const auto &entry : *theApp->downloadqueue) {
		parts.push_back(entry.second);
	}
#else
	theApp->downloadqueue->CopyFileList(parts);
#endif

	uint64 missing = 0;
	for (CPartFile *part : parts) {
		if (!part) {
			continue;
		}
		// Upcast before the row lookup: the list keys its rows by the
		// CKnownFile the share holds, which is what was stored.
		const CKnownFile *known = part;
		if (RowOfData(reinterpret_cast<wxUIntPtr>(known)) == -1) {
			continue;
		}
		const uint64 size = part->GetFileSize();
		const uint64 obtained = part->GetCompletedSize();
		if (obtained < size) {
			missing += size - obtained;
		}
	}
	return missing;
}

void CSharedFilesCtrl::UpdateTotalSize()
{
	// The label lives in the statistics box (the bottom pane of the shared
	// splitter), a different window from this list, so reach it through the
	// shared-files window rather than GetParent(). Resolved once; see the
	// note on m_freeSpaceLabel.
	if (!theApp->amuledlg || !theApp->amuledlg->m_sharedfileswnd) {
		return;
	}
	if (!m_totalSizeLabel) {
		m_totalSizeLabel =
			CastByName("sharedFilesTotalSize", theApp->amuledlg->m_sharedfileswnd, wxStaticText);
		if (!m_totalSizeLabel) {
			return;
		}
	}

	// The total is the sum of the Size column, which shows a part file's
	// final size. That answers "how much am I sharing" but not "how much of
	// it do I have", and the two differ by however much is still to
	// download -- 400 GB of it in the report that prompted this (#927). Both
	// are shown rather than one replaced, so the total still adds up to its
	// own column.
	// "completed", the word the Downloads list already uses for this same
	// quantity (its Completed column), rather than "complete": what is being
	// counted is bytes obtained, not files that are finished.
	const uint64 missing = ShownIncompleteBytes();
	const wxString text =
		missing ? CFormat(_("Total size of Shared Files: %s (%s completed)")) %
				  CastItoXBytes(m_shownSize) % CastItoXBytes(m_shownSize - missing)
			: CFormat(_("Total size of Shared Files: %s")) % CastItoXBytes(m_shownSize);

	// SetLabel() repaints even when the text is unchanged, and the GUI timer
	// calls this once a second for as long as the panel is up.
	if (m_totalSizeLabel->GetLabel() == text) {
		return;
	}
	m_totalSizeLabel->SetLabel(text);
	m_totalSizeLabel->GetParent()->Layout();
}

void CSharedFilesCtrl::UpdateFreeSpace()
{
	if (!theApp->amuledlg || !theApp->amuledlg->m_sharedfileswnd) {
		return;
	}
	// Resolved once; see the note on CDownloadListCtrl's copy.
	if (!m_freeSpaceLabel) {
		m_freeSpaceLabel =
			CastByName("sharedFilesFreeSpace", theApp->amuledlg->m_sharedfileswnd, wxStaticText);
	}
	CamuleDlg::SetFreeSpaceLabel(m_freeSpaceLabel, theStats::GetIncomingFreeSpace(), false);
}

void CSharedFilesCtrl::ShowFilesCount()
{
	wxStaticText *label = CastByName("sharedFilesLabel", GetParent(), wxStaticText);

	if (m_hashingRemaining > 0) {
		// Startup is still hashing files the scan found; they join the list
		// as each one finishes. Said here rather than in a dialog: it is
		// progress, not something to acknowledge (#853).
		label->SetLabel(CFormat(wxPLURAL("Shared Files (%i, hashing %u more)",
					"Shared Files (%i, hashing %u more)",
					m_hashingRemaining)) %
				ItemDataCount() % m_hashingRemaining);
	} else {
		label->SetLabel(CFormat(_("Shared Files (%i)")) % ItemDataCount());
	}

	UpdateTotalSize();

	label->GetParent()->Layout();
	// If file list was updated, the "selection" is involved too, if we chose to show clients for all
	// files. So update client list here too.
	theApp->amuledlg->m_sharedfileswnd->SelectionUpdated();
}

void CSharedFilesCtrl::OnRename(wxCommandEvent &WXUNUSED(event))
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (selected.empty()) {
		return;
	}
	CKnownFile *file = reinterpret_cast<CKnownFile *>(selected.front());

	wxString strNewName = ::wxGetTextFromUser(
		_("Enter new name for this file:"), _("File rename"), file->GetFileName().GetPrintable());

	CPath newName = CPath(strNewName);
	if (newName.IsOk() && (newName != file->GetFileName())) {
		theApp->sharedfiles->RenameFile(file, newName);
	}
}

bool CSharedFilesCtrl::OnListKey(wxKeyEvent &event)
{
	if (event.GetKeyCode() == WXK_F2) {
		wxCommandEvent evt;
		OnRename(evt);
		return true;
	}
	return false;
}

void CSharedFilesCtrl::OnAddCollection(wxCommandEvent &WXUNUSED(evt))
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (selected.empty()) {
		return;
	}
	CKnownFile *file = reinterpret_cast<CKnownFile *>(selected.front());

	// The collection is parsed on this side, so its bytes have to be readable
	// from this host. amulegui is shown the daemon's directory, which
	// ResolvePath() rewrites through the user's configured mapping; in the
	// monolithic build the path is already local and resolves unchanged.
	//
	// A part file is turned away rather than resolved: it resolves to the
	// ".part" in the temp directory, which does exist, so a half-downloaded
	// collection would parse and quietly queue whichever links happened to be
	// on disk already. The menu entry is disabled for those, so this repeats
	// that test only to keep the handler correct on its own.
	CPath collection;
	if (file->IsPartFile() || !FileLaunch::ResolvePath(file, collection)) {
		return;
	}

	// The same expansion the command line and the macOS open-document path
	// use: it reports a missing, malformed or empty collection itself, and
	// runs every entry through CheckPassedLink() instead of forwarding the raw
	// strings -- so magnet entries convert and invalid ones are named.
	wxArrayString links;
	if (theApp->ExpandPassedCollection(collection.GetRaw(), links, 0) !=
		CamuleAppCommon::kCollectionExpanded) {
		// Already logged by ExpandPassedCollection().
		return;
	}
	theApp->downloadqueue->AddLinks(links);
}

// Both act on one file: the menu is opened over a row, and opening several
// files at once is not a thing either desktop handler does gracefully.
void CSharedFilesCtrl::OnOpenFile(wxCommandEvent &WXUNUSED(event))
{
	// Bound re-checked: PopupMenu runs a nested event loop, so the shared-dir
	// watcher can mutate the list while the menu is open. Pinning the identity
	// means a row removed above this one cannot redirect the click.
	if (m_menuItem != 0 && HasItemData(m_menuItem)) {
		FileLaunch::Open(reinterpret_cast<CKnownFile *>(m_menuItem), this);
	}
}

void CSharedFilesCtrl::OnShowInFolder(wxCommandEvent &WXUNUSED(event))
{
	if (m_menuItem != 0 && HasItemData(m_menuItem)) {
		FileLaunch::Reveal(reinterpret_cast<CKnownFile *>(m_menuItem), this);
	}
}

// File_checked_for_headers
