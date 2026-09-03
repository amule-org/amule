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
#include "GenericClientListCtrl.h"

#include <protocol/ed2k/ClientSoftware.h>
#include <common/MenuIDs.h>

#include <common/Format.h> // Needed for CFormat
#include "amule.h"         // Needed for theApp
#include "amuleDlg.h"      // Needed for CamuleDlg
#include "SearchDlg.h"     // Needed for CSearchDlg (View Files browse tab)
#include "BarShader.h"     // Needed for CBarShader
#include "BitVector.h"
#include "ClientDetailDialog.h"   // Needed for CClientDetailDialog
#include "ClientContextActions.h" // Needed for BuildClientContextMenu, ClientAction*
#include "ClientNameCell.h"       // Needed for MakeClientNameCell, DrawClientNameCell
#include "ChatWnd.h"              // Needed for CChatWnd
#include "CommentDialogLst.h"     // Needed for CCommentDialogLst
#include "DataToText.h"           // Needed for PriorityToStr
#include "FileDetailDialog.h"     // Needed for CFileDetailDialog
#include "GuiEvents.h"            // Needed for CoreNotify_*
#include "InfoGridDialog.h"       // Needed for ShowInfoGridDialog
#ifdef GEOIP_GUI
#include "CountryFlags.h"   // Needed for CCountryFlags (flag bitmaps)
#include "CountryDisplay.h" // Needed for GetDisplayCountryCode
#endif
#include "MuleBarRenderer.h" // Needed for CBarFillSpec, CBarFillSpan, CMuleBarRenderer
#include "MuleColour.h"      // Needed for IsListBackgroundDark
#include "muuli_wdr.h"       // Needed for ID_CLIENTCOUNT
#include "PartFile.h"        // Needed for CPartFile
#include "Preferences.h"
#include "SharedFileList.h" // Needed for CSharedFileList
#include "ClientRef.h"      // Needed for CClientRef
// CUpDownClient (country accessors, #439). MUST match the build's client class:
// the reduced EC client for amulegui, the full one for monolithic. Including the
// wrong header gives this TU a different CUpDownClient layout than the rest of
// the (remote) GUI, so member reads land at the wrong offset (garbage country).
#ifdef CLIENT_GUI
#include "UpDownClientEC.h"
#else
#include "updownclient.h"
#endif
#include "FriendList.h"

#include <wx/dcmemory.h> // Needed for wxMemoryDC (legend swatches)
#include <wx/dialog.h>   // Needed for wxDialog (legend)
#include <wx/sizer.h>    // Needed for wxBoxSizer, wxFlexGridSizer
#include <wx/statbmp.h>  // Needed for wxStaticBitmap
#include <wx/stattext.h> // Needed for wxStaticText

namespace
{
/**
 * Renders the User Name column.
 *
 * Not a literal bar -- this reuses CMuleBarRenderer's identity-carrying
 * CBarFillSpec/GetItemBarFill() extension point (see CBarFillSpec::GetIdentity())
 * to reach the row's ClientCtrlItem_Struct, the same way CDownloadBarRenderer
 * reaches its CPartFile*. Registered via AddBarColumn() rather than growing a
 * new column-registration entry point for a single, list-local column.
 *
 * The drawing itself lives in DrawClientNameCell(), shared with the global
 * clients list so the two cannot drift apart.
 */
class CClientNameRenderer : public CMuleBarRenderer
{
public:
	bool Render(wxRect cell, wxDC *dc, int WXUNUSED(state)) override
	{
		ClientCtrlItem_Struct *item =
			reinterpret_cast<ClientCtrlItem_Struct *>(GetSpec().GetIdentity());
		if (item == nullptr) {
			return true;
		}
		// Read straight off the live CClientRef: this list holds an owning
		// reference to every peer it shows, so the client is alive for as
		// long as the row is.
		DrawClientNameCell(
			MakeClientNameCell(item->GetSource().GetClient(), item->GetType() == A4AF_SOURCE),
			cell,
			dc);
		return true;
	}
};

/**
 * A 16x16 swatch of one bar colour, drawn the way CCatDialog::MakeBitmap()
 * draws the category colour: a wxMemoryDC over a wxBitmap, default pen, so the
 * fill keeps a border and the two pale greys stay visible on a light dialog.
 */
wxBitmap MakeLegendSwatch(const partbar::BarColour &colour)
{
	wxBitmap bitmap(16, 16);
	wxMemoryDC dc(bitmap);

	dc.SetBrush(CMuleColour(colour.red, colour.green, colour.blue).GetBrush());
	dc.DrawRectangle(0, 0, 16, 16);

	return bitmap;
}

wxString SourcePartStateLabel(partbar::SourcePartState state)
{
	switch (state) {
	case partbar::SourcePartState::Missing:
		return _("This source does not have the part");
	case partbar::SourcePartState::Complete:
		return _("You and this source both have the part");
	case partbar::SourcePartState::Downloading:
		return _("Being downloaded from this source now");
	case partbar::SourcePartState::NextRequested:
		return _("The next part that will be asked of this source");
	case partbar::SourcePartState::Needed:
		return _("This source has the part and you still need it");
	}
	return wxEmptyString;
}

wxString PeerPartStateLabel(partbar::PeerPartState state)
{
	switch (state) {
	case partbar::PeerPartState::Present:
		return _("This peer already has the part");
	case partbar::PeerPartState::Missing:
		return _("This peer does not have the part");
	}
	return wxEmptyString;
}

//! One swatch-and-text row of a legend.
void AddLegendRow(wxWindow *parent, wxSizer *grid, const partbar::BarColour &colour, const wxString &label)
{
	grid->Add(new wxStaticBitmap(parent, wxID_ANY, MakeLegendSwatch(colour)), 0, wxALIGN_CENTRE_VERTICAL);
	grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTRE_VERTICAL);
}
} // namespace

