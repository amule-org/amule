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

namespace
{
//! Canonical form of a peer's directory string: runs of separators collapsed
//! to one, and a trailing separator dropped.
//!
//! The nodes are keyed by this string, so anything left un-normalised is a
//! spelling that gets its own folder: "Shared/Movies", "Shared//Movies" and
//! "Shared/Movies/" all name one directory, and keying them apart would draw
//! a sibling row per spelling, all three showing "Movies". A trailing
//! separator additionally leaves the last segment empty, which is the blank
//! row. A string of nothing but separators normalises to empty, which is no
//! directory at all.
//!
//! Which separator survives a mixed run is whichever came first; the peer's
//! own choice is not knowable from the packet either way.
//!
//! Collapsing runs also rewrites the one string where a doubled separator
//! means something: a UNC name arrives as "\\server\share" and is keyed as
//! "\server\share". It is a label here and nothing resolves it, so this
//! costs a backslash in a folder row rather than a wrong lookup.
wxString NormalizeDirectory(const wxString &path)
{
	wxString normalized;
	normalized.reserve(path.length());

	bool lastWasSeparator = false;
	for (const wxUniChar ch : path) {
		const bool separator = (ch == '/' || ch == '\\');
		if (separator && lastWasSeparator) {
			continue;
		}
		normalized += ch;
		lastWasSeparator = separator;
	}

	if (lastWasSeparator && !normalized.IsEmpty()) {
		normalized.RemoveLast();
	}
	return normalized;
}
} // namespace

CBrowseListModel::CBrowseListModel(CSearchListCtrl *owner)
: CSearchListModel(owner)
{
}

bool CBrowseListModel::IsFolder(const wxDataViewItem &item) const
{
	return item.IsOk() && m_folderAddresses.count(item.GetID()) != 0;
}

CBrowseFolderNode *CBrowseListModel::EnsureFolder(const wxString &path, size_t depth) const
{
	const auto existing = m_folders.find(path);
	if (existing != m_folders.end()) {
		return existing->second.get();
	}

	// Split off the last segment: whichever separator appears last is the
	// one this peer uses, so a name containing the other one stays intact.
	//
	// Two bounds apply, and they stop different things. The depth argument
	// caps this function's own recursion, which one long string drives. The
	// node's depth caps the tree, which many strings drive between them: the
	// early return above hands back a node with the parents it already has,
	// so without that check a peer could add a capped run per packet and
	// nest for as long as it kept sending. Where either bites, the remainder
	// stays one folder name instead of being split further.
	const size_t slash = path.find_last_of("/\\");
	CBrowseFolderNode *parent = nullptr;
	wxString name = path;
	if (slash != wxString::npos && depth < MAX_FOLDER_DEPTH) {
		// Already canonical, and so is every prefix of it: the callers
		// normalise, so there is no run to collapse here and the last
		// separator cannot be trailing.
		const wxString parentPath = path.Left(slash);
		if (parentPath.IsEmpty()) {
			// A leading separator leaves no parent path, which is no
			// directory at all -- this node is a root under its own
			// last segment.
			name = path.Mid(slash + 1);
		} else {
			CBrowseFolderNode *candidate = EnsureFolder(parentPath, depth + 1);
			if (candidate->GetDepth() + 1 < MAX_FOLDER_DEPTH) {
				parent = candidate;
				name = path.Mid(slash + 1);
			}
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
	m_fileParents.clear();

	if (!m_owner->GetSearchId()) {
		return;
	}

	const CSearchResultList &results = theApp->searchlist->GetSearchResults(m_owner->GetSearchId());
	for (CSearchFile *file : results) {
		if (file->GetParent() || !m_owner->ShouldShow(file)) {
			continue;
		}

		// The one place a directory string is normalised, because this is
		// the one place it becomes a key. GetParent() reads the answer
		// back from m_fileParents rather than deriving it again, so there
		// is no second spelling of this to keep in step.
		const wxString directory = NormalizeDirectory(file->GetDirectory());
		if (directory.IsEmpty()) {
			m_looseFiles.push_back(file);
			continue;
		}

		CBrowseFolderNode *folder = EnsureFolder(directory);
		folder->m_files.push_back(file);
		m_fileParents.emplace(file, folder);
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
		// Here too, not only on the root branch: this is the path that
		// reads the cached CSearchFile pointers, so it is the one that has
		// to see a generation bump. The call is a comparison once the
		// grouping is current.
		RebuildFolders();

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

	// Read back from the grouping walk rather than re-derived from the
	// directory string. This is the traversal path and wx asks per item, so
	// it does no string work at all: RebuildFolders() knew the node when it
	// filed the result, and a result the peer gave no directory is simply
	// absent, which is the invisible root.
	RebuildFolders();

	const auto it = m_fileParents.find(file);
	return it != m_fileParents.end() ? ToItem(it->second) : wxDataViewItem();
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
