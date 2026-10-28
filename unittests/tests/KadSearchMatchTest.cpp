//								-*- C++ -*-
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
#include <kademlia/kademlia/Entry.h>
#include <kademlia/kademlia/Indexed.h>
#include <kademlia/net/SearchExpression.h>
#include <MemFile.h>
#include <Tag.h>
#include <tags/FileTags.h>

#include <wx/tokenzr.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

using namespace muleunit;
using Kademlia::CKeyEntry;

// Indexed.cpp defines these; linking it would pull in the whole Kad index.
SSearchTerm::SSearchTerm()
: type(AND)
, tag(nullptr)
, astr(nullptr)
, left(nullptr)
, right(nullptr)
{
}

SSearchTerm::~SSearchTerm()
{
	if (type == String) {
		delete astr;
	}
	delete tag;
}

namespace
{

// Owns every node, so a test only wires them together.
class CTree
{
public:
	SSearchTerm *Words(const wxString &words)
	{
		SSearchTerm *t = New(SSearchTerm::String);
		t->astr = new wxArrayString;
		wxStringTokenizer tok(words, wxT(" "));
		while (tok.HasMoreTokens()) {
			t->astr->Add(tok.GetNextToken());
		}
		return t;
	}

	SSearchTerm *Op(SSearchTerm::ESearchTermType type, SSearchTerm *l, SSearchTerm *r)
	{
		SSearchTerm *t = New(type);
		t->left = l;
		t->right = r;
		return t;
	}

	SSearchTerm *FileFormat(const wxString &ext)
	{
		SSearchTerm *t = New(SSearchTerm::MetaTag);
		t->tag = new CTagString(TAG_FILEFORMAT, ext);
		return t;
	}

	// A string tag condition, "tag == value" compared without case.
	SSearchTerm *StrEquals(const wxString &tag, const wxString &value)
	{
		SSearchTerm *t = New(SSearchTerm::MetaTag);
		t->tag = new CTagString(tag, value);
		return t;
	}

	// "tag == value", which looks the tag up in the entry's tag list.
	SSearchTerm *IntEquals(const wxString &tag, uint64_t value)
	{
		SSearchTerm *t = New(SSearchTerm::OpEqual);
		t->tag = new CTagVarInt(tag, value);
		return t;
	}

	SSearchTerm *AnyIntEquals(const wxString &tag, uint64_t value, size_t leaves)
	{
		SSearchTerm *t = IntEquals(tag, value);
		for (size_t i = 1; i < leaves; ++i) {
			t = Op(SSearchTerm::OR, t, IntEquals(tag, value));
		}
		return t;
	}

	// A left-nested OR of single-word leaves, the shape the eMule and aMule parsers build.
	SSearchTerm *AnyOf(const std::vector<wxString> &words)
	{
		SSearchTerm *t = Words(words.front());
		for (size_t i = 1; i < words.size(); ++i) {
			t = Op(SSearchTerm::OR, t, Words(words[i]));
		}
		return t;
	}

private:
	SSearchTerm *New(SSearchTerm::ESearchTermType type)
	{
		m_nodes.push_back(std::make_unique<SSearchTerm>());
		m_nodes.back()->type = type;
		return m_nodes.back().get();
	}

