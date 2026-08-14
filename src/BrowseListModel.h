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

#ifndef BROWSELISTMODEL_H
#define BROWSELISTMODEL_H

#include "SearchListModel.h"

#include <map>    // Needed for std::map (the folder registry)
#include <memory> // Needed for std::unique_ptr (stable folder node addresses)

/**
 * A folder in a browsed peer's shared-file listing.
 *
 * Not a CSearchFile: it has no hash, no size and no sources, and every
 * operation the search list offers on a result is meaningless for it. It
 * exists only to be a wxDataViewItem the control can draw and expand.
 */
class CBrowseFolderNode
{
public:
	explicit CBrowseFolderNode(const wxString &name)
	: m_name(name)
	{
	}

	const wxString &GetName() const noexcept { return m_name; }

	//! The results filed under this folder, refreshed on every rebuild.
	std::vector<CSearchFile *> m_files;

private:
	wxString m_name;
};

/**
 * The model behind a browse tab: the same result list as a search, grouped
 * under the directories the peer reported.
 *
 * Extends rather than alters CSearchListModel, because a browse is the only
 * place folders exist -- an ordinary Kad/eD2k search has no directory to
 * group by, and nothing about its model should have to know that folders are
 * a possibility.
 *
 * The grouping is one level deep, one node per distinct directory string.
 * The protocol hands us a set of directory names (OP_ASKSHAREDFILESDIRANS,
 * one response per directory), not a hierarchy, and the names are the remote
 * host's own -- splitting them into a deeper tree would mean guessing at a
 * path separator that may be '\', '/', or neither.
 *
 * Peers that answer the older whole-list request send no directory at all
 * (ClientTCPSocket, OP_ASKSHAREDFILESANSWER passes an empty string). Those
 * results have no folder to sit under and stay at the top level, so browsing
 * such a peer looks exactly like it did before folders existed.
 */
class CBrowseListModel : public CSearchListModel
{
public:
	explicit CBrowseListModel(CSearchListCtrl *owner);

	bool IsFolder(const wxDataViewItem &item) const wxOVERRIDE;

	// wxDataViewModel interface. Each of these has to answer for folder
	// items, which the base class would hand to ToFile() and read as a
	// CSearchFile.
	void GetValue(wxVariant &variant, const wxDataViewItem &item, unsigned int col) const wxOVERRIDE;
	bool GetAttr(const wxDataViewItem &item, unsigned int col, wxDataViewItemAttr &attr) const wxOVERRIDE;
	wxDataViewItem GetParent(const wxDataViewItem &item) const wxOVERRIDE;
	bool IsContainer(const wxDataViewItem &item) const wxOVERRIDE;
	//! False for a folder: it is a section header with a name and nothing
	//! else, unlike a grouped result, which is a row in its own right.
	bool HasContainerColumns(const wxDataViewItem &item) const wxOVERRIDE;
	unsigned int GetChildren(const wxDataViewItem &item, wxDataViewItemArray &children) const wxOVERRIDE;
	int Compare(const wxDataViewItem &item1,
		const wxDataViewItem &item2,
		unsigned int column,
		bool ascending) const wxOVERRIDE;

private:
	static CBrowseFolderNode *ToFolder(const wxDataViewItem &item)
	{
		return static_cast<CBrowseFolderNode *>(item.GetID());
	}
	static wxDataViewItem ToItem(const CBrowseFolderNode *folder)
	{
		return wxDataViewItem(const_cast<CBrowseFolderNode *>(folder));
	}

	/**
	 * Re-files the current results under their directories.
	 *
	 * Called from the const tree accessors, because the model deliberately
	 * keeps no copy of the result set (see CSearchListModel) and the folders
	 * are derived from whatever it holds right now.
	 *
	 * Nodes are keyed by directory name and never erased while the model
	 * lives: the control may still be holding an item for a folder whose
	 * last result just went away, and that pointer has to stay readable. A
	 * node with no files is simply not reported as a child.
	 */
	void RebuildFolders() const;

	//! Directory name -> node. unique_ptr so that a rebuild that inserts a
	//! new directory cannot move the existing nodes: the control holds their
	//! addresses as item IDs.
	mutable std::map<wxString, std::unique_ptr<CBrowseFolderNode>> m_folders;

	//! Every address in m_folders, for IsFolder(). The item is a bare void*
	//! with nothing to distinguish it, so membership here is what makes the
	//! difference between a folder and a result answerable at all.
	mutable std::set<const void *> m_folderAddresses;

	//! Results the peer reported with no directory. Top level, next to the
	//! folders.
	mutable std::vector<CSearchFile *> m_looseFiles;
};

#endif // BROWSELISTMODEL_H
// File_checked_for_headers