#define m_ImageList theApp->amuledlg->m_imagelist

wxBEGIN_EVENT_TABLE(CGenericClientListCtrl, CMuleVirtualDataViewCtrl)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CGenericClientListCtrl::OnItemActivated)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CGenericClientListCtrl::OnItemRightClicked)
	EVT_MIDDLE_DOWN(CGenericClientListCtrl::OnMouseMiddleClick)

	EVT_MENU(MP_CHANGE2FILE, CGenericClientListCtrl::OnSwapSource)
	EVT_MENU(MP_SHOWLIST, CGenericClientListCtrl::OnViewFiles)
	EVT_MENU(MP_ADDFRIEND, CGenericClientListCtrl::OnAddFriend)
	EVT_MENU(MP_FRIENDSLOT, CGenericClientListCtrl::OnSetFriendslot)
	EVT_MENU(MP_SENDMESSAGE, CGenericClientListCtrl::OnSendMessage)
	EVT_MENU(MP_DETAIL, CGenericClientListCtrl::OnViewClientInfo)
	EVT_MENU(MP_BARLEGEND, CGenericClientListCtrl::OnShowBarLegend)
wxEND_EVENT_TABLE()

CGenericClientListCtrl::CGenericClientListCtrl(const wxString &tablename,
	wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: CMuleVirtualDataViewCtrl(parent, winid, pos, size, style, name)
, m_columndata(0, nullptr)
, m_menu(nullptr)
, m_clientcount(0)
, m_showing(false)
{
	m_columnStore.SetTableName(tablename);
}

wxString CGenericClientListCtrl::TranslateCIDToName(GenericColumnEnum cid)
{
	wxString name;

	switch (cid) {
	case ColumnUserName:
		name = "N";
		break;
	case ColumnUserDownloaded:
		name = "D";
		break;
	case ColumnUserUploaded:
		name = "U";
		break;
	case ColumnUserSpeedDown:
		name = "S";
		break;
	case ColumnUserSpeedUp:
		name = "s";
		break;
	case ColumnUserProgress:
		name = "P";
		break;
	case ColumnUserAvailable:
		name = "A";
		break;
	case ColumnUserVersion:
		name = "V";
		break;
	case ColumnUserQueueRankLocal:
		name = "Q";
		break;
	case ColumnUserQueueRankRemote:
		name = "q";
		break;
	case ColumnUserOrigin:
		name = "O";
		break;
	case ColumnUserFileNameDownload:
		name = "F";
		break;
	case ColumnUserFileNameUpload:
		name = "f";
		break;
	case ColumnUserFileNameDownloadRemote:
		name = "R";
		break;
	case ColumnUserSharedFiles:
		name = "m";
		break;
	case ColumnInvalid:
	default:
		wxFAIL;
		break;
	}

	return name;
}

bool CGenericClientListCtrl::IsLiveSortColumn() const
{
	if (m_sort_orders.empty()) {
		return false;
	}
	const int col = static_cast<int>(m_sort_orders.front().first);
	if (col < 0 || col >= m_columndata.n_columns) {
		return false;
	}
	switch (m_columndata.columns[col].cid) {
	case ColumnUserDownloaded:
	case ColumnUserUploaded:
	case ColumnUserSpeedDown:
	case ColumnUserSpeedUp:
	case ColumnUserProgress:
	case ColumnUserAvailable:
	case ColumnUserQueueRankLocal:
	case ColumnUserQueueRankRemote:
		return true;
	default:
		return false;
	}
}

void CGenericClientListCtrl::InitColumnData()
{
	if (!m_columndata.n_columns) {
		throw wxString("CRITICAL: Initialization of the column data lacks subclass information");
	}

	const int baseFlags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;

	for (int i = 0; i < m_columndata.n_columns; ++i) {
		const GenericColumnEnum cid = m_columndata.columns[i].cid;
		const wxString title = wxGetTranslation(m_columndata.columns[i].title);
		const wxString key = TranslateCIDToName(cid);
		const int width = m_columndata.columns[i].width;

		switch (cid) {
		case ColumnUserName:
			// See CClientNameRenderer: a composite icon-cluster + text cell,
			// not a literal bar.
			AddBarColumn(title, i, key, width, baseFlags, new CClientNameRenderer());
			break;
		case ColumnUserProgress:
			AddBarColumn(title, i, key, width, baseFlags, CreateProgressBarRenderer());
			break;
		case ColumnUserAvailable:
			// No Compare() case exists for this column (matches the
			// pre-port behaviour, where it was never sortable either) --
			// SORTABLE is deliberately left off so no non-functional sort
			// caret is shown.
			AddBarColumn(title, i, key, width, wxDATAVIEW_COL_RESIZABLE, nullptr);
			break;
		default:
			AddTextColumn(title, i, key, width, wxALIGN_LEFT, baseFlags);
			break;
		}
	}

	// Absorbs the macOS trailing-column sizing, which would otherwise fall on
	// the last real column and collapse it once the columns are wider than the
	// control. n_columns is the id to use: the model answers every column past
	// its own table with an empty value already.
	AppendSpacerColumn(m_columndata.n_columns);

	AssociateVirtualModel();
	ApplySorting(0, 0);

	LoadColumnSettings();
	InitColumnState();
}

CGenericClientListCtrl::~CGenericClientListCtrl()
{
	delete m_menu;
	while (!m_ListItems.empty()) {
		delete m_ListItems.begin()->second;
		m_ListItems.erase(m_ListItems.begin());
	}
}

void CGenericClientListCtrl::RawAddSource(CKnownFile *owner, CClientRef source, SourceItemType type)
{
	ClientCtrlItem_Struct *newitem = new ClientCtrlItem_Struct;
	newitem->SetContents(owner, source, type);

	m_ListItems.insert(ListItemsPair(source.ECID(), newitem));

	// Append at the end (unsorted); ShowSources() sorts once at the end of a
	// bulk add, a single runtime add just shows at the bottom -- same as the
	// old InsertItem(GetItemCount()) behaviour.
	AppendItemDataNow(reinterpret_cast<wxUIntPtr>(newitem));
}

void CGenericClientListCtrl::AddSource(CKnownFile *owner, const CClientRef &source, SourceItemType type)
{
	wxCHECK_RET(owner, "NULL owner in CGenericClientListCtrl::AddSource");
	wxCHECK_RET(source.IsLinked(), "Unlinked source in CGenericClientListCtrl::AddSource");

	// Update the other instances of this source
	bool bFound = false;
	ListIteratorPair rangeIt = m_ListItems.equal_range(source.ECID());
	for (ListItems::iterator it = rangeIt.first; it != rangeIt.second; ++it) {
		ClientCtrlItem_Struct *cur_item = it->second;

		// Check if this source has been already added to this file => to be sure
		if (cur_item->GetOwner() == owner) {
			// Update this instance with its new setting
			if ((type == A4AF_SOURCE) && cur_item->GetSource().GetRequestFile() &&
				std::binary_search(m_knownfiles.begin(),
					m_knownfiles.end(),
					cur_item->GetSource().GetRequestFile())) {
				cur_item->SetContents(owner, source, AVAILABLE_SOURCE);
			} else {
				cur_item->SetContents(owner, source, type);
			}
			bFound = true;
		} else if (type == AVAILABLE_SOURCE) {
			// The state 'Available' is exclusive
			cur_item->SetContents(cur_item->GetOwner(), source, A4AF_SOURCE);
		}
	}

	if (bFound) {
		return;
	}

	if (std::binary_search(m_knownfiles.begin(), m_knownfiles.end(), owner)) {
		RawAddSource(owner, source, type);

		ShowSourcesCount(1);
	}
}

void CGenericClientListCtrl::RawRemoveSource(ListItems::iterator &it)
{
	ClientCtrlItem_Struct *item = it->second;

	RemoveItemData(reinterpret_cast<wxUIntPtr>(item));

	delete item;

	// Remove it from the m_ListItems
	m_ListItems.erase(it);
}

void CGenericClientListCtrl::RemoveSource(uint32 source, const CKnownFile *owner)
{
	// A NULL owner means remove it no matter what.
	wxCHECK_RET(source, "NULL source in CGenericClientListCtrl::RemoveSource");

	// Retrieve all entries matching the source
	ListIteratorPair rangeIt = m_ListItems.equal_range(source);

	int removedItems = 0;

	for (ListItems::iterator it = rangeIt.first; it != rangeIt.second; /* no ++, it happens later */) {
		ListItems::iterator tmp = it++;

		if (owner == nullptr || owner == tmp->second->GetOwner()) {

			RawRemoveSource(tmp);

			++removedItems;
		}
	}

	ShowSourcesCount((-1) * removedItems);
}

void CGenericClientListCtrl::UpdateItem(uint32 toupdate, SourceItemType type)
{
	// Retrieve all entries matching the source
	ListIteratorPair rangeIt = m_ListItems.equal_range(toupdate);

	if (rangeIt.first != rangeIt.second) {
		for (ListItems::iterator it = rangeIt.first; it != rangeIt.second; ++it) {
			ClientCtrlItem_Struct *item = it->second;

			if ((type == A4AF_SOURCE) && item->GetSource().GetRequestFile() &&
				std::binary_search(m_knownfiles.begin(),
					m_knownfiles.end(),
					item->GetSource().GetRequestFile())) {

				item->SetType(AVAILABLE_SOURCE);
			} else {
				item->SetType(type);
			}

			// Repaints the row and, if sorted by a live column, schedules
			// the throttled+idle-gated re-sort.
			RefreshItemData(reinterpret_cast<wxUIntPtr>(item));
		}
	}
}

void CGenericClientListCtrl::ShowSources(const CKnownFileVector &files)
{
	Freeze();

	// The stored vector is sorted, as is the received one, so we can use binary_search

	for (unsigned i = 0; i < m_knownfiles.size(); ++i) {
		// Files that are not in the new list must have show set to false.
		if (!std::binary_search(files.begin(), files.end(), m_knownfiles[i])) {
			SetShowSources(m_knownfiles[i], false);
		}
	}

	// This will call again SetShowSources in files that were in both vectors. Right now
	// that function is just a inline setter, so any way to prevent it would be wasteful,
	// but this must be reviewed if that fact changes.

	for (unsigned i = 0; i < files.size(); ++i) {
		SetShowSources(files[i], true);
	}

	// We must cleanup sources that are not in the received files.

	int itemDiff = 0;

	for (ListItems::iterator it = m_ListItems.begin(); it != m_ListItems.end();
		/* no ++, it happens later */) {
		ListItems::iterator tmp = it++;
		ClientCtrlItem_Struct *item = tmp->second;
		if (!std::binary_search(files.begin(), files.end(), item->GetOwner())) {
			// Remove it from the m_ListItems
			RawRemoveSource(tmp);
			--itemDiff;
		}
	}

	for (unsigned i = 0; i < files.size(); ++i) {

		// Only those that weren't showing already
		if (!std::binary_search(m_knownfiles.begin(), m_knownfiles.end(), files[i])) {

			CKnownFile *file = files[i];

			wxASSERT_MSG(file, "NULL file in CGenericClientListCtrl::ShowSources");

			if (file) {

				CKnownFile::SourceSet::const_iterator it;

				if (IsShowingDownloadSources()) {
					const CKnownFile::SourceSet &normSources =
						(dynamic_cast<CPartFile *>(file))->GetSourceList();
					const CKnownFile::SourceSet &a4afSources =
						(dynamic_cast<CPartFile *>(file))->GetA4AFList();

					// Adding normal sources
					for (it = normSources.begin(); it != normSources.end(); ++it) {
						switch (it->GetDownloadState()) {
						case DS_DOWNLOADING:
						case DS_ONQUEUE:
							RawAddSource(file, *it, AVAILABLE_SOURCE);
							++itemDiff;
							break;
						default:
							// Any other state
							RawAddSource(file, *it, UNAVAILABLE_SOURCE);
							++itemDiff;
						}
					}

					// Adding A4AF sources
					for (it = a4afSources.begin(); it != a4afSources.end(); ++it) {
						// Only add if the A4AF file is not in the shown list.
						if (!std::binary_search(files.begin(),
							    files.end(),
							    it->GetRequestFile())) {
							RawAddSource(file, *it, A4AF_SOURCE);
							++itemDiff;
						}
					}
				} else {
					// Known file
					const CKnownFile::SourceSet &sources = file->m_ClientUploadList;
					for (it = sources.begin(); it != sources.end(); ++it) {
						switch (it->GetUploadState()) {
						case US_UPLOADING:
						case US_ONUPLOADQUEUE:
							RawAddSource(file, *it, AVAILABLE_SOURCE);
							++itemDiff;
							break;
						default:
							// Any other state
							RawAddSource(file, *it, UNAVAILABLE_SOURCE);
							++itemDiff;
						}
					}
				}
			}
		}
	}

	m_knownfiles = files;

	ShowSourcesCount(itemDiff);

	SortList();

	Thaw();
}

void CGenericClientListCtrl::RemoveKnownFile(CKnownFile *file)
{
	// Pure pointer-value comparison — `file` may already be freed by
	// the destruction site that fired Notify_KnownFileBeingDestroyed.
	// We must never dereference it; we only need its value as a key
	// to drop from m_knownfiles and m_ListItems. See
	// MuleNotify::KnownFileBeingDestroyed in GuiEvents.cpp.
	if (file == nullptr) {
		return;
	}

	// Drop the cached "currently showing sources for" entry. This is
	// #755's crash site: without this, the next ShowSources() loop
	// would walk the dangling entry and write into the recycled heap
	// region via SetShowSources(file, false).
	CKnownFileVector::iterator kf = std::find(m_knownfiles.begin(), m_knownfiles.end(), file);
	if (kf != m_knownfiles.end()) {
		m_knownfiles.erase(kf);
	}

	// Strip any per-row state whose m_owner matches. We have to walk
	// the multimap once because m_ListItems is keyed by client ECID,
	// not by file.
	for (ListItems::iterator it = m_ListItems.begin(); it != m_ListItems.end(); /* manual ++ */) {
		ClientCtrlItem_Struct *item = it->second;
		if (item && item->GetOwner() == file) {
			// Drop the model row first while the multimap entry is
			// still valid, then erase the multimap entry.
			RemoveItemData(reinterpret_cast<wxUIntPtr>(item));
			delete item;
			m_ListItems.erase(it++);
		} else {
			++it;
		}
	}
}

void CGenericClientListCtrl::ShowSourcesCount(int diff)
{
	m_clientcount += diff;
	wxStaticText *label = CastByID(ID_CLIENTCOUNT, GetParent(), wxStaticText);

	if (label) {
		label->SetLabel(CFormat("%i") % m_clientcount);
		label->GetParent()->Layout();
	}
}

void CGenericClientListCtrl::OnSwapSource(wxCommandEvent &WXUNUSED(event))
{
	for (wxUIntPtr data : GetSelectedItemData()) {
		ClientCtrlItem_Struct *source = reinterpret_cast<ClientCtrlItem_Struct *>(data);
		CKnownFile *kf = source->GetOwner();
		if (!kf->IsPartFile()) {
			wxFAIL_MSG("File is not a partfile when swapping sources");
			continue;
		}
		source->GetSource().SwapToAnotherFile(true, false, false, dynamic_cast<CPartFile *>(kf));
	}
}

namespace
{
//! The peers behind the current selection, as the shared actions want them.
std::vector<CClientRef> SelectedClients(const std::vector<wxUIntPtr> &selected)
{
	std::vector<CClientRef> clients;
	clients.reserve(selected.size());
	for (wxUIntPtr data : selected) {
		clients.push_back(reinterpret_cast<ClientCtrlItem_Struct *>(data)->GetSource());
	}
	return clients;
}
} // namespace

void CGenericClientListCtrl::OnViewFiles(wxCommandEvent &WXUNUSED(event))
{
	ClientActionViewFiles(SelectedClients(GetSelectedItemData()));
}

void CGenericClientListCtrl::OnAddFriend(wxCommandEvent &WXUNUSED(event))
{
	ClientActionToggleFriend(SelectedClients(GetSelectedItemData()));
}

void CGenericClientListCtrl::OnSetFriendslot(wxCommandEvent &evt)
{
	ClientActionSetFriendSlot(this, SelectedClients(GetSelectedItemData()), evt.IsChecked());
}

void CGenericClientListCtrl::OnSendMessage(wxCommandEvent &WXUNUSED(event))
{
	ClientActionSendMessage(SelectedClients(GetSelectedItemData()));
}

void CGenericClientListCtrl::OnViewClientInfo(wxCommandEvent &WXUNUSED(event))
{
	ClientActionShowDetails(this, SelectedClients(GetSelectedItemData()));
}

void CGenericClientListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	if (!event.GetItem().IsOk()) {
		return;
	}
	const wxUIntPtr data = ItemAt(GetModelRow(event.GetItem()));
	if (!data) {
		return;
	}
	CClientDetailDialog(this, reinterpret_cast<ClientCtrlItem_Struct *>(data)->GetSource()).ShowModal();
}

