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
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include <webapi/PrefsSchema.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <string>

using namespace muleunit;

DECLARE_SIMPLE(PrefsWebUiBounds)

namespace
{

struct JsBounds
{
	std::string where;
	int64_t min = -1;
	int64_t max = -1;
	int64_t step = -1;
};

// preferences.js cannot include the C++ limits, so read its numbers here: the fields of each
// tab, under the tab's category unless the field names its own.
std::map<std::string, JsBounds> ReadWebUiBounds()
{
	std::ifstream in(PREFERENCES_JS);
	std::stringstream text;
	text << in.rdbuf();
	const std::string js = text.str();

	const std::regex tab(R"re(\{ id: "\w+"[^\[{]*?cat: "([\w.]+)")re");
	const std::regex field(R"re(\{ key: "(\w+)"([^{}]*)\})re");
	const std::regex cat(R"re(cat: "([\w.]+)")re");
	const std::regex number(R"re((min|max|step): (\d+))re");

	std::map<std::size_t, std::string> tabCats;
	for (std::sregex_iterator it(js.begin(), js.end(), tab), end; it != end; ++it) {
		tabCats[it->position()] = (*it)[1];
	}
	std::map<std::string, JsBounds> bounds;
	for (std::sregex_iterator it(js.begin(), js.end(), field), end; it != end; ++it) {
		const std::string attrs = (*it)[2];
		JsBounds b;
		for (std::sregex_iterator n(attrs.begin(), attrs.end(), number); n != end; ++n) {
			const int64_t value = std::stoll((*n)[2]);
			const std::string name = (*n)[1];
			(name == "min" ? b.min : name == "max" ? b.max : b.step) = value;
		}
		if (b.min < 0 && b.max < 0 && b.step < 0) {
			continue;
		}
		std::smatch own;
		auto owner = tabCats.upper_bound(it->position());
		const std::string category = std::regex_search(attrs, own, cat) ? own[1].str()
					     : owner == tabCats.begin()         ? std::string()
										: std::prev(owner)->second;
		b.where = category + "." + (*it)[1].str();
		bounds[b.where] = b;
	}
	return bounds;
}

const webapi::PrefField *FindRow(const std::string &where)
{
	for (std::size_t i = 0; i < webapi::PrefSchemaSize(); ++i) {
		const webapi::PrefField &f = webapi::PrefSchema()[i];
		if (where == std::string(f.category) + "." + f.key) {
			return &f;
		}
	}
	return nullptr;
}

} // namespace

TEST(PrefsWebUiBounds, ReadsTheWebUiFields)
{
	const auto bounds = ReadWebUiBounds();
	ASSERT_TRUE(bounds.size() >= 20);
	ASSERT_TRUE(bounds.count("advanced.source_reask_minutes") == 1);
	ASSERT_TRUE(bounds.count("remote_controls.amuleapi.port") == 1);
}

// The WebUI must never offer a value amuleapi rejects. Where the core clamps or quantises (a
// floor or a step), it must offer exactly the same range.
TEST(PrefsWebUiBounds, WebUiBoundsMatchTheSchema)
{
	for (const auto &entry : ReadWebUiBounds()) {
		const JsBounds &js = entry.second;
		const webapi::PrefField *row = FindRow(js.where);
		ASSERT_TRUE_M(
			row != nullptr, wxString("preferences.js bounds an unknown field: ") + js.where);
		const wxString what = wxString(js.where) + ": preferences.js disagrees with PrefsSchema.cpp";
		ASSERT_TRUE_M(js.min < 0 || js.min >= static_cast<int64_t>(row->min), what);
		ASSERT_TRUE_M(js.max <= static_cast<int64_t>(row->max), what);
		if (row->step) {
			ASSERT_TRUE_M(js.step == static_cast<int64_t>(row->step), what);
		}
		if (row->min || row->step) {
			ASSERT_TRUE_M(js.min == static_cast<int64_t>(row->min), what);
			ASSERT_TRUE_M(js.max == static_cast<int64_t>(row->max), what);
		}
	}
}
