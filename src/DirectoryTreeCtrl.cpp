//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2003-2011 Robert Rostek ( tecxx@rrs.at )
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

#include "DirectoryTreeCtrl.h" // Interface declarations

#include <wx/app.h>
#include <wx/artprov.h> // Needed for the "amule:folder"/"amule:folder_shared" art ids
#include <wx/filename.h>
#include <wx/imaglist.h>
#include <wx/msgdlg.h> // Needed for wxMessageBox (was pulled in transitively via muuli_wdr.h's wx/wx.h)

#include <common/StringFunctions.h>
#include <common/FileFunctions.h>
#include "amule.h" // Needed for theApp

wxBEGIN_EVENT_TABLE(CDirectoryTreeCtrl, wxTreeCtrl)
	EVT_TREE_ITEM_RIGHT_CLICK(wxID_ANY, CDirectoryTreeCtrl::OnRButtonDown)
	EVT_TREE_ITEM_ACTIVATED(wxID_ANY, CDirectoryTreeCtrl::OnItemActivated)
	EVT_TREE_ITEM_EXPANDED(wxID_ANY, CDirectoryTreeCtrl::OnItemExpanding)
wxEND_EVENT_TABLE()

class CItemData : public wxTreeItemData
{
public:
	CItemData(const CPath &pathComponent)
	: m_path(pathComponent)
	{
	}

	~CItemData() {}

	const CPath &GetPathComponent() const { return m_path; }

private:
	CPath m_path;
};

CDirectoryTreeCtrl::CDirectoryTreeCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize siz, int flags)
: wxTreeCtrl(parent, id, pos, siz, flags, wxDefaultValidator, "ShareTree")
{
	m_IsInit = false;
	HasChanged = false;
#ifdef CLIENT_GUI
	m_IsRemote = !theApp->m_connect->IsConnectedToLocalHost();
#else
	m_IsRemote = false;
#endif
}

wxFont CDirectoryTreeCtrl::GetRecursiveFont()
{
	// Cached: GetFont() is a non-trivial wx call on some backends, and AddChildItem
	// hits this once per tree item during the lazy expand pass.
	if (!m_fontRecursiveRoot.IsOk()) {
		m_fontRecursiveRoot = GetFont().MakeBold().MakeItalic();
	}
	return m_fontRecursiveRoot;
}

void CDirectoryTreeCtrl::ApplyRecursiveMark(wxTreeItemId hItem, bool isRecursive)
{
	if (isRecursive) {
		SetItemFont(hItem, GetRecursiveFont());
	} else {
		// Revert to the tree's default font. SetItemBold is the orthogonal axis, so
		// clearing the custom font here does not drop it.
		//
		// Pass GetFont() rather than wxNullFont: on wxMSW 3.2
		// wxTreeCtrl::SetItemFont calls wxFont::WXAdjustToPPI(), which dereferences
		// the font's refdata with no null check, and wxNullFont has none (#827).
		SetItemFont(hItem, GetFont());
	}
}

CDirectoryTreeCtrl::~CDirectoryTreeCtrl() {}

enum
{
	IMAGE_FOLDER = 0,
	IMAGE_FOLDER_SUB_SHARED
};

void CDirectoryTreeCtrl::Rebuild()
{
	if (!m_IsInit) {
		// Never built: leave it to the first Init(), which starts from the
		// current state anyway. Keeps the initial drive scan lazy.
		return;
	}
	DeleteAllItems();
	m_IsInit = false;
	Init();
}