	std::vector<std::unique_ptr<SSearchTerm>> m_nodes;
};

// A TV episode search: the title quoted to stay under the operator limit, then the episode
// variants.
SSearchTerm *EpisodeSearch(CTree &tree)
{
	return tree.Op(SSearchTerm::AND,
		tree.Words(wxT("the lord of the rings the rings of power")),
		tree.AnyOf({ wxT("s01e05"), wxT("1x05"), wxT("01x05"), wxT("1x5"), wxT("05") }));
}

const uint64_t UNLIMITED = std::numeric_limits<uint64_t>::max();

uint64_t Cost(const CKeyEntry &entry, const SSearchTerm *tree)
{
	uint64_t budget = UNLIMITED;
	entry.SearchTermsMatch(tree, budget);
	return UNLIMITED - budget;
}

// File names shaped like the ones on the network: release-style tokens, 40 to 120 characters.
std::vector<std::unique_ptr<CKeyEntry>> HonestIndex(size_t count)
{
	static const wxChar *const tokens[] = { wxT("the"),
		wxT("of"),
		wxT("and"),
		wxT("lord"),
		wxT("rings"),
		wxT("power"),
		wxT("matrix"),
		wxT("star"),
		wxT("wars"),
		wxT("ubuntu"),
		wxT("linux"),
		wxT("debian"),
		wxT("desktop"),
		wxT("amd64"),
		wxT("iso"),
		wxT("2019"),
		wxT("2024"),
		wxT("1999"),
		wxT("1080p"),
		wxT("720p"),
		wxT("2160p"),
		wxT("x264"),
		wxT("x265"),
		wxT("hevc"),
		wxT("aac"),
		wxT("dts"),
		wxT("bluray"),
		wxT("webrip"),
		wxT("s01e05"),
		wxT("s02e01"),
		wxT("1x05"),
		wxT("complete"),
		wxT("season"),
		wxT("edition"),
		wxT("remastered"),
		wxT("extended"),
		wxT("director"),
		wxT("cut"),
		wxT("live"),
		wxT("concert"),
		wxT("album"),
		wxT("flac"),
		wxT("mp3"),
		wxT("320kbps"),
		wxT("english"),
		wxT("italian"),
		wxT("german"),
		wxT("multi"),
		wxT("subs"),
		wxT("proper") };
	static const wxChar *const exts[] = {
		wxT("mkv"), wxT("avi"), wxT("mp4"), wxT("iso"), wxT("flac"), wxT("mp3"), wxT("zip")
	};
	const size_t nTokens = sizeof(tokens) / sizeof(tokens[0]);
	uint32_t seed = 12345;
	auto next = [&seed]() {
		seed = seed * 1103515245u + 12345u;
		return seed >> 16;
	};
	std::vector<std::unique_ptr<CKeyEntry>> index;
	for (size_t i = 0; i < count; ++i) {
		wxString name;
		while (name.length() < 40 + next() % 80) {
			if (!name.IsEmpty()) {
				name += (next() % 2) ? wxT(".") : wxT(" ");
			}
			name += tokens[next() % nTokens];
		}
		name += wxT(".");
		name += exts[next() % (sizeof(exts) / sizeof(exts[0]))];
		index.push_back(std::make_unique<CKeyEntry>());
		index.back()->SetFileName(name);
	}
	return index;
}

// Scans the index as one request does, sharing one budget, and returns the matches.
size_t Scan(const std::vector<std::unique_ptr<CKeyEntry>> &index, const SSearchTerm *tree, uint64_t &budget)
{
	size_t matches = 0;
	for (const auto &entry : index) {
		if (budget == 0) {
			break;
		}
		if (entry->SearchTermsMatch(tree, budget)) {
			++matches;
		}
	}
	return matches;
}

// Entries one request visits before its budget runs out.
size_t Scanned(
	const std::vector<std::unique_ptr<CKeyEntry>> &index, const SSearchTerm *tree, uint64_t &budget)
{
	size_t visited = 0;
	for (const auto &entry : index) {
		if (budget == 0) {
			break;
		}
		entry->SearchTermsMatch(tree, budget);
		++visited;
	}
	return visited;
}

std::vector<std::unique_ptr<CKeyEntry>> RepeatedNames(size_t count, size_t length)
{
	std::vector<std::unique_ptr<CKeyEntry>> index;
	for (size_t i = 0; i < count; ++i) {
		index.push_back(std::make_unique<CKeyEntry>());
		index.back()->SetFileName(wxString(wxT('a'), length));
	}
	return index;
}

// Entries carrying as many integer tags as a keyword publish keeps, none of them named "B".
std::vector<std::unique_ptr<CKeyEntry>> ManyTags(size_t count, size_t tags)
{
	std::vector<std::unique_ptr<CKeyEntry>> index;
	for (size_t i = 0; i < count; ++i) {
		index.push_back(std::make_unique<CKeyEntry>());
		index.back()->SetFileName(wxT("file.mkv"));
		for (size_t t = 0; t < tags; ++t) {
			index.back()->AddTag(new CTagVarInt(wxString::Format(wxT("t%u"), unsigned(t)), t), 0);
		}
	}
	return index;
}

std::unique_ptr<CKeyEntry> StringTagged(const wxString &value)
{
	auto entry = std::make_unique<CKeyEntry>();
	entry->SetFileName(wxT("file.mkv"));
	entry->AddTag(new CTagString(wxT("artist"), value), 0);
	return entry;
}

} // namespace

