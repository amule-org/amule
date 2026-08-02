//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#ifndef MAC_APP_HELPER_H
#define MAC_APP_HELPER_H

#ifdef __WXMAC__

#ifdef __cplusplus
extern "C" {
#endif

// Toggles NSApp's activation policy. Passing true switches the app to
// NSApplicationActivationPolicyAccessory (no Dock icon, menu-bar /
// NSStatusItem only); false restores NSApplicationActivationPolicyRegular
// (normal Dock icon). Used to remove the Dock thumbnail when the main
// window is hidden via "minimize to tray".
void mac_set_accessory_mode(bool accessory);

// Drops the inset row style from the NSTableView (or NSOutlineView) backing
// the passed wxWindow handle, restoring the flush one. macOS 11 made the
// inset style the default: it is what draws a selected row as a rounded
// pill, and it reserves padding at both ends of every row, whether or not
// the row is the selected one. The padding comes out of the cell the label
// is drawn in, so a list whose width is derived from its longest label
// ellipsises that label no matter how wide the column is made. Does nothing
// if the handle is not backed by a table view, or below macOS 11.
void mac_set_table_view_flush(void *windowHandle);

#ifdef __cplusplus
}
#endif

#endif // __WXMAC__

#endif // MAC_APP_HELPER_H
