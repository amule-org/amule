// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 aMule Team

#include "MenuIcons.h"

#include <wx/artprov.h>
#include <wx/bmpbndl.h>

wxMenuItem *AppendMenuIcon(wxMenu *menu, int id, const wxString &label, MenuIcon icon)
{
	const char *name = nullptr;
	switch (icon) {
	case MenuIcon::Pause:
		name = "pause_fill";
		break;
	case MenuIcon::Resume:
		name = "play_fill";
		break;
	case MenuIcon::Stop:
		name = "stop_fill";
		break;
	case MenuIcon::Cancel:
		name = "x_lg";
		break;
	case MenuIcon::Folder:
		name = "folder2_open";
		break;
	case MenuIcon::Info:
		name = "info_circle";
		break;
	case MenuIcon::Link:
		name = "link_45deg";
		break;
	case MenuIcon::Preview:
		name = "eye";
		break;
	case MenuIcon::Comments:
		name = "chat_left_text";
		break;
	case MenuIcon::ClearCompleted:
		name = "check2_all";
		break;
	case MenuIcon::Download:
		name = "download";
		break;
	case MenuIcon::Open:
		name = "file_earmark";
		break;
	}
	auto *item = new wxMenuItem(menu, id, label);
	const wxBitmapBundle bitmap =
		wxArtProvider::GetBitmapBundle(wxString("amule:menu_") + name, wxART_MENU, wxSize(16, 16));
	if (bitmap.IsOk()) {
		// Set the image before insertion; native ports own layout and disabled states.
		// Do not override GTK's menu-image preference or replace native checkmarks.
		item->SetBitmap(bitmap);
	}
	return menu->Append(item);
}
