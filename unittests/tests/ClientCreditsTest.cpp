// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <ClientCredits.h>
#include <GetTickCount.h>

using namespace muleunit;

// Link-time clock seam: exercise wait ownership without sleeps or application state.
static uint64 now = 1000;
uint64 GetTickCount64()
{
	return now;
}

static CreditStruct *SecureRecord()
{
	auto *record = new CreditStruct();
	record->nKeySize = 1;
	record->abySecureIdent[0] = 1;
	return record;
}

static CNetworkAddress IPv6(uint8_t suffix)
{
	CNetworkAddress::Octets octets{};
	octets[0] = 0x20;
	octets[1] = 0x01;
	octets[15] = suffix;
	return CNetworkAddress::IPv6FromOctets(octets);
}

static CNetworkAddress MappedIPv4()
{
	CNetworkAddress::Octets octets{};
	octets[10] = octets[11] = 0xff;
	octets[12] = 192;
	octets[14] = 2;
	octets[15] = 1;
	return CNetworkAddress::IPv6FromOctets(octets);
}

DECLARE_SIMPLE(ClientCredits)

TEST(ClientCredits, VerifiedIdentityDoesNotCollideAcrossIPv6Peers)
{
	CClientCredits credits(SecureRecord());
	credits.Verified(IPv6(1));
	ASSERT_EQUALS(IS_IDENTIFIED, credits.GetCurrentIdentState(IPv6(1)));
	ASSERT_EQUALS(IS_IDBADGUY, credits.GetCurrentIdentState(IPv6(2)));
	ASSERT_EQUALS(IS_IDBADGUY, credits.GetCurrentIdentState(CNetworkAddress::Absent()));
	ASSERT_EQUALS(IS_IDBADGUY, credits.GetCurrentIdentState(CNetworkAddress::FromIPv4NetworkOrder(0)));
	credits.AddDownloaded(2000000, IPv6(2), true);
	credits.AddUploaded(100, IPv6(2), true);
	ASSERT_EQUALS(uint64(0), credits.GetDownloadedTotal());
	ASSERT_EQUALS(uint64(0), credits.GetUploadedTotal());
	credits.AddDownloaded(2000000, IPv6(1), true);
	ASSERT_EQUALS(uint64(2000000), credits.GetDownloadedTotal());
	ASSERT_TRUE(credits.GetScoreRatio(IPv6(1), true) > 1.0f);
	ASSERT_EQUALS(1.0f, credits.GetScoreRatio(IPv6(2), true));
}

TEST(ClientCredits, MappedAndPlainIPv4ShareIdentityAndWait)
{
	const auto plain = CNetworkAddress::FromIPv4NetworkOrder(0x010200c0);
	CClientCredits credits(SecureRecord());
	credits.Verified(MappedIPv4());
	ASSERT_EQUALS(IS_IDENTIFIED, credits.GetCurrentIdentState(plain));
	credits.Verified(plain);
	ASSERT_EQUALS(IS_IDENTIFIED, credits.GetCurrentIdentState(MappedIPv4()));
	credits.SetIdentState(IS_IDNEEDED);
	now = 1000;
	credits.SetSecWaitStartTime(MappedIPv4());
	now = 2000;
	ASSERT_EQUALS(uint64(999), credits.GetSecureWaitStartTime(plain));
	credits.SetSecWaitStartTime(plain);
	now = 3000;
	ASSERT_EQUALS(uint64(1999), credits.GetSecureWaitStartTime(MappedIPv4()));
}

TEST(ClientCredits, UnverifiedWaitResetsForDifferentIPv6Endpoint)
{
	CClientCredits credits(SecureRecord());
	now = 1000;
	credits.SetSecWaitStartTime(IPv6(1));
	now = 2000;
	ASSERT_EQUALS(uint64(999), credits.GetSecureWaitStartTime(IPv6(1)));
	ASSERT_EQUALS(uint64(2000), credits.GetSecureWaitStartTime(IPv6(2)));
	now = 3000;
	ASSERT_EQUALS(uint64(2000), credits.GetSecureWaitStartTime(IPv6(2)));
	// Verification restores the secure wait, not another endpoint's unverified wait.
	credits.Verified(IPv6(2));
	ASSERT_EQUALS(uint64(999), credits.GetSecureWaitStartTime(IPv6(2)));
	ASSERT_EQUALS(uint64(3000), credits.GetSecureWaitStartTime(IPv6(1)));
}

TEST(ClientCredits, AbsenceCannotBecomeVerified)
{
	CClientCredits credits(SecureRecord());
	ASSERT_FALSE(credits.Verified(CNetworkAddress::Absent()));
	ASSERT_EQUALS(IS_IDNEEDED, credits.GetIdentState());
}

TEST(ClientCredits, IPv4CreditsKeepMappedScoreEquivalence)
{
	const auto plain = CNetworkAddress::FromIPv4NetworkOrder(0x010200c0);
	CClientCredits credits(SecureRecord());
	credits.Verified(plain);
	credits.AddDownloaded(2000000, plain, true);
	credits.AddUploaded(1000000, MappedIPv4(), true);
	ASSERT_EQUALS(uint64(1000000), credits.GetUploadedTotal());
	ASSERT_EQUALS(credits.GetScoreRatio(plain, true), credits.GetScoreRatio(MappedIPv4(), true));
}
