//
// This file is part of aMule Project
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2003-2011 Barry Dunne ( http://www.emule-project.net )
// Copyright (C)2007-2011 Merkur ( strEmail.Format("%s@%s", "devteam", "emule-project.net") /
// http://www.emule-project.net )

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA

#ifndef KADEMLIA_NET_SEARCHEXPRESSION_H
#define KADEMLIA_NET_SEARCHEXPRESSION_H

class CMemFile;
struct SSearchTerm;

namespace Kademlia
{

// Reads the search tree of a KADEMLIA2_SEARCH_KEY_REQ. Returns nullptr for a malformed tree or
// one beyond the depth or node limit. Release the result with FreeSearchExpression().
SSearchTerm *ParseSearchExpression(CMemFile &bio);
void FreeSearchExpression(SSearchTerm *term);

} // namespace Kademlia

#endif // KADEMLIA_NET_SEARCHEXPRESSION_H
