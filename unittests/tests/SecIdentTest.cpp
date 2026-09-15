// This file is part of the aMule Project.
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <tags/ClientTags.h>

using namespace muleunit;

DECLARE_SIMPLE(SecIdent)

TEST(SecIdent, AdvertisementRequiresCryptoAndIPv4ForV2)
{
	ASSERT_EQUALS(0u, SecIdent::SupportedVersions(false, false));
	ASSERT_EQUALS(0u, SecIdent::SupportedVersions(false, true));
	ASSERT_EQUALS(1u, SecIdent::SupportedVersions(true, false));
	ASSERT_EQUALS(3u, SecIdent::SupportedVersions(true, true));
}

TEST(SecIdent, IPv4KeepsLegacyVersionPreference)
{
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0, true));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(1, true));
	ASSERT_EQUALS(SecIdent::V2, SecIdent::SignatureVersion(2, true));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(3, true));
}

TEST(SecIdent, WithoutIPv4OnlyMutuallySupportedV1IsUsable)
{
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0, false));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(1, false));
	// A fallback must not invent v1 support for a peer advertising only v2.
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(2, false));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(3, false));
}

TEST(SecIdent, ReservedFeatureBitsDoNotEnableSecureIdentification)
{
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0xfc, true));
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0xfc, false));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(0xfd, false));
	ASSERT_EQUALS(SecIdent::V2, SecIdent::SignatureVersion(0xfe, true));
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0xfe, false));
}
