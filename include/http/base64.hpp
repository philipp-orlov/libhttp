// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// base64 lives in develop-core's libbase; these keep the old names.
#include <cstdint>
#include <string>
#include <string_view>

#include "base/encoding.hpp"

namespace http {

inline std::string base64Encode(const uint8_t* data, size_t len) { return base::base64Encode(data, len); }
// Non-alphabet characters (padding, whitespace) are skipped rather than rejected.
inline std::string base64Decode(std::string_view input) { return base::base64Decode(input); }

}  // namespace http
