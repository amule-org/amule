//
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
//
// macOS backend for ProtocolHandlerManager: LaunchServices scheme
// registration (LSSetDefaultHandlerForURLScheme) plus a kAEGetURL Apple
// Event handler installed at dylib-load time to catch cold-launch
// scheme clicks.

#include "ProtocolHandlerManager.h"

// Guard the whole body so the Linux clang-tidy runner skips it rather
// than failing on ApplicationServices/ApplicationServices.h.
#if defined(__APPLE__)

#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>

#import <wx/app.h>

namespace
{
// The scheme for a URL protocol, or the imported UTI for the collection
// file type (declared by UTImportedTypeDeclarations in the bundle plist).
NSString *NSStringFromScheme(HandlerTarget target)
{
	switch (target) {
	case HandlerTarget::Ed2kScheme:
		return @"ed2k";
	case HandlerTarget::MagnetScheme:
		return @"magnet";
	case HandlerTarget::CollectionFile:
		return @"org.amule.emulecollection";
	}
	return @"";
}

bool IsUriScheme(HandlerTarget target)
{
	return target != HandlerTarget::CollectionFile;
}

wxString WxStringFromNSString(NSString *ns)
{
	if (ns == nil) {
		return wxEmptyString;
	}
	return wxString::FromUTF8([ns UTF8String]);
}
} // namespace

wxString MacOwnBundleId()
{
	// nil outside a .app bundle (dev-loop harnesses); the CMake
	// target always builds into aMule.app, so only unit tests hit it.
	NSString *bid = [[NSBundle mainBundle] bundleIdentifier];
	return WxStringFromNSString(bid);
}

wxString MacReadHandler(HandlerTarget target)
{
	// Both LSCopy… calls return nil when no handler is set. Deprecated
	// in 10.15 but still working; the modern NSWorkspace lookups don't
	// cover the write path (LSSetDefault…) so we stick with the LS pairs
	// for symmetry.
	CFStringRef nameCf = (CFStringRef)NSStringFromScheme(target);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	CFStringRef handler = IsUriScheme(target)
				  ? LSCopyDefaultHandlerForURLScheme(nameCf)
				  : LSCopyDefaultRoleHandlerForContentType(nameCf, kLSRolesAll);
#pragma clang diagnostic pop
	if (handler == nullptr) {
		return wxEmptyString;
	}
	wxString result = WxStringFromNSString((__bridge NSString *)handler);
	CFRelease(handler);
	return result;
}

bool MacWrite(HandlerTarget target, const wxString &canonicalExe)
{
	// LaunchServices tracks bundle id, not path — the canonicalExe
	// arg is only meaningful on the Windows/Linux backends.
	(void)canonicalExe;

	wxString ownBid = MacOwnBundleId();
	if (ownBid.empty()) {
		return false;
	}
	CFStringRef bidCf =
	    CFStringCreateWithCString(nullptr, ownBid.mb_str(wxConvUTF8), kCFStringEncodingUTF8);
	if (bidCf == nullptr) {
		return false;
	}
	CFStringRef nameCf = (CFStringRef)NSStringFromScheme(target);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	OSStatus rc = IsUriScheme(target)
			  ? LSSetDefaultHandlerForURLScheme(nameCf, bidCf)
			  : LSSetDefaultRoleHandlerForContentType(nameCf, kLSRolesAll, bidCf);
#pragma clang diagnostic pop
	CFRelease(bidCf);
	return rc == noErr;
}

bool MacRemove(HandlerTarget /*scheme*/)
{
	// LaunchServices has no "clear default handler" call. Disable
	// on macOS is a UI-only signal; see ProtocolHandlerManager.cpp.
	return true;
}

// ---------------------------------------------------------------------
// Runtime URL delivery
// ---------------------------------------------------------------------
// Cold-launch scheme clicks are delivered by macOS as a kAEGetURL
// Apple Event dispatched between applicationWillFinishLaunching and
// applicationDidFinishLaunching — earlier than any wxApp OnInit
// runs. We register the handler in a __attribute__((constructor)) so
// it's already live when the AE dispatches.
//
// wxNSAppController registers for the same event class in
// applicationWillFinishLaunching, and -setEventHandler: replaces per
// (class, id) rather than stacking, so exactly one of the two is live.
// Observed behaviour is that this one wins and wxApp::MacOpenURL never
// fires; CamuleGuiApp overrides it anyway so a load order that goes the
// other way still delivers the URL. Only one handler ever runs, so
// there is no risk of queueing a link twice.

// C shim exposed to ProtocolHandlerManager.cpp so its diagnostics land
// under the same [amuleurl] Console.app filter.
extern "C" void amule_url_log(const char *msg)
{
	if (msg == nullptr) {
		return;
	}
	NSLog(@"[amuleurl] %s", msg);
}

@interface AmuleURLAppleEventHandler : NSObject {
}
- (void)getUrl:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

static AmuleURLAppleEventHandler *g_url_handler = nil;

@implementation AmuleURLAppleEventHandler
- (void)getUrl:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
	(void)reply;
	NSString *raw = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
	if (raw == nil) {
		return;
	}
	NSLog(@"[amuleurl] kAEGetURL received: %@", raw);
	// Marshal to the wx main loop. QueueSchemeLink is build-agnostic;
	// it writes to ED2KLinks and lets each app's ~1 s polling loop
	// (CDownloadQueue on amule/amuled, OnPollTimer on amulegui) drain
	// it — safe even when downloadqueue isn't wired yet on cold launch.
	wxString url = wxString::FromUTF8([raw UTF8String]);
	if (wxTheApp != nullptr) {
		wxTheApp->CallAfter([url]() { ProtocolHandler_QueueSchemeLink(url); });
	}
}
@end

// Runs at dylib-load time, before main(), before wxEntry, before Cocoa's
// applicationWillFinishLaunching — so the cold-launch kAEGetURL Apple
// Event finds our handler already registered. NSAppleEventManager is
// available at library-load in Cocoa apps.
__attribute__((constructor)) static void amule_install_url_handler_at_load(void)
{
	@autoreleasepool {
		if (g_url_handler != nil) {
			return;
		}
		g_url_handler = [[AmuleURLAppleEventHandler alloc] init];
		[[NSAppleEventManager sharedAppleEventManager] setEventHandler:g_url_handler
			andSelector:@selector(getUrl:withReplyEvent:)
			forEventClass:kInternetEventClass
			andEventID:kAEGetURL];
		NSLog(@"[amuleurl] library-load: kAEGetURL handler installed");
	}
}

#endif // __APPLE__
