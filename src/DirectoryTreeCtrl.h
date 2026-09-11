//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Robert Rostek ( tecxx@rrs.at )
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

#ifndef DIRECTORYTREECTRL_H
#define DIRECTORYTREECTRL_H

#include <wx/treectrl.h>
#include <vector>
#include <map>
#include <set> // painted-roots snapshot (stale-view detection)

#include <common/Path.h>

class CDirectoryTreeCtrl : public wxTreeCtrl
{
public:
	typedef std::vector<CPath> PathList;

	CDirectoryTreeCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize siz, int flags);
	virtual ~CDirectoryTreeCtrl();

	// Get all explicitly shared directories. Single right-click is "recursive", double-click is
	// "this dir only"; this returns only the directories the user marked individually.
	void GetSharedDirectories(PathList *list);
	// set list of explicitly-shared directories
	void SetSharedDirectories(PathList *list);

	// Recursive-share intents: roots the user wants to share along with every descendant. The
	// descendant enumeration is deferred to the PrefsUnifiedDlg apply path, having once run
	// synchronously inside OnRButtonDown and frozen the UI on large trees.
	void GetRecursiveSharedDirectories(PathList *list);
	void SetRecursiveSharedDirectories(PathList *list);

	// User made any changes to list?
	bool HasChanged;

	//! Re-create an already built tree from the current shared roots. Needed when those change
	//! underneath us: UpdateSharedDirectories only re-marks the root's immediate children, so
	//! anything already expanded would keep the marks it was built with. Rebuilds straight away
	//! rather than deferring to the next Init(), because the dialog can reopen directly on the
	//! Directories page where no page-change event fires. A tree that was never built is left
	//! alone.
	void Rebuild();

	//! True when the tree was painted from roots other than these, i.e. the view is stale and
	//! needs rebuilding. The shared marks below the root's immediate children are applied when
	//! a node is created, so refreshing the backing maps alone leaves already-expanded nodes
	//! showing old state.
	bool NeedsRepaintFor(const PathList &explicitDirs, const PathList &recursiveDirs) const;

	void Init();

private:
	void AddChildItem(wxTreeItemId hBranch, const CPath &item);
	void AddSubdirectories(wxTreeItemId hBranch, const CPath &path);
	// returns true if folder has at least one subdirectory
	bool HasSubdirectories(const CPath &path);
	// return the full path of an item (like C:\abc\somewhere\inheaven\)
	CPath GetFullPath(wxTreeItemId hItem);
	// check status of an item has changed
	void CheckChanged(wxTreeItemId hItem, bool bChecked, bool recursed);
	// returns true if a subdirectory of strDir is shared
	bool HasSharedSubdirectory(const CPath &path);
	void UpdateSharedDirectories();
	// when sharing a directory, make all parent directories red
	void UpdateParentItems(wxTreeItemId hChild, bool add);
	// set color red if there's a shared subdirectory
	void SetHasSharedSubdirectory(wxTreeItemId hItem, bool add);

	// share list access
	bool IsShared(const CPath &path);
	void AddShare(const CPath &path);
	void DelShare(const CPath &path);
	void MarkChildren(wxTreeItemId hChild, bool mark, bool recursed);

	// Recursive-share intent helpers. These aren't `const` because
	// GetKey() may resolve cwd for relative paths via wxGetCwd().
	bool IsRecursiveShare(const CPath &path);
	bool IsInsideRecursiveShare(const CPath &path);
	void AddRecursiveShare(const CPath &path);
	void DelRecursiveShare(const CPath &path);
	// Sweep all m_lstShared entries that are descendants of `root`. Used by OnRButtonDown's
	// unshare path, so a recursive share whose flat descendants survived a previous Prefs
	// session is fully removed by right-clicking the root again, without expanding every subdir
	// to find them.
	void DelSharesUnder(const CPath &root);

	void OnItemExpanding(wxTreeEvent &evt);
	void OnRButtonDown(wxTreeEvent &evt);
	void OnItemActivated(wxTreeEvent &evt);

	typedef std::pair<wxString, CPath> SharedMapItem;
	typedef std::map<wxString, CPath> SharedMap;
	SharedMap m_lstShared;
	// Recursive-share roots. Keys are normalized paths just like m_lstShared, so prefix-
	// matching (IsInsideRecursiveShare) is consistent across the two maps.
	SharedMap m_lstSharedRecursive;

	//! Roots the currently-painted tree was built from, so a later open can
	//! tell whether the view still reflects them.
	std::set<wxString> m_paintedRoots;
	// get map key from path (normalized path)
	wxString GetKey(const CPath &path);

	bool m_IsInit;
	// Are we running the remote GUI, and from a remote location?
	bool m_IsRemote;

	wxTreeItemId m_root;

	// Font for recursive-share roots. Bold-italic, so "this is the recursive root" is
	// distinguishable from a plain explicit share and from a descendant covered by a recursive
	// expansion -- all three would otherwise look identical. Constructed lazily on first paint,
	// because the tree control's default font is not fully resolved until after the ctor runs.
	wxFont GetRecursiveFont();
	// Toggle the italic-bold marker on a single tree item. Called from AddChildItem on initial
	// render and from the right-click toggle paths, so the visual stays in sync with
	// m_lstSharedRecursive on every transition.
	void ApplyRecursiveMark(wxTreeItemId hItem, bool isRecursive);
	wxFont m_fontRecursiveRoot;

	wxDECLARE_EVENT_TABLE();
};

#undef wxTreeItemId

#endif // DIRECTORYTREECTRL_H
// File_checked_for_headers
