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
#include <algorithm>
#include <wx/bmpbndl.h>
#include <wx/bitmap.h>
#include <wx/menu.h>

@interface AMuleTrayTarget : NSObject {
@public
	MuleTrayIconBase *owner;
}
- (void)clicked:(id)sender;
@end
@implementation AMuleTrayTarget
- (void)clicked:(id)sender
{
	(void)sender;
	wxTaskBarIconEvent event(wxEVT_TASKBAR_CLICK, owner);
	owner->SafelyProcessEvent(event);
}
@end

struct MuleTrayIconBase::Impl
{
	NSStatusItem *status = nil;
	AMuleTrayTarget *target = nil;
	wxBitmapBundle icon;
	wxString tooltip;
	int pixels = 0;
	CGFloat points = 0;
};
MuleTrayIconBase::MuleTrayIconBase()
: m_native(new Impl)
{
}
MuleTrayIconBase::~MuleTrayIconBase()
{
	RemoveIcon();
}

bool MuleTrayIconBase::SetIcon(const wxBitmapBundle &icon, const wxString &tooltip)
{
	if (!icon.IsOk()) {
		return false;
	}
	if (!m_native->status) {
		m_native->status = [[[NSStatusBar systemStatusBar]
			statusItemWithLength:NSSquareStatusItemLength] retain];
		m_native->target = [[AMuleTrayTarget alloc] init];
		m_native->target->owner = this;
		m_native->status.button.target = m_native->target;
		m_native->status.button.action = @selector(clicked:);
		[m_native->status.button sendActionOn:NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown];
	}
	const CGFloat points = std::max<CGFloat>(1, [NSStatusBar systemStatusBar].thickness - 4);
	const CGFloat scale =
		m_native->status.button.window.backingScaleFactor ?: [NSScreen mainScreen].backingScaleFactor;
	const int pixels = wxRound(points * std::max<CGFloat>(1, scale));
	if (m_native->pixels == pixels && m_native->points == points && m_native->icon.IsSameAs(icon) &&
		m_native->tooltip == tooltip) {
		return true;
	}
	const wxBitmap bitmap = icon.GetBitmap(wxSize(pixels, pixels));
	if (!bitmap.IsOk()) {
		return false;
	}
	// wx's status backend wraps the bitmap in a new non-template NSImage. Own
	// the status item so the image AppKit actually displays carries the flag.
	NSImage *image = [bitmap.GetNSImage() copy];
	image.size = NSMakeSize(points, points);
	[image setTemplate:YES];
	m_native->status.button.image = image;
	[image release];
	m_native->status.button.toolTip = [NSString stringWithUTF8String:tooltip.utf8_str()];
	m_native->icon = icon;
	m_native->tooltip = tooltip;
	m_native->pixels = pixels;
	m_native->points = points;
	return true;
}

bool MuleTrayIconBase::RemoveIcon()
{
	if (m_native->status) {
		m_native->status.button.target = nil;
		[[NSStatusBar systemStatusBar] removeStatusItem:m_native->status];
		[m_native->status release];
		[m_native->target release];
		m_native->status = nil;
		m_native->target = nil;
		m_native->icon = wxBitmapBundle();
		m_native->pixels = 0;
	}
	return true;
}

bool MuleTrayIconBase::IsIconInstalled() const
{
	return m_native->status != nil;
}
bool MuleTrayIconBase::PopupMenu(wxMenu *menu)
{
	if (!m_native->status || !menu) {
		return false;
	}
	// A menu command can remove the icon while native menu tracking is active.
	// Keep its button alive until highlighting and menu routing are restored.
	struct PopupState
	{
		wxMenu *menu;
		wxEvtHandler *previous;
		NSStatusBarButton *button;
		~PopupState()
		{
			[button highlight:NO];
			[button release];
			menu->SetEventHandler(previous);
		}
	} popup{ menu, menu->GetEventHandler(), [m_native->status.button retain] };
	menu->SetEventHandler(this);
	menu->UpdateUI();
	// NSStatusItem::popUpStatusItemMenu was deprecated in 10.14. Track the
	// freshly built wx menu through NSMenu's public contextual-menu API instead.
	NSStatusBarButton *button = popup.button;
	const NSRect bounds = button.bounds;
	const NSPoint anchor =
		NSMakePoint(NSMinX(bounds), button.isFlipped ? NSMaxY(bounds) : NSMinY(bounds));
	[button highlight:YES];
	[(NSMenu *)menu->GetHMenu() popUpMenuPositioningItem:nil atLocation:anchor inView:button];
	return true;
}
#endif
