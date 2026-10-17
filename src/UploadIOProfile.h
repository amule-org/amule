//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

// Temporary, opt-in baseline instrumentation for issue #1586.
#ifndef UPLOADIOPROFILE_H
#define UPLOADIOPROFILE_H

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <locale>
#include <utility>
#include <vector>

class UploadIOProfile
{
	using Clock = std::chrono::steady_clock;
	struct Sample
	{
		int64_t duration;
		uint64_t bytes;
		bool success;
		bool part;
		bool mmap;
	};

public:
	// At most ~100 KiB, allocated once while opting in, never in a read destructor.
	static constexpr size_t MaxSamplesPerLoop = 4096;

	UploadIOProfile() noexcept
	{
		const char *path = std::getenv("AMULE_UPLOAD_IO_PROFILE");
		if (!path || !*path) {
			return;
		}
		try {
			m_samples.reserve(MaxSamplesPerLoop);
			m_output.emplace();
			m_output->imbue(std::locale::classic());
			m_output->open(path, std::ios::out | std::ios::trunc);
			// Version 2 includes complete-file open latency. Earlier captures cannot be
			// compared with it, even when they have a valid completion marker.
			*m_output << "amule-upload-io,2\n"
				     "kind,elapsed_ns,duration_ns,bytes,success,part,mmap\n";
			m_output->flush();
			if (!*m_output) {
				Fail();
				return;
			}
			m_start = Clock::now();
			m_enabled = true;
		} catch (...) {
			// Optional diagnostics must not kill the upload worker on allocation/I/O failure.
			Fail();
		}
	}

	UploadIOProfile(const UploadIOProfile &) = delete;
	UploadIOProfile &operator=(const UploadIOProfile &) = delete;

	bool Enabled() const { return m_enabled; }
	bool Failed() const { return m_failed; }

	void BeginLoop() noexcept
	{
		if (m_enabled && m_loopStarted) {
			Fail();
		}
		if (m_enabled) {
			m_loopStart = Clock::now();
			m_loopStarted = true;
		}
	}

	bool EndLoop() noexcept
	{
		if (m_enabled && !m_loopStarted) {
			Fail();
		}
		if (m_enabled) {
			const auto end = Clock::now();
			try {
				// Serialization, flushing and condition-variable sleep are outside active
				// time.
				for (const auto &sample : m_samples) {
					*m_output << "read," << Nanoseconds(end - m_start) << ','
						  << sample.duration << ',' << sample.bytes << ','
						  << sample.success << ',' << sample.part << ','
						  << sample.mmap << '\n';
				}
				*m_output << "loop," << Nanoseconds(end - m_start) << ','
					  << Nanoseconds(end - m_loopStart) << ",0,0,0,0\n";
				m_output->flush();
				if (!*m_output) {
					Fail();
				}
			} catch (...) {
				Fail();
			}
			m_samples.clear();
			m_loopStarted = false;
		}
		return !std::exchange(m_failurePending, false);
	}

	// Called explicitly after the worker's final loop. Failure/crash captures lack this footer.
	bool Close() noexcept
	{
		if (m_enabled) {
			try {
				if (m_loopStarted) {
					Fail();
				} else {
					*m_output << "end," << Nanoseconds(Clock::now() - m_start)
						  << ",0,0,0,0,0\n";
					m_output->flush();
					if (!*m_output) {
						Fail();
					}
				}
			} catch (...) {
				Fail();
			}
			m_enabled = false;
		}
		return !std::exchange(m_failurePending, false);
	}

	class Read
	{
	public:
		Read(UploadIOProfile &profile, uint64_t bytes, bool part, bool mmap)
		: m_profile(profile)
		, m_sample{ 0, bytes, false, part, mmap }
		{
		}
		Read(const Read &) = delete;
		Read &operator=(const Read &) = delete;

		void Start() noexcept
		{
			if (m_profile.Enabled() && !m_profile.m_loopStarted) {
				m_profile.Fail();
			}
			if (m_profile.Enabled()) {
				m_started = true;
				m_start = Clock::now();
			}
		}

		void Finish() noexcept
		{
			if (m_started) {
				m_sample.duration = Nanoseconds(Clock::now() - m_start);
				m_sample.success = true;
			}
		}

		~Read() noexcept
		{
			if (m_started && m_profile.Enabled()) {
				if (!m_sample.success) {
					m_sample.duration = Nanoseconds(Clock::now() - m_start);
				}
				if (m_profile.m_samples.size() == MaxSamplesPerLoop) {
					// Do not silently drop slow samples and bias the reported
					// percentiles.
					m_profile.Fail();
				} else {
					// reserve() has already allocated the whole bound; Sample cannot
					// throw.
					m_profile.m_samples.push_back(m_sample);
				}
			}
		}

	private:
		UploadIOProfile &m_profile;
		Sample m_sample;
		bool m_started = false;
		Clock::time_point m_start;
	};

private:
	void Fail() noexcept
	{
		m_enabled = false;
		m_failed = true;
		m_failurePending = true;
		m_samples.clear();
	}

	static int64_t Nanoseconds(Clock::duration duration)
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
	}

	bool m_enabled = false;
	bool m_failed = false;
	bool m_failurePending = false;
	bool m_loopStarted = false;
	std::optional<std::ofstream> m_output;
	std::vector<Sample> m_samples;
	Clock::time_point m_start;
	Clock::time_point m_loopStart;
};

#endif
