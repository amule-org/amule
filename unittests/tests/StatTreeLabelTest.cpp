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

#include <muleunit/test.h>

#include <common/Format.h>
#include <ec/cpp/ECSpecialTags.h>

#include <wx/arrstr.h>
#include <wx/translation.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace muleunit;

// The client version and OS nodes carry peer text in their label head. Some catalogs translate a
// msgid without '%' into a string with one (nn: "&Stop" -> "%Stopp"), so such a head must reach
// CFormat untranslated.

DECLARE_SIMPLE(StatTreeLabel)

namespace
{
// A .mo catalog in host byte order, which readers detect from the magic number.
std::string MakeMo(const std::vector<std::pair<std::string, std::string>> &entries)
{
	const uint32_t count = static_cast<uint32_t>(entries.size());
	const uint32_t tables = 28;
	uint32_t offset = tables + 16 * count;
	std::string originals, translations, strings;
	auto put = [](std::string &out, uint32_t value) {
		out.append(reinterpret_cast<const char *>(&value), sizeof(value));
	};
	for (const auto &entry : entries) {
		put(originals, static_cast<uint32_t>(entry.first.size()));
		put(originals, offset);
		strings += entry.first + '\0';
		offset += static_cast<uint32_t>(entry.first.size()) + 1;
	}
	for (const auto &entry : entries) {
		put(translations, static_cast<uint32_t>(entry.second.size()));
		put(translations, offset);
		strings += entry.second + '\0';
		offset += static_cast<uint32_t>(entry.second.size()) + 1;
	}
	std::string mo;
	for (uint32_t value : { 0x950412deU, 0U, count, tables, tables + 8 * count, 0U, 0U }) {
		put(mo, value);
	}
	return mo + originals + translations + strings;
}

class CMemoryLoader : public wxTranslationsLoader
{
public:
	explicit CMemoryLoader(std::string mo)
	: m_mo(std::move(mo))
	{
	}

	wxMsgCatalog *LoadCatalog(const wxString &domain, const wxString &) override
	{
		return wxMsgCatalog::CreateFromData(
			wxScopedCharBuffer::CreateNonOwned(m_mo.data(), m_mo.size()), domain);
	}

	wxArrayString GetAvailableTranslations(const wxString &) const override
	{
		wxArrayString languages;
		languages.Add("nn");
		return languages;
	}

private:
	std::string m_mo;
};

wxString Display(const wxString &label, const wxString &raw)
{
	CECTag node(EC_TAG_STATTREE_NODE, label);
	if (!raw.IsEmpty()) {
		node.AddTag(CECTag(EC_TAG_STAT_NODE_RAW, raw));
	}
	CECTag value(EC_TAG_STAT_NODE_VALUE, static_cast<uint64>(3));
	value.AddTag(CECTag(EC_TAG_STAT_VALUE_TYPE, static_cast<uint8>(EC_VALUE_ISTRING)));
	node.AddTag(value);
	return static_cast<const CEC_StatTree_Node_Tag &>(node).GetDisplayString();
}
} // namespace

TEST(StatTreeLabel, PeerLabelIsNotTranslated)
{
	wxTranslations *translations = new wxTranslations;
	translations->SetLoader(new CMemoryLoader(MakeMo({
		{ "", "Content-Type: text/plain; charset=UTF-8\n" },
		{ "&Stop", "%Stopp" },
		{ "Uptime", "Oppetid" },
	})));
	translations->SetLanguage("nn");
	translations->AddCatalog("amule");
	wxTranslations::Set(translations);

	// A fixed label is still translated.
	ASSERT_EQUALS("Oppetid: 3", Display("Uptime: %s", wxEmptyString));
	// A peer label is rendered as sent, escaped the way CStatistics builds it.
	ASSERT_EQUALS("&Stop: 3", Display(CFormat::Escape("&Stop") + ": %s", "&Stop"));
	ASSERT_EQUALS("%4294967295s 50%: 3",
		Display(CFormat::Escape("%4294967295s 50%") + ": %s", "%4294967295s 50%"));

	wxTranslations::Set(nullptr);
}
