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

#include "DownloadListCtrl.h" // Interface declarations

#include <common/Format.h> // Needed for CFormat
#include <common/MenuIDs.h>

#include "amule.h"            // Needed for theApp
#include "amuleDlg.h"         // Needed for CamuleDlg
#include "CommentDialogLst.h" // Needed for CCommentDialogLst
#include "DataToText.h"       // Needed for PriorityToStr
#include "DownloadQueue.h"
#include "FileDetailDialog.h" // Needed for CFileDetailDialog
#include "FileLaunch.h"       // Needed for FileLaunch::Open / Reveal
#include "GuiEvents.h"        // Needed for CoreNotify_*, Notify_DownloadCtrlDoItemSelectionChanged
#include "Logger.h"
#include "MuleBarRenderer.h" // Needed for CBarFillSpec, CBarFillSpan, CMuleBarRenderer
#include "PartFile.h"        // Needed for CPartFile
#include "Preferences.h"
#include "SharedFileList.h" // Needed for CSharedFileList
#include "SourceListCtrl.h"
#include "Statistics.h" // Needed for theStats (free space), FREE_SPACE_UNKNOWN
#include "TransferWnd.h"
#include "muuli_wdr.h" // Needed for ID_DLOADLIST

namespace
{
// Colours for the chunk/gap bar; matches every value DrawFileStatusBar used.
const CMuleColour crHave(104, 104, 104);
const CMuleColour crFlatHave(0, 0, 0);

const CMuleColour crPending(255, 208, 0);
const CMuleColour crFlatPending(255, 255, 100);

const CMuleColour crProgress(0, 224, 0);
const CMuleColour crFlatProgress(0, 150, 0);

const CMuleColour crMissing(255, 0, 0);

/**
 * Draws the Progress column: the chunk/gap bar (via the base class) plus two
 * things Shared Files' bar never needed -- a completed-progress overlay
 * strip and a live percentage label. Checked both CGenericClientListCtrl
 * bars while designing this: neither draws text or a strip, so this stays a
 * Downloads-only subclass rather than growing CBarFillSpec for everyone.
 */
class CDownloadBarRenderer : public CMuleBarRenderer
{
public:
	bool Render(wxRect cell, wxDC *dc, int state) override
	{
		// Gates the whole cell -- bar, strip and percent text alike -- exactly
		// as the pre-port ColumnProgress case did.
		if (!thePrefs::ShowProgBar()) {
			return true;
		}
		CMuleBarRenderer::Render(cell, dc, state);
		DrawOverlay(cell, dc);
		return true;
	}

private:
	void DrawOverlay(wxRect cell, wxDC *dc) const
	{
		CPartFile *file = reinterpret_cast<CPartFile *>(GetSpec().GetIdentity());
		if (!file || file->GetFileSize() == 0) {
			return;
		}

		const bool bFlat = thePrefs::UseFlatBar();
		const wxRect barRect = InsetForBar(cell, bFlat);

		// The completed-progress strip only makes sense for a file still
		// assembling from gaps/pending blocks: DrawFileStatusBar's completed
		// and hashing branches already fill the whole bar solid, so the strip
		// would be redundant there (and, while hashing, actively wrong -- that
		// fill tracks hashed bytes, not completed bytes).
		const bool showStrip = !file->IsCompleted() && file->GetStatus() != PS_COMPLETING &&
				       file->GetHashingProgress() == 0;
		if (showStrip && barRect.GetWidth() > 0) {
			const int width = static_cast<int>((static_cast<double>(barRect.GetWidth()) /
								   static_cast<double>(file->GetFileSize())) *
							   static_cast<double>(file->GetCompletedSize()));
			if (bFlat) {
				// Deliberately no outline: master drew this strip into a fresh
				// wxMemoryDC, whose default black pen gave it one, but that was
				// incidental -- nothing set the pen on purpose, so it would
				// have carried over whatever the list's own DC last used had
				// this been drawn there instead. Transparent is the
				// intentional, deterministic choice; wxBLACK_PEN would
				// reproduce the old look if that turns out to be preferred.
				dc->SetPen(*wxTRANSPARENT_PEN);
				dc->SetBrush(crFlatProgress.GetBrush());
				dc->DrawRectangle(barRect.x, barRect.y, width, 3);
			} else {
				dc->SetPen(*wxBLACK_PEN);
				dc->DrawLine(barRect.x, barRect.y + 0, barRect.x + width, barRect.y + 0);
				dc->DrawLine(barRect.x, barRect.y + 2, barRect.x + width, barRect.y + 2);
				dc->SetPen(*(wxThePenList->FindOrCreatePen(crProgress, 1, wxPENSTYLE_SOLID)));
				dc->DrawLine(barRect.x, barRect.y + 1, barRect.x + width, barRect.y + 1);
			}
		}

		if (!thePrefs::ShowPercent()) {
			return;
		}
		const uint16 hashingProgress = file->GetHashingProgress();
		double percent = hashingProgress == 0 ? file->GetPercentCompleted()
						      : 100.0 * hashingProgress * PARTSIZE /
								static_cast<double>(file->GetFileSize());
		if (file->IsCompleted()) {
			percent = 100.0;
		} else if (percent > 99.9) {
			percent = 99.9;
		}
		const wxString buffer = CFormat("%.1f%%") % percent;
		const int middlex = (2 * cell.GetX() + cell.GetWidth()) >> 1;
		const int middley = (2 * cell.GetY() + cell.GetHeight()) >> 1;
		wxCoord textwidth;
		wxCoord textheight;
		dc->GetTextExtent(buffer, &textwidth, &textheight);
		const wxColour oldColour = dc->GetTextForeground();
		// Ordinary progress bar: white percentage. Hashing (green/yellow
		// bar): black percentage.
		dc->SetTextForeground(hashingProgress == 0 ? *wxWHITE : *wxBLACK);
		dc->DrawText(buffer, middlex - (textwidth >> 1), middley - (textheight >> 1));
		dc->SetTextForeground(oldColour);
	}
};
} // namespace