void CDirectoryTreeCtrl::Init()
{
	if (m_IsInit) {
		return;
	}
	m_IsInit = true;

	wxImageList *images = new wxImageList(16, 16);
	// wxImageList::Add() wants a flat wxBitmap, not a bundle -- it's a
	// raster cache with no per-DPI resolution of its own.
	images->Add(wxArtProvider::GetBitmap("amule:folder", wxART_OTHER, wxSize(16, 16)));
	images->Add(wxArtProvider::GetBitmap("amule:folder_shared", wxART_OTHER, wxSize(16, 16)));
	AssignImageList(images);

	// Create an empty root item, which we can
	// safely append when creating a full path.
	m_root = AddRoot("", IMAGE_FOLDER, -1, new CItemData(CPath()));

	if (!m_IsRemote) {
#ifndef __WINDOWS__
		AddChildItem(m_root, CPath("/"));
#else
		::wxSetCursor(*wxHOURGLASS_CURSOR);
		uint32 drives = GetLogicalDrives();
		drives >>= 1;
		for (char drive = 'C'; drive <= 'Z'; drive++) {
			drives >>= 1;
			if (!(drives & 1)) { // skip non existent drives
				continue;
			}
			wxString driveStr = CFormat("%c:") % drive;
			uint32 type = GetDriveType((driveStr + "\\").wc_str());

			// skip removable/undefined drives, share only fixed or remote drives
			if ((type == 3 || type == 4) // fixed drive / remote drive
				&& CPath::DirExists(driveStr)) {
				AddChildItem(m_root, CPath(driveStr));
			}
		}
		::wxSetCursor(*wxSTANDARD_CURSOR);
#endif
	}

	HasChanged = false;

	UpdateSharedDirectories();

	// Remember what this paint was based on (see NeedsRepaintFor).
	m_paintedRoots.clear();
	for (SharedMap::const_iterator it = m_lstShared.begin(); it != m_lstShared.end(); ++it) {
		m_paintedRoots.insert(it->second.GetRaw());
	}
	for (SharedMap::const_iterator it = m_lstSharedRecursive.begin(); it != m_lstSharedRecursive.end();
		++it) {
		m_paintedRoots.insert("R:" + it->second.GetRaw());
	}
}

bool CDirectoryTreeCtrl::NeedsRepaintFor(const PathList &explicitDirs, const PathList &recursiveDirs) const
{
	if (!m_IsInit) {
		// Nothing painted yet; the first Init() will use current state.
		return false;
	}
	std::set<wxString> wanted;
	for (const CPath &path : explicitDirs) {
		wanted.insert(path.GetRaw());
	}
	for (const CPath &path : recursiveDirs) {
		wanted.insert("R:" + path.GetRaw());
	}
	return wanted != m_paintedRoots;
}

void CDirectoryTreeCtrl::OnItemExpanding(wxTreeEvent &evt)
{
	wxTreeItemId hItem = evt.GetItem();

	DeleteChildren(hItem);
	AddSubdirectories(hItem, GetFullPath(hItem));

	SortChildren(hItem);
}

void CDirectoryTreeCtrl::OnItemActivated(wxTreeEvent &evt)
{
	if (m_IsRemote) {
		return;
	}
	const wxTreeItemId hItem = evt.GetItem();
	// A descendant of a recursive-share root cannot be individually un-shared from
	// this UI: the apply task re-flattens the root's subtree at commit time, so a
	// left-click here would only un-bold the item and have the entry reappear after
	// Apply. Block it with a message saying what to do instead (drop the recursive
	// marker on the root, or right-click an ancestor).
	if (IsInsideRecursiveShare(GetFullPath(hItem))) {
		wxMessageBox(_("This directory is part of a recursive share. "
			       "To remove it, un-share or modify the recursive "
			       "share root above it."),
			_("Cannot unshare inside a recursive share"),
			wxOK | wxICON_INFORMATION,
			this);
		return;
	}
	CheckChanged(hItem, !IsBold(hItem), false);
	HasChanged = true;
}