void CGenericClientListCtrl::OnMouseMiddleClick(wxMouseEvent &event)
{
	wxDataViewItem hitItem;
	wxDataViewColumn *hitColumn = nullptr;
	HitTest(event.GetPosition(), hitItem, hitColumn);
	if (!hitItem.IsOk()) {
		event.Skip();
		return;
	}

	wxDataViewItemArray selection;
	GetSelections(selection);
	if (selection.Index(hitItem) == wxNOT_FOUND) {
		UnselectAll();
		Select(hitItem);
	}

	const wxUIntPtr data = ItemAt(GetModelRow(hitItem));
	if (!data) {
		return;
	}
	CClientDetailDialog(this, reinterpret_cast<ClientCtrlItem_Struct *>(data)->GetSource()).ShowModal();
}

int CGenericClientListCtrl::FindBarLegendColumn() const
{
	for (int i = 0; i < m_columndata.n_columns; ++i) {
		if (partbar::LegendForColumn(m_columndata.columns[i].cid) == partbar::BarLegendKind::None) {
			continue;
		}
		// A bar the user has hidden is not on screen to be explained. Model
		// id and view position coincide here -- InitColumnState() requires
		// it -- so one index answers both.
		if (!IsColumnHidden(i)) {
			return i;
		}
	}
	return -1;
}

