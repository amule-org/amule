//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
// Copyright (c) 2004-2011 Froenchenko Leonid (lfroen@users.sourceforge.net)
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

#ifndef MULEGUIEVENT_H
#define MULEGUIEVENT_H

// Kept out of GuiEvents.h: binaries that include it without linking GuiEvents.cpp
// fail to link under LTO, which keeps this inline class's MULE_EVT_NOTIFY reference.
#include "GuiEvents.h"

wxDECLARE_EVENT(MULE_EVT_NOTIFY, wxEvent);

namespace MuleNotify
{

/**
 * Sent when a worker thread uses a notify macro, so every notification runs on the main thread. The
 * events go to wxTheApp.
 */
class CMuleGUIEvent : public wxEvent
{
public:
	/** Takes ownership a notifier functor. */
	CMuleGUIEvent(CMuleNotiferBase *ntf, bool always = false)
	: wxEvent(-1, MULE_EVT_NOTIFY)
	, m_functor(ntf)
	, m_always(always)
	{
		wxASSERT(m_functor);
	}

	CMuleGUIEvent(const CMuleGUIEvent &) = delete;
	CMuleGUIEvent &operator=(const CMuleGUIEvent &) = delete;

	/** Destructor, frees the functor object. */
	virtual ~CMuleGUIEvent() { delete m_functor; }

	/** Executes the notification. */
	void Notify() const { m_functor->Notify(); }

	/// True for a notification queued by HandleNotificationAlways(), i.e. one that has to run
	/// whether or not there is a window. The two kinds share this event and its handler, so the
	/// handler has to tell them apart. DoNotify() drives things the GUI displays, and
	/// HandleNotification() already declines to run one with no main window. DoNotifyAlways()
	/// drives the socket layer -- CoreNotify_LibSocketConnect and friends are how an asio
	/// callback reaches the main thread -- and amulegui has no main window until an EC
	/// connection is made, so dropping those would stop the connection ever completing.
	bool IsAlways() const { return m_always; }

	/** @see wxEvent::Clone */
	virtual wxEvent *Clone() const { return new CMuleGUIEvent(m_functor->Clone(), m_always); }

private:
	//! Whether this one runs regardless of the GUI's state; see IsAlways().
	bool m_always;

	//! The actual functor object,
	CMuleNotiferBase *m_functor;
};

} // namespace MuleNotify

//! Placing CMuleGUIEvent in the global namespace.
using MuleNotify::CMuleGUIEvent;

//! The event-handler type that takes a CMuleGUIEvent.
typedef void (wxEvtHandler::*MuleNotifyEventFunction)(CMuleGUIEvent &);

//! Event-handler for cross-thread GUI notification events.
#define EVT_MULE_NOTIFY(func) \
	wx__DECLARE_EVT0(MULE_EVT_NOTIFY, wxEVENT_HANDLER_CAST(MuleNotifyEventFunction, func))

#endif // MULEGUIEVENT_H
