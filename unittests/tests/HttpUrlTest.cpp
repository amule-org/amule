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

#include <muleunit/test.h>

#include "HttpUrl.h"

using namespace muleunit;

DECLARE_SIMPLE(HttpUrl)

TEST(HttpUrl, AcceptsHttpAndHttps)
{
	ASSERT_TRUE(IsHttpUrl(wxT("http://example.org/server.met")));
	ASSERT_TRUE(IsHttpUrl(wxT("https://example.org/server.met")));
	ASSERT_TRUE(IsHttpUrl(wxT("HTTPS://Example.org/nodes.dat")));
	ASSERT_TRUE(IsHttpUrl(wxT("  http://example.org/ipfilter.zip  ")));
}

TEST(HttpUrl, RejectsOtherSchemes)
{
	ASSERT_FALSE(IsHttpUrl(wxT("file:///etc/hostname")));
	ASSERT_FALSE(IsHttpUrl(wxT("ftp://example.org/server.met")));
	ASSERT_FALSE(IsHttpUrl(wxT("gopher://127.0.0.1:7777/_x")));
	ASSERT_FALSE(IsHttpUrl(wxT("dict://127.0.0.1:11211/stats")));
	ASSERT_FALSE(IsHttpUrl(wxT("httpx://example.org/")));
	ASSERT_FALSE(IsHttpUrl(wxT("example.org/server.met")));
	ASSERT_FALSE(IsHttpUrl(wxT("")));
}
