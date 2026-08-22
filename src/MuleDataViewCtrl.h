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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
//

#ifndef MULEDATAVIEWCTRL_H
#define MULEDATAVIEWCTRL_H

#include <wx/dataview.h>

#include "ListColumnStore.h" // Needed for CListColumnStore, IColumnWidthProvider, COL_SIZE_MIN
#include "Types.h"           // Needed for uint64

#include <list>
#include <utility>
#include <vector>

class CMuleBarRenderer;

/**
 * Shared behaviour for aMule's wxDataViewCtrl-backed lists.
 *
 * This is the wxDataViewCtrl counterpart of CMuleListCtrl: the chrome every
 * list needs and no list should implement twice -- column widths and their
 * persistence, the show/hide menu, the multi-column sort chain, type-ahead
 * selection, and the per-platform compensation the ports have had to grow.
 *
 * It owns no data. A subclass supplies its rows through GetDisplayOrder(),
 * their label through GetRowLabel(), and how two of them compare on a given
 * column through CompareByColumn(); everything else here is written in terms
 * of those three.
 *
 * Lists with a flat, row-addressed model should derive from
 * CMuleVirtualDataViewCtrl instead, which adds the item-identity bookkeeping
 * on top of this.
 */
class CMuleDataViewCtrl : public wxDataViewCtrl
{
public:
	CMuleDataViewCtrl(wxWindow *parent,
		wxWindowID winid = wxID_ANY,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxString &name = "muledataviewctrl");

	~CMuleDataViewCtrl() override;

	//! Sort-order bits, matching CMuleListCtrl so the persisted config stays
	//! wire-compatible across both list families.
	enum
	{
		SORT_DES = 0x1,
		SORT_ALT = 0x2,
		SORTING_MASK = 0x3
	};

	/**
	 * What asked for a sort, which decides how the header caret is drawn.
	 *
	 * Only wxGTK cares, and only because it takes a second run at the caret
	 * on its own after a header click -- see ShowSortCaret().
	 */
	enum class SortTrigger
	{
		Programmatic, //!< aMule chose the order: startup, defaults, list sync
		HeaderClick   //!< the user clicked the header
	};

	/**
	 * Whether a column is hidden.
	 *
	 * Tracked here rather than derived from the control: a hidden
	 * wxDataViewColumn keeps reporting its previous width, so width alone
	 * cannot answer the question, and the width is what CListColumnStore
	 * persists (as a negative entry carrying the size to restore).
	 */
	bool IsColumnHidden(int col) const;
	//! Hide or show a column; width is the one to restore when showing.
	void SetColumnHidden(int col, bool hidden, int width);

	/**
	 * Columns the user owns, excluding the macOS spacer (see the
	 * constructor), which must stay out of the header menu, the persisted
	 * widths and any cross-list synchronisation.
	 */
	unsigned RealColumnCount() const;

protected:
	typedef std::pair<unsigned, unsigned> CColPair;
	typedef std::list<CColPair> CSortingList;

	// --- what a subclass must provide -------------------------------------

	/**
	 * The rows as displayed, addressed by position.
	 *
	 * Built once per operation (a keystroke, a page key) rather than probed
	 * per row: a list that derives its order has to walk its data to answer
	 * at all, so per-row access would repeat that walk for every row.
	 * Implementations must include the children of expanded containers where
	 * they have them, since the callers measure against what is on screen.
	 */
	virtual void GetDisplayOrder(wxDataViewItemArray &ordered) const = 0;

	//! Text a row is matched against when the user types to select.
	virtual wxString GetRowLabel(const wxDataViewItem &item) const = 0;

	/**
	 * Single-column comparison, without the chain or the direction: the
	 * caller applies `modifier` (-1 when descending) to the result.
	 */
	virtual int CompareByColumn(const wxDataViewItem &item1,
		const wxDataViewItem &item2,
		unsigned column,
		bool alt,
		int modifier) const = 0;

