// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include "JsonInteger.h"
#include <limits>
#include <string>
#include <vector>

using namespace muleunit;
DECLARE_SIMPLE(JsonInteger)

TEST(JsonInteger, CategoryAcceptsEveryByteValue)
{
	for (unsigned value = 0; value <= 255; ++value) {
		std::uint8_t category = 42;
		const char *error = nullptr;
		ASSERT_TRUE(webapi::ParseCategoryIndexField(picojson::value(double(value)), category, error));
		ASSERT_EQUALS(value, unsigned(category));
	}
}

TEST(JsonInteger, CategoryRejectsOtherJsonTypesWithoutChangingOutput)
{
	const std::vector<picojson::value> inputs{ picojson::value(),
		picojson::value(false),
		picojson::value(true),
		picojson::value(std::string("2")),
		picojson::value(picojson::array{}),
		picojson::value(picojson::object{}) };
	for (const auto &input : inputs) {
		std::uint8_t category = 42;
		const char *error = nullptr;
		ASSERT_FALSE(webapi::ParseCategoryIndexField(input, category, error));
		ASSERT_EQUALS(unsigned(42), unsigned(category));
		ASSERT_EQUALS(
			std::string("`category_index` must be an integer in 0..255"), std::string(error));
	}
}

TEST(JsonInteger, CategoryRejectsFractionsAndOutOfRangeNumbers)
{
	const std::vector<double> inputs{ -1,
		256,
		-0.5,
		0.5,
		2.9,
		254.5,
		255.5,
		std::numeric_limits<double>::lowest(),
		std::numeric_limits<double>::max() };
	for (double input : inputs) {
		std::uint8_t category = 42;
		const char *error = nullptr;
		ASSERT_FALSE(webapi::ParseCategoryIndexField(picojson::value(input), category, error));
		ASSERT_EQUALS(unsigned(42), unsigned(category));
		ASSERT_EQUALS(
			std::string("`category_index` must be an integer in 0..255"), std::string(error));
	}
}

TEST(JsonInteger, ParsedJsonRetainsIntegerAndFractionSemantics)
{
	struct ParsedCategory
	{
		const char *body;
		unsigned expected;
	};
	for (const auto &test : { ParsedCategory{ "0", 0 },
		     ParsedCategory{ "-0", 0 },
		     ParsedCategory{ "2.0", 2 },
		     ParsedCategory{ "2e0", 2 },
		     ParsedCategory{ "255", 255 } }) {
		picojson::value input;
		ASSERT_TRUE(picojson::parse(input, std::string(test.body)).empty());
		std::uint8_t category = 42;
		const char *error = nullptr;
		ASSERT_TRUE(webapi::ParseCategoryIndexField(input, category, error));
		ASSERT_EQUALS(test.expected, unsigned(category));
	}
	picojson::value input;
	ASSERT_TRUE(picojson::parse(input, std::string("2.9")).empty());
	std::uint8_t category = 42;
	const char *error = nullptr;
	ASSERT_FALSE(webapi::ParseCategoryIndexField(input, category, error));
	ASSERT_EQUALS(unsigned(42), unsigned(category));
}

TEST(JsonInteger, IntegralityCheckStillSupportsLargeSearchSizes)
{
	for (double value : { 0.0, -1.0, 2147483648.0, 9223372036854775808.0, 18446744073709549568.0 }) {
		ASSERT_TRUE(webapi::IsIntegralJsonNumber(value));
	}
	for (double value : { 0.5,
		     -2.9,
		     std::numeric_limits<double>::infinity(),
		     std::numeric_limits<double>::quiet_NaN() }) {
		ASSERT_FALSE(webapi::IsIntegralJsonNumber(value));
	}
}
