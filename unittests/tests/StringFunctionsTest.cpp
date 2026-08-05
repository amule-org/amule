#include <muleunit/test.h>
#include <common/StringFunctions.h>

using namespace muleunit;

DECLARE_SIMPLE(StringFunctions)

///////////////////////////////////////////////////////////
// Tests for the FuzzyStrCmp function

//! Returns the number of items in an array.
#define itemsof(x) (sizeof(x) / sizeof(x[0]))

TEST(StringFunctions, FuzzyStrCmp)
{
	struct FuzzyTest
	{
		const char *a;
		const char *b;
		int expected;
	};

	FuzzyTest checks[] = {
		{ "a (10)", "a (2)", 1 },
		{ "a (10)", "b (2)", -1 },
		{ "c3 (7)", "c3 (12)", -1 },
		{ "c3 (12)", "c3 (7)", 1 },
		{ "c3 12d", "c3 12d", 0 },
		{ "a (10)  ", "a (2)  ", 1 },
		{ "a (10)  ", "b (2)  ", -1 },
		{ "  c3 (7)", "  c3 (12)", -1 },
		{ "  c3 (12)", "  c3 (7)", 1 },
		{ "c3 12d", "c3 12d", 0 },
		{ "", "", 0 },
		{ "", "c3 12d", -1 },
		{ "c3 12d", "", 1 },
		{ " ", "c3 12d", -1 },
		{ "c3 12d", " ", 1 },
		{ "17.10", "17.2", 1 },
		{ "  c3 (a)", "  c3 (12)", 1 },
		{ "  c3 (12)", "  c3 (a)", -1 },
	};

	for (size_t i = 0; i < itemsof(checks); ++i) {
		wxString a = checks[i].a;
		wxString b = checks[i].b;

		ASSERT_EQUALS(checks[i].expected, FuzzyStrCmp(a, b));
	}
}

///////////////////////////////////////////////////////////
// Tests for the CSimpleParser class

DECLARE_SIMPLE(SimpleParser)

TEST(SimpleParser, Constructor)
{
	// Empty strings are acceptable and should just return an empty string
	{
		CSimpleTokenizer tkz1("", '-');
		ASSERT_EQUALS("", tkz1.remaining());
		ASSERT_EQUALS("", tkz1.next());
		ASSERT_EQUALS("", tkz1.remaining());
		ASSERT_EQUALS("", tkz1.next());
	}

	// String with no tokens should be return immediately
	{
		CSimpleTokenizer tkz2(" abc ", '-');
		ASSERT_EQUALS(" abc ", tkz2.remaining());
		ASSERT_EQUALS(" abc ", tkz2.next());
		ASSERT_EQUALS("", tkz2.next());
		ASSERT_EQUALS("", tkz2.next());
	}
}

TEST(SimpleParser, EmptyTokens)
{
	{
		CSimpleTokenizer tkz1(" a", ' ');
		ASSERT_EQUALS(" a", tkz1.remaining());
		ASSERT_EQUALS(0u, tkz1.tokenCount());

		ASSERT_EQUALS("", tkz1.next());
		ASSERT_EQUALS("a", tkz1.remaining());
		ASSERT_EQUALS(1u, tkz1.tokenCount());

		ASSERT_EQUALS("a", tkz1.next());
		ASSERT_EQUALS("", tkz1.remaining());
		ASSERT_EQUALS(1u, tkz1.tokenCount());

		ASSERT_EQUALS("", tkz1.next());
		ASSERT_EQUALS("", tkz1.remaining());
		ASSERT_EQUALS(1u, tkz1.tokenCount());
	}

	{
		CSimpleTokenizer tkz2("c ", ' ');
		ASSERT_EQUALS("c ", tkz2.remaining());
		ASSERT_EQUALS(0u, tkz2.tokenCount());

		ASSERT_EQUALS("c", tkz2.next());
		ASSERT_EQUALS("", tkz2.remaining());
		ASSERT_EQUALS(1u, tkz2.tokenCount());

		ASSERT_EQUALS("", tkz2.next());
		ASSERT_EQUALS("", tkz2.remaining());
		ASSERT_EQUALS(1u, tkz2.tokenCount());

		ASSERT_EQUALS("", tkz2.next());
		ASSERT_EQUALS("", tkz2.remaining());
		ASSERT_EQUALS(1u, tkz2.tokenCount());
	}

	{
		CSimpleTokenizer tkz3(" a c ", ' ');
		ASSERT_EQUALS(" a c ", tkz3.remaining());
		ASSERT_EQUALS(0u, tkz3.tokenCount());

		ASSERT_EQUALS("", tkz3.next());
		ASSERT_EQUALS("a c ", tkz3.remaining());
		ASSERT_EQUALS(1u, tkz3.tokenCount());

		ASSERT_EQUALS("a", tkz3.next());
		ASSERT_EQUALS("c ", tkz3.remaining());
		ASSERT_EQUALS(2u, tkz3.tokenCount());

		ASSERT_EQUALS("c", tkz3.next());
		ASSERT_EQUALS("", tkz3.remaining());
		ASSERT_EQUALS(3u, tkz3.tokenCount());

		ASSERT_EQUALS("", tkz3.next());
		ASSERT_EQUALS("", tkz3.remaining());
		ASSERT_EQUALS(3u, tkz3.tokenCount());

		ASSERT_EQUALS("", tkz3.next());
		ASSERT_EQUALS("", tkz3.remaining());
		ASSERT_EQUALS(3u, tkz3.tokenCount());
	}
}