	//! Whether a column offers a secondary "alt" sort criterion.
	virtual bool AltSortAllowed(unsigned WXUNUSED(column)) const { return false; }

	//! Column-name string for migrating pre-CListColumnStore config entries.
	virtual wxString GetOldColumnOrder() const { return wxEmptyString; }

	/**
	 * First refusal on every key, before type-ahead or the backend sees it.
	 * Return true to swallow it; the default claims nothing.
	 */
	virtual bool OnListKey(wxKeyEvent &WXUNUSED(evt)) { return false; }

	//! Called once per idle after the base has done its own housekeeping.
	virtual void OnIdleHook() {}

	//! Called after a column is shown or hidden from the header menu.
	virtual void OnColumnVisibilityChanged() {}

	//! Called from idle when a drag-resize changed one or more widths.
	/**
	 * A drag-resize has been detected (see OnIdle -- wxDataViewCtrl has no
	 * portable end-of-drag event, so widths are polled).
	 *
	 * The default persists them. wxListCtrl had EVT_LIST_COL_END_DRAG to hook,
	 * and when these lists moved to wxDataViewCtrl the poll replaced it but was
	 * never wired to the store, so a resized column was only ever saved if the
	 * user happened to sort or toggle a column afterwards -- SaveColumnSettings'
	 * only other callers. An override must call the base, or resizing stops
	 * being remembered for that list.
	 *
	 * Fired once per drag, when the widths stop moving -- not once per pixel.
	 * The poll runs on idle, which fires continuously while the mouse does, so
	 * acting on every observed change made a single drag do its work hundreds
	 * of times: a wxConfig write each time here, and in CSearchListCtrl a
	 * width-set on every column of every other open search tab, each forcing a
	 * header relayout and repaint. That is what made resizing flicker and spike
	 * the CPU, the more so the more tabs were open (issue #1022). Only the
	 * final width matters, so only the final width is acted on.
	 */
	virtual void OnColumnWidthsChanged() { SaveColumnSettings(); }

	//! Called after the sort chain changes; the list re-orders its rows here.
	virtual void OnSortingChanged() {}

	// --- services for subclasses ------------------------------------------

	//! Full chain comparison: primary column first, then the tie-breakers.
	int CompareItems(const wxDataViewItem &item1, const wxDataViewItem &item2) const;

	//! Pushes a column to the front of the sort chain and applies the caret.
	void ApplySorting(unsigned column, unsigned order, SortTrigger trigger = SortTrigger::Programmatic);

	/**
	 * Draws the header caret for `order` on `column` and clears it elsewhere.
	 *
	 * Split out of ApplySorting() for the lists that keep the sort chain in
	 * step by hand (CSearchListCtrl mirrors one list's order onto the others)
	 * and must not re-enter the chain bookkeeping.
	 */
	void ShowSortCaret(unsigned column, unsigned order, SortTrigger trigger = SortTrigger::Programmatic);

	/**
	 * Drops the header's sort key for the duration of a bulk append, and puts
	 * it back afterwards.
	 *
	 * The caret is drawn by marking a column as the control's sort key, which
	 * the native backends read as an instruction to keep the view ordered
	 * themselves -- and macOS acts on it per insertion: every RowInserted()
	 * has NSOutlineView re-query the data source and re-run our Compare(), so
	 * appending N rows costs O(N^2) even though each append is O(1) on our
	 * side. Suspending the key for the burst leaves the rows in arrival order
	 * (which the caller sorts once at the end anyway) and the comparisons
	 * never happen.
	 *
	 * Balanced calls; nesting is not supported, and restoring re-draws the
	 * caret from m_sort_orders, so a sort change during the burst is honoured.
	 */
	void SuspendHeaderSort();
	void RestoreHeaderSort();

	void LoadColumnSettings();
	void SaveColumnSettings();

