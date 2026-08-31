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

#ifndef GENERICCLIENTLISTCTRL_H
#define GENERICCLIENTLISTCTRL_H

#include <map>    // Needed for std::multimap
#include <vector> // Needed for std::vector

#include "Types.h"                   // Needed for uint8
#include "Constants.h"               // Needed for DownloadItemType
#include "ClientRef.h"               // Needed for CClientRef (stored by value below)
#include "MuleVirtualDataViewCtrl.h" // Needed for CMuleVirtualDataViewCtrl
#include "PartBarLegend.h"           // Needed for GenericColumnEnum, partbar::BarLegendKind
#include "amuleDlg.h"                // Needed for CamuleDlg::DialogType

class CPartFile;
class CMuleBarRenderer;
class wxMenu;

/**
 * One row: a client together with the file it is a source/peer of and
 * whether it is a current or A4AF source. Not just a cache-avoidance
 * wrapper like the pre-port CDownloadListCtrl's FileCtrlItem_Struct was --
 * genuinely necessary here, since one client (ECID) can be a source/peer of
 * more than one file at once, so identity has to be the (client, owner,
 * type) tuple, not the CClientRef alone. Public (not file-local to
 * GenericClientListCtrl.cpp) because CSourceBarRenderer, in the leaf's own
 * .cpp, needs to read it back from a CBarFillSpec identity.
 */
struct ClientCtrlItem_Struct
{
	ClientCtrlItem_Struct()
	: m_owner(nullptr)
	, m_type(UNAVAILABLE_SOURCE)
	{
	}

	SourceItemType GetType() const { return m_type; }

	CKnownFile *GetOwner() const { return m_owner; }

	CClientRef &GetSource() { return m_sourceValue; }

	void SetContents(CKnownFile *owner, const CClientRef &source, SourceItemType type)
	{
		m_owner = owner;
		m_sourceValue = source;
		m_type = type;
	}

	void SetType(SourceItemType type) { m_type = type; }

private:
	CKnownFile *m_owner;
	CClientRef m_sourceValue;
	SourceItemType m_type;
};

struct CGenericClientListCtrlColumn
{
	GenericColumnEnum cid;
	wxString title;
	int width;
};

struct GenericColumnInfo
{
	GenericColumnInfo(int n, CGenericClientListCtrlColumn *col)
	: n_columns(n)
	, columns(col) {};
	int n_columns;
	CGenericClientListCtrlColumn *columns;
};

typedef std::vector<CKnownFile *> CKnownFileVector;

/**
 * This class is responsible for representing clients in a generic way.
 *
 * Rows are addressed by ClientCtrlItem_Struct* identity -- unlike every other
 * ported list, that wrapper survives the port: a client (ECID) can be a
 * source/peer of more than one file at once, so each row needs the client
 * together with its owner file and A4AF/available type, not just the
 * CClientRef alone. See ClientCtrlItem_Struct in the .cpp.
 */
class CGenericClientListCtrl : public CMuleVirtualDataViewCtrl
{
public:
	/**
	 * Constructor.
	 *
	 * @see CMuleVirtualDataViewCtrl::CMuleVirtualDataViewCtrl for documentation of parameters.
	 */
	CGenericClientListCtrl(const wxString &tablename,
		wxWindow *parent,
		wxWindowID winid,
		const wxPoint &pos,
		const wxSize &size,
		long style,
		const wxString &name);

	/**
	 * Destructor.
	 */
	virtual ~CGenericClientListCtrl();

	/**
	 * Initializes the control. We need a 2-stage initialization so the derived class members can be
	 * called.
	 */
	void InitColumnData();

	/**
	 * Adds a source belonging to the specified file.
	 *
	 * @param owner The owner of this specific source-entry, must be a valid pointer.
	 * @param source The client object to be added, must be a valid pointer.
	 * @param type If the source is a current source, or a A4AF source.
	 *
	 * Please note that the specified client will only be added to the list if it's
	 * owner is shown, otherwise the source will simply be ignored.
	 * Duplicates wont be added.
	 */
	void AddSource(CKnownFile *owner, const CClientRef &source, SourceItemType type);