void CDirectoryTreeCtrl::OnRButtonDown(wxTreeEvent &evt)
{
	if (m_IsRemote) {
		SelectItem(evt.GetItem()); // looks weird otherwise
		return;
	}

	// Right-click is the "recursive share" gesture. The handler used to eagerly
	// walk the entire subtree, expanding every directory it had never opened, which
	// on large roots like /home produced multi-minute UI freezes with no progress
	// and no cancel (issue #592).
	//
	// The intent is now recorded on the right-clicked item only, and
	// PrefsUnifiedDlg::OnOk flattens the recursive roots into concrete
	// subdirectory paths on a background thread with a progress dialog.
	// Already-expanded descendants are still toggled visually, but nothing new is
	// enumerated: collapsed subtrees keep their visual state and are bolded the
	// next time they are expanded (AddChildItem checks IsInsideRecursiveShare).
	const wxTreeItemId hItem = evt.GetItem();
	const bool wasBold = IsBold(hItem);
	const CPath fullPath = GetFullPath(hItem);

	// A descendant of an existing recursive root is already covered by that root,
	// so its own recursive marker is either redundant or impotent
	// (DelRecursiveShare on a non-root is a no-op). Block both with the same
	// explanatory message.
	if (IsInsideRecursiveShare(fullPath)) {
		wxMessageBox(_("This directory is part of a recursive share. "
			       "To remove it, un-share or modify the recursive "
			       "share root above it."),
			_("Cannot modify inside a recursive share"),
			wxOK | wxICON_INFORMATION,
			this);
		return;
	}

	if (wasBold) {
		// Unshare. Clean both the recursive intent and any m_lstShared entries
		// underneath this path -- the latter removes the flat descendants a previous
		// Prefs session may have committed from a recursive share. An in-memory map
		// sweep catches subdirs that are not currently rendered without expanding
		// them from disk.
		DelRecursiveShare(fullPath);
		DelSharesUnder(fullPath);
	} else {
		AddRecursiveShare(fullPath);
	}

	// Walk only the ALREADY-LOADED descendants, so the in-tree visual stays
	// consistent without forcing disk I/O. CheckChanged inside this walk resets
	// hItem's per-item font to match the new bold state, so the recursive-off path
	// needs no separate font revert.
	MarkChildren(hItem, !wasBold, false);

	if (!wasBold) {
		// Overlay the bold-italic marker so the item carrying the recursive intent
		// is distinguishable from descendants that inherit it as plain bold. After
		// MarkChildren, or CheckChanged's plain-bold per-item font on hItem would
		// clobber the italic.
		ApplyRecursiveMark(hItem, true);
	}
	HasChanged = true;
}

void CDirectoryTreeCtrl::MarkChildren(wxTreeItemId hChild, bool mark, bool recursed)
{
	// Touch only the children ALREADY loaded into the tree control; enumerating
	// collapsed subtrees would re-introduce the unbounded directory walk. They stay
	// as they are and are re-evaluated on demand by AddChildItem the next time the
	// user expands them.
	wxTreeItemIdValue cookie;
	wxTreeItemId hChild2 = GetFirstChild(hChild, cookie);
	if (hChild2.IsOk()) {
		SetHasSharedSubdirectory(hChild, mark);
	}
	while (hChild2.IsOk()) {
		if (IsExpanded(hChild) || ItemHasChildren(hChild2)) {
			MarkChildren(hChild2, mark, true);
		} else {
			CheckChanged(hChild2, mark, true);
		}

		hChild2 = GetNextSibling(hChild2);
	}

	CheckChanged(hChild, mark, recursed);
}

void CDirectoryTreeCtrl::AddChildItem(wxTreeItemId hBranch, const CPath &item)
{
	wxCHECK_RET(hBranch.IsOk(), "Attempted to add children to invalid item");

	CPath fullPath = GetFullPath(hBranch).JoinPaths(item);
	wxTreeItemId treeItem =
		AppendItem(hBranch, item.GetPrintable(), IMAGE_FOLDER, -1, new CItemData(item));

	// BUG: wxGenericTreeControl won't set text calculated sizes when the item is created in AppendItem.
	// This causes asserts on Mac and possibly other systems, so we have to repeat setting the string
	// here.
	SetItemText(treeItem, item.GetPrintable());

	// Bold means "this directory is part of the pending share set", covering both
	// the explicit case (m_lstShared) and a descendant of a recursive-share root
	// (m_lstSharedRecursive). The latter is what keeps the tree consistent after a
	// right-click whose expansion is deferred to commit time: the subtree is bolded
	// lazily as the user opens it.
	const bool isRecursiveRoot = IsRecursiveShare(fullPath);
	if (IsShared(fullPath) || isRecursiveRoot || IsInsideRecursiveShare(fullPath)) {
		SetItemBold(treeItem, true);
	}
	// A recursive root gets bold-italic, so it is distinguishable from plain
	// explicit shares and from descendants covered by an inherited expansion.
	if (isRecursiveRoot) {
		ApplyRecursiveMark(treeItem, true);
	}

	if (HasSharedSubdirectory(fullPath)) {
		SetHasSharedSubdirectory(treeItem, true);
	}

	if (HasSubdirectories(fullPath)) {
		// Trick. will show + if it has subdirs
		AppendItem(treeItem, ".");
	}
}

