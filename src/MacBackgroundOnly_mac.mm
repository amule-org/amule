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

#include <ApplicationServices/ApplicationServices.h>

// Run before main(), so wxEntry() cannot register this helper process as a foreground app.
// The background-only process transform was added to the SDK in macOS 10.7.
__attribute__((constructor)) static void mac_declare_background_only()
{
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1070
	// kCurrentProcess names this process; the first field of the serial number is unused then.
	// TransformProcessType is the runtime equivalent of LSUIElement for a binary that shares a
	// bundle with a real GUI app, so it narrows the effect to this process only.
	ProcessSerialNumber psn = { 0, kCurrentProcess };
	TransformProcessType(&psn, kProcessTransformToBackgroundApplication);
#endif
}