	/**
	 * Removes a source from the list.
	 *
	 * @param source ID of the source to be removed.
	 * @param owner Either a specific file, or NULL to remove the source from all files.
	 */
	void RemoveSource(uint32 source, const CKnownFile *owner);

	/**
	 * Shows the clients of specific files.
	 *
	 * @param file A valid, sorted vector of files whose clients will be shown.
	 *
	 * WARNING: The received vector *MUST* be odered with std::sort.
	 *
	 */
	void ShowSources(const CKnownFileVector &files);

	/**
	 * Updates the state of the specified item, possibly causing a redrawing.
	 *
	 * @param toupdate ID of the client to be updated.
	 * @param type If the source is a current source, or a A4AF source.
	 *
	 */
	void UpdateItem(uint32 toupdate, SourceItemType type);

	void SetShowing(bool status) { m_showing = status; }
	bool GetShowing() const { return m_showing; }

	/**
	 * Drop every reference to `file` from this control before the
	 * CKnownFile is destroyed. Called from
	 * MuleNotify::KnownFileBeingDestroyed (see GuiEvents.cpp) for
	 * every CKnownFile destruction site. The control walks
	 * m_knownfiles by pointer-value comparison and erases matching
	 * entries — does NOT dereference `file`, which by the time this
	 * runs on the main thread is already freed. Also strips any
	 * ClientCtrlItem_Struct rows whose m_owner matches.
	 *
	 * Without this, m_knownfiles would carry the dangling pointer
	 * into the next ShowSources() call's `SetShowSources(_, false)`
	 * loop and crash on the freed heap (issue #755).
	 */
	void RemoveKnownFile(CKnownFile *file);

protected:
	// The columns with their attributes; MUST be defined by the derived class.
	GenericColumnInfo m_columndata;

	/// Text of one cell, pulled on demand for the cells being drawn.
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;

	/// Per-row/per-column display attributes: the queue-rank-diff and
	/// filename-mismatch colour cues DrawClientItem used to set on the wxDC.
	bool GetItemAttr(wxUIntPtr item, unsigned column, wxDataViewItemAttr &attr) const override;

	/// Fill data for the Name column's composite icon+text renderer (see
	/// CClientNameRenderer) and for a leaf's own bar column.
	void GetItemBarFill(wxUIntPtr item, unsigned column, CBarFillSpec &out) const override;

	/// Single-column comparison for the base's sort chain, after the
	/// type-precedence pre-check (A4AF sources always last).
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;

	/** Live auto-sort: re-order when sorted by a column whose value changes
	 *  during transfer (speed, progress, up/downloaded, availability, queue
	 *  rank). Static columns (name, version, filename, ...) don't auto-resort. */
	bool IsLiveSortColumn() const override;

	/** Pause live auto-sort while the context menu is open. */
	bool IsMenuOpen() const override { return m_menu != nullptr; }

	//! Renderer for a leaf's own bar column (ColumnUserProgress for Sources,
	//! ColumnUserAvailable for Peers). Sources overrides this to opt into the
	//! A4AF-badge-drawing subclass; Peers' plain 2-state bar needs no
	//! subclass, so the default (nullptr) is a plain CMuleBarRenderer.
	virtual CMuleBarRenderer *CreateProgressBarRenderer() const { return nullptr; }

private:
	/**
	 *
	 * Must be overridden by the derived class and return the dialog where this list is.
	 * @see CamuleDlg::DialogType
	 *
	 */
	virtual CamuleDlg::DialogType GetParentDialog() = 0;

	/**
	 * Updates the displayed number representing the amount of clients currently shown.
	 */
	void ShowSourcesCount(int diff);