CPath CDirectoryTreeCtrl::GetFullPath(wxTreeItemId hItem)
{
	{
		wxCHECK_MSG(hItem.IsOk(), CPath(), "Invalid item in GetFullPath");
	}

	CPath result;
	for (; hItem.IsOk(); hItem = GetItemParent(hItem)) {
		CItemData *data = dynamic_cast<CItemData *>(GetItemData(hItem));
		wxCHECK_MSG(data, CPath(), "Missing data-item in GetFullPath");

		result = data->GetPathComponent().JoinPaths(result);
	}

	return result;
}

void CDirectoryTreeCtrl::AddSubdirectories(wxTreeItemId hBranch, const CPath &path)
{
	wxCHECK_RET(path.IsOk(), "Invalid path in AddSubdirectories");

	CDirIterator sharedDir(path);

	CPath dirName = sharedDir.GetFirstFile(CDirIterator::Dir);
	while (dirName.IsOk()) {
		AddChildItem(hBranch, dirName);

		dirName = sharedDir.GetNextFile();
	}
}

bool CDirectoryTreeCtrl::HasSubdirectories(const CPath &folder)
{
	// Prevent error-messages if we try to traverse somewhere we have no access.
	wxLogNull logNo;

	return CDirIterator(folder).HasSubDirs();
}

void CDirectoryTreeCtrl::GetSharedDirectories(PathList *list)
{
	wxCHECK_RET(list, "Invalid list in GetSharedDirectories");

	for (SharedMap::iterator it = m_lstShared.begin(); it != m_lstShared.end(); ++it) {
		list->push_back(it->second);
	}
}

void CDirectoryTreeCtrl::SetSharedDirectories(PathList *list)
{
	wxCHECK_RET(list, "Invalid list in SetSharedDirectories");

	m_lstShared.clear();
	for (PathList::iterator it = list->begin(); it != list->end(); ++it) {
		m_lstShared.insert(SharedMapItem(GetKey(*it), *it));
	}

	if (m_IsInit) {
		UpdateSharedDirectories();
	}
}

void CDirectoryTreeCtrl::GetRecursiveSharedDirectories(PathList *list)
{
	wxCHECK_RET(list, "Invalid list in GetRecursiveSharedDirectories");

	for (SharedMap::iterator it = m_lstSharedRecursive.begin(); it != m_lstSharedRecursive.end(); ++it) {
		list->push_back(it->second);
	}
}

void CDirectoryTreeCtrl::SetRecursiveSharedDirectories(PathList *list)
{
	wxCHECK_RET(list, "Invalid list in SetRecursiveSharedDirectories");

	m_lstSharedRecursive.clear();
	for (PathList::iterator it = list->begin(); it != list->end(); ++it) {
		m_lstSharedRecursive.insert(SharedMapItem(GetKey(*it), *it));
	}

	// Mirror SetSharedDirectories: refresh the tree so a recursive root reloaded
	// from shareddir-recursive.dat at prefs-open is bold straight away rather than
	// only once the user expands its branch. PrefsUnifiedDlg calls both
	// back-to-back, and without this only the explicit set is visualised.
	if (m_IsInit) {
		UpdateSharedDirectories();
	}
}

wxString CDirectoryTreeCtrl::GetKey(const CPath &path)
{
	if (m_IsRemote) {
		return path.GetRaw();
	}

	// Sanity check, see IsSameAs() in Path.cpp. Skip wxGetCwd() when the path is
	// already absolute: Normalize ignores cwd then, and wxGetCwd() emits a
	// wxLogSysError on every call once the process's recorded CWD is gone.
	wxString cwd;
	wxFileName fn(path.GetRaw());
	if (!fn.IsAbsolute()) {
		cwd = wxGetCwd();
	}
	// wxPATH_NORM_ALL is deprecated in wx3 — use explicit flags instead (excluding wxPATH_NORM_ENV_VARS)
	const int flags = wxPATH_NORM_DOTS | wxPATH_NORM_TILDE | wxPATH_NORM_CASE | wxPATH_NORM_ABSOLUTE |
			  wxPATH_NORM_LONG | wxPATH_NORM_SHORTCUT;
	fn.Normalize(flags, cwd);
	return fn.GetFullPath();
}