void CGenericClientListCtrl::ShowBarLegend(partbar::BarLegendKind kind, const wxString &columnTitle)
{
	// Both legends read their colours from the functions GetItemBarFill()
	// fills the bar from, under the bar preference in force right now: a
	// swatch cannot disagree with the pixels it explains.
	const bool bFlat = thePrefs::UseFlatBar();
	const bool sourceKind = (kind == partbar::BarLegendKind::SourceParts);

	ShowInfoGridDialog(this,
		columnTitle,
		sourceKind ? _("One block per part of the file being downloaded.")
			   : _("One block per part of the shared file."),
		[bFlat, sourceKind](wxWindow *dlg, wxSizer *grid) {
			if (sourceKind) {
				for (const partbar::SourcePartState state : partbar::kSourceLegendOrder) {
					AddLegendRow(dlg,
						grid,
						partbar::SourcePartColour(state, bFlat),
						SourcePartStateLabel(state));
				}
			} else {
				for (const partbar::PeerPartState state : partbar::kPeerLegendOrder) {
					AddLegendRow(dlg,
						grid,
						partbar::PeerPartColour(state, bFlat),
						PeerPartStateLabel(state));
				}
			}
		});
}

void CGenericClientListCtrl::OnShowBarLegend(wxCommandEvent &WXUNUSED(event))
{
	// Resolved again rather than remembered from when the menu was built: the
	// entry only exists on menus built while a bar column was on screen, and
	// re-asking costs a walk over at most a couple of dozen columns.
	const int column = FindBarLegendColumn();
	if (column < 0) {
		return;
	}
	ShowBarLegend(partbar::LegendForColumn(m_columndata.columns[column].cid),
		wxGetTranslation(m_columndata.columns[column].title));
}

