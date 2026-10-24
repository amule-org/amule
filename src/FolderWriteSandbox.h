// This file is part of aMule. Licensed under GPL-2.0-or-later.
#ifndef AMULE_FOLDERWRITESANDBOX_H
#define AMULE_FOLDERWRITESANDBOX_H
#include <string>
#include <vector>

namespace FolderWriteSandbox
{
// Irreversible for this process and its children. Files remain readable according
// to the surrounding Flatpak sandbox; mutations require an explicitly allowed root.
bool Restrict(const std::vector<std::string> &writableRoots, std::string &error);
} // namespace FolderWriteSandbox
#endif