DECLARE_SIMPLE(KadSearchMatch)

TEST(KadSearchMatch, HonestSearchMatches)
{
	CTree tree;
	CKeyEntry entry;
	entry.SetFileName(wxT("The Lord of the Rings The Rings of Power S01E05 1080p.mkv"));

	uint64_t budget = CKeyEntry::SEARCH_BUDGET;
	ASSERT_TRUE(entry.SearchTermsMatch(EpisodeSearch(tree), budget));
}

TEST(KadSearchMatch, OtherFileFailsOnItsFirstMissingWord)
{
	CTree tree;
	CKeyEntry entry;
	entry.SetFileName(wxT("Unrelated Holiday Video.avi"));
	SSearchTerm *search = EpisodeSearch(tree);

	uint64_t budget = UNLIMITED;
	ASSERT_FALSE(entry.SearchTermsMatch(search, budget));
	// The AND, the title leaf, the name, and one lookup ("the" is missing): far less than a
	// match that looks up all 14 words.
	CKeyEntry match;
	match.SetFileName(wxT("The Lord of the Rings The Rings of Power S01E05 1080p.mkv"));
	ASSERT_TRUE(Cost(entry, search) * 4 < Cost(match, search));
}

TEST(KadSearchMatch, CaseAndFileFormatStillMatch)
{
	CTree tree;
	CKeyEntry entry;
	entry.SetFileName(wxT("Some.Show.S02E01.MKV"));

	uint64_t budget = CKeyEntry::SEARCH_BUDGET;
	ASSERT_TRUE(entry.SearchTermsMatch(
		tree.Op(SSearchTerm::AND, tree.Words(wxT("show s02e01")), tree.FileFormat(wxT("mkv"))),
		budget));
}

TEST(KadSearchMatch, SpentBudgetDoesNotMatch)
{
	CTree tree;
	CKeyEntry entry;
	entry.SetFileName(wxT("The Lord of the Rings The Rings of Power S01E05 1080p.mkv"));

	uint64_t budget = 5;
	ASSERT_FALSE(entry.SearchTermsMatch(EpisodeSearch(tree), budget));
	ASSERT_EQUALS(0u, budget);
}

// A NOT whose right side ran out of budget used to match.
TEST(KadSearchMatch, SpentBudgetInsideNotDoesNotMatch)
{
	CTree tree;
	CKeyEntry entry;
	entry.SetFileName(wxT("Some Show S02E01.mkv"));
	SSearchTerm *notTerm = tree.Op(SSearchTerm::NOT, tree.Words(wxT("show")), tree.Words(wxT("trailer")));

	// The right side's lookup is the last thing charged, so this budget runs out there.
	uint64_t budget = Cost(entry, notTerm) - 1;
	ASSERT_FALSE(entry.SearchTermsMatch(notTerm, budget));
	ASSERT_EQUALS(0u, budget);
}