void CGenericClientListCtrl::OnItemRightClicked(wxDataViewEvent &event)
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
	m_menuItem = selected.front();
	ClientCtrlItem_Struct *item = reinterpret_cast<ClientCtrlItem_Struct *>(m_menuItem);
	CClientRef &client = item->GetSource();

	delete m_menu;
	// Same menu the global clients list offers.
	m_menu = BuildClientContextMenu(client);

	// Both of these are appended here rather than inside
	// BuildClientContextMenu(): that builder is shared with CClientRowListCtrl,
	// the Clients tab, which has no file in context and draws no chunk bar, so
	// neither entry can mean anything there.
	//
	// Swapping is a download notion: it moves a source off whatever it is
	// currently downloading and onto this file, and A4AF only means anything
	// among a download's sources. The shared-files peer list shows clients
	// downloading FROM us, so there is nothing to swap and the entry is
	// omitted rather than shown dead -- the same rule the Clients tab follows.
	//
	// Disabled for a non-A4AF source, where the peer is already on the file it
	// would swap to: "not right now" rather than "never here".
	if (IsShowingDownloadSources()) {
		m_menu->Append(MP_CHANGE2FILE, _("Swap to this file"));
		m_menu->Enable(MP_CHANGE2FILE, item->GetType() == A4AF_SOURCE);
	}

	// Asking the list which of its own columns has a legend keeps this true for
	// any future subclass without naming one here.
	if (FindBarLegendColumn() >= 0) {
		m_menu->AppendSeparator();
		m_menu->Append(MP_BARLEGEND, _("Colour legend"));
	}

	PopupMenu(m_menu, event.GetPosition());

	delete m_menu;
	m_menu = nullptr;
}

