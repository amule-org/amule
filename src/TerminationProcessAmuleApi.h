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

#ifndef TERMINATIONPROCESSAMULEAPI_H
#define TERMINATIONPROCESSAMULEAPI_H

#include "TerminationProcess.h" // for CTerminationProcess

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

// This is the handler for process termination events.
// Used on some calls to wxExecute().
class CTerminationProcessAmuleApi : public CTerminationProcess
{
private:
	// A callback rather than CamuleApp: this unit is in a library shared by amule and amuled,
	// and amule.h declares a different CamuleApp in each.
	std::function<void(int pid, int status)> m_onTerminated;
	std::mutex m_outputMutex;
	std::condition_variable m_outputWake;
	bool m_outputReady = false;
	bool m_outputStopped = false;
	std::thread m_outputDrain;
	void DrainOutput();
	void StopOutputDrain();

public:
	CTerminationProcessAmuleApi(
		const wxString &cmd, std::function<void(int pid, int status)> onTerminated);
	~CTerminationProcessAmuleApi() override;
	void StartOutputDrain();
	void ReleaseFromApp();
	void OnTerminate(int pid, int status) override;
};

#endif // TERMINATIONPROCESSAMULEAPI_H
