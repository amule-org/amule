// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#ifndef WEBAPI_JSONINTEGER_H
#define WEBAPI_JSONINTEGER_H

#include "PicoJson_Inc.h"
#include <cmath>
#include <cstdint>

namespace webapi
{
// Numeric fields are read through picojson's double accessor. Check integrality before
// narrowing; casting to an integer to test it can overflow for large byte counts.
inline bool IsIntegralJsonNumber(double value)
{
	return std::isfinite(value) && value == std::floor(value);
}

// All category-taking endpoints use the same byte range and error messages. Presence and
// defaults belong to the caller: an omitted category must not become a PATCH operation.
inline bool ParseCategoryIndex(const picojson::value &input, std::uint8_t &category, const char *&error)
{
	if (!input.is<double>()) {
		error = "`category_index` must be a non-negative integer";
		return false;
	}
	const double value = input.get<double>();
	if (value < 0 || value > 255 || !IsIntegralJsonNumber(value)) {
		error = "`category_index` must be in [0, 255]";
		return false;
	}
	category = static_cast<std::uint8_t>(value);
	return true;
}
} // namespace webapi

#endif // WEBAPI_JSONINTEGER_H