// A lookup costs what a substring search may compare: start positions times word length.
TEST(KadSearchMatch, LookupCostFollowsTheCandidates)
{
	CTree tree;
	CKeyEntry repeated;
	repeated.SetFileName(wxString(wxT('a'), 1000));
	CKeyEntry other;
	other.SetFileName(wxString(wxT('x'), 1000));
	SSearchTerm *shortWord = tree.Words(wxT("ab"));
	SSearchTerm *longWord = tree.Words(wxString(wxT('a'), 499) + wxT("b"));

	// Every position of "aaa..." starts a compare (CANDIDATE_COST is 48); a name without the
	// first character costs one scan.
	ASSERT_TRUE(Cost(repeated, shortWord) - Cost(other, shortWord) >= uint64_t(1000 - 2 + 1) * 48);
	ASSERT_TRUE(Cost(repeated, longWord) >= uint64_t(1000 - 500 + 1) * 500);
}

// Lowercasing the name costs per character, far more than scanning it.
TEST(KadSearchMatch, NameCostGrowsWithItsLength)
{
	CTree tree;
	CKeyEntry longName;
	longName.SetFileName(wxString(wxT('X'), 8000));
	ASSERT_TRUE(Cost(longName, tree.Words(wxT("zq"))) >= uint64_t(8000) * 20);
}

TEST(KadSearchMatch, StringTagConditionsMatchWithoutCase)
{
	CTree tree;
	const auto entry = StringTagged(wxT("Video"));
	uint64_t budget = UNLIMITED;
	ASSERT_TRUE(entry->SearchTermsMatch(tree.StrEquals(wxT("artist"), wxT("video")), budget));
	ASSERT_FALSE(entry->SearchTermsMatch(tree.StrEquals(wxT("artist"), wxT("audio")), budget));
}

// String compares pay for the characters they examine, both against a tag and against the
// file's extension.
TEST(KadSearchMatch, StringCompareCostGrowsWithTheValue)
{
	CTree tree;
	const wxString value(wxT('a'), 1000);
	const auto tagged = StringTagged(value);
	ASSERT_TRUE(Cost(*tagged, tree.StrEquals(wxT("artist"), value)) >= uint64_t(1000) * 50);

	CKeyEntry dotted;
	dotted.SetFileName(wxT(".") + value);
	ASSERT_TRUE(Cost(dotted, tree.FileFormat(value)) >= uint64_t(1000) * 50);
}

TEST(KadSearchMatch, TagConditionCostGrowsWithTheTags)
{
	CTree tree;
	SSearchTerm *missing = tree.IntEquals(wxT("B"), 7);
	const auto bare = ManyTags(1, 0);
	const auto tagged = ManyTags(1, 253);

	ASSERT_TRUE(Cost(*tagged.front(), missing) > Cost(*bare.front(), missing) * 50);
}

// Honest searches against the most entries a key can hold (KADEMLIAMAXINDEX): the budget never
// cuts them short, and they leave most of it unused.
TEST(KadSearchMatch, HonestSearchesFitTheBudget)
{
	const auto index = HonestIndex(50000);
	CTree tree;
	// The most an eMule or aMule search box allows: 10 operators.
	SSearchTerm *widest = tree.AnyOf({ wxT("ubuntu"),
		wxT("debian"),
		wxT("fedora"),
		wxT("arch"),
		wxT("mint"),
		wxT("gentoo"),
		wxT("slackware"),
		wxT("suse"),
		wxT("centos"),
		wxT("alpine"),
		wxT("void") });
	const std::vector<SSearchTerm *> searches = {
		EpisodeSearch(tree),
		widest,
		tree.Words(wxT("matrix 1999 1080p x264 dts bluray remastered edition english subs")),
		tree.Op(SSearchTerm::AND, tree.Words(wxT("star wars")), tree.FileFormat(wxT("mkv"))),
		tree.Words(wxT("the of and")),
	};
	for (const SSearchTerm *search : searches) {
		uint64_t unlimited = UNLIMITED;
		const size_t all = Scan(index, search, unlimited);
		uint64_t budget = CKeyEntry::SEARCH_BUDGET;
		ASSERT_EQUALS(all, Scan(index, search, budget));
		ASSERT_TRUE(UNLIMITED - unlimited <= CKeyEntry::SEARCH_BUDGET / 3);
	}
}