wxString CGenericClientListCtrl::GetItemColumnText(wxUIntPtr data, unsigned column) const
{
	if (column >= static_cast<unsigned>(m_columndata.n_columns)) {
		return wxEmptyString;
	}
	ClientCtrlItem_Struct *item = reinterpret_cast<ClientCtrlItem_Struct *>(data);
	CClientRef &client = item->GetSource();
	const bool notA4AF = item->GetType() != A4AF_SOURCE;

	switch (m_columndata.columns[column].cid) {
	case ColumnUserName:
		// Not drawn through this path (see CClientNameRenderer), but still
		// answered for type-ahead / accessible row label (#180).
		return client.GetUserName().IsEmpty() ? wxString("?") : client.GetUserName();

	case ColumnUserDownloaded:
		return (notA4AF && client.GetTransferredDown()) ? CastItoXBytes(client.GetTransferredDown())
								: wxString();

	case ColumnUserUploaded:
		return (notA4AF && client.GetTransferredUp()) ? CastItoXBytes(client.GetTransferredUp())
							      : wxString();

	case ColumnUserSpeedDown:
		if (notA4AF && client.GetKBpsDown() > 0.001) {
			if (client.GetKBpsDown() >= 1024) {
				return CFormat(_("%.1f MiB/s")) % (client.GetKBpsDown() / 1024.0);
			}
			return CFormat(_("%.1f KiB/s")) % client.GetKBpsDown();
		}
		return wxEmptyString;

	case ColumnUserSpeedUp:
		// Datarate is in bytes.
		if (notA4AF && client.GetUploadDatarate() >= 1024) {
			if (client.GetUploadDatarate() >= 1048576) {
				return CFormat(_("%.1f MiB/s")) % (client.GetUploadDatarate() / 1048576.0);
			}
			return CFormat(_("%.1f KiB/s")) % (client.GetUploadDatarate() / 1024.0);
		}
		return wxEmptyString;

	case ColumnUserVersion:
		return client.GetClientVerString();

	case ColumnUserQueueRankRemote: {
		if (notA4AF && client.GetDownloadState() == DS_ONQUEUE) {
			if (client.IsRemoteQueueFull()) {
				return _("Queue Full");
			}
			const uint16 rank = client.GetRemoteQueueRank();
			if (rank) {
				sint16 qrDiff = static_cast<sint16>(rank - client.GetOldRemoteQueueRank());
				if (qrDiff == rank) {
					qrDiff = 0;
				}
				return CFormat(_("On Queue: %u (%i)")) % rank % qrDiff;
			}
			return _("On Queue");
		}
		if (notA4AF) {
			return DownloadStateToStr(client.GetDownloadState(), client.IsRemoteQueueFull());
		}
		wxString buffer = _("Asked for another file");
		if (client.GetRequestFile() && client.GetRequestFile()->GetFileName().IsOk()) {
			buffer += CFormat(" (%s)") % client.GetRequestFile()->GetFileName();
		}
		return buffer;
	}

	case ColumnUserQueueRankLocal:
		if (!notA4AF) {
			return _("Asked for another file");
		}
		if (client.GetUploadState() == US_ONUPLOADQUEUE) {
			const uint16 nRank = client.GetUploadQueueWaitingPosition();
			return nRank == 0 ? wxString(_("Waiting for upload slot"))
					  : wxString(CFormat(_("On Queue: %u")) % nRank);
		}
		if (client.GetUploadState() == US_UPLOADING) {
			return _("Uploading");
		}
		return _("None");

	case ColumnUserOrigin:
		return wxGetTranslation(OriginToText(client.GetSourceFrom()));

	case ColumnUserFileNameDownload:
		if (const CPartFile *pf = client.GetRequestFile()) {
			return pf->GetFileName().GetPrintable();
		}
		return "[" + wxString(_("Unknown")) + "]";

	case ColumnUserFileNameUpload:
		if (const CKnownFile *kf = client.GetUploadFile()) {
			return kf->GetFileName().GetPrintable();
		}
		return "[" + wxString(_("Unknown")) + "]";

	case ColumnUserFileNameDownloadRemote:
		if (client.GetClientFilename().IsEmpty() || !notA4AF) {
			return "[" + wxString(_("Unknown")) + "]";
		}
		return client.GetClientFilename();

	case ColumnUserSharedFiles:
		return client.HasDisabledSharedFiles() ? _("No") : _("Yes");

	default:
		return wxEmptyString;
	}
}

