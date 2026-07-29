#include <muleunit/test.h>
#include <NetworkFunctions.h>
#include <amuleIPV4Address.h>

#define itemsof(x) (sizeof(x) / sizeof(x[0]))

using namespace muleunit;

// Needed for Boost-enabled build
namespace MuleNotify
{
void HandleNotificationAlways(const class CMuleNotiferBase &) {}
}; // namespace MuleNotify

DECLARE_SIMPLE(NetworkFunctions)

TEST(NetworkFunctions, StringIPtoUint32)
{
	unsigned int values[] = { 0, 1, 127, 254, 255 };
	const wxChar whitespace[] = { ' ', '\t', '\n' };
	int items = itemsof(values);
	int whites = 2;
	int zeros = 2;

	// Test a few standard IP combinations
	for (int wl = 0; wl < whites; ++wl) {
		for (int wr = 0; wr < whites; ++wr) {
			for (int a = 0; a < items; ++a) {
				for (int za = 0; za < zeros; ++za) {
					for (int b = 0; b < items; ++b) {
						for (int zb = 0; zb < zeros; ++zb) {
							for (int c = 0; c < items; ++c) {
								for (int zc = 0; zc < zeros; ++zc) {
									for (int d = 0; d < items; ++d) {
										for (int zd = 0; zd < zeros;
											++zd) {
											wxString IP;

											IP << wxString(
												      ' ', wl)
											   << wxString(
												      '0', za)
											   << values[a] << '.'
											   << wxString(
												      '0', zb)
											   << values[b] << '.'
											   << wxString(
												      '0', zc)
											   << values[c] << '.'
											   << wxString(
												      '0', zd)
											   << values[d]
											   << wxString(' ',
												      wr);

											uint32 resultIP = 17;

											ASSERT_TRUE(StringIPtoUint32(
												IP,
												resultIP));

											uint32 expected =
												(values[d]
													<< 24) |
												(values[c]
													<< 16) |
												(values[b]
													<< 8) |
												values[a];

											ASSERT_EQUALS(
												expected,
												resultIP);
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	// Test invalid IPs
	uint32 dummyIP = 27;

	// Missing fields
	ASSERT_FALSE(StringIPtoUint32(".2.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1..3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2..4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.3.", dummyIP));
	ASSERT_FALSE(StringIPtoUint32(".2.3.", dummyIP));

	// Extra dots
	ASSERT_FALSE(StringIPtoUint32(".1.2.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1..2.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2..3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.3..4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.3.4.", dummyIP));
	ASSERT_FALSE(StringIPtoUint32(".1.2.3.4.", dummyIP));

	// Garbage
	ASSERT_FALSE(StringIPtoUint32("abc", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("a1.1.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.1.3.4b", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("a.1.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.b.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.c.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.3.d", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.b.3.d", dummyIP));

	// Invalid fields
	ASSERT_FALSE(StringIPtoUint32("256.2.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.256.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.256.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.3.256", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("256.2.3.256", dummyIP));

	// Negative fields
	ASSERT_FALSE(StringIPtoUint32("-1.2.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.-2.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.-3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.3.-4", dummyIP));

	// Whitespace between fields
	for (unsigned i = 0; i < itemsof(whitespace); ++i) {
		wxChar c = whitespace[i];

		ASSERT_FALSE(StringIPtoUint32(wxString::Format("1%c.2.3.4", c), dummyIP));
		ASSERT_FALSE(StringIPtoUint32(wxString::Format("1.%c2.3.4", c), dummyIP));
		ASSERT_FALSE(StringIPtoUint32(wxString::Format("1.2%c.3.4", c), dummyIP));
		ASSERT_FALSE(StringIPtoUint32(wxString::Format("1.2.%c3.4", c), dummyIP));
		ASSERT_FALSE(StringIPtoUint32(wxString::Format("1.2.3%c.4", c), dummyIP));
		ASSERT_FALSE(StringIPtoUint32(wxString::Format("1.2.3.%c4", c), dummyIP));
	}

	// Faar too large values (triggered overflow and became negative)
	ASSERT_FALSE(StringIPtoUint32("2147483648.2.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2147483648.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.2147483648.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.3.2147483648", dummyIP));

	// Values greater than 2 ** 32 - 1 (triggered overflow and becomes x - (2 ** 32 - 1))
	ASSERT_FALSE(StringIPtoUint32("4294967296.2.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.4294967296.3.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.4294967296.4", dummyIP));
	ASSERT_FALSE(StringIPtoUint32("1.2.3.4294967296", dummyIP));

	// The dummyIP value shouldn't have been changed by any of these calls
	ASSERT_EQUALS(27u, dummyIP);
}

// Testing the IsGoodIP() and IsLanIP() functions
TEST(NetworkFunctions, IsGoodIP)
{
	struct
	{
		wxString ip;
		bool isgood;
		bool islan;
	} ipList[] = { { "0.0.0.0", false, false },
		{ "0.0.0.1", false, false },
		{ "0.0.1.0", false, false },
		{ "0.1.0.0", false, false },
		{ "1.0.0.0", true, false },
		{ "10.0.0.1", true, true },
		{ "10.0.1.0", true, true },
		{ "10.1.0.0", true, true },
		{ "14.156.39.4", true, false },
		{ "24.93.63.177", true, false },
		{ "172.15.0.0", true, false },
		{ "172.16.0.0", true, true },
		{ "172.17.0.0", true, true },
		{ "172.31.0.0", true, true },
		{ "172.32.0.0", true, false },
		{ "192.88.98.176", true, false },
		{ "192.88.99.175", false, false },
		{ "192.88.100.17", true, false },
		{ "192.167.0.0", true, false },
		{ "192.168.0.0", true, true },
		{ "192.168.255.255", true, true },
		{ "192.169.0.0", true, false },
		{ "198.17.0.0", true, false },
		{ "198.18.0.0", false, false },
		{ "198.19.0.0", false, false },
		{ "198.20.0.0", true, false } };

	unsigned int ip;
	for (unsigned int i = 0; i < itemsof(ipList); i++) {
		ASSERT_TRUE(StringIPtoUint32(ipList[i].ip, ip));
		ASSERT_EQUALS(ipList[i].isgood, IsGoodIP(ip, false));
		ASSERT_EQUALS(ipList[i].islan, IsLanIP(ip));
		ASSERT_EQUALS(ipList[i].isgood && !ipList[i].islan, IsGoodIP(ip, true));
	}
}

// amuleIPV4Address is v4-only by contract: the endpoint feeds uint32 IPs
// and v4 sockets throughout aMule. Name resolution used to hand back
// whatever the platform resolver ranked first, which on a dual-stack name
// is routinely an AAAA record -- issue #695.
//
// Every case here resolves offline: numeric addresses and "localhost" (hosts
// file) need no DNS server, so the test cannot go flaky when a public zone
// changes its records.
//
// The numeric IPv6 literals are what actually pin the regression on every
// platform. They are not dotted quads, so they fall through to the resolver,
// and an unrestricted query answers with the v6 address -- which the old code
// accepted. Ordering plays no part, so unlike "localhost" (which only exposes
// the bug where ::1 sorts first, i.e. Windows) these fail on macOS and Linux
// too if the family restriction is ever dropped again.
//
// Note what is NOT asserted: a specific resolved address. Which A record
// surfaces varies by platform and between runs (round-robin, resolver
// ordering), so pinning one would be flaky by construction. The invariant is
// the address family.
TEST(NetworkFunctions, HostnameResolvesToIPv4Only)
{
	amuleIPV4Address addr;

	// A dotted quad must survive untouched (the no-resolution fast path).
	ASSERT_TRUE(addr.Hostname(wxT("127.0.0.1")));
	ASSERT_EQUALS(wxString(wxT("127.0.0.1")), addr.IPAddress());

	// An IPv6 literal has no v4 answer, so it must be refused outright
	// rather than stored in a v4-only endpoint.
	amuleIPV4Address v6literal;
	ASSERT_FALSE(v6literal.Hostname(wxT("::1")));
	amuleIPV4Address v6global;
	ASSERT_FALSE(v6global.Hostname(wxT("2001:4860:4860::8888")));

	// A name that has both A and AAAA records must still yield IPv4.
	amuleIPV4Address local;
	ASSERT_TRUE(local.Hostname(wxT("localhost")));
	unsigned int ip = 0;
	// StringIPtoUint32 only parses a dotted quad, so this is what actually
	// pins the family: an IPv6 result fails to parse here.
	ASSERT_TRUE(StringIPtoUint32(local.IPAddress(), ip));
	ASSERT_TRUE(IsLoopbackIP(ip));

	// The same invariant through the helper the rest of the tree calls.
	ASSERT_TRUE(StringHosttoUint32(wxT("localhost")) != 0);
	ASSERT_EQUALS((unsigned int)0, StringHosttoUint32(wxT("::1")));

	// An empty name is rejected rather than resolved to anything.
	amuleIPV4Address empty;
	ASSERT_FALSE(empty.Hostname(wxEmptyString));
}
