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

#include "TerminationProcessAmuleApi.h"

#include <chrono>
#include <utility>
#include <wx/stream.h>

CTerminationProcessAmuleApi::CTerminationProcessAmuleApi(
	const wxString &cmd, std::function<void(int pid, int status)> onTerminated)
: CTerminationProcess(cmd)
, m_onTerminated(std::move(onTerminated))
{
	// Don't open a command window on Windows
	Redirect();
	// Create the worker before launching the child: a thread allocation failure
	// must not leave us supervising a child whose redirected pipes cannot drain.
	m_outputDrain = std::thread([this]() {
		std::unique_lock<std::mutex> lock(m_outputMutex);
		m_outputWake.wait(lock, [this]() { return m_outputReady || m_outputStopped; });
		while (!m_outputStopped) {
			lock.unlock();
			DrainOutput();
			lock.lock();
			m_outputWake.wait_for(
				lock, std::chrono::milliseconds(100), [this]() { return m_outputStopped; });
		}
	});
}

CTerminationProcessAmuleApi::~CTerminationProcessAmuleApi()
{
	StopOutputDrain();
}

void CTerminationProcessAmuleApi::StartOutputDrain()
{
	// wxExecute has finished attaching the streams. Publish them to the sole
	// reader only now, so it never races their initialization.
	{
		std::lock_guard<std::mutex> lock(m_outputMutex);
		m_outputReady = true;
	}
	m_outputWake.notify_one();
}

void CTerminationProcessAmuleApi::ReleaseFromApp()
{
	StopOutputDrain();
	m_onTerminated = nullptr;
	// Keep the wxProcess observer alive until wx reports the child's exit,
	// but close its pipes now. amuleapi ignores SIGPIPE and treats a closed
	// console as best effort, so its logfile/teardown can still complete.
	delete GetInputStream();
	delete GetErrorStream();
	CloseOutput();
	SetPipeStreams(nullptr, nullptr, nullptr);
}

void CTerminationProcessAmuleApi::StopOutputDrain()
{
	{
		std::lock_guard<std::mutex> lock(m_outputMutex);
		m_outputStopped = true;
	}
	m_outputWake.notify_one();
	if (m_outputDrain.joinable()) {
		m_outputDrain.join();
	}
}

void CTerminationProcessAmuleApi::DrainOutput()
{
	// amuleapi keeps its own logfile. Discard the redirected console copies,
	// including stderr, rather than letting a full pipe block its logging or
	// shutdown. This runs independently of a stalled core's event loop.
	char buffer[4096];
	for (wxInputStream *stream : { GetInputStream(), GetErrorStream() }) {
		// Bound each pass so a noisy stream cannot starve the other or shutdown.
		for (unsigned chunks = 0; stream && chunks < 32 && stream->CanRead(); ++chunks) {
			stream->Read(buffer, sizeof(buffer));
			if (!stream->LastRead()) {
				break;
			}
		}
	}
}

void CTerminationProcessAmuleApi::OnTerminate(int pid, int status)
{
	// Join before wxProcess destroys the streams used by the worker.
	StopOutputDrain();

	// Tell the application that amuleapi is no longer running
	if (m_onTerminated) {
		m_onTerminated(pid, status);
	}

	// Normal termination processing here
	CTerminationProcess::OnTerminate(pid, status);
}
