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

#import <AppKit/AppKit.h>

#include "MacAppHelper.h"

extern "C" void mac_set_table_view_flush(void *windowHandle)
{
	if (!windowHandle) {
		return;
	}

	// wxOSX hands back whichever view it made the peer from: the scroll
	// view for a scrolled control, the table itself otherwise.
	NSView *view = (NSView *)windowHandle;
	NSTableView *table = nil;
	if ([view isKindOfClass:[NSScrollView class]]) {
		NSView *doc = [(NSScrollView *)view documentView];
		if ([doc isKindOfClass:[NSTableView class]]) {
			table = (NSTableView *)doc;
		}
	} else if ([view isKindOfClass:[NSTableView class]]) {
		table = (NSTableView *)view;
	}

	if (!table) {
		return;
	}
	if (@available(macOS 11.0, *)) {
		table.style = NSTableViewStyleFullWidth;
	}
}

extern "C" void mac_set_accessory_mode(bool accessory)
{
	[NSApp setActivationPolicy:
		accessory ? NSApplicationActivationPolicyAccessory
		          : NSApplicationActivationPolicyRegular];

	if (!accessory) {
		// Restoring from Accessory: switching policy gives us a
		// Dock icon back but doesn't make the app active, so any
		// subsequent Show(true)/Raise() lands behind whichever app
		// currently holds focus. Activate explicitly so our window
		// comes to the foreground.
		[NSApp activateIgnoringOtherApps:YES];
	}
}