wxBEGIN_EVENT_TABLE(CDownloadListCtrl, CMuleVirtualDataViewCtrl)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CDownloadListCtrl::OnItemActivated)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CDownloadListCtrl::OnItemRightClicked)
	EVT_DATAVIEW_SELECTION_CHANGED(wxID_ANY, CDownloadListCtrl::OnSelectionChanged)

	EVT_MENU(MP_CANCEL, CDownloadListCtrl::OnCancelFile)

	EVT_MENU(MP_PAUSE, CDownloadListCtrl::OnSetStatus)
	EVT_MENU(MP_STOP, CDownloadListCtrl::OnSetStatus)
	EVT_MENU(MP_RESUME, CDownloadListCtrl::OnSetStatus)

	EVT_MENU(MP_PRIOLOW, CDownloadListCtrl::OnSetPriority)
	EVT_MENU(MP_PRIONORMAL, CDownloadListCtrl::OnSetPriority)
	EVT_MENU(MP_PRIOHIGH, CDownloadListCtrl::OnSetPriority)
	EVT_MENU(MP_PRIOAUTO, CDownloadListCtrl::OnSetPriority)

	EVT_MENU(MP_SWAP_A4AF_TO_THIS, CDownloadListCtrl::OnSwapSources)
	EVT_MENU(MP_SWAP_A4AF_TO_THIS_AUTO, CDownloadListCtrl::OnSwapSources)
	EVT_MENU(MP_SWAP_A4AF_TO_ANY_OTHER, CDownloadListCtrl::OnSwapSources)

	EVT_MENU_RANGE(MP_ASSIGNCAT, MP_ASSIGNCAT + 99, CDownloadListCtrl::OnSetCategory)

	EVT_MENU(MP_CLEARCOMPLETED, CDownloadListCtrl::OnClearCompleted)

	EVT_MENU(MP_GETMAGNETLINK, CDownloadListCtrl::OnGetLink)
	EVT_MENU(MP_GETED2KLINK, CDownloadListCtrl::OnGetLink)

	EVT_MENU(MP_METINFO, CDownloadListCtrl::OnViewFileInfo)
	EVT_MENU(MP_VIEW, CDownloadListCtrl::OnPreviewFile)
	EVT_MENU(MP_SHOWINFOLDER, CDownloadListCtrl::OnShowInFolder)
	EVT_MENU(MP_VIEWFILECOMMENTS, CDownloadListCtrl::OnViewFileComments)

	EVT_MENU(MP_WS, CDownloadListCtrl::OnGetFeedback)
wxEND_EVENT_TABLE()

CDownloadListCtrl::CDownloadListCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: CMuleVirtualDataViewCtrl(parent, winid, pos, size, style, name)
{
	const int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
	AddTextColumn(_("Part"), COLUMN_DL_PART, "a", 30, wxALIGN_LEFT, flags);
	AddIconTextColumn(_("File Name"), COLUMN_DL_NAME, "N", 260, wxALIGN_LEFT, flags);
	AddTextColumn(_("Size"), COLUMN_DL_SIZE, "Z", 60, wxALIGN_LEFT, flags);
	AddTextColumn(_("Transferred"), COLUMN_DL_TRANSFERRED, "T", 65, wxALIGN_LEFT, flags);
	AddTextColumn(_("Completed"), COLUMN_DL_COMPLETED, "C", 65, wxALIGN_LEFT, flags);
	AddTextColumn(_("Speed"), COLUMN_DL_SPEED, "S", 65, wxALIGN_LEFT, flags);
	AddBarColumn(_("Progress"), COLUMN_DL_PROGRESS, "P", 170, flags, new CDownloadBarRenderer());
	AddTextColumn(_("Sources"), COLUMN_DL_SOURCES, "u", 50, wxALIGN_LEFT, flags);
	AddTextColumn(_("Priority"), COLUMN_DL_PRIORITY, "p", 55, wxALIGN_LEFT, flags);
	AddTextColumn(_("Status"), COLUMN_DL_STATUS, "s", 70, wxALIGN_LEFT, flags);
	AddTextColumn(_("Time Remaining"), COLUMN_DL_TIMEREMAINING, "r", 110, wxALIGN_LEFT, flags);
	AddTextColumn(_("Last Seen Complete"), COLUMN_DL_LASTSEENCOMPLETE, "c", 220, wxALIGN_LEFT, flags);
	AddTextColumn(_("Last Reception"), COLUMN_DL_LASTRECEPTION, "R", 220, wxALIGN_LEFT, flags);

	AppendSpacerColumn(COLUMN_DL_SPACER);
	AssociateVirtualModel();

	// Default sort is by name, ascending; LoadColumnSettings() replaces it
	// when the config has something saved.
	ApplySorting(COLUMN_DL_NAME, 0);

	m_columnStore.SetTableName("Download");
	LoadColumnSettings();
	InitColumnState();
}

wxString CDownloadListCtrl::GetOldColumnOrder() const
{
	return "N,Z,T,C,S,P,u,p,s,r,c,R";
}

wxString CDownloadListCtrl::GetRowLabel(const wxDataViewItem &item) const
{
	// Column 0 is the part number here, not the name -- unlike every other
	// ported list, so the base's "column 0 is the label" default would make
	// type-to-select match part numbers. GetModel() is always the
	// wxDataViewIndexListModel AssociateVirtualModel() installed.
	const wxDataViewListModel *model = static_cast<const wxDataViewListModel *>(GetModel());
	const wxUIntPtr data = ItemAt(static_cast<long>(model->GetRow(item)));
	if (!data) {
		return wxEmptyString;
	}
	return GetItemColumnText(data, COLUMN_DL_NAME);
}

CDownloadListCtrl::~CDownloadListCtrl() = default;

