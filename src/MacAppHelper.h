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

// Toggles NSApp's activation policy. True switches the app to
// NSApplicationActivationPolicyAccessory (no Dock icon, menu-bar / NSStatusItem only); false
// restores NSApplicationActivationPolicyRegular. Used to remove the Dock thumbnail when the main
// window is hidden via "minimize to tray".
void mac_set_accessory_mode(bool accessory);

// Returns the vertical centre of the tab buttons of the NSTabView backing the passed wxWindow
// handle, in the coordinates of the tab view's superview (top-left origin, points). Negative if
// the handle is not a tab view or it has no tab buttons.
double mac_tab_view_buttons_mid_y(void *windowHandle);

// Reveals `path` in Finder, selected in its containing folder.
//
// Goes through NSWorkspace rather than spawning `open -R`: LaunchServices does the work out of
// process, so there is no subprocess to inherit anything from aMule, and the file ends up selected
// rather than merely having its folder opened. False when the path is gone.
bool mac_reveal_in_finder(const char *path);

#ifdef __cplusplus
}
#endif

#endif // __WXMAC__

#endif // MAC_APP_HELPER_H
