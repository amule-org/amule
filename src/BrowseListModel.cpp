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

#include "BrowseListModel.h"

#include "SearchFile.h"
#include "SearchList.h"
#include "SearchListCtrl.h"
#include "amule.h"

CBrowseListModel::CBrowseListModel(CSearchListCtrl *owner)
: CSearchListModel(owner)
{
}

bool CBrowseListModel::IsFolder(const wxDataViewItem &item) const
{
	return item.IsOk() && m_folderAddresses.count(item.GetID()) != 0;
}

CBrowseFolderNode *CBrowseListModel::EnsureFolder(const wxString &path) const
{
	const auto existing = m_folders.find(path);
	if (existing != m_folders.end()) {
		return existing->second.get();
	}

	// Split off the last segment: whichever separator appears last is the
	// one this peer uses, so a name containing the other one stays intact.
	const int slash = static_cast<int>(path.find_last_of("/\\"));
	CBrowseFolderNode *parent = NULL;
	wxString name = path;
	if (slash != static_cast<int>(wxString::npos)) {
		name = path.Mid(slash + 1);
		const wxString parentPath = path.Left(slash);
		// A leading or doubled separator leaves an empty parent path,
		// which is no directory at all -- this node is a root.
		if (!parentPath.IsEmpty()) {
			parent = EnsureFolder(parentPath);
		}
	}

	CBrowseFolderNode *node = m_folders.emplace(path, std::make_unique<CBrowseFolderNode>(name, parent))
					  .first->second.get();
	m_folderAddresses.insert(node);
	if (!parent) {
		m_rootFolders.push_back(node);
	}
	return node;
}

void CBrowseListModel::RebuildFolders() const
{
	// Once per change, not once per query. The control asks for the root's
	// children several times per rebuild -- twice from OnIdleHook alone, to
	// save and restore expansion around a Cleared() -- and this walks every
	// result, which on a large share is the cost the browse throttle exists
	// to keep down (issue #898).
	const unsigned generation = GetContentGeneration();
	const wxUIntPtr searchId = m_owner->GetSearchId();
	if (m_foldersValid && m_foldersGeneration == generation && m_foldersSearchId == searchId) {
		return;
	}
	m_foldersValid = true;
	m_foldersGeneration = generation;
	m_foldersSearchId = searchId;

	for (auto &entry : m_folders) {
		entry.second->m_files.clear();
		entry.second->m_subfolders.clear();
	}
	m_looseFiles.clear();

	if (!m_owner->GetSearchId()) {
		return;
	}

	const CSearchResultList &results = theApp->searchlist->GetSearchResults(m_owner->GetSearchId());
	for (CSearchFile *file : results) {
		if (file->GetParent() || !m_owner->ShouldShow(file)) {
			continue;
		}

		const wxString &directory = file->GetDirectory();
		if (directory.IsEmpty()) {
			m_looseFiles.push_back(file);
			continue;
		}

		EnsureFolder(directory)->m_files.push_back(file);
	}

	// Relink the hierarchy. Done as a second pass rather than while
	// creating the nodes, because the links are cleared on every rebuild
	// while the nodes outlive it: a folder created for an earlier refresh
	// still has to be reattached to its parent on this one.
	//
	// Unconditionally, empty or not. Whether a folder is worth drawing
	// depends on what is under it at any depth, which is not knowable until
	// every link exists -- so that question is asked at GetChildren() time
	// instead, once the tree is whole.
	for (const auto &entry : m_folders) {
		CBrowseFolderNode *node = entry.second.get();
		if (node->GetParent()) {
			node->GetParent()->m_subfolders.push_back(node);
		}
	}
}

bool CBrowseListModel::HasContent(const CBrowseFolderNode *folder)
{
	if (!folder->m_files.empty()) {
		return true;
	}
	for (const CBrowseFolderNode *sub : folder->m_subfolders) {
		if (HasContent(sub)) {
			return true;
		}
	}
	return false;
}