TEST(SimpleParser, NormalTokens)
{
	CSimpleTokenizer tkz("a c", ' ');
	ASSERT_EQUALS("a c", tkz.remaining());
	ASSERT_EQUALS(0u, tkz.tokenCount());

	ASSERT_EQUALS("a", tkz.next());
	ASSERT_EQUALS("c", tkz.remaining());
	ASSERT_EQUALS(1u, tkz.tokenCount());

	ASSERT_EQUALS("c", tkz.next());
	ASSERT_EQUALS("", tkz.remaining());
	ASSERT_EQUALS(1u, tkz.tokenCount());

	ASSERT_EQUALS("", tkz.next());
	ASSERT_EQUALS("", tkz.remaining());
	ASSERT_EQUALS(1u, tkz.tokenCount());

	ASSERT_EQUALS("", tkz.next());
	ASSERT_EQUALS("", tkz.remaining());
	ASSERT_EQUALS(1u, tkz.tokenCount());
}

///////////////////////////////////////////////////////////
// Tests for UnescapeHTML

TEST(StringFunctions, UnescapeHTMLPlainAscii)
{
	ASSERT_EQUALS("some file name.avi", UnescapeHTML("some%20file%20name.avi"));
	ASSERT_EQUALS("a|b", UnescapeHTML("a%7Cb"));
	// An escape that is not two hex digits is left exactly as it stands.
	ASSERT_EQUALS("100%25 sure", UnescapeHTML("100%2525%20sure"));
	ASSERT_EQUALS("50% off", UnescapeHTML("50%%20off"));
}

// Regression: the escapes were located by byte offset in a UTF-8 buffer but
// read by character offset out of the wxString, so one multi-byte character
// desynchronised the two and every following escape decoded from the wrong
// place -- injecting stray bytes (a newline, here) and leaving the real
// escapes untouched. An eD2k link whose filename had an accent came out of
// CED2KFileLink split in half.
TEST(StringFunctions, UnescapeHTMLAfterNonAsciiCharacter)
{
	const wxString escaped =
		wxString::FromUTF8("Une%20beaut\xC3\xA9%20faite%20au%20naturel%20-%20Part%202.mkv");
	const wxString expected = wxString::FromUTF8("Une beaut\xC3\xA9 faite au naturel - Part 2.mkv");

	ASSERT_EQUALS(expected, UnescapeHTML(escaped));
}

TEST(StringFunctions, UnescapeHTMLKeepsNonAsciiWithoutEscapes)
{
	const wxString name = wxString::FromUTF8("caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.mkv");

	ASSERT_EQUALS(name, UnescapeHTML(name));
}

// A '%' whose next two bytes are not hex digits at all -- here the start of a
// UTF-8 sequence -- has to survive as a literal, without tripping a debug
// assertion on the way.
TEST(StringFunctions, UnescapeHTMLPercentBeforeNonAscii)
{
	const wxString name = wxString::FromUTF8("100%\xC3\xA9.mkv");

	ASSERT_EQUALS(name, UnescapeHTML(name));
}