void CDirectoryTreeCtrl::UpdateSharedDirectories()
{
	// ugly hack to at least show shared dirs in remote gui
	if (m_IsRemote) {
		DeleteChildren(m_root);
		for (SharedMap::iterator it = m_lstShared.begin(); it != m_lstShared.end(); ++it) {
			AppendItem(m_root,
				it->second.GetPrintable(),
				IMAGE_FOLDER,
				-1,
				new CItemData(it->second));
		}
		return;
	}

	// Mark all shared root items (on windows this can be multiple
	// drives, on unix there is only the root dir).
	wxTreeItemIdValue cookie;
	wxTreeItemId hChild = GetFirstChild(GetRootItem(), cookie);

	while (hChild.IsOk()) {
		if (HasSharedSubdirectory(GetFullPath(hChild))) {
			SetHasSharedSubdirectory(hChild, true);
		}

		if (IsShared(GetFullPath(hChild))) {
			SetItemBold(hChild, true);
		}

		hChild = GetNextSibling(hChild);
	}
}

bool CDirectoryTreeCtrl::HasSharedSubdirectory(const CPath &path)
{
	// 1. An explicit-share entry lives below `path`: the "user ticked
	// /Pictures/2024" case, where /Pictures should advertise a shared subdir.
	{
		SharedMap::iterator it =
			m_lstShared.upper_bound(GetKey(path) + wxFileName::GetPathSeparator());
		if (it != m_lstShared.end() && it->second.StartsWith(path)) {
			return true;
		}
	}

	// 2. `path` itself is, or sits below, a recursive root, which implicitly shares
	// every descendant. Without this branch a recursive-only root loaded from
	// shareddir-recursive.dat would show plain bold without the "has shared
	// subdirs" icon, because its descendants live only in the recursive expansion.
	if (IsRecursiveShare(path) || IsInsideRecursiveShare(path)) {
		return true;
	}

	// 3. A recursive root sits below `path`, so some descendant is shared via that
	// root and the icon should be on.
	{
		SharedMap::iterator it =
			m_lstSharedRecursive.upper_bound(GetKey(path) + wxFileName::GetPathSeparator());
		if (it != m_lstSharedRecursive.end() && it->second.StartsWith(path)) {
			return true;
		}
	}

	return false;
}

void CDirectoryTreeCtrl::SetHasSharedSubdirectory(wxTreeItemId hItem, bool add)
{
	SetItemImage(hItem, add ? IMAGE_FOLDER_SUB_SHARED : IMAGE_FOLDER);
}

void CDirectoryTreeCtrl::CheckChanged(wxTreeItemId hItem, bool bChecked, bool recursed)
{
	if (IsBold(hItem) != bChecked) {
		SetItemBold(hItem, bChecked);
		// Mirror the bold state into the per-item font. wxMSW honours a per-item
		// font over TVIS_BOLD, so leaving a plain-font override in place would make a
		// later SetItemBold(true) render as plain (#827 fallout). No-op on
		// wxGTK/wxOSX, where TVIS_BOLD overlays the per-item font.
		SetItemFont(hItem, bChecked ? GetFont().Bold() : GetFont());

		const CPath fullPath = GetFullPath(hItem);
		bool wasRecursive = false;
		if (bChecked) {
			AddShare(fullPath);
		} else {
			DelShare(fullPath);
			// Double-clicking a recursive-share root must also drop the recursive
			// intent, or the expansion task would re-flatten the subtree at commit
			// time and the files would reappear in the shared list. Calls from
			// MarkChildren on descendants are harmless no-ops: only the root is
			// keyed in m_lstSharedRecursive.
			wasRecursive = IsRecursiveShare(fullPath);
			DelRecursiveShare(fullPath);
		}

		if (!recursed) {
			UpdateParentItems(hItem, bChecked);
			// Dropping the recursive marker leaves the already-rendered descendants
			// painted bold from the original AddChildItem pass, which nothing
			// re-evaluates -- a "ghost selection" of bold subdirs. Walk them and
			// unbold to match the now-empty state.
			if (!bChecked && wasRecursive) {
				MarkChildren(hItem, false, true);
			}
		}
	}
}