	/**
	 * Set "show sources" or "show peers" flag in Known File
	 */
	virtual void SetShowSources(CKnownFile *, bool) const = 0;

	/**
	 * Translate the CID to a unique string for saving column sizes
	 */
	wxString TranslateCIDToName(GenericColumnEnum cid);

	int CompareByCid(GenericColumnEnum cid, const CClientRef &client1, const CClientRef &client2) const;

	// Event-handlers for clients.
	void OnSwapSource(wxCommandEvent &event);
	void OnViewFiles(wxCommandEvent &event);
	void OnAddFriend(wxCommandEvent &event);
	void OnSetFriendslot(wxCommandEvent &event);
	void OnSendMessage(wxCommandEvent &event);
	void OnViewClientInfo(wxCommandEvent &event);
	/**
	 * Opens the colour legend for this list's chunk-bar column. The bar has no
	 * other explanation of what its colours mean (issue #1192), and the two
	 * variants of the column -- ColumnUserProgress for Sources,
	 * ColumnUserAvailable for Peers -- have different palettes, so the legend
	 * is chosen per cid (partbar::LegendForColumn).
	 */
	void OnShowBarLegend(wxCommandEvent &event);

	// Misc event-handlers
	void OnItemActivated(wxDataViewEvent &event);
	void OnItemRightClicked(wxDataViewEvent &event);
	/**
	 * Shows the client detail dialog for the row under the pointer.
	 * wxDataViewCtrl has no dedicated middle-click item event, unlike
	 * wxListCtrl's EVT_LIST_ITEM_MIDDLE_CLICK, so this is a raw mouse event
	 * resolved through HitTest() -- see CDownloadListCtrl::OnItemActivated
	 * for why an event's item is resolved rather than read off the
	 * selection.
	 */
	void OnMouseMiddleClick(wxMouseEvent &event);

	/**
	 * Index in m_columndata of the chunk-bar column this list shows, or -1
	 * when there is none on screen -- either the subclass declares no bar
	 * column, or the user has hidden the one it declares. Found by asking
	 * partbar::LegendForColumn() about the list's own columns rather than by
	 * naming cids here, so a subclass that gains or loses a bar column needs
	 * nothing changed in this class.
	 */
	int FindBarLegendColumn() const;

	//! Pops up the legend of @a kind, titled with @a columnTitle. Swatches
	//! are filled from partbar::SourcePartColour()/PeerPartColour(), the same
	//! functions GetItemBarFill() fills the bar itself from.
	void ShowBarLegend(partbar::BarLegendKind kind, const wxString &columnTitle);

	/**
	 * The item the context menu was built for, by identity rather than row —
	 * see the identical field on CDownloadListCtrl for why (PopupMenu runs a
	 * nested event loop, so a row index would not survive it).
	 */
	wxUIntPtr m_menuItem = 0;

	//! The type of list used to store items on the listctrl. We use the unique ECID as key.
	typedef std::multimap<uint32, ClientCtrlItem_Struct *> ListItems;
	//! Shortcut to the pair-type used on the list.
	typedef ListItems::value_type ListItemsPair;
	//! This pair is used when searching for equal-ranges.
	typedef std::pair<ListItems::iterator, ListItems::iterator> ListIteratorPair;

	//! This list contains everything shown on the list. Sources are only to
	//! be found on this list if they are being displayed
	ListItems m_ListItems;

	//! Pointer to the current menu object, used to avoid multiple menus.
	wxMenu *m_menu;

	//! The number of displayed sources
	int m_clientcount;

	//! The files being shown, if any.
	CKnownFileVector m_knownfiles;

	wxDECLARE_EVENT_TABLE();

	bool m_showing;

	void RawAddSource(CKnownFile *owner, CClientRef source, SourceItemType type);
	void RawRemoveSource(ListItems::iterator &it);

	virtual bool IsShowingDownloadSources() const = 0;
};

#endif
// File_checked_for_headers