void CDownloadListCtrl::AddFile(CPartFile *file, bool deferView)
{
	wxASSERT(file);

	// Avoid duplicate entries of files
	if (m_files.insert(file).second) {
		// During a bulk load (remote GUI first sync) the caller defers the
		// display: showing and, above all, re-sorting the list on every one
		// of thousands of files is O(n^2) and freezes the GUI (issue #414).
		// ShowFileList() shows and sorts the whole list once afterwards.
		if (deferView) {
			return;
		}

		if (IsVisibleInCat(file, m_category)) {
			ShowFile(file, true);
			if (file->IsCompleted()) {
				CastByID(ID_BTNCLRCOMPL, GetParent(), wxButton)->Enable(true);
			}
			// During a batch update (a reconnect resync -- issue #444) the row
			// is still appended, but the per-item sort is deferred to
			// EndBatchUpdate()'s single SortList() so a large add stays
			// O(n log n) instead of O(n^2).
			if (!m_batchUpdate) {
				SortList();
			}
		}
	}
}

void CDownloadListCtrl::BeginBatchUpdate()
{
	// Coalesce a burst of AddFile()/UpdateItem() calls into a single repaint
	// (Freeze) and suppress the per-item SortList; EndBatchUpdate() sorts
	// once. Used when reconciling the whole list against a fresh server
	// snapshot after a reconnect (issue #444).
	Freeze();
	m_batchUpdate = true;
}

void CDownloadListCtrl::EndBatchUpdate(bool doSort)
{
	m_batchUpdate = false;
	// A poll that only updated rows in place (no new files) leaves the sort
	// order untouched, so skip the O(n log n) SortList entirely (issue #615).
	if (doSort) {
		SortList();
	}
	Thaw();
}

void CDownloadListCtrl::RebuildVisibleList()
{
	// Batch counterpart to AddFile()'s per-item path: drop the visible rows
	// and re-append every model item that passes the current category + text
	// filter, then sort once. Mirrors CSharedFilesCtrl::ShowFileList().
	Freeze();

	const std::vector<wxUIntPtr> selected = GetSelectedItemData();

	ClearItemData();

	bool hasCompletedDownloads = false;
	int shown = 0;
	uint64 totalSize = 0;

	for (CPartFile *file : m_files) {
		if (!IsVisibleInCat(file, m_category)) {
			continue;
		}
		AppendItemData(reinterpret_cast<wxUIntPtr>(file));
		++shown;
		totalSize += file->GetFileSize();
		if (file->IsCompleted()) {
			hasCompletedDownloads = true;
		}
	}

	FinishBulkLoad();
	SetSelectedItemData(selected);

	CastByID(ID_BTNCLRCOMPL, GetParent(), wxButton)->Enable(hasCompletedDownloads);
	SetFilesCount(shown);
	SetTotalSize(totalSize);

	Thaw();
}

void CDownloadListCtrl::ShowFileList()
{
	// Used after a deferred bulk load (remote GUI first sync), where the
	// model is populated but no rows are shown yet.
	RebuildVisibleList();
}

void CDownloadListCtrl::RemoveFile(CPartFile *file)
{
	wxASSERT(file);

	// Order matters: ShowFile() early-returns once `file` is no longer in
	// m_files, so the erase has to come after it -- swapped, the row would
	// stay in the model holding a pointer the caller is about to free.
	ShowFile(file, false);
	m_files.erase(file);
}

void CDownloadListCtrl::UpdateItem(const void *toupdate)
{
	CPartFile *file = const_cast<CPartFile *>(static_cast<const CPartFile *>(toupdate));
	if (m_files.find(file) == m_files.end()) {
		return;
	}

	const wxUIntPtr data = reinterpret_cast<wxUIntPtr>(file);
	const bool show = IsVisibleInCat(file, m_category);

	if (HasItemData(data)) {
		if (show) {
			// Repaints the row and, if sorted by a live column, schedules the
			// throttled+idle-gated re-sort.
			RefreshItemData(data);
		} else {
			// No longer shown in the current category.
			ShowFile(file, false);
		}
	} else if (show) {
		// Was hidden, but its new status means it belongs in the current
		// category now.
		ShowFile(file, true);
	}

	if (file->IsCompleted() && show) {
		CastByID(ID_BTNCLRCOMPL, GetParent(), wxButton)->Enable(true);
	}
}

void CDownloadListCtrl::ShowFile(CPartFile *file, bool show)
{
	wxASSERT(file);
	if (m_files.find(file) == m_files.end()) {
		return;
	}

	const wxUIntPtr data = reinterpret_cast<wxUIntPtr>(file);
	if (show) {
		// Add the row unless it is already being displayed. Appended at the
		// end (unsorted); the caller sorts once afterwards.
		if (!HasItemData(data)) {
			AppendItemDataNow(data);
			ShowFilesCount(1);
			SetTotalSize(m_shownSize + file->GetFileSize());
		}
	} else {
		if (HasItemData(data)) {
			RemoveItemData(data);
			ShowFilesCount(-1);
			SetTotalSize(m_shownSize - file->GetFileSize());
		}
	}
}

void CDownloadListCtrl::ChangeCategory(int newCategory)
{
	// Same one-pass rebuild as the text filter: hiding the rows of the old
	// category individually paid an O(n) row-index rebuild per removal, so
	// switching category on a large queue froze the GUI just like filtering
	// did (issue #669).
	m_category = newCategory;
	RebuildVisibleList();
}

bool CDownloadListCtrl::IsVisibleInCat(CPartFile *file, int category) const
{
	return file->CheckShowItemInGivenCat(category) && MatchesFilter(file->GetFileName().GetPrintable());
}

void CDownloadListCtrl::RebuildFilteredView()
{
	// Filter hook from CMuleVirtualDataViewCtrl: m_files always holds every
	// download, so the visible set is rebuilt from it in one pass.
	RebuildVisibleList();
}

