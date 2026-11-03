// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The validator lives in libcore's libbase; this keeps the old name.
#include "base/utf8.hpp"

namespace http {
using base::isValidUtf8;
}
