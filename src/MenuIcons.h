// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 aMule Team

#ifndef AMULE_MENUICONS_H
#define AMULE_MENUICONS_H

#include <wx/menu.h>

// Semantic names keep a command's artwork consistent across context menus.
enum class MenuIcon
{
	Pause,
	Resume,
	Stop,
	Cancel,
	Folder,
	Info,
	Link,
	Preview,
	Comments,
	ClearCompleted,
	Download,
	Open
};

wxMenuItem *AppendMenuIcon(wxMenu *menu, int id, const wxString &label, MenuIcon icon);

#endif
