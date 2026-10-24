// This file is part of aMule. Licensed under GPL-2.0-or-later.
#ifndef AMULE_FOLDERACCESS_H
#define AMULE_FOLDERACCESS_H

#include <vector>
#include <common/Path.h>

class wxWindow;
class wxFileConfig;
class wxMBConv;

namespace FolderAccess
{
bool IsSandboxed();
// Applied before loading downloads or shared content. Failure stops startup.
bool RestrictWrites(wxString &error);
// Atomic owner-only profile writes which need no metadata mutation after startup.
bool SaveConfig(wxFileConfig &config, const wxString &path, const wxMBConv &conversion);
bool BackupConfig(const wxString &path);
// Original host location of a portal path, retained for safe reauthorization.
wxString HostPath(const wxString &path);
// Approved roots remain available independently of whether they are shared.
std::vector<CPath> ApprovedFolders();
#ifndef AMULE_DAEMON
// In Flatpak, always use the host portal. A typed path is only a chooser hint;
// it never bypasses the system's approval. Empty result means cancelled or failed.
wxString Choose(wxWindow *parent, const wxString &title, const wxString &hint, bool writable);
#endif
} // namespace FolderAccess
#endif
