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

#ifndef USEREVENTS_H
#define USEREVENTS_H

#include <wx/intl.h> // Needed for wxTRANSLATE

#ifdef _MSC_VER
#define ATTR(x)
#else
#define ATTR(x) __attribute__((x))
#endif

/* Each event uses 5 IDs: the panel showing its prefs, the 'Core command enabled' checkbox, the
   'Core command' textctrl, the 'GUI command enabled' checkbox and the 'GUI command' textctrl. */
#define USEREVENTS_IDS_PER_EVENT 5

const int USEREVENTS_FIRST_ID = 11500; /* Some safe GUI ID to start from */

/**
 * Macro listing all the events.
 *
 * Expanded 5 times in the sources, each producing different code. Dropping the macro -- on style
 * grounds, or because a compiler cannot cope with one this big -- means keeping those five places
 * in sync by hand: one in PrefsUnifiedDlg.cpp (EVENT_LIST, PrefsUnifiedDlg::PrefsUnifiedDlg()), one
 * in this header (CUserEvents::EventType), and two in UserEvents.cpp (static struct EventList[];
 * CUserEvent::ExecuteCommand()).
 */
#define USEREVENTS_EVENTLIST() \
	USEREVENTS_EVENT(DownloadCompleted, \
		wxTRANSLATE("Download completed"), \
		USEREVENTS_REPLACE_VAR("FILE", \
			wxTRANSLATE("The full path to the file."), \
			static_cast<const CPartFile *>(object) \
				->GetFullName() \
				.GetRaw()) USEREVENTS_REPLACE_VAR("NAME", \
			wxTRANSLATE("The name of the file without path component."), \
			static_cast<const CPartFile *>(object)->GetFileName().GetRaw()) \
			USEREVENTS_REPLACE_VAR("HASH", \
				wxTRANSLATE("The eD2k hash of the file."), \
				static_cast<const CPartFile *>(object)->GetFileHash().Encode()) \
				USEREVENTS_REPLACE_VAR("SIZE", \
					wxTRANSLATE("The size of the file in bytes."), \
					(CFormat("%llu") % \
						static_cast<const CPartFile *>(object)->GetFileSize()) \
						.GetString()) USEREVENTS_REPLACE_VAR("DLACTIVETIME", \
					wxTRANSLATE("Cumulative download activity time."), \
					CastSecondsToHM( \
						static_cast<const CPartFile *>(object)->GetDlActiveTime()))) \
	USEREVENTS_EVENT(NewChatSession, \
		wxTRANSLATE("New chat session started"), \
		USEREVENTS_REPLACE_VAR( \
			"SENDER", wxTRANSLATE("Message sender."), *static_cast<const wxString *>(object))) \
	USEREVENTS_EVENT(OutOfDiskSpace, \
		wxTRANSLATE("Out of space"), \
		USEREVENTS_REPLACE_VAR("PARTITION", \
			wxTRANSLATE("Disk partition."), \
			wxString(static_cast<const wxChar *>(object)))) \
	USEREVENTS_EVENT(ErrorOnCompletion, \
		wxTRANSLATE("Error on completion"), \
		USEREVENTS_REPLACE_VAR("FILE", \
			wxTRANSLATE("The full path to the file."), \
			static_cast<const CPartFile *>(object)->GetFullName().GetRaw()))

#define USEREVENTS_EVENT(ID, NAME, VARS) ID,

/**
 * Handles userspace events: events published to the user, who can specify a command to run when one
 * occurs.
 */
class CUserEvents
{
	friend class CPreferences;

public:
	//! Event list
	enum EventType
	{
		USEREVENTS_EVENTLIST()
		/* This macro expands to the following list of user event types:
		   DownloadCompleted, NewChatSession, OutOfDiskSpace, ErrorOnCompletion */
	};

	/**
	 * Process a user event.
	 *
	 * `object` should be a pointer to an object instance from which every replacement text can
	 * be generated. That is not type-safe; a list of (key, replacement) string pairs would be
	 * better, but it would mean either expanding the macro at every CUserEvents::ProcessEvent
	 * call site or creating a parameter list per event -- more lists to keep in sync by hand.
	 */
	static void ProcessEvent(enum EventType event, const void *object);

	/**
	 * The number of defined user events.
	 */
	static unsigned int GetCount() ATTR(__const__);

	/**
	 * The human-readable name of the event.
	 */
	static const wxString &GetDisplayName(enum EventType event) ATTR(__pure__);

	/**
	 * Whether the core command is enabled.
	 */
	static bool IsCoreCommandEnabled(enum EventType event) ATTR(__pure__);

	/**
	 * Whether the GUI command is enabled.
	 */
	static bool IsGUICommandEnabled(enum EventType event) ATTR(__pure__);

private:
	// functions for CPreferences
	static const wxString &GetKey(const unsigned int event) ATTR(__pure__);
	static bool &GetCoreEnableVar(const unsigned int event) ATTR(__pure__);
	static wxString &GetCoreCommandVar(const unsigned int event) ATTR(__pure__);
	static bool &GetGUIEnableVar(const unsigned int event) ATTR(__pure__);
	static wxString &GetGUICommandVar(const unsigned int event) ATTR(__pure__);
};

#undef USEREVENTS_EVENT

#endif /* USEREVENTS_H */
