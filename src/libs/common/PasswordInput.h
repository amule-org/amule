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

#ifndef AMULE_PASSWORD_INPUT_H
#define AMULE_PASSWORD_INPUT_H

#include <string>

namespace mulecommon
{
// Call on the main thread before starting workers: terminal input temporarily owns signal
// handlers on POSIX. Reads UTF-8 from redirected stdin (one line, including an unterminated
// final line, and nothing after it), or hides terminal input and optionally confirms it. Empty
// lines are successful; EOF before any input, invalid UTF-8, cancellation and I/O errors fail
// without changing output.
// Prompts go to stderr. No caller should log the returned password.
bool ReadPassword(const char *prompt, bool confirm, std::string &password, std::string &error);
} // namespace mulecommon

#endif
