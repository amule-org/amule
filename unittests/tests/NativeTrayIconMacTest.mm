//								-*- C++ -*-
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

#include "NativeTrayIcon.h"
#if defined(AMULE_NATIVE_TRAY_ICON) && defined(__WXOSX_COCOA__)
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <wx/app.h>
#include <wx/bmpbndl.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <iostream>
#include <stdexcept>

// Intercept the public factory to inspect the actual displayed status item.
static NSStatusItem *status;
static IMP originalFactory;
static NSStatusItem *CaptureStatus(id bar, SEL method, CGFloat length)
{
	status = reinterpret_cast<NSStatusItem *(*)(id, SEL, CGFloat)>(originalFactory)(bar, method, length);
	return status;
}
static bool popupAnchoredToButton;
static BOOL CapturePopup(id menu, SEL, NSMenuItem *, NSPoint, NSView *view)
{
	popupAnchoredToButton = view == status.button;
	// Exercise Cocoa's real wx menu-item action and command routing, but avoid
	// entering an interactive menu tracking loop on a CI runner.
	[(NSMenu *)menu performActionForItemAtIndex:0];
	return YES;
}
class TestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(TestApp);
static void require(bool ok, const char *message)
{
	if (!ok) {
		throw std::runtime_error(message);
	}
}
class TestTray : public MuleTrayIconBase
{
public:
	int menus = 0;
	wxMenu *CreatePopupMenu() override { return new wxMenu; }
	bool PopupMenu(wxMenu *menu) override
	{
		require(menu != nullptr, "missing menu");
		++menus;
		return true;
	}
};
int main(int argc, char **argv)
{
	if (!wxEntryStart(argc, argv)) {
		return 1;
	}
	if (!wxTheApp->CallOnInit()) {
		wxEntryCleanup();
		return 1;
	}
	Method factory = class_getInstanceMethod(
		object_getClass([NSStatusBar systemStatusBar]), @selector(statusItemWithLength:));
	originalFactory = method_setImplementation(factory, reinterpret_cast<IMP>(CaptureStatus));
	int result = 0;
	try {
		wxImage image(22, 22);
		image.SetRGB(wxRect(0, 0, 22, 22), 0, 0, 0);
		const auto bundle = wxBitmapBundle::FromBitmap(wxBitmap(image));
		TestTray tray;
		require(!tray.SetIcon(wxBitmapBundle()), "invalid bundle accepted");
		require(tray.SetIcon(bundle, "template test"), "SetIcon failed");
		require(tray.IsIconInstalled() && status && [status.button.image isTemplate],
			"displayed image is not a template");
		require([status.button.toolTip isEqualToString:@"template test"], "tooltip lost");
		require(status.button.image.size.height > 0 &&
				status.button.image.size.height < [NSStatusBar systemStatusBar].thickness,
			"menu bar image has wrong logical size");
		NSImage *unchanged = status.button.image;
		require(tray.SetIcon(bundle, "template test") && status.button.image == unchanged,
			"unchanged rate tick rebuilt the native image");
		require(tray.SetIcon(bundle, "changed tooltip") &&
				[status.button.toolTip isEqualToString:@"changed tooltip"],
			"changed tooltip was ignored");
		[status.button performClick:nil];
		require(tray.menus == 1, "native click lost wx menu dispatch");
		wxMenu commandMenu;
		commandMenu.Append(12345, "Test command");
		bool commandReceived = false;
		bool removeDuringCommand = false;
		tray.Bind(
			wxEVT_MENU,
			[&](wxCommandEvent &) {
				commandReceived = true;
				if (removeDuringCommand) {
					tray.RemoveIcon();
				}
			},
			12345);
		wxEvtHandler *previousHandler = commandMenu.GetEventHandler();
		Method popup = class_getInstanceMethod(
			[NSMenu class], @selector(popUpMenuPositioningItem:atLocation:inView:));
		IMP previousPopup = method_setImplementation(popup, reinterpret_cast<IMP>(CapturePopup));
		const bool shown = tray.MuleTrayIconBase::PopupMenu(&commandMenu);
		method_setImplementation(popup, previousPopup);
		require(shown && popupAnchoredToButton && commandReceived,
			"native popup anchor or wx command dispatch lost");
		require(commandMenu.GetEventHandler() == previousHandler,
			"native popup did not restore wx menu handler");
		removeDuringCommand = true;
		commandReceived = false;
		method_setImplementation(popup, reinterpret_cast<IMP>(CapturePopup));
		const bool removedFromMenu = tray.MuleTrayIconBase::PopupMenu(&commandMenu);
		method_setImplementation(popup, previousPopup);
		require(removedFromMenu && commandReceived && !tray.IsIconInstalled() &&
				commandMenu.GetEventHandler() == previousHandler,
			"removing icon during native menu command broke popup cleanup");
		require(tray.SetIcon(bundle, "restored after command"), "restore after menu command failed");
		NSStatusItem *removed = [status retain];
		tray.RemoveIcon();
		require(!tray.IsIconInstalled() && removed.button.target == nil,
			"removed icon retained callbacks");
		[removed release];
		require(tray.SetIcon(bundle, "restored") && [status.button.image isTemplate],
			"restore lost template flag");
		tray.RemoveIcon();
		tray.RemoveIcon();
		std::cout << "PASS: displayed template image, native menu dispatch and remove/restore\n";
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		result = 1;
	}
	method_setImplementation(factory, originalFactory);
	wxTheApp->OnExit();
	wxEntryCleanup();
	return result;
}

#else
int main()
{
	return 77;
}
#endif