bool CDirectoryTreeCtrl::IsShared(const CPath &path)
{
	wxCHECK_MSG(path.IsOk(), false, "Invalid path in IsShared");

	return m_lstShared.find(GetKey(path)) != m_lstShared.end();
}

void CDirectoryTreeCtrl::AddShare(const CPath &path)
{
	wxCHECK_RET(path.IsOk(), "Invalid path in AddShare");

	if (IsShared(path)) {
		return;
	}

	m_lstShared.insert(SharedMapItem(GetKey(path), path));
}

void CDirectoryTreeCtrl::DelShare(const CPath &path)
{
	wxCHECK_RET(path.IsOk(), "Invalid path in DelShare");

	m_lstShared.erase(GetKey(path));
}

bool CDirectoryTreeCtrl::IsRecursiveShare(const CPath &path)
{
	return m_lstSharedRecursive.find(GetKey(path)) != m_lstSharedRecursive.end();
}

bool CDirectoryTreeCtrl::IsInsideRecursiveShare(const CPath &path)
{
	// True iff `path` is a strict descendant of any recursive-share
	// root. Used by AddChildItem to bold subtree items when the tree
	// is expanded long after the right-click that set the intent.
	if (m_lstSharedRecursive.empty() || !path.IsOk()) {
		return false;
	}
	const wxString key = GetKey(path);
	for (SharedMap::const_iterator it = m_lstSharedRecursive.begin(); it != m_lstSharedRecursive.end();
		++it) {
		const wxString rootKey = it->first;
		if (key.length() > rootKey.length() && key.StartsWith(rootKey) &&
			(rootKey.empty() || rootKey.Last() == wxFileName::GetPathSeparator() ||
				key[rootKey.length()] == wxFileName::GetPathSeparator())) {
			return true;
		}
	}
	return false;
}

void CDirectoryTreeCtrl::AddRecursiveShare(const CPath &path)
{
	wxCHECK_RET(path.IsOk(), "Invalid path in AddRecursiveShare");

	const wxString key = GetKey(path);
	m_lstSharedRecursive.insert(SharedMapItem(key, path));
}

void CDirectoryTreeCtrl::DelRecursiveShare(const CPath &path)
{
	wxCHECK_RET(path.IsOk(), "Invalid path in DelRecursiveShare");

	m_lstSharedRecursive.erase(GetKey(path));
}

void CDirectoryTreeCtrl::DelSharesUnder(const CPath &root)
{
	if (!root.IsOk() || m_lstShared.empty()) {
		return;
	}

	// Compare on the normalized form, with a trailing separator so /home does not
	// also match /home2. A root that already ends in one (the Windows drive root
	// "C:\") is used as-is.
	wxString prefix = GetKey(root);
	if (prefix.empty()) {
		return;
	}
	if (prefix.Last() != wxFileName::GetPathSeparator()) {
		prefix += wxFileName::GetPathSeparator();
	}

	for (SharedMap::iterator it = m_lstShared.begin(); it != m_lstShared.end();) {
		if (it->first.StartsWith(prefix)) {
			it = m_lstShared.erase(it);
		} else {
			++it;
		}
	}
}

void CDirectoryTreeCtrl::UpdateParentItems(wxTreeItemId hChild, bool add)
{
	wxTreeItemId parent = hChild;
	while (parent != GetRootItem()) {
		parent = GetItemParent(parent);
		if (add) {
			if (GetItemImage(parent) == IMAGE_FOLDER_SUB_SHARED) {
				// parent already marked -> so are all its parents, finished
				break;
			} else {
				SetHasSharedSubdirectory(parent, true);
			}
		} else {
			if (GetItemImage(parent) == IMAGE_FOLDER_SUB_SHARED) {
				// check if now there are still other shared dirs
				if (HasSharedSubdirectory(GetFullPath(parent))) {
					// yes, then further parents can stay red
					break;
				} else {
					// no, further parents have to be checked too
					SetHasSharedSubdirectory(parent, false);
				}
			} else { // should not happen (unmark child of which the parent is already unmarked
				break;
			}
		}
	}
}
// File_checked_for_headers