	/**
	 * True when LoadColumnSettings() restored at least one stored width, i.e.
	 * this profile has a layout the user has already influenced.
	 *
	 * An auto-fit must not run in that case: it would overwrite a width that
	 * was deliberately dragged. That mattered little while drag-resizes were
	 * never saved, but they are now, so a fit-on-load would discard the choice
	 * one launch later.
	 */
	bool HasPersistedColumnWidths() const { return m_hasPersistedWidths; }

	//! Keeps the expander on the leftmost visible column.
	void UpdateExpanderColumn();

	/**
	 * Appends the trailing spacer column on macOS.
	 *
	 * Must be called by the subclass after its own columns are appended and
	 * before LoadColumnSettings(): macOS sizes the last *resizable* column
	 * to the leftover space, collapsing it to nothing once the columns are
	 * wider than the control, so a spacer takes that role instead of a
	 * column the user cares about. `modelColumn` is an always-empty column
	 * the subclass' model must answer for.
	 */
	void AppendSpacerColumn(unsigned modelColumn);

	/**
	 * Appends a column rendered by a CMuleBarRenderer (a CBarShader chunk
	 * bar), for lists that used to paint one with direct wxDC calls in an
	 * owner-drawn OnDrawItem(). The subclass supplies the per-row fill data
	 * through CMuleVirtualDataViewCtrl::GetItemBarFill().
	 *
	 * @a renderer lets a subclass draw more than the plain chunk bar (e.g.
	 * CDownloadListCtrl's CDownloadBarRenderer, which adds a completed-progress
	 * overlay and percent text) by passing its own CMuleBarRenderer subclass;
	 * null constructs a plain one, today's behaviour.
	 */
	void AppendBarColumn(const wxString &title,
		unsigned modelColumn,
		int width,
		int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE,
		CMuleBarRenderer *renderer = nullptr);

	/**
	 * Append a column and register it for persistence, in one call.
	 *
	 * A column's width has to reach two places that never meet: wx, which
	 * uses it as the initial on-screen width, and CListColumnStore, which
	 * keeps it as the default to restore a column to when it is un-hidden
	 * with no width cached for it. Passing it separately to AppendTextColumn()
	 * and RegisterColumn() meant writing the same number twice with nothing
	 * keeping the two honest -- the legacy CMuleListCtrl::InsertColumn() took
	 * both in one call and could not drift.
	 *
	 * @a key is the persistence key: a single letter, written to
	 * TableWidths<list> / TableOrdering<list> in the config. It is the on-disk
	 * format, so changing one resets that column's saved width and forgets its
	 * sort.
	 *
	 * @a mode is last, rather than in wx's position ahead of the width, because
	 * width has no default and a parameter before it could not have one either
	 * -- every caller would have to spell out INERT. Every column is inert
	 * today; the parameter exists so that an editable one later does not have
	 * to drop back to Append* plus a hand-written RegisterColumn(), which is
	 * the duplication this exists to remove.
	 *
	 * Named Add* rather than overloading wx's Append*: an overload with a
	 * different signature would still bind to the wx method wherever a call
	 * was missed, compiling cleanly and persisting nothing. A distinct name
	 * makes "still calls Append*" a signal that grep can check, which is what
	 * the migration is validated with.
	 */
	void AddTextColumn(const wxString &label,
		unsigned modelColumn,
		const wxString &key,
		int width,
		wxAlignment align = wxALIGN_LEFT,
		int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE,
		wxDataViewCellMode mode = wxDATAVIEW_CELL_INERT);

	/// AddTextColumn() for a column whose cells carry an icon beside the text.
	void AddIconTextColumn(const wxString &label,
		unsigned modelColumn,
		const wxString &key,
		int width,
		wxAlignment align = wxALIGN_LEFT,
		int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE,
		wxDataViewCellMode mode = wxDATAVIEW_CELL_INERT);