void CDownloadListCtrl::OnItemRightClicked(wxDataViewEvent &event)
{
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
	// Bound re-checked by the handlers that use it (OnPreviewFile,
	// OnShowInFolder): PopupMenu runs a nested event loop, so the queue can
	// mutate while the menu is open.
	m_menuItem = selected.front();
	CPartFile *file = reinterpret_cast<CPartFile *>(m_menuItem);

	delete m_menu;
	m_menu = new wxMenu(_("Downloads"));

	wxMenu *priomenu = new wxMenu();
	priomenu->AppendCheckItem(MP_PRIOLOW, _("Low"));
	priomenu->AppendCheckItem(MP_PRIONORMAL, _("Normal"));
	priomenu->AppendCheckItem(MP_PRIOHIGH, _("High"));
	priomenu->AppendCheckItem(MP_PRIOAUTO, _("Auto"));

	m_menu->Append(MP_MENU_PRIO, _("Priority"), priomenu);
	m_menu->Append(MP_CANCEL, _("Cancel"));
	m_menu->Append(MP_STOP, _("&Stop"));
	m_menu->Append(MP_PAUSE, _("&Pause"));
	m_menu->Append(MP_RESUME, _("&Resume"));
	m_menu->Append(MP_CLEARCOMPLETED, _("C&lear completed"));
	m_menu->AppendSeparator();
	wxMenu *extendedmenu = new wxMenu();
	extendedmenu->Append(MP_SWAP_A4AF_TO_THIS, _("Swap every A4AF to this file now"));
	extendedmenu->AppendCheckItem(MP_SWAP_A4AF_TO_THIS_AUTO, _("Swap every A4AF to this file (Auto)"));
	extendedmenu->AppendSeparator();
	extendedmenu->Append(MP_SWAP_A4AF_TO_ANY_OTHER, _("Swap every A4AF to any other file now"));
	m_menu->Append(MP_MENU_EXTD, _("Extended Options"), extendedmenu);
	m_menu->AppendSeparator();

	m_menu->Append(MP_VIEW, _("Preview"));
	m_menu->Append(MP_SHOWINFOLDER, _("Show in file manager"));
	m_menu->Append(MP_METINFO, _("Show file &details"));
	m_menu->Append(MP_VIEWFILECOMMENTS, _("Show all comments"));
	m_menu->AppendSeparator();
	m_menu->Append(MP_GETMAGNETLINK, _("Copy magnet URI to clipboard"));
	m_menu->Append(MP_GETED2KLINK, _("Copy eD2k &link to clipboard"));
	m_menu->Append(MP_WS, _("Copy feedback to clipboard"));
	m_menu->AppendSeparator();

	wxMenu *cats = new wxMenu(_("Category"));
	if (theApp->glob_prefs->GetCatCount() > 1) {
		for (uint32 i = 0; i < theApp->glob_prefs->GetCatCount(); i++) {
			if (i == 0) {
				cats->Append(MP_ASSIGNCAT, _("unassign"));
			} else {
				cats->Append(MP_ASSIGNCAT + static_cast<int>(i),
					theApp->glob_prefs->GetCategory(i)->title);
			}
		}
	}
	m_menu->Append(MP_MENU_CATS, _("Assign to category"), cats);
	m_menu->Enable(MP_MENU_CATS, (theApp->glob_prefs->GetCatCount() > 1));

	bool canStop;
	bool canPause;
	bool canCancel;
	bool fileResumable;
	if (file->GetStatus(true) != PS_ALLOCATING) {
		const uint8_t fileStatus = file->GetStatus();
		canStop = (fileStatus != PS_ERROR) && (fileStatus != PS_COMPLETE) &&
			  (file->IsStopped() != true);
		canPause = (file->GetStatus() != PS_PAUSED) && canStop;
		fileResumable = (fileStatus == PS_PAUSED) || (fileStatus == PS_ERROR) ||
				(fileStatus == PS_INSUFFICIENT);
		canCancel = fileStatus != PS_COMPLETE;
	} else {
		canStop = canPause = canCancel = fileResumable = false;
	}

	m_menu->Enable(MP_CANCEL, canCancel);
	m_menu->Enable(MP_PAUSE, canPause);
	m_menu->Enable(MP_STOP, canStop);
	m_menu->Enable(MP_RESUME, fileResumable);
	m_menu->Enable(MP_CLEARCOMPLETED, CastByID(ID_BTNCLRCOMPL, GetParent(), wxButton)->IsEnabled());

	wxString view;
	if (file->IsPartFile()) {
		view = CFormat("%s [%s]") % _("Preview") % file->GetPartMetFileName().RemoveExt();
	} else {
		view = _("&Open the file");
	}
	m_menu->SetLabel(MP_VIEW, view);
	const bool previewable = file->IsPartFile() ? file->PreviewAvailable() : true;
	bool canOpen = false;
	bool canReveal = false;
	FileLaunch::GetAvailability(file, canOpen, canReveal);
	m_menu->Enable(MP_VIEW, previewable && canOpen);
	m_menu->Enable(MP_SHOWINFOLDER, canReveal);

	FileRatingList ratingList;
	file->GetRatingAndComments(ratingList);
	// Enable when there are source comments to show, or when Kad is connected
	// so the dialog's "Get from Kad" lookup can retrieve community notes
	// (#434) even for a file that has no per-source comments yet.
	m_menu->Enable(MP_VIEWFILECOMMENTS, !ratingList.empty() || theApp->IsConnectedKad());

	m_menu->Check(MP_SWAP_A4AF_TO_THIS_AUTO, file->IsA4AFAuto());

	int priority = file->IsAutoDownPriority() ? PR_AUTO : file->GetDownPriority();
	priomenu->Check(MP_PRIOHIGH, priority == PR_HIGH);
	priomenu->Check(MP_PRIONORMAL, priority == PR_NORMAL);
	priomenu->Check(MP_PRIOLOW, priority == PR_LOW);
	priomenu->Check(MP_PRIOAUTO, priority == PR_AUTO);

	m_menu->Enable(MP_MENU_EXTD, canPause);

	PopupMenu(m_menu);

	delete m_menu;
	m_menu = nullptr;
}