bool CGenericClientListCtrl::GetItemAttr(wxUIntPtr data, unsigned column, wxDataViewItemAttr &attr) const
{
	if (column >= static_cast<unsigned>(m_columndata.n_columns)) {
		return false;
	}
	ClientCtrlItem_Struct *item = reinterpret_cast<ClientCtrlItem_Struct *>(data);
	CClientRef &client = item->GetSource();
	const bool isDark = IsListBackgroundDark(this);

	switch (m_columndata.columns[column].cid) {
	case ColumnUserQueueRankRemote: {
		if (item->GetType() == A4AF_SOURCE || client.GetDownloadState() != DS_ONQUEUE ||
			client.IsRemoteQueueFull()) {
			return false;
		}
		const uint16 rank = client.GetRemoteQueueRank();
		if (!rank) {
			return false;
		}
		sint16 qrDiff = static_cast<sint16>(rank - client.GetOldRemoteQueueRank());
		if (qrDiff == static_cast<sint16>(rank)) {
			qrDiff = 0;
		}
		// Queue rank change cue: down (good) = blue, up (bad) = red. Pure
		// *wxBLUE / *wxRED were unreadable on dark themes.
		if (qrDiff < 0) {
			attr.SetColour(isDark ? wxColour(120, 170, 255) : wxColour(0, 80, 200));
			return true;
		}
		if (qrDiff > 0) {
			attr.SetColour(isDark ? wxColour(255, 100, 100) : wxColour(220, 0, 0));
			return true;
		}
		return false;
	}

	case ColumnUserFileNameDownloadRemote: {
		if (client.GetClientFilename().IsEmpty() || item->GetType() == A4AF_SOURCE) {
			return false;
		}
		const CPartFile *pf = client.GetRequestFile();
		if (pf && (pf->GetFileName().GetPrintable().CmpNoCase(client.GetClientFilename()) != 0)) {
			// "watch out: peer is advertising a different name for this
			// hash" warning cue. Pure *wxRED was unreadable on dark themes.
			attr.SetColour(isDark ? wxColour(255, 100, 100) : wxColour(220, 0, 0));
			return true;
		}
		return false;
	}

	default:
		return false;
	}
}

namespace
{
//! The bar palette lives in PartBarLegend.h, where the legend reads the same
//! values -- see the header comment for why they cannot be kept in two places.
inline CMuleColour ToMuleColour(const partbar::BarColour &colour)
{
	return CMuleColour(colour.red, colour.green, colour.blue);
}
} // namespace

void CGenericClientListCtrl::GetItemBarFill(wxUIntPtr data, unsigned column, CBarFillSpec &out) const
{
	if (column >= static_cast<unsigned>(m_columndata.n_columns)) {
		return;
	}
	ClientCtrlItem_Struct *item = reinterpret_cast<ClientCtrlItem_Struct *>(data);

	switch (m_columndata.columns[column].cid) {
	case ColumnUserName:
		// CClientNameRenderer only needs the identity; it draws straight
		// off the live CClientRef, not off spans.
		out = CBarFillSpec(data, 0, {});
		return;

	case ColumnUserProgress: {
		// Gate mirrors DrawClientItem's ColumnUserProgress case: nothing is
		// computed (or drawn, including the A4AF badge) with the pref off --
		// see CSourceBarRenderer::Render() for the matching gate.
		if (!thePrefs::ShowProgBar()) {
			return;
		}
		CClientRef &client = item->GetSource();
		CPartFile *reqfile = client.GetRequestFile();
		const BitVector &partStatus = client.GetPartStatus();
		const bool bFlat = thePrefs::UseFlatBar();

		std::vector<CBarFillSpan> spans;
		uint64 fileSize = 1;

		if (reqfile && reqfile->GetPartCount() == partStatus.size()) {
			fileSize = reqfile->GetFileSize();
			const uint16 lastDownloadingPart = client.GetDownloadState() == DS_DOWNLOADING
								   ? client.GetLastDownloadingPart()
								   : 0xffff;
			const uint16 nextRequestedPart = client.GetNextRequestedPart();
			const bool stopped = reqfile->IsStopped();

			for (uint32 i = 0; i < partStatus.size(); i++) {
				const uint64 uStart = PARTSIZE * i;
				const uint64 uEnd = uStart + reqfile->GetPartSize(static_cast<uint16>(i)) - 1;

				// The order of these tests is the order the legend
				// lists the states in (partbar::kSourceLegendOrder).
				partbar::SourcePartState state;
				if (!partStatus.get(i)) {
					state = partbar::SourcePartState::Missing;
				} else if (reqfile->IsComplete(static_cast<uint16>(i))) {
					state = partbar::SourcePartState::Complete;
				} else if (lastDownloadingPart == static_cast<uint16>(i)) {
					state = partbar::SourcePartState::Downloading;
				} else if (nextRequestedPart == static_cast<uint16>(i)) {
					state = partbar::SourcePartState::NextRequested;
				} else {
					state = partbar::SourcePartState::Needed;
				}
				CMuleColour colour = ToMuleColour(partbar::SourcePartColour(state, bFlat));
				if (stopped) {
					// Not in the legend: a dimmed swatch out of
					// context reads as a sixth state rather than as
					// the same five turned down.
					colour.Blend(50);
				}
				spans.push_back({ uStart, uEnd, colour });
			}
		} else {
			spans.push_back({ 0,
				1,
				ToMuleColour(partbar::SourcePartColour(
					partbar::SourcePartState::Missing, bFlat)) });
		}

		out = CBarFillSpec(data, fileSize, std::move(spans));
		return;
	}

	case ColumnUserAvailable: {
		CClientRef &client = item->GetSource();
		const uint32 partCount = client.GetUpPartCount();
		if (!partCount) {
			// Matches DrawStatusBar's caller, which skips drawing (border
			// included) entirely when there is nothing to show.
			return;
		}
		const bool bFlat = thePrefs::UseFlatBar();

		std::vector<CBarFillSpan> spans;
		for (uint64 i = 0; i < partCount; i++) {
			const uint64 uStart = PARTSIZE * i;
			const uint64 uEnd = uStart + PARTSIZE - 1;
			spans.push_back({ uStart,
				uEnd,
				ToMuleColour(partbar::PeerPartColour(
					client.IsUpPartAvailable(i) ? partbar::PeerPartState::Present
								    : partbar::PeerPartState::Missing,
					bFlat)) });
		}
		out = CBarFillSpec(data, static_cast<uint64>(partCount) * PARTSIZE, std::move(spans));
		return;
	}

	default:
		return;
	}
}

