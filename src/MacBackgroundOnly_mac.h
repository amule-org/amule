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

#ifndef MAC_BACKGROUND_ONLY_MAC_H
#define MAC_BACKGROUND_ONLY_MAC_H

#ifdef __WXMAC__

#ifdef __cplusplus
extern "C" {
#endif

// Declares this process background-only to LaunchServices.
//
// A helper executable that lives inside a .app bundle (amuleapi, amuleweb) inherits that bundle's
// identity, and as soon as it connects to the window server LaunchServices registers it as a
// Foreground app of the bundle: a second Dock tile that keeps bouncing, because a daemon never
// completes an application launch. wxEntryStart() opens that connection even in a wxUSE_GUI=0
// build, so this has to run before wxEntry(). Background-only drops the Dock tile and the menu
// bar; the process keeps running, logging and serving HTTP as usual.
//
// False if LaunchServices refused the transform; callers carry on either way.
bool mac_declare_background_only();

#ifdef __cplusplus
}
#endif

#endif // __WXMAC__

#endif // MAC_BACKGROUND_ONLY_MAC_H