void CDownloadListCtrl::OnCancelFile(wxCommandEvent &WXUNUSED(event))
{
	std::vector<wxUIntPtr> files = GetSelectedItemData();
	files.erase(std::remove_if(files.begin(),
			    files.end(),
			    [](wxUIntPtr data) {
				    switch (reinterpret_cast<CPartFile *>(data)->GetStatus()) {
				    case PS_WAITING_FOR_HASH:
				    case PS_HASHING:
				    case PS_COMPLETING:
				    case PS_COMPLETE:
					    return true;
				    default:
					    return false;
				    }
			    }),
		files.end());
	if (files.empty()) {
		return;
	}

	const wxString question =
		files.size() == 1 ? wxString(_("Are you sure that you wish to delete the selected file?"))
				  : wxString(_("Are you sure that you wish to delete the selected files?"));
	if (wxMessageBox(question, _("Cancel"), wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT, this) == wxYES) {
		for (wxUIntPtr data : files) {
			CoreNotify_PartFile_Delete(reinterpret_cast<CPartFile *>(data));
		}
	}
}

void CDownloadListCtrl::OnSetPriority(wxCommandEvent &event)
{
	int priority = 0;
	switch (event.GetId()) {
	case MP_PRIOLOW:
		priority = PR_LOW;
		break;
	case MP_PRIONORMAL:
		priority = PR_NORMAL;
		break;
	case MP_PRIOHIGH:
		priority = PR_HIGH;
		break;
	case MP_PRIOAUTO:
		priority = PR_AUTO;
		break;
	default:
		wxFAIL;
	}

	for (wxUIntPtr data : GetSelectedItemData()) {
		CPartFile *file = reinterpret_cast<CPartFile *>(data);
		if (priority == PR_AUTO) {
			CoreNotify_PartFile_PrioAuto(file, true);
		} else {
			CoreNotify_PartFile_PrioAuto(file, false);
			CoreNotify_PartFile_PrioSet(file, priority, true);
		}
	}
}

void CDownloadListCtrl::OnSwapSources(wxCommandEvent &event)
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		CPartFile *file = reinterpret_cast<CPartFile *>(data);
		switch (event.GetId()) {
		case MP_SWAP_A4AF_TO_THIS:
			CoreNotify_PartFile_Swap_A4AF(file);
			break;
		case MP_SWAP_A4AF_TO_THIS_AUTO:
			CoreNotify_PartFile_Swap_A4AF_Auto(file);
			break;
		case MP_SWAP_A4AF_TO_ANY_OTHER:
			CoreNotify_PartFile_Swap_A4AF_Others(file);
			break;
		}
	}
}

void CDownloadListCtrl::OnSetCategory(wxCommandEvent &event)
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		CPartFile *file = reinterpret_cast<CPartFile *>(data);
		CoreNotify_PartFile_SetCat(file, event.GetId() - MP_ASSIGNCAT);
		ShowFile(file, false);
	}
	DoItemSelectionChanged(); // clear clients that may have been shown

	ChangeCategory(m_category); // This only updates the visibility of the clear completed button
	theApp->amuledlg->m_transferwnd->UpdateCatTabTitles();
}

void CDownloadListCtrl::OnSetStatus(wxCommandEvent &event)
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		CPartFile *file = reinterpret_cast<CPartFile *>(data);
		switch (event.GetId()) {
		case MP_PAUSE:
			CoreNotify_PartFile_Pause(file);
			break;
		case MP_RESUME:
			CoreNotify_PartFile_Resume(file);
			break;
		case MP_STOP:
			CoreNotify_PartFile_Stop(file);
			break;
		}
	}
}

void CDownloadListCtrl::OnClearCompleted(wxCommandEvent &WXUNUSED(event))
{
	ClearCompleted();
}

void CDownloadListCtrl::OnGetLink(wxCommandEvent &event)
{
	wxString URIs;
	for (wxUIntPtr data : GetSelectedItemData()) {
		CPartFile *file = reinterpret_cast<CPartFile *>(data);
		if (event.GetId() == MP_GETED2KLINK) {
			URIs += theApp->CreateED2kLink(file) + "\n";
		} else {
			URIs += theApp->CreateMagnetLink(file) + "\n";
		}
	}
	if (!URIs.IsEmpty()) {
		theApp->CopyTextToClipboard(URIs.BeforeLast('\n'));
	}
}

void CDownloadListCtrl::OnGetFeedback(wxCommandEvent &WXUNUSED(event))
{
	wxString feed;
	for (wxUIntPtr data : GetSelectedItemData()) {
		if (feed.IsEmpty()) {
			feed = CFormat(_("Feedback from: %s (%s)\n\n")) % thePrefs::GetUserNick() %
			       theApp->GetFullMuleVersion();
		} else {
			feed += "\n";
		}
		feed += reinterpret_cast<CPartFile *>(data)->GetFeedback();
	}
	if (!feed.IsEmpty()) {
		theApp->CopyTextToClipboard(feed);
	}
}

void CDownloadListCtrl::OnViewFileInfo(wxCommandEvent &WXUNUSED(event))
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (!selected.empty()) {
		ShowFileDetailDialog(RowOfData(selected.front()));
	}
}

void CDownloadListCtrl::OnViewFileComments(wxCommandEvent &WXUNUSED(event))
{
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	if (selected.size() == 1) {
		CCommentDialogLst dialog(this, reinterpret_cast<CPartFile *>(selected.front()));
		dialog.ShowModal();
	}
}

void CDownloadListCtrl::OnPreviewFile(wxCommandEvent &WXUNUSED(event))
{
	// The clicked row, matching how the menu's enabled state was decided.
	// With several rows selected, taking the selection would act on a
	// different file than the one the entry was enabled for.
	if (m_menuItem != 0 && HasItemData(m_menuItem)) {
		FileLaunch::Open(reinterpret_cast<CPartFile *>(m_menuItem), this);
	}
}

void CDownloadListCtrl::OnShowInFolder(wxCommandEvent &WXUNUSED(event))
{
	if (m_menuItem != 0 && HasItemData(m_menuItem)) {
		FileLaunch::Reveal(reinterpret_cast<CPartFile *>(m_menuItem), this);
	}
}

void CDownloadListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	if (!event.GetItem().IsOk()) {
		return;
	}
	// Read the row straight off the event, the way GetRowLabel() does --
	// touching the selection here (as an earlier version of this did) would
	// discard whatever multi-selection the user already had, and firing
	// EVT_DATAVIEW_SELECTION_CHANGED as a side effect of a double-click would
	// rebuild the sources panel for no reason.
	const wxDataViewListModel *model = static_cast<const wxDataViewListModel *>(GetModel());
	const long row = static_cast<long>(model->GetRow(event.GetItem()));
	const wxUIntPtr data = ItemAt(row);
	if (!data) {
		return;
	}
	CPartFile *file = reinterpret_cast<CPartFile *>(data);

	// Double-click is media-only: it is an easy gesture to trigger by
	// accident, and handing a completed .exe or .desktop to the platform
	// opener that way is not a thing to do silently. The menu's Open is the
	// broad one.
	//
	// It does now cover an in-progress download once enough of the media is
	// on disk to play -- PreviewAvailable()'s own test -- where the previous
	// condition also required completion. That is the classic eMule gesture,
	// and it matches what the menu's Preview entry offers for the same row.
	// Anything else opens the file-details modal, as the shared-files table
	// does.
	if (file->PreviewAvailable() && FileLaunch::CanOpen(file)) {
		FileLaunch::Open(file, this);
	} else {
		ShowFileDetailDialog(row);
	}
}

void CDownloadListCtrl::OnSelectionChanged(wxDataViewEvent &WXUNUSED(event))
{
	if (!m_ItemSelectionChangePending) {
		m_ItemSelectionChangePending = true;
		Notify_DownloadCtrlDoItemSelectionChanged();
	}
}

void CDownloadListCtrl::DoItemSelectionChanged()
{
	m_ItemSelectionChangePending = false;
	CKnownFileVector filesVector;
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	filesVector.reserve(selected.size());
	for (wxUIntPtr data : selected) {
		CPartFile *file = reinterpret_cast<CPartFile *>(data);
		if (file->IsPartFile()) {
			filesVector.push_back(file);
		}
	}
	std::sort(filesVector.begin(), filesVector.end());
	theApp->amuledlg->m_transferwnd->clientlistctrl->ShowSources(filesVector);
}

void CDownloadListCtrl::ShowFileDetailDialog(long row)
{
	if (row < 0) {
		return;
	}
	// Make list of part files in control (CFileDetailDialog takes CKnownFile*;
	// a CPartFile upcasts, and the dialog shows download rows from its state).
	std::vector<CKnownFile *> files;
	const long nrItems = ItemDataCount();
	files.reserve(nrItems);
	for (long i = 0; i < nrItems; i++) {
		files.push_back(reinterpret_cast<CPartFile *>(ItemAt(i)));
	}
	CFileDetailDialog(this, files, static_cast<int>(row)).ShowModal();
}

bool CDownloadListCtrl::OnListKey(wxKeyEvent &event)
{
	switch (event.GetKeyCode()) {
	case WXK_NUMPAD_DELETE:
	case WXK_DELETE: {
		wxCommandEvent evt;
		OnCancelFile(evt);
		return true;
	}
	case WXK_F2: {
		const std::vector<wxUIntPtr> selected = GetSelectedItemData();
		if (selected.size() == 1) {
			CPartFile *file = reinterpret_cast<CPartFile *>(selected.front());
			// Currently renaming of completed files causes problem with kad
			if (file->IsPartFile()) {
				wxString strNewName = ::wxGetTextFromUser(_("Enter new name for this file:"),
					_("File rename"),
					file->GetFileName().GetPrintable());
				CPath newName = CPath(strNewName);
				if (newName.IsOk() && (newName != file->GetFileName())) {
					theApp->sharedfiles->RenameFile(file, newName);
				}
			}
		}
		return true;
	}
	default:
		return false;
	}
}

wxString CDownloadListCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	CPartFile *file = reinterpret_cast<CPartFile *>(item);

	switch (column) {
	case COLUMN_DL_PART:
		if (file->IsPartFile() && !file->IsCompleted()) {
			return CFormat("%03d") % file->GetPartMetNumber();
		}
		return wxEmptyString;

	case COLUMN_DL_NAME:
		// The rating/comment smiley is the icon on this column, not part of
		// the text -- see GetItemIcon().
		return file->GetFileName().GetPrintable();

	case COLUMN_DL_SIZE:
		return CastItoXBytes(file->GetFileSize());

	case COLUMN_DL_TRANSFERRED:
		return CastItoXBytes(file->GetTransferred());

	case COLUMN_DL_COMPLETED:
		return CastItoXBytes(file->GetCompletedSize());

	case COLUMN_DL_SPEED:
		if (file->GetTransferingSrcCount()) {
			if (file->GetKBpsDown() >= 1024) {
				return CFormat(_("%.1f MB/s")) % (file->GetKBpsDown() / 1024.0);
			}
			return CFormat(_("%.1f kB/s")) % file->GetKBpsDown();
		}
		return wxEmptyString;

	case COLUMN_DL_SOURCES: {
		const uint16 sc = file->GetSourceCount();
		const uint16 ncsc = file->GetNotCurrentSourcesCount();
		wxString text = ncsc ? CFormat("%i/%i") % (sc - ncsc) % sc : CFormat("%i") % sc;
		if (file->GetSrcA4AFCount()) {
			text += CFormat("+%i") % file->GetSrcA4AFCount();
		}
		if (file->GetTransferingSrcCount()) {
			text += CFormat(" (%i)") % file->GetTransferingSrcCount();
		}
		return text;
	}

	case COLUMN_DL_PRIORITY:
		return PriorityToStr(file->GetDownPriority(), file->IsAutoDownPriority());

	case COLUMN_DL_STATUS:
		return file->getPartfileStatus();

	case COLUMN_DL_TIMEREMAINING: {
		if ((file->GetStatus() != PS_COMPLETING) && file->IsPartFile()) {
			const uint64 remainSize = file->GetFileSize() - file->GetCompletedSize();
			const sint32 remainTime = file->getTimeRemaining();
			wxString text =
				(remainTime >= 0) ? CastSecondsToHM(remainTime) : wxString(_("Unknown"));
			text += " (" + CastItoXBytes(remainSize) + ")";
			return text;
		}
		return wxEmptyString;
	}

	case COLUMN_DL_LASTSEENCOMPLETE:
		if (file->lastseencomplete) {
			return wxDateTime(file->lastseencomplete).Format(_("%y/%m/%d %H:%M:%S"));
		}
		return _("Unknown");

	case COLUMN_DL_LASTRECEPTION: {
		const time_t lastReceived = file->GetLastChangeDatetime();
		if (lastReceived) {
			return wxDateTime(lastReceived).Format(_("%y/%m/%d %H:%M:%S"));
		}
		return _("Unknown");
	}

	default:
		return wxEmptyString;
	}
}