int CGenericClientListCtrl::CompareByCid(
	GenericColumnEnum cid, const CClientRef &client1, const CClientRef &client2) const
{
	switch (cid) {
	case ColumnUserName:
		return CmpAny(client1.GetUserName(), client2.GetUserName());

	case ColumnUserDownloaded:
		return CmpAny(client1.GetTransferredDown(), client2.GetTransferredDown());

	case ColumnUserUploaded:
		return CmpAny(client1.GetTransferredUp(), client2.GetTransferredUp());

	case ColumnUserSpeedDown:
		return CmpAny(client1.GetKBpsDown(), client2.GetKBpsDown());

	case ColumnUserSpeedUp:
		return CmpAny(client1.GetUploadDatarate(), client2.GetUploadDatarate());

	case ColumnUserProgress:
		return CmpAny(client1.GetAvailablePartCount(), client2.GetAvailablePartCount());

	case ColumnUserVersion: {
		int cmp = client1.GetSoftStr().Cmp(client2.GetSoftStr());
		if (cmp == 0) {
			cmp = CmpAny(client1.GetVersion(), client2.GetVersion());
		}
		if (cmp == 0) {
			cmp = client1.GetClientModString().Cmp(client2.GetClientModString());
		}
		return cmp;
	}

	case ColumnUserQueueRankRemote: {
		// This will sort by download state: Downloading, OnQueue, Connecting...
		// However, Asked For Another will always be placed last, due to the
		// type-precedence pre-check in CompareItemData().
		if (client1.GetDownloadState() != client2.GetDownloadState()) {
			return client1.GetDownloadState() - client2.GetDownloadState();
		}
		// Placing items on queue before items on full queues
		if (client1.IsRemoteQueueFull()) {
			return client2.IsRemoteQueueFull() ? 0 : 1;
		}
		if (client2.IsRemoteQueueFull()) {
			return -1;
		}
		if (client1.GetRemoteQueueRank()) {
			return client2.GetRemoteQueueRank()
				       ? CmpAny(client1.GetRemoteQueueRank(), client2.GetRemoteQueueRank())
				       : -1;
		}
		return client2.GetRemoteQueueRank() ? 1 : 0;
	}

	case ColumnUserQueueRankLocal: {
		if (client1.GetUploadState() != client2.GetUploadState()) {
			return client1.GetUploadState() - client2.GetUploadState();
		}
		const uint16 rank1 = client1.GetUploadQueueWaitingPosition();
		const uint16 rank2 = client2.GetUploadQueueWaitingPosition();
		if (!rank1) {
			return !rank2 ? 0 : 1;
		}
		if (!rank2) {
			return -1;
		}
		return CmpAny(rank1, rank2);
	}

	case ColumnUserOrigin:
		return CmpAny(client1.GetSourceFrom(), client2.GetSourceFrom());

	case ColumnUserFileNameDownload: {
		wxString buffer1, buffer2;
		if (const CPartFile *pf1 = client1.GetRequestFile()) {
			buffer1 = pf1->GetFileName().GetPrintable();
		}
		if (const CPartFile *pf2 = client2.GetRequestFile()) {
			buffer2 = pf2->GetFileName().GetPrintable();
		}
		return CmpAny(buffer1, buffer2);
	}

	case ColumnUserFileNameUpload: {
		wxString buffer1, buffer2;
		if (const CKnownFile *kf1 = client1.GetUploadFile()) {
			buffer1 = kf1->GetFileName().GetPrintable();
		}
		if (const CKnownFile *kf2 = client2.GetUploadFile()) {
			buffer2 = kf2->GetFileName().GetPrintable();
		}
		return CmpAny(buffer1, buffer2);
	}

	case ColumnUserFileNameDownloadRemote:
		return CmpAny(client1.GetClientFilename(), client2.GetClientFilename());

	case ColumnUserSharedFiles:
		return CmpAny(client1.HasDisabledSharedFiles(), client2.HasDisabledSharedFiles());

	// ColumnUserAvailable intentionally has no case: unsortable, matching
	// the pre-port behaviour (see InitColumnData()'s SORTABLE-less
	// registration for this column).
	default:
		return 0;
	}
}

int CGenericClientListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	ClientCtrlItem_Struct *item1 = reinterpret_cast<ClientCtrlItem_Struct *>(data1);
	ClientCtrlItem_Struct *item2 = reinterpret_cast<ClientCtrlItem_Struct *>(data2);

	// Available sources first, if we have both an available and an
	// unavailable one -- the order is fixed regardless of sort direction, so
	// `modifier` is deliberately not applied here. Runs identically at every
	// level of a multi-column sort chain (CompareItemsFull calls this once
	// per level with the same two items); harmless, since it always agrees
	// with itself.
	const int typeOrder = item2->GetType() - item1->GetType();
	if (typeOrder) {
		return typeOrder;
	}

	if (column >= static_cast<unsigned>(m_columndata.n_columns)) {
		return 0;
	}
	return modifier *
	       CompareByCid(m_columndata.columns[column].cid, item1->GetSource(), item2->GetSource());
}

// File_checked_for_headers
