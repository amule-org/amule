#include <muleunit/test.h>
#include <OtherFunctions.h>

#include <cstring>

using namespace muleunit;

DECLARE_SIMPLE(Base16)

// Valid hex string round-trips correctly
TEST(Base16, RoundTrip)
{
	const unsigned char input[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	wxString encoded = EncodeBase16(input, 4);
	ASSERT_EQUALS(wxString(wxT("DEADBEEF")), encoded);

	unsigned char decoded[4];
	unsigned int len = DecodeBase16(encoded, encoded.Length(), decoded);
	ASSERT_EQUALS(4u, len);
	ASSERT_TRUE(memcmp(input, decoded, 4) == 0);
}

// Characters ':' through '@' (ASCII 58-64) are not valid hex digits;
// DecodeBase16 must return 0 instead of silently mapping them to 0x9.
TEST(Base16, RejectsNonHexChars)
{
	unsigned char buf[4] = {};
	ASSERT_EQUALS(0u, DecodeBase16(wxT("DE:D"), 4, buf));
	ASSERT_EQUALS(0u, DecodeBase16(wxT("DE;D"), 4, buf));
	ASSERT_EQUALS(0u, DecodeBase16(wxT("DE<D"), 4, buf));
	ASSERT_EQUALS(0u, DecodeBase16(wxT("DE=D"), 4, buf));
	ASSERT_EQUALS(0u, DecodeBase16(wxT("DE>D"), 4, buf));
	ASSERT_EQUALS(0u, DecodeBase16(wxT("DE?D"), 4, buf));
	ASSERT_EQUALS(0u, DecodeBase16(wxT("DE@D"), 4, buf));
}

// Odd-length input is not valid Base16
TEST(Base16, RejectsOddLength)
{
	unsigned char buf[4] = {};
	ASSERT_EQUALS(0u, DecodeBase16(wxT("ABC"), 3, buf));
}

DECLARE_SIMPLE(Base64)

// EncodeBase64 with a header must include both the encoded content and the
// footer — regression for the pbBufferOut = vs += bug.
TEST(Base64, HeaderAndFooterBothPresent)
{
	SetBase64Header(wxT("TEST"));
	wxString result = EncodeBase64("Man", 3);
	SetBase64Header(wxEmptyString); // restore global state

	// "Man" encodes to "TWFu" in standard Base64
	ASSERT_TRUE(result.Contains(wxT("-----BEGIN TEST-----")));
	ASSERT_TRUE(result.Contains(wxT("TWFu")));
	ASSERT_TRUE(result.Contains(wxT("-----END TEST-----")));

	// Encoded content must appear before the footer
	ASSERT_TRUE(result.Find(wxT("TWFu")) < result.Find(wxT("-----END TEST-----")));
}

// Without a header, EncodeBase64 produces plain Base64 with no PEM framing
TEST(Base64, NoHeaderProducesPlainBase64)
{
	SetBase64Header(wxEmptyString);
	wxString result = EncodeBase64("Man", 3);

	ASSERT_FALSE(result.Contains(wxT("-----")));
	ASSERT_TRUE(result.Contains(wxT("TWFu")));
}

// CompareLatestReleaseVersion — the shared release-tag parse + version
// comparison used by the daemon check (CamuleApp::CheckNewVersion) and the
// GUI check (CVersionCheck). Comparisons use extreme tags (999.x / 0.0.1) so
// the up-to-date / outdated results are deterministic regardless of the
// version this test binary was compiled with.

DECLARE_SIMPLE(VersionCompare)

TEST(VersionCompare, ParsesTagAndReportsOutdated)
{
	CVersionCompareResult r = CompareLatestReleaseVersion(wxT("{\"tag_name\": \"999.5.3\"}"));
	ASSERT_TRUE(r.state == CVersionCompareResult::Outdated);
	ASSERT_EQUALS(999, r.major);
	ASSERT_EQUALS(5, r.minor);
	ASSERT_EQUALS(3, r.update);
	ASSERT_EQUALS(wxString(wxT("999.5.3")), r.latest);
}

TEST(VersionCompare, ReportsUpToDate)
{
	CVersionCompareResult r = CompareLatestReleaseVersion(wxT("{\"tag_name\": \"0.0.1\"}"));
	ASSERT_TRUE(r.state == CVersionCompareResult::UpToDate);
}

// A leading v/V is tolerated.
TEST(VersionCompare, StripsVPrefix)
{
	CVersionCompareResult r = CompareLatestReleaseVersion(wxT("{\"tag_name\": \"v999.5.3\"}"));
	ASSERT_TRUE(r.state == CVersionCompareResult::Outdated);
	ASSERT_EQUALS(999, r.major);
	ASSERT_EQUALS(5, r.minor);
	ASSERT_EQUALS(3, r.update);
}

// A pre-release / build-metadata suffix is stripped before comparison.
TEST(VersionCompare, StripsSuffix)
{
	CVersionCompareResult a = CompareLatestReleaseVersion(wxT("{\"tag_name\": \"999.5.3-rc1\"}"));
	ASSERT_EQUALS(999, a.major);
	ASSERT_EQUALS(5, a.minor);
	ASSERT_EQUALS(3, a.update);

	CVersionCompareResult b = CompareLatestReleaseVersion(wxT("{\"tag_name\": \"999.5.3+build42\"}"));
	ASSERT_EQUALS(3, b.update);
}

// Tags with fewer than three components are valid; missing fields are 0.
TEST(VersionCompare, FewerComponents)
{
	CVersionCompareResult r = CompareLatestReleaseVersion(wxT("{\"tag_name\": \"999.5\"}"));
	ASSERT_TRUE(r.state == CVersionCompareResult::Outdated);
	ASSERT_EQUALS(999, r.major);
	ASSERT_EQUALS(5, r.minor);
	ASSERT_EQUALS(0, r.update);
	ASSERT_EQUALS(wxString(wxT("999.5.0")), r.latest);
}

// Whitespace around the JSON colon is tolerated (regex match, not full parse).
TEST(VersionCompare, WhitespaceTolerant)
{
	CVersionCompareResult r = CompareLatestReleaseVersion(wxT("{ \"tag_name\"  :   \"999.5.3\" }"));
	ASSERT_TRUE(r.state == CVersionCompareResult::Outdated);
	ASSERT_EQUALS(999, r.major);
}

// No tag_name field at all is a ParseError.
TEST(VersionCompare, ParseErrorNoTag)
{
	CVersionCompareResult r = CompareLatestReleaseVersion(wxT("{\"name\": \"whatever\"}"));
	ASSERT_TRUE(r.state == CVersionCompareResult::ParseError);
}

// A tag that reduces to empty after prefix/suffix strip is a ParseError.
TEST(VersionCompare, ParseErrorEmptyAfterStrip)
{
	CVersionCompareResult r = CompareLatestReleaseVersion(wxT("{\"tag_name\": \"v\"}"));
	ASSERT_TRUE(r.state == CVersionCompareResult::ParseError);
}

// A non-numeric version component is a ParseError.
TEST(VersionCompare, ParseErrorNonNumeric)
{
	CVersionCompareResult r = CompareLatestReleaseVersion(wxT("{\"tag_name\": \"3.x.0\"}"));
	ASSERT_TRUE(r.state == CVersionCompareResult::ParseError);
}