bool CDownloadListCtrl::GetItemIcon(wxUIntPtr item, unsigned column, wxIcon &icon) const
{
	if (column != COLUMN_DL_NAME) {
		return false;
	}
	CPartFile *file = reinterpret_cast<CPartFile *>(item);
	if (!file->HasRating() && !file->HasComment()) {
		return false;
	}
	int image = Client_CommentOnly_Smiley;
	if (file->HasRating()) {
		image = Client_InvalidRating_Smiley + file->UserRating() - 1;
	}
	wxASSERT(image >= Client_InvalidRating_Smiley);
	wxASSERT(image <= Client_CommentOnly_Smiley);
	icon = theApp->amuledlg->m_imagelist.GetIcon(image);
	return true;
}

void CDownloadListCtrl::GetItemBarFill(wxUIntPtr item, unsigned column, CBarFillSpec &out) const
{
	if (column != COLUMN_DL_PROGRESS || !thePrefs::ShowProgBar()) {
		return;
	}
	CPartFile *file = reinterpret_cast<CPartFile *>(item);
	if (file->GetFileSize() == 0) {
		return;
	}

	std::vector<CBarFillSpan> spans;
	const bool bFlat = thePrefs::UseFlatBar();

	if (file->IsCompleted() || file->GetStatus() == PS_COMPLETING) {
		spans.push_back({ 0, file->GetFileSize() - 1, bFlat ? crFlatProgress : crProgress });
		out = CBarFillSpec(item, file->GetFileSize(), std::move(spans));
		return;
	}

	if (file->GetHashingProgress() > 0) {
		uint64 left = file->GetHashingProgress() * PARTSIZE;
		if (left < file->GetFileSize() - 1) {
			spans.push_back(
				{ left + 1, file->GetFileSize() - 1, bFlat ? crFlatPending : crPending });
		} else {
			left = file->GetFileSize() - 1;
		}
		spans.push_back({ 0, left, bFlat ? crFlatProgress : crProgress });
		out = CBarFillSpec(item, file->GetFileSize(), std::move(spans));
		return;
	}

	// Part availability (of missing parts).
	const CGapList &gaplist = file->GetGapList();
	uint64 lastGapEnd = 0;
	for (CGapList::const_iterator it = gaplist.begin(); it != gaplist.end(); ++it) {
		const uint64 start = it.start() / PARTSIZE;
		if (it.start()) {
			spans.push_back({ lastGapEnd + 1, it.start() - 1, bFlat ? crFlatHave : crHave });
		}
		lastGapEnd = it.end();
		uint64 end = (it.end() / PARTSIZE) + 1;
		if (end > file->GetPartCount()) {
			end = file->GetPartCount();
		}

		// Place each gap, one PART at a time.
		for (uint64 i = start; i < end; ++i) {
			CMuleColour colour;
			if (i < file->m_SrcpartFrequency.size() && file->m_SrcpartFrequency[i]) {
				const int blue = 210 - (22 * (file->m_SrcpartFrequency[i] - 1));
				colour.Set(0, static_cast<uint8_t>(blue < 0 ? 0 : blue), 255);
			} else {
				colour = crMissing;
			}
			if (file->IsStopped()) {
				colour.Blend(50);
			}
			const uint64 gap_begin = (i == start ? it.start() : PARTSIZE * i);
			const uint64 gap_end = (i == end - 1 ? it.end() : PARTSIZE * (i + 1) - 1);
			spans.push_back({ gap_begin, gap_end, colour });
		}
	}
	// Fill the last Have-part (between this gap and the last).
	spans.push_back({ lastGapEnd + 1, file->GetFileSize() - 1, bFlat ? crFlatHave : crHave });

	// Pending parts. Adjacent pending parts must be joined to avoid bright
	// lines between them.
	const CPartFile::CReqBlockPtrList requestedBlocks = file->GetRequestedBlockList();
	uint64 lastStartOffset = 0;
	uint64 lastEndOffset = 0;
	CMuleColour pendingColour = bFlat ? crFlatPending : crPending;
	if (file->IsStopped()) {
		pendingColour.Blend(50);
	}
	for (const Requested_Block_Struct *block : requestedBlocks) {
		if (block->StartOffset > lastEndOffset + 1) {
			spans.push_back({ lastStartOffset, lastEndOffset, pendingColour });
			lastStartOffset = block->StartOffset;
			lastEndOffset = block->EndOffset;
		} else {
			lastEndOffset = block->EndOffset;
		}
	}
	spans.push_back({ lastStartOffset, lastEndOffset, pendingColour });

	out = CBarFillSpec(item, file->GetFileSize(), std::move(spans));
}

bool CDownloadListCtrl::IsLiveSortColumn() const
{
	if (m_sort_orders.empty()) {
		return false;
	}
	switch (static_cast<int>(m_sort_orders.front().first)) {
	case COLUMN_DL_TRANSFERRED:
	case COLUMN_DL_COMPLETED:
	case COLUMN_DL_SPEED:
	case COLUMN_DL_PROGRESS:
	case COLUMN_DL_SOURCES:
	case COLUMN_DL_TIMEREMAINING:
	case COLUMN_DL_LASTSEENCOMPLETE:
	case COLUMN_DL_LASTRECEPTION:
		return true;
	default:
		return false;
	}
}

int CDownloadListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	const CPartFile *file1 = reinterpret_cast<const CPartFile *>(data1);
	const CPartFile *file2 = reinterpret_cast<const CPartFile *>(data2);
	const int mod = modifier;

	switch (column) {
	case COLUMN_DL_PART:
		return mod * CmpAny(file1->GetPartMetNumber(), file2->GetPartMetNumber());

	case COLUMN_DL_NAME:
		return mod * CmpAny(file1->GetFileName(), file2->GetFileName());

	case COLUMN_DL_SIZE:
		return mod * CmpAny(file1->GetFileSize(), file2->GetFileSize());

	case COLUMN_DL_TRANSFERRED:
		return mod * CmpAny(file1->GetTransferred(), file2->GetTransferred());

	case COLUMN_DL_COMPLETED:
		return mod * CmpAny(file1->GetCompletedSize(), file2->GetCompletedSize());

	case COLUMN_DL_SPEED:
		return mod * CmpAny(file1->GetKBpsDown(), file2->GetKBpsDown());

	case COLUMN_DL_PROGRESS:
		return mod * CmpAny(file1->GetPercentCompleted(), file2->GetPercentCompleted());

	case COLUMN_DL_SOURCES:
		return mod * CmpAny(file1->GetSourceCount(), file2->GetSourceCount());

	case COLUMN_DL_PRIORITY:
		return mod * CmpAny(file1->GetDownPriority(), file2->GetDownPriority());

	case COLUMN_DL_STATUS:
		return mod * CmpAny(file1->getPartfileStatusRang(), file2->getPartfileStatusRang());

	case COLUMN_DL_TIMEREMAINING:
		// -1 ("unknown") always sorts last, regardless of direction -- `mod`
		// is deliberately not applied to this branch.
		if (file1->getTimeRemaining() == -1) {
			return (file2->getTimeRemaining() == -1) ? 0 : 1;
		}
		if (file2->getTimeRemaining() == -1) {
			return -1;
		}
		return mod * CmpAny(file1->getTimeRemaining(), file2->getTimeRemaining());

	case COLUMN_DL_LASTSEENCOMPLETE:
		return mod * CmpAny(file1->lastseencomplete, file2->lastseencomplete);

	case COLUMN_DL_LASTRECEPTION:
		return mod * CmpAny(file1->GetLastChangeDatetime(), file2->GetLastChangeDatetime());

	default:
		return 0;
	}
}

void CDownloadListCtrl::ClearCompleted()
{
	CastByID(ID_BTNCLRCOMPL, GetParent(), wxButton)->Enable(false);

	ListOfUInts32 toClear;
	for (CPartFile *file : m_files) {
		if (file->IsCompleted() && file->CheckShowItemInGivenCat(m_category)) {
			toClear.push_back(file->ECID());
		}
	}
	if (!toClear.empty()) {
		theApp->downloadqueue->ClearCompleted(toClear);
	}
}

void CDownloadListCtrl::ShowFilesCount(int diff)
{
	SetFilesCount(m_filecount + diff);
}

void CDownloadListCtrl::SetFilesCount(int count)
{
	m_filecount = count;

	wxStaticText *label = CastByName("downloadsLabel", GetParent(), wxStaticText);
	label->SetLabel(CFormat(_("Downloads (%i)")) % m_filecount);
	label->GetParent()->Layout();
}

void CDownloadListCtrl::SetTotalSize(uint64 total)
{
	m_shownSize = total;

	// This label lives in the sources pane (the bottom of the transfer
	// splitter), a different window from the download list, so reach it via
	// the transfer window rather than GetParent(). Guard the early calls
	// before the transfer window is fully constructed.
	if (!theApp->amuledlg || !theApp->amuledlg->m_transferwnd) {
		return;
	}
	wxStaticText *label = CastByName("downloadsTotalSize", theApp->amuledlg->m_transferwnd, wxStaticText);
	if (label) {
		label->SetLabel(CFormat(_("Total queue size: %s")) % CastItoXBytes(m_shownSize));
		label->GetParent()->Layout();
	}
}

void CDownloadListCtrl::UpdateFreeSpace()
{
	if (!theApp->amuledlg || !theApp->amuledlg->m_transferwnd) {
		return;
	}
	// Resolved once: this runs on the GUI timer, and the label outlives
	// every call -- it belongs to the transfer window, which is built with
	// the main dialog and torn down with it.
	if (!m_freeSpaceLabel) {
		m_freeSpaceLabel =
			CastByName("downloadsFreeSpace", theApp->amuledlg->m_transferwnd, wxStaticText);
		if (!m_freeSpaceLabel) {
			return;
		}
	}

	const sint64 freeSpace = theStats::GetTempFreeSpace();

	// What the queue still has to write, over every category. Completed
	// files are already out of temp, and the bytes a part file holds are
	// already off the free-space figure, so what is left to download is
	// exactly what the filesystem still has to find room for. Paused and
	// stopped files count too: they are space the queue will need as soon
	// as they resume.
	//
	// Skipped when there is no figure to compare against: without one there
	// is nothing to warn about, and this walks the whole queue.
	uint64 remaining = 0;
	if (freeSpace != FREE_SPACE_UNKNOWN) {
		for (CPartFile *file : m_files) {
			if (file->IsCompleted()) {
				continue;
			}
			const uint64 size = file->GetFileSize();
			const uint64 done = file->GetCompletedSize();
			if (done < size) {
				remaining += size - done;
			}
		}
	}

	const bool warn = (freeSpace != FREE_SPACE_UNKNOWN) && (static_cast<uint64>(freeSpace) < remaining);
	// Continues the "Total queue size:" label to its left.
	CamuleDlg::SetFreeSpaceLabel(m_freeSpaceLabel, freeSpace, warn, " | ");
}

// File_checked_for_headers