unsigned int CBrowseListModel::GetChildren(const wxDataViewItem &item, wxDataViewItemArray &children) const
{
	if (!item.IsOk()) {
		RebuildFolders();

		unsigned int count = 0;
		for (CBrowseFolderNode *folder : m_rootFolders) {
			// A folder whose results have all gone away, or are all
			// filtered out, is not drawn -- its node stays allocated so
			// that any item the control still holds remains readable.
			if (HasContent(folder)) {
				children.Add(ToItem(folder));
				++count;
			}
		}
		for (CSearchFile *file : m_looseFiles) {
			children.Add(CSearchListModel::ToItem(file));
			++count;
		}
		return count;
	}

	if (IsFolder(item)) {
		const CBrowseFolderNode *folder = ToFolder(item);
		unsigned int count = 0;
		for (CBrowseFolderNode *sub : folder->m_subfolders) {
			if (HasContent(sub)) {
				children.Add(ToItem(sub));
				++count;
			}
		}
		for (CSearchFile *file : folder->m_files) {
			children.Add(CSearchListModel::ToItem(file));
			++count;
		}
		return count;
	}

	// A result under a folder still groups its own alternative sources.
	return CSearchListModel::GetChildren(item, children);
}

wxDataViewItem CBrowseListModel::GetParent(const wxDataViewItem &item) const
{
	if (!item.IsOk()) {
		return wxDataViewItem();
	}

	if (IsFolder(item)) {
		CBrowseFolderNode *parent = ToFolder(item)->GetParent();
		return parent ? ToItem(parent) : wxDataViewItem();
	}

	CSearchFile *file = ToFile(item);
	if (file->GetParent()) {
		return CSearchListModel::GetParent(item);
	}

	const wxString &directory = file->GetDirectory();
	if (directory.IsEmpty()) {
		return wxDataViewItem();
	}

	const auto it = m_folders.find(directory);
	return it != m_folders.end() ? ToItem(it->second.get()) : wxDataViewItem();
}

bool CBrowseListModel::IsContainer(const wxDataViewItem &item) const
{
	if (!item.IsOk()) {
		return true; // invisible root
	}
	if (IsFolder(item)) {
		return true;
	}
	return CSearchListModel::IsContainer(item);
}

bool CBrowseListModel::HasContainerColumns(const wxDataViewItem &item) const
{
	if (IsFolder(item)) {
		return false;
	}
	return CSearchListModel::HasContainerColumns(item);
}

void CBrowseListModel::GetValue(wxVariant &variant, const wxDataViewItem &item, unsigned int col) const
{
	if (!IsFolder(item)) {
		CSearchListModel::GetValue(variant, item, col);
		return;
	}

	// Every column has a type the control will try to read, so a folder has
	// to answer for all of them even though it only has a name.
	switch (col) {
	case COL_NAME:
		variant = ToFolder(item)->GetName();
		break;

	case COL_RATING:
		variant << wxDataViewIconText(wxEmptyString, wxIcon());
		break;

	default:
		variant = wxEmptyString;
		break;
	}
}

bool CBrowseListModel::GetAttr(const wxDataViewItem &item, unsigned int col, wxDataViewItemAttr &attr) const
{
	if (!IsFolder(item)) {
		return CSearchListModel::GetAttr(item, col, attr);
	}

	// The colours the base model picks encode a result's download state,
	// which a folder does not have. Bold instead, so the grouping reads as
	// structure rather than as a result in some unexplained state.
	attr.SetBold(true);
	return true;
}

int CBrowseListModel::Compare(
	const wxDataViewItem &item1, const wxDataViewItem &item2, unsigned int column, bool ascending) const
{
	const bool folder1 = IsFolder(item1);
	const bool folder2 = IsFolder(item2);

	if (folder1 != folder2) {
		// Folders first, whatever the sort column and direction: they are
		// the structure the results sit in, and interleaving them with the
		// loose results would read as an ordering accident.
		return folder1 ? -1 : 1;
	}

	if (folder1) {
		const int result = ToFolder(item1)->GetName().CmpNoCase(ToFolder(item2)->GetName());
		return ascending ? result : -result;
	}

	return CSearchListModel::Compare(item1, item2, column, ascending);
}
// File_checked_for_headers