	/// AddTextColumn() for a CMuleBarRenderer column; see AppendBarColumn().
	void AddBarColumn(const wxString &label,
		unsigned modelColumn,
		const wxString &key,
		int width,
		int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE,
		CMuleBarRenderer *renderer = nullptr);

	/**
	 * Sizes the per-column state and snapshots the current widths. Call
	 * after the columns exist and after LoadColumnSettings(): it preserves
	 * any hidden flags already restored through the width adapter.
	 */
	void InitColumnState();

	/**
	 * Adapts this control to IColumnWidthProvider for CListColumnStore.
	 * A separate object rather than multiple inheritance: wxDataViewCtrl's
	 * own GetColumnCount() is virtual and returns unsigned, which isn't
	 * override-compatible with the interface's int.
	 */
	class ColumnWidthAdapter : public IColumnWidthProvider
	{
	public:
		explicit ColumnWidthAdapter(CMuleDataViewCtrl *ctrl)
		: m_ctrl(ctrl)
		{
		}
		int GetColumnCount() const override { return static_cast<int>(m_ctrl->RealColumnCount()); }
		//! A hidden column reads as zero-width, which is how
		//! CListColumnStore already persists hidden state.
		int GetColumnWidth(int col) const override;
		bool SetColumnWidth(int col, int width) override
		{
			m_ctrl->SetColumnHidden(col, width <= COL_SIZE_MIN, width);
			return true;
		}

	private:
		CMuleDataViewCtrl *m_ctrl;
	};

	CListColumnStore m_columnStore;
	ColumnWidthAdapter m_widthAdapter;
	CSortingList m_sort_orders;
	//! Set between SuspendHeaderSort() and RestoreHeaderSort().
	bool m_headerSortSuspended = false;
	bool m_hasPersistedWidths = false;

	void OnColumnHeaderClick(wxDataViewEvent &event);
	void OnColumnHeaderRightClick(wxDataViewEvent &event);
	void OnColumnMenuSelected(wxCommandEvent &evt);
	void OnIdle(wxIdleEvent &event);
	void OnChar(wxKeyEvent &evt);
	void OnKeyDown(wxKeyEvent &evt);
	void OnContextMenuKey(wxContextMenuEvent &evt);

	//! Synthesises wxEVT_DATAVIEW_ITEM_CONTEXT_MENU for the current item,
	//! positioned at its row rather than the mouse -- shared by every
	//! keyboard-triggered path (OnContextMenuKey, and OSX's Shift+F10 in
	//! OnKeyDown).
	void RaiseItemContextMenu();

private:
	//! Per-column hidden state, indexed like the control's columns.
	std::vector<bool> m_columnHidden;

	//! Last-seen widths, for detecting a drag-resize (no portable event).
	std::vector<int> m_lastKnownWidths;

	//! Whether the widths moved since the last idle, i.e. a drag is still in
	//! progress. OnColumnWidthsChanged() fires when this goes back to false,
	//! not while it is true -- see OnIdle().
	bool m_widthsSettling = false;

	//! True once AppendSpacerColumn() has run (macOS only).
	bool m_hasSpacer = false;

	// Type-ahead state.
	wxString m_ttsText;
	uint64 m_ttsTime = 0;
	void *m_ttsItem = nullptr;

#ifdef __WXOSX__
	enum class PageMotion
	{
		PageUp,
		PageDown,
		Home,
		End
	};
	/**
	 * Moves the cursor by a page, or to the start/end of the list, and
	 * brings it into view.
	 *
	 * GTK and MSW get this from their backends; NSOutlineView scrolls
	 * without moving cursor or selection, which leaves the next arrow key
	 * jumping back to wherever the cursor was left.
	 *
	 * @param extend Grow the selection to the row landed on, as Shift does
	 *               on the other ports, rather than replacing it.
	 */
	void MoveByPage(PageMotion motion, bool extend);
#endif

	wxDECLARE_EVENT_TABLE();
};

#endif // MULEDATAVIEWCTRL_H
