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

#ifndef AMULE_SESSION_TRAFFIC_STATS_H
#define AMULE_SESSION_TRAFFIC_STATS_H

// The statistics tree splits payload by session only in builds that have a figure to fill: IPv6
// data, or LowID to LowID data, which needs server-coordinated NAT traversal over uTP.
#if defined(ENABLE_IPV6) || (defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT))
#define AMULE_SESSION_TRAFFIC_STATS
#endif

#endif // AMULE_SESSION_TRAFFIC_STATS_H
