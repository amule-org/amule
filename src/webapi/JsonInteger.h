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

// JSON category fields use the same byte range and error messages. Presence and
// defaults belong to the caller: an omitted category must not become a PATCH operation.
inline bool ParseCategoryIndexField(const picojson::value &input, std::uint8_t &category, const char *&error)
{
	constexpr const char *message = "`category_index` must be an integer in 0..255";
	if (!input.is<double>()) {
		error = message;
		return false;
	}
	const double value = input.get<double>();
	if (!IsIntegralJsonNumber(value) || value < 0 || value > 255) {
		error = message;
		return false;
	}
	category = static_cast<std::uint8_t>(value);
	return true;
}
} // namespace webapi

#endif // WEBAPI_JSONINTEGER_H
