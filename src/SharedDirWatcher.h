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
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

#ifndef SHAREDDIRWATCHER_H
#define SHAREDDIRWATCHER_H

#include <wx/event.h>
#include <wx/timer.h>
#include <wx/fswatcher.h>

#include <set>
#include <unordered_map>
#include <vector>

#include "Types.h"

class CSharedFileList;
class CPath;

// Watches every directory in CPreferences::shareddir_list for file/dir
// creation, deletion, rename and modification, and triggers a debounced
// CSharedFileList::Reload() so newly-added files become shared without a
// manual "Reload shared files" click.
//
// New subdirectories created under any watched path are auto-added to
// shareddir_list so the watcher keeps following them, which mirrors the
// behaviour users expect from the existing "recursive share" button —
// once a parent is shared, anything created beneath it is shared too.
// We do not persist a per-entry recursive flag because shareddir.dat
// already enumerates each subdirectory individually.
//
// Backends per platform are provided by wxFileSystemWatcher: inotify on
// Linux, FSEvents on macOS, ReadDirectoryChangesW on Windows, kqueue on
// BSD. AddTree() handles initial recursive setup; we re-Add new subdirs
// individually as we discover them so the watch graph keeps pace with
// disk changes.
class CSharedDirWatcher : public wxEvtHandler
{
public:
	explicit CSharedDirWatcher(CSharedFileList *parent);
	~CSharedDirWatcher();

	// Start watching every path in shareddir_list. Safe to call when
	// already enabled — no-op in that case.
	void Enable();

	// Stop watching. The watcher object itself is freed from the event
	// queue rather than here -- see Disable(). Safe to call when already
	// disabled.
	void Disable();

	bool IsEnabled() const { return m_watcher != NULL; }

	// Called from CSharedFileList::Reload() after the list has been
	// rebuilt. Re-walks shareddir_list and updates the watcher's path
	// set so removed dirs stop firing events and newly-added dirs
	// start firing.
	void Refresh();

	// Event-type flags accumulated for one pending path between
	// fs-watcher delivery and debounce-flush. Stored as ints so we
	// don't have to expose wxFSW_EVENT_* in the header to includers.
	struct PendingPathEvents
	{
		int flags;          // bitmask of wxFSW_EVENT_CREATE/DELETE/MODIFY
		wxString renamedTo; // populated for RENAME; empty otherwise
	};

private:
	void OnFileSystemEvent(wxFileSystemWatcherEvent &event);
	void OnDebounceTimer(wxTimerEvent &event);
#ifdef __APPLE__
	// macOS amuled (wxAppConsole) doesn't spin the main thread's
	// CFRunLoop, so FSEvents callbacks scheduled by wx never deliver.
	// A periodic non-blocking drain fixes that without spawning a
	// dedicated thread. No-op under aMule.app whose Cocoa main loop
	// already pumps the runloop.
	void OnMacRunLoopPump(wxTimerEvent &event);
#endif

	// Walk shareddir_list and Add() every path. Errors per-path are
	// logged but do not abort the rest (so a single Linux
	// max_user_watches refusal doesn't blank the whole watcher).
	void RegisterAllPaths();

	// Coalesce a burst of FS events into per-path deltas applied on
	// the debounce-timer flush. Resets the 5-second timer on every
	// new event; processing runs when the timer finally fires.
	void ScheduleProcessing();

	// Walk m_pendingEvents and apply each one to CSharedFileList via
	// NotifyPathAdded/Removed/Modified. Skipped if a resync is owed
	// was set by a wxFSW_EVENT_WARNING / _ERROR event since the last
	// flush -- in that case we fall back to the bulk Reload() because
	// the watcher backend has signalled it dropped events and we
	// can't trust our incremental view of the tree any more.
	void FlushPendingEvents();

	// Append a newly-created directory to shareddir_list (if not
	// already there) and start watching it. Used for Option A's
	// "auto-share new subdirs of watched parents" behaviour.
	void RegisterNewSubdirectory(const wxString &path);

	// Tear down a renamed-away / deleted shared dir: detach its subtree, drop it
	// from the runtime shared set + its watch. Never touches the user's
	// explicit/recursive config. Returns true if it removed a set entry.
	bool HandleDirRemoved(const wxString &path);

	// Is `path` in the runtime shared set? Distinguishes a dir event from a
	// file event once the path is gone from disk.
	bool IsInSharedSet(const wxString &path) const;