// The GHSA-52mv shape: a wide OR of leaves that never match. The budget is shared by all entries
// of one request, so the scan stops long before the last entry.
TEST(KadSearchMatch, WideTreeStopsTheScan)
{
	CTree tree;
	SSearchTerm *wide = tree.AnyOf(std::vector<wxString>(128, wxT("zq")));
	const auto index = HonestIndex(1000);

	uint64_t budget = Cost(*index.front(), wide) * 10;
	Scan(index, wide, budget);
	ASSERT_EQUALS(0u, budget);
}

// A short word against names of one repeated character: every position is a compare. With the
// cost of a lookup counted per position only, this scan ran about 20 times past its time target.
TEST(KadSearchMatch, RepeatedNamesStopTheScan)
{
	CTree tree;
	SSearchTerm *wide = tree.AnyOf(std::vector<wxString>(128, wxT("ab")));
	const auto index = RepeatedNames(200, 8000);

	uint64_t budget = CKeyEntry::SEARCH_BUDGET;
	ASSERT_TRUE(Scanned(index, wide, budget) < 50);
	ASSERT_EQUALS(0u, budget);
}

// A condition on a tag the entries lack walks all their tags. Charged as one node, this scan never
// reached the budget.
TEST(KadSearchMatch, ManyTagsStopTheScan)
{
	CTree tree;
	SSearchTerm *wide = tree.AnyIntEquals(wxT("B"), 7, 128);
	const auto index = ManyTags(1000, 253);

	uint64_t budget = CKeyEntry::SEARCH_BUDGET;
	ASSERT_TRUE(Scanned(index, wide, budget) < 1000);
	ASSERT_EQUALS(0u, budget);
}

namespace
{

// A balanced OR tree of @a leaves single-word leaves, as it travels in a search request.
void WriteOrTree(CMemFile &out, unsigned leaves)
{
	if (leaves == 1) {
		out.WriteUInt8(0x01); // string
		out.WriteString(wxT("zq"), utf8strRaw);
		return;
	}
	out.WriteUInt8(0x00); // boolean operator
	out.WriteUInt8(0x01); // OR
	WriteOrTree(out, leaves / 2);
	WriteOrTree(out, leaves - leaves / 2);
}

// A chain of @a depth NOT operators above one leaf.
void WriteNotChain(CMemFile &out, unsigned depth)
{
	for (unsigned i = 0; i < depth; ++i) {
		out.WriteUInt8(0x00);
		out.WriteUInt8(0x02); // NOT
		out.WriteUInt8(0x01);
		out.WriteString(wxT("a"), utf8strRaw);
	}
	out.WriteUInt8(0x01);
	out.WriteString(wxT("b"), utf8strRaw);
}

// One string tag condition whose value is @a length characters.
void WriteMetaValue(CMemFile &out, unsigned length)
{
	out.WriteUInt8(0x02); // meta tag
	out.WriteString(wxString(wxT('a'), length), utf8strRaw);
	out.WriteString(wxT("artist"), utf8strNone);
}

bool Parses(void (*write)(CMemFile &, unsigned), unsigned n)
{
	CMemFile packet;
	write(packet, n);
	packet.Seek(0, wxFromStart);
	SSearchTerm *tree = Kademlia::ParseSearchExpression(packet);
	Kademlia::FreeSearchExpression(tree);
	return tree != nullptr;
}

} // namespace

// 128 leaves make 255 nodes and 129 make 257; the limit is 256.
TEST(KadSearchMatch, ParserLimitsTheNodeCount)
{
	ASSERT_TRUE(Parses(WriteOrTree, 128));
	ASSERT_FALSE(Parses(WriteOrTree, 129));
}

TEST(KadSearchMatch, ParserLimitsTheDepth)
{
	ASSERT_TRUE(Parses(WriteNotChain, 23));
	ASSERT_FALSE(Parses(WriteNotChain, 24));
}

TEST(KadSearchMatch, ParserLimitsTheMetaValueLength)
{
	ASSERT_TRUE(Parses(WriteMetaValue, 256));
	ASSERT_FALSE(Parses(WriteMetaValue, 257));
}