	// Closes the inotify/kqueue race window inside RegisterNewSubdirectory:
	// between the kernel mkdir and our wxFileSystemWatcher::Add(), any
	// files or subdirs created inside the new directory fire on a watch
	// that doesn't exist yet and get silently dropped. Walks `parent`
	// after the watch is in place and feeds existing entries through
	// NotifyPathAdded (files) and RegisterNewSubdirectory (subdirs,
	// recursively) so the catch-up is symmetric with what the live
	// event path would have done. Idempotent on second observation.
	void ScanNewSubdirRace(const CPath &parent);

	// Recursively walk each path in shareddir_list and add any subdir
	// not already listed. The "cold" twin of RegisterNewSubdirectory:
	// without this, subdirs created while aMule was offline are never
	// observed -- the watcher only fires CREATE events post-Enable(),
	// so a /Music share whose disk grew three new albums in the
	// interim would never see them until each gained a new file via
	// some other path. Batches in-memory and writes shareddir.dat
	// once at the end so a large discovery pass doesn't trigger N
	// rewrites.
	void ColdDiscoverSubdirs();

	// Recursive walker used by ColdDiscoverSubdirs. Visits every
	// subdirectory of `root`; for each that is not already in
	// `known` it inserts the path into `known` and appends to `out`.
	// Always recurses (even through already-known subdirs) so a tree
	// whose top layer is in shareddir.dat but whose deeper layers
	// are not still gets fully covered in one pass.
	void WalkForUnknownSubdirs(const CPath &root, std::set<wxString> &known, std::vector<CPath> &out);

	// Destroy the watchers Disable() detached. Queued with CallAfter() so
	// the delete lands between dispatches; also called by the destructor
	// for whatever the queue never reached.
	void ReapPendingWatchers();

	// Stop the debounce (and, on macOS, the run-loop pump) timer. Shared
	// by Disable() and the destructor.
	void StopTimers();

	CSharedFileList *m_parent;
	wxFileSystemWatcher *m_watcher;
	//! Watchers detached by Disable() and not yet destroyed. ~wxFileSystemWatcher
	//! frees the wxFDIOEventLoopSourceHandler wx registered for its inotify fd,
	//! and wxEpollDispatcher::Dispatch walks a snapshot of the ready events
	//! without re-checking any handler -- so freeing one from inside an event
	//! handler makes that loop call a virtual on freed memory as soon as the
	//! same batch reaches the watcher's fd. Destruction is therefore deferred
	//! to the pending-event queue, which the event loop drains before it
	//! dispatches, and the destructor sweeps whatever the queue never reached.
	std::vector<wxFileSystemWatcher *> m_pendingDelete;
	wxTimer m_debounceTimer;

	// Per-path accumulator. Keyed on the raw filesystem path. Events
	// coalesce per-path: a CREATE followed by N MODIFYs followed by
	// CLOSE_WRITE on the same file becomes a single dispatch on the
	// debounce flush. RENAME stores the destination in `renamedTo`.
	std::unordered_map<wxString, PendingPathEvents> m_pendingEvents;

	// Why a full-reload resync is owed; cleared once the fallback fires.
	//
	// ResyncDroppedEvents means the watcher backend reported overflow or drop
	// (inotify queue overflow, kqueue race, Windows ReadDirectoryChangesW
	// buffer exhaust). FlushPendingEvents() responds with the bulk Reload()
	// because the per-path deltas can no longer be trusted -- a real fault,
	// and logged as one.
	//
	// ResyncColdDiscovery means subdirectories simply appeared while aMule was
	// not running. Nothing failed. Both reasons used to share one bool and so
	// one message, which would have reported this routine case as a watcher
	// failure, in red.
	//
	// That is hard to provoke in practice: CPreferences::ReloadSharedFolders
	// expands recursive roots into the shared-dir union and the startup Reload
	// runs before the watcher is enabled, so ColdDiscoverSubdirs normally
	// finds nothing left to discover -- adding subdirectories while aMule is
	// stopped and restarting does *not* produce the wrong message (verified
	// against the pre-change build). What remains is the narrow window where a
	// directory appears between that Reload and Enable(), plus whatever the
	// expansion skips. Separating the reasons is cheap and means that if the
	// path is ever reached it says what happened rather than blaming the
	// backend (issue #968).
	enum ResyncReason
	{
		ResyncNone,
		//! Backend overflow/error: per-path deltas untrustworthy.
		ResyncDroppedEvents,
		//! New subdirs found at startup; nothing failed.
		ResyncColdDiscovery,
	};
	ResyncReason m_resyncReason = ResyncNone;
	//! Subdirectory count for the ResyncColdDiscovery message.
	unsigned m_coldDiscoveredDirs = 0;
#ifdef __APPLE__
	wxTimer m_macPumpTimer;
#endif

	DECLARE_EVENT_TABLE()
};

#endif // SHAREDDIRWATCHER_H
